// clock.hpp — FF800 clock-config register (0x801c0004) assembly (portable).
//
// The clock can be driven via two overlapping surfaces (spec/02 §2.2/§2.3, §2.5):
//   1. the streaming/clock surface — the clock-config register 0x801c0004: mode bit 0x1,
//      source field 0x1c00, rate-code field 0x1e (rate code from rate.hpp);
//   2. the CR2 settings block (settings.hpp assemble_cr2) — clock_master + SYNC_REF bits.
// This module builds the register-1 value; the CR2 mirror lives in settings.hpp. The two are kept
// consistent behind one setter that updates the SettingsShadow and, if the clock surface is
// used, writes this value. NOTE the source encodings differ: the clock-config source field puts
// TCO at 0x1800, whereas CR2 SYNC_REF puts TCO at 0x1400 (spec/02 §2.3 vs spec/06) — do not share
// the mapping. The 0x801c0004-vs-CR2 overlap is a [?] to confirm on hardware.
#pragma once
#include "uf/protocol/endian.hpp"
#include "uf/protocol/rate.hpp"
#include "uf/protocol/registers.hpp"
#include "uf/protocol/settings.hpp"

namespace uf {

// Clock-config register source-field bits (mask clockcfg::kSourceMask) for a sync reference.
constexpr u32 clock_config_source_bits(SyncRef r) {
    switch (r) {
        case SyncRef::Adat1: return clockcfg::kSourceAdat1;
        case SyncRef::Adat2: return clockcfg::kSourceAdat2;
        case SyncRef::Spdif: return clockcfg::kSourceSpdif;
        case SyncRef::WordClock: return clockcfg::kSourceWordClock;
        case SyncRef::Tco: return clockcfg::kSourceTco;  // 0x1800 (differs from CR2's 0x1400)
    }
    return 0;
}

// Build the clock-config register (0x801c0004) value: master-mode bit + sync source + rate code.
// `rate_hz` is a supported rate (rate.hpp); its code occupies clockcfg::kRateMask. When slaved the
// device derives the rate from the incoming clock, but the field is still written for consistency.
constexpr u32 assemble_clock_config(bool master, SyncRef source, u32 rate_hz) {
    u32 v = 0;
    if (master) v |= clockcfg::kClockModeMaster;
    v |= clock_config_source_bits(source);
    const u32 code = clock_config_rate_code(rate_hz);
    if (code != 0xffffffffu) v |= (code & clockcfg::kRateMask);
    return v;
}

}  // namespace uf
