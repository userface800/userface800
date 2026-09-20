// uf-mix — drive the FF800's matrix mixer from the command line, through the dext.
//
// Uses the portable uf::mixer addressing + dB->coefficient math (spec/07, transcribed from FFADO
// fireface_hw.cpp::set_hardware_mixergain) and writes single quadlets to the matrix RAM via the
// UFFireWireOHCI user client — the same transport uf-set/uf-status use. The mixer runs in the FF800's
// own DSP, so a route set here is audible immediately (zero-latency monitoring), which is how we
// confirm the addressing by ear.
//
// Channel numbers on the command line are 1-based (as on the front panel / in the device's own
// mixer); they map to
// the 0-based src/dest indices the matrix uses. dB defaults to 0 (unity).
//
//   uf-mix in 3 1            # physical input 3 -> output 1 at 0 dB (unity monitor)
//   uf-mix in 3 1 -6         # ...at -6 dB
//   uf-mix pb 1 1            # playback channel 1 -> output 1 at 0 dB
//   uf-mix fader 1 -3        # output 1 master fader at -3 dB
//   uf-mix mute in 3 1 on    # mute that crosspoint, KEEPING the gain under it
//   uf-mix phase in 3 1 on   # invert its phase 180 deg (a negative coefficient)
//   uf-mix loopback 5 on     # output 5's mix replaces input 5 in the record stream
//   uf-mix stereo in 1 on    # pair input channels 1/2 (host-side; affects UIs, not the device)
//   uf-mix pan in 9 1 -0.5   # input 9 across the 1/2 output pair, half left
//   uf-mix copy 1 5          # copy output 1's whole submix onto output 5
//   uf-mix clear 5           # drop everything feeding output 5 (its fader is left alone)
//   uf-mix raw 0x80080008 0x8000   # raw quadlet write (probing escape hatch)
//   uf-mix -n in 3 1         # dry run: print the transaction, write nothing (no hardware needed)
#include <IOKit/IOKitLib.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../platform/shared/uf_ctl_client.hpp"
#include "../platform/shared/uf_mixer_model.hpp"
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
        "  in    <src> <dst> [dB]        physical input src -> output dst  (default 0 dB)\n"
        "  pb    <src> <dst> [dB]        playback channel src -> output dst\n"
        "  fader <out> [dB]              per-output master fader\n"
        "  mute  in|pb <src> <dst> on|off   mute a crosspoint, keeping its gain\n"
        "  phase in|pb <src> <dst> on|off   invert phase 180 deg (negative coefficient)\n"
        "  omute <out> on|off            mute a hardware output\n"
        "  loopback <out> on|off         send that output's mix to the recorder (device loopback)\n"
        "  stereo in|pb <ch> on|off      pair ch with its neighbour (host-side view/edit only)\n"
        "  pan   in|pb <src> <dstL> <-1..1> [dB]   spread across the dstL/dstL+1 output pair\n"
        "  copy  <fromOut> <toOut>       copy a whole submix column\n"
        "  clear <out>                   drop everything feeding an output\n"
        "  raw   <addr> <value>          raw quadlet write (hex ok)\n"
        "  -n                            dry run: print the transaction, do not write\n"
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

// "in"/"pb" -> which half of the destination block. The distinction is the manual's top row vs
// middle row: hardware inputs vs software playback (§25.2).
static bool srckind(const char* s, uf::ctl::MixerSrcKind* out) {
    if (!std::strcmp(s, "in")) { *out = uf::ctl::MixerSrcKind::Input; return true; }
    if (!std::strcmp(s, "pb")) { *out = uf::ctl::MixerSrcKind::Playback; return true; }
    std::fprintf(stderr, "expected 'in' or 'pb', got '%s'\n", s);
    return false;
}

