// OHCI-2 (portable) — IEEE-1394 async request/response packet layer.
#include "uf/ohci/async.hpp"
#include "uf/protocol/registers.hpp"
#include "uf_test.hpp"

using namespace uf;
using namespace uf::ohci;

UF_TEST(node_id_helper) {
    UF_CHECK_EQ(local_node_id(0), 0xffc0u);
    UF_CHECK_EQ(local_node_id(1), 0xffc1u);
    UF_CHECK_EQ(local_node_id(63), 0xffffu);
}

UF_TEST(read_quadlet_request_firmware_reg) {
    // Read the FF800 firmware revision (0x200000100) from node 1, tlabel 5.
    auto p = read_quadlet_request(local_node_id(1), 5, reg::kFirmwareRev);
    UF_CHECK_EQ(p.count, 3u);  // read request = 3 header quadlets, no payload
    UF_CHECK(p.payload.empty());
    UF_CHECK_EQ(hdr_tcode(p.header), static_cast<u32>(kReadQuadletReq));
    UF_CHECK_EQ(hdr_tlabel(p.header), 5u);
    UF_CHECK_EQ(hdr_destination(p.header), 0xffc1u);
    // 48-bit offset reconstructs to the register address (high-part 0x0002).
    UF_CHECK_EQ(hdr_offset(p.header), static_cast<u64>(reg::kFirmwareRev));
    UF_CHECK_EQ(hdr_offset(p.header), 0x200000100ull);
}

UF_TEST(write_quadlet_request_rate_reg) {
    // spec/09 §9.2: set 48 kHz = write 0xBB80 to STF 0xfc88f000.
    auto p = write_quadlet_request(local_node_id(1), 3, reg::kStf, 0x0000BB80);
    UF_CHECK_EQ(p.count, 4u);
    UF_CHECK_EQ(hdr_tcode(p.header), static_cast<u32>(kWriteQuadletReq));
    UF_CHECK_EQ(hdr_offset(p.header), 0xfc88f000ull);
    UF_CHECK_EQ(hdr_quadlet_data(p.header), 0x0000BB80u);
    // High-part 0x0000 register: offset_high field is 0.
    UF_CHECK_EQ(p.header[1] & 0xffffu, 0u);
}

UF_TEST(write_block_request_conf_block) {
    // The CR0/CR1/CR2 conf block: 12-byte block write to 0xfc88f014.
    std::vector<u8> data(12, 0);
    host_to_le32(0x00000810, &data[0]);
    host_to_le32(0x0000001A, &data[4]);
    host_to_le32(0x80000001, &data[8]);
    auto p = write_block_request(local_node_id(1), 7, reg::kConfBlock, data);
    UF_CHECK_EQ(hdr_tcode(p.header), static_cast<u32>(kWriteBlockReq));
    UF_CHECK_EQ(hdr_data_length(p.header), 12u);
    UF_CHECK_EQ(hdr_extended_tcode(p.header), 0u);
    UF_CHECK_EQ(hdr_offset(p.header), 0xfc88f014ull);
    UF_CHECK_EQ(p.payload.size(), static_cast<size_t>(12));
}

UF_TEST(lock_compare_swap) {
    auto p = lock_compare_swap_request(local_node_id(1), 2, reg::kStatus0, 0xdead, 0xbeef);
    UF_CHECK_EQ(hdr_tcode(p.header), static_cast<u32>(kLockReq));
    UF_CHECK_EQ(hdr_extended_tcode(p.header), static_cast<u32>(kExtCompareSwap));
    UF_CHECK_EQ(hdr_data_length(p.header), 8u);
    UF_CHECK_EQ(p.payload.size(), static_cast<size_t>(8));
    UF_CHECK_EQ(le32_to_host(&p.payload[0]), 0xdeadu);
    UF_CHECK_EQ(le32_to_host(&p.payload[4]), 0xbeefu);
}

UF_TEST(at_header_repacks_ieee) {
    // Read-quadlet request -> OHCI AT header: dest moves to Q1, speed embeds in Q0.
    auto p = read_quadlet_request(local_node_id(1), 5, reg::kFirmwareRev);
    auto at = build_at_header(p, kS400);
    UF_CHECK_EQ(at.count, 3u);
    // Q0: spd (S400=2) at bits 18:16, tlabel 5 at 15:10, retry_x at 9:8, tcode 4 at 7:4.
    UF_CHECK_EQ((at.q[0] & 0x00070000u) >> 16, static_cast<u32>(kS400));
    UF_CHECK_EQ((at.q[0] & 0x0000fc00u) >> 10, 5u);
    UF_CHECK_EQ((at.q[0] & 0x000000f0u) >> 4, static_cast<u32>(kReadQuadletReq));
    // Q0 high half [31:19] is 0 — the destination is NOT in Q0 (only spd sits at [18:16]).
    UF_CHECK_EQ(at.q[0] & 0xfff80000u, 0u);
    // Q1: destinationId in the high half, offset-high (0x0002) in the low half.
    UF_CHECK_EQ((at.q[1] & 0xffff0000u) >> 16, 0xffc1u);
    UF_CHECK_EQ(at.q[1] & 0x0000ffffu, 0x0002u);
    // Q2: offset low.
    UF_CHECK_EQ(at.q[2], 0x00000100u);
}

UF_TEST(at_header_block_write) {
    std::vector<u8> data(12, 0);
    auto p = write_block_request(local_node_id(2), 7, reg::kConfBlock, data);
    auto at = build_at_header(p, kS800);
    UF_CHECK_EQ(at.count, 4u);
    UF_CHECK_EQ((at.q[0] & 0x00070000u) >> 16, static_cast<u32>(kS800));
    UF_CHECK_EQ((at.q[1] & 0xffff0000u) >> 16, 0xffc2u);
    UF_CHECK_EQ(at.q[2], 0xfc88f014u);
    UF_CHECK_EQ((at.q[3] & 0xffff0000u) >> 16, 12u);  // data_length
}

UF_TEST(parse_read_quadlet_response) {
    // Build a synthetic read-quadlet response: tcode 6, rcode complete, tlabel 5, data 0xCAFEF00D.
    u32 h[4] = {0, 0, 0, 0};
    h[0] = (0xffc1u << 16) | (5u << 10) | (static_cast<u32>(kReadQuadletResp) << 4);
    h[1] = (static_cast<u32>(kRcodeComplete) << 12);
    h[3] = 0xCAFEF00D;
    auto r = parse_response(h);
    UF_CHECK_EQ(r.tcode, static_cast<u32>(kReadQuadletResp));
    UF_CHECK_EQ(r.rcode, static_cast<u32>(kRcodeComplete));
    UF_CHECK_EQ(r.tlabel, 5u);
    UF_CHECK_EQ(r.quadlet, 0xCAFEF00Du);
}

UF_TEST(parse_error_response) {
    u32 h[4] = {0, 0, 0, 0};
    h[0] = (static_cast<u32>(kWriteResp) << 4);
    h[1] = (static_cast<u32>(kRcodeAddressError) << 12);
    auto r = parse_response(h);
    UF_CHECK_EQ(r.tcode, static_cast<u32>(kWriteResp));
    UF_CHECK_EQ(r.rcode, static_cast<u32>(kRcodeAddressError));
}

UF_TEST_MAIN()
