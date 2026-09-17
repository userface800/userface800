// endian.hpp — little-endian quadlet pack/unpack for the FF800 protocol.
//
// The FireWire wire format for the FF800 register/stream data is little-endian 32-bit
// quadlets (spec/01, spec/03). The macOS UFAsyncIO layer handles the wire byte-swap for
// register transactions; this header provides the pure host<->LE conversions the streaming
// codec and test vectors need, independent of the host's native byte order.
#pragma once
#include <cstdint>

namespace uf {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

// A FireWire quadlet: a 32-bit register/stream word, in host order once decoded.
using Quadlet = std::uint32_t;

// Read a little-endian 32-bit value from a 4-byte buffer -> host order.
constexpr u32 le32_to_host(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}

// Write a host-order 32-bit value to a 4-byte buffer as little-endian.
constexpr void host_to_le32(u32 v, u8* p) {
    p[0] = static_cast<u8>(v & 0xff);
    p[1] = static_cast<u8>((v >> 8) & 0xff);
    p[2] = static_cast<u8>((v >> 16) & 0xff);
    p[3] = static_cast<u8>((v >> 24) & 0xff);
}

}  // namespace uf
