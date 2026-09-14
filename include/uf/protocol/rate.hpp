// rate.hpp — FF800 sample-rate encoding (portable).
//
// The streaming rate is written as raw Hz to STF (0xfc88f000) — the snd-fireface path
// (spec/02 §2.2, §3.6, §9.2). This module validates a rate, gives its 1x/2x/4x speed class
// (which drives downstream channel/packet sizing, spec/03), and maps it to/from the
// clock-config rate code (0x801c0004 mask 0x1e, spec/02 §2.3) used when reading back status.
//
// Note: the CR2 FREQ0/FREQ1/DSPEED/QSSPEED bits are left 0 by both working drivers (spec/06),
// so the settings block does NOT carry the rate — only STF does.
#pragma once
#include "uf/protocol/endian.hpp"
#include "uf/protocol/registers.hpp"

namespace uf {

// Streaming speed class (spec/02 §2.3, spec/03 §3.3). The value is the multiplier.
enum class Speed : u32 { X1 = 1, X2 = 2, X4 = 4 };

// One supported rate: Hz, its clock-config rate code (field within clockcfg::kRateMask), speed.
struct RateInfo {
    u32 hz;
    u32 clock_code;
    Speed speed;
};

// The nine FF800-supported rates (spec/02 §2.3). Order is arbitrary; look-ups scan the table.
inline constexpr RateInfo kRates[] = {
    {32000, 0x02, Speed::X1},  {44100, 0x00, Speed::X1},  {48000, 0x06, Speed::X1},
    {64000, 0x0a, Speed::X2},  {88200, 0x08, Speed::X2},  {96000, 0x0e, Speed::X2},
    {128000, 0x12, Speed::X4}, {176400, 0x10, Speed::X4}, {192000, 0x16, Speed::X4},
};

// Speed class from a raw rate, by the driver thresholds (spec/02 §2.3): <64k=1x, <128k=2x, else 4x.
// Defined for any positive Hz (used for sizing even if the exact rate isn't in kRates).
constexpr Speed speed_for_rate(u32 hz) {
    if (hz < 64000) return Speed::X1;
    if (hz < 128000) return Speed::X2;
    return Speed::X4;
}

constexpr u32 speed_multiplier(Speed s) { return static_cast<u32>(s); }

// Is this an exact FF800-supported rate?
constexpr bool is_supported_rate(u32 hz) {
    for (const auto& r : kRates)
        if (r.hz == hz) return true;
    return false;
}

// Clock-config rate code for a supported rate (already positioned within clockcfg::kRateMask);
// returns 0xffffffff for an unsupported rate.
constexpr u32 clock_config_rate_code(u32 hz) {
    for (const auto& r : kRates)
        if (r.hz == hz) return r.clock_code;
    return 0xffffffff;
}

// Reverse: Hz for a clock-config rate code (the low field, masked with clockcfg::kRateMask);
// returns 0 if the code is not a known rate.
constexpr u32 hz_for_clock_code(u32 code) {
    const u32 field = code & clockcfg::kRateMask;
    for (const auto& r : kRates)
        if (r.clock_code == field) return r.hz;
    return 0;
}

// The value to write to STF (0xfc88f000) to select this rate: raw Hz (spec/09 §9.2).
constexpr u32 stf_value(u32 hz) { return hz; }

}  // namespace uf
