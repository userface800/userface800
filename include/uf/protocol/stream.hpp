// stream.hpp — FF800 isoch stream start/stop register sequence (portable planner).
//
// The streaming lifecycle register writes (spec/03 §3.6, spec/09 §9.4). This is the pure value +
// ordering logic; the host UFStreamEngine executes these RegWrites via UFAsyncIO and interleaves the
// host-only steps it can't express here: polling the device-assigned TX channel (0x801c0008 until
// != 0xffffffff), allocating the RX iso channel + bus bandwidth, and starting the OHCI IR/IT
// contexts. Adapted from snd-fireface ff800_allocate_resources / begin_session / finish_session
// (GPL-2.0). Validated against spec/09 §9.4.
#pragma once
#include <vector>
#include "uf/protocol/channels.hpp"
#include "uf/protocol/control.hpp"
#include "uf/protocol/endian.hpp"
#include "uf/protocol/registers.hpp"

namespace uf {

// FireWire bus speed the session runs at (affects the comm-start speed flag).
enum class BusSpeed { S400, S800 };

// RX_PACKET_FORMAT (0xfc88f004): ((data_block_quadlets << 3) << 8) | rx_iso_channel (spec/03 §3.6).
constexpr u32 rx_packet_format_value(u32 dbq, u32 rx_channel) {
    return ((dbq << 3) << 8) | rx_channel;
}
inline RegWrite rx_packet_format_write(u32 dbq, u32 rx_channel) {
    return quadlet_write(reg::kRxPacketFormat, rx_packet_format_value(dbq, rx_channel));
}

// ALLOC_TX_STREAM (0xfc88f008): data_block_quadlets (spec/03 §3.6).
inline RegWrite alloc_tx_stream_write(u32 dbq) { return quadlet_write(reg::kAllocTxStream, dbq); }

// ISOC_COMM_START (0xfc88f00c): 0x80000000 | dbq | (S800 ? 0x800 : 0) (spec/03 §3.6).
constexpr u32 comm_start_value(u32 dbq, BusSpeed bus) {
    return 0x80000000u | dbq | (bus == BusSpeed::S800 ? 0x800u : 0u);
}
inline RegWrite comm_start_write(u32 dbq, BusSpeed bus) {
    return quadlet_write(reg::kIsocCommStart, comm_start_value(dbq, bus));
}

// ISOC_COMM_STOP (0xfc88f010): 0x80000000 (spec/03 §3.6).
inline RegWrite comm_stop_write() { return quadlet_write(reg::kIsocCommStop, 0x80000000u); }

// The device-register writes to start a session, in order (spec/03 §3.6): RX packet format, TX
// stream allocation, then begin-session. The rate write (STF) precedes these (UFControl::rate_write,
// with a ~100 ms settle), and the host performs the TX-channel poll + RX bandwidth/channel
// allocation + OHCI context starts around them (see file header). `rx_channel` is the host-allocated
// playback channel; `dbq` = data_block_quadlets for the active (speed, mode).
inline std::vector<RegWrite> stream_start_writes(u32 dbq, u32 rx_channel, BusSpeed bus) {
    return {rx_packet_format_write(dbq, rx_channel), alloc_tx_stream_write(dbq), comm_start_write(dbq, bus)};
}

// Convenience: derive dbq from (speed, mode).
inline std::vector<RegWrite> stream_start_writes(Speed s, BwLimit m, u32 rx_channel, BusSpeed bus) {
    return stream_start_writes(data_block_quadlets(s, m), rx_channel, bus);
}

}  // namespace uf