static bool onoff(const char* s, bool* out) {
    if (!std::strcmp(s, "on")  || !std::strcmp(s, "1")) { *out = true;  return true; }
    if (!std::strcmp(s, "off") || !std::strcmp(s, "0")) { *out = false; return true; }
    std::fprintf(stderr, "expected 'on' or 'off', got '%s'\n", s);
    return false;
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
    // What to tell the daemon. `raw` has no model equivalent, so it stays a direct register poke.
    enum class Act { Cell, Fader, CellFlags, OutFlags, Stereo, Pan, Submix, Raw };
    Act act = Act::Cell;
    uf::ctl::MixerSrcKind kind = uf::ctl::MixerSrcKind::Input;
    uint16_t csrc = 0, cdst = 0, cout = 0;
    uint16_t cellMask = 0, cellValue = 0;
    uint32_t panL = 0, panR = 0;

    if (!std::strcmp(cmd, "in") || !std::strcmp(cmd, "pb")) {
        if (nrest < 2 || nrest > 3) { usage(); return 2; }
        uf::u32 src = 0, dst = 0;
        if (!chan(rest[0], &src) || !chan(rest[1], &dst)) return 2;
        double db = (nrest == 3) ? std::atof(rest[2]) : 0.0;
        value = uf::db_to_coeff(db);
        kind = std::strcmp(cmd, "in") == 0 ? uf::ctl::MixerSrcKind::Input
                                           : uf::ctl::MixerSrcKind::Playback;
        csrc = (uint16_t)src; cdst = (uint16_t)dst;
        addr = kind == uf::ctl::MixerSrcKind::Input ? uf::input_coeff_addr(src, dst)
                                                    : uf::playback_coeff_addr(src, dst);
    } else if (!std::strcmp(cmd, "mute") || !std::strcmp(cmd, "phase")) {
        // Flags, not a coefficient. Muting used to write 0, which destroyed the gain it was hiding
        // and made un-muting impossible; the daemon now keeps the two apart (uf_mixer_model.hpp).
        if (nrest != 4) { usage(); return 2; }
        uf::u32 src = 0, dst = 0;
        if (!srckind(rest[0], &kind) || !chan(rest[1], &src) || !chan(rest[2], &dst)) return 2;
        bool on = false;
        if (!onoff(rest[3], &on)) return 2;
        csrc = (uint16_t)src; cdst = (uint16_t)dst;
        cellMask = std::strcmp(cmd, "mute") == 0 ? uf::kMfMuted : uf::kMfInverted;
        cellValue = on ? cellMask : 0;
        act = Act::CellFlags;
    } else if (!std::strcmp(cmd, "omute") || !std::strcmp(cmd, "loopback")) {
        if (nrest != 2) { usage(); return 2; }
        uf::u32 out = 0;
        if (!chan(rest[0], &out)) return 2;
        bool on = false;
        if (!onoff(rest[1], &on)) return 2;
        cout = (uint16_t)out;
        cellMask = std::strcmp(cmd, "omute") == 0 ? uf::kMfMuted : uf::kMfRec;
        cellValue = on ? cellMask : 0;
        act = Act::OutFlags;
    } else if (!std::strcmp(cmd, "stereo")) {
        // Inputs and playback pair independently; hardware outputs are always stereo (manual §25.3).
        if (nrest != 3) { usage(); return 2; }
        uf::u32 ch = 0;
        if (!srckind(rest[0], &kind) || !chan(rest[1], &ch)) return 2;
        bool on = false;
        if (!onoff(rest[2], &on)) return 2;
        csrc = (uint16_t)ch; cellValue = on ? 1 : 0;
        act = Act::Stereo;
    } else if (!std::strcmp(cmd, "pan")) {
        // Pan is not device state: it IS the two crosspoints. Computed here with the model's own
        // -3 dB centre law and sent as two ordinary cell writes, so a pan and a hand-edited
        // crosspoint can never disagree about what the matrix contains.
        if (nrest < 4 || nrest > 5) { usage(); return 2; }
        uf::u32 src = 0, dstL = 0;
        if (!srckind(rest[0], &kind) || !chan(rest[1], &src) || !chan(rest[2], &dstL)) return 2;
        const double pan = std::atof(rest[3]);
        const double db  = (nrest == 5) ? std::atof(rest[4]) : 0.0;
        uf::MixerModel mm;
        mm.set_pan(kind, src, dstL, pan, uf::db_to_coeff(db));
        csrc = (uint16_t)src; cdst = (uint16_t)dstL;
        panL = mm.cell(kind, src, dstL).gain;
        panR = mm.cell(kind, src, uf::MixerModel::partner(dstL)).gain;
        act = Act::Pan;
    } else if (!std::strcmp(cmd, "copy") || !std::strcmp(cmd, "clear")) {
        const bool isClear = std::strcmp(cmd, "clear") == 0;
        if (nrest != (isClear ? 1 : 2)) { usage(); return 2; }
        uf::u32 a = 0, b = 0;
        if (isClear) { if (!chan(rest[0], &b)) return 2; }
        else if (!chan(rest[0], &a) || !chan(rest[1], &b)) return 2;
        csrc = (uint16_t)a; cdst = (uint16_t)b; cellValue = isClear ? 1 : 0;
        act = Act::Submix;
    } else if (!std::strcmp(cmd, "fader")) {
        if (nrest < 1 || nrest > 2) { usage(); return 2; }
        uf::u32 out = 0;
        if (!chan(rest[0], &out)) return 2;
        double db = (nrest == 2) ? std::atof(rest[1]) : 0.0;
        value = uf::db_to_coeff(db);
        act = Act::Fader; cout = (uint16_t)out;
        addr = uf::output_fader_addr(out);
    } else if (!std::strcmp(cmd, "raw")) {
        if (nrest != 2) { usage(); return 2; }
        act = Act::Raw;
        addr = std::strtoull(rest[0], nullptr, 0);
        value = static_cast<uint32_t>(std::strtoul(rest[1], nullptr, 0));
    } else {
        usage();
        return 2;
    }

    // Say what will actually happen, in the device's terms where there is a register behind it —
    // a dry run whose output is just the command echoed back tells you nothing you did not type.
    const char* tag = dry ? "  [dry-run]" : "";
    switch (act) {
        case Act::Cell: case Act::Fader: case Act::Raw:
            std::printf("wq(0x%llx, 0x%08x)%s\n", (unsigned long long)addr, value, tag);
            break;
        case Act::Pan: {
            const uf::u32 dR = uf::MixerModel::partner(cdst);
            const uf::Addr aL = kind == uf::ctl::MixerSrcKind::Input
                                  ? uf::input_coeff_addr(csrc, cdst) : uf::playback_coeff_addr(csrc, cdst);
            const uf::Addr aR = kind == uf::ctl::MixerSrcKind::Input
                                  ? uf::input_coeff_addr(csrc, dR) : uf::playback_coeff_addr(csrc, dR);
            std::printf("wq(0x%llx, 0x%08x)  wq(0x%llx, 0x%08x)%s\n",
                        (unsigned long long)aL, panL, (unsigned long long)aR, panR, tag);
            break;
        }
        case Act::CellFlags:
            std::printf("%s %u -> %u = %s%s\n", cmd, csrc + 1, cdst + 1,
                        cellValue ? "on" : "off", tag);
            break;
        case Act::OutFlags:
            std::printf("%s out %u = %s%s\n", cmd, cout + 1, cellValue ? "on" : "off", tag);
            break;
        case Act::Stereo:
            std::printf("stereo %u/%u = %s%s\n", (csrc & ~1u) + 1, (csrc | 1u) + 1,
                        cellValue ? "on" : "off", tag);
            break;
        case Act::Submix:
            if (cellValue) std::printf("clear submix on output %u%s\n", cdst + 1, tag);
            else           std::printf("copy submix output %u -> %u%s\n", csrc + 1, cdst + 1, tag);
            break;
    }
    if (dry) return 0;

    // Everything except `raw` goes THROUGH THE DAEMON. Two reasons, both learned the hard way:
    // the daemon holds the mixer shadow, so a route sent this way survives the next rate change
    // (writing the register directly does not — session_start re-applies the shadow over it); and
    // the dext serialises nothing, so a second user client poking registers can interleave with the
    // daemon's own transactions inside its single AT context.
    if (act != Act::Raw) {
        std::fflush(stdout);
        uf::ctl::Client cli;
        std::string err;
        if (!cli.connect(&err, /*clientKind=*/1 /*CLI*/)) {
            std::fprintf(stderr, "uf-mix: %s\n", err.c_str());
            return 1;
        }
        bool ok = false;
        switch (act) {
            case Act::Cell:      ok = cli.set_mixer(kind, csrc, cdst, value); break;
            case Act::Fader:     ok = cli.set_fader(cout, value); break;
            case Act::CellFlags: ok = cli.set_cell_flags(kind, csrc, cdst, cellMask, cellValue); break;
            case Act::OutFlags:  ok = cli.set_out_flags(cout, cellMask, cellValue); break;
            case Act::Stereo:    ok = cli.set_stereo(kind, csrc, cellValue != 0); break;
            // Two crosspoints of the SAME source, onto the two halves of the output pair.
            case Act::Pan:       ok = cli.set_mixer(kind, csrc, cdst, panL)
                                   && cli.set_mixer(kind, csrc,
                                                    (uint16_t)uf::MixerModel::partner(cdst), panR);
                                 break;
            case Act::Submix:    ok = cellValue ? cli.clear_submix(cdst)
                                                : cli.copy_submix(csrc, cdst); break;
            case Act::Raw:       break;
        }
        if (!ok) { std::fprintf(stderr, "uf-mix: send failed\n"); return 1; }
        std::printf("sent\n");
        return 0;
    }

    // `raw` is the probing escape hatch: an arbitrary address the daemon has no shadow for, so it
    // cannot be expressed as a command and cannot survive a session restart. Direct write, and it
    // shares the AT context with a running daemon — fine for a one-off probe, not for routine use.
    io_connect_t conn = open_dext();
    if (!conn) { std::fprintf(stderr, "dext user client not available — is the FF800 connected "
                                      "and UFFireWireOHCI activated?\n"); return 1; }

    uint64_t in[2] = {addr, value};
    kern_return_t kr = IOConnectCallScalarMethod(conn, kUFOhciWriteQuadlet, in, 2, nullptr, nullptr);
    IOServiceClose(conn);
    if (kr != KERN_SUCCESS) { std::fprintf(stderr, "write failed: 0x%x\n", kr); return 1; }
    std::printf("written (raw, direct — will NOT survive a session restart)\n");
    return 0;
}
