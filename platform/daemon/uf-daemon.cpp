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
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <atomic>
#include <mach/mach_time.h>
#include <thread>

#include "uf/protocol/channels.hpp"
#include "uf/protocol/codec.hpp"
#include "uf/protocol/registers.hpp"
#include "uf/protocol/stream.hpp"
#include "uf/protocol/rate.hpp"
#include "uf/protocol/settings.hpp"
#include "../shared/uf_shm_ring.hpp"

// Must match UFFireWireOHCIShared.h.
enum { kRQ = 0, kWQ = 1, kWB = 2, kIsoStart = 3, kIsoPoll = 4, kIsoStop = 5,
       kIsoTxStart = 6, kIsoTxStop = 7, kIsoStartCont = 8, kIsoCompleted = 9, kIsoRelease = 10,
       kReadCycleTimer = 14 };

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

static std::atomic<bool> g_run{true};
static void on_signal(int) { g_run.store(false); }

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

static uint64_t scalar0(io_connect_t c, uint32_t sel) {
    uint64_t out = 0; uint32_t n = 1;
    IOConnectCallScalarMethod(c, sel, nullptr, 0, &out, &n);
    return out;
}

// ── a shared-memory ring, created + owned by the daemon ─────────────────────────────────────────
static uf::shm::Ring* make_ring(const char* name) {
    shm_unlink(name);
    int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
    if (fd < 0) { std::perror("shm_open"); return nullptr; }
    if (ftruncate(fd, sizeof(uf::shm::Ring)) != 0) { std::perror("ftruncate"); close(fd); return nullptr; }
    void* p = mmap(nullptr, sizeof(uf::shm::Ring), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { std::perror("mmap"); return nullptr; }
    auto* r = static_cast<uf::shm::Ring*>(p);
    r->init();
    return r;
}

int main() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    io_connect_t conn = open_dext();
    if (!conn) { std::fprintf(stderr, "uf-daemon: dext not available\n"); return 1; }

    // Session parameters. 48 kHz, 28 channels — the plugin declares the same.
    const uint32_t rate = 48000;
    const uf::Speed speed = uf::speed_for_rate(rate);
    const uint32_t dbq = uf::data_block_quadlets(speed);
    const uint32_t rx_channel = 1;
    const uint32_t dbqFlag = dbq | 0x800u;   // S800 double-speed flag rides with dbq

    // [0] CLAIM THE DEVICE FIRST. The host-LED register is written before the rate and before any
    // stream op, unconditionally, on attach and every bus reset. An unclaimed FF800 runs
    // in standalone mode driving its own output source — which matches our symptom exactly (a fixed
    // tone on every output, independent of what we transmit) and the HOST LED staying red. We used to
    // write this LAST, after fetch-enable.
    wq(conn, uf::reg::kHostLed, 0);
    usleep(20 * 1000);

    // Lock the device to INTERNAL MASTER clock at `rate` first. Without this it sits in autosync with
    // nothing locked and free-runs (observed: 64 kHz / 8000 full packets/s), so the stream rate is
    // wrong. The default SettingsShadow is internal-master; the conf block carries the clock mode.
    { uf::ConfBlock cb = uf::assemble_conf_block(uf::SettingsShadow{});
      const uint32_t conf[3] = {cb.cr0, cb.cr1, cb.cr2};
      wblock(conn, uf::reg::kConfBlock, conf, 3); }
    usleep(20 * 1000);

    // 0x801c0000 = FORMER_REG_FETCH_PCM_FRAMES (snd-fireface). 0 per channel = FETCH (play) that
    // channel, 1 = don't fetch. Critically, snd-fireface enables fetch (writes 0) only AFTER the iso
    // streams are up and running (snd_ff_stream_start_duplex). Enabling it before the stream exists
    // makes the device fetch garbage → the beep. So DISABLE fetch (write 1s) during setup here, and
    // enable it last (step [5], after iso start + settle).
    { std::vector<uint32_t> ones(dbq, 1);
      wblock(conn, uf::reg::kStatus0, ones.data(), dbq);
      std::printf("uf-daemon: [1] fetch DISABLED (0x801c0000 <- 1) during setup\n"); }

    // 0x801c0080 = OUTPUT_REC_MASK (FFADO set_hardware_output_rec writes 28 quads of (rec!=0); the
    // factory driver writes the same 28-quadlet block). We have NEVER written this. It is the one
    // playback-side register both reference drivers touch and we don't.
    if (!getenv("UF_REC_SKIP")) {
        uint32_t v = getenv("UF_REC") ? (uint32_t)strtoul(getenv("UF_REC"), nullptr, 0) : 0u;
        std::vector<uint32_t> rec(dbq, v);
        wblock(conn, uf::reg::kOutputRecMask, rec.data(), dbq);
        std::printf("uf-daemon: [1b] OUTPUT_REC_MASK 0x801c0080 <- %u x%u\n", v, dbq);
    }

    // The device also accepts a 4-quadlet BLOCK write at 0xfc88f004 covering f004/f008/f00c/f010
    // atomically — where snd-fireface writes them as separate quadlets and we write none. It is
    // additive to the 0x0002 init below. q[2]/q[3] (comm start/stop) left 0 so we don't trigger
    // streaming here. UF_FC88F=<n quads>, default off.
    if (const char* nq = getenv("UF_FC88F")) {
        const uint32_t q4[4] = {(dbq << 11) | rx_channel, dbq, 0u, 0u};
        uint32_t n = (uint32_t)atoi(nq); if (n < 1 || n > 4) n = 4;
        wblock(conn, uf::reg::kRxPacketFormat, q4, n);
        std::printf("uf-daemon: [1c] 0xfc88f004 block <- %u quads {%08x,%08x,%08x,%08x}\n",
                    n, q4[0], q4[1], q4[2], q4[3]);
        usleep(20 * 1000);
    }

    // Session up on the 0x0002 bank. The tested FF800 firmware treats the fc88f STF as dead code and
    // uses this bank. init[1] = (dbq<<11)|rx_channel IS the rx-packet-format (playback-arm) value;
    // init[0]=rate, init[2]=dbq|s800flag.
    { const uint32_t init[3] = {rate, (dbq << 11) | rx_channel, dbqFlag};
      wblock(conn, uf::reg::kInitBankStream, init, 3); }
    usleep(100 * 1000);

    uint32_t tx_channel = 0xffffffff;
    for (int i = 0; i < 100 && tx_channel == 0xffffffff; ++i) {
        if (!rq(conn, uf::reg::kTxIsoChannel, &tx_channel)) break;
        if (tx_channel == 0xffffffff) usleep(20 * 1000);
    }
    if (tx_channel == 0xffffffff) { std::fprintf(stderr, "uf-daemon: no tx channel\n"); return 1; }
    std::printf("uf-daemon: STF+RxFormat+AllocTx done, device tx ch=%u\n", tx_channel);

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
        std::printf("uf-daemon: UF_TX_FPP=%u fullPayload=%u fullCount=%u\n", sytInterval, fullPayload, fullCount);
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
        // FFADO writes the mixer RAM one QUADLET at a time (writeRegister per cell); the FF800 mixer
        // may ignore block writes, which would leave the matrix full of power-on garbage that the
        // unmuted outputs then play (the beep). Zero every cell with quadlet writes.
        for (uint32_t off = 0; off < 0x2000; off += 4)
            wq(conn, uf::reg::kMixerRam + off, 0);
        for (uint32_t i = 0; i < dbq && i < 28; ++i) {
            if (!getenv("UF_NODIAG"))
                wq(conn, uf::reg::kMixerRam + (uint64_t)i * 0x100 + 0x80 + 4 * i, 0x8000);
            wq(conn, uf::reg::kMixerRam + 0x1f80 + 4 * i, 0x8000);
        }
        std::printf("uf-daemon: [2] TotalMix routed via quadlet writes (playback->output unity + faders)\n");
    }

    // [3] comm-start — BEFORE the iso streams, so the device is armed for playback when our transmit
    // begins. This is the ordering the device requires; we previously started the iso contexts first,
    // which left the device ignoring our playback entirely.
    wq(conn, uf::reg::kInitBankStart, 0x80000000u | dbqFlag);   // 0x0002 bank comm-start
    std::printf("uf-daemon: [3] comm-start (fc88f00c)\n");
    usleep(5 * 1000);

    // [4] iso start AFTER comm-start: capture context on the device-published channel, then our
    // transmit on rx_channel (the device now listening for playback there).
    call1(conn, kIsoStartCont, tx_channel);
    if (!getenv("UF_NO_TX")) {
        uint64_t a[3] = {rx_channel, fullPayload, fullCount};
        IOConnectCallScalarMethod(conn, kIsoTxStart, a, 3, nullptr, nullptr);
    } else std::printf("uf-daemon: [4] TRANSMIT SKIPPED (UF_NO_TX)\n");
    std::printf("uf-daemon: [4] iso started\n");

    // [5] ENABLE fetch LAST — snd-fireface enables fetch only after the iso streams are up and running
    // (snd_ff_stream_start_duplex → switch_fetching_mode(true)). Give the streams a moment to settle so
    // the device is receiving a valid packet cadence before it starts fetching PCM to its outputs.
    usleep(getenv("UF_FETCH_DELAY_MS") ? atoi(getenv("UF_FETCH_DELAY_MS")) * 1000 : 150 * 1000);
    if (!getenv("UF_FETCH_SKIP")) {
        uint32_t v = getenv("UF_FETCH") ? (uint32_t)strtoul(getenv("UF_FETCH"), nullptr, 0) : 0u;  // 0 = fetch/play
        std::vector<uint32_t> mask(dbq, v);
        wblock(conn, uf::reg::kStatus0, mask.data(), dbq);
        std::printf("uf-daemon: [5] fetch ENABLED (0x801c0000 <- %u) after streams settled\n", v);
    }


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

    uf::shm::Ring* cap = make_ring(uf::shm::kCaptureName);
    uf::shm::Ring* play = make_ring(uf::shm::kPlaybackName);
    if (!cap || !play) return 1;
    std::printf("uf-daemon: streaming (tx ch %u), rings up. Ctrl-C to stop.\n", tx_channel);

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
    uint64_t rt0 = mach_absolute_time(), rf0 = 0, rc0 = 0;
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
    uint64_t txCursor = 0; uint32_t txFill = 0; uint8_t* txSlotPtr = txBuf;

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
                std::printf("  REAL: %.0f frames/s (mach)  %.1f frames/bus-s  bus8k=%.2f Hz  %.0f pkt/s  ring=%llu over=%u under=%u\n",
                            (deviceFrames - rf0) / el, fVsBus, dct / busSec / 3072.0,
                            (consumed - rc0) / el,
                            (unsigned long long)cap->depth(), cap->overruns.load(), cap->underruns.load());
                std::fflush(stdout);
                rt0 = now; rf0 = deviceFrames; rc0 = consumed; ct0 = ct;
            }
        }
        if (++ticks % 1000 == 0) {
            std::printf("  cap: %llu pkt (+%llu/s)  frames=%llu (+%llu/s)  ring=%llu over=%u under=%u\n",
                        (unsigned long long)consumed, (unsigned long long)(consumed - lastConsumed),
                        (unsigned long long)deviceFrames, (unsigned long long)(deviceFrames - lastFrames),
                        (unsigned long long)cap->depth(),
                        cap->overruns.load(), cap->underruns.load());
            std::printf("       lengths: full=%llu empty=%llu other=%llu  dup-timestamps=%llu\n",
                        (unsigned long long)nFull, (unsigned long long)nEmpty, (unsigned long long)nOther,
                        (unsigned long long)nDupTs);
            std::fflush(stdout);
            lastConsumed = consumed; lastFrames = deviceFrames;
        }
        const uint64_t total = scalar0(conn, kIsoCompleted);
        while (consumed < total) {
            const uint8_t* slot = dextBuf + (consumed % (dextSize / kDextSlotBytes)) * kDextSlotBytes;
            const uint32_t* q = reinterpret_cast<const uint32_t*>(slot);
            const uint32_t len = q[1] >> 16;               // iso header data_length (2-quad prefix)
            if (len == 0) ++nEmpty; else if (len == fullPayload) ++nFull; else ++nOther;
            if (q[0] == lastTs) ++nDupTs; lastTs = q[0];   // duplicate iso timestamp = stale re-read
            if (len && len <= kDextSlotBytes - 8) {
                const uint8_t* payload = slot + 8;
                const uint32_t nframes = uf::frames_in_payload(len, dbq);
                const uint32_t ch = dbq < uf::shm::kMaxChannels ? dbq : uf::shm::kMaxChannels;
                for (uint32_t k = 0; k < nframes; ++k) {
                    // Push into the shm ring for the plugin; a full ring drops the frame — but the
                    // clock (deviceFrames) still counts it below, so the timebase never stalls.
                    if (!capSlot) { capSlot = cap->acquireWrite(); capFill = 0; }
                    if (capSlot) {
                        uf::decode_frame(payload + (size_t)k * dbq * 4, dbq, frame.data());
                        for (uint32_t c = 0; c < ch; ++c)
                            capSlot->audio[capFill * uf::shm::kMaxChannels + c] = frame[c] << 8; // 24->32
                        if (++capFill >= uf::shm::kFramesPerSlot) {
                            capSlot->frameCount = capFill; capSlot->channelCount = ch;
                            cap->commitWrite(); capSlot = nullptr;
                        }
                    }
                }
                deviceFrames += nframes;   // TRUE device sample position — advances for every frame
            }
            ++consumed;
        }
        call1(conn, kIsoRelease, consumed);

        // Publish the device-paced clock: {true frame position, now}, ~once per ZeroTimeStampPeriod
        // (kFramesPerSlot). CoreAudio locks its timeline to this, so it pulls at the real device rate.
        if (deviceFrames >= lastPublish + uf::shm::kFramesPerSlot) {
            cap->publishTimestamp((double)deviceFrames, mach_absolute_time());
            lastPublish = deviceFrames;
        }

        // Playback: encode the plugin's PCM into the transmit ring's FULL slots (blocking mode). Each
        // full slot takes syt_interval frames; empty slots (the 4th of every group) are skipped since
        // the dext transmits them header-only. SCAFFOLDING: the slot cursor free-runs at the pump
        // rate, not the isoch clock, so it needs pacing off an IT interrupt/cycle-timer to avoid
        // drift — verify on hardware. The plugin's frames come as fixed-size shm slots, so we thread a
        // frame cursor through them.
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
        else
        while (const uf::shm::Slot* ps = play->acquireRead()) {
            const uint32_t pch = ps->channelCount ? ps->channelCount : dbq;
            for (uint32_t f = 0; f < ps->frameCount; ++f) {
                // Skip empty tx slots (same cadence the dext programmed) so audio only lands in packets
                // that are actually transmitted.
                while (!uf::is_full_packet(txCursor % kTxSlots, fullCount)) ++txCursor;
                if (txFill == 0) txSlotPtr = txBuf + (size_t)(txCursor % kTxSlots) * kTxSlotBytes;
                uint8_t* fr = txSlotPtr + (size_t)txFill * dbq * 4;
                for (uint32_t c = 0; c < dbq; ++c) {
                    int32_t s = (c < pch) ? ps->audio[(size_t)f * pch + c] : 0;
                    uf::encode_sample_le(s >> 8, fr + (size_t)c * 4);   // ring int32 -> 24-bit sample
                }
                if (++txFill >= sytInterval) { txFill = 0; ++txCursor; }
            }
            play->commitRead();
        }

        usleep(1000);   // ~1 ms; a real daemon waits on an interrupt, not a sleep
    }

    std::printf("uf-daemon: stopping\n");
    { std::vector<uint32_t> ones(dbq, 1); wblock(conn, uf::reg::kStatus0, ones.data(), dbq); }  // fetch off
    IOConnectCallScalarMethod(conn, kIsoStop, nullptr, 0, nullptr, nullptr);
    IOConnectCallScalarMethod(conn, kIsoTxStop, nullptr, 0, nullptr, nullptr);
    { const uint32_t blob[3] = {0u,0u,0u}; wblock(conn, uf::reg::kInitBankStop, blob, 3); }   // 0x0002 comm-stop
    shm_unlink(uf::shm::kCaptureName);
    shm_unlink(uf::shm::kPlaybackName);
    IOServiceClose(conn);
    return 0;
}
