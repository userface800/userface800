// uf-init — write the FF800's settings/conf block through the dext, and prove it landed.
//
// The conf block (CR0/CR1/CR2 -> 0xfc88f014) is what a driver writes at init; it is what FFADO
// sends and what the spec/09 §9.2 vector describes. The default shadow selects internal (master)
// clock, and SR1 bit 0 reports master mode — so a successful write must flip that bit from 0 to 1.
// Everything here is volatile device state: a power cycle resets it.
#include <IOKit/IOKitLib.h>
#include <cstdint>
#include <cstdio>

#include "uf/protocol/registers.hpp"
#include "uf/protocol/settings.hpp"
#include "uf/protocol/status.hpp"

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

static bool read_quadlet(io_connect_t conn, uint64_t offset, uint32_t* out) {
    uint64_t in = offset, val = 0;
    uint32_t cnt = 1;
    if (IOConnectCallScalarMethod(conn, kUFOhciReadQuadlet, &in, 1, &val, &cnt) != KERN_SUCCESS)
        return false;
    *out = (uint32_t)val;
    return true;
}

static kern_return_t write_block(io_connect_t conn, uint64_t offset,
                                 const uint32_t* quads, uint32_t count) {
    return IOConnectCallMethod(conn, kUFOhciWriteBlock, &offset, 1, quads, count * 4,
                               nullptr, nullptr, nullptr, nullptr);
}

int main() {
    io_connect_t conn = open_dext();
    if (!conn) { std::fprintf(stderr, "dext user client not available\n"); return 1; }

    uint32_t r0 = 0, r1 = 0;
    read_quadlet(conn, uf::reg::kStatus0, &r0);
    read_quadlet(conn, uf::reg::kClockConfig, &r1);
    std::printf("before:  SR0=0x%08x  SR1=0x%08x  (master=%d)\n", r0, r1, r1 & uf::sr1::kClockMaster ? 1 : 0);

    uf::SettingsShadow s{};                       // defaults: internal master clock, +4dBu, coax
    uf::ConfBlock cb = uf::assemble_conf_block(s);
    const uint32_t quads[3] = {cb.cr0, cb.cr1, cb.cr2};
    std::printf("writing conf block -> 0x%llx:  CR0=0x%08x CR1=0x%08x CR2=0x%08x\n",
                (unsigned long long)uf::reg::kConfBlock, cb.cr0, cb.cr1, cb.cr2);

    kern_return_t kr = write_block(conn, uf::reg::kConfBlock, quads, 3);
    if (kr != KERN_SUCCESS) {
        std::fprintf(stderr, "write failed: 0x%x\n", kr);
        IOServiceClose(conn);
        return 1;
    }
    std::printf("write acked\n");

    usleep(200 * 1000);
    read_quadlet(conn, uf::reg::kStatus0, &r0);
    read_quadlet(conn, uf::reg::kClockConfig, &r1);
    const bool master = r1 & uf::sr1::kClockMaster;
    std::printf("after:   SR0=0x%08x  SR1=0x%08x  (master=%d)\n", r0, r1, master ? 1 : 0);
    std::printf("\n%s\n", master ? "*** the device switched to internal master clock — the write landed ***"
                                 : "master bit did NOT flip — the write did not take effect");
    IOServiceClose(conn);
    return master ? 0 : 1;
}
