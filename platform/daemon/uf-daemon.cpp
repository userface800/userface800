// uf-daemon — owns the FF800 through the OHCI dext and bridges its audio to the AudioServerPlugIn.
//
// Layer 3 of the CoreAudio stack: the daemon owns the device and bridges audio to the
// AudioServerPlugIn over shared memory. Responsibilities:
//   - open the dext user client, wait for the FF800 to be identified;
//   - run the streaming session lifecycle (rate, packet format, tx channel, comm start, fetch on);
//   - create two POSIX shared-memory rings (capture: daemon->plugin, playback: plugin->daemon);
//   - pump continuously: decode the dext's capture ring into the capture shm ring, and encode the
//     playback shm ring into the dext's transmit payload.
//
// STATUS: the shm ring + decode/encode wiring is ordinary logic and is what this file is for. The
// continuous dext capture ring it drives has not run on hardware — verify the pump against a real
// stream. This is scaffolding to iterate on with the FF800 attached, not a finished daemon.
#include <IOKit/IOKitLib.h>
#include <CoreMIDI/CoreMIDI.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <cmath>
#include <atomic>
#include <mach/mach_time.h>
#include <mach/mach_init.h>
#include <mach/mach_port.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#include <map>
#include <thread>

#include "uf/protocol/channels.hpp"
#include "uf/protocol/codec.hpp"
#include "uf/protocol/registers.hpp"
#include "uf/protocol/stream.hpp"
#include "uf/protocol/rate.hpp"
#include "uf/protocol/settings.hpp"
#include "uf/protocol/status.hpp"
#include "uf/protocol/wav.hpp"
#include "uf/protocol/control.hpp"
#include "uf/protocol/midi.hpp"
#include "../shared/uf_control.hpp"
#include "../shared/uf_shm_ring.hpp"
#include "../shared/uf_tx_fill.hpp"
#include "../shared/uf_meters.hpp"
#include "../shared/uf_mixer_model.hpp"
#include "../shared/uf_mixer_shadow.hpp"
#include "uf_ctl_server.hpp"
#include "../shared/uf_state_store.hpp"
#include "../shared/uf_state_shm.hpp"

// Short names for the user-client ABI. Defined in terms of the shared header rather than copied out
// of it, so the two cannot drift.
#include "../ohci-dext/UFFireWireOHCIShared.h"
enum { kRQ = kUFOhciReadQuadlet, kWQ = kUFOhciWriteQuadlet, kWB = kUFOhciWriteBlock,
       kRB = kUFOhciReadBlock,
       kIsoStart = kUFOhciIsoStart, kIsoPoll = kUFOhciIsoPoll, kIsoStop = kUFOhciIsoStop,
       kIsoTxStart = kUFOhciIsoTxStart, kIsoTxStop = kUFOhciIsoTxStop,
       kIsoStartCont = kUFOhciIsoStartCont, kIsoCompleted = kUFOhciIsoCompleted,
       kIsoRelease = kUFOhciIsoRelease, kReadCycleTimer = kUFOhciReadCycleTimer,
       kIsoTxSent = kUFOhciIsoTxSent, kIsoTxRefill = kUFOhciIsoTxRefill, kPump = kUFOhciPump,
       kIsoTxSetBytes = kUFOhciIsoTxSetBytes, kMidiInEnable = kUFOhciMidiInEnable,
       kDebugInbound = kUFOhciDebugInbound, kIsoWake = kUFOhciIsoWake };

// OHCI isochronous cycle timer -> linear 24.576 MHz bus ticks. Fields: [31:25] seconds (wraps at 128s),
// [24:12] cycleCount (0..7999, 8000/s), [11:0] cycleOffset (0..3071, 24.576 MHz). This is the bus clock
// our isoch transmit is paced by; sampling it at capture completion recovers the FF800 rate against it.
static constexpr uint64_t kCycleTimerTicksPerSec = 24576000ull;
static inline uint64_t cycle_timer_ticks(uint32_t ct) {
    return (uint64_t)((ct >> 25) & 0x7f) * 8000ull * 3072ull
         + (uint64_t)((ct >> 12) & 0x1fff) * 3072ull
         + (uint64_t)(ct & 0xfff);
}
static const uint32_t kDextSlotBytes = 2048;   // must match kUFOhciIsoSlotBytes
static const uint32_t kDextMemType   = 0;
static const uint32_t kTxMemType     = 1;   // must match kUFOhciTxMemoryType
static const uint32_t kTxSlots       = 640; // must match kUFOhciTxSlots / kITPackets
static const uint32_t kTxSlotBytes   = 2048;// must match kUFOhciTxSlotBytes

// Transmit pacing. The IT ring self-loops at one packet per isochronous cycle (125 us), so we fill
// it a fixed distance ahead of the controller's transmit head rather than at our own wake-up rate.
// The lead must cover the pump's worst-case sleep (1 ms = 8 packets) plus scheduling jitter, and is
// pure added output latency, so 8 ms is a deliberate compromise. It must stay well under kTxSlots:
// a lead of kTxSlots means writing the very slot being transmitted.
static const uint64_t kTxLeadPkts    = 64;  // 8 ms ahead of the transmit head
static const uint64_t kTxMinLeadPkts = 8;   // 1 ms: below this we have lost the race, resync

static std::atomic<bool> g_run{true};
static void on_signal(int) { g_run.store(false); }

// Wall-clock prefix for EVENT lines. Running under launchd the output is a file nobody is watching,
// and "RESTART ... / no tx channel / restart FAILED" repeated 60 times tells you nothing about
// whether that happened over five seconds or five hours. The per-second telemetry keeps its own
// uptime stamp and is suppressed by UF_QUIET anyway, so only events need this.
static const char* uf_now() {
    static char buf[16];
    time_t t = time(nullptr);
    struct tm tm_;
    localtime_r(&t, &tm_);
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d", tm_.tm_hour, tm_.tm_min, tm_.tm_sec);
    return buf;
}

// SIGUSR1 arms (or re-arms) a capture. This exists because arming via UF_CAP_WAV alone requires
// STARTING the daemon, and a fresh session is precisely what clears the intermittent faults we are
// trying to record — every capture taken that way came back clean by construction. With this, the
// daemon stays up and the capture is triggered while the fault is audible.
static std::atomic<bool> g_armCapture{false};
static void on_arm_capture(int) { g_armCapture.store(true); }

// The device configuration shadow. Most FF800 registers are write-only, so the host copy is the
// source of truth and every change rebuilds the whole conf block from it (spec/02 §2.5). Currently
// only the clock source is reachable from CoreAudio; the rest of the shadow is what uf-set writes.
static uf::SettingsShadow g_settings{};

// What the mixer matrix is SUPPOSED to contain. The device forgets it on every session start, so the
// shadow is the source of truth and the hardware is a write target — the same relationship
// SettingsShadow already has with the write-only control registers. Without this, any route set with
// uf-mix died at the next rate change, wedge recovery or bus reset.
// Two layers, and the split matters. g_model is what the USER means — per-crosspoint gain plus mute
// and phase flags, per-output loopback, stereo pairing — and g_mixer is what the DEVICE gets, the
// 2048 rendered quadlets. Mute is why: rendering a muted crosspoint writes 0, so if 0 were all we
// kept, un-muting could not restore the gain. See uf_mixer_model.hpp.
static uf::MixerModel g_model{};
static uf::MixerShadow g_mixer{};
static bool g_mixerInit = false;

// Persisting the model. The pump owns it and is the only writer, so it takes the SNAPSHOT
// (a memcpy — no syscall, safe in the realtime loop) and a saver thread does the file I/O. Reading
// the live model from another thread would risk writing a torn matrix to disk, which would come
// back as unknown gains on someone's outputs at the next start.
static uf::MixerModel::Blob g_stateSnap{};
static std::atomic<uint64_t> g_stateSnapSeq{0};   // pump bumps after filling g_stateSnap
static std::atomic<bool> g_stateRun{true};

// Copy the mixer into the published state: the rendered matrix, the logical gains and flags above
// it, and the stereo pairing. One place, because a UI that saw `cells` updated but the flags stale
// would draw a mute that is not there — and a muted crosspoint renders to 0, so the flags are the
// only way to tell "muted at -6 dB" from "faded to nothing".
static_assert(uf::state::Shared::kMixerCh == uf::MixerModel::kCh, "published mixer geometry");
static void publish_mixer(uf::state::Shared* o) {
    constexpr uint32_t kCh = uf::MixerModel::kCh;
    for (uint32_t i = 0; i < uf::state::kCells; ++i) o->cells[i] = g_mixer.cell(i);
    for (uint32_t sc = 0; sc < kCh; ++sc)
        for (uint32_t d = 0; d < kCh; ++d) {
            const auto& ci = g_model.cell(uf::MixerSrcKind::Input, sc, d);
            const auto& cp = g_model.cell(uf::MixerSrcKind::Playback, sc, d);
            o->inputFlags[sc * kCh + d]    = ci.flags;
            o->inputGain[sc * kCh + d]     = ci.gain;
            o->playbackFlags[sc * kCh + d] = cp.flags;
            o->playbackGain[sc * kCh + d]  = cp.gain;
        }
    for (uint32_t x = 0; x < kCh; ++x) {
        o->outputFlags[x] = g_model.output(x).flags;
        o->outputGain[x]  = g_model.output(x).gain;
    }
    o->stereoIn = g_model.stereo_bits(uf::MixerSrcKind::Input);
    o->stereoPb = g_model.stereo_bits(uf::MixerSrcKind::Playback);
}

// Suppresses session_start's step-by-step progress chatter. Set while retrying a session that keeps
// failing — the first attempt's output is worth having, the hundredth identical copy is not.
static bool g_sessionQuiet = false;

// ── dext helpers ──────────────────────────────────────────────────────────────────────────────
static io_connect_t open_dext() {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
                                                  IOServiceNameMatching("UFFireWireOHCI"));
    if (!svc) return 0;
    io_connect_t c = 0;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &c);
    IOObjectRelease(svc);
    return kr == KERN_SUCCESS ? c : 0;
}
static bool rq(io_connect_t c, uint64_t off, uint32_t* v) {
    uint64_t in = off, out = 0; uint32_t n = 1;
    if (IOConnectCallScalarMethod(c, kRQ, &in, 1, &out, &n) != KERN_SUCCESS) return false;
    *v = (uint32_t)out; return true;
}
static bool wq(io_connect_t c, uint64_t off, uint32_t v) {
    uint64_t in[2] = {off, v};
    return IOConnectCallScalarMethod(c, kWQ, in, 2, nullptr, nullptr) == KERN_SUCCESS;
}
static bool wblock(io_connect_t c, uint64_t off, const uint32_t* q, uint32_t n) {
    return IOConnectCallMethod(c, kWB, &off, 1, q, n * 4, nullptr, nullptr, nullptr, nullptr)
           == KERN_SUCCESS;
}
// Block READ. One round trip for a whole region — the meters are 254 quadlets, and reading those a
// quadlet at a time would put thousands of transactions a second through the dext's single AT
// context (UFFireWireOHCIShared.h).
static bool rblock(io_connect_t c, uint64_t off, uint32_t* q, uint32_t n) {
    uint64_t in[2] = {off, n};
    size_t bytes = n * 4;
    return IOConnectCallMethod(c, kRB, in, 2, nullptr, 0, nullptr, nullptr, q, &bytes)
           == KERN_SUCCESS && bytes == n * 4;
}
static bool call1(io_connect_t c, uint32_t sel, uint64_t a) {
    return IOConnectCallScalarMethod(c, sel, &a, 1, nullptr, nullptr) == KERN_SUCCESS;
}
// DIAGNOSTIC: fill the whole tx ring (all slots, framesPerSlot frames each) with a ~437 Hz sine on one
// playback channel, zeros elsewhere. The tx ring is a static loop (kTxSlots*framesPerSlot frames), so
// an integer number of cycles loops cleanly. Lets us verify per-output playback routing with no app.
static void fill_tone(uint8_t* txBuf, uint32_t kTxSlots, uint32_t slotBytes,
                      uint32_t framesPerSlot, uint32_t dbq, int ch, double freqHz) {
    const uint32_t total = kTxSlots * framesPerSlot;
    int cycles = (int)(freqHz * total / 48000.0 + 0.5);   // integer cycles over the ring -> clean loop
    if (cycles < 1) cycles = 1;
    const double amp = (double)0x400000;   // ~ -6 dBFS (loud, so it's visible under the beep)
    for (uint32_t slot = 0; slot < kTxSlots; ++slot)
        for (uint32_t f = 0; f < framesPerSlot; ++f) {
            uint32_t g = slot * framesPerSlot + f;
            uint8_t* fr = txBuf + (size_t)slot * slotBytes + (size_t)f * dbq * 4;
            for (uint32_t c = 0; c < dbq; ++c) {
                int32_t s = ((int)c == ch)
                    ? (int32_t)(amp * std::sin(2.0 * M_PI * cycles * g / total)) : 0;
                uf::encode_sample_le(s, fr + (size_t)c * 4);
            }
        }
}

