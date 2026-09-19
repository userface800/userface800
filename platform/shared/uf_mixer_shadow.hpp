// uf_mixer_shadow.hpp — the daemon's copy of what the FF800's mixer matrix is supposed to contain.
//
// The problem this exists to solve: the mixer RAM is write-only from our side and the device forgets
// it. `session_start` used to zero all 0x2000 bytes and write a hardcoded default, which meant every
// user route was destroyed by a daemon start, a SAMPLE-RATE CHANGE, wedge recovery or a bus reset —
// i.e. monitoring set with uf-mix survived only until you next changed rate.
//
// So the shadow is the source of truth and the device is a write target, exactly as
// uf::SettingsShadow already does for the write-only control registers. Session start re-applies the
// shadow instead of a hardcoded default; a runtime change updates the shadow and writes one cell.
//
// Modelled as the raw 2048-quadlet RAM rather than as typed src/dest arrays. That is deliberate: the
// layout has a genuine overlap — the per-output fader row lives at 0x1f80, which is inside
// destination 31's playback block (31*0x100 + 0x80) — so a "32 dests x 32 srcs" model would be a
// lie. Addressing comes from uf/protocol/mixer.hpp, which is spec/09-validated; this only stores.
//
// Pure logic, no IOKit: unit-tested in tests/test_mixer_shadow.cpp.
#pragma once
#include <array>
#include <cstdint>
#include "uf/protocol/mixer.hpp"
#include "uf/protocol/registers.hpp"

namespace uf {

class MixerShadow {
public:
    static constexpr uint32_t kBytes = 0x2000;          // the whole matrix RAM
    static constexpr uint32_t kCells = kBytes / 4;      // 2048 quadlets

    MixerShadow() { cells_.fill(0); }

    void clear() { cells_.fill(0); }

    // The routing the daemon has always come up with: everything muted, then each playback channel
    // to its own physical output at unity, and the per-output faders at unity (zeroing set them to
    // 0, i.e. mute). `dbq` is the channel count at the current rate — 28 at 1x, 20 at 2x, 12 at 4x.
    //
    // `diag` mirrors the daemon's UF_NODIAG escape hatch: when false the playback->output cells are
    // left muted and only the faders come up. Kept so the shadow can reproduce that exactly rather
    // than quietly changing behaviour.
    void reset_defaults(uint32_t dbq, bool diag = true) {
        cells_.fill(0);
        const uint32_t n = dbq < kMixerChannels ? dbq : kMixerChannels;
        for (uint32_t i = 0; i < n; ++i) {
            if (diag) set_addr(playback_coeff_addr(i, i), kMixerUnity);
            set_addr(output_fader_addr(i), kMixerUnity);
        }
    }

    // Setters return the ABSOLUTE address written, so a caller can push exactly that one cell to the
    // device without re-deriving the address (and without re-applying all 2048).
    Addr set_input(uint32_t src, uint32_t dest, uint32_t coeff) {
        return set_addr(input_coeff_addr(src, dest), coeff);
    }
    Addr set_playback(uint32_t src, uint32_t dest, uint32_t coeff) {
        return set_addr(playback_coeff_addr(src, dest), coeff);
    }
    Addr set_fader(uint32_t out, uint32_t coeff) {
        return set_addr(output_fader_addr(out), coeff);
    }

    // Write by absolute address, for a caller that already has one — MixerModel renders a cell to
    // an (address, value) pair and would otherwise have to re-derive which setter it corresponds to.
    Addr set_at(Addr addr, uint32_t quadlet) { return set_addr(addr, quadlet); }

    // Restore path: the saved file is the raw cell array, so it is written back by index. Bounds
    // are checked here rather than trusted, since the array came off disk.
    void set_cell(uint32_t i, uint32_t coeff) { if (i < kCells) cells_[i] = coeff; }

    uint32_t at(Addr addr) const {
        const uint32_t i = index_of(addr);
        return i < kCells ? cells_[i] : 0;
    }
    uint32_t cell(uint32_t i) const { return i < kCells ? cells_[i] : 0; }

    // Push the whole matrix. Every cell, unconditionally — after a session start the device's RAM
    // holds power-on garbage, so cells we believe are zero still have to be written as zero. This is
    // the same 2048 quadlet writes the old inline code did; it is not a new cost.
    //
    // FFADO writes the mixer one QUADLET at a time and the FF800 may ignore block writes, which
    // would leave the matrix full of garbage the unmuted outputs then play — the original "beep".
    // So: quadlets, and the caller does not get to batch them.
    template <class WriteQuadlet>
    void apply_all(WriteQuadlet&& wq) const {
        for (uint32_t i = 0; i < kCells; ++i)
            wq(reg::kMixerRam + (Addr)i * 4, cells_[i]);
    }

private:
    static uint32_t index_of(Addr addr) {
        return (uint32_t)((addr - reg::kMixerRam) / 4);
    }
    Addr set_addr(Addr addr, uint32_t coeff) {
        const uint32_t i = index_of(addr);
        if (i < kCells) cells_[i] = coeff;
        return addr;
    }

    std::array<uint32_t, kCells> cells_{};
};

}  // namespace uf
