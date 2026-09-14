// WAV serialization (portable part).
#include <vector>
#include "uf/protocol/wav.hpp"
#include "uf/protocol/codec.hpp"
#include "uf_test.hpp"

using namespace uf;

static u32 rd_u32(const std::vector<u8>& b, size_t off) {
    return static_cast<u32>(b[off]) | (static_cast<u32>(b[off + 1]) << 8) |
           (static_cast<u32>(b[off + 2]) << 16) | (static_cast<u32>(b[off + 3]) << 24);
}
static u16 rd_u16(const std::vector<u8>& b, size_t off) {
    return static_cast<u16>(b[off] | (b[off + 1] << 8));
}
static bool tag(const std::vector<u8>& b, size_t off, const char* t) {
    for (int i = 0; i < 4; ++i) if (b[off + i] != static_cast<u8>(t[i])) return false;
    return true;
}

UF_TEST(header_fields) {
    // 2 ch, 48k, 24-bit, 10 frames -> data = 10*2*3 = 60 bytes.
    auto h = wav_header(48000, 2, 24, 60);
    UF_CHECK_EQ(h.size(), kWavHeaderSize);
    UF_CHECK(tag(h, 0, "RIFF"));
    UF_CHECK_EQ(rd_u32(h, 4), 36u + 60u);
    UF_CHECK(tag(h, 8, "WAVE"));
    UF_CHECK(tag(h, 12, "fmt "));
    UF_CHECK_EQ(rd_u32(h, 16), 16u);      // fmt size
    UF_CHECK_EQ(rd_u16(h, 20), 1u);       // PCM
    UF_CHECK_EQ(rd_u16(h, 22), 2u);       // channels
    UF_CHECK_EQ(rd_u32(h, 24), 48000u);   // sample rate
    UF_CHECK_EQ(rd_u32(h, 28), 48000u * 2u * 3u);  // byte rate
    UF_CHECK_EQ(rd_u16(h, 32), 2u * 3u);  // block align
    UF_CHECK_EQ(rd_u16(h, 34), 24u);      // bits
    UF_CHECK(tag(h, 36, "data"));
    UF_CHECK_EQ(rd_u32(h, 40), 60u);      // data size
}

UF_TEST(pcm_24bit_little_endian) {
    // Frame-major samples -> 24-bit LE data after the 44-byte header.
    std::vector<i32> s = {0x123456, -1, 0x7FFFFF, -8388608};  // 4 mono samples
    auto w = write_wav(s, 44100, 1, 24);
    UF_CHECK_EQ(w.size(), kWavHeaderSize + 4u * 3u);
    // 0x123456 -> bytes 56 34 12.
    UF_CHECK_EQ(w[44], 0x56);
    UF_CHECK_EQ(w[45], 0x34);
    UF_CHECK_EQ(w[46], 0x12);
    // -1 (24-bit) -> FF FF FF.
    UF_CHECK_EQ(w[47], 0xFF);
    UF_CHECK_EQ(w[48], 0xFF);
    UF_CHECK_EQ(w[49], 0xFF);
    // -8388608 (0x800000) -> 00 00 80.
    UF_CHECK_EQ(w[53], 0x00);
    UF_CHECK_EQ(w[54], 0x00);
    UF_CHECK_EQ(w[55], 0x80);
}

UF_TEST(decode_to_wav_pipeline) {
    // A decoded packet's frame-major output feeds write_wav directly (same interleave order).
    const u16 nch = 2;
    const u32 nframes = 3;
    std::vector<u8> payload(nframes * nch * 4, 0);
    for (u32 f = 0; f < nframes; ++f)
        for (u32 c = 0; c < nch; ++c)
            host_to_le32(static_cast<u32>((f + 1) * 0x010000 + c) << 8, &payload[(f * nch + c) * 4]);
    std::vector<i32> pcm(nframes * nch);
    decode_packet(payload.data(), static_cast<u32>(payload.size()), nch, pcm.data());
    auto w = write_wav(pcm, 48000, nch, 24);
    UF_CHECK_EQ(rd_u16(w, 22), 2u);
    UF_CHECK_EQ(w.size(), kWavHeaderSize + nframes * nch * 3u);
    // First sample 0x010000 -> LE 00 00 01.
    UF_CHECK_EQ(w[44], 0x00);
    UF_CHECK_EQ(w[45], 0x00);
    UF_CHECK_EQ(w[46], 0x01);
}

UF_TEST_MAIN()
