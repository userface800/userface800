// OHCI-2 — AT descriptor-block construction (host-testable; descriptors are LE in memory).
// NOTE: valid only on a little-endian host (Apple Silicon / x86), which is where the dext runs.
#include "../platform/ohci-dext/ohci_dma.h"
#include "../platform/ohci-dext/ohci_async.h"
#include "uf/ohci/async.hpp"
#include "uf/protocol/registers.hpp"
#include "uf_test.hpp"

using namespace uf;
using namespace uf::ohci;

UF_TEST(read_quadlet_block_is_z2) {
    // Read firmware rev: 3-quadlet header, no payload -> Z=2, single OUTPUT_LAST_IMMEDIATE.
    auto p = read_quadlet_request(local_node_id(1), 5, reg::kFirmwareRev);
    auto at = build_at_header(p, kS400);
    ohci_descriptor d[4];
    unsigned z = ohci_build_at_block(d, at.q, at.count, 0, 0);
    UF_CHECK_EQ(z, 2u);
    // d[0]: KEY_IMMEDIATE + (since no payload) OUTPUT_LAST | IRQ_ALWAYS | BRANCH_ALWAYS.
    UF_CHECK(d[0].control & DESC_KEY_IMMEDIATE);
    UF_CHECK(d[0].control & DESC_OUTPUT_LAST);
    UF_CHECK(d[0].control & DESC_IRQ_ALWAYS);
    UF_CHECK(d[0].control & DESC_BRANCH_ALWAYS);
    UF_CHECK_EQ(d[0].req_count, static_cast<uint16_t>(12));  // 3 quadlets
    // Header copied inline at d[1].
    const uint32_t* imm = reinterpret_cast<const uint32_t*>(&d[1]);
    UF_CHECK_EQ(imm[0], at.q[0]);
    UF_CHECK_EQ(imm[1], at.q[1]);
    UF_CHECK_EQ(imm[2], at.q[2]);
}

UF_TEST(block_write_is_z3_with_payload) {
    std::vector<u8> data(12, 0);
    auto p = write_block_request(local_node_id(1), 7, reg::kConfBlock, data);
    auto at = build_at_header(p, kS800);
    ohci_descriptor d[4];
    unsigned z = ohci_build_at_block(d, at.q, at.count, 0xDEAD0000u, 12);
    UF_CHECK_EQ(z, 3u);
    UF_CHECK_EQ(d[0].req_count, static_cast<uint16_t>(16));  // 4-quadlet header
    // d[0] is NOT the last one here (no OUTPUT_LAST on d[0]); d[2] is the payload OUTPUT_LAST.
    UF_CHECK(!(d[0].control & DESC_OUTPUT_LAST));
    UF_CHECK_EQ(d[2].req_count, static_cast<uint16_t>(12));
    UF_CHECK_EQ(d[2].data_address, 0xDEAD0000u);
    UF_CHECK(d[2].control & DESC_OUTPUT_LAST);
    UF_CHECK(d[2].control & DESC_BRANCH_ALWAYS);
}

UF_TEST_MAIN()
