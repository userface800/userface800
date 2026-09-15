// selfid.hpp — IEEE-1394 self-ID (bus topology) parsing (portable).
//
// After every bus reset the OHCI receives a self-ID stream (one "zero" packet per node, plus
// optional extended packets for nodes with >3 ports). Parsing it gives the node list — phy id, link
// status, speed, contender — which UFAsyncIO uses to locate the FF800's node (node_id = 0xffc0 |
// phy_id) and pick the transfer speed. Ported from Linux phy-packet-definitions.h (GPL-2.0). The
// OHCI-buffer framing (header quadlet + inverse-check quadlets) is stripped in the dext; this parses
// the resulting self-ID quadlet stream and is testable on the host.
#pragma once
#include <vector>
#include "uf/protocol/endian.hpp"
#include "uf/ohci/async.hpp"  // Scode

namespace uf::ohci {

inline constexpr u32 kPacketIdSelfId = 2;  // bits [31:30] of a self-ID quadlet

// One node's self-ID (from its "zero" packet).
struct SelfId {
    u32 phy_id;         // 0..62
    bool link_active;   // node has an active link layer (can be addressed)
    u32 gap_count;
    Scode scode;        // max speed this node supports
    bool contender;     // bus-manager contender
    bool initiated_reset;
    u16 node_id() const { return static_cast<u16>(0xffc0u | (phy_id & 0x3f)); }
};

constexpr u32 self_id_packet_identifier(u32 q) { return (q & 0xc0000000u) >> 30; }
constexpr bool self_id_is_extended(u32 q) { return (q & 0x00800000u) != 0; }
constexpr u32 self_id_phy_id(u32 q) { return (q & 0x3f000000u) >> 24; }

// Parse a self-ID quadlet stream into per-node entries. Extended (continuation) packets are skipped
// — only the "zero" packet carries the fields we need. Non-self-ID quadlets are ignored.
inline std::vector<SelfId> parse_self_ids(const u32* quadlets, size_t count) {
    std::vector<SelfId> nodes;
    for (size_t i = 0; i < count; ++i) {
        const u32 q = quadlets[i];
        if (self_id_packet_identifier(q) != kPacketIdSelfId) continue;
        if (self_id_is_extended(q)) continue;  // continuation of the previous node
        SelfId s{};
        s.phy_id = self_id_phy_id(q);
        s.link_active = (q & 0x00400000u) != 0;
        s.gap_count = (q & 0x003f0000u) >> 16;
        s.scode = static_cast<Scode>((q & 0x0000c000u) >> 14);
        s.contender = (q & 0x00000800u) != 0;
        s.initiated_reset = (q & 0x00000002u) != 0;
        nodes.push_back(s);
    }
    return nodes;
}

inline std::vector<SelfId> parse_self_ids(const std::vector<u32>& quadlets) {
    return parse_self_ids(quadlets.data(), quadlets.size());
}

// Build a self-ID "zero" quadlet (used to construct test streams / for round-trip checks).
constexpr u32 make_self_id_zero(u32 phy_id, bool link_active, u32 gap_count, Scode scode,
                                bool contender, bool more_packets) {
    return (kPacketIdSelfId << 30) | ((phy_id & 0x3f) << 24) |
           (link_active ? 0x00400000u : 0u) | ((gap_count & 0x3f) << 16) |
           ((static_cast<u32>(scode) & 0x3) << 14) | (contender ? 0x00000800u : 0u) |
           (more_packets ? 0x1u : 0u);
}

}  // namespace uf::ohci