// Put the pump on the real-time scheduler (THREAD_TIME_CONSTRAINT_POLICY) — the same contract
// CoreAudio's own IO thread uses. At normal priority the pump is just another thread the kernel may
// preempt for as long as it likes, and every packet it is late by is a packet of transmit lead we
// have to carry. This tells the scheduler: over each `period` I need `computation` of CPU, and I am
// useless if I do not get it within `constraint`.
//
// preemptible=0 because a late audio tick is a glitch, not a slowdown. The duty cycle is what keeps
// that honest: ~0.3 ms of work per 1 ms period, so we are asking for 30% of one core, not a spin.
// Over-declaring computation is the safer error — a thread that overruns it gets demoted to
// timeshare for a while, which is exactly the jitter we are buying our way out of. (The UF_TX_WAV
// diagnostic rewrites the whole ring every tick and will overrun this; that path is not real-time.)
static void pump_realtime(double periodMs, double computeMs, double constraintMs) {
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    const double ticksPerMs = 1.0e6 * tb.denom / tb.numer;
    thread_time_constraint_policy_data_t pol;
    pol.period      = (uint32_t)(periodMs * ticksPerMs);
    pol.computation = (uint32_t)(computeMs * ticksPerMs);
    pol.constraint  = (uint32_t)(constraintMs * ticksPerMs);
    pol.preemptible = 0;
    thread_port_t self = mach_thread_self();
    kern_return_t kr = thread_policy_set(self, THREAD_TIME_CONSTRAINT_POLICY,
                                         (thread_policy_t)&pol, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    mach_port_deallocate(mach_task_self(), self);
    if (kr == KERN_SUCCESS)
        std::printf("uf-daemon: pump on the real-time scheduler (%.1f/%.1f/%.1f ms)\n",
                    periodMs, computeMs, constraintMs);
    else
        std::fprintf(stderr, "uf-daemon: real-time scheduling refused (0x%x); pump stays normal-priority\n", kr);
}

// One pump tick in one IPC: release what we consumed, hand back the tx slots we rewrote, and read
// both contexts' positions. Every external method is a cross-process call into the dext, so at a
// 1 kHz tick the difference between this and four separate calls is 3000 round trips a second.
// The release/refill arguments are last tick's values — a 1 ms deferral against a 64 ms capture ring
// and an 80 ms transmit lap, which neither cares about.
static void pump_dext(io_connect_t c, uint64_t releaseUpTo, uint64_t txRefillUpTo,
                      uint64_t* completed, uint64_t* sent) {
    uint64_t in[2] = {releaseUpTo, txRefillUpTo};
    uint64_t out[2] = {0, 0}; uint32_t n = 2;
    if (IOConnectCallScalarMethod(c, kPump, in, 2, out, &n) == KERN_SUCCESS) {
        *completed = out[0]; *sent = out[1];
    }
}

// ── MIDI ──────────────────────────────────────────────────────────────────────────────────────
// Two virtual CoreMIDI endpoints, bridged to the FF800's async MIDI registers (spec/04). This lives
// in the daemon rather than in a separate uf-midi process for a specific reason: every register
// transaction goes through the dext's single AT context, and two processes each holding their own
// user client can have their external methods dispatched on different queues — so a MIDI write and
// an audio-path register write could interleave inside one transaction and corrupt both. One user
// client, one caller, no interleaving.
//
// OUT is real and testable. IN is HARDWARE-GATED: the dext's AR-request receive path has never
// delivered a packet, so nothing has ever been received.
static io_connect_t g_conn = 0;
static MIDIEndpointRef g_midiSrc = 0;   // FF800 -> apps

// Pack MIDI bytes one per little-endian quadlet and block-write them, in <=9-byte transactions
// paced at the DIN-MIDI rate so we cannot outrun the device's UART (uf::midi_throttle_ns).
static void midi_send_to_ff800(const uint8_t* bytes, size_t n) {
    if (!g_conn) return;
    size_t i = 0;
    while (i < n) {
        const size_t chunk = (n - i) < uf::kMidiMaxQuads ? (n - i) : uf::kMidiMaxQuads;
        std::vector<uint32_t> quads = uf::midi_out_quadlets(bytes + i, chunk);
        wblock(g_conn, uf::kMidiOutAddr, quads.data(), (uint32_t)quads.size());
        usleep((useconds_t)(uf::midi_throttle_ns((uint32_t)chunk) / 1000));
        i += chunk;
    }
}

// CoreMIDI calls this on the MIDIServer thread when an app sends to our destination. It is the one
// place another thread touches the dext; the throttle above means it holds the AT context in short
// bursts rather than continuously.
static void midi_read_proc(const MIDIPacketList* pktlist, void*, void*) {
    const MIDIPacket* p = &pktlist->packet[0];
    for (unsigned i = 0; i < pktlist->numPackets; ++i) {
        midi_send_to_ff800(p->data, p->length);
        p = MIDIPacketNext(p);
    }
}

// Drain whatever the device has async-written to our advertised host address and hand it to
// CoreMIDI. Called from the pump, so it inherits the pump's cadence for free.
static void midi_poll_in() {
    if (!g_conn || !g_midiSrc) return;
    uint64_t out[2] = {0, 0}; uint32_t n = 2;
    if (IOConnectCallScalarMethod(g_conn, kUFOhciMidiInPoll, nullptr, 0, out, &n) != KERN_SUCCESS)
        return;
    const uint32_t count = (uint32_t)out[0];
    if (count == 0 || count > 8) return;
    uint8_t bytes[8];
    for (uint32_t i = 0; i < count; ++i) bytes[i] = (uint8_t)((out[1] >> (8 * i)) & 0xff);

    uint8_t pktbuf[256];
    MIDIPacketList* pl = reinterpret_cast<MIDIPacketList*>(pktbuf);
    MIDIPacket* cur = MIDIPacketListInit(pl);
    cur = MIDIPacketListAdd(pl, sizeof(pktbuf), cur, 0, count, bytes);
    if (cur) MIDIReceived(g_midiSrc, pl);
}

static uint64_t scalar0(io_connect_t c, uint32_t sel) {
    uint64_t out = 0; uint32_t n = 1;
    IOConnectCallScalarMethod(c, sel, nullptr, 0, &out, &n);
    return out;
}

// ── the interrupt-driven wake ─────────────────────────────────────────────────────────────────
// The dext raises the OHCI isochronous receive completion interrupt every kUFOhciIrqEvery captured
// packets and signals this port. That is the FF800's own packet cadence, so the pump now runs on the
// device's clock instead of a host timer that drifts against it — the difference between feeding a
// small CoreAudio buffer reliably under DAW load and not.
//
// The message is only an edge: everything the tick needs is in the status page the interrupt handler
// fills, so there is no async payload to decode on the realtime thread. We only have to receive and
// discard, which is also why this can be a bare mach_msg rather than a runloop — a CFRunLoop on the
// pump thread would be its own source of latency.
static bool wake_wait(mach_port_t port, uint32_t timeoutMs) {
    struct { mach_msg_header_t hdr; uint8_t body[512]; } msg;
    kern_return_t kr = mach_msg(&msg.hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
                                sizeof(msg), port, timeoutMs, MACH_PORT_NULL);
    if (kr != MACH_MSG_SUCCESS && kr != MACH_RCV_TOO_LARGE) return false;
    // Coalesce anything already queued behind it. If we ever fall a few interrupts behind, waking
    // once per backlogged message would have us doing empty ticks to catch up instead of one full
    // one — and the backlog is exactly when we can least afford the waste.
    while (mach_msg(&msg.hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
                    sizeof(msg), port, 0, MACH_PORT_NULL) == MACH_MSG_SUCCESS) {}
    return true;
}

// A coherent read of the dext's status page (seqlock; the interrupt handler is the only writer).
static bool status_read(const volatile struct UFOhciStatus* s, struct UFOhciStatus* out) {
    for (int retry = 0; retry < 4; ++retry) {
        const uint64_t s0 = s->seq;
        if (s0 & 1) continue;                       // write in progress
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        out->irqCount    = s->irqCount;
        out->rxCompleted = s->rxCompleted;
        out->txSent      = s->txSent;
        out->hostTime    = s->hostTime;
        out->cycleTimer  = s->cycleTimer;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (s->seq == s0) return s0 != 0;           // seq 0 = the interrupt has never fired
    }
    return false;
}

// ── a shared-memory ring, created + owned by the daemon ─────────────────────────────────────────
static uf::shm::Ring* make_ring(const char* name, uint64_t instanceId) {
    shm_unlink(name);
    int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
    if (fd < 0) { std::perror("shm_open"); return nullptr; }
    if (ftruncate(fd, sizeof(uf::shm::Ring)) != 0) { std::perror("ftruncate"); close(fd); return nullptr; }
    void* p = mmap(nullptr, sizeof(uf::shm::Ring), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { std::perror("mmap"); return nullptr; }
    auto* r = static_cast<uf::shm::Ring*>(p);
    r->init(instanceId);
    return r;
}

// ── the streaming session ─────────────────────────────────────────────────────────────────────
// Everything from claiming the device to the device fetching PCM, plus the packet geometry that
// follows from the rate. Factored out of main() because it now has three callers, not one: initial
// bring-up, a rate change requested by CoreAudio, and wedge recovery. Those last two are the same
// operation — stop the session, start it again — which is exactly why this is a function.
struct Session {
    uint32_t rate;
    uf::Speed speed;
    uint32_t dbq;             // data-block quadlets = PCM channels at this speed class
    uint32_t rx_channel;      // the iso channel WE transmit playback on
    uint32_t dbqFlag;
    uint32_t tx_channel;      // the iso channel the DEVICE transmits capture on
    uint32_t sytInterval;     // frames per full transmit packet
    uint32_t fullPayload;     // bytes per full transmit packet
    uint32_t fullCount;       // full packets per ring lap (the blocking cadence)
};

static void session_stop(io_connect_t conn, const Session& s) {
    // Fetch off first so the device stops pulling PCM, then the DMA contexts, then the device-side
    // comm-stop. Stopping DMA before the device stop-write is the order the device requires.
    { std::vector<uint32_t> ones(s.dbq, 1); wblock(conn, uf::reg::kStatus0, ones.data(), s.dbq); }
    IOConnectCallScalarMethod(conn, kIsoStop, nullptr, 0, nullptr, nullptr);
    IOConnectCallScalarMethod(conn, kIsoTxStop, nullptr, 0, nullptr, nullptr);
    { const uint32_t blob[3] = {0u, 0u, 0u}; wblock(conn, uf::reg::kInitBankStop, blob, 3); }
}

// Bring the mixer model up once per daemon lifetime: defaults, then whatever was saved over the
// top. Called early in session_start because BOTH the matrix and the output-record (loopback) mask
// are re-applied from it, and the mask is written first.
static void mixer_init_once(uint32_t dbq) {
    if (g_mixerInit) return;
    g_model.reset_defaults(dbq, getenv("UF_NODIAG") == nullptr);
    // Restore a previously saved mixer over the defaults. Absent, truncated or an unknown
    // version all mean "carry on with defaults" — see uf_state_store.hpp for why guessing at
    // an unrecognised file is worse than ignoring it.
    //
    // A v1 file holds the rendered matrix from before the model existed; it is adopted
    // rather than discarded, so this upgrade does not cost anyone their routing.
    union { uf::MixerModel::Blob blob; uint32_t cells[uf::MixerShadow::kCells]; } saved{};
    uint32_t bytes = 0;
    const uint32_t ver = uf::load_blob(uf::state_path(), &saved, sizeof saved, &bytes);
    if (ver == uf::kStateVersion && bytes == sizeof(uf::MixerModel::Blob)) {
        g_model.from_blob(saved.blob);
    } else if (ver == uf::kStateVersionCells) {
        g_model.import_cells(saved.cells, bytes / 4);
    }
    if (ver && !g_sessionQuiet)
        std::printf("uf-daemon: mixer state restored from %s (v%u)\n",
                    uf::state_path().c_str(), ver);
    g_mixerInit = true;
}

// Apply control commands with NO DEVICE PRESENT: model, published state and disk only.
//
// A cut-down twin of the pump's drain, and deliberately not shared with it — the pump's version has
// to coalesce, rate-limit and push register writes, none of which mean anything here. What matters
// is that both keep the model as the single source of truth, so whatever is set while waiting is
// exactly what session_start applies when the device turns up.
static void drain_ctl_idle(uf::CtlQueue& q, uf::state::Shared* pub) {
    uf::CtlCommand c;
    bool dirty = false;
    while (q.pop(&c)) {
        const auto kind = (uf::MixerSrcKind)c.srcKind;
        switch (c.type) {
            case uf::ctl::MsgType::SetMixer:
                g_model.cell(kind, c.src, c.dest).gain = c.coeff; break;
            case uf::ctl::MsgType::SetFader:
                g_model.output(c.out).gain = c.coeff; break;
            case uf::ctl::MsgType::SetCellFlags: {
                auto& f = g_model.cell(kind, c.src, c.dest).flags;
                f = (uint8_t)((f & ~c.mask) | (c.flags & c.mask));
                break;
            }
            case uf::ctl::MsgType::SetOutFlags: {
                auto& f = g_model.output(c.out).flags;
                f = (uint8_t)((f & ~c.mask) | (c.flags & c.mask));
                break;
            }
            case uf::ctl::MsgType::SetStereo:
                g_model.set_stereo(kind, c.src, c.on != 0); break;
            case uf::ctl::MsgType::Submix:
                if (c.on) g_model.clear_submix(c.dest); else g_model.copy_submix(c.src, c.dest);
                break;
            case uf::ctl::MsgType::SetSettings:
                // Adopted, not written: the conf block needs a device. session_start assembles it
                // from g_settings, so this arrives with the session.
                g_settings = c.settings;
                break;
            default: continue;
        }
        dirty = true;
    }
    if (!dirty) return;
    g_model.render(g_mixer);
    g_stateSnap = g_model.to_blob();
    g_stateSnapSeq.fetch_add(1, std::memory_order_release);
    if (pub) uf::state::publish(pub, [](uf::state::Shared* o) {
        publish_mixer(o);
        o->settings = g_settings;
    });
}

static bool session_start(io_connect_t conn, uint32_t rate, Session* out) {
    if (!uf::is_supported_rate(rate)) {
        std::fprintf(stderr, "uf-daemon: unsupported rate %u\n", rate);
        return false;
    }
    Session s = {};
    s.rate       = rate;
    s.speed      = uf::speed_for_rate(rate);
    s.dbq        = uf::data_block_quadlets(s.speed);
    s.rx_channel = 1;
    s.dbqFlag    = s.dbq | 0x800u;   // S800 double-speed flag rides with dbq
    const uint32_t dbq = s.dbq, rx_channel = s.rx_channel, dbqFlag = s.dbqFlag;
    const uf::Speed speed = s.speed;

    // UF_STEP_PAUSE=<sec> pauses after each setup step, so a human can see which write changes the
    // front-panel HOST LED (red at power-on -> off -> ?).
    auto step_pause = [&](const char* label) {
        if (const char* sp = getenv("UF_STEP_PAUSE")) {
            std::printf(">>> STEP: %s — WATCH THE LED (%ss)\n", label, sp);
            std::fflush(stdout);
            usleep((useconds_t)(atof(sp) * 1e6));
        }
    };

    // [0] CLAIM THE DEVICE FIRST. The host-LED register is written before the rate and before any
    // stream op, unconditionally, on attach and every bus reset. An unclaimed FF800 runs
    // in standalone mode driving its own output source — which matches our symptom exactly (a fixed
    // tone on every output, independent of what we transmit) and the HOST LED staying red. We used to
    // write this LAST, after fetch-enable.
    wq(conn, uf::reg::kHostLed, 0);
    usleep(20 * 1000);
    step_pause("0: hostLED 0x324<-0 (claim)");

    // Lock the device to the configured clock source at `rate` first. Without this it sits in
    // autosync with nothing locked and free-runs (observed: 64 kHz / 8000 full packets/s), so the
    // stream rate is wrong. The conf block carries the clock mode; g_settings defaults to internal
    // master and is what the control plane edits.
    { uf::ConfBlock cb = uf::assemble_conf_block(g_settings);
      const uint32_t conf[3] = {cb.cr0, cb.cr1, cb.cr2};
      wblock(conn, uf::reg::kConfBlock, conf, 3); }
    usleep(20 * 1000);
    step_pause("conf block 0xfc88f014 (internal master)");

    // 0x801c0000 = FORMER_REG_FETCH_PCM_FRAMES (snd-fireface). 0 per channel = FETCH (play) that
    // channel, 1 = don't fetch. Critically, snd-fireface enables fetch (writes 0) only AFTER the iso
    // streams are up and running (snd_ff_stream_start_duplex). Enabling it before the stream exists
    // makes the device fetch garbage → the beep. So DISABLE fetch (write 1s) during setup here, and
    // enable it last (step [5], after iso start + settle).
    { std::vector<uint32_t> ones(dbq, 1);
      wblock(conn, uf::reg::kStatus0, ones.data(), dbq);
      if (!g_sessionQuiet) std::printf("uf-daemon: [1] fetch DISABLED (0x801c0000 <- 1) during setup\n"); }
    step_pause("1: fetch disabled 0x801c0000<-1");

    // 0x801c0080 = OUTPUT_REC_MASK (FFADO set_hardware_output_rec writes 28 quads of (rec!=0); the
    // factory driver writes the same 28-quadlet block). We have NEVER written this. It is the one
    // playback-side register both reference drivers touch and we don't.
    //
    // Now driven by the mixer model: this register IS TotalMix's Loopback (manual §27.5), one flag
    // per hardware output, so it has to be re-applied on every session start alongside the matrix or
    // a rate change silently drops the user's loopbacks. UF_REC still forces every output on.
    mixer_init_once(dbq);
    if (!getenv("UF_REC_SKIP")) {
        std::vector<uint32_t> rec(dbq, 0);
        if (getenv("UF_REC")) {
            rec.assign(dbq, (uint32_t)strtoul(getenv("UF_REC"), nullptr, 0));
        } else {
            const auto m = g_model.rec_mask();
            for (uint32_t i = 0; i < dbq && i < m.size(); ++i) rec[i] = m[i];
        }
        wblock(conn, uf::reg::kOutputRecMask, rec.data(), dbq);
        if (!g_sessionQuiet) std::printf("uf-daemon: [1b] OUTPUT_REC_MASK 0x801c0080 <- %u x%u\n", rec[0], dbq);
    }

    // The device also accepts a 4-quadlet BLOCK write at 0xfc88f004 covering f004/f008/f00c/f010
    // atomically — where snd-fireface writes them as separate quadlets and we write none. It is
    // additive to the 0x0002 init below. q[2]/q[3] (comm start/stop) left 0 so we don't trigger
    // streaming here. UF_FC88F=<n quads>, default off.
    if (const char* nq = getenv("UF_FC88F")) {
        const uint32_t q4[4] = {(dbq << 11) | rx_channel, dbq, 0u, 0u};
        uint32_t n = (uint32_t)atoi(nq); if (n < 1 || n > 4) n = 4;
        wblock(conn, uf::reg::kRxPacketFormat, q4, n);
        if (!g_sessionQuiet) std::printf("uf-daemon: [1c] 0xfc88f004 block <- %u quads {%08x,%08x,%08x,%08x}\n",
                    n, q4[0], q4[1], q4[2], q4[3]);
        usleep(20 * 1000);
    }

    // Session up on the 0x0002 bank. The tested FF800 firmware treats the fc88f STF as dead code and
    // uses this bank. init[1] = (dbq<<11)|rx_channel IS the rx-packet-format (playback-arm) value;
    // init[0]=rate, init[2]=dbq|s800flag.
    // UF_RATE_LATCH: standalone single-quad rate latch to 0x2_0000001c BEFORE the 3-quad init,
    // which the factory driver always does and we skip.
    if (getenv("UF_RATE_LATCH")) { wq(conn, uf::reg::kInitBankStream, rate);
        if (!g_sessionQuiet) std::printf("uf-daemon: [LED6] standalone rate latch 0x2_0000001c <- %u\n", rate); }
    // UF_MIDI_HOST: register a host async-receive address at 0x2_00000320 (snd-fireface does this
    // at probe + every reset; we never do). MidiInEnable writes (localNodeId<<16)|highIndex there.
    if (getenv("UF_MIDI_HOST")) { uint64_t a = 0x11; IOConnectCallScalarMethod(conn, kMidiInEnable, &a, 1, nullptr, nullptr);
        if (!g_sessionQuiet) std::printf("uf-daemon: [LED2] host address registered at 0x2_00000320 (MidiInEnable)\n"); }
    { const uint32_t init[3] = {rate, (dbq << 11) | rx_channel, dbqFlag};
      wblock(conn, uf::reg::kInitBankStream, init, 3); }
    usleep(100 * 1000);
    step_pause("init 0x2_0000001c 3-quad");

    uint32_t tx_channel = 0xffffffff;
    for (int i = 0; i < 100 && tx_channel == 0xffffffff; ++i) {
        if (!rq(conn, uf::reg::kTxIsoChannel, &tx_channel)) break;
        if (tx_channel == 0xffffffff) usleep(20 * 1000);
    }
    if (tx_channel == 0xffffffff) {
        if (!g_sessionQuiet) std::fprintf(stderr, "uf-daemon: no tx channel\n");
        return false;
    }
    s.tx_channel = tx_channel;
    if (!g_sessionQuiet) std::printf("uf-daemon: STF+RxFormat+AllocTx done, device tx ch=%u\n", tx_channel);
    step_pause("tx channel published");

    // Continuous capture + a blocking transmit stream, then open the session and fetch PCM. Blocking
    // (ref/snd-fireface): full packets carry syt_interval frames; fullPerFour full packets per 4 hit
    // the rate/8000 average (3/4 for 48/96/192 kHz).
    // The factory driver transmits NON-BLOCKING: a fixed frames/packet in EVERY cycle, no
    // empty/no-data packets. The FF800's receiver wants that steady stream — the
    // blocking cadence (full packets interleaved with 0-length empties) makes it click at the empty
    // rate (a fixed-pitch buzz independent of the audio). For rates divisible by 8000 the per-cycle
    // frame count is exact (48k->6, 96k->12, 192k->24); fall back to blocking only for 44.1/88.2k.
    uint32_t sytInterval, fullPayload, fullCount;
    if (const char* fp = getenv("UF_TX_FPP")) {          // DIAGNOSTIC: force frames/packet, blocking cadence
        sytInterval = (uint32_t)atoi(fp);
        fullPayload = sytInterval * dbq * 4;
        fullCount   = rate * kTxSlots / (8000u * sytInterval);
        if (!g_sessionQuiet) std::printf("uf-daemon: UF_TX_FPP=%u fullPayload=%u fullCount=%u\n", sytInterval, fullPayload, fullCount);
    } else if (rate % 8000 == 0) {
        sytInterval = rate / 8000;
        fullPayload = sytInterval * dbq * 4;
        fullCount   = kTxSlots;                                           // every packet full: no empties
    } else {
        sytInterval = uf::frames_per_packet(speed);                      // 8/16/32 frames per full pkt
        fullPayload = sytInterval * dbq * 4;
        fullCount   = uf::blocking_full_count(rate, speed);              // full packets in the 640-ring
    }
    // [2] TotalMix matrix: zero every gain, then route each FireWire playback channel to its own
    // physical output at unity + bring the per-output faders to unity (zeroing set them to 0 = mute).
    // FF800 layout (ffado set_hardware_mixergain): per-output block 0x100; playback src at +0x80+4*src;
    // output fader block at 0x1f80. Unity = 0x8000, mute = 0. (Before comm-start.)
    if (!getenv("UF_MIXER_SKIP")) {
        // Re-apply THE SHADOW, not a hardcoded default. On the first session it holds the defaults,
        // so behaviour is unchanged; on every session after — rate change, wedge recovery, bus reset
        // — it still holds whatever routes the user has since set, which is the entire point.
        //
        // Still one QUADLET per cell: FFADO writes the mixer that way and the FF800 may ignore block
        // writes, which would leave the matrix full of power-on garbage the unmuted outputs then
        // play (the original beep). Every cell is written even where we believe it is zero, because
        // the device's RAM is garbage until we say otherwise.
        g_model.render(g_mixer);
        g_mixer.apply_all([&](uf::Addr a, uint32_t v) { wq(conn, a, v); });
        if (!g_sessionQuiet) std::printf("uf-daemon: [2] TotalMix matrix applied from the shadow\n");
    }

    // [3] comm-start — BEFORE the iso streams, so the device is armed for playback when our transmit
    // begins. This is the ordering the device requires; we previously started the iso contexts first,
    // which left the device ignoring our playback entirely.
    wq(conn, uf::reg::kInitBankStart, 0x80000000u | dbqFlag);   // 0x0002 bank comm-start
    if (!g_sessionQuiet) std::printf("uf-daemon: [3] comm-start (fc88f00c)\n");
    usleep(5 * 1000);
    step_pause("3: comm-start 0x2_00000028");

    // [4] iso start AFTER comm-start: capture context on the device-published channel, then our
    // transmit on rx_channel (the device now listening for playback there).
    call1(conn, kIsoStartCont, tx_channel);
    if (!getenv("UF_NO_TX")) {
        uint64_t a[3] = {rx_channel, fullPayload, fullCount};
        IOConnectCallScalarMethod(conn, kIsoTxStart, a, 3, nullptr, nullptr);
    } else if (!g_sessionQuiet) std::printf("uf-daemon: [4] TRANSMIT SKIPPED (UF_NO_TX)\n");
    std::printf("uf-daemon: [4] iso started\n");
    step_pause("4: iso streams started");

    // [5] ENABLE fetch LAST — snd-fireface enables fetch only after the iso streams are up and running
    // (snd_ff_stream_start_duplex → switch_fetching_mode(true)). Give the streams a moment to settle so
    // the device is receiving a valid packet cadence before it starts fetching PCM to its outputs.
    usleep(getenv("UF_FETCH_DELAY_MS") ? atoi(getenv("UF_FETCH_DELAY_MS")) * 1000 : 150 * 1000);
    if (!getenv("UF_FETCH_SKIP")) {
        uint32_t v = getenv("UF_FETCH") ? (uint32_t)strtoul(getenv("UF_FETCH"), nullptr, 0) : 0u;  // 0 = fetch/play
        std::vector<uint32_t> mask(dbq, v);
        wblock(conn, uf::reg::kStatus0, mask.data(), dbq);
        std::printf("uf-daemon: [5] fetch ENABLED (0x801c0000 <- %u) after streams settled\n", v);
        step_pause("5: fetch ENABLED 0x801c0000<-0 (device fetches host PCM) — GREEN?");
    }

    s.sytInterval = sytInterval;
    s.fullPayload = fullPayload;
    s.fullCount   = fullCount;
    *out = s;
    std::printf("uf-daemon: session up at %u Hz (%u ch, %u frames/pkt)\n", rate, dbq, sytInterval);
    return true;
}

int main() {
    std::signal(SIGUSR1, on_arm_capture);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // Control socket, up BEFORE we even look for the device. Front-ends (uf-mix, uf-set, the TUI)
    // send commands here instead of opening their own dext client — which also removes the
    // AT-context collision, since the dext serialises nothing and two user clients issuing register
    // transactions can interleave.
    //
    // Deliberately first: a UI has to be able to connect and show state while the daemon is still
    // waiting for hardware, not be refused until streaming starts. Commands arriving before the
    // device exists WAIT IN THE QUEUE — the pump has not started, so nothing folds them into the
    // shadow yet — and are applied in order once it does. Beyond kSlots the oldest are dropped,
    // which is the right trade for a state-set command. Failure to bind is non-fatal: audio does not
    // depend on the control plane.
    uf::CtlQueue ctlQueue;
    uf::CtlServer ctlServer;
    ctlServer.start(&ctlQueue);

    // Published state, for anything that wants to READ what the device is doing (uf-status and the
    // TUI today, a GUI later). Separate from the audio ring on purpose: growing that would change its
    // ABI and force the AudioServerPlugIn to be rebuilt in lockstep for data it does not use.
    uf::state::Shared* pubState = nullptr;
    {
        // Unlink any previous segment FIRST. macOS lets a POSIX shm object be ftruncate'd exactly
        // ONCE, at creation — so a stale segment left by an older build (a different struct size)
        // can never be resized, ftruncate fails with EINVAL, and state publication silently stops.
        // Unlinking costs nothing: existing readers keep their mapping, new ones get the new object.
        shm_unlink(uf::state::kName);
        int fd = shm_open(uf::state::kName, O_CREAT | O_RDWR, 0666);
        if (fd >= 0) {
            if (ftruncate(fd, sizeof(uf::state::Shared)) == 0) {
                void* p = mmap(nullptr, sizeof(uf::state::Shared), PROT_READ | PROT_WRITE,
                               MAP_SHARED, fd, 0);
                if (p != MAP_FAILED) { pubState = (uf::state::Shared*)p; pubState->init(); }
            }
            close(fd);
        }
        if (!pubState)
            std::fprintf(stderr, "uf-daemon: could not publish state shm: %s (non-fatal — "
                                 "control still works, but UIs cannot read state)\n", strerror(errno));
    }

    // Saver thread. Writes the mixer state to disk when it has been STABLE for a moment, rather than
    // on every change: dragging a fader produces a stream of updates and there is no reason to touch
    // the SSD for each one. It only ever reads g_stateSnap, which the pump fills, so it can never
    // observe a half-updated matrix.
    std::thread stateSaver([&] {
        uint64_t seen = 0, lastSeq = 0;
        int stable = 0;
        while (g_stateRun.load(std::memory_order_relaxed)) {
            usleep(250 * 1000);
            const uint64_t seq = g_stateSnapSeq.load(std::memory_order_acquire);
            if (seq != lastSeq) { lastSeq = seq; stable = 0; continue; }   // still changing: wait
            if (seq == seen || seq == 0) continue;                        // nothing new to write
            if (++stable < 4) continue;                                   // ~1 s of quiet
            ::mkdir(uf::state_dir().c_str(), 0755);                       // harmless if it exists
            if (uf::save_blob(uf::state_path(), &g_stateSnap, sizeof g_stateSnap)) seen = seq;
        }
    });

    // EVERY exit path goes through this, including the early returns in the wait loops below.
    //
    // The final save and the join used to sit inline at the end of main, which meant a SIGTERM
    // arriving while the daemon was still waiting for hardware skipped both — and a std::thread
    // destroyed while joinable calls std::terminate, so the daemon ABORTED (SIGABRT, "libc++abi:
    // terminating") on every such shutdown, silently losing whatever had been set since the saver
    // last ran. It is in the log dozens of times.
    //
    // The save is here rather than only in the saver thread because the saver waits for a second of
    // quiet before writing: a change made just before shutdown would otherwise be lost exactly when
    // someone expects it to have been remembered.
    struct SaverGuard {
        std::thread& t;
        ~SaverGuard() {
            if (g_stateSnapSeq.load(std::memory_order_acquire)) {
                ::mkdir(uf::state_dir().c_str(), 0755);
                uf::save_blob(uf::state_path(), &g_stateSnap, sizeof g_stateSnap);
            }
            g_stateRun.store(false);
            if (t.joinable()) t.join();
        }
    } saverGuard{stateSaver};

    // Wait for the device rather than exiting and letting launchd respawn us every
    // ThrottleInterval. With nothing plugged in, exiting means a process spawn AND a log line every
    // 10 seconds, forever, for a device that is simply not there — ~1 MB/day of noise that buries
    // the events worth reading. Announce once, then poll quietly; this also picks the device up in
    // ~2 s instead of ~10 when you plug it in.
    //
    // Deliberately NOT the same as the dead-connection case in restart_session, where exiting stays
    // correct: there the process needs a FRESH handle and launchd is how it gets one. Here there is
    // nothing to reconnect to yet.
    //
    // Readiness is "the FF800 answers a register read", not merely "the dext is loaded" — the dext
    // matches the OHCI controller, which is present whenever the Thunderbolt adapter is, with or
    // without an FF800 on the far end of the FireWire cable.
    // Bring the mixer model up NOW, before the device is even known to exist.
    //
    // It used to happen at the first session start, which meant that with no device attached the
    // daemon had no model, published none, and drained no commands: a UI showed factory defaults
    // while the user set routes, and those commands sat in the queue until a session began (or fell
    // out of it — the queue drops the oldest when full). The mixer is state the daemon owns; it does
    // not need hardware to exist, only to be applied.
    //
    // 28 rather than the session's channel count on purpose: the model deliberately persists across
    // rate changes, so which rate the daemon happened to start at should not decide how much of it
    // is populated. Cells beyond the current rate's channel count are simply not on the wire.
    mixer_init_once(uf::MixerModel::kCh);
    // Publish it straight away, so a UI attaching before any device or any edit sees the routing it
    // will actually get rather than an empty matrix.
    g_model.render(g_mixer);
    if (pubState) uf::state::publish(pubState, [&](uf::state::Shared* o) {
        publish_mixer(o);
        o->settings = g_settings;
    });

    io_connect_t conn = 0;
    {
        bool announced = false, hadDext = false;
        for (;;) {
            conn = open_dext();
            hadDext = (conn != 0);
            if (conn) {
                uint32_t probe = 0;
                if (rq(conn, uf::reg::kStatus0, &probe)) break;   // it answers — go
                IOServiceClose(conn);
                conn = 0;
            }
            if (!g_run.load()) return 0;          // Ctrl-C / SIGTERM while waiting is not an error
            // Keep serving clients while we wait. Nothing can be written to a device that is not
            // there, but the model can be updated, published and saved — and session_start applies
            // the whole matrix anyway, so a route set now simply arrives when the device does.
            if (!announced) {
                std::printf("[%s] uf-daemon: waiting for the FF800 (%s)\n", uf_now(),
                            hadDext ? "dext up, device not answering" : "dext not available");
                std::fflush(stdout);
                announced = true;
            }
            // Probe for the device every 2 s, but serve clients eight times as often: a UI whose
            // edits took two seconds to appear would feel broken, and there is no reason to couple
            // how fast we answer a person to how fast we poll for hardware.
            for (int i = 0; i < 8 && g_run.load(); ++i) {
                drain_ctl_idle(ctlQueue, pubState);
                usleep(250 * 1000);
            }
        }
        if (announced) { std::printf("[%s] uf-daemon: device present\n", uf_now()); std::fflush(stdout); }
    }
    g_conn = conn;


    // Rate via UF_RATE (default 48 kHz); CoreAudio can change it later through the shm ring.
    const uint32_t startRate = getenv("UF_RATE") ? (uint32_t)atoi(getenv("UF_RATE")) : 48000;
    // Same treatment for a device that is present but will not start — typically "no tx channel",
    // i.e. the FF800 is wedged and wants a power cycle. Exiting here meant launchd respawning every
    // 10 s and rewriting the same three lines forever. Say it once, keep trying quietly, and say so
    // again when it comes good. Whoever reads this log later gets the fault and its duration, not a
    // thousand copies of it.
    Session ses = {};
    {
        bool announced = false;
        for (unsigned attempt = 0; !session_start(conn, startRate, &ses); ++attempt) {
            if (!g_run.load()) return 0;
            if (!announced) {
                std::fprintf(stderr,
                    "[%s] uf-daemon: device present but the session will not start. If the log above "
                    "says 'no tx channel' the FF800 is wedged and needs a POWER CYCLE. Retrying "
                    "quietly every 5 s.\n", uf_now());
                std::fflush(nullptr);
                announced = true;
                g_sessionQuiet = true;   // first attempt's detail is kept; the repeats are not
            }
            sleep(5);
        }
        g_sessionQuiet = false;
        if (announced) { std::printf("[%s] uf-daemon: session started\n", uf_now()); std::fflush(stdout); }
    }
    uint32_t rate = ses.rate, dbq = ses.dbq, rx_channel = ses.rx_channel;
    uint32_t sytInterval = ses.sytInterval, fullPayload = ses.fullPayload, fullCount = ses.fullCount;

    // Map the dext's capture buffer (packets land here, 2 prefix quadlets + payload per slot).
    mach_vm_address_t dextAddr = 0; mach_vm_size_t dextSize = 0;
    if (IOConnectMapMemory64(conn, kDextMemType, mach_task_self(), &dextAddr, &dextSize,
                             kIOMapAnywhere | kIOMapReadOnly) != KERN_SUCCESS) {
        std::fprintf(stderr, "uf-daemon: cannot map dext capture buffer\n"); return 1;
    }
    const uint8_t* dextBuf = reinterpret_cast<const uint8_t*>(dextAddr);

    // Map the dext's transmit payload ring (read-write) so we can feed real playback PCM into it.
    mach_vm_address_t txAddr = 0; mach_vm_size_t txSize = 0;
    if (IOConnectMapMemory64(conn, kTxMemType, mach_task_self(), &txAddr, &txSize,
                             kIOMapAnywhere) != KERN_SUCCESS) {
        std::fprintf(stderr, "uf-daemon: cannot map dext transmit buffer\n"); return 1;
    }
    uint8_t* txBuf = reinterpret_cast<uint8_t*>(txAddr);
    std::memset(txBuf, 0, txSize);   // idle = true silence; else the FF800 plays stale DMA garbage

    // Map the dext's interrupt status page and subscribe to the isochronous completion interrupt.
    // Both are optional: if either fails we fall back to the 1 ms polled pump, which is what this
    // daemon did before there was an interrupt to block on. UF_NO_IRQ forces that path for A/B.
    const volatile struct UFOhciStatus* devStatus = nullptr;
    struct UFOhciControl* devControl = nullptr;
    mach_port_t wakePort = MACH_PORT_NULL;
    IONotificationPortRef notifyPort = nullptr;
    if (!getenv("UF_NO_IRQ")) {
        mach_vm_address_t stAddr = 0; mach_vm_size_t stSize = 0;
        if (IOConnectMapMemory64(conn, kUFOhciStatusMemoryType, mach_task_self(), &stAddr, &stSize,
                                 kIOMapAnywhere) == KERN_SUCCESS) {
            devStatus  = reinterpret_cast<const volatile struct UFOhciStatus*>(stAddr);
            devControl = reinterpret_cast<struct UFOhciControl*>(stAddr + kUFOhciControlOffset);
            notifyPort = IONotificationPortCreate(kIOMainPortDefault);
            wakePort = IONotificationPortGetMachPort(notifyPort);
            uint64_t ref[1] = {0};
            uint32_t outCnt = 0;
            kern_return_t kr = IOConnectCallAsyncScalarMethod(conn, kIsoWake, wakePort, ref, 1,
                                                              nullptr, 0, nullptr, &outCnt);
            if (kr != KERN_SUCCESS) {
                std::fprintf(stderr, "uf-daemon: iso wake subscribe failed (0x%x); polling\n", kr);
                wakePort = MACH_PORT_NULL;
            } else {
                std::printf("uf-daemon: pump is interrupt-driven (irq every %u packets)\n",
                            kUFOhciIrqEvery);
            }
        } else {
            std::fprintf(stderr, "uf-daemon: no dext status page; polling\n");
        }
    }
    const bool irqPump = wakePort != MACH_PORT_NULL && devStatus != nullptr;

    // The pump wants a 1 ms tick it actually hits: real-time scheduling for the priority, an absolute
    // deadline for the period. Both are gated so they can be A/B'd against the old behaviour.
    if (!getenv("UF_NO_RT")) pump_realtime(1.0, 0.3, 0.7);
    mach_timebase_info_data_t ptb; mach_timebase_info(&ptb);
    const uint64_t kPumpPeriodTicks = (uint64_t)(1.0e6 * ptb.denom / ptb.numer);   // 1 ms in mach ticks
    uint64_t deadline = mach_absolute_time(), pumpLate = 0, wakeTimeouts = 0;

    // One id per daemon run, stamped into both rings so the plugin can tell a restart from a
    // repeat sighting of the ring it already has.
    const uint64_t runId = mach_absolute_time();
    uf::shm::Ring* cap = make_ring(uf::shm::kCaptureName, runId);
    uf::shm::Ring* play = make_ring(uf::shm::kPlaybackName, runId);
    if (!cap || !play) return 1;
    // Tell the plugin what is actually on the wire. Until this is set it advertises nothing, so
    // CoreAudio cannot select a rate we are not at.
    cap->deviceRate.store(rate, std::memory_order_release);

    // MIDI endpoints. Created after the session is up so they only appear alongside a device that
    // can actually carry their traffic. UF_NO_MIDI skips them entirely.
    MIDIClientRef midiClient = 0;
    MIDIEndpointRef midiDest = 0;
    if (!getenv("UF_NO_MIDI")) {
        MIDIClientCreate(CFSTR("UserFace800"), nullptr, nullptr, &midiClient);
        MIDIDestinationCreate(midiClient, CFSTR("Fireface 800 MIDI Out"), midi_read_proc, nullptr,
                              &midiDest);
        MIDISourceCreate(midiClient, CFSTR("Fireface 800 MIDI In"), &g_midiSrc);
        // Advertise a host receive address so the device async-writes incoming MIDI to us. High
        // index 1 puts it at 0x1_00000000 — the first middle-address-space address, above the
        // physical-DMA range, so the writes land in the AR-request context rather than being DMA'd
        // straight into host memory (the same convention Linux's firewire core uses).
        uint64_t hi = 1;
        IOConnectCallScalarMethod(conn, kMidiInEnable, &hi, 1, nullptr, nullptr);
        std::printf("uf-daemon: MIDI endpoints up (out real; in hardware-gated)\n");
    }
    std::printf("uf-daemon: streaming (tx ch %u), rings up. Ctrl-C to stop.\n", ses.tx_channel);

    // LED probe: with a fully-established session up, apply a register write and watch the
    // front-panel HOST LED. UF_PROBE_ADDR=<48-bit hex>, UF_PROBE_VAL=<hex> (default 0). Applied once
    // here; UF_PROBE_PERIODIC re-asserts it each second in the pump. Selecting the address from the
    // environment means a different one needs no rebuild.
    // UF_RECLAIM_S: the factory driver re-asserts the WHOLE claim on every bus reset while streams
    // are up, which a claim written before the streams exist does not reproduce. Re-issue the full
    // claim once, UF_RECLAIM_S seconds after streams are locked.
    const double reclaimAt = getenv("UF_RECLAIM_S") ? atof(getenv("UF_RECLAIM_S")) : -1.0;
    bool reclaimed = false;
    const bool probePeriodic = getenv("UF_PROBE_PERIODIC");
    uint64_t probeAddr = 0; uint32_t probeVal = 0; bool probeOn = false;
    if (const char* pa = getenv("UF_PROBE_ADDR")) {
        probeAddr = strtoull(pa, nullptr, 16);
        probeVal = getenv("UF_PROBE_VAL") ? (uint32_t)strtoul(getenv("UF_PROBE_VAL"), nullptr, 16) : 0;
        probeOn = true;
        bool ok = wq(conn, probeAddr, probeVal);
        std::printf("uf-daemon: LED PROBE write 0x%llx <- 0x%08x -> %s\n",
                    (unsigned long long)probeAddr, probeVal, ok ? "ok" : "FAILED");
        std::fflush(stdout);
    }

    // ── the pump ────────────────────────────────────────────────────────────────────────────────
    // Capture: read newly-completed dext packets, decode each into an shm slot's frames. We pack one
    // shm slot per period; for scaffolding we simply forward frames as they arrive and let the slot
    // fill. Playback: (silence for now) drain the playback ring so the plugin never blocks.
    uint64_t consumed = 0;
    uint64_t deviceFrames = 0;    // TRUE device sample position: counts EVERY frame the FF800 sends,
    uint64_t lastPublish = 0;     // even ones dropped on a full ring. This is the clock — it must not
                                  // stall on overrun, or CoreAudio reads slow and the ring death-spirals.
    uf::shm::Slot* capSlot = nullptr;
    uint32_t capFill = 0;
    std::vector<int32_t> frame(dbq);
    uint64_t ticks = 0, lastConsumed = 0, lastFrames = 0;
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    // Host clock ticks per audio frame — used to back-date the clock anchor onto the period boundary
    // it belongs to (see the publish below).
    double hostTicksPerFrame = 1.0e9 / (double)rate * (double)tb.denom / (double)tb.numer;
    uint64_t rt0 = mach_absolute_time(), rf0 = 0, rc0 = 0, tx0 = 0, irq0 = 0, txHeld0 = 0;
    uint64_t toneStart = mach_absolute_time(); int toneCh = -1;   // UF_TX_TONE per-channel sweep
    std::vector<int32_t> wavSamps; uint64_t wavFrames = 0;        // UF_TX_WAV=<raw s32le stereo 48k>
    if (const char* wp = getenv("UF_TX_WAV")) {
        if (FILE* wf = fopen(wp, "rb")) {
            fseek(wf, 0, SEEK_END); long n = ftell(wf); fseek(wf, 0, SEEK_SET);
            wavSamps.resize(n / 4); fread(wavSamps.data(), 4, n / 4, wf); fclose(wf);
            wavFrames = wavSamps.size() / 2;
            std::printf("uf-daemon: loaded %llu stereo frames from %s\n", (unsigned long long)wavFrames, wp);
        } else std::fprintf(stderr, "uf-daemon: cannot open UF_TX_WAV file\n");
    }
    uint64_t ct0 = cycle_timer_ticks((uint32_t)scalar0(conn, kReadCycleTimer));   // bus-clock anchor
    uint64_t nFull = 0, nEmpty = 0, nOther = 0, nDupTs = 0; uint32_t lastTs = 0xffffffff;
    // Cycle-time continuity. A gap means the controller missed isochronous cycles, so capture has a
    // hole and the frame counts after it no longer describe the device's real position.
    uint32_t lastCycle = 0, worstGap = 0; bool haveCycle = false;
    uint64_t cycleGaps = 0, missedCycles = 0, cycleRestarts = 0;
    // Self-arming state for the restart above. UF_CYCLE_RESTART=0 forces it off entirely; =1 arms it
    // immediately, skipping the proving window (for testing the restart path itself).
    bool cycleArmed = getenv("UF_CYCLE_RESTART") && atoi(getenv("UF_CYCLE_RESTART")) == 1;
    bool cycleDisarmed = getenv("UF_CYCLE_RESTART") && atoi(getenv("UF_CYCLE_RESTART")) == 0;
    uint64_t lastCycleRestart = 0, cycleRestartBurst = 0;
    const double kCycleArmSec = 10.0;           // clean streaming needed before we believe the premise
    const double kCycleRestartFloorSec = 30.0;  // restarts closer together than this look like flapping
    // How big a gap to treat as a broken stream rather than a blip. One cycle is 125 us; 8 is a
    // millisecond of missing capture, which is already past anything the ring depth hides.
    const uint32_t kCycleGapRestart =
        getenv("UF_CYCLE_GAP") ? (uint32_t)atoi(getenv("UF_CYCLE_GAP")) : 8;
    // Transmit pacing state. txFillPkt is the monotonic packet index we have filled up to and mirrors
    // the dext's itFillCursor; it starts one lap ahead because IsoTxStart leaves the whole ring
    // populated with silence. txSrc below threads a frame cursor through the plugin's slots.
    uint64_t txFillPkt = kTxSlots, txResyncs = 0, txDry = 0, txLastSent = 0, txHeld = 0;
    // Where a transmit frame comes from: the playback ring when it has one, the last real frame
    // (decaying) when it does not. Lives in platform/shared so it can be unit-tested with no FF800
    // attached — tests/test_tx_fill.cpp. The per-tick `dry` latch that used to live here inline is
    // what caused the 192 kHz ring modulation, and it went unfound for days precisely because
    // nothing could reach it from a test.
    uf::TxFill txSrc(dbq);

    // Level meters. Accumulated from the PCM we already handle, so they cost no FireWire traffic and
    // need no calibration constant (uf_meters.hpp); the device's own meter region is polled
    // separately below, raw, because its scaling is still open.
    uf::MeterBank<uf::shm::kMaxChannels> inMeters, pbMeters;
    uint64_t meterNextNs = 0;
    // ~30 Hz. Faster buys nothing a person can see, and every publish bumps the state seqlock that
    // UIs read the matrix through.
    static const uint64_t kMeterPeriodNs = 33'000'000;
    // The device meter poll is OFF unless asked for. It is the only metering that costs a FireWire
    // transaction, and until spec/10 §10.3's calibration is settled the numbers it publishes are raw
    // — useful for working that out, not yet for drawing a bar.
    const bool meterDevice = getenv("UF_METER_DEVICE") != nullptr;
    const bool txFreeRun = getenv("UF_TX_FREERUN") != nullptr;   // A/B against the old free-running fill
    const bool txPaced   = !txFreeRun && !getenv("UF_NO_TX");
    // Clock servo. The FF800 fetches playback at its own crystal (~48002), we deliver a base 48000
    // (sytInterval frames every bus cycle); the deficit drains its playback FIFO (SR0 bit 19 latches
    // ~26 s in). The servo sprinkles occasional (sytInterval+1)-frame packets so the long-term average
    // tracks the measured device rate. servoAccum carries the fractional frame debt across packets.
    const bool servoOn   = txPaced && !getenv("UF_NO_SERVO");
    double servoAccum = 0.0;
    bool servoReady = false;                          // a valid (non-startup) rate sample has arrived
    double measuredRate = (double)rate;               // device frames/bus-s, updated once per second
    double baseRate = (double)(8000u * sytInterval);   // what an all-sytInterval cadence delivers
    std::vector<uint32_t> slotBytes(kTxSlots, fullPayload);  // payload currently programmed per slot
    // UF_TX_PACED_TONE=<ch>: synthesise a phase-continuous tone straight into the paced+servo path, so
    // the servo can be tested from the daemon alone (the static UF_TX_TONE ring cannot be servoed).
    const int pacedToneCh = getenv("UF_TX_PACED_TONE") ? atoi(getenv("UF_TX_PACED_TONE")) : -1;
    const double pacedToneFreq = getenv("UF_TX_FREQ") ? atof(getenv("UF_TX_FREQ")) : 437.0;
    double pacedTonePhase = 0.0;
    const double pacedTonePhaseInc = 2.0 * M_PI * pacedToneFreq / (double)rate;
    // UF_TX_VIBRATO=<Hz>: slow frequency modulation, off by default. A dead-steady tone is the
    // right thing to MEASURE — sidebands stand out against it — but it is a poor thing to JUDGE by
    // ear, because there is nothing to compare the timbre against and roughness is easy to talk
    // yourself into. A tone that moves gives the ear a reference: the pitch should glide smoothly
    // and the timbre should not change with it. Phase-continuous, so it adds no clicks of its own.
    const double vibHz = getenv("UF_TX_VIBRATO") ? atof(getenv("UF_TX_VIBRATO")) : 0.0;
    const double vibDepth = getenv("UF_TX_VIB_DEPTH") ? atof(getenv("UF_TX_VIB_DEPTH")) : 0.03;
    double vibPhase = 0.0;
    const double vibPhaseInc = 2.0 * M_PI * vibHz / (double)rate;
    if (vibHz > 0)
        std::printf("uf-daemon: tone %.1f Hz with %.2f Hz vibrato, depth %.1f%%\n",
                    pacedToneFreq, vibHz, vibDepth * 100.0);
    uint64_t txCursor = 0; uint32_t txFill = 0; uint8_t* txSlotPtr = txBuf;
    // UF_CAP_WAV=<path>: write decoded capture straight to a WAV, taken from the decode loop BEFORE
    // the shm ring. That matters — it is a copy, not a second consumer, so it does not break the
    // ring's single-producer/single-consumer invariant the way an external capture tool would. With
    // this plus UF_TX_PACED_TONE, a loopback cable gives a measurement with no CoreAudio anywhere in
    // it: the daemon generates the signal and the daemon records what came back.
    const char* capWavPath = getenv("UF_CAP_WAV");
    const double capWavSecs = getenv("UF_CAP_SECONDS") ? atof(getenv("UF_CAP_SECONDS")) : 10.0;
    const uint64_t capWavLimit = capWavPath ? (uint64_t)(capWavSecs * rate) : 0;
    std::vector<int32_t> capWav;
    uint64_t capWavFrames = 0;
    bool capWavDone = false;
    // UF_CAP_SIGNAL=1 holds the capture back until SIGUSR1, so it can be triggered mid-episode.
    const bool capOnSignal = getenv("UF_CAP_SIGNAL") != nullptr;
    if (capOnSignal) capWavDone = true;          // "not capturing"; SIGUSR1 clears it
    if (capWavPath) {
        capWav.reserve((size_t)capWavLimit * dbq);
        std::printf("uf-daemon: capturing %.1f s of %u-channel input to %s\n",
                    capWavSecs, dbq, capWavPath);
        if (capOnSignal)
            std::printf("uf-daemon: capture ARMED ON SIGNAL — kill -USR1 %d to start it\n", getpid());
    }

    // Cells changed by control commands but not yet pushed to the device. A map, so repeated writes
    // to the same cell collapse to one — the whole point of coalescing.
    std::map<uf::Addr, uint32_t> ctlDirty;
    // Set by changes that touch no device register (stereo pairing) but must still be saved and
    // republished, so they are not lost just because nothing needed writing.
    bool stateDirty = false;
    static const uint32_t kCtlWritesPerTick = 8;
    uint32_t restartFailures = 0;    // consecutive failed session rebuilds (see restart_session)
    // ~25 s at the 5 s wedge backoff: long enough that a device still settling is not given up on,
    // short enough that a replug recovers while you are still looking at it.
    const uint32_t kMaxRestartFailures =
        getenv("UF_MAX_RESTART_FAILURES") ? (uint32_t)atoi(getenv("UF_MAX_RESTART_FAILURES")) : 5;
    uint64_t lastAnchorAt = 0;       // when we last published a clock anchor
    double   anchorGapMaxMs = 0.0;   // worst anchor interval this second (see the publish site)
    uint64_t playMinDepth = ~0ull;   // shallowest the playback ring got this second
    uint64_t lastProgress = mach_absolute_time();   // when capture last advanced (wedge detection)
    uint64_t lastConsumedForWedge = 0, wedgeRestarts = 0, restarts = 0;
    uint64_t seenRequest = cap->requestSeq.load(std::memory_order_acquire);
    uint64_t seenControl = cap->controlSeq.load(std::memory_order_acquire);
    // How long capture may stall before we call it a wedge. Comfortably longer than any scheduling
    // hiccup (the pump ticks at 1 kHz) and far shorter than a human noticing the audio has died.
    const double kWedgeMs = getenv("UF_WEDGE_MS") ? atof(getenv("UF_WEDGE_MS")) : 500.0;
    // UF_QUIET=1 drops the once-a-second telemetry. Measured at ~600 B/s that is 2.2 MB an hour and
    // ~52 MB a day — fine for a session you are watching, far too much for a service running from
    // login to shutdown. EVENTS are never suppressed (session up/down, restarts, wedges, errors):
    // those are exactly the lines worth having when something went wrong overnight.
    const bool quiet = getenv("UF_QUIET") != nullptr;
    const bool debugInbound = getenv("UF_DEBUG_INBOUND") != nullptr;   // see the call site

    // Tear the session down and bring it back up, at `newRate`. Used for a CoreAudio rate change and
    // for wedge recovery — the same operation, which is why they share this.
    //
    // deviceFrames deliberately does NOT reset. It is the sample position CoreAudio's timeline is
    // built on, and rewinding it would hand the plugin a backwards jump; carrying it across the seam
    // just means the clock's slope changes, which is what a rate change IS. Everything tied to the
    // dext's cursors does reset, because IsoStartContinuous/IsoTxStart rewind those to zero.
    auto restart_session = [&](uint32_t newRate, const char* why) -> bool {
        std::printf("[%s] uf-daemon: RESTART (%s) -> %u Hz\n", uf_now(), why, newRate);
        std::fflush(stdout);
        cap->deviceRate.store(0, std::memory_order_release);   // "nothing on the wire" while we switch
        // Drop our cursors before the contexts restart. They are monotonic counts into the OLD
        // session, and IsoStartContinuous/IsoTxStart rewind the dext's to zero — an interrupt landing
        // in between would apply a cursor from the previous run and release packets the new session
        // has only just captured.
        if (devControl) { devControl->releaseUpTo = 0; devControl->txFillUpTo = 0; }
        session_stop(conn, ses);
        Session next = {};
        if (!session_start(conn, newRate, &next)) {
            std::fprintf(stderr, "[%s] uf-daemon: restart FAILED at %u Hz\n", uf_now(), newRate);
            // "no tx channel" is not something we can recover from: the FF800 itself is wedged and
            // needs a power cycle. Retrying is still right (a replug DOES clear it), but repeating
            // an identical failure forever without naming the remedy is useless to whoever reads
            // this log later. Say it once, clearly, then stop shouting.
            // A dead user client is the common cause and it is UNRECOVERABLE IN PLACE. Replugging
            // the FF800 (or reinstalling the dext) re-matches the driver, so the dext gets a NEW
            // process — and our io_connect_t still refers to the old one. Every call then fails, no
            // amount of retrying helps, and KeepAlive cannot save us because we never exit.
            //
            // Re-opening the client in place would mean redoing every buffer mapping and all the
            // session state, i.e. everything main() does at startup. Exiting is the same work with
            // none of the risk: under launchd we are restarted within ThrottleInterval with a clean
            // connection, which is also exactly the behaviour that makes replug work. Run by hand,
            // the message says what happened.
            //
            // This is safe across sleep: the machine suspends us too, so failures do not accumulate
            // in real time, and the dext survives a sleep/wake cycle with the same process.
            if (++restartFailures >= kMaxRestartFailures) {
                std::fprintf(stderr,
                    "[%s] uf-daemon: %u consecutive restart failures — exiting so a fresh start can "
                    "reconnect. Usually means the dext restarted (device replug or reinstall) and "
                    "our connection to it died. Under launchd this restarts automatically.\n",
                    uf_now(), restartFailures);
                std::fflush(nullptr);
                std::exit(1);
            }
            return false;
        }
        restartFailures = 0;
        ses = next;
        rate = ses.rate; dbq = ses.dbq; rx_channel = ses.rx_channel;
        sytInterval = ses.sytInterval; fullPayload = ses.fullPayload; fullCount = ses.fullCount;

        consumed = 0;                       // the dext rewound both context cursors
        lastConsumed = 0;                   // else the per-second delta underflows into nonsense
        capSlot = nullptr; capFill = 0;
        txFillPkt = kTxSlots; txLastSent = 0; txCursor = 0; txFill = 0; txSlotPtr = txBuf;
        txSrc.reset();   // the restarted session gets a fresh ring; drop any held slot
        frame.assign(dbq, 0);
        slotBytes.assign(kTxSlots, fullPayload);
        std::memset(txBuf, 0, txSize);      // stale PCM at the old geometry is noise at the new one
        servoAccum = 0.0; servoReady = false; measuredRate = (double)rate;
        baseRate = (double)(8000u * sytInterval);
        hostTicksPerFrame = 1.0e9 / (double)rate * (double)tb.denom / (double)tb.numer;
        rt0 = mach_absolute_time(); rf0 = deviceFrames; rc0 = 0; tx0 = 0;
        ct0 = cycle_timer_ticks((uint32_t)scalar0(conn, kReadCycleTimer));
        lastProgress = mach_absolute_time();
        ++restarts;
        cap->deviceRate.store(rate, std::memory_order_release);
        return true;
    };

    while (g_run.load()) {
        // Per second: the FF800 frame rate measured two ways — against the CPU clock (mach) and against
        // the FireWire BUS clock (OHCI cycle timer). The bus-clock rate is the one that matters for
        // playback: our isoch transmit is paced by the bus cycle, so to feed the FF800 at its true rate
        // we must pace the tx ring to (framesRateVsBus) frames per bus second. Recovering it here is the
        // ISOCH_QUERY_CYCLE_TIME / DCL-wrap-timestamp equivalent — the first half of clock-locked TX.
        {
            uint64_t now = mach_absolute_time();
            double el = double(now - rt0) * tb.numer / tb.denom / 1e9;
            if (el >= 1.0) {
                uint64_t ct = cycle_timer_ticks((uint32_t)scalar0(conn, kReadCycleTimer));
                uint64_t dct = ct - ct0;                              // 24.576 MHz bus ticks elapsed
                if (ct < ct0) dct = ct + 128ull * 8000ull * 3072ull - ct0;   // seconds field wrapped
                double busSec = double(dct) / (double)kCycleTimerTicksPerSec;
                double fVsBus = busSec > 0 ? (deviceFrames - rf0) / busSec : 0;
                // Servo target = the device's true rate. Reject the startup-ramp transient (~51300 in
                // second 1) with a TIGHT band — a single bad sample fed to the servo overshoots the
                // FIFO the other way — then low-pass, because the per-second frame count is noisy at
                // ~+/-8 frames (quantisation) while the signal we track is only ~2 frames/s.
                if (fVsBus > baseRate - 60 && fVsBus < baseRate + 60) {
                    measuredRate = servoReady ? 0.8 * measuredRate + 0.2 * fVsBus : fVsBus;
                    servoReady = true;
                }
                // pumpLate = ticks whose work overran the 1 ms period, so the deadline had to be
                // resynced instead of chasing a backlog. Non-zero means the pump is not keeping up.
                if (!quiet) std::printf("  REAL: %.0f frames/s (mach)  %.1f frames/bus-s  bus8k=%.2f Hz  %.0f pkt/s  ring=%llu over=%u under=%u late=%llu\n",
                            (deviceFrames - rf0) / el, fVsBus, dct / busSec / 3072.0,
                            (consumed - rc0) / el,
                            (unsigned long long)cap->depth(), cap->overruns.load(), cap->underruns.load(),
                            (unsigned long long)pumpLate);
                // Interrupt health. irq/s should sit at 8000/kUFOhciIrqEvery (1000/s at the default);
                // a climbing timeout count means the completion interrupt stopped arriving, which is
                // the wedge signature seen from this side.
                if (irqPump) {
                    struct UFOhciStatus st = {};
                    const bool ok = status_read(devStatus, &st);
                    if (!quiet) std::printf("  IRQ: %.0f irq/s (total %llu)  timeouts=%llu  live=%d\n",
                                ok ? (double)(st.irqCount - irq0) / el : 0.0,
                                (unsigned long long)(ok ? st.irqCount : 0),
                                (unsigned long long)wakeTimeouts, ok ? 1 : 0);
                    irq0 = ok ? st.irqCount : irq0;
                }
                // Transmit pacing health. sent should climb at 8000 pkt/s (one per isoch cycle) and
                // lead should sit at kTxLeadPkts; a non-zero resync count means the fill loop lost
                // the race with the transmit head, which is exactly what tears the playback audio.
                // The playback ring's own counters separate a host/device clock-rate mismatch from
                // a pacing fault: a ~42 ppm mismatch takes ~32 MINUTES to walk the 3840-frame tx
                // ring, so a mismatch large enough to matter on a shorter period shows up here as
                // the playback ring steadily filling (over=) or draining (dry=).
                // The playback ring's MINIMUM depth over the second is what sizes the safety offset.
                // `under=` only tells you the cushion was too small (it already glitched); the
                // minimum tells you how much margin was actually left, so one run gives the number
                // for UF_SAFETY_OFFSET_MS directly. Depth is in slots; each slot is
                // kFramesPerSlot frames, so a minimum of N slots means roughly N x (512/rate) of
                // margin — trim the offset toward zero margin, never past it.
                if (!quiet) std::printf("  ANCHOR: worst gap %.1f ms this second (nominal %.1f ms)  "
                            "HAL clock=%s seed=%llu unlocks=%u\n",
                            anchorGapMaxMs, (double)uf::shm::kAnchorFrames * 1000.0 / rate,
                            cap->halLocked.load(std::memory_order_relaxed) ? "LOCKED" : "FREE-RUN",
                            (unsigned long long)cap->halSeed.load(std::memory_order_relaxed),
                            cap->halUnlocks.load(std::memory_order_relaxed));
                anchorGapMaxMs = 0.0;
                if (pubState) uf::state::publish(pubState, [&](uf::state::Shared* o) {
                    o->deviceRate  = cap->deviceRate.load(std::memory_order_relaxed);
                    o->clockSource = cap->clockSource.load(std::memory_order_relaxed);
                    o->clockLocked = cap->clockLocked.load(std::memory_order_relaxed);
                    o->dbq         = dbq;
                    o->settings    = g_settings;
                    // Also every second, not only on change: a UI started after the last edit would
                    // otherwise attach to a matrix of zeros and show an empty mixer.
                    publish_mixer(o);
                });
                if (!quiet) std::printf("  MARGIN: play ring min=%llu slots = %.1f ms spare (under=%u)\n",
                            (unsigned long long)playMinDepth,
                            (double)playMinDepth * uf::shm::kFramesPerSlot * 1000.0 / rate,
                            play->underruns.load());
                playMinDepth = ~0ull;
                if (!txFreeRun && !getenv("UF_NO_TX"))
                    if (!quiet) std::printf("  TX: %.0f pkt/s  lead=%lld pkt  resync=%llu dry=%llu held=%llu (%.2f ms/s)  "
                                "play ring=%llu over=%u under=%u  servo=%d rate=%.1f acc=%+.2f\n",
                                (txLastSent - tx0) / el, (long long)(txFillPkt - txLastSent),
                                (unsigned long long)txResyncs, (unsigned long long)txDry,
                                (unsigned long long)txHeld,
                                (txHeld - txHeld0) * 1000.0 / rate / el,
                                (unsigned long long)play->depth(), play->overruns.load(),
                                play->underruns.load(), servoOn ? 1 : 0, measuredRate, servoAccum);
                // Live device status — the FF800's own view of its clock. We are hunting the "host
                // locked" (green LED) state, so dump raw SR0/SR1 (undecoded bits included) plus the
                // decoded master/sync/rate, and watch it across the ~20 s settling transient.
                uint32_t sr0v = 0, sr1v = 0;
                rq(conn, uf::reg::kStatus0, &sr0v);
                rq(conn, uf::reg::kClockConfig, &sr1v);
                uf::UFStatus st = uf::decode_status(sr0v, sr1v);
                // Publish what the device says is driving it, which is not necessarily what was
                // asked for: an external clock can disappear and leave the FF800 back on its own
                // crystal. CoreAudio should show the truth — that is the whole point of looking at
                // the clock-source menu when something sounds wrong.
                {
                    const auto active = uf::ctl::clock_source_from_status(st.clock_master, st.sync_source);
                    cap->clockSource.store((uint32_t)active, std::memory_order_release);
                    const bool locked =
                        st.clock_master ||
                        (active == uf::ctl::ClockSource::Adat1     && st.adat1_lock && st.adat1_sync) ||
                        (active == uf::ctl::ClockSource::Adat2     && st.adat2_lock && st.adat2_sync) ||
                        (active == uf::ctl::ClockSource::Spdif     && st.spdif_lock && st.spdif_sync) ||
                        (active == uf::ctl::ClockSource::WordClock && st.wclk_lock  && st.wclk_sync) ||
                        (active == uf::ctl::ClockSource::Tco       && st.tco_lock   && st.tco_sync);
                    cap->clockLocked.store(locked ? 1u : 0u, std::memory_order_release);
                }
                double upSec = double(now - toneStart) * tb.numer / tb.denom / 1e9;
                if (!quiet) std::printf("  STATUS @%5.1fs: SR0=%08x SR1=%08x  bit31=%d bit11=%d  master=%d\n",
                            upSec, sr0v, sr1v, (sr0v >> 31) & 1, (sr0v >> 11) & 1, st.clock_master);
                // DIAGNOSTIC ONLY — off by default. This makes the dext os_log one line per inbound
                // packet, and inbound is not rare: a MIDI source sending Active Sensing produces
                // 11-26 packets a second indefinitely. Left on, it writes to the unified log (and
                // so to disk) forever, for output nobody is reading. UF_DEBUG_INBOUND=1 to enable.
                if (debugInbound)
                    IOConnectCallScalarMethod(conn, kDebugInbound, nullptr, 0, nullptr, nullptr);
                if (probeOn && probePeriodic) wq(conn, probeAddr, probeVal);   // re-assert candidate each second
                if (reclaimAt >= 0 && !reclaimed && upSec >= reclaimAt) {       // LED1: re-claim while locked
                    reclaimed = true;
                    wq(conn, uf::reg::kHostLed, 0);
                    const uint32_t init[3] = {rate, (dbq << 11) | rx_channel, ses.dbqFlag};
                    wblock(conn, uf::reg::kInitBankStream, init, 3);
                    wq(conn, uf::reg::kInitBankStart, 0x80000000u | ses.dbqFlag);
                    std::printf("uf-daemon: [LED1] re-asserted full claim at %.1fs (streams locked) — WATCH THE LED\n", upSec);
                }
                std::fflush(stdout);
                rt0 = now; rf0 = deviceFrames; rc0 = consumed; ct0 = ct; tx0 = txLastSent;
                txHeld0 = txHeld;
            }
        }
        // A SIGUSR1 since the last tick starts a fresh capture, whether or not one already ran —
        // so several episodes can be recorded in one daemon lifetime.
        if (g_armCapture.exchange(false) && capWavPath) {
            capWav.clear();
            capWav.reserve((size_t)capWavLimit * dbq);
            capWavFrames = 0;
            capWavDone = false;
            std::printf("uf-daemon: capture STARTED (%.1f s to %s)\n", capWavSecs, capWavPath);
            std::fflush(stdout);
        }

        // Apply control commands, COALESCED. Every mixer write is a FireWire register write, and the
        // dext runs one outstanding AT transaction at a time, poll-based, shared with our own
        // register I/O — so ~1000 automation updates/s would saturate it. Fold the queue into the
        // shadow first (last value per cell wins), then push at most kCtlWritesPerTick cells to the
        // device. At 1 kHz that is still far more than the ~30-50 Hz past which a fader ride is
        // indistinguishable, and a burst of automation costs a bounded number of writes.
        {
            uf::CtlCommand c;
            while (ctlQueue.pop(&c)) {
                // Every path updates the MODEL, then renders the cells it touched. Rendering in
                // one place is what keeps "set one cell" and "re-apply everything" from disagreeing
                // about how a mute or a phase flip turns into a quadlet.
                const auto kind = (uf::MixerSrcKind)c.srcKind;
                auto dirty = [&](uf::MixerModel::Write w) {
                    g_mixer.set_at(w.addr, w.value);
                    ctlDirty[w.addr] = w.value;      // last write to a cell wins — the coalescing
                };
                switch (c.type) {
                    case uf::ctl::MsgType::SetMixer:
                        g_model.cell(kind, c.src, c.dest).gain = c.coeff;
                        dirty(g_model.render_cell(kind, c.src, c.dest));
                        continue;
                    case uf::ctl::MsgType::SetFader:
                        g_model.output(c.out).gain = c.coeff;
                        dirty(g_model.render_output(c.out));
                        continue;
                    case uf::ctl::MsgType::SetCellFlags: {
                        auto& f = g_model.cell(kind, c.src, c.dest).flags;
                        f = (uint8_t)((f & ~c.mask) | (c.flags & c.mask));
                        dirty(g_model.render_cell(kind, c.src, c.dest));
                        continue;
                    }
                    case uf::ctl::MsgType::SetOutFlags: {
                        auto& f = g_model.output(c.out).flags;
                        const bool wasRec = (f & uf::kMfRec) != 0;
                        f = (uint8_t)((f & ~c.mask) | (c.flags & c.mask));
                        dirty(g_model.render_output(c.out));
                        // Loopback lives in a different register from the matrix — the 28-quadlet
                        // output-record mask — and it is written as a block, so a change costs one
                        // extra write, and only when the flag actually moved.
                        if (wasRec != ((f & uf::kMfRec) != 0)) {
                            const auto m = g_model.rec_mask();
                            std::vector<uint32_t> rec(dbq, 0);
                            for (uint32_t i = 0; i < dbq && i < m.size(); ++i) rec[i] = m[i];
                            wblock(conn, uf::reg::kOutputRecMask, rec.data(), dbq);
                        }
                        continue;
                    }
                    case uf::ctl::MsgType::SetStereo:
                        // Host-side only: nothing to write to the device, but it is published and
                        // persisted so every front-end agrees and it survives a restart.
                        g_model.set_stereo(kind, c.src, c.on != 0);
                        stateDirty = true;
                        continue;
                    case uf::ctl::MsgType::Submix: {
                        if (c.on) g_model.clear_submix(c.dest);
                        else      g_model.copy_submix(c.src, c.dest);
                        // A column is 28 inputs + 28 playbacks. They go through the same coalescing
                        // and rate limit as everything else, so a submix copy lands over a few ms
                        // rather than blocking the pump for 56 register writes in one tick.
                        for (uint32_t sc = 0; sc < uf::MixerModel::kCh; ++sc) {
                            dirty(g_model.render_cell(uf::MixerSrcKind::Input, sc, c.dest));
                            dirty(g_model.render_cell(uf::MixerSrcKind::Playback, sc, c.dest));
                        }
                        continue;
                    }
                    case uf::ctl::MsgType::SetSettings: {
                        // Adopt the shadow, then write it. Adopting is what makes it stick: the next
                        // session_start re-assembles the conf block from g_settings, so a setting
                        // written straight to the register (as uf-set used to) was silently undone
                        // by the next rate change — the same bug the mixer had.
                        g_settings = c.settings;
                        uf::ConfBlock cb = uf::assemble_conf_block(g_settings);
                        const uint32_t conf[3] = {cb.cr0, cb.cr1, cb.cr2};
                        wblock(conn, uf::reg::kConfBlock, conf, 3);
                        wq(conn, uf::reg::kClockConfig,
                           uf::assemble_clock_config(g_settings.clock_master, g_settings.sync_ref,
                                                     c.rate ? c.rate : rate));
                        if (pubState) uf::state::publish(pubState, [&](uf::state::Shared* o) {
                            o->settings = g_settings;
                        });
                        continue;                        // not a mixer cell: nothing to coalesce
                    }
                    default: continue;
                }
            }
            uint32_t written = 0;
            for (auto it = ctlDirty.begin(); it != ctlDirty.end() && written < kCtlWritesPerTick; ) {
                wq(conn, it->first, it->second);
                it = ctlDirty.erase(it);
                ++written;
            }
            if (written || stateDirty) {
                stateDirty = false;
                // Hand the saver thread a consistent copy. A struct copy, no syscall — the file
                // write itself happens off this thread.
                g_stateSnap = g_model.to_blob();
                g_stateSnapSeq.fetch_add(1, std::memory_order_release);
                // Republish so a UI sees its own change reflected rather than waiting a second.
                if (pubState) uf::state::publish(pubState, publish_mixer);
            }
        }

        // Meters, ~30 Hz. Taking the accumulators also resets them, so the RMS window is exactly the
        // interval between publishes and there is no separate window to fall out of step.
        {
            const uint64_t nowNs = mach_absolute_time() * tb.numer / tb.denom;
            if (nowNs >= meterNextNs) {
                meterNextNs = nowNs + kMeterPeriodNs;
                uf::MeterValue inv[uf::shm::kMaxChannels], pbv[uf::shm::kMaxChannels];
                const uint32_t frames = inMeters.frames();
                inMeters.take(inv, uf::shm::kMaxChannels);
                pbMeters.take(pbv, uf::shm::kMaxChannels);

                // The device's own meter region, RAW. Polled at the same cadence but ONLY when it is
                // wanted: it is the one part of metering that costs a FireWire transaction, and its
                // scaling is still unsettled (spec/10 §10.3), so it is not on by default.
                uint32_t devq[64] = {0};
                bool devOk = false;
                if (meterDevice) devOk = rblock(conn, uf::reg::kMeterBase, devq, 64);

                if (pubState) uf::state::publish(pubState, [&](uf::state::Shared* o) {
                    for (uint32_t i = 0; i < uf::state::Shared::kMixerCh; ++i) {
                        o->inputMeters[i] = inv[i];
                        o->playbackMeters[i] = pbv[i];
                    }
                    o->meterFrames = frames;
                    if (devOk) {
                        // Little-endian u64 per channel, as it comes off the wire — see the header.
                        for (uint32_t i = 0; i < 32; ++i)
                            o->deviceMeters[i] = (uint64_t)devq[i * 2] |
                                                 ((uint64_t)devq[i * 2 + 1] << 32);
                        ++o->deviceMeterSeq;
                    }
                });
            }
        }

        // MIDI in. One cheap external method per tick; at 31250 baud the device can produce at most
        // ~3 bytes in a millisecond, well inside the 8 a poll returns.
        if (g_midiSrc) midi_poll_in();

        if (++ticks % 1000 == 0 && !quiet) {
            std::printf("  cap: %llu pkt (+%llu/s)  frames=%llu (+%llu/s)  ring=%llu over=%u under=%u\n",
                        (unsigned long long)consumed, (unsigned long long)(consumed - lastConsumed),
                        (unsigned long long)deviceFrames, (unsigned long long)(deviceFrames - lastFrames),
                        (unsigned long long)cap->depth(),
                        cap->overruns.load(), cap->underruns.load());
            std::printf("       lengths: full=%llu empty=%llu other=%llu  dup-timestamps=%llu\n",
                        (unsigned long long)nFull, (unsigned long long)nEmpty, (unsigned long long)nOther,
                        (unsigned long long)nDupTs);
            // Cycle continuity. gaps=0 is the healthy state: every packet lands exactly one
            // isochronous cycle after the last. Anything else is capture with holes in it, and
            // `missed` counts how many cycles' worth of frames the timeline never saw.
            std::printf("       cycles: gaps=%llu missed=%llu restarts=%llu\n",
                        (unsigned long long)cycleGaps, (unsigned long long)missedCycles,
                        (unsigned long long)cycleRestarts);
            std::fflush(stdout);
            lastConsumed = consumed; lastFrames = deviceFrames;
        }
        // The tick's single dext round trip. Release/refill carry last tick's cursors; the returned
        // positions drive this tick's capture drain and playback fill.
        // Where the controller is, and where we are. On the interrupt path both directions travel
        // through the shared page — the dext's cursors come from the snapshot the interrupt handler
        // published, and ours go back in the control block for the next interrupt to apply — so the
        // steady-state tick makes NO external method call at all. The polled fallback still does its
        // one round trip, and so does the interrupt path until the first interrupt has landed.
        struct UFOhciStatus snap = {};
        const bool haveSnap = irqPump && status_read(devStatus, &snap);
        uint64_t total = consumed, sent = txLastSent;
        if (haveSnap) {
            total = snap.rxCompleted;
            sent  = snap.txSent;
            devControl->releaseUpTo = consumed;
            devControl->txFillUpTo  = txPaced ? txFillPkt : 0;
        } else {
            pump_dext(conn, consumed, txPaced ? txFillPkt : 0, &total, &sent);
        }

        // Has CoreAudio asked for a different clock source? Unlike a rate change this does not
        // restart the session — the FF800 keeps streaming while it re-locks — so it is just the two
        // register surfaces that carry the clock (the CR conf block and the clock-config register,
        // whose overlap is a documented [?]; uf::UFControl emits both from one shadow).
        {
            const uint64_t seq = cap->controlSeq.load(std::memory_order_acquire);
            if (seq != seenControl) {
                seenControl = seq;
                const uint32_t want = cap->requestedClockSource.load(std::memory_order_acquire);
                if (uf::ctl::is_valid_clock_source(want)) {
                    const auto src = (uf::ctl::ClockSource)want;
                    g_settings.clock_master = uf::ctl::clock_source_is_master(src);
                    g_settings.sync_ref     = uf::ctl::clock_source_sync_ref(src);
                    uf::UFControl ctlShadow;
                    ctlShadow.set_sample_rate(rate);
                    ctlShadow.set_settings(g_settings);
                    for (const auto& w : {ctlShadow.conf_block_write(), ctlShadow.clock_config_write()}) {
                        if (w.kind == uf::RegWrite::Kind::Block)
                            wblock(conn, w.addr, w.quads.data(), (uint32_t)w.quads.size());
                        else
                            wq(conn, w.addr, w.quad);
                    }
                    std::printf("uf-daemon: clock source -> %s\n", uf::ctl::clock_source_name(src));
                    std::fflush(stdout);
                }
            }
        }

        // Has CoreAudio asked for a different rate? Checked before the drain so we do not decode a
        // tick's worth of packets at a geometry we are about to abandon.
        {
            const uint64_t req = cap->requestSeq.load(std::memory_order_acquire);
            if (req != seenRequest) {
                seenRequest = req;
                const uint32_t want = cap->requestedRate.load(std::memory_order_acquire);
                if (want && want != rate && uf::is_supported_rate(want)) {
                    // Same rule as the wedge path: a rate change that lands during sleep must
                    // be retried, not treated as the end of the daemon.
                    if (!restart_session(want, "rate change"))
                        lastProgress = mach_absolute_time();
                    continue;
                }
            }
        }

        // The snapshot carries the one thing a round trip cannot: the host time at which the
        // controller actually completed that packet, taken inside the interrupt handler. Pairing the
        // frame count with THAT instead of with "whenever the pump got round to asking" is what
        // takes our scheduling jitter out of the clock CoreAudio reads.
        uint64_t framesAtSnap = 0;
        bool haveAnchor = false;

        while (consumed < total) {
            const uint8_t* slot = dextBuf + (consumed % (dextSize / kDextSlotBytes)) * kDextSlotBytes;
            const uint32_t* q = reinterpret_cast<const uint32_t*>(slot);
            const uint32_t len = q[1] >> 16;               // iso header data_length (2-quad prefix)
            if (len == 0) ++nEmpty; else if (len == fullPayload) ++nFull; else ++nOther;
            if (q[0] == lastTs) ++nDupTs; lastTs = q[0];   // duplicate iso timestamp = stale re-read

            // Cycle-time drift detection — the factory driver makes the same check in its DMA
            // callback and actions it from a watchdog as a full stream restart.
            //
            // q[0] is the packet's arrival timestamp, low 16 bits: cycleCount[12:0] plus the low 3
            // bits of cycleSeconds (ohci.c copy_iso_headers reads exactly these). The FF800
            // transmits in every isochronous cycle, empty packets included, so consecutive packets
            // must be exactly one cycle apart. Anything else means the controller missed cycles —
            // capture has a hole in it, and every frame count derived from it after that point is
            // wrong by the gap. That is a clock discontinuity, not a dropout to paper over.
            {
                const uint32_t ts  = q[0] & 0xffff;
                const uint32_t cyc = (ts & 0x1fff) + ((ts >> 13) & 0x7) * 8000u;   // linear, 8 s span
                if (haveCycle) {
                    int32_t d = (int32_t)cyc - (int32_t)lastCycle;
                    if (d < 0) d += 8 * 8000;             // the 3-bit seconds field wrapped
                    if (d != 1) { ++cycleGaps; missedCycles += (uint64_t)(d - 1); }
                    if (d > (int32_t)kCycleGapRestart) worstGap = (uint32_t)d;
                }
                lastCycle = cyc; haveCycle = true;
            }
            if (len && len <= kDextSlotBytes - 8) {
                const uint8_t* payload = slot + 8;
                const uint32_t nframes = uf::frames_in_payload(len, dbq);
                const uint32_t ch = dbq < uf::shm::kMaxChannels ? dbq : uf::shm::kMaxChannels;
                for (uint32_t k = 0; k < nframes; ++k) {
                    // Push into the shm ring for the plugin; a full ring drops the frame — but the
                    // clock (deviceFrames) still counts it below, so the timebase never stalls.
                    if (!capSlot) { capSlot = cap->acquireWrite(); capFill = 0; }
                    if (capSlot || (capWavPath && !capWavDone)) {
                        uf::decode_frame(payload + (size_t)k * dbq * 4, dbq, frame.data());
                        // Top-justify to match the ring (and the playback side), so both meter rows
                        // are on one scale.
                        {
                            int32_t up[uf::shm::kMaxChannels];
                            const uint32_t mc = ch;
                            for (uint32_t c = 0; c < mc; ++c) up[c] = frame[c] << 8;
                            inMeters.add_frame(up, mc);
                        }
                        if (capWavPath && !capWavDone) {
                            capWav.insert(capWav.end(), frame.begin(), frame.begin() + dbq);
                            if (++capWavFrames >= capWavLimit) {
                                auto bytes = uf::write_wav(capWav, rate, (uint16_t)dbq);
                                if (FILE* wf = fopen(capWavPath, "wb")) {
                                    fwrite(bytes.data(), 1, bytes.size(), wf);
                                    fclose(wf);
                                    std::printf("uf-daemon: wrote %s (%llu frames, %u ch)\n",
                                                capWavPath, (unsigned long long)capWavFrames, dbq);
                                    std::fflush(stdout);
                                } else std::perror("uf-daemon: capture wav");
                                capWav.clear(); capWav.shrink_to_fit();
                                capWavDone = true;
                            }
                        }
                    }
                    if (capSlot) {
                        int32_t* dst = capSlot->audio + (size_t)capFill * uf::shm::kMaxChannels;
                        for (uint32_t c = 0; c < ch; ++c)
                            dst[c] = frame[c] << 8;                       // 24->32
                        // The ring frame is always kMaxChannels wide; at 2x/4x the device sends
                        // fewer (20/12) and the rest of the frame is whatever the previous lap left
                        // there — audible on the ADAT channels unless cleared.
                        if (ch < uf::shm::kMaxChannels)
                            std::memset(dst + ch, 0, (uf::shm::kMaxChannels - ch) * sizeof(int32_t));
                        if (++capFill >= uf::shm::kFramesPerSlot) {
                            capSlot->frameCount = capFill; capSlot->channelCount = ch;
                            cap->commitWrite(); capSlot = nullptr;
                        }
                    }
                }
                deviceFrames += nframes;   // TRUE device sample position — advances for every frame
            }
            ++consumed;
            // The frame position the interrupt's timestamp belongs to.
            if (haveSnap && consumed == snap.rxCompleted) { framesAtSnap = deviceFrames; haveAnchor = true; }
        }

        // A cycle discontinuity big enough to matter is handled the same way RME handles it: restart
        // the stream. Recovering the timeline any other way would mean guessing how many frames went
        // missing, and a wrong guess is a permanent offset in the clock CoreAudio is locked to.
        //
        // This SELF-ARMS rather than being a flag someone has to set. The detection rests on an
        // assumption no hardware run has confirmed — that the FF800 transmits in EVERY isochronous
        // cycle, empty packets included, so consecutive arrival timestamps are always one cycle
        // apart — and acting on it when it is false would restart the stream continuously, which is
        // far worse than the fault it detects. But leaving it off by default means it never runs at
        // all, and a flag nobody remembers to set is not a safety mechanism either.
        //
        // So the daemon proves the assumption on the hardware in front of it: stream for
        // kCycleArmSec with no gaps at all and the premise holds here, so arm. See a gap during that
        // window and it does not, so stay disarmed and say so. And because "no gaps in ten seconds"
        // is evidence rather than proof, restarts that come too fast disarm it again — a
        // false positive costs a few restarts, not an endless loop.
        {
            const double upSec = double(mach_absolute_time() - toneStart) * tb.numer / tb.denom / 1e9;
            if (!cycleArmed && !cycleDisarmed && upSec >= kCycleArmSec) {
                if (cycleGaps == 0) {
                    cycleArmed = true;
                    std::printf("uf-daemon: cycle-continuity restart ARMED (%.0fs, no gaps)\n", upSec);
                } else {
                    cycleDisarmed = true;
                    std::printf("uf-daemon: cycle-continuity restart NOT armed — %llu gaps in the "
                                "first %.0fs, so this device does not transmit every cycle\n",
                                (unsigned long long)cycleGaps, upSec);
                }
                std::fflush(stdout);
            }
        }
        if (worstGap && cycleArmed && !cycleDisarmed) {
            const uint32_t gap = worstGap;
            worstGap = 0;
            const uint64_t now = mach_absolute_time();
            const double sinceLast = double(now - lastCycleRestart) * tb.numer / tb.denom / 1e9;
            lastCycleRestart = now;
            if (sinceLast < kCycleRestartFloorSec && ++cycleRestartBurst >= 3) {
                cycleDisarmed = true;
                std::printf("uf-daemon: cycle-continuity restart DISARMED — 3 restarts inside %.0fs, "
                            "treating the gaps as normal for this device\n", kCycleRestartFloorSec);
                std::fflush(stdout);
            } else {
                if (sinceLast >= kCycleRestartFloorSec) cycleRestartBurst = 0;
                ++cycleRestarts;
                std::printf("uf-daemon: cycle discontinuity — %u cycles missed at once\n", gap - 1);
                haveCycle = false;
                if (!restart_session(rate, "cycle-time discontinuity"))
                    lastProgress = mach_absolute_time();   // retry on the wedge schedule, do not exit
                continue;
            }
        }
        worstGap = 0;   // not armed (or just disarmed): keep counting, do not act

        // Wedge detection. Twice under CoreAudio load the FF800 has dropped off the bus mid-stream
        // (SR0=SR1=0, capture frozen) and the daemon just spun on the dead connection until it was
        // killed by hand. Capture silence is the signal: the device transmits continuously whenever
        // it is alive, so packets not advancing for kWedgeMs means the stream is gone, not idle.
        // Recovery is the same stop/start a rate change does — the device itself comes back via the
        // bus reset that the re-claim triggers.
        if (consumed != lastConsumedForWedge) {
            lastConsumedForWedge = consumed;
            lastProgress = mach_absolute_time();
            wedgeRestarts = 0;                    // real packets: whatever we did last time worked
        } else if (!getenv("UF_NO_WEDGE_RECOVERY")) {
            const double stalledMs =
                double(mach_absolute_time() - lastProgress) * tb.numer / tb.denom / 1e6;
            // Back off after repeated failures rather than hammering a device that is simply gone
            // (unplugged, powered off) — a restart storm on the bus helps nobody.
            const double limit = wedgeRestarts >= 3 ? 5000.0 : kWedgeMs;
            if (stalledMs > limit) {
                ++wedgeRestarts;
                // A failed rebuild is "not yet", never "give up". The commonest cause by far is
                // system sleep: the dext gates all MMIO while the PCIe bridge is powered down, so
                // session_start CANNOT succeed until the machine is awake again. Treating that as
                // fatal is what made the daemon exit on every lid-close, leaving no audio after
                // wake even though the controller had rebuilt itself perfectly.
                //
                // Resetting lastProgress restarts the backoff clock, so we retry on the same
                // schedule as any other wedge (5 s once it has failed a few times) instead of
                // spinning on a device that is simply not there.
                if (!restart_session(rate, "capture stalled — device wedged"))
                    lastProgress = mach_absolute_time();
                continue;
            }
        }

        // Publish the device-paced clock anchor, once per ZeroTimeStampPeriod (kAnchorFrames).
        // CoreAudio locks its timeline to this, so it pulls at the real device rate.
        //
        // The anchor must be the (frame, host time) correspondence at the START of a period: both
        // libASPL and the HAL index zero timestamps as periodCounter * ZeroTimeStampPeriod. Two
        // errors creep into the raw cursor. The frame count arrives in 6-frame packet bursts, so it
        // is essentially never a multiple of 512; and it is read when the PUMP woke, up to a tick
        // after the frames actually landed, so the host time carries the pump's scheduling jitter.
        // Both feed straight into the HAL's rate estimate — which is what has to be right for 44.1 k
        // material to resample cleanly. So: quantise to the boundary just reached, and back-date the
        // host time by however many frames we have already drained past it.
        //
        // The (frames, host time) pair comes from the interrupt when we have one, and from this
        // thread's own clock only as a fallback.
        const uint64_t anchorFrames = haveAnchor ? framesAtSnap : deviceFrames;
        const uint64_t anchorHost   = haveAnchor ? snap.hostTime : mach_absolute_time();
        if (anchorFrames >= lastPublish + uf::shm::kAnchorFrames) {
            lastPublish = anchorFrames / uf::shm::kAnchorFrames * uf::shm::kAnchorFrames;
            const double past = (double)(anchorFrames - lastPublish);
            cap->publishTimestamp((double)lastPublish,
                                  anchorHost - (uint64_t)(past * hostTicksPerFrame));
            // Longest interval between anchors this second. The plugin's clock unlocks when an
            // anchor falls further behind than its staleness window, so a spike here is the daemon
            // side of a re-lock — which is what we are trying to line up against audible gating.
            const uint64_t nowT = mach_absolute_time();
            if (lastAnchorAt) {
                const double gapMs = double(nowT - lastAnchorAt) * tb.numer / tb.denom / 1e6;
                if (gapMs > anchorGapMaxMs) anchorGapMaxMs = gapMs;
            }
            lastAnchorAt = nowT;
        }

        // Playback: encode the plugin's PCM into the transmit ring's FULL slots (blocking mode). Each
        // full slot takes syt_interval frames; empty slots (the 4th of every group) are skipped since
        // the dext transmits them header-only. The plugin's frames come as fixed-size shm slots, so
        // we thread a frame cursor through them.
        if (wavFrames) {
            // Stream the loaded WAV (stereo) to playback ch 0/1 -> outputs 1/2, looping. The tx ring is
            // a static 3840-frame loop the controller reads at 48000/s; we keep it filled with the
            // real-time window file[base .. base+ring] (base = elapsed*48000), so playback tracks wall
            // clock. Free-running (not isoch-paced) so minor artifacts are expected — a hearing test.
            while (play->acquireRead()) play->commitRead();
            double sec = double(mach_absolute_time() - toneStart) * tb.numer / tb.denom / 1e9;
            uint64_t base = (uint64_t)(sec * 48000.0);
            for (uint32_t slot = 0; slot < kTxSlots; ++slot)
                for (uint32_t f = 0; f < sytInterval; ++f) {
                    uint64_t sf = (base + slot * sytInterval + f) % wavFrames;
                    uint8_t* fr = txBuf + (size_t)slot * kTxSlotBytes + (size_t)f * dbq * 4;
                    for (uint32_t c = 0; c < dbq; ++c) {
                        int32_t s = (c == 0) ? wavSamps[sf * 2] : (c == 1) ? wavSamps[sf * 2 + 1] : 0;
                        uf::encode_sample_le(s >> 8, fr + (size_t)c * 4);
                    }
                }
        }
        else if (getenv("UF_TX_TONE")) {
            while (play->acquireRead()) play->commitRead();   // ignore CoreAudio; generate our own
            int wantCh = getenv("UF_TX_CH") ? atoi(getenv("UF_TX_CH")) : 0;
            double freq = getenv("UF_TX_FREQ") ? atof(getenv("UF_TX_FREQ")) : 437.0;
            if (toneCh != wantCh) {
                toneCh = wantCh;
                fill_tone(txBuf, kTxSlots, kTxSlotBytes, sytInterval, dbq, wantCh, freq);
                std::printf("uf-daemon: TONE %.0f Hz on playback ch %d (output %d)\n", freq, wantCh, wantCh + 1);
                std::fflush(stdout);
            }
        }
        else if (getenv("UF_TX_SILENT")) { while (play->acquireRead()) play->commitRead(); }
        else if (txFreeRun)
        while (const uf::shm::Slot* fps = play->acquireRead()) {
            const uint32_t pch = fps->channelCount ? fps->channelCount : dbq;
            for (uint32_t f = 0; f < fps->frameCount; ++f) {
                // Skip empty tx slots (same cadence the dext programmed) so audio only lands in packets
                // that are actually transmitted.
                while (!uf::is_full_packet(txCursor % kTxSlots, fullCount)) ++txCursor;
                if (txFill == 0) txSlotPtr = txBuf + (size_t)(txCursor % kTxSlots) * kTxSlotBytes;
                uint8_t* fr = txSlotPtr + (size_t)txFill * dbq * 4;
                for (uint32_t c = 0; c < dbq; ++c) {
                    int32_t s = (c < pch) ? fps->audio[(size_t)f * pch + c] : 0;
                    uf::encode_sample_le(s >> 8, fr + (size_t)c * 4);   // ring int32 -> 24-bit sample
                }
                if (++txFill >= sytInterval) { txFill = 0; ++txCursor; }
            }
            play->commitRead();
        }
        else {
            // Paced fill: keep exactly kTxLeadPkts packets of the ring written ahead of the
            // controller's transmit head. The old free-running cursor above advanced at the pump's
            // wake-up rate with nothing tying it to the hardware, so the write and read positions
            // slid through each other and the audio tore every few tens of seconds.
            txLastSent = sent;
            if (txFillPkt < sent + kTxMinLeadPkts || txFillPkt - sent > kTxSlots) {
                txFillPkt = sent + kTxLeadPkts;   // stalled long enough to lose the race
                ++txResyncs;
            }
            const uint64_t target = sent + kTxLeadPkts;
            txSrc.beginTick();
            for (; txFillPkt < target; ++txFillPkt) {
                const uint32_t slot = (uint32_t)(txFillPkt % kTxSlots);
                if (!uf::is_full_packet(slot, fullCount, kTxSlots)) continue;  // empty packet, no payload
                txSrc.beginPacket();   // re-arms the acquire attempt: scoped to a PACKET, not a tick

                // Servo: this packet carries sytInterval frames, plus one whenever the accumulated
                // fractional debt against the measured device rate reaches a frame (or minus one if we
                // are running ahead). At 48000 base vs ~48002 measured that is ~one extra frame every
                // ~0.5 s — enough to keep the device FIFO from draining without ever overflowing it.
                uint32_t frames = sytInterval;
                if (servoOn && servoReady) {
                    servoAccum += (measuredRate - baseRate) / 8000.0;
                    if (servoAccum > 4.0) servoAccum = 4.0;      // a bad estimate can't run away
                    else if (servoAccum < -4.0) servoAccum = -4.0;
                    if (servoAccum >= 1.0)      { ++frames; servoAccum -= 1.0; }
                    else if (servoAccum <= -1.0){ --frames; servoAccum += 1.0; }
                }
                const uint32_t wantBytes = frames * dbq * 4;
                if (slotBytes[slot] != wantBytes) {   // reprogram this slot's descriptor once
                    uint64_t a[2] = {slot, wantBytes};
                    IOConnectCallScalarMethod(conn, kIsoTxSetBytes, a, 2, nullptr, nullptr);
                    slotBytes[slot] = wantBytes;
                }

                uint8_t* base = txBuf + (size_t)slot * kTxSlotBytes;
                for (uint32_t f = 0; f < frames; ++f) {
                    // The tone path synthesises straight into the packet so the servo can be tested
                    // from the daemon alone; the normal path takes its frame from uf::TxFill, which
                    // is where the ring-miss policy lives (and where it is unit-tested).
                    int32_t tone[uf::shm::kMaxChannels] = {0};
                    const int32_t* frameSrc;
                    if (pacedToneCh >= 0) {
                        // Top-justified like a shm-ring sample (the encode below applies >>8): 2^30 = -6 dBFS.
                        if (pacedToneCh < (int)dbq)
                            tone[pacedToneCh] = (int32_t)(1073741824.0 * std::sin(pacedTonePhase));
                        if (vibHz > 0) {
                            pacedTonePhase += pacedTonePhaseInc * (1.0 + vibDepth * std::sin(vibPhase));
                            vibPhase += vibPhaseInc;
                            if (vibPhase > 2.0 * M_PI) vibPhase -= 2.0 * M_PI;
                        } else {
                            pacedTonePhase += pacedTonePhaseInc;
                        }
                        if (pacedTonePhase > 2.0 * M_PI) pacedTonePhase -= 2.0 * M_PI;
                        frameSrc = tone;
                    } else {
                        frameSrc = txSrc.frame(*play);
                        pbMeters.add_frame(frameSrc, txSrc.channels());
                    }
                    uint8_t* fr = base + (size_t)f * dbq * 4;
                    for (uint32_t c = 0; c < dbq; ++c)
                        uf::encode_sample_le(frameSrc[c] >> 8, fr + (size_t)c * 4);  // int32 -> 24-bit
                }
            }
            if (txSrc.dryTick()) ++txDry;   // refill is handed back on the next tick's pump_dext
            txHeld = txSrc.held();
        }

        // Sample how much playback the plugin is keeping ahead of us. The minimum over a
        // second is the real headroom the safety offset is buying (see MARGIN above).
        { const uint64_t d = play->depth(); if (d < playMinDepth) playMinDepth = d; }

        // Wait for the next tick. Blocking on the isochronous completion interrupt means the pump
        // wakes because the hardware moved, not because a timer said so: no phase drift against the
        // DMA, and no window where the controller has finished a packet but nobody is awake to
        // notice. The timeout is a safety net, not the pacing — at kUFOhciIrqEvery=8 an interrupt is
        // due every millisecond, so hitting it means the stream has stopped (a wedge), and we fall
        // through to run the tick anyway so the wedge shows up in the per-second stats.
        //
        // Without the interrupt we sleep to an ABSOLUTE deadline rather than for a relative
        // interval: usleep(1000) made each tick take work+1 ms, so the pump ran slower than 1 kHz by
        // however long its work took and the period wandered with load.
        if (irqPump) {
            if (!wake_wait(wakePort, 20)) ++wakeTimeouts;
        } else {
            deadline += kPumpPeriodTicks;
            const uint64_t nowTicks = mach_absolute_time();
            if (deadline < nowTicks) {      // overran the period; resync rather than chase a backlog
                deadline = nowTicks + kPumpPeriodTicks;
                ++pumpLate;
            }
            mach_wait_until(deadline);
        }
    }

    std::printf("uf-daemon: stopping (%llu session restarts)\n", (unsigned long long)restarts);
    ctlServer.stop();   // joins the listener thread and unlinks the socket, so a restart can bind
    // The final save and the saver join happen in ~SaverGuard, so they cover the early returns too.
    if (midiClient) {
        IOConnectCallScalarMethod(conn, kUFOhciMidiInDisable, nullptr, 0, nullptr, nullptr);
        g_midiSrc = 0;
        MIDIClientDispose(midiClient);   // disposes the endpoints it owns
    }
    cap->deviceRate.store(0, std::memory_order_release);
    session_stop(conn, ses);
    shm_unlink(uf::state::kName);
    shm_unlink(uf::shm::kCaptureName);
    shm_unlink(uf::shm::kPlaybackName);
    IOServiceClose(conn);
    return 0;
}
