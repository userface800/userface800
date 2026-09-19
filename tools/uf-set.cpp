// uf-set — configure the FF800 from the command line, through the dext.
//
// Assembles the settings conf block and writes it to 0xfc88f014, and writes the
// clock-config register (0x801c0004) for clock source + sample rate. Everything it touches is VOLATILE
// device state (a power cycle resets it) and maps 1:1 onto the front panel, so the device itself tells
// you whether a setting took. To persist settings across power-off you must store to flash — NOT done
// here (see spec/11 for the flash layout; that write is destructive and still to be verified).
//
//   uf-set                              # defaults: internal master, 48 kHz, +4 dBu in/out
//   uf-set --rate=44100 --master        # master clock at 44.1 kHz
//   uf-set --autosync --sync=adat1      # slave to ADAT1
//   uf-set --sync=wclk --autosync       # slave to word clock
//   uf-set --in=-10 --out=+4 --phones=hi
//   uf-set --phantom=7,8                # phantom power on mic inputs 7 and 8
//   uf-set --filter --drive             # ch1 instrument filter + drive
//   uf-set --spdif-in=optical --spdif-out=optical --spdif-pro
//   uf-set --print                      # show what would be written, don't write
#include <IOKit/IOKitLib.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "uf/protocol/registers.hpp"
#include "../platform/shared/uf_ctl_client.hpp"
#include "uf/protocol/settings.hpp"
#include "uf/protocol/clock.hpp"
#include "uf/protocol/rate.hpp"

enum { kUFOhciReadQuadlet = 0, kUFOhciWriteQuadlet = 1, kUFOhciWriteBlock = 2 };

static void usage() {
    std::printf(
      "usage: uf-set [options]   (all settings are volatile until a power cycle)\n"
      "  clock:   --master | --autosync   --sync=adat1|adat2|spdif|wclk|tco   --rate=<hz>   --wclk-1x\n"
      "  levels:  --in=lo|+4|-10   --out=hi|+4|-10   --phones=+4|-10|hi\n"
      "  mics:    --phantom=7,8,9,10\n"
      "  inputs:  --input1=front|rear   --input7=front|rear   --input8=front|rear\n"
      "  instr:   --filter   --drive   --no-limiter\n"
      "  spdif:   --spdif-in=coax|optical   --spdif-out=coax|optical   --spdif-pro   --spdif-emphasis   --spdif-nonaudio\n"
      "  misc:    --print (show, don't write)\n");
}

