// wav.hpp — canonical PCM WAV serialization for captured audio (portable).
//
// The capture path decodes device packets to frame-major PCM (uf::decode_packet) and
// drains it to a file. The WAV *serialization* is pure logic and lives here (unit-tested); wiring
// the DCL/ring that feeds it is host. FF800 audio is native 24-bit,
// so this writes 24-bit PCM by default (spec/03 §3.4). Input is frame-major i32 (== WAV interleave
// order): sample[frame * n_channels + channel], each a sign-extended 24-bit value.
#pragma once
#include <cstddef>
#include <vector>
#include "uf/protocol/endian.hpp"

namespace uf {

namespace detail {
inline void push_u16(std::vector<u8>& b, u16 v) {
    b.push_back(static_cast<u8>(v & 0xff));
    b.push_back(static_cast<u8>((v >> 8) & 0xff));
}
inline void push_u32(std::vector<u8>& b, u32 v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>((v >> (8 * i)) & 0xff));
}
inline void push_tag(std::vector<u8>& b, const char (&t)[5]) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(t[i]));
}
}  // namespace detail

// The canonical 44-byte PCM WAV header size.
inline constexpr size_t kWavHeaderSize = 44;

// Build the 44-byte canonical WAV header for `data_bytes` of PCM (spec: RIFF/WAVE/fmt /data).
inline std::vector<u8> wav_header(u32 sample_rate, u16 n_channels, u16 bits_per_sample,
                                  u32 data_bytes) {
    const u16 bytes_per_sample = static_cast<u16>(bits_per_sample / 8);
    const u16 block_align = static_cast<u16>(n_channels * bytes_per_sample);
    const u32 byte_rate = sample_rate * block_align;
    std::vector<u8> h;
    h.reserve(kWavHeaderSize);
    detail::push_tag(h, "RIFF");
    detail::push_u32(h, 36u + data_bytes);   // RIFF chunk size = 36 + data
    detail::push_tag(h, "WAVE");
    detail::push_tag(h, "fmt ");
    detail::push_u32(h, 16u);                // PCM fmt chunk size
    detail::push_u16(h, 1u);                 // audio format = PCM
    detail::push_u16(h, n_channels);
    detail::push_u32(h, sample_rate);
    detail::push_u32(h, byte_rate);
    detail::push_u16(h, block_align);
    detail::push_u16(h, bits_per_sample);
    detail::push_tag(h, "data");
    detail::push_u32(h, data_bytes);
    return h;
}

// Append one sample's low `bytes_per_sample` bytes, little-endian (24-bit: low 3 bytes of the
// sign-extended value — the exact top-24-bits payload decode_sample produced).
inline void push_pcm_sample(std::vector<u8>& b, i32 sample, u16 bytes_per_sample) {
    u32 v = static_cast<u32>(sample);
    for (u16 i = 0; i < bytes_per_sample; ++i) b.push_back(static_cast<u8>((v >> (8 * i)) & 0xff));
}

// Serialize frame-major PCM (`samples`, length n_frames * n_channels) to a complete WAV byte stream.
inline std::vector<u8> write_wav(const std::vector<i32>& samples, u32 sample_rate, u16 n_channels,
                                 u16 bits_per_sample = 24) {
    const u16 bytes_per_sample = static_cast<u16>(bits_per_sample / 8);
    const u32 data_bytes = static_cast<u32>(samples.size()) * bytes_per_sample;
    std::vector<u8> out = wav_header(sample_rate, n_channels, bits_per_sample, data_bytes);
    out.reserve(out.size() + data_bytes);
    for (i32 s : samples) push_pcm_sample(out, s, bytes_per_sample);
    return out;
}

}  // namespace uf
