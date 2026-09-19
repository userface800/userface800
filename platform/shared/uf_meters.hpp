// uf_meters.hpp — per-channel peak and RMS, computed from the PCM the daemon already handles.
//
// The manual (§25.1) says every input, playback and output channel has a peak and RMS meter
// "calculated in hardware", and the device does expose a pollable meter region at 0x80100000
// (spec/10 §10.3). So why compute them here at all?
//
//   * The daemon already HAS every input and playback sample. Computing a peak and a sum of squares
//     over them is a few operations per sample and needs no FireWire traffic, no calibration
//     constant, and no round trip through a context shared with the register path.
//   * The device region is still uncalibrated: it reads as a u64 accumulator whose scaling does not
//     close against the 2^-55 scaling (spec/10 §10.3 leaves the block-average N open), and no peak
//     has been located on the wire at all. Blocking a working meter display on that would be
//     choosing the unknown source over the known one.
//
// What this CANNOT do is the hardware output row: those are summed by the FF800's own DSP and we
// never see the result. That row is what the device poll is for.
//
// Peak is zero-attack — one sample at full scale lights it — matching the manual's description.
// RMS is a plain mean square over the publish interval, which gives the "relatively slow time
// constant" the manual describes for free, since the interval is ~30 ms rather than per-sample.
//
// Pure logic, no IOKit — unit-tested in tests/test_meters.cpp.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>

namespace uf {

// Full scale for the daemon's internal PCM: samples are top-justified int32 (the device's 24 bits
// shifted up by 8), so full scale is 2^31.
inline constexpr double kMeterFullScale = 2147483648.0;

// One channel's reading, normalised so 1.0 is 0 dBFS. Floats, because this crosses into shared
// memory for UIs to draw and nobody needs more than float precision to move a bar.
struct MeterValue {
    float peak = 0.0f;
    float rms  = 0.0f;
};

// dBFS from a normalised 0..1 level. Silence is reported as a floor rather than -inf so a UI can
// draw it without special-casing; -144 dB is below the 24-bit noise floor either way.
inline float meter_db(float norm) {
    if (norm <= 0.0f) return -144.0f;
    const float db = 20.0f * std::log10(norm);
    return db < -144.0f ? -144.0f : db;
}

template <uint32_t kChannels>
class MeterBank {
public:
    static constexpr uint32_t kCh = kChannels;

    // One frame. Hot path — called once per channel per frame at up to 192 kHz, so it stays a
    // compare and a multiply-accumulate with no branching beyond the peak test.
    void add_frame(const int32_t* samples, uint32_t n) {
        const uint32_t c = n < kCh ? n : kCh;
        for (uint32_t i = 0; i < c; ++i) {
            const int32_t s = samples[i];
            // Negate rather than abs(): abs(INT32_MIN) is undefined, and a full-scale negative
            // sample is exactly the one a meter must not get wrong.
            const uint32_t mag = s < 0 ? (uint32_t)(-(int64_t)s) : (uint32_t)s;
            if (mag > peak_[i]) peak_[i] = mag;
            const double d = (double)s;
            sumSq_[i] += d * d;
        }
        ++frames_;
    }

    // Read the accumulators out and start again. Called at the publish interval, so the RMS window
    // is exactly the interval between reads — no separate window bookkeeping to get out of step.
    void take(MeterValue* out, uint32_t n) {
        const uint32_t c = n < kCh ? n : kCh;
        const double inv = frames_ ? 1.0 / (double)frames_ : 0.0;
        for (uint32_t i = 0; i < c; ++i) {
            out[i].peak = (float)((double)peak_[i] / kMeterFullScale);
            out[i].rms  = (float)(std::sqrt(sumSq_[i] * inv) / kMeterFullScale);
        }
        reset();
    }

    void reset() {
        peak_.fill(0);
        sumSq_.fill(0.0);
        frames_ = 0;
    }

    uint32_t frames() const { return frames_; }

private:
    std::array<uint32_t, kCh> peak_{};
    std::array<double, kCh> sumSq_{};
    uint32_t frames_ = 0;
};

}  // namespace uf
