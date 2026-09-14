// status.hpp — decode FF800 status registers SR0/SR1 (portable).
//
// SR0 = read 0x801c0000, SR1 = read 0x801c0004 (spec/02 §2.4, spec/06 §SR0/§SR1). Pure decode:
// the register reads themselves are the host UFAsyncIO path. Validated against spec/09 §9.3.
#pragma once
#include "uf/protocol/endian.hpp"
#include "uf/protocol/registers.hpp"

namespace uf {

// The clock source the device reports it is currently locked/synced to (SR0 bits 22-24).
enum class SyncSource : u32 { Adat1, Adat2, Spdif, WordClock, Tco, None };

// Decoded device status. Frequencies are Hz (0 = not present / unlocked).
struct UFStatus {
    // Per-source lock/sync (SR0).
    bool adat1_lock, adat1_sync;
    bool adat2_lock, adat2_sync;
    bool spdif_lock, spdif_sync;
    bool wclk_lock, wclk_sync;
    bool over;  // clipping

    SyncSource sync_source;   // selected/active sync reference
    u32 ext_rate_hz;          // external sample rate when slaved (SR0 low 10 bits * 250)
    u32 input_freq_hz;        // autosync source frequency (SR0 bits 25-28)
    u32 spdif_freq_hz;        // SPDIF input frequency (SR0 bits 14-17)

    // SR1.
    bool clock_master;        // internal (master) clock
    bool tco_lock, tco_sync;
};

// The device's 1..9 frequency index -> Hz (spec/06: shared by autosync and SPDIF freq fields).
constexpr u32 freq_index_to_hz(u32 idx) {
    switch (idx) {
        case 1: return 32000;
        case 2: return 44100;
        case 3: return 48000;
        case 4: return 64000;
        case 5: return 88200;
        case 6: return 96000;
        case 7: return 128000;
        case 8: return 176400;
        case 9: return 192000;
        default: return 0;
    }
}

constexpr SyncSource decode_sync_source(u32 r0) {
    switch (r0 & sr0::kSyncSrcMask) {
        case sr0::kSyncSrcAdat1: return SyncSource::Adat1;
        case sr0::kSyncSrcAdat2: return SyncSource::Adat2;
        case sr0::kSyncSrcSpdif: return SyncSource::Spdif;
        case sr0::kSyncSrcWordClock: return SyncSource::WordClock;
        case sr0::kSyncSrcTco: return SyncSource::Tco;
        default: return SyncSource::None;  // includes kSyncSrcNone (0x1800000)
    }
}

// Decode both status registers into a structured status (spec/09 §9.3). Params are the raw
// SR0/SR1 quadlet contents; named r0/r1 to avoid shadowing the uf::sr0/uf::sr1 mask namespaces.
constexpr UFStatus decode_status(u32 r0, u32 r1) {
    UFStatus s{};
    s.adat1_lock = r0 & sr0::kAdat1Lock;
    s.adat1_sync = r0 & sr0::kAdat1Sync;
    s.adat2_lock = r0 & sr0::kAdat2Lock;
    s.adat2_sync = r0 & sr0::kAdat2Sync;
    s.spdif_lock = r0 & sr0::kSpdifLock;
    s.spdif_sync = r0 & sr0::kSpdifSync;
    s.wclk_lock = r0 & sr0::kWordClockLock;
    s.wclk_sync = r0 & sr0::kWordClockSync;
    s.over = r0 & sr0::kOver;

    s.sync_source = decode_sync_source(r0);
    s.ext_rate_hz = (r0 & sr0::kExtRateMask) * sr0::kExtRateUnit;
    s.input_freq_hz = freq_index_to_hz((r0 & sr0::kInputFreqMask) >> sr0::kInputFreqShift);
    s.spdif_freq_hz = freq_index_to_hz((r0 & sr0::kSpdifFreqMask) >> sr0::kSpdifFreqShift);

    s.clock_master = r1 & sr1::kClockMaster;
    s.tco_lock = r1 & sr1::kTcoLock;
    s.tco_sync = r1 & sr1::kTcoSync;
    return s;
}

}  // namespace uf
