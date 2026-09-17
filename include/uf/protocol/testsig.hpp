// testsig.hpp — a self-describing test signal for proving a playback path is bit-exact (portable).
//
// The question this exists to answer: two drivers feed the same FF800, and one sounds harsh at
// 96 kHz. Listening cannot tell you whether samples are being dropped, duplicated, shifted by one,
// or delivered to the wrong channel — all of those sound like "harsh". So the signal carries its own
// coordinates: every sample says which channel it belongs to and what its index is. Record it back
// and the damage is not inferred, it is read off the values.
//
// Deliberately a FILE format rather than a driver feature, so the identical signal can be played
// through OUR driver and through RME's with no privileged tooling on the other machine. The
// comparison is then symmetric by construction.
//
// Encoding, per 24-bit sample:
//
//     bit  23      marker, always 1
//     bits 22..19  channel id (0..15)
//     bits 18..3   frame index, wrapping every 65536 frames (~0.68 s at 96 kHz)
//     bits 2..0    check digits over the above
//
// Two properties are load-bearing and were both learned the hard way. The marker means a valid
// sample is never the value 0, so digital silence is unambiguous — without it, channel 0 index 0
// encodes to 0 and the analyser skips the first real sample as pre-roll. And the check digits mean
// not every 24-bit value is a legal encoding: without them any corruption still decodes as *some*
// (channel, index) pair, so a path that applies gain or dither reads as merely discontinuous
// instead of as what it is. Seven in eight altered samples now fail outright.
//
// A wrap is unambiguous (0xFFFF -> 0) and so is not confused with a discontinuity. The channel id
// makes channel shear — the failure mode of our four-stream layout — visible directly: a block of
// samples carrying the wrong id is the whole diagnosis.
//
// CAVEAT for whoever runs this: the path must be bit-transparent. Any gain, dither, resampling,
// plugin or mixer fader off unity destroys the encoding and every sample reads as corrupt. That is
// itself a useful result the first time it happens, but it is not the result you were looking for.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "uf/protocol/endian.hpp"

