// MIDI out framing (spec/04 §4.2, spec/09 §9.7).
#include <vector>
#include "uf/protocol/midi.hpp"
#include "uf/protocol/endian.hpp"
#include "uf_test.hpp"

using namespace uf;

UF_TEST(midi_out_note_on_9_7) {
    // spec/09 §9.7: Note On ch1, note 60, vel 127 -> quadlets [0x90, 0x3C, 0x7F].
    u8 msg[3] = {0x90, 0x3C, 0x7F};
    auto q = midi_out_quadlets(msg, 3);
    UF_CHECK_EQ(q.size(), static_cast<size_t>(3));
    UF_CHECK_EQ(q[0], 0x90u);
    UF_CHECK_EQ(q[1], 0x3Cu);
    UF_CHECK_EQ(q[2], 0x7Fu);
    UF_CHECK_EQ(kMidiOutAddr, 0x80180000ull);
    // Wire serialization: each quadlet LE puts the MIDI byte in the low byte (90 00 00 00 ...).
    u8 wire[4];
    host_to_le32(q[0], wire);
    UF_CHECK_EQ(wire[0], 0x90);
    UF_CHECK_EQ(wire[1], 0x00);
}

UF_TEST(midi_out_caps_at_9) {
    u8 msg[16];
    for (int i = 0; i < 16; ++i) msg[i] = static_cast<u8>(i + 1);
    auto q = midi_out_quadlets(msg, 16);
    UF_CHECK_EQ(q.size(), static_cast<size_t>(9));  // kMidiMaxQuads
    UF_CHECK_EQ(q[8], 9u);
    // Empty input -> empty block.
    UF_CHECK_EQ(midi_out_quadlets(msg, 0).size(), static_cast<size_t>(0));
}

UF_TEST(midi_throttle) {
    // 1 byte at 31250 baud = 8 bits * 32000 ns/bit = 256000 ns.
    UF_CHECK_EQ(midi_throttle_ns(1), 256000ull);
    UF_CHECK_EQ(midi_throttle_ns(3), 768000ull);
    UF_CHECK_EQ(midi_throttle_ns(0), 0ull);
}

// ── MIDI in ─────────────────────────────────────────────────────────────────────────────
UF_TEST(midi_in_register_9_8) {
    // spec/09 §9.8: wq(0x200000320, (node_id<<16) | 0x0001) for region offset 0x0001_00000000.
    UF_CHECK_EQ(kMidiHighAddr, 0x200000320ull);
    UF_CHECK_EQ(midi_in_register_value(0xffc0, 0x0001), 0xffc00001u);
    UF_CHECK_EQ(midi_in_register_value(0x0000, 0x0001), 0x00000001u);
    UF_CHECK_EQ(midi_in_unregister_value(), 0u);
    UF_CHECK_EQ(kMidiAddrRange, 12u);
}

UF_TEST(midi_in_decode) {
    UF_CHECK_EQ(midi_in_byte(0x00000090u), 0x90);
    UF_CHECK_EQ(midi_in_byte(0xdeadbe3Cu), 0x3C);  // upper 24 bits ignored
    // A device write of three MIDI bytes (LE quadlets on the wire): 90 .. , 3C .. , 7F ..
    u8 wire[12];
    host_to_le32(0x90, wire + 0);
    host_to_le32(0x3C, wire + 4);
    host_to_le32(0x7F, wire + 8);
    auto bytes = midi_in_bytes(wire, sizeof(wire));
    UF_CHECK_EQ(bytes.size(), static_cast<size_t>(3));
    UF_CHECK_EQ(bytes[0], 0x90);
    UF_CHECK_EQ(bytes[1], 0x3C);
    UF_CHECK_EQ(bytes[2], 0x7F);
}

UF_TEST_MAIN()
