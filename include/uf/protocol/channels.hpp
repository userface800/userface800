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
// Frames in a FULL packet. The FF800 streams in CIP blocking mode (ref/snd-fireface amdtp-ff.c:
// CIP_BLOCKING|CIP_UNAWARE_SYT|CIP_NO_HEADER), so each packet carries exactly this many frames or 0
// (empty), and empty packets pad down to the rate/8000 average. This is the AMDTP syt_interval,
// [HW-CONFIRMED] on the FF800 (a 48 kHz capture packet = 8 frames = 896 B/28ch). Corrects spec/03's
// 7/15/25.
constexpr u32 frames_per_packet(Speed s) {
    switch (s) {
        case Speed::X1: return 8;    // 32/44.1/48 kHz
        case Speed::X2: return 16;   // 88.2/96 kHz
        case Speed::X4: return 32;   // 176.4/192 kHz
    }
    return 0;
}

// Average frames per isochronous cycle (8000 cycles/s) — integer for the 48 k family, fractional for
// the 44.1 k family (44100/8000 = 5.5125), which is why the blocking cadence needs a whole period.
constexpr u32 avg_frames_per_cycle(u32 rate_hz) { return rate_hz / 8000; }

// The blocking transmit cadence repeats over this many packets. 640 is the exact period for EVERY
// FF800 native rate: rate*640/(8000*syt_interval) is a whole number of full packets for 32/44.1/48/
// 88.2/96/176.4/192 kHz (the 48 k family's period is 4, which divides 640). So a 640-packet ring
// reproduces 44.1 kHz exactly (5.5125 fr/cyc), not just the clean rates.
inline constexpr u32 kBlockingRingLen = 640;

// Number of FULL (syt_interval-frame) packets in one 640-packet ring for this rate; the rest are empty.
constexpr u32 blocking_full_count(u32 rate_hz, Speed s) {
    return rate_hz * kBlockingRingLen / (8000u * frames_per_packet(s));
}

// Is packet `i` of the ring a full packet? Bresenham distribution of `full_count` full packets across
// `ring_len` — spreads them evenly so the device's buffer never sees a long run of empties. The dext
// (programming descriptors) and the daemon (filling only full slots) MUST agree via this one function.
constexpr bool is_full_packet(u32 i, u32 full_count, u32 ring_len = kBlockingRingLen) {
    return (i + 1) * full_count / ring_len > i * full_count / ring_len;
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
//
// These are the channel names the device ships with, verbatim. Matching them exactly matters more
// than picking nicer words: a DAW session saved against the factory driver remembers its inputs by
// name, so "Mic 9" and "Analog 9" are the difference between a project re-opening with its routing
// intact and the user rebuilding it by hand.
//
// The two that are not what you would guess: input slots 9/10 are the FF800's mic inputs ("Mic 9",
// "Mic 10") while the same slots on output are the headphone sends ("Phones 9", "Phones 10", NOT
// L/R) — which is exactly the capture/playback split channel_map() already encodes. SPDIF is L/R
// rather than numbered.
inline std::string channel_name(ChannelSlot sl) {
    switch (sl.kind) {
        case ChannelKind::Analog:
            return (sl.index >= 9 ? "Mic " : "Analog ") + std::to_string(sl.index);
        case ChannelKind::Phones: return "Phones " + std::to_string(sl.index + 8);
        case ChannelKind::Spdif: return sl.index == 1 ? "SPDIF L" : "SPDIF R";
        case ChannelKind::Adat: return "ADAT " + std::to_string(sl.index);
    }
    return "?";
}

// The group a channel belongs to, for CoreAudio's element *category* name — the label Audio MIDI
// Setup puts above a block of channels. The factory driver shows four per direction, because it
// publishes one stream per group: Analog 1-8, Phones/Mic 9-10, SPDIF, ADAT.
//
// These are also the boundaries along which channels disappear as the rate rises — ADAT halves at 2x
// and vanishes at 4x, leaving the other three groups untouched — which is why the FF800 is a 24-channel
// device in practice (8 analog + 2x8 ADAT) with four channels of phones/SPDIF always along for the ride.
inline std::string channel_group_name(ChannelSlot sl) {
    switch (sl.kind) {
        case ChannelKind::Analog: return sl.index >= 9 ? "Mic" : "Analog";
        case ChannelKind::Phones: return "Phones";
        case ChannelKind::Spdif: return "SPDIF";
        case ChannelKind::Adat: return "ADAT";
    }
    return "?";
}

// A contiguous run of channels of one kind, as CoreAudio should see it: one stream per group, which
// is what the factory driver publishes too — one stream per (start, count) pair below.
struct ChannelGroup {
    std::string name;
    u32 start;   // 1-based channel number of the first channel in the group
    u32 count;
};

// Split a channel map into its groups. Boundaries are kind changes, plus the analog 1-8 / 9-10 split
// that RME shows separately (mic ins on capture, phones on playback). These are also the boundaries
// channels disappear along as the rate rises — ADAT halves at 2x and is gone at 4x — so a speed
// change resizes or removes whole groups instead of renumbering a flat list.
inline std::vector<ChannelGroup> channel_groups(Direction dir, Speed s,
                                                BwLimit m = BwLimit::SendAll) {
    const auto map = channel_map(dir, s, m);
    std::vector<ChannelGroup> groups;
    for (u32 i = 0; i < map.size(); ++i) {
        const std::string g = channel_group_name(map[i]);
        if (!groups.empty() && groups.back().name == g) {
            ++groups.back().count;
        } else {
            groups.push_back({g, i + 1, 1});
        }
    }
    return groups;
}

}  // namespace uf
