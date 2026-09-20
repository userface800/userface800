// uf-flash — read/erase/write the FF800's on-device flash (NVRAM) through the dext.
//
// The FF800 persists its settings (and the TotalMix mixer state) in flash so they survive power-off
// and drive the unit standalone. Protocol adapted from FFADO fireface_flash.cpp (spec/11):
//   read:  quadlet reads of the flash address region (our dext has no block-read yet).
//   erase: write 0 to the erase register, then poll SR1 (0x801c0004) bit 30 until ready.
//   write: block-write the data region in <=32-quad chunks, polling ready after each.
//
//   uf-flash rev                          firmware revision
//   uf-flash read 0x3000f0000 64          dump N quadlets from a flash address
//   uf-flash dump-settings                dump the settings record (64 quads)
//   uf-flash dump-mixer 512               dump N quads of the mixer-shadow region
//   uf-flash erase-settings --force       erase the settings block  (DESTRUCTIVE)
//
// READ is safe. ERASE/WRITE are DESTRUCTIVE and HARDWARE-GATED (never run on hardware); they require
// --force. A faithful "store my settings" also needs the flash settings-record layout, which reading
// a driver-written region will reveal — do that before trusting a write.
#include <IOKit/IOKitLib.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <vector>

#include "uf/protocol/registers.hpp"

enum { kRQ = 0, kWQ = 1, kWB = 2 };

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

// Poll SR1 until the flash-ready bit is set. The bit reads 1 when the flash is idle, so this
// returns true once the device is ready and false if it never becomes ready.
static bool wait_ready(io_connect_t c, int init_ms) {
    for (int i = 0; i < 25; ++i) {
        usleep(init_ms * 1000);
        uint32_t s = 0;
        if (rq(c, uf::reg::kClockConfig, &s) && (s & uf::reg::kFlashBusyBit)) return true;
    }
    return false;
}

static void dump(io_connect_t c, uint64_t addr, uint32_t nquads) {
    for (uint32_t i = 0; i < nquads; i += 4) {
        std::printf("0x%09llx:", (unsigned long long)(addr + (uint64_t)i * 4));
        for (uint32_t j = 0; j < 4 && i + j < nquads; ++j) {
            uint32_t v = 0;
            std::printf(" %08x", rq(c, addr + (uint64_t)(i + j) * 4, &v) ? v : 0xffffffffu);
        }
        std::printf("\n");
    }
}

int main(int argc, char** argv) {
    bool force = false;
    for (int i = 0; i < argc; ++i) if (!std::strcmp(argv[i], "--force")) force = true;
    if (argc < 2) { std::fprintf(stderr, "usage: uf-flash rev|read|dump-settings|dump-mixer|erase-settings\n"); return 2; }

    io_connect_t conn = open_dext();
    if (!conn) { std::fprintf(stderr, "dext user client not available\n"); return 1; }

    const char* cmd = argv[1];
    if (!std::strcmp(cmd, "rev")) {
        uint32_t v = 0; rq(conn, uf::reg::kFirmwareRev, &v);
        std::printf("firmware revision: 0x%08x (%u)\n", v, v);
    } else if (!std::strcmp(cmd, "read") && argc >= 4) {
        dump(conn, strtoull(argv[2], nullptr, 0), (uint32_t)strtoul(argv[3], nullptr, 0));
    } else if (!std::strcmp(cmd, "dump-settings")) {
        std::printf("settings record @ 0x%09llx:\n", (unsigned long long)uf::reg::kFlashSettings);
        dump(conn, uf::reg::kFlashSettings, uf::reg::kFlashSectorQuads);
    } else if (!std::strcmp(cmd, "dump-mixer")) {
        uint32_t n = argc >= 3 ? (uint32_t)strtoul(argv[2], nullptr, 0) : 64;
        dump(conn, uf::reg::kFlashMixerShadow, n);
    } else if (!std::strcmp(cmd, "erase-settings")) {
        if (!force) { std::fprintf(stderr, "erase is DESTRUCTIVE — pass --force. UNTESTED on hardware.\n"); return 2; }
        std::printf("erasing settings block...\n");
        if (!wq(conn, uf::reg::kFlashEraseSettings, 0)) { std::fprintf(stderr, "erase write failed\n"); return 1; }
        if (!wait_ready(conn, 500)) {
            std::fprintf(stderr, "timeout waiting for flash ready\n");
            IOServiceClose(conn);
            return 1;
        }
        std::printf("erased (flash ready)\n");
        usleep(20000);
    } else {
        std::fprintf(stderr, "unknown/incomplete command\n"); IOServiceClose(conn); return 2;
    }
    IOServiceClose(conn);
    return 0;
}
