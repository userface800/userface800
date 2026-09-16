// uf_control.hpp — the clock-source vocabulary shared by the plugin and the daemon.
//
// CoreAudio's clock-source property is a list of opaque UInt32 IDs plus a name for each; the FF800's
// surface is a master-mode bit plus a SYNC_REF field (uf::SyncRef). This is the mapping between them,
// in one place, because both ends have to agree on the numbering: the plugin publishes the IDs to
// Audio MIDI Setup and the daemon turns the chosen one back into register bits.
//
// The IDs are our own and must stay stable — a DAW or the OS may remember a device's clock source by
// ID across launches, so renumbering silently changes what a saved setup selects. Append only.
#pragma once
#include <cstdint>
#include "uf/protocol/settings.hpp"
#include "uf/protocol/status.hpp"

namespace uf::ctl {

enum class ClockSource : uint32_t {
    Internal  = 0,   // the FF800's own crystal — master
    WordClock = 1,
    Adat1     = 2,
    Adat2     = 3,
    Spdif     = 4,
    Tco       = 5,   // the optional time-code option board
};

inline constexpr ClockSource kAllClockSources[] = {
    ClockSource::Internal, ClockSource::WordClock, ClockSource::Adat1,
    ClockSource::Adat2, ClockSource::Spdif, ClockSource::Tco,
};

inline constexpr const char* clock_source_name(ClockSource s) {
    switch (s) {
        case ClockSource::Internal:  return "Internal";
        case ClockSource::WordClock: return "Word Clock";
        case ClockSource::Adat1:     return "ADAT 1";
        case ClockSource::Adat2:     return "ADAT 2";
        case ClockSource::Spdif:     return "SPDIF";
        case ClockSource::Tco:       return "TCO";
    }
    return "?";
}

inline constexpr bool clock_source_is_master(ClockSource s) {
    return s == ClockSource::Internal;
}

// The SYNC_REF the device should slave to. Meaningless for Internal (master mode ignores it); we
// return Adat1 there only so the field holds something defined.
inline constexpr SyncRef clock_source_sync_ref(ClockSource s) {
    switch (s) {
        case ClockSource::WordClock: return SyncRef::WordClock;
        case ClockSource::Adat1:     return SyncRef::Adat1;
        case ClockSource::Adat2:     return SyncRef::Adat2;
        case ClockSource::Spdif:     return SyncRef::Spdif;
        case ClockSource::Tco:       return SyncRef::Tco;
        case ClockSource::Internal:  break;
    }
    return SyncRef::Adat1;
}

inline constexpr bool is_valid_clock_source(uint32_t v) {
    return v <= static_cast<uint32_t>(ClockSource::Tco);
}

// Reverse: what the device reports it is actually locked to (status.hpp SyncSource) as one of our
// IDs. The device's own view is the truth — it can fall back to internal when an external clock
// disappears, and CoreAudio should be told what is really driving the converters, not what we asked
// for. `master` comes from SR1's clock-master bit.
inline constexpr ClockSource clock_source_from_status(bool master, SyncSource src) {
    if (master) return ClockSource::Internal;
    switch (src) {
        case SyncSource::WordClock: return ClockSource::WordClock;
        case SyncSource::Adat1:     return ClockSource::Adat1;
        case SyncSource::Adat2:     return ClockSource::Adat2;
        case SyncSource::Spdif:     return ClockSource::Spdif;
        case SyncSource::Tco:       return ClockSource::Tco;
        case SyncSource::None:      break;   // device reports no external reference
    }
    return ClockSource::Internal;
}

}  // namespace uf::ctl
