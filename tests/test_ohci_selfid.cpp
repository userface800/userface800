// OHCI-1 (portable) — self-ID / bus topology parsing.
#include "uf/ohci/selfid.hpp"
#include "uf_test.hpp"

using namespace uf;
using namespace uf::ohci;

UF_TEST(parse_two_node_bus) {
    // Node 0 = the controller (S800, contender), node 1 = the FF800 (S400, link active).
    std::vector<u32> stream = {
        make_self_id_zero(0, /*link*/ true, /*gap*/ 0x3f, kS800, /*contender*/ true, false),
        make_self_id_zero(1, /*link*/ true, /*gap*/ 0x3f, kS400, /*contender*/ false, false),
    };
    auto nodes = parse_self_ids(stream);
    UF_CHECK_EQ(nodes.size(), static_cast<size_t>(2));

    UF_CHECK_EQ(nodes[0].phy_id, 0u);
    UF_CHECK(nodes[0].link_active);
    UF_CHECK(nodes[0].scode == kS800);
    UF_CHECK(nodes[0].contender);
    UF_CHECK_EQ(nodes[0].node_id(), 0xffc0u);

    UF_CHECK_EQ(nodes[1].phy_id, 1u);
    UF_CHECK(nodes[1].scode == kS400);
    UF_CHECK(!nodes[1].contender);
    UF_CHECK_EQ(nodes[1].node_id(), 0xffc1u);  // this is what UFAsyncIO addresses the FF800 at
}

UF_TEST(skips_extended_and_noise) {
    std::vector<u32> stream = {
        make_self_id_zero(0, true, 0, kS400, false, /*more*/ true),
        0x80800000u | (0u << 24),  // extended (continuation) packet for node 0 — must be skipped
        make_self_id_zero(1, true, 0, kS800, false, false),
        0x00000000u,               // not a self-ID (identifier 0) — ignored
    };
    auto nodes = parse_self_ids(stream);
    UF_CHECK_EQ(nodes.size(), static_cast<size_t>(2));
    UF_CHECK_EQ(nodes[0].phy_id, 0u);
    UF_CHECK_EQ(nodes[1].phy_id, 1u);
    UF_CHECK(nodes[1].scode == kS800);
}

UF_TEST(link_inactive_node) {
    // A node whose link is not active can't be addressed as a transaction target.
    std::vector<u32> stream = {make_self_id_zero(3, /*link*/ false, 0, kS400, false, false)};
    auto nodes = parse_self_ids(stream);
    UF_CHECK_EQ(nodes.size(), static_cast<size_t>(1));
    UF_CHECK(!nodes[0].link_active);
    UF_CHECK_EQ(nodes[0].node_id(), 0xffc3u);
}

UF_TEST_MAIN()
