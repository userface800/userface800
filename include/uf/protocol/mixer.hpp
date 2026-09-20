// mixer.hpp — matrix mixer RAM addressing + coefficient math + mute/rec masks (portable).
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

// Which half of a destination's 0x100-byte block a coefficient lives in. Not a flat src x dest grid:
// physical inputs occupy the first 0x80, playback streams the second, so "which kind of source" is
// part of the address rather than a detail (spec/07 §7.2). The mixer's three rows are exactly this
// distinction — hardware inputs, software playback, hardware outputs (manual §25.2).
enum class MixerSrcKind : u16 { Input = 0, Playback = 1 };

// The coefficient is SIGNED: a NEGATIVE value inverts the phase by 180°. FFADO validates
// `abs(val) <= 0x10000` in set_hardware_mixergain and negates the value when a crosspoint carries
// FF_SWPARAM_MF_INVERTED; the matrix view draws such a crosspoint red (manual §26.2). So the
// magnitude is the gain and the sign is the phase — one quadlet carries both.
inline constexpr i32 kMixerMinCoeff = -static_cast<i32>(kMixerMax);

// Magnitude + phase -> the quadlet actually written, two's complement.
//
// The `magnitude == 0` special case is FFADO's, and is a hardware workaround rather than tidiness:
// on the FF800 a transition from 0 (-inf dB) to -1 (-90 dB) makes the device run the volume far UP
// before dropping to the set point about a tenth of a second later. Writing -1 instead of a negative
// zero (which is just 0, and would read back as "not inverted" anyway) keeps the crosspoint on the
// negative side and avoids crossing that boundary. Inaudible: -90 dB.
inline constexpr i32 signed_coeff(u32 magnitude, bool inverted) {
    if (magnitude > kMixerMax) magnitude = kMixerMax;
    if (!inverted) return static_cast<i32>(magnitude);
    return -static_cast<i32>(magnitude == 0 ? 1u : magnitude);
}

// The quadlet as the device stores it, and back. Separate names because a raw cast in the middle of
// addressing code reads like a mistake.
inline constexpr u32 coeff_quadlet(i32 coeff) { return static_cast<u32>(coeff); }
inline constexpr i32 quadlet_coeff(u32 q) { return static_cast<i32>(q); }
inline constexpr u32 coeff_magnitude(i32 coeff) {
    return static_cast<u32>(coeff < 0 ? -coeff : coeff);
}

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
