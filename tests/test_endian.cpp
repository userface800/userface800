// Smoke test for the harness + the LE quadlet helpers (spec/01, spec/03 §3.4).
#include "uf/protocol/endian.hpp"
#include "uf_test.hpp"

using namespace uf;

UF_TEST(le32_roundtrip) {
    u8 buf[4] = {0x80, 0xbb, 0x00, 0x00};
    UF_CHECK_EQ(le32_to_host(buf), 0x0000bb80u);  // 48000, wire bytes 80 bb 00 00

    u8 out[4] = {};
    host_to_le32(0x8000001Cu, out);
    UF_CHECK_EQ(out[0], 0x1Cu);
    UF_CHECK_EQ(out[1], 0x00u);
    UF_CHECK_EQ(out[2], 0x00u);
    UF_CHECK_EQ(out[3], 0x80u);
    UF_CHECK_EQ(le32_to_host(out), 0x8000001Cu);
}

UF_TEST_MAIN()
