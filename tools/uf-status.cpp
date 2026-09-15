// uf-status — read the FF800's status/identity registers through the UFFireWireOHCI dext and decode
// them with the portable protocol core (the same code the spec/09 unit tests exercise).
#include <IOKit/IOKitLib.h>
#include <cstdint>
#include <cstdio>

#include "uf/protocol/registers.hpp"
#include "uf/protocol/status.hpp"

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

int main() {
    io_connect_t conn = open_dext();
    if (!conn) {
        std::fprintf(stderr, "dext user client not available — is the adapter plugged in "
                             "and UFFireWireOHCI activated?\n");
        return 1;
    }

    uint32_t fw = 0, r0 = 0, r1 = 0, r3 = 0;
    bool ok = read_quadlet(conn, uf::reg::kFirmwareRev, &fw)
            & read_quadlet(conn, uf::reg::kStatus0, &r0)
            & read_quadlet(conn, uf::reg::kClockConfig, &r1)
            & read_quadlet(conn, uf::reg::kStatus3, &r3);
    IOServiceClose(conn);
    if (!ok) { std::fprintf(stderr, "register read failed\n"); return 1; }

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
