// uf-status — one command that answers "is this thing working, and if not, what do I do?".
//
// Two halves. First a SYSTEM view: is the dext loaded, is the adapter attached, is the daemon
// running and streaming — each failure naming its own remedy, because "no audio" has several
// completely different causes and they need different actions. Then the DEVICE view: clock, sync
// source and per-input lock, decoded by the portable protocol core.
//
// The device half prefers the daemon's shared-memory ring over touching the hardware. The dext has a
// SINGLE AT context with no serialisation, so a second user client issuing register reads can
// interleave with the daemon's own transactions. Reading rate/clock out of the ring costs the device
// nothing and cannot collide; direct register reads are the fallback for when no daemon is running,
// which is exactly when they are safe.
#include <IOKit/IOKitLib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "uf/protocol/registers.hpp"
#include "uf/protocol/status.hpp"
#include "../platform/shared/uf_control.hpp"
#include "../platform/shared/uf_mixer_model.hpp"
#include "../platform/shared/uf_shm_ring.hpp"
#include "../platform/shared/uf_state_shm.hpp"
#include "../platform/shared/uf_mixer_shadow.hpp"

// Must match UFFireWireOHCIShared.h.
enum { kUFOhciReadQuadlet = 0, kUFOhciWriteQuadlet = 1 };

static io_connect_t open_dext() {
    // A dext publishes as class IOUserService named after its personality — match the name.
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
                                                  IOServiceNameMatching("UFFireWireOHCI"));
    if (!svc) return 0;
    io_connect_t conn = 0;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    return kr == KERN_SUCCESS ? conn : 0;
}

static bool read_quadlet(io_connect_t conn, uint64_t offset, uint32_t* out) {
    uint64_t in = offset, val = 0;
    uint32_t cnt = 1;
    if (IOConnectCallScalarMethod(conn, kUFOhciReadQuadlet, &in, 1, &val, &cnt) != KERN_SUCCESS)
        return false;
    *out = (uint32_t)val;
    return true;
}

static const char* sync_name(uf::SyncSource s) {
    switch (s) {
        case uf::SyncSource::Adat1: return "ADAT1";
        case uf::SyncSource::Adat2: return "ADAT2";
        case uf::SyncSource::Spdif: return "SPDIF";
        case uf::SyncSource::WordClock: return "word clock";
        case uf::SyncSource::Tco: return "TCO";
        default: return "none";
    }
}

static void line(const char* name, bool lock, bool sync, uint32_t hz) {
    std::printf("  %-12s lock=%-3s sync=%-3s", name, lock ? "yes" : "no", sync ? "yes" : "no");
    if (hz) std::printf("  %u Hz", hz);
    std::printf("\n");
}

// Is the dext installed and activated at all? Distinguishing "never loaded" from "loaded but no
// hardware" matters most on a first run, where "plug it in" is actively misleading if the real
// answer is that the driver was never installed. Only systemextensionsctl knows.
static bool dext_activated() {
    FILE* f = popen("systemextensionsctl list 2>/dev/null", "r");
    if (!f) return false;
    char buf[512];
    bool found = false;
    while (std::fgets(buf, sizeof buf, f))
        if (std::strstr(buf, "com.userface800.UFLoader.UFFireWireOHCI") && std::strstr(buf, "activated"))
            found = true;
    pclose(f);
    return found;
}

