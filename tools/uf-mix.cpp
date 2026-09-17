// uf-mix — drive the FF800's TotalMix matrix mixer from the command line, through the dext.
//
// Uses the portable uf::mixer addressing + dB->coefficient math (spec/07, transcribed from FFADO
// fireface_hw.cpp::set_hardware_mixergain) and writes single quadlets to the matrix RAM via the
// UFFireWireOHCI user client — the same transport uf-set/uf-status use. The mixer runs in the FF800's
// own DSP, so a route set here is audible immediately (zero-latency monitoring), which is how we
// confirm the addressing by ear.
//
// Channel numbers on the command line are 1-based (as on the front panel / in TotalMix); they map to
// the 0-based src/dest indices the matrix uses. dB defaults to 0 (unity).
//
//   uf-mix in 3 1          # physical input 3 -> output 1 at 0 dB (unity monitor)
//   uf-mix in 3 1 -6       # ...at -6 dB
//   uf-mix pb 1 1          # playback channel 1 -> output 1 at 0 dB
//   uf-mix fader 1 -3      # output 1 master fader at -3 dB
//   uf-mix mute 3 1        # mute the input3->output1 cell (coefficient 0)
//   uf-mix raw 0x80080008 0x8000   # raw quadlet write (probing escape hatch)
//   uf-mix -n in 3 1       # dry run: print the transaction, write nothing (no hardware needed)
#include <IOKit/IOKitLib.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "uf/protocol/mixer.hpp"
#include "uf/protocol/registers.hpp"

enum { kUFOhciReadQuadlet = 0, kUFOhciWriteQuadlet = 1, kUFOhciWriteBlock = 2 };

static io_connect_t open_dext() {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
                                                  IOServiceNameMatching("UFFireWireOHCI"));
    if (!svc) return 0;
    io_connect_t conn = 0;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    return kr == KERN_SUCCESS ? conn : 0;
}

static void usage() {
    std::fprintf(stderr,
        "usage: uf-mix [-n] <command>\n"
        "  in    <src> <dst> [dB]   physical input src -> output dst  (default 0 dB)\n"
        "  pb    <src> <dst> [dB]   playback channel src -> output dst\n"
        "  fader <out> [dB]         per-output master fader\n"
        "  mute  <src> <dst>        mute the input src -> output dst cell (coefficient 0)\n"
        "  raw   <addr> <value>     raw quadlet write (hex ok)\n"
        "  -n                       dry run: print the transaction, do not write\n"
        "channel numbers are 1-based (front-panel numbering); dB may be negative/fractional\n");
}

// 1-based CLI channel -> 0-based matrix index; validates against the FF800 channel count.
static bool chan(const char* s, uf::u32* out) {
    char* end = nullptr;
    long v = std::strtol(s, &end, 10);
    if (end == s || *end || v < 1 || v > static_cast<long>(uf::kMixerChannels)) {
        std::fprintf(stderr, "bad channel '%s' (expect 1..%u)\n", s, uf::kMixerChannels);
        return false;
    }
    *out = static_cast<uf::u32>(v - 1);
    return true;
}

int main(int argc, char** argv) {
    bool dry = false;
    int i = 1;
    if (i < argc && !std::strcmp(argv[i], "-n")) { dry = true; ++i; }
    if (i >= argc) { usage(); return 2; }

    const char* cmd = argv[i++];
    int nrest = argc - i;
    char** rest = argv + i;

    uf::Addr addr = 0;
    uint32_t value = 0;

    if (!std::strcmp(cmd, "in") || !std::strcmp(cmd, "pb")) {
        if (nrest < 2 || nrest > 3) { usage(); return 2; }
        uf::u32 src = 0, dst = 0;
        if (!chan(rest[0], &src) || !chan(rest[1], &dst)) return 2;
        double db = (nrest == 3) ? std::atof(rest[2]) : 0.0;
        value = uf::db_to_coeff(db);
        addr = std::strcmp(cmd, "in") == 0 ? uf::input_coeff_addr(src, dst)
                                           : uf::playback_coeff_addr(src, dst);
    } else if (!std::strcmp(cmd, "mute")) {
        if (nrest != 2) { usage(); return 2; }
        uf::u32 src = 0, dst = 0;
        if (!chan(rest[0], &src) || !chan(rest[1], &dst)) return 2;
        value = uf::kMixerMute;
        addr = uf::input_coeff_addr(src, dst);
    } else if (!std::strcmp(cmd, "fader")) {
        if (nrest < 1 || nrest > 2) { usage(); return 2; }
        uf::u32 out = 0;
        if (!chan(rest[0], &out)) return 2;
        double db = (nrest == 2) ? std::atof(rest[1]) : 0.0;
        value = uf::db_to_coeff(db);
        addr = uf::output_fader_addr(out);
    } else if (!std::strcmp(cmd, "raw")) {
        if (nrest != 2) { usage(); return 2; }
        addr = std::strtoull(rest[0], nullptr, 0);
        value = static_cast<uint32_t>(std::strtoul(rest[1], nullptr, 0));
    } else {
        usage();
        return 2;
    }

    std::printf("wq(0x%llx, 0x%08x)%s\n", (unsigned long long)addr, value,
                dry ? "  [dry-run]" : "");
    if (dry) return 0;

    io_connect_t conn = open_dext();
    if (!conn) { std::fprintf(stderr, "dext user client not available — is the FF800 connected "
                                      "and UFFireWireOHCI activated?\n"); return 1; }

    uint64_t in[2] = {addr, value};
    kern_return_t kr = IOConnectCallScalarMethod(conn, kUFOhciWriteQuadlet, in, 2, nullptr, nullptr);
    IOServiceClose(conn);
    if (kr != KERN_SUCCESS) { std::fprintf(stderr, "write failed: 0x%x\n", kr); return 1; }
    std::printf("written\n");
    return 0;
}
