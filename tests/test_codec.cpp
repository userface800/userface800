// Capture packet decode (spec/03 §3.4, spec/09 §9.5).
#include <vector>
#include "uf/protocol/codec.hpp"
#include "uf/protocol/channels.hpp"
#include "uf_test.hpp"

using namespace uf;

UF_TEST(decode_sample_positive) {
    // Top 24 bits are the sample; low byte housekeeping is discarded.
    UF_CHECK_EQ(decode_sample(0x12345678), 0x123456);
    UF_CHECK_EQ(decode_sample(0x000000FF), 0);          // only housekeeping -> 0
    UF_CHECK_EQ(decode_sample(0x7FFFFF00), 0x7FFFFF);    // max positive 24-bit
}

UF_TEST(decode_sample_negative) {
    // spec/09 §9.5: sign-extend from bit 23 (quad & 0x80000000).
    UF_CHECK_EQ(decode_sample(0xFF800000), -32768);      // 0xFFFF8000
    UF_CHECK_EQ(decode_sample(0xFFFFFF00), -1);          // all-ones 24-bit
    UF_CHECK_EQ(decode_sample(0x80000000), -8388608);    // min negative 24-bit (-2^23)
}

UF_TEST(decode_sample_le_buffer) {
    // Wire bytes LE: 0x78 0x56 0x34 0x12 -> host 0x12345678 -> sample 0x123456.
    u8 buf[4] = {0x78, 0x56, 0x34, 0x12};
    UF_CHECK_EQ(decode_sample_le(buf), 0x123456);
}

UF_TEST(frames_in_payload_math) {
    // spec/09 §9.5: 784 bytes / (28*4) = 7 frames at 1x.
    UF_CHECK_EQ(frames_in_payload(784, 28), 7u);
    UF_CHECK_EQ(frames_in_payload(1200, 20), 15u);  // 2x
    UF_CHECK_EQ(frames_in_payload(1200, 12), 25u);  // 4x
    UF_CHECK_EQ(frames_in_payload(100, 0), 0u);     // guard
}

UF_TEST(decode_packet_roundtrip) {
    // Build a 2-frame, 28-channel packet where channel c of frame f holds sample (f*100 + c) in the
    // top 24 bits, then decode and check indexing.
    const u32 nch = pcm_channels(Speed::X1);  // 28
    const u32 nframes = 2;
    std::vector<u8> payload(nframes * nch * 4, 0);
    for (u32 f = 0; f < nframes; ++f)
        for (u32 c = 0; c < nch; ++c) {
            i32 sample = static_cast<i32>(f * 100 + c);
            host_to_le32(static_cast<u32>(sample) << 8, &payload[(f * nch + c) * 4]);
        }
    std::vector<i32> out(nframes * nch, 0);
    u32 got = decode_packet(payload.data(), static_cast<u32>(payload.size()), nch, out.data());
    UF_CHECK_EQ(got, nframes);
    UF_CHECK_EQ(out[0 * nch + 0], 0);
    UF_CHECK_EQ(out[0 * nch + 5], 5);
    UF_CHECK_EQ(out[1 * nch + 0], 100);
    UF_CHECK_EQ(out[1 * nch + 27], 127);
}

// ── Playback encode ─────────────────────────────────────────────────────────────────────
UF_TEST(encode_sample_placement) {
    // 24-bit sample goes in bits [31:8], low byte 0 (spec/03 §3.4).
    UF_CHECK_EQ(encode_sample(0x123456), 0x12345600u);
    UF_CHECK_EQ(encode_sample(0), 0u);
    UF_CHECK_EQ(encode_sample(-1), 0xFFFFFF00u);        // -1 (24-bit) -> top 24 bits all ones
    UF_CHECK_EQ(encode_sample(-8388608), 0x80000000u);  // -2^23
}

UF_TEST(encode_sample_le_buffer) {
    u8 buf[4] = {0xAA, 0xAA, 0xAA, 0xAA};
    encode_sample_le(0x123456, buf);
    // host 0x12345600 -> LE bytes 00 56 34 12.
    UF_CHECK_EQ(buf[0], 0x00);
    UF_CHECK_EQ(buf[1], 0x56);
    UF_CHECK_EQ(buf[2], 0x34);
    UF_CHECK_EQ(buf[3], 0x12);
}

UF_TEST(encode_decode_roundtrip) {
    // Every 24-bit audio value should survive encode -> decode (housekeeping byte aside).
    const i32 vals[] = {0, 1, -1, 0x7FFFFF, -8388608, 0x123456, -1234567};
    for (i32 v : vals) {
        u8 q[4];
        encode_sample_le(v, q);
        UF_CHECK_EQ(decode_sample_le(q), v);
    }
}

UF_TEST(encode_packet_and_silence) {
    const u32 nch = pcm_channels(Speed::X1);  // 28
    const u32 nframes = frames_per_packet(Speed::X1);  // 7
    std::vector<i32> in(nframes * nch);
    for (u32 f = 0; f < nframes; ++f)
        for (u32 c = 0; c < nch; ++c) in[f * nch + c] = static_cast<i32>(f) - static_cast<i32>(c);
    std::vector<u8> payload(nframes * nch * 4, 0xFF);
    u32 bytes = encode_packet(in.data(), nframes, nch, payload.data());
    UF_CHECK_EQ(bytes, packet_payload_bytes(Speed::X1));  // 784
    // Decode back and compare.
    std::vector<i32> out(nframes * nch, 0);
    decode_packet(payload.data(), bytes, nch, out.data());
    UF_CHECK(in == out);
    // Silence frame is all zero.
    std::vector<u8> sf(nch * 4, 0x55);
    encode_silence_frame(nch, sf.data());
    for (u8 b : sf) UF_CHECK_EQ(b, 0);
}

UF_TEST_MAIN()
