// registers.hpp — FF800 register addresses, bitfield masks, and clock/rate codes (portable).
//
// A faithful transcription of the register *facts* from spec/02 §2.1/§2.3 and spec/06 (the
// complete CR0/CR1/CR2 + SR0/SR1 bit tables). These are constants only — the assembly/decode
// logic lives in the settings, status, rate and mixer modules.
//
// Facts re-expressed as fresh named constants (not a copy of FFADO fireface_def.h / snd-fireface
// structure). Values verified against the spec/09 worked examples.
#pragma once
#include "uf/protocol/endian.hpp"

namespace uf {

// A full 48-bit FireWire target address: address = (hi << 32) | lo (spec/01 §1.2). Register
// constants below are the full 48-bit value; addr_hi/addr_lo split them for FWAddress on the host.
using Addr = u64;
constexpr u16 addr_hi(Addr a) { return static_cast<u16>((a >> 32) & 0xffff); }
constexpr u32 addr_lo(Addr a) { return static_cast<u32>(a & 0xffffffff); }

// ── Register addresses (spec/02 §2.1) ────────────────────────────────────────────────────────
namespace reg {

// Streaming / clock surface.
inline constexpr Addr kStf = 0xfc88f000;             // W: set streaming sample rate (raw Hz)
inline constexpr Addr kRxPacketFormat = 0xfc88f004;  // W: ((dbq<<3)<<8) | rx_iso_channel
inline constexpr Addr kAllocTxStream = 0xfc88f008;   // W: data_block_quadlets
inline constexpr Addr kIsocCommStart = 0xfc88f00c;   // W: 0x80000000 | dbq | (S800?0x800:0)
inline constexpr Addr kIsocCommStop = 0xfc88f010;    // W: 0x80000000
inline constexpr Addr kConfBlock = 0xfc88f014;       // W(3 quad): CR0/CR1/CR2 settings block

// The FF800 also accepts streaming setup through the 0x0002 command bank instead of the fc88f…
// bank above; both work on hardware. On this bank the init is one 3-quad block, not three separate
// writes.
inline constexpr Addr kInitBankStream = 0x20000001c; // W(3 quad): {rate, (dbq<<11)|rxCh, dbq[|0x800]}
inline constexpr Addr kInitBankStart  = 0x200000028; // W: 0x80000000 | dbq | (S800?0x800:0)
inline constexpr Addr kInitBankStop   = 0x200000034; // W(3 quad): stop blob (three zero quadlets)

// Status / control surface.
inline constexpr Addr kStatus0 = 0x801c0000;         // R: SR0; W: channel-mute mask
inline constexpr Addr kClockConfig = 0x801c0004;     // R/W: clock config + SR1
inline constexpr Addr kTxIsoChannel = 0x801c0008;    // R: device-assigned tx channel (0xffffffff until ready)
inline constexpr Addr kStatus3 = 0x801c001c;         // R: status 3
// W: per-output record mask — the mixer's "Loopback" (manual §27.5). A set output's MIX is
// sent to
// the recording software in place of the corresponding hardware input. 28 quadlets, block-written.
inline constexpr Addr kOutputRecMask = 0x801c0080;

// Mixer / MIDI / misc.
inline constexpr Addr kMixerRam = 0x80080000;        // W: matrix mixer RAM
// R: level meters. HARDWARE-CONFIRMED pollable. 8 bytes per channel, channel N at N*8, reading as
// a little-endian u64 accumulator whose absolute scaling is still open (spec/10 §10.3).
inline constexpr Addr kMeterBase = 0x80100000;
inline constexpr Addr kMidiOut = 0x80180000;         // W: MIDI out (host->device), 1 byte/LE quadlet
inline constexpr Addr kMidiHighAddr = 0x200000320;   // W: MIDI host-receive high-addr register
inline constexpr Addr kFirmwareRev = 0x200000100;    // R: firmware revision
inline constexpr Addr kHostLed = 0x200000324;        // W: host LED

// FF800 flash (persist settings to device NVRAM; facts from FFADO fireface_flash.cpp). Erase = write
// 0 to the erase reg then poll SR1 bit 30; read/write the data region in <=256 B (64-quad) sectors.
inline constexpr Addr kFlashSettings   = 0x3000f0000; // R/W: settings record (survives power-off)
inline constexpr Addr kFlashMixerShadow = 0x3000e0000;// R/W: mixer state (0x2000 bytes)
inline constexpr Addr kFlashEraseSettings = 0x3fffffff0; // W 0: erase the settings block
inline constexpr u32  kFlashBusyBit    = 0x40000000;  // SR1 (0x801c0004) bit: set = flash ready
inline constexpr u32  kFlashSectorQuads = 64;         // 256-byte flash sector

}  // namespace reg

// ── CR0 (conf-block quadlet 0), spec/06 ─────────────────────────────────────────────────────
namespace cr0 {
inline constexpr u32 kPhantomMic7 = 0x00000001;
inline constexpr u32 kPhantomMic9 = 0x00000002;
inline constexpr u32 kFilterCh1 = 0x00000004;    // speaker emulation (FPGA)
inline constexpr u32 kPhantomMic8 = 0x00000080;
inline constexpr u32 kPhantomMic10 = 0x00000100;
inline constexpr u32 kDriveCh1 = 0x00000200;     // instrument (FPGA)
// Input level (FPGA / FF800 LED mirror), one-hot.
inline constexpr u32 kInLevelLoGain = 0x00000008;
inline constexpr u32 kInLevelP4dBu = 0x00000010;
inline constexpr u32 kInLevelM10dBV = 0x00000020;
// Output level (FPGA / FF800 LED mirror), one-hot.
inline constexpr u32 kOutLevelHiGain = 0x00000400;
inline constexpr u32 kOutLevelP4dBu = 0x00000800;
inline constexpr u32 kOutLevelM10dBV = 0x00001000;
// Phones level.
inline constexpr u32 kPhonesP4dBu = 0x00000000;
inline constexpr u32 kPhonesM10dBV = 0x00010000;
inline constexpr u32 kPhonesHiGain = 0x00020000;
}  // namespace cr0

// ── CR1 (conf-block quadlet 1), spec/06 — the *actual* level (CR0 is the LED mirror) ────────
namespace cr1 {
inline constexpr u32 kInput1Rear = 0x00000004;       // opt0 B
inline constexpr u32 kInput7Front = 0x00000020;      // opt1 A
inline constexpr u32 kInput7Rear = 0x00000040;       // opt1 B
inline constexpr u32 kInput8Front = 0x00000080;      // opt2 A
inline constexpr u32 kInput8Rear = 0x00000100;       // opt2 B
inline constexpr u32 kInstrumentDrive = 0x00000200;  // CPLD
inline constexpr u32 kInput1FrontFilter = 0x00000400;  // opt0 A1 (filter/speaker-emu on)
inline constexpr u32 kInput1Front = 0x00000800;        // opt0 A0 (normal)
// Input level (CPLD): LoGain=0, +4dBu=0x02, -10dBV=0x03.
inline constexpr u32 kInLevelLoGain = 0x00000000;
inline constexpr u32 kInLevelP4dBu = 0x00000002;
inline constexpr u32 kInLevelM10dBV = 0x00000003;
// Output level (CPLD): -10dBV=0x08, HiGain=0x10, +4dBu=0x18.
inline constexpr u32 kOutLevelM10dBV = 0x00000008;
inline constexpr u32 kOutLevelHiGain = 0x00000010;
inline constexpr u32 kOutLevelP4dBu = 0x00000018;
}  // namespace cr1

// ── CR2 (conf-block quadlet 2) — clock mode, SPDIF format, sync ref, defaults, spec/06 ──────
namespace cr2 {
inline constexpr u32 kClockMaster = 0x00000001;      // 1=master, 0=autosync
inline constexpr u32 kFreq0 = 0x00000002;            // leave 0
inline constexpr u32 kFreq1 = 0x00000004;            // leave 0
inline constexpr u32 kDSpeed = 0x00000008;           // leave 0
inline constexpr u32 kQsSpeed = 0x00000010;          // leave 0
inline constexpr u32 kSpdifOutPro = 0x00000020;
inline constexpr u32 kSpdifOutEmphasis = 0x00000040;
inline constexpr u32 kSpdifOutNonAudio = 0x00000080;
inline constexpr u32 kSpdifOutOptical = 0x00000100;  // on ADAT2 port
inline constexpr u32 kSpdifInOptical = 0x00000200;   // 0=coax
inline constexpr u32 kSyncRef0 = 0x00000400;
inline constexpr u32 kSyncRef1 = 0x00000800;
inline constexpr u32 kSyncRef2 = 0x00001000;
inline constexpr u32 kWordClock1x = 0x00002000;      // single-speed word clock
inline constexpr u32 kToggleTco = 0x00004000;        // normally 0
inline constexpr u32 kDisableLimiter = 0x00010000;   // P12dB, normally 0
inline constexpr u32 kTms = 0x40000000;              // normally 0
inline constexpr u32 kDropAndStop = 0x80000000;      // normally SET
// Sync-reference selection (SYNC_REF bits), spec/06.
inline constexpr u32 kSyncRefAdat1 = 0x00000000;
inline constexpr u32 kSyncRefAdat2 = 0x00000400;
inline constexpr u32 kSyncRefSpdif = 0x00000c00;
inline constexpr u32 kSyncRefWordClock = 0x00001000;
inline constexpr u32 kSyncRefTco = 0x00001400;
}  // namespace cr2

// ── SR0 — status register 0 (read 0x801c0000), spec/06 ──────────────────────────────────────
namespace sr0 {
inline constexpr u32 kAdat1Lock = 0x00000400;
inline constexpr u32 kAdat2Lock = 0x00000800;
inline constexpr u32 kAdat1Sync = 0x00001000;
inline constexpr u32 kAdat2Sync = 0x00002000;
inline constexpr u32 kSpdifSync = 0x00040000;
inline constexpr u32 kOver = 0x00080000;             // clipping
inline constexpr u32 kSpdifLock = 0x00100000;
inline constexpr u32 kWordClockSync = 0x20000000;
inline constexpr u32 kWordClockLock = 0x40000000;
inline constexpr u32 kExtRateMask = 0x000003ff;      // external sample rate = value * 250
inline constexpr u32 kExtRateUnit = 250;
// Selected sync source (bits 22-24).
inline constexpr u32 kSyncSrcMask = 0x01c00000;
inline constexpr u32 kSyncSrcAdat1 = 0x00000000;
inline constexpr u32 kSyncSrcAdat2 = 0x00400000;
inline constexpr u32 kSyncSrcSpdif = 0x00c00000;
inline constexpr u32 kSyncSrcWordClock = 0x01000000;
inline constexpr u32 kSyncSrcTco = 0x01400000;
inline constexpr u32 kSyncSrcNone = 0x01800000;
// Autosync input freq (bits 25-28), value 1..9 * this weight.
inline constexpr u32 kInputFreqMask = 0x1e000000;
inline constexpr u32 kInputFreqShift = 25;
// SPDIF freq (bits 14-17), value 1..9 * this weight.
inline constexpr u32 kSpdifFreqMask = 0x0003c000;
inline constexpr u32 kSpdifFreqShift = 14;
}  // namespace sr0

// ── SR1 — status register 1 (read 0x801c0004), spec/06 ──────────────────────────────────────
namespace sr1 {
inline constexpr u32 kClockMaster = 0x00000001;
inline constexpr u32 kTcoSync = 0x00400000;
inline constexpr u32 kTcoLock = 0x00800000;
}  // namespace sr1

// ── Clock-config write field (0x801c0004), spec/02 §2.3 ─────────────────────────────────────
namespace clockcfg {
inline constexpr u32 kRateMask = 0x0000001e;    // rate code, see rate.hpp
inline constexpr u32 kClockModeMaster = 0x00000001;
inline constexpr u32 kSourceMask = 0x00001c00;
inline constexpr u32 kSourceAdat1 = 0x00000000;
inline constexpr u32 kSourceAdat2 = 0x00000400;
inline constexpr u32 kSourceSpdif = 0x00000c00;
inline constexpr u32 kSourceWordClock = 0x00001000;
inline constexpr u32 kSourceTco = 0x00001800;   // LTC/TCO
}  // namespace clockcfg

}  // namespace uf
