// channels.hpp — FF800 isoch packet geometry (portable).
//
// Channel counts, frames-per-packet, packet payload size, data-block-quadlets and bus bandwidth,
// parameterized by speed (rate.hpp) and bandwidth-limit mode. Facts from spec/03 §3.2/§3.3/§3.7,
// spec/08. Validated against spec/09 §9.4/§9.5. This replaces the base isoch engine's hard-coded
// AMDTP-stereo sizing (the engine edits themselves are host).
#pragma once
#include <string>
#include <vector>
#include "uf/protocol/endian.hpp"
#include "uf/protocol/rate.hpp"

namespace uf {

// Bandwidth-limit modes (spec/08 §8.3), applied before the speed-based ADAT reduction.
enum class BwLimit {
    SendAll,       // analog 10 + SPDIF 2 + ADAT 16 (default)
    NoAdat2,       // ADAT1 only (8)
    AnalogSpdif,   // analog + SPDIF, no ADAT
    AnalogOnly,    // analog channels 1-8 only
};

// Frames (data blocks) per isochronous packet by speed (spec/03 §3.2): 1x=7, 2x=15, 4x=25.
constexpr u32 frames_per_packet(Speed s) {
    switch (s) {
        case Speed::X1: return 7;
        case Speed::X2: return 15;
        case Speed::X4: return 25;
    }
    return 0;
}

// Analog channel count for a mode (spec/08 §8.3): AnalogOnly = 8 (channels 1-8), else 10.
constexpr u32 analog_channels(BwLimit m) { return m == BwLimit::AnalogOnly ? 8 : 10; }

// SPDIF channel count: 0 for AnalogOnly, else 2.
constexpr u32 spdif_channels(BwLimit m) { return m == BwLimit::AnalogOnly ? 0 : 2; }

// ADAT channel count for (mode, speed). 1x base: SendAll=16, NoAdat2=8, else 0; halved at 2x,
// zeroed at 4x (spec/08 §8.3).
constexpr u32 adat_channels(BwLimit m, Speed s) {
    u32 base = 0;
    if (m == BwLimit::SendAll) base = 16;
    else if (m == BwLimit::NoAdat2) base = 8;
    switch (s) {
        case Speed::X1: return base;
        case Speed::X2: return base / 2;
        case Speed::X4: return 0;
    }
    return 0;
}

// Total PCM channels = data_block_quadlets for the active (mode, speed). Same for capture/playback
// (spec/08 §8.2). Defaults to the send-all mode.
constexpr u32 pcm_channels(Speed s, BwLimit m = BwLimit::SendAll) {
    return analog_channels(m) + spdif_channels(m) + adat_channels(m, s);
}

// Convenience: PCM channels straight from a rate.
constexpr u32 pcm_channels_for_rate(u32 hz, BwLimit m = BwLimit::SendAll) {
    return pcm_channels(speed_for_rate(hz), m);
}

// data_block_quadlets written to RX_PACKET_FORMAT / ALLOC_TX_STREAM (spec/03 §3.6) = pcm_channels.
constexpr u32 data_block_quadlets(Speed s, BwLimit m = BwLimit::SendAll) { return pcm_channels(s, m); }

// Isoch packet audio payload in bytes = pcm_channels * 4 * frames_per_packet (spec/03 §3.2).
constexpr u32 packet_payload_bytes(Speed s, BwLimit m = BwLimit::SendAll) {
    return pcm_channels(s, m) * 4 * frames_per_packet(s);
}

// Bus bandwidth allocation units for the playback (rx) stream (spec/03 §3.7):
// 25 + pcm_channels * 4 * frames_per_packet (S400: 1 unit = 1 byte; +25 protocol overhead).
constexpr u32 bandwidth_units(Speed s, BwLimit m = BwLimit::SendAll) {
    return 25 + packet_payload_bytes(s, m);
}

// ── Channel/frame ordering (spec/08 §8.2, spec/03 §3.5) ────────────────────────────────────────
enum class Direction { Capture, Playback };
enum class ChannelKind { Analog, Phones, Spdif, Adat };

// One frame slot: its channel kind and 1-based index within that kind (Phones: 1=L, 2=R).
struct ChannelSlot {
    ChannelKind kind;
    u32 index;
};

// The slot->channel map for a direction/speed/mode: index in the vector == frame slot (quadlet)
// index. Order: analog 1..N -> [phones L,R on playback | analog 9,10 on capture] -> SPDIF 1,2 ->
// ADAT 1..N (spec/08 §8.2). Size == pcm_channels(speed, mode).
inline std::vector<ChannelSlot> channel_map(Direction dir, Speed s, BwLimit m = BwLimit::SendAll) {
    std::vector<ChannelSlot> v;
    const u32 an = analog_channels(m);  // 8 (analog-only) or 10
    for (u32 i = 1; i <= 8 && i <= an; ++i) v.push_back({ChannelKind::Analog, i});
    if (an >= 10) {
        // Slots 8-9: capture exposes analog 9/10; playback reassigns them to phones L/R.
        if (dir == Direction::Capture) {
            v.push_back({ChannelKind::Analog, 9});
            v.push_back({ChannelKind::Analog, 10});
        } else {
            v.push_back({ChannelKind::Phones, 1});
            v.push_back({ChannelKind::Phones, 2});
        }
    }
    for (u32 i = 1; i <= spdif_channels(m); ++i) v.push_back({ChannelKind::Spdif, i});
    for (u32 i = 1; i <= adat_channels(m, s); ++i) v.push_back({ChannelKind::Adat, i});
    return v;
}

// Human-readable channel name for CoreAudio naming.
inline std::string channel_name(ChannelSlot sl) {
    switch (sl.kind) {
        case ChannelKind::Analog: return "Analog " + std::to_string(sl.index);
        case ChannelKind::Phones: return sl.index == 1 ? "Phones L" : "Phones R";
        case ChannelKind::Spdif: return "SPDIF " + std::to_string(sl.index);
        case ChannelKind::Adat: return "ADAT " + std::to_string(sl.index);
    }
    return "?";
}

}  // namespace uf
