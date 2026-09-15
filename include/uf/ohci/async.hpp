// async.hpp — IEEE-1394 asynchronous request/response packet layer (portable).
//
// The pure-logic core of UFAsyncIO-over-OHCI: build the 4-quadlet async request headers the OHCI AT
// (async-transmit) DMA context sends, and parse the AR (async-receive) response headers. This is
// what turns a uf register op (addr, value) — see registers.hpp addr_hi/addr_lo — into wire packets.
// Ported from Linux firewire packet-header-definitions.h + firewire-constants.h + core-transaction.c
// (GPL-2.0). Header/DMA poking lives in the dext (platform/ohci-dext); this is testable on the host.
#pragma once
#include <vector>
#include "uf/protocol/endian.hpp"
#include "uf/protocol/registers.hpp"

namespace uf::ohci {

// Transaction codes (IEEE 1394 / firewire-constants.h).
enum Tcode : u32 {
    kWriteQuadletReq = 0x0, kWriteBlockReq = 0x1, kWriteResp = 0x2,
    kReadQuadletReq = 0x4, kReadBlockReq = 0x5, kReadQuadletResp = 0x6, kReadBlockResp = 0x7,
    kLockReq = 0x9, kLockResp = 0xb,
};

// Response codes.
enum Rcode : u32 {
    kRcodeComplete = 0x0, kRcodeConflictError = 0x4, kRcodeDataError = 0x5,
    kRcodeTypeError = 0x6, kRcodeAddressError = 0x7,
    kRcodeSendError = 0x10, kRcodeCancelled = 0x11, kRcodeBusy = 0x12,
    kRcodeGeneration = 0x13, kRcodeNoAck = 0x14,
};

// Speed codes (Sxxx).
enum Scode : u32 { kS100 = 0, kS200 = 1, kS400 = 2, kS800 = 3 };

// Lock extended tcodes.
enum ExtCode : u32 { kExtMaskSwap = 0x1, kExtCompareSwap = 0x2, kExtFetchAdd = 0x3 };

// "retry_X" — the normal retry protocol value in the async header retry field.
inline constexpr u32 kRetryX = 0x1;

// Node id for a device on the local bus (bus_id 0x3ff): 0xffc0 | phy_id.
constexpr u16 local_node_id(u16 phy_id) { return static_cast<u16>(0xffc0u | (phy_id & 0x3f)); }

// One built async packet: `count` header quadlets (3 or 4) + optional block payload bytes.
struct Packet {
    u32 header[4] = {0, 0, 0, 0};
    unsigned count = 0;              // number of valid header quadlets
    std::vector<u8> payload;         // block-write / lock data (empty for quadlet/read requests)
};

// ── Header field accessors (port of async_header_* ) ──────────────────────────────────────────
constexpr u32 hdr_tcode(const u32 h[4]) { return (h[0] & 0x000000f0u) >> 4; }
constexpr u32 hdr_tlabel(const u32 h[4]) { return (h[0] & 0x0000fc00u) >> 10; }
constexpr u32 hdr_retry(const u32 h[4]) { return (h[0] & 0x00000300u) >> 8; }
constexpr u32 hdr_destination(const u32 h[4]) { return (h[0] & 0xffff0000u) >> 16; }
constexpr u32 hdr_source(const u32 h[4]) { return (h[1] & 0xffff0000u) >> 16; }
constexpr u32 hdr_rcode(const u32 h[4]) { return (h[1] & 0x0000f000u) >> 12; }
constexpr u64 hdr_offset(const u32 h[4]) {
    return (static_cast<u64>(h[1] & 0x0000ffffu) << 32) | h[2];
}
constexpr u32 hdr_quadlet_data(const u32 h[4]) { return h[3]; }
constexpr u32 hdr_data_length(const u32 h[4]) { return (h[3] & 0xffff0000u) >> 16; }
constexpr u32 hdr_extended_tcode(const u32 h[4]) { return h[3] & 0x0000ffffu; }

// Assemble header quadlet 0/1/2 shared by every request (dest, tlabel, tcode, 48-bit offset).
constexpr void set_request_common(u32 h[4], u16 dest, u32 tlabel, u32 tcode, Addr offset) {
    h[0] = (static_cast<u32>(dest) << 16) | ((tlabel & 0x3f) << 10) | (kRetryX << 8) |
           ((tcode & 0xf) << 4);
    h[1] = static_cast<u32>(addr_hi(offset));  // source (bits 31:16) filled by the controller
    h[2] = addr_lo(offset);
}

// ── Request builders ─────────────────────────────────────────────────────────────────────────
inline Packet read_quadlet_request(u16 dest, u32 tlabel, Addr offset) {
    Packet p;
    set_request_common(p.header, dest, tlabel, kReadQuadletReq, offset);
    p.count = 3;
    return p;
}

inline Packet write_quadlet_request(u16 dest, u32 tlabel, Addr offset, u32 value) {
    Packet p;
    set_request_common(p.header, dest, tlabel, kWriteQuadletReq, offset);
    p.header[3] = value;
    p.count = 4;
    return p;
}

inline Packet read_block_request(u16 dest, u32 tlabel, Addr offset, u32 length) {
    Packet p;
    set_request_common(p.header, dest, tlabel, kReadBlockReq, offset);
    p.header[3] = (length & 0xffffu) << 16;  // extended_tcode 0
    p.count = 4;
    return p;
}

// Block write. `data` is the payload (already LE-serialized quadlets, e.g. the CR block).
inline Packet write_block_request(u16 dest, u32 tlabel, Addr offset, std::vector<u8> data) {
    Packet p;
    set_request_common(p.header, dest, tlabel, kWriteBlockReq, offset);
    p.header[3] = (static_cast<u32>(data.size()) & 0xffffu) << 16;
    p.payload = std::move(data);
    p.count = 4;
    return p;
}

// Compare-swap lock (the only lock UFAsyncIO needs). Payload = arg_value then data_value (LE).
inline Packet lock_compare_swap_request(u16 dest, u32 tlabel, Addr offset, u32 arg, u32 data) {
    Packet p;
    set_request_common(p.header, dest, tlabel, kLockReq, offset);
    p.payload.resize(8);
    host_to_le32(arg, &p.payload[0]);
    host_to_le32(data, &p.payload[4]);
    p.header[3] = (8u << 16) | kExtCompareSwap;
    p.count = 4;
    return p;
}

// ── OHCI AT immediate header ──────────────────────────────────────────────────────────────────
// The OHCI async-transmit header stored inline in the KEY_IMMEDIATE descriptor differs from the
// on-bus IEEE header (async.hpp Packet): the destination id moves to Q1, and the transfer speed is
// embedded in Q0. Ported from ohci.h OHCI1394_AT_DATA_* + at_context_queue_packet (GPL-2.0). This
// is what the dext copies into d[1..] of the AT descriptor block (OHCI-2).
struct AtHeader {
    u32 q[4] = {0, 0, 0, 0};
    unsigned count = 0;   // header quadlets: 3 (read/quadlet-write) or 4 (block / lock)
};

// Repack a built request Packet into the OHCI AT header for the given transfer speed.
inline AtHeader build_at_header(const Packet& pkt, Scode speed) {
    AtHeader h{};
    const u32 tcode = hdr_tcode(pkt.header);
    const u32 tlabel = hdr_tlabel(pkt.header);
    const u32 retry = hdr_retry(pkt.header);
    const u32 dest = hdr_destination(pkt.header);
    const u64 offset = hdr_offset(pkt.header);
    // Q0: srcBusID=0 | spd | tLabel | rt | tCode  (destination is NOT here in the AT format).
    h.q[0] = ((static_cast<u32>(speed) & 0x7) << 16) | (tlabel << 10) | (retry << 8) | (tcode << 4);
    // Q1: destinationId | destinationOffsetHigh.
    h.q[1] = (dest << 16) | static_cast<u32>((offset >> 32) & 0xffff);
    // Q2: destinationOffsetLow.
    h.q[2] = static_cast<u32>(offset & 0xffffffff);
    // Q3: quadlet data, or [data_length | extended_tcode] for block/lock (same as pkt.header[3]).
    h.q[3] = pkt.header[3];
    h.count = pkt.count;  // mirrors the request (3 = 12-byte header, 4 = 16-byte header)
    return h;
}

// ── Response parsing ─────────────────────────────────────────────────────────────────────────
struct Response {
    u32 tcode;
    u32 rcode;
    u32 tlabel;
    u32 quadlet;          // read-quadlet-response data (valid for kReadQuadletResp)
    u32 data_length;      // read-block-response payload length
};

inline Response parse_response(const u32 h[4]) {
    Response r{};
    r.tcode = hdr_tcode(h);
    r.rcode = hdr_rcode(h);
    r.tlabel = hdr_tlabel(h);
    if (r.tcode == kReadQuadletResp) r.quadlet = hdr_quadlet_data(h);
    if (r.tcode == kReadBlockResp || r.tcode == kLockResp) r.data_length = hdr_data_length(h);
    return r;
}

}  // namespace uf::ohci
