// mixer.hpp — TotalMix matrix RAM addressing + coefficient math + mute/rec masks (portable).
//
// The mixer is optional (zero-latency monitoring only); snd-fireface doesn't implement it. Facts
// from spec/07 (transcribed from FFADO fireface_hw.cpp::set_hardware_mixergain, GPL-2.0). Validated
// against spec/09 §9.6. Addressing is integer/constexpr; the dB<->coefficient conversion is runtime
// (uses std::pow/std::log10).
#pragma once
#include <array>
#include <cmath>
#include "uf/protocol/endian.hpp"
#include "uf/protocol/registers.hpp"

namespace uf {

// Per-output block: 0x80 bytes hold a destination's input coefficients; the next 0x80 hold its
// playback coefficients, so each destination owns dest*0x100 (spec/07 §7.2).
inline constexpr u32 kMixerBlock = 0x80;
inline constexpr u32 kOutputFaderBase = 0x1f80;  // offset of the per-output fader row
inline constexpr u32 kMixerChannels = 28;        // FF800 mixer channels (block padded to 32)

// Coefficient encoding (spec/07 §7.1): 0 = mute, 0x8000 = 0 dB, 0x10000 = +6 dB.
inline constexpr u32 kMixerMute = 0x00000;
inline constexpr u32 kMixerUnity = 0x08000;  // 0 dB
inline constexpr u32 kMixerMax = 0x10000;    // +6 dB

// physical input `src` -> output `dest` coefficient address (spec/07 §7.2).
constexpr Addr input_coeff_addr(u32 src, u32 dest) {
    return reg::kMixerRam + dest * 2 * kMixerBlock + 4 * src;
}
// playback stream `src` -> output `dest` coefficient address.
constexpr Addr playback_coeff_addr(u32 src, u32 dest) {
    return reg::kMixerRam + dest * 2 * kMixerBlock + kMixerBlock + 4 * src;
}
// per-output fader address (`out` = output index).
constexpr Addr output_fader_addr(u32 out) {
    return reg::kMixerRam + kOutputFaderBase + 4 * out;
}

// dB -> coefficient: val = round(32768 * 10^(dB/20)), clamped to [0, 0x10000] (spec/07 §7.1).
inline u32 db_to_coeff(double db) {
    double v = 32768.0 * std::pow(10.0, db / 20.0);
    long r = std::lround(v);
    if (r < 0) r = 0;
    if (r > static_cast<long>(kMixerMax)) r = static_cast<long>(kMixerMax);
    return static_cast<u32>(r);
}
// coefficient -> dB (spec/07 §7.1). c == 0 (mute) returns -inf.
inline double coeff_to_db(u32 c) { return 20.0 * std::log10(static_cast<double>(c) / 32768.0); }

// Channel-mute mask (block-write to 0x801c0000) and output-record mask (0x801c0080): 28 quadlets,
// one 0/1 flag per channel (spec/07 §7.3; FFADO writes 28 even on FF400). `on` sets every channel.
constexpr std::array<u32, kMixerChannels> mask_all(bool on) {
    std::array<u32, kMixerChannels> m{};
    for (auto& q : m) q = on ? 1u : 0u;
    return m;
}

}  // namespace uf