namespace uf {

inline constexpr u32 kTestSigIndexBits = 16;
inline constexpr u32 kTestSigIndexMask = (1u << kTestSigIndexBits) - 1;   // 0xFFFF
inline constexpr u32 kTestSigMaxChannels = 16;
inline constexpr u32 kTestSigPeriod = 1u << kTestSigIndexBits;            // frames before wrap
inline constexpr u32 kTestSigMarker = 0x800000u;

constexpr u32 testsig_check(u32 channel, u32 index) {
    return (channel ^ index ^ (index >> 8)) & 0x7u;
}

// The 24-bit value for (channel, frame index), sign-extended into an i32 the way the rest of the
// codebase carries 24-bit audio. The marker bit is the sign bit, so every sample is negative.
constexpr i32 testsig_sample(u32 channel, u64 frame) {
    const u32 ch = channel & 0xF;
    const u32 ix = (u32)(frame & kTestSigIndexMask);
    const u32 raw = kTestSigMarker | (ch << 19) | (ix << 3) | testsig_check(ch, ix);
    return (i32)(raw | 0xFF000000u);   // sign-extend from 24 bits
}

constexpr u32 testsig_channel_of(i32 sample) { return ((u32)sample >> 19) & 0xF; }
constexpr u32 testsig_index_of(i32 sample) { return ((u32)sample >> 3) & kTestSigIndexMask; }

// Is this a legal encoding at all? Fails for silence, for any value without the marker, and for
// seven in eight values whose bits have been altered by gain, dither or resampling.
constexpr bool testsig_valid(i32 sample) {
    const u32 raw = (u32)sample & 0xFFFFFFu;
    if (!(raw & kTestSigMarker)) return false;
    return testsig_sample(testsig_channel_of(sample), testsig_index_of(sample)) == sample;
}

// One frame-major buffer of the signal: sample[frame * channels + channel].
inline std::vector<i32> testsig_generate(u32 channels, u64 frames) {
    std::vector<i32> v;
    v.reserve((size_t)frames * channels);
    for (u64 f = 0; f < frames; ++f)
        for (u32 c = 0; c < channels; ++c) v.push_back(testsig_sample(c, f));
    return v;
}

// ── Analysis ──────────────────────────────────────────────────────────────────────────────────

enum class TestSigFault {
    Gap,          // indices jumped forward: samples were dropped
    Repeat,       // the same index twice: a sample was duplicated
    Backwards,    // index went back without a clean wrap: reordering, or a ring served stale data
    WrongChannel, // the sample carries another channel's id: channel shear
    NotEncoded,   // the value is not a valid encoding at all: the path is not bit-transparent
};

struct TestSigEvent {
    TestSigFault fault;
    u32 channel;      // channel being analysed
    u64 frame;        // position in the RECORDING where it was seen
    i64 delta;        // for Gap/Repeat/Backwards: index change (expected 1)
    u32 sawChannel;   // for WrongChannel: the id actually found
};

struct TestSigReport {
    bool started = false;         // any valid encoded sample seen at all
    u64 framesChecked = 0;
    u64 gaps = 0, repeats = 0, backwards = 0, wrongChannel = 0, notEncoded = 0;
    u64 droppedFrames = 0;        // total frames missing across all gaps
    std::vector<TestSigEvent> events;   // capped; see kMaxEvents
    bool clean() const {
        return started && !gaps && !repeats && !backwards && !wrongChannel && !notEncoded;
    }
};

inline constexpr size_t kTestSigMaxEvents = 64;   // enough to see the pattern, not to drown in it

// Walk one channel of a frame-major recording and report every discontinuity.
//
// `expectChannel` is what this column SHOULD carry. Leading silence is skipped rather than reported:
// a DAW recording always starts before the signal does, and flagging that as corruption would bury
// the real events.
inline TestSigReport testsig_analyse(const i32* frames, u64 frameCount, u32 channels,
                                     u32 column, u32 expectChannel) {
    TestSigReport r;
    bool have = false;
    u32 prevIndex = 0;
    for (u64 f = 0; f < frameCount; ++f) {
        const i32 s = frames[f * channels + column];
        if (!have && s == 0) continue;              // pre-roll silence
        const u32 ch = testsig_channel_of(s);
        const u32 ix = testsig_index_of(s);

        // A valid sample must carry the marker and satisfy its own check digits; anything else
        // means the path altered the value rather than merely reordering it.
        if (!testsig_valid(s)) {
            ++r.notEncoded;
            if (r.events.size() < kTestSigMaxEvents)
                r.events.push_back({TestSigFault::NotEncoded, expectChannel, f, 0, ch});
            continue;
        }
        if (ch != expectChannel) {
            ++r.wrongChannel;
            if (r.events.size() < kTestSigMaxEvents)
                r.events.push_back({TestSigFault::WrongChannel, expectChannel, f, 0, ch});
        }
        if (!have) { have = true; r.started = true; prevIndex = ix; ++r.framesChecked; continue; }

        // Expected step is +1, modulo the wrap.
        const u32 expect = (prevIndex + 1) & kTestSigIndexMask;
        if (ix != expect) {
            i64 d = (i64)ix - (i64)prevIndex;
            if (d < 0) d += kTestSigPeriod;         // treat as a forward jump across the wrap
            if (d == 0) {
                ++r.repeats;
                if (r.events.size() < kTestSigMaxEvents)
                    r.events.push_back({TestSigFault::Repeat, expectChannel, f, 0, ch});
            } else if (d > 0 && d < (i64)kTestSigPeriod / 2) {
                ++r.gaps;
                r.droppedFrames += (u64)(d - 1);
                if (r.events.size() < kTestSigMaxEvents)
                    r.events.push_back({TestSigFault::Gap, expectChannel, f, d, ch});
            } else {
                // More than half a period forward is far more likely to be a backwards step.
                ++r.backwards;
                if (r.events.size() < kTestSigMaxEvents)
                    r.events.push_back({TestSigFault::Backwards, expectChannel, f,
                                        d - (i64)kTestSigPeriod, ch});
            }
        }
        prevIndex = ix;
        ++r.framesChecked;
    }
    return r;
}

inline std::string testsig_fault_name(TestSigFault f) {
    switch (f) {
        case TestSigFault::Gap: return "gap (samples dropped)";
        case TestSigFault::Repeat: return "repeat (sample duplicated)";
        case TestSigFault::Backwards: return "backwards (reordered / stale)";
        case TestSigFault::WrongChannel: return "wrong channel (shear)";
        case TestSigFault::NotEncoded: return "not encoded (path not bit-transparent)";
    }
    return "?";
}

}  // namespace uf
