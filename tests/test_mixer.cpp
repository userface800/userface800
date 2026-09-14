// Mixer addressing + coefficient math (spec/07, spec/09 §9.6).
#include "uf/protocol/mixer.hpp"
#include "uf_test.hpp"

using namespace uf;

UF_TEST(worked_example_9_6) {
    // spec/09 §9.6: input 3 (src index 2) -> output 1 (dest index 0) at 0 dB.
    UF_CHECK_EQ(input_coeff_addr(2, 0), 0x80080008ull);
    UF_CHECK_EQ(kMixerUnity, 0x00008000u);
}

UF_TEST(matrix_addressing) {
    // Input half vs playback half of a destination's 0x100 block.
    UF_CHECK_EQ(input_coeff_addr(0, 0), 0x80080000ull);
    UF_CHECK_EQ(playback_coeff_addr(0, 0), 0x80080080ull);
    // dest*0x100 stride.
    UF_CHECK_EQ(input_coeff_addr(0, 1), 0x80080100ull);
    UF_CHECK_EQ(playback_coeff_addr(3, 2), reg::kMixerRam + 2 * 0x100 + 0x80 + 4 * 3);
    // Output fader row.
    UF_CHECK_EQ(output_fader_addr(0), 0x80081f80ull);
    UF_CHECK_EQ(output_fader_addr(5), 0x80081f80ull + 4 * 5);
}

UF_TEST(coefficient_scale) {
    UF_CHECK_EQ(db_to_coeff(0.0), 0x8000u);                    // 0 dB = unity
    UF_CHECK_EQ(db_to_coeff(-100.0), 0u);                      // rounds to 0
    UF_CHECK_EQ(db_to_coeff(-90.0), 1u);                       // ~1 LSB (not exactly mute)
    // +6.0206 dB = exactly 2x -> 65536, clamps to the +6 dB max (0x10000).
    UF_CHECK_EQ(db_to_coeff(6.0206), kMixerMax);
    UF_CHECK_EQ(db_to_coeff(100.0), kMixerMax);                // clamps well above max
    // +6.0 dB is just under 2x: ~65385, not yet clamped.
    u32 near6 = db_to_coeff(6.0);
    UF_CHECK(near6 > 0xff00u && near6 < kMixerMax);
    // -6.02 dB halves the coefficient (~0x4000).
    u32 half = db_to_coeff(-6.0206);
    UF_CHECK(half >= 0x3ff0u && half <= 0x4010u);
    // Round-trip near unity.
    double db = coeff_to_db(0x8000);
    UF_CHECK(db > -0.001 && db < 0.001);
}

UF_TEST(masks) {
    auto on = mask_all(true);
    auto off = mask_all(false);
    UF_CHECK_EQ(on.size(), static_cast<size_t>(28));
    for (auto q : on) UF_CHECK_EQ(q, 1u);
    for (auto q : off) UF_CHECK_EQ(q, 0u);
}

UF_TEST_MAIN()
