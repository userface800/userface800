// The self-describing test signal. Every fault it claims to detect is injected here deliberately,
// because an analyser that reports "clean" is worthless unless it has been shown to report dirty.
#include <vector>
#include "uf/protocol/testsig.hpp"
#include "uf_test.hpp"

using namespace uf;

static std::vector<i32> gen(u32 ch, u64 frames) { return testsig_generate(ch, frames); }

UF_TEST(encoding_round_trips_for_every_channel) {
    for (u32 c = 0; c < kTestSigMaxChannels; ++c)
        for (u64 f : {0ull, 1ull, 12345ull, (u64)kTestSigIndexMask, (u64)kTestSigIndexMask + 1}) {
            const i32 s = testsig_sample(c, f);
            UF_CHECK_EQ(testsig_channel_of(s), c);
            UF_CHECK_EQ(testsig_index_of(s), (u32)(f & kTestSigIndexMask));
        }
}

UF_TEST(channels_8_and_above_survive_sign_extension) {
    // Channel >= 8 sets bit 23, the sign bit of a 24-bit sample, so these come back negative. If
    // sign extension is wrong they decode as some other channel and every recording reads as shear.
    for (u32 c = 8; c < kTestSigMaxChannels; ++c) {
        const i32 s = testsig_sample(c, 7);
        UF_CHECK(s < 0);
        UF_CHECK_EQ(testsig_channel_of(s), c);
        UF_CHECK_EQ(testsig_index_of(s), 7u);
    }
}

UF_TEST(a_perfect_recording_is_clean) {
    auto v = gen(4, 5000);
    for (u32 c = 0; c < 4; ++c) {
        auto r = testsig_analyse(v.data(), 5000, 4, c, c);
        UF_CHECK(r.clean());
        UF_CHECK_EQ(r.framesChecked, 5000ull);
    }
}

UF_TEST(leading_silence_is_not_a_fault) {
    // A DAW recording always starts before the signal does.
    std::vector<i32> v(2 * 1000, 0);
    auto sig = gen(2, 1000);
    v.insert(v.end(), sig.begin(), sig.end());
    auto r = testsig_analyse(v.data(), 2000, 2, 0, 0);
    UF_CHECK(r.clean());
    UF_CHECK_EQ(r.framesChecked, 1000ull);
}

UF_TEST(wrap_is_not_mistaken_for_a_discontinuity) {
    // Start just before the counter wraps and run through it.
    const u64 start = kTestSigPeriod - 10;
    std::vector<i32> v;
    for (u64 f = start; f < start + 40; ++f) v.push_back(testsig_sample(0, f));
    auto r = testsig_analyse(v.data(), v.size(), 1, 0, 0);
    UF_CHECK(r.clean());
}

// ── each fault, injected ──────────────────────────────────────────────────────────────────────

UF_TEST(dropped_samples_are_reported_with_their_size) {
    auto v = gen(1, 1000);
    v.erase(v.begin() + 500, v.begin() + 507);        // lose 7 frames
    auto r = testsig_analyse(v.data(), v.size(), 1, 0, 0);
    UF_CHECK_EQ(r.gaps, 1ull);
    UF_CHECK_EQ(r.droppedFrames, 7ull);
    UF_CHECK(r.events.size() == 1 && r.events[0].fault == TestSigFault::Gap);
    UF_CHECK(!r.clean());
}

UF_TEST(duplicated_samples_are_reported) {
    auto v = gen(1, 1000);
    v.insert(v.begin() + 500, v[500]);                // hold one sample twice
    auto r = testsig_analyse(v.data(), v.size(), 1, 0, 0);
    UF_CHECK_EQ(r.repeats, 1ull);
    UF_CHECK_EQ(r.gaps, 0ull);
    UF_CHECK(!r.clean());
}

UF_TEST(reordering_is_reported_as_backwards) {
    auto v = gen(1, 1000);
    v[600] = testsig_sample(0, 300);                  // a stale sample served again
    auto r = testsig_analyse(v.data(), v.size(), 1, 0, 0);
    UF_CHECK(r.backwards >= 1);
    UF_CHECK(!r.clean());
}

UF_TEST(channel_shear_is_reported) {
    // The failure mode of the four-stream layout: a block carrying another channel's data.
    auto v = gen(2, 1000);
    for (u64 f = 400; f < 450; ++f) v[f * 2] = testsig_sample(1, f);   // ch0 column carries ch1
    auto r = testsig_analyse(v.data(), 1000, 2, 0, 0);
    UF_CHECK_EQ(r.wrongChannel, 50ull);
    UF_CHECK(r.events[0].sawChannel == 1u);
    UF_CHECK(!r.clean());
}

UF_TEST(a_path_that_is_not_bit_transparent_is_reported) {
    // Any gain, dither or resampling breaks the encoding. Detecting that explicitly matters: it is
    // the difference between "the driver corrupted audio" and "the DAW was not at unity".
    auto v = gen(1, 1000);
    for (auto& s : v) s = (i32)(s * 0.999);           // a hair of gain
    auto r = testsig_analyse(v.data(), v.size(), 1, 0, 0);
    UF_CHECK(r.notEncoded > 0);
    UF_CHECK(!r.clean());
}

UF_TEST(one_sample_shift_shows_up_as_a_single_gap) {
    // A single frame inserted or dropped, twice a second, must read as two one-sample gaps. That
    // signature is what distinguishes a stream being resampled from a burst of dropouts.
    auto v = gen(1, 100000);
    v.erase(v.begin() + 20000);                       // one frame gone
    v.erase(v.begin() + 60000);                       // and another, later
    auto r = testsig_analyse(v.data(), v.size(), 1, 0, 0);
    UF_CHECK_EQ(r.gaps, 2ull);
    UF_CHECK_EQ(r.droppedFrames, 2ull);
    for (const auto& e : r.events) UF_CHECK_EQ(e.delta, (i64)2);   // index stepped by 2, not 1
}

UF_TEST_MAIN()
