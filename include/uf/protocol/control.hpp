// control.hpp — UFControl: the host-side configuration shadow + register-write planner (portable).
//
// UFControl holds the complete device configuration (sample rate + the CR settings shadow) and, on
// change, produces the register writes needed to push the affected surface (spec/02 §2.2).
// It is pure logic: it emits RegWrite records; the macOS layer executes them via UFAsyncIO.
// Most registers are write-only, so the shadow is the source of truth; status is read separately
// (status.hpp).
#pragma once
#include <vector>
#include "uf/protocol/clock.hpp"
#include "uf/protocol/endian.hpp"
#include "uf/protocol/rate.hpp"
#include "uf/protocol/registers.hpp"
#include "uf/protocol/settings.hpp"

namespace uf {

// A pending register write the host should perform (host-order values; UFAsyncIO swaps to LE).
struct RegWrite {
    enum class Kind { Quadlet, Block };
    Addr addr;
    Kind kind;
    u32 quad = 0;              // used when kind == Quadlet
    std::vector<u32> quads;    // used when kind == Block
};

inline RegWrite quadlet_write(Addr a, u32 v) { return {a, RegWrite::Kind::Quadlet, v, {}}; }
inline RegWrite block_write(Addr a, std::vector<u32> qs) {
    return {a, RegWrite::Kind::Block, 0, std::move(qs)};
}

class UFControl {
public:
    // ── Shadow accessors ─────────────────────────────────────────────────────────────────────
    u32 sample_rate() const { return rate_; }
    const SettingsShadow& settings() const { return settings_; }
    SettingsShadow& settings() { return settings_; }

    void set_sample_rate(u32 hz) { rate_ = hz; }
    void set_settings(const SettingsShadow& s) { settings_ = s; }

    // ── Surface builders (one RegWrite per surface) ──────────────────────────────────────────
    // STF (0xfc88f000): raw-Hz sample rate.
    RegWrite rate_write() const { return quadlet_write(reg::kStf, stf_value(rate_)); }

    // CONF block (0xfc88f014): CR0/CR1/CR2 assembled from the settings shadow.
    RegWrite conf_block_write() const {
        ConfBlock cb = assemble_conf_block(settings_);
        return block_write(reg::kConfBlock, {cb.cr0, cb.cr1, cb.cr2});
    }

    // Clock-config (0x801c0004): the alternate streaming/clock surface (spec/02 §2.3). Overlaps the
    // CR2 clock bits; kept available behind the same shadow (the overlap is a hardware [?]).
    RegWrite clock_config_write() const {
        return quadlet_write(reg::kClockConfig,
                             assemble_clock_config(settings_.clock_master, settings_.sync_ref, rate_));
    }

    // A full re-write of the current config: rate then the CR settings block. Idempotent — issuing
    // it against an already-configured device is a no-op change.
    std::vector<RegWrite> full_rewrite() const {
        return {rate_write(), conf_block_write()};
    }

private:
    u32 rate_ = 44100;
    SettingsShadow settings_{};
};

}  // namespace uf
