// uf_mixer_model.hpp — the LOGICAL mixer: what the user means, above what the device stores.
//
// MixerShadow (uf_mixer_shadow.hpp) is a device image: 2048 raw quadlets, one per matrix cell. That
// is enough to re-apply a routing after a rate change, and it was enough while the only operation
// was "set this crosspoint to this gain". It is not enough for TotalMix's actual feature set, and
// the reason is worth stating precisely, because it is the whole justification for this file:
//
//   **Muting a crosspoint writes 0 to it.** If 0 is all we store, the gain that was there is gone,
//   and un-muting cannot put it back. Mute is not a value, it is a modifier ON a value.
//
// Phase invert has the same shape (it negates the coefficient, so the magnitude must survive), and
// so does solo (mute everything else, then restore). FFADO hit this too and solved it identically:
// FF_software_settings_t carries `input_faders[]` alongside `input_mixerflags[]`, and setMixerGain()
// consults the flags on the way to set_hardware_mixergain(). This is that, in our shape.
//
// ## What is device state and what is not
//
// Reading the FF800 manual (§25-§27) against FFADO's register code, TotalMix's features split
// cleanly, and only the first group exists in hardware:
//
//   DEVICE   crosspoint gains (signed: sign = phase), per-output faders, the channel-mute mask at
//            0x801c0000, and the output-record mask at 0x801c0080 (= "Loopback").
//   HOST     stereo pairing, pan, mute, solo, width, M/S, trim, cue, talkback, groups, snapshots.
//
// Everything in the second group is TotalMix computing crosspoints. Pan is the clearest case: a
// source panned between a stereo output pair is just its two crosspoints at different gains, which
// is why the manual can say the Matrix "operates monaural" (§26.3) and still be a complete view of
// the mixer. So this model does NOT store pan: it stores crosspoints, and offers pan as an EDIT that
// writes two of them. State that can be derived from the matrix is not state.
//
// What genuinely needs storing beyond the matrix is only: the per-crosspoint mute/invert flags, the
// per-output loopback flag, and which channels the user has paired into stereo (a display and
// edit-linking choice the hardware knows nothing about).
//
// Pure logic, no IOKit — unit-tested in tests/test_mixer_model.cpp.
#pragma once
#include <array>
#include <cstdint>

#include "uf/protocol/mixer.hpp"
#include "uf_mixer_shadow.hpp"

namespace uf {

// Crosspoint / output flags. Same bit values as FFADO's FF_SWPARAM_MF_* so the two are readable
// side by side; there is no wire format here to be compatible with, only a reader.
inline constexpr uint8_t kMfNone     = 0x00;
inline constexpr uint8_t kMfMuted    = 0x01;   // crosspoints and outputs
inline constexpr uint8_t kMfInverted = 0x02;   // crosspoints only — phase 180°
inline constexpr uint8_t kMfRec      = 0x04;   // OUTPUTS only — TotalMix "Loopback"

class MixerModel {
public:
    static constexpr uint32_t kCh = kMixerChannels;   // 28 on the FF800

    // A crosspoint. `gain` is a MAGNITUDE (0 … 0x10000, 0x8000 = unity) and never negative: the sign
    // that reaches the device comes from kMfInverted. Keeping them apart is what lets un-inverting
    // restore the gain, exactly as un-muting does.
    struct Cell {
        uint32_t gain  = kMixerMute;
        uint8_t  flags = kMfNone;
    };

    // TotalMix's default and ours: every input crosspoint down, each playback channel to its own
    // physical output at unity, every output fader at unity. `dbq` is the channel count at the
    // current rate (28 / 20 / 12); `diag` mirrors the daemon's UF_NODIAG escape hatch.
    void reset_defaults(uint32_t dbq, bool diag = true) {
        *this = MixerModel{};
        const uint32_t n = dbq < kCh ? dbq : kCh;
        for (uint32_t i = 0; i < n; ++i) {
            if (diag) playback_[i][i].gain = kMixerUnity;
            outputs_[i].gain = kMixerUnity;
        }
    }

