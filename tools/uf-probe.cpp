// uf-probe — read a region of the FF800's address space quadlet-by-quadlet, to hunt the metering
// data (spec/10 §10.3). The dext exposes only single-quadlet reads, so a "block" here is
// N sequential readQuadlet calls; that is enough to test whether a candidate region (e.g. the FF400
// status/meter bank 0x80100000, or the CSR variant 0xffff_ff000000) returns anything on the FF800.
//
// The key mode is --watch: read the region repeatedly and flag which quadlets CHANGE between reads.
// Feed a known signal (e.g. a -18 dBFS tone on analog in 1), run --watch, and the quadlets that move
// with the signal are the meter — then correlate index<->channel by muting inputs one at a time.
//
//   uf-probe 0x80100000 254          # dump 254 quadlets (1016 B) once
//   uf-probe 0xffffff000000 98        # the CSR variant (392 B)
//   uf-probe --watch 0x80100000 254  # loop; print only quadlets that change (Ctrl-C to stop)
//   uf-probe --watch --period=50 0x80100000 254   # 50 ms between reads
#include <IOKit/IOKitLib.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

enum { kUFOhciReadQuadlet = 0 };

static io_connect_t open_dext() {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
                                                  IOServiceNameMatching("UFFireWireOHCI"));
    if (!svc) return 0;
    io_connect_t conn = 0;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    return kr == KERN_SUCCESS ? conn : 0;
}

// Reads one quadlet; returns false if the transaction failed (e.g. the device rejects this address).
static bool read_quadlet(io_connect_t conn, uint64_t addr, uint32_t* out) {
    uint64_t in = addr, val = 0;
    uint32_t cnt = 1;
    if (IOConnectCallScalarMethod(conn, kUFOhciReadQuadlet, &in, 1, &val, &cnt) != KERN_SUCCESS)
        return false;
    *out = (uint32_t)val;
    return true;
}

// Reads `count` quadlets from `base` into out[]; ok[i] flags whether that quadlet read succeeded.
static void read_region(io_connect_t conn, uint64_t base, int count,
                        std::vector<uint32_t>& out, std::vector<char>& ok) {
    out.assign(count, 0);
    ok.assign(count, 0);
    for (int i = 0; i < count; ++i) ok[i] = read_quadlet(conn, base + 4 * i, &out[i]) ? 1 : 0;
}

static void dump(uint64_t base, const std::vector<uint32_t>& v, const std::vector<char>& ok) {
    for (size_t i = 0; i < v.size(); ++i) {
        if (i % 4 == 0) std::printf("%s0x%09llx:", i ? "\n" : "", (unsigned long long)(base + 4 * i));
        if (ok[i]) std::printf(" %08x", v[i]);
        else       std::printf(" --------");
    }
    std::printf("\n");
}

int main(int argc, char** argv) {
    bool watch = false;
    long period_ms = 100;
    int i = 1;
    for (; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--watch")) watch = true;
        else if (!std::strncmp(argv[i], "--period=", 9)) period_ms = std::strtol(argv[i] + 9, nullptr, 10);
        else break;
    }
    if (argc - i < 1) {
        std::fprintf(stderr,
            "usage: uf-probe [--watch] [--period=MS] <addr> [count]\n"
            "  read `count` quadlets (default 16) from `addr` (hex ok)\n"
            "  --watch: loop and print only quadlets that change between reads\n");
        return 2;
    }
    uint64_t base = std::strtoull(argv[i], nullptr, 0);
    int count = (argc - i >= 2) ? (int)std::strtol(argv[i + 1], nullptr, 0) : 16;
    if (count < 1) { std::fprintf(stderr, "count must be >= 1\n"); return 2; }

    io_connect_t conn = open_dext();
    if (!conn) { std::fprintf(stderr, "dext user client not available — is the FF800 connected "
                                      "and UFFireWireOHCI activated?\n"); return 1; }

    std::vector<uint32_t> cur, prev;
    std::vector<char> okc, okp;

    if (!watch) {
        read_region(conn, base, count, cur, okc);
        int nok = 0; for (char c : okc) nok += c;
        std::printf("read 0x%09llx .. 0x%09llx  (%d/%d quadlets ok)\n",
                    (unsigned long long)base, (unsigned long long)(base + 4 * (count - 1)), nok, count);
        dump(base, cur, okc);
        IOServiceClose(conn);
        return nok ? 0 : 1;
    }

    std::printf("watching 0x%09llx (%d quadlets), %ld ms period — Ctrl-C to stop\n"
                "(printing only quadlets whose value changes)\n",
                (unsigned long long)base, count, period_ms);
    read_region(conn, base, count, prev, okp);
    struct timespec ts{period_ms / 1000, (period_ms % 1000) * 1000000L};
    for (;;) {
        nanosleep(&ts, nullptr);
        read_region(conn, base, count, cur, okc);
        bool any = false;
        for (int k = 0; k < count; ++k) {
            if (okc[k] && okp[k] && cur[k] != prev[k]) {
                std::printf("  [%3d] 0x%09llx  %08x -> %08x\n", k,
                            (unsigned long long)(base + 4 * k), prev[k], cur[k]);
                any = true;
            }
        }
        if (any) std::printf("  ----\n");
        prev.swap(cur);
        okp.swap(okc);
    }
}
