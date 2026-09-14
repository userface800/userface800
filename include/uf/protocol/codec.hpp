// codec.hpp — FF800 isochronous PCM packet codec (portable).
//
// Raw quadlet payload, no CIP header: a packet is N frames, a frame is `pcm_channels` little-endian
// quadlets, channel c at byte c*4 (spec/03 §3.2/§3.4/§3.5). Each quadlet carries 24-bit audio in
// its top 24 bits; the low byte is housekeeping/unused. Validated against spec/09 §9.5.
//
// Capture (device->host) decode and playback (host->device) encode both live here.
#pragma once
#include "uf/protocol/endian.hpp"

namespace uf {

// ── Sample codec (spec/03 §3.4) ──────────────────────────────────────────────────────────────
// Decode one host-order quadlet to a sign-extended 24-bit sample (in an i32, range [-2^23, 2^23)).
constexpr i32 decode_sample(u32 quad) {
    i32 s = static_cast<i32>((quad >> 8) & 0x00FFFFFF);
    if (quad & 0x80000000u) s |= static_cast<i32>(0xFF000000u);  // sign-extend bit 23
    return s;
}

// Decode one little-endian quadlet from a 4-byte buffer to a 24-bit sample.
constexpr i32 decode_sample_le(const u8* p) { return decode_sample(le32_to_host(p)); }

// ── Frame / packet decode ────────────────────────────────────────────────────────────────────
// Number of frames in a capture packet payload: payload_bytes / (pcm_channels * 4).
constexpr u32 frames_in_payload(u32 payload_bytes, u32 pcm_channels) {
    return pcm_channels ? payload_bytes / (pcm_channels * 4) : 0;
}

// Decode one frame (pcm_channels LE quadlets at `frame`) into out[0..pcm_channels).
constexpr void decode_frame(const u8* frame, u32 pcm_channels, i32* out) {
    for (u32 c = 0; c < pcm_channels; ++c) out[c] = decode_sample_le(frame + c * 4);
}

// Decode a whole capture packet payload (n_frames * pcm_channels quadlets) into `out`, laid out
// frame-major: out[f * pcm_channels + c]. Returns the number of frames decoded.
constexpr u32 decode_packet(const u8* payload, u32 payload_bytes, u32 pcm_channels, i32* out) {
    const u32 n = frames_in_payload(payload_bytes, pcm_channels);
    for (u32 f = 0; f < n; ++f) decode_frame(payload + f * pcm_channels * 4, pcm_channels, out + f * pcm_channels);
    return n;
}

// ── Sample / frame / packet encode (playback, host->device; spec/03 §3.4) ─────────────────────
// Encode a 24-bit sample into a host-order quadlet: sample in bits [31:8], low byte 0.
constexpr u32 encode_sample(i32 sample24) { return static_cast<u32>(sample24) << 8; }

// Encode a 24-bit sample as a little-endian quadlet into a 4-byte buffer.
constexpr void encode_sample_le(i32 sample24, u8* p) { host_to_le32(encode_sample(sample24), p); }

// Encode one frame (in[0..pcm_channels)) as pcm_channels LE quadlets at `frame`.
constexpr void encode_frame(const i32* in, u32 pcm_channels, u8* frame) {
    for (u32 c = 0; c < pcm_channels; ++c) encode_sample_le(in[c], frame + c * 4);
}

// Encode `n_frames` frames (frame-major in[f * pcm_channels + c]) into a packet payload buffer.
// Returns the payload byte count written (n_frames * pcm_channels * 4).
constexpr u32 encode_packet(const i32* in, u32 n_frames, u32 pcm_channels, u8* payload) {
    for (u32 f = 0; f < n_frames; ++f) encode_frame(in + f * pcm_channels, pcm_channels, payload + f * pcm_channels * 4);
    return n_frames * pcm_channels * 4;
}

// Fill a frame with digital silence (all channels 0) — used for underrun/partial-buffer padding.
constexpr void encode_silence_frame(u32 pcm_channels, u8* frame) {
    for (u32 c = 0; c < pcm_channels * 4; ++c) frame[c] = 0;
}

}  // namespace uf