    // Crosspoint access. Out-of-range lands on a scratch cell rather than reading past the array:
    // channel indices arrive from a socket and from files, so they are not trusted.
    Cell& cell(MixerSrcKind kind, uint32_t src, uint32_t dest) {
        if (src >= kCh || dest >= kCh) return scratch_;
        return kind == MixerSrcKind::Playback ? playback_[src][dest] : inputs_[src][dest];
    }
    const Cell& cell(MixerSrcKind kind, uint32_t src, uint32_t dest) const {
        return const_cast<MixerModel*>(this)->cell(kind, src, dest);
    }
    Cell& output(uint32_t out) { return out < kCh ? outputs_[out] : scratch_; }
    const Cell& output(uint32_t out) const { return const_cast<MixerModel*>(this)->output(out); }

    // Stereo pairing. Purely a host-side notion — the device has no idea — but it decides how a UI
    // draws a channel and whether an edit applies to one channel or two.
    //
    // Stored per PAIR (adjacent even/odd), and SEPARATELY for inputs and playback, because the
    // manual keeps them separate: Stereo is a per-channel setting on the input and playback rows
    // (§25.3), while "Hardware Outputs are always stereo" — so there is no third mask, and one
    // shared mask would mean pairing input 1/2 silently paired playback 1/2 too.
    bool stereo(MixerSrcKind kind, uint32_t ch) const {
        return ch < kCh && ((bits(kind) >> (ch / 2)) & 1u);
    }
    void set_stereo(MixerSrcKind kind, uint32_t ch, bool on) {
        if (ch >= kCh) return;
        const uint32_t bit = 1u << (ch / 2);
        uint32_t& b = (kind == MixerSrcKind::Playback) ? stereoPb_ : stereoIn_;
        b = on ? (b | bit) : (b & ~bit);
    }
    uint32_t stereo_bits(MixerSrcKind kind) const { return bits(kind); }
    void set_stereo_bits(MixerSrcKind kind, uint32_t b) {
        (kind == MixerSrcKind::Playback ? stereoPb_ : stereoIn_) = b;
    }

    // The partner channel in a stereo pair, or `ch` itself when the pair is mono. Edits that must
    // apply to both sides of a stereo channel go through this.
    static uint32_t partner(uint32_t ch) { return ch ^ 1u; }

    // ---- rendering to the device -------------------------------------------------------------
    //
    // One place turns logical state into a quadlet, so mute and invert can never be applied
    // inconsistently between "set one cell" and "re-apply the whole matrix".
    static uint32_t quadlet(const Cell& c) {
        const uint32_t g = (c.flags & kMfMuted) ? kMixerMute : c.gain;
        return coeff_quadlet(signed_coeff(g, (c.flags & kMfInverted) != 0));
    }

    // Address + value for a single crosspoint, so a runtime change costs one write rather than 2048.
    struct Write { Addr addr; uint32_t value; };
    Write render_cell(MixerSrcKind kind, uint32_t src, uint32_t dest) const {
        const Addr a = kind == MixerSrcKind::Playback ? playback_coeff_addr(src, dest)
                                                      : input_coeff_addr(src, dest);
        return {a, quadlet(cell(kind, src, dest))};
    }
    Write render_output(uint32_t out) const {
        return {output_fader_addr(out), quadlet(output(out))};
    }

    // The whole matrix into a device image. The shadow stays the thing that knows the RAM layout
    // (including the fader row's overlap with destination 31's playback block), so rendering writes
    // THROUGH it rather than reimplementing addressing.
    void render(MixerShadow& sh) const {
        sh.clear();
        for (uint32_t d = 0; d < kCh; ++d) {
            for (uint32_t s = 0; s < kCh; ++s) {
                sh.set_input(s, d, quadlet(inputs_[s][d]));
                sh.set_playback(s, d, quadlet(playback_[s][d]));
            }
        }
        // Faders LAST: output_fader_addr(0) is 0x1f80, which lies inside destination 31's playback
        // block. Destination 31 does not exist on a 28-channel device, so nothing is lost — but the
        // order matters, because the playback loop above would otherwise overwrite the fader row.
        for (uint32_t o = 0; o < kCh; ++o) sh.set_fader(o, quadlet(outputs_[o]));
    }

