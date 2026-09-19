// Per-channel peak and RMS from the daemon's own PCM (uf_meters.hpp).
#include "../platform/shared/uf_meters.hpp"
#include "uf_test.hpp"

#include <cmath>

using namespace uf;

namespace {
constexpr int32_t kFull = 2147483647;      // +full scale, top-justified int32
bool near(float a, float b, float tol) { return std::fabs(a - b) < tol; }
}  // namespace

UF_TEST(silence_reads_as_nothing) {
    MeterBank<8> m;
    int32_t f[8] = {0};
    for (int i = 0; i < 100; ++i) m.add_frame(f, 8);
    MeterValue v[8];
    m.take(v, 8);
    UF_CHECK(v[0].peak == 0.0f);
    UF_CHECK(v[0].rms == 0.0f);
}

UF_TEST(peak_is_zero_attack) {
    // The manual: "1 sample is enough for a full scale display". One loud sample in a thousand quiet
    // ones must light the peak while barely moving the RMS — that difference is the whole point of
    // showing both.
    MeterBank<2> m;
    int32_t quiet[2] = {0, 0};
    int32_t loud[2]  = {kFull, 0};
    m.add_frame(loud, 2);
    for (int i = 0; i < 999; ++i) m.add_frame(quiet, 2);
    MeterValue v[2];
    m.take(v, 2);
    UF_CHECK(near(v[0].peak, 1.0f, 0.001f));
    UF_CHECK(v[0].rms < 0.04f);            // sqrt(1/1000) ~ 0.0316
    UF_CHECK(v[1].peak == 0.0f);           // channels stay independent
}

UF_TEST(rms_of_a_square_wave_equals_its_amplitude) {
    // A square wave is the one signal whose RMS is exactly its peak, so it checks the scaling
    // without dragging a sine approximation into the test.
    MeterBank<1> m;
    for (int i = 0; i < 1000; ++i) {
        int32_t s = (i & 1) ? kFull / 2 : -(kFull / 2);
        m.add_frame(&s, 1);
    }
    MeterValue v[1];
    m.take(v, 1);
    UF_CHECK(near(v[0].peak, 0.5f, 0.001f));
    UF_CHECK(near(v[0].rms, 0.5f, 0.001f));
}

UF_TEST(rms_of_a_sine_is_its_amplitude_over_root_two) {
    MeterBank<1> m;
    const int n = 4800;
    for (int i = 0; i < n; ++i) {
        const double x = std::sin(2.0 * M_PI * 100.0 * i / 48000.0);
        int32_t s = (int32_t)(x * (kFull / 2));
        m.add_frame(&s, 1);
    }
    MeterValue v[1];
    m.take(v, 1);
    UF_CHECK(near(v[0].peak, 0.5f, 0.002f));
    UF_CHECK(near(v[0].rms, 0.5f / (float)M_SQRT2, 0.005f));
}

// abs(INT32_MIN) is undefined behaviour, and a full-scale NEGATIVE sample is exactly the one a meter
// must not get wrong — it is what a clipped waveform is made of.
UF_TEST(a_full_scale_negative_sample_does_not_overflow) {
    MeterBank<1> m;
    int32_t s = INT32_MIN;
    m.add_frame(&s, 1);
    MeterValue v[1];
    m.take(v, 1);
    UF_CHECK(near(v[0].peak, 1.0f, 0.001f));
    UF_CHECK(near(v[0].rms, 1.0f, 0.001f));
}

UF_TEST(taking_a_reading_starts_the_next_window) {
    MeterBank<1> m;
    int32_t loud = kFull;
    m.add_frame(&loud, 1);
    MeterValue v[1];
    m.take(v, 1);
    UF_CHECK(near(v[0].peak, 1.0f, 0.001f));

    int32_t quiet = 0;
    m.add_frame(&quiet, 1);
    m.take(v, 1);
    UF_CHECK(v[0].peak == 0.0f);           // the previous window's peak does not carry over
}

UF_TEST(fewer_channels_than_the_bank_holds_is_fine) {
    MeterBank<28> m;
    int32_t f[4] = {kFull, 0, 0, 0};
    m.add_frame(f, 4);                     // 12-channel 4x mode passes fewer than 28
    MeterValue v[28];
    m.take(v, 28);
    UF_CHECK(near(v[0].peak, 1.0f, 0.001f));
    UF_CHECK(v[27].peak == 0.0f);
}

UF_TEST(taking_before_any_frame_does_not_divide_by_zero) {
    MeterBank<4> m;
    MeterValue v[4];
    m.take(v, 4);
    UF_CHECK(v[0].rms == 0.0f);
    UF_CHECK_EQ(m.frames(), 0u);
}

UF_TEST(db_conversion_floors_rather_than_returning_negative_infinity) {
    UF_CHECK(near(meter_db(1.0f), 0.0f, 0.001f));
    UF_CHECK(near(meter_db(0.5f), -6.0206f, 0.01f));
    UF_CHECK(meter_db(0.0f) == -144.0f);
    UF_CHECK(meter_db(-1.0f) == -144.0f);
    UF_CHECK(std::isfinite(meter_db(1e-30f)));
}

UF_TEST_MAIN()