// Map the daemon's capture ring read-only. Its presence AND a non-zero deviceRate is what "the
// daemon is streaming" actually means — a stale ring can outlive the process that made it.
static const uf::shm::Ring* map_capture_ring() {
    int fd = shm_open(uf::shm::kCaptureName, O_RDONLY, 0);
    if (fd < 0) return nullptr;
    void* p = mmap(nullptr, sizeof(uf::shm::Ring), PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return nullptr;
    const uf::shm::Ring* r = static_cast<const uf::shm::Ring*>(p);
    if (r->magic != uf::shm::kMagic) { munmap(p, sizeof(uf::shm::Ring)); return nullptr; }
    return r;
}

// The published state segment — separate from the audio ring, so reading it can never disturb
// coreaudiod or require the plugin to be rebuilt in step.
static bool read_pub_state(uf::state::Shared* out) {
    int fd = shm_open(uf::state::kName, O_RDONLY, 0);
    if (fd < 0) return false;
    void* p = mmap(nullptr, sizeof(uf::state::Shared), PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return false;
    const auto* s = static_cast<const uf::state::Shared*>(p);
    bool ok = false;
    // Version, not just magic: the magic survives layout changes, so an older daemon's segment would
    // be read with fields that have moved and reported as though it were the device's state.
    if (s->compatible())
        for (int i = 0; i < 16 && !ok; ++i) ok = uf::state::read(s, out);   // retry a torn read
    munmap(p, sizeof(uf::state::Shared));
    return ok;
}

// The settings the daemon believes are in effect. Worth stating plainly that these come from the
// daemon's SHADOW, not from the device: the FF800's settings register is write-only, so the shadow
// is the only place these values exist at all.
static void print_settings(const uf::state::Shared& st) {
    const auto& s = st.settings;
    const char* src[] = {"(unset)", "front", "rear"};
    std::printf("\nsettings (as the daemon has them — the device cannot be read back):\n");
    std::printf("  phantom     7:%s 8:%s 9:%s 10:%s\n",
                s.phantom7 ? "on" : "off", s.phantom8 ? "on" : "off",
                s.phantom9 ? "on" : "off", s.phantom10 ? "on" : "off");
    std::printf("  levels      in %s   out %s   phones %s\n",
                uf::level_name(s.input_level), uf::level_name(s.output_level),
                uf::level_name(s.phones_level));
    std::printf("  inputs      1=%s 7=%s 8=%s\n",
                src[(int)s.input1], src[(int)s.input7], src[(int)s.input8]);
    std::printf("  ch1         filter %s   drive %s\n",
                s.filter ? "on" : "off", s.drive ? "on" : "off");
    std::printf("  spdif       out %s%s   in %s\n",
                s.spdif_out_pro ? "pro" : "consumer",
                s.spdif_out_optical ? ", optical" : ", coax",
                s.spdif_in_optical ? "optical" : "coax");
}

// Only the cells that are actually doing something. Printing 2048 mostly-muted cells would bury the
// three that matter.
static void print_routing(const uf::state::Shared& st) {
    // The LOGICAL mixer, not the rendered cells. A muted crosspoint renders to 0, so listing the
    // matrix alone hid every mute — which is the one state a user most wants confirmed, because a
    // mute is why something is not making a sound.
    constexpr uint32_t kCh = uf::state::Shared::kMixerCh;
    auto cellstr = [](uint32_t gain, uint8_t flags, char* buf, size_t n) {
        std::snprintf(buf, n, "0x%05x%s%s", gain,
                      (flags & uf::kMfMuted) ? "  MUTED" : "",
                      (flags & uf::kMfInverted) ? "  PHASE-INV" : "");
    };

    std::printf("\nmixer routing:\n");
    unsigned shown = 0;
    char buf[48];
    for (uint32_t dest = 0; dest < kCh && shown < 64; ++dest)
        for (uint32_t src = 0; src < kCh && shown < 64; ++src) {
            const uint32_t i = src * kCh + dest;
            if (!st.inputGain[i] && !st.inputFlags[i]) continue;
            cellstr(st.inputGain[i], st.inputFlags[i], buf, sizeof buf);
            std::printf("  input %-2u -> out %-2u  %s\n", src + 1, dest + 1, buf);
            ++shown;
        }
    for (uint32_t dest = 0; dest < kCh && shown < 64; ++dest)
        for (uint32_t src = 0; src < kCh && shown < 64; ++src) {
            const uint32_t i = src * kCh + dest;
            if (src == dest) continue;      // the default 1:1 playback route; not news
            if (!st.playbackGain[i] && !st.playbackFlags[i]) continue;
            cellstr(st.playbackGain[i], st.playbackFlags[i], buf, sizeof buf);
            std::printf("  playback %-2u -> out %-2u  %s\n", src + 1, dest + 1, buf);
            ++shown;
        }
    if (!shown) std::printf("  (defaults only: playback N -> output N at unity)\n");

    // Outputs: anything other than "at unity, no flags" is worth saying. Loopback especially — it
    // changes what the RECORDER gets, so it is invisible in monitoring and baffling in a DAW.
    unsigned outs = 0;
    for (uint32_t o = 0; o < kCh; ++o) {
        const uint8_t f = st.outputFlags[o];
        if (st.outputGain[o] == uf::kMixerUnity && !f) continue;
        if (!outs++) std::printf("\noutputs (non-default):\n");
        std::printf("  out %-2u  0x%05x%s%s\n", o + 1, st.outputGain[o],
                    (f & uf::kMfMuted) ? "  MUTED" : "",
                    (f & uf::kMfRec) ? "  LOOPBACK (this mix replaces the input in the recorder)" : "");
    }

    if (st.stereoIn || st.stereoPb) {
        std::printf("\nstereo pairs (host-side only — the device has no notion of it):\n");
        auto pairs = [](const char* what, uint32_t bits) {
            if (!bits) return;
            std::printf("  %-8s", what);
            for (uint32_t p = 0; p < kCh / 2; ++p)
                if ((bits >> p) & 1u) std::printf(" %u/%u", p * 2 + 1, p * 2 + 2);
            std::printf("\n");
        };
        pairs("inputs", st.stereoIn);
        pairs("playback", st.stereoPb);
    }
}

int main() {
    // ── system ────────────────────────────────────────────────────────────────────────────────
    io_connect_t conn = open_dext();
    const uf::shm::Ring* ring = map_capture_ring();
    const uint32_t devRate = ring ? ring->deviceRate.load(std::memory_order_relaxed) : 0;

    std::printf("dext:    %s\n", conn ? "loaded, matched to the adapter"
                                       : (dext_activated() ? "activated, but NOT matched"
                                                           : "NOT LOADED"));
    std::printf("daemon:  %s\n", devRate ? "running, streaming"
                                          : (ring ? "ring present but not streaming" : "not running"));
    if (devRate) std::printf("rate:    %u Hz  (clock %s, %s)\n", devRate,
                             uf::ctl::clock_source_name((uf::ctl::ClockSource)ring->clockSource.load(std::memory_order_relaxed)),
                             ring->clockLocked.load(std::memory_order_relaxed) ? "locked" : "NOT locked");

    // The daemon's own view, printed BEFORE any early return: it comes from the shm segment, not the
    // hardware, so it is exactly as valid when no device is attached — and that is precisely when
    // you want to see what the daemon is holding for you.
    uf::state::Shared st{};
    const bool haveState = read_pub_state(&st);
    if (haveState) { print_settings(st); print_routing(st); }

    if (!conn) {
        std::printf("\n");
        std::fflush(stdout);   // else the stderr guidance below prints before the report above
        if (!dext_activated()) {
            std::fprintf(stderr,
                "The driver is not installed. This is a setup problem, not a cable:\n"
                "  bash platform/loader/build-and-sign.sh\n"
                "  /Applications/UFLoader.app/Contents/MacOS/UFLoader\n"
                "It also needs the security prerequisites in platform/loader/LOADING.md.\n");
        } else {
            std::fprintf(stderr,
                "The dext is activated but has nothing to drive — attach the Thunderbolt->FireWire\n"
                "adapter. If it IS attached, the old dext may still be bound after a reinstall:\n"
                "  sudo pkill -9 -f 'UFFireWireOHCI\\.dext/UFFireWireOHCI'\n");
        }
        return 1;
    }
    std::printf("\n");

    uint32_t fw = 0, r0 = 0, r1 = 0, r3 = 0;
    bool ok = read_quadlet(conn, uf::reg::kFirmwareRev, &fw)
            & read_quadlet(conn, uf::reg::kStatus0, &r0)
            & read_quadlet(conn, uf::reg::kClockConfig, &r1)
            & read_quadlet(conn, uf::reg::kStatus3, &r3);
    IOServiceClose(conn);
    if (!ok) {
        std::fflush(stdout);
        std::fprintf(stderr,
            "The adapter is present but the FF800 is not answering register reads.\n"
            "Check it is powered on and the FireWire cable is seated. If the daemon log says\n"
            "'no tx channel', the device is wedged and needs a POWER CYCLE — it recovers by itself\n"
            "once you do, no restart needed.\n");
        return 1;
    }

    std::printf("FF800 raw:  firmware=0x%08x  SR0=0x%08x  SR1=0x%08x  SR3=0x%08x\n\n", fw, r0, r1, r3);

    uf::UFStatus s = uf::decode_status(r0, r1);
    // A frequency field only means anything once the corresponding input is locked — otherwise it is
    // stale bits and reporting it invites a wrong reading (an unlocked SR0 decodes to "250 Hz").
    const bool synced = s.adat1_lock || s.adat2_lock || s.spdif_lock || s.wclk_lock || s.tco_lock;

    std::printf("clock:  %s\n", s.clock_master ? "internal (master)" : "autosync / external");
    if (synced) {
        std::printf("sync source: %s\n", sync_name(s.sync_source));
        if (s.ext_rate_hz)   std::printf("external rate: %u Hz\n", s.ext_rate_hz);
        if (s.input_freq_hz) std::printf("autosync input: %u Hz\n", s.input_freq_hz);
    } else {
        std::printf("sync source: (nothing locked)\n");
    }
    std::printf("\ninputs:\n");
    line("ADAT1", s.adat1_lock, s.adat1_sync, 0);
    line("ADAT2", s.adat2_lock, s.adat2_sync, 0);
    line("SPDIF", s.spdif_lock, s.spdif_sync, s.spdif_lock ? s.spdif_freq_hz : 0);
    line("word clock", s.wclk_lock, s.wclk_sync, 0);
    line("TCO", s.tco_lock, s.tco_sync, 0);
    if (s.over) std::printf("\n  ** OVER (clipping) **\n");

    return 0;
}