    // The 28-quadlet output-record mask for 0x801c0080 — TotalMix's Loopback, per output.
    //
    // A set output sends that output's MIX to the recording software in place of the corresponding
    // hardware input (manual §27.5). FFADO only ever writes it all-on or all-off
    // (set_hardware_output_rec); per-output is what TotalMix actually exposes, and the register is
    // already per-channel, so there is nothing to invent.
    std::array<uint32_t, kCh> rec_mask() const {
        std::array<uint32_t, kCh> m{};
        for (uint32_t i = 0; i < kCh; ++i) m[i] = (outputs_[i].flags & kMfRec) ? 1u : 0u;
        return m;
    }

    // ---- edits that write more than one crosspoint -------------------------------------------

    // Pan a source across a stereo output pair, with the manual's -3 dB centre law (§25.3). `pan` is
    // -1 hard left … 0 centre … +1 hard right; `destL` is the left output of the pair.
    //
    // Stored as the two crosspoints it produces, not as a pan value — see the header comment. A UI
    // that wants to show a pan knob recovers it from the pair, which also means a pan set here and a
    // crosspoint set by hand in the Matrix cannot disagree.
    void set_pan(MixerSrcKind kind, uint32_t src, uint32_t destL, double pan, uint32_t gain) {
        if (pan < -1.0) pan = -1.0;
        if (pan > 1.0) pan = 1.0;
        // Constant-power: cos/sin over a quarter turn, which is -3.01 dB at centre.
        const double t = (pan + 1.0) * 0.25 * 3.14159265358979323846;
        cell(kind, src, destL).gain            = scale(gain, std::cos(t));
        cell(kind, src, partner(destL)).gain   = scale(gain, std::sin(t));
    }

    // Recover the pan of a source across an output pair, for display. Returns 0 when the pair is
    // silent (nothing to show a knob at).
    double pan_of(MixerSrcKind kind, uint32_t src, uint32_t destL) const {
        const double l = cell(kind, src, destL).gain;
        const double r = cell(kind, src, partner(destL)).gain;
        if (l <= 0.0 && r <= 0.0) return 0.0;
        return (std::atan2(r, l) / (0.25 * 3.14159265358979323846)) - 1.0;
    }

    // Copy every source's routing to one output onto another — the manual's Copy Submix (§27.2).
    // A submix IS a destination column of the matrix, so this is a column copy.
    void copy_submix(uint32_t from, uint32_t to) {
        if (from >= kCh || to >= kCh || from == to) return;
        for (uint32_t s = 0; s < kCh; ++s) {
            inputs_[s][to]   = inputs_[s][from];
            playback_[s][to] = playback_[s][from];
        }
    }

    // Clear Submix (§27.3): drop everything feeding one output, leaving its fader alone.
    void clear_submix(uint32_t out) {
        if (out >= kCh) return;
        for (uint32_t s = 0; s < kCh; ++s) {
            inputs_[s][out]   = Cell{};
            playback_[s][out] = Cell{};
        }
    }

    // ---- persistence --------------------------------------------------------------------------
    //
    // A flat POD so save/restore is a single fwrite, with the same "a version we do not know is
    // IGNORED" rule as the rest of uf_state_store.hpp. Saving the MODEL rather than the rendered
    // matrix is the point: a mute stored as a rendered 0 loses the gain underneath it, so a restart
    // would silently un-do exactly what mute is supposed to be reversible about.
    struct Blob {
        uint32_t stereoIn;
        uint32_t stereoPb;
        uint32_t inputGain[kCh][kCh];
        uint32_t playbackGain[kCh][kCh];
        uint32_t outputGain[kCh];
        uint8_t  inputFlags[kCh][kCh];
        uint8_t  playbackFlags[kCh][kCh];
        uint8_t  outputFlags[kCh];
    };

