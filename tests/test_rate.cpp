// Sample-rate encoding (spec/02 §2.3, spec/09 §9.2).
#include "uf/protocol/rate.hpp"
#include "uf_test.hpp"

using namespace uf;

UF_TEST(stf_is_raw_hz) {
    // spec/09 §9.2: wq(0xfc88f000, 0x0000BB80) == 48000.
    UF_CHECK_EQ(stf_value(48000), 0xBB80u);
    UF_CHECK_EQ(stf_value(44100), 44100u);
    UF_CHECK_EQ(stf_value(192000), 192000u);
}

UF_TEST(clock_code_table) {
    UF_CHECK_EQ(clock_config_rate_code(44100), 0x00u);
    UF_CHECK_EQ(clock_config_rate_code(32000), 0x02u);
    UF_CHECK_EQ(clock_config_rate_code(48000), 0x06u);
    UF_CHECK_EQ(clock_config_rate_code(88200), 0x08u);
    UF_CHECK_EQ(clock_config_rate_code(96000), 0x0eu);
    UF_CHECK_EQ(clock_config_rate_code(192000), 0x16u);
    UF_CHECK_EQ(clock_config_rate_code(50000), 0xffffffffu);  // unsupported
    // Every code fits within the clock-config rate field.
    for (const auto& r : kRates) UF_CHECK_EQ(r.clock_code & ~clockcfg::kRateMask, 0u);
}

UF_TEST(clock_code_roundtrip) {
    for (const auto& r : kRates) {
        UF_CHECK_EQ(hz_for_clock_code(r.clock_code), r.hz);
        // Robust to other clock-config bits being set alongside the rate field.
        UF_CHECK_EQ(hz_for_clock_code(r.clock_code | 0x00001c01u), r.hz);
    }
    UF_CHECK_EQ(hz_for_clock_code(0x04), 0u);  // 0x04 is not a rate code
}

UF_TEST(speed_classes) {
    UF_CHECK(speed_for_rate(44100) == Speed::X1);
    UF_CHECK(speed_for_rate(48000) == Speed::X1);
    UF_CHECK(speed_for_rate(88200) == Speed::X2);
    UF_CHECK(speed_for_rate(96000) == Speed::X2);
    UF_CHECK(speed_for_rate(176400) == Speed::X4);
    UF_CHECK(speed_for_rate(192000) == Speed::X4);
    UF_CHECK_EQ(speed_multiplier(Speed::X1), 1u);
    UF_CHECK_EQ(speed_multiplier(Speed::X2), 2u);
    UF_CHECK_EQ(speed_multiplier(Speed::X4), 4u);
    // Table speed agrees with the threshold function.
    for (const auto& r : kRates) UF_CHECK(r.speed == speed_for_rate(r.hz));
}

UF_TEST(supported_rate) {
    UF_CHECK(is_supported_rate(48000));
    UF_CHECK(is_supported_rate(176400));
    UF_CHECK(!is_supported_rate(22050));
    UF_CHECK(!is_supported_rate(0));
}

UF_TEST_MAIN()
