// Transaction-log formatting in spec/09 notation.
#include "uf/protocol/txnlog.hpp"
#include "uf/protocol/control.hpp"
#include "uf/protocol/stream.hpp"
#include "uf_test.hpp"

using namespace uf;

UF_TEST(basic_notation) {
    // spec/09 §9.2: wq(0xfc88f000, 0x0000bb80).
    UF_CHECK_EQ(fmt_wq(reg::kStf, 48000), std::string("wq(0xfc88f000, 0x0000bb80)"));
    UF_CHECK_EQ(fmt_rq(reg::kStatus0, 0x00100000),
                std::string("rq(0x801c0000) -> 0x00100000"));
    // High-part address widens to 9 nibbles (0x200000100).
    UF_CHECK_EQ(fmt_addr(reg::kFirmwareRev), std::string("0x200000100"));
}

UF_TEST(block_notation_9_2) {
    // spec/09 §9.2: wb(0xfc88f014, [0x00000810, 0x0000001a, 0x80000001]).
    auto s = fmt_wb(reg::kConfBlock, {0x00000810, 0x0000001a, 0x80000001});
    UF_CHECK_EQ(s, std::string("wb(0xfc88f014, [0x00000810, 0x0000001a, 0x80000001])"));
}

UF_TEST(regwrite_and_sequence) {
    UFControl c;
    c.set_sample_rate(48000);
    SettingsShadow st;
    st.input_level = InputLevel::P4dBu;
    st.output_level = OutputLevel::P4dBu;
    c.set_settings(st);
    auto seq = c.full_rewrite();
    // The planned §9.2 sequence formats line-by-line.
    std::string expect =
        "wq(0xfc88f000, 0x0000bb80)\n"
        "wb(0xfc88f014, [0x00000810, 0x0000001a, 0x80000001])\n";
    UF_CHECK_EQ(fmt_sequence(seq), expect);
}

UF_TEST(stream_sequence_9_4) {
    auto seq = stream_start_writes(Speed::X1, BwLimit::SendAll, 1, BusSpeed::S400);
    std::string expect =
        "wq(0xfc88f004, 0x0000e001)\n"
        "wq(0xfc88f008, 0x0000001c)\n"
        "wq(0xfc88f00c, 0x8000001c)\n";
    UF_CHECK_EQ(fmt_sequence(seq), expect);
}

UF_TEST_MAIN()