    Blob to_blob() const {
        Blob b{};
        b.stereoIn = stereoIn_;
        b.stereoPb = stereoPb_;
        for (uint32_t s = 0; s < kCh; ++s)
            for (uint32_t d = 0; d < kCh; ++d) {
                b.inputGain[s][d]     = inputs_[s][d].gain;
                b.inputFlags[s][d]    = inputs_[s][d].flags;
                b.playbackGain[s][d]  = playback_[s][d].gain;
                b.playbackFlags[s][d] = playback_[s][d].flags;
            }
        for (uint32_t o = 0; o < kCh; ++o) {
            b.outputGain[o]  = outputs_[o].gain;
            b.outputFlags[o] = outputs_[o].flags;
        }
        return b;
    }

    void from_blob(const Blob& b) {
        stereoIn_ = b.stereoIn;
        stereoPb_ = b.stereoPb;
        for (uint32_t s = 0; s < kCh; ++s)
            for (uint32_t d = 0; d < kCh; ++d) {
                inputs_[s][d]   = {clamp_gain(b.inputGain[s][d]), b.inputFlags[s][d]};
                playback_[s][d] = {clamp_gain(b.playbackGain[s][d]), b.playbackFlags[s][d]};
            }
        for (uint32_t o = 0; o < kCh; ++o)
            outputs_[o] = {clamp_gain(b.outputGain[o]), b.outputFlags[o]};
    }

    // Adopt a raw 2048-quadlet matrix — the format the daemon saved before this model existed, so an
    // upgrade keeps the user's routing instead of silently resetting it.
    //
    // The rendered form is losslessly invertible for everything it can express: the sign is the
    // phase and the magnitude is the gain. What it cannot carry is the difference between "muted"
    // and "turned down to zero", so a zero cell comes back as gain 0 and unmuted. That is the
    // conservative reading — it sounds identical, and un-muting a cell that was never muted is not a
    // surprise anyone can hear.
    void import_cells(const uint32_t* cells, uint32_t count) {
        auto get = [&](Addr a) -> int32_t {
            const uint32_t i = (uint32_t)((a - reg::kMixerRam) / 4);
            return i < count ? quadlet_coeff(cells[i]) : 0;
        };
        auto adopt = [](int32_t v) -> Cell {
            return {coeff_magnitude(v), (uint8_t)(v < 0 ? kMfInverted : kMfNone)};
        };
        for (uint32_t s = 0; s < kCh; ++s)
            for (uint32_t d = 0; d < kCh; ++d) {
                inputs_[s][d]   = adopt(get(input_coeff_addr(s, d)));
                playback_[s][d] = adopt(get(playback_coeff_addr(s, d)));
            }
        for (uint32_t o = 0; o < kCh; ++o) outputs_[o] = adopt(get(output_fader_addr(o)));
    }

private:
    static uint32_t clamp_gain(uint32_t g) { return g > kMixerMax ? kMixerMax : g; }

    static uint32_t scale(uint32_t gain, double f) {
        double v = static_cast<double>(gain) * f;
        if (v < 0.0) v = 0.0;
        if (v > static_cast<double>(kMixerMax)) v = static_cast<double>(kMixerMax);
        return static_cast<uint32_t>(v + 0.5);
    }

    // [src][dest], matching how the matrix is addressed and how a UI reads it (a row per source).
    std::array<std::array<Cell, kCh>, kCh> inputs_{};
    std::array<std::array<Cell, kCh>, kCh> playback_{};
    std::array<Cell, kCh> outputs_{};
    uint32_t bits(MixerSrcKind kind) const {
        return kind == MixerSrcKind::Playback ? stereoPb_ : stereoIn_;
    }

    uint32_t stereoIn_ = 0;    // one bit per PAIR: bit n covers channels 2n and 2n+1
    uint32_t stereoPb_ = 0;
    Cell scratch_{};           // where out-of-range writes go to die
};

}  // namespace uf