int main(int argc, char** argv) {
    uf::SettingsShadow s{};          // defaults: internal master clock, +4 dBu in/out
    uint32_t rate = 48000;
    bool print_only = false;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto is = [&](const char* f){ return !std::strcmp(a, f); };
        auto pfx = [&](const char* f){ return !std::strncmp(a, f, std::strlen(f)); };

        if      (is("--help") || is("-h")) { usage(); return 0; }
        else if (is("--print"))     print_only = true;
        // clock
        else if (is("--master"))    s.clock_master = true;
        else if (is("--autosync"))  s.clock_master = false;
        else if (is("--wclk-1x"))   s.word_clock_1x = true;
        else if (pfx("--rate="))    rate = (uint32_t)strtoul(a + 7, nullptr, 10);
        else if (pfx("--sync=")) {
            const char* v = a + 7;
            if      (!std::strcmp(v, "adat1")) s.sync_ref = uf::SyncRef::Adat1;
            else if (!std::strcmp(v, "adat2")) s.sync_ref = uf::SyncRef::Adat2;
            else if (!std::strcmp(v, "spdif")) s.sync_ref = uf::SyncRef::Spdif;
            else if (!std::strcmp(v, "wclk"))  s.sync_ref = uf::SyncRef::WordClock;
            else if (!std::strcmp(v, "tco"))   s.sync_ref = uf::SyncRef::Tco;
            else { std::fprintf(stderr, "bad --sync=%s\n", v); return 2; }
        }
        // levels
        else if (is("--out=-10")) s.output_level = uf::OutputLevel::M10dBV;
        else if (is("--out=+4"))  s.output_level = uf::OutputLevel::P4dBu;
        else if (is("--out=hi"))  s.output_level = uf::OutputLevel::HiGain;
        else if (is("--in=-10"))  s.input_level = uf::InputLevel::M10dBV;
        else if (is("--in=+4"))   s.input_level = uf::InputLevel::P4dBu;
        else if (is("--in=lo"))   s.input_level = uf::InputLevel::LoGain;
        else if (is("--phones=+4"))  s.phones_level = uf::PhonesLevel::P4dBu;
        else if (is("--phones=-10")) s.phones_level = uf::PhonesLevel::M10dBV;
        else if (is("--phones=hi"))  s.phones_level = uf::PhonesLevel::HiGain;
        // input source select. Channels 1, 7 and 8 have both a front and a rear jack; leaving one
        // Unset emits no bits for it, which is what the spec/09 §9.2 vector does.
        else if (pfx("--input1=") || pfx("--input7=") || pfx("--input8=")) {
            const char* v = std::strchr(a, '=') + 1;
            uf::InputSource src;
            if      (!std::strcmp(v, "front")) src = uf::InputSource::Front;
            else if (!std::strcmp(v, "rear"))  src = uf::InputSource::Rear;
            else { std::fprintf(stderr, "bad %s (want front|rear)\n", a); return 2; }
            if      (a[7] == '1') s.input1 = src;
            else if (a[7] == '7') s.input7 = src;
            else                  s.input8 = src;
        }
        // instrument
        else if (is("--filter")) s.filter = true;
        else if (is("--drive"))  s.drive = true;
        // FF800: only effective with the front instrument input selected (spec/06).
        else if (is("--no-limiter")) s.disable_limiter = true;
        // spdif
        else if (is("--spdif-in=optical"))  s.spdif_in_optical = true;
        else if (is("--spdif-in=coax"))     s.spdif_in_optical = false;
        else if (is("--spdif-out=optical")) s.spdif_out_optical = true;
        else if (is("--spdif-out=coax"))    s.spdif_out_optical = false;
        else if (is("--spdif-pro"))         s.spdif_out_pro = true;
        else if (is("--spdif-emphasis"))    s.spdif_out_emphasis = true;
        else if (is("--spdif-nonaudio"))    s.spdif_out_nonaudio = true;
        // phantom
        else if (pfx("--phantom=")) {
            for (const char* p = a + 10; *p; ++p) {
                if (*p == '7') s.phantom7 = true;
                if (*p == '8') s.phantom8 = true;
                if (*p == '9') s.phantom9 = true;
                if (*p == '1') s.phantom10 = true;   // "10"
            }
        }
        else { std::fprintf(stderr, "unknown flag: %s (try --help)\n", a); return 2; }
    }

    if (!uf::is_supported_rate(rate)) {
        std::fprintf(stderr, "unsupported --rate=%u (32000/44100/48000/64000/88200/96000/128000/176400/192000)\n", rate);
        return 2;
    }

    // Assembled here only to SHOW what will be written; the daemon re-assembles it from the shadow
    // it adopts, so these values are informational rather than the thing sent.
    uf::ConfBlock cb = uf::assemble_conf_block(s);
    const uint32_t clkcfg = uf::assemble_clock_config(s.clock_master, s.sync_ref, rate);

    std::printf("clock:  %s", s.clock_master ? "internal master" : "autosync/slave");
    if (!s.clock_master) {
        const char* sr[] = {"ADAT1","ADAT2","SPDIF","word clock","TCO"};
        std::printf(" (sync=%s)", sr[(int)s.sync_ref]);
    }
    std::printf("   rate: %u Hz\n", rate);
    {
        const char* srcName[] = {"(unset)", "front", "rear"};
        std::printf("inputs: 1=%s 7=%s 8=%s   limiter: %s\n",
                    srcName[(int)s.input1], srcName[(int)s.input7], srcName[(int)s.input8],
                    s.disable_limiter ? "disabled" : "on");
    }
    std::printf("conf block  0x%llx <- CR0=0x%08x CR1=0x%08x CR2=0x%08x\n",
                (unsigned long long)uf::reg::kConfBlock, cb.cr0, cb.cr1, cb.cr2);
    std::printf("clock cfg   0x%llx <- 0x%08x\n",
                (unsigned long long)uf::reg::kClockConfig, clkcfg);
    if (print_only) return 0;

    // Through the daemon, not straight to the device. The daemon keeps the SettingsShadow and
    // re-assembles the conf block from it on every session start, so a register written directly
    // here was silently undone by the next rate change — the same bug the mixer had. Going through
    // it also means only one process ever issues register transactions, which matters because the
    // dext serialises nothing and its single AT context is shared.
    std::fflush(stdout);   // else the stderr below prints before the report above, when piped
    uf::ctl::Client cli;
    std::string err;
    if (!cli.connect(&err, /*clientKind=*/1 /*CLI*/)) {
        std::fprintf(stderr, "uf-set: %s\n", err.c_str());
        return 1;
    }
    if (!cli.set_settings(s, rate)) { std::fprintf(stderr, "uf-set: send failed\n"); return 1; }
    std::printf("sent (volatile on the device — a power cycle resets it, but the daemon re-applies\n"
                "      these settings on every session start, and remembers them across restarts)\n");
    return 0;
}
