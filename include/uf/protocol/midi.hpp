// midi.hpp — FF800 MIDI framing over async transactions (portable).
//
// FF800 MIDI is async, separate from the audio stream (spec/04). Framing (both directions): one
// MIDI byte per 32-bit little-endian quadlet, byte in the low 8 bits, no header. OUT (host->device)
// block-writes up to 9 byte-quadlets to 0x80180000. Facts
// adapted from snd-fireface ff-transaction.c / former_fill_midi_msg (GPL-2.0). Validated against
// spec/09 §9.7.
#pragma once
#include <vector>
#include "uf/protocol/endian.hpp"
#include "uf/protocol/registers.hpp"

namespace uf {

// Max MIDI bytes per async transaction (snd-fireface SND_FF_MAXIMIM_MIDI_QUADS).
inline constexpr u32 kMidiMaxQuads = 9;
inline constexpr Addr kMidiOutAddr = reg::kMidiOut;  // 0x80180000
inline constexpr u32 kMidiBaud = 31250;              // DIN-MIDI bit rate

// Expand MIDI bytes into host-order quadlets (one byte per quadlet, value = byte). Caps at
// kMidiMaxQuads bytes per call (spec/04 §4.2); the caller loops for longer streams. The host writes
// these to kMidiOutAddr — a quadlet-write when there is one, else a block-write (like snd-fireface).
inline std::vector<u32> midi_out_quadlets(const u8* bytes, size_t n) {
    if (n > kMidiMaxQuads) n = kMidiMaxQuads;
    std::vector<u32> quads;
    quads.reserve(n);
    for (size_t i = 0; i < n; ++i) quads.push_back(static_cast<u32>(bytes[i]));
    return quads;
}

// Minimum spacing (ns) before the next MIDI-out transaction, paced at the 31250-baud MIDI rate:
// nbytes * 8 bits * (1e9 / 31250) ns/bit (snd-fireface next_ktime throttle, spec/04 §4.2).
constexpr u64 midi_throttle_ns(u32 nbytes) {
    return static_cast<u64>(nbytes) * 8u * (1000000000ull / kMidiBaud);
}

// ── MIDI IN (device->host; spec/04 §4.3) ──────────────────────────────────────────────────────
inline constexpr Addr kMidiHighAddr = reg::kMidiHighAddr;  // 0x200000320
inline constexpr u32 kMidiAddrRange = 12;                  // host receive region size (bytes)

// Value to write to the MIDI host-receive register to advertise a host-local address to the device
// (spec/04 §4.3, spec/09 §9.8): (node_id << 16) | high_index, where the host region's low 32 bits
// are 0 and high_index = region_offset >> 32. The device then async-writes incoming MIDI there.
constexpr u32 midi_in_register_value(u16 node_id, u16 high_index) {
    return (static_cast<u32>(node_id) << 16) | high_index;
}

// Value that clears the MIDI-in registration (stops the device's async writes), spec/04 §4.4.
constexpr u32 midi_in_unregister_value() { return 0; }

// Decode one incoming MIDI quadlet (device write) to its MIDI byte (spec/04 §4.3): low 8 bits.
constexpr u8 midi_in_byte(u32 quad) { return static_cast<u8>(quad & 0xFFu); }

// Decode a block of incoming MIDI quadlets (little-endian on the wire) into MIDI bytes.
inline std::vector<u8> midi_in_bytes(const u8* payload, size_t len_bytes) {
    std::vector<u8> out;
    out.reserve(len_bytes / 4);
    for (size_t i = 0; i + 4 <= len_bytes; i += 4) out.push_back(midi_in_byte(le32_to_host(payload + i)));
    return out;
}

}  // namespace uf
