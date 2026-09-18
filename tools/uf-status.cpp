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
#include "../platform/shared/uf_shm_ring.hpp"

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
