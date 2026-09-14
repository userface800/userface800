// Streaming start/stop register sequence (spec/03 §3.6, spec/09 §9.4).
#include "uf/protocol/stream.hpp"
#include "uf_test.hpp"

using namespace uf;

UF_TEST(worked_example_9_4) {
    // spec/09 §9.4: 48 kHz, 28 channels, rx_channel = 1, S400.
    const u32 dbq = data_block_quadlets(Speed::X1);  // 28
    UF_CHECK_EQ(dbq, 28u);
    // rx packet format = ((28<<3)<<8) | 1 = 0x0000E001.
    UF_CHECK_EQ(rx_packet_format_value(28, 1), 0x0000E001u);
    // alloc tx = 28.
    UF_CHECK_EQ(alloc_tx_stream_write(28).quad, 0x0000001Cu);
    // comm start S400 = 0x8000001C; S800 = 0x8000081C.
    UF_CHECK_EQ(comm_start_value(28, BusSpeed::S400), 0x8000001Cu);
    UF_CHECK_EQ(comm_start_value(28, BusSpeed::S800), 0x8000081Cu);
    // comm stop = 0x80000000.
    UF_CHECK_EQ(comm_stop_write().quad, 0x80000000u);
}

UF_TEST(start_sequence_order_and_addrs) {
    auto w = stream_start_writes(Speed::X1, BwLimit::SendAll, 1, BusSpeed::S400);
    UF_CHECK_EQ(w.size(), static_cast<size_t>(3));
    UF_CHECK_EQ(w[0].addr, reg::kRxPacketFormat);
    UF_CHECK_EQ(w[0].quad, 0x0000E001u);
    UF_CHECK_EQ(w[1].addr, reg::kAllocTxStream);
    UF_CHECK_EQ(w[1].quad, 28u);
    UF_CHECK_EQ(w[2].addr, reg::kIsocCommStart);
    UF_CHECK_EQ(w[2].quad, 0x8000001Cu);
    for (auto& rw : w) UF_CHECK(rw.kind == RegWrite::Kind::Quadlet);
}

UF_TEST(sizes_scale_with_speed) {
    // At 2x: dbq 20 -> rx format ((20<<3)<<8)|2, comm start 0x80000000|20.
    UF_CHECK_EQ(rx_packet_format_value(20, 2), (((20u << 3) << 8) | 2u));
    UF_CHECK_EQ(comm_start_value(12, BusSpeed::S800), 0x80000000u | 12u | 0x800u);  // 4x, S800
}

UF_TEST_MAIN()
