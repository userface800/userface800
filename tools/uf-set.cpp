// uf-set — drive the FF800's settings from the command line, through the dext.
//
// Assembles the conf block with the portable settings code and writes it to 0xfc88f014. Everything it
// touches is volatile device state (a power cycle resets it), and the flags map 1:1 onto the front
// panel, so the device itself tells you whether the settings path is right.
//
//   uf-set                       # defaults: master clock, +4dBu in/out
//   uf-set --out=-10 --in=-10    # switch the level LEDs
//   uf-set --phantom=7,8         # phantom power on mic inputs 7 and 8
//   uf-set --autosync            # clock: back to AutoSync
#include <IOKit/IOKitLib.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "uf/protocol/registers.hpp"
#include "uf/protocol/settings.hpp"

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

int main(int argc, char** argv) {
    uf::SettingsShadow s{};   // defaults: internal master clock, +4dBu in/out

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strcmp(a, "--out=-10"))      s.output_level = uf::OutputLevel::M10dBV;
        else if (!std::strcmp(a, "--out=+4"))  s.output_level = uf::OutputLevel::P4dBu;
        else if (!std::strcmp(a, "--out=hi"))  s.output_level = uf::OutputLevel::HiGain;
        else if (!std::strcmp(a, "--in=-10"))  s.input_level = uf::InputLevel::M10dBV;
        else if (!std::strcmp(a, "--in=+4"))   s.input_level = uf::InputLevel::P4dBu;
        else if (!std::strcmp(a, "--in=lo"))   s.input_level = uf::InputLevel::LoGain;
        else if (!std::strcmp(a, "--autosync")) s.clock_master = false;
        else if (!std::strncmp(a, "--phantom=", 10)) {
            for (const char* p = a + 10; *p; ++p) {
                if (*p == '7') s.phantom7 = true;
                if (*p == '8') s.phantom8 = true;
                if (*p == '9') s.phantom9 = true;
                if (*p == '1') s.phantom10 = true;   // "10"
            }
        } else {
            std::fprintf(stderr, "unknown flag: %s\n", a);
            return 2;
        }
    }

    io_connect_t conn = open_dext();
    if (!conn) { std::fprintf(stderr, "dext user client not available\n"); return 1; }

    uf::ConfBlock cb = uf::assemble_conf_block(s);
    const uint32_t quads[3] = {cb.cr0, cb.cr1, cb.cr2};
    uint64_t off = uf::reg::kConfBlock;
    std::printf("CR0=0x%08x CR1=0x%08x CR2=0x%08x -> 0x%llx\n",
                cb.cr0, cb.cr1, cb.cr2, (unsigned long long)off);

    kern_return_t kr = IOConnectCallMethod(conn, kUFOhciWriteBlock, &off, 1, quads, 3 * 4,
                                           nullptr, nullptr, nullptr, nullptr);
    IOServiceClose(conn);
    if (kr != KERN_SUCCESS) { std::fprintf(stderr, "write failed: 0x%x\n", kr); return 1; }
    std::printf("written\n");
    return 0;
}
