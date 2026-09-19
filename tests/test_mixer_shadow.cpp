// The mixer shadow — the daemon's copy of what the FF800's matrix should contain.
//
// The bug these pin down: session_start zeroed the whole matrix and wrote a hardcoded default, so
// every user route died at the next rate change. The shadow's whole job is that a route set once is
// still there after apply_all() runs again, which is what survives_reapply below asserts.
#include "../platform/shared/uf_mixer_shadow.hpp"
#include "uf_test.hpp"

#include <map>

using uf::MixerShadow;

namespace {
// Capture what would be written to the device.
struct Recorder {
    std::map<uf::Addr, uint32_t> writes;
    uint32_t count = 0;
    void operator()(uf::Addr a, uint32_t v) { writes[a] = v; ++count; }
};
}  // namespace

UF_TEST(defaults_match_what_the_daemon_always_wrote) {
    MixerShadow m;
    m.reset_defaults(28);
    // playback i -> output i at unity, faders at unity, everything else muted.
    for (uint32_t i = 0; i < 28; ++i) {
        UF_CHECK_EQ(m.at(uf::playback_coeff_addr(i, i)), uf::kMixerUnity);
        UF_CHECK_EQ(m.at(uf::output_fader_addr(i)), uf::kMixerUnity);
    }
    UF_CHECK_EQ(m.at(uf::input_coeff_addr(3, 0)), 0u);
    UF_CHECK_EQ(m.at(uf::playback_coeff_addr(0, 5)), 0u);
}

UF_TEST(rate_dependent_channel_count_is_respected) {
    MixerShadow m;
    m.reset_defaults(12);            // 192 kHz: 12 channels on the wire
    UF_CHECK_EQ(m.at(uf::playback_coeff_addr(11, 11)), uf::kMixerUnity);
    UF_CHECK_EQ(m.at(uf::playback_coeff_addr(12, 12)), 0u);   // beyond dbq: untouched
}

// THE POINT OF THE WHOLE TASK. A route set by the user must still be there after the matrix is
// re-applied, which is what happens on every rate change, wedge recovery and bus reset.
UF_TEST(a_user_route_survives_reapply) {
    MixerShadow m;
    m.reset_defaults(28);
    const uf::Addr a1 = m.set_input(8, 0, uf::kMixerUnity);   // input 9 -> output 1 (0-based)
    const uf::Addr a2 = m.set_input(8, 1, uf::kMixerUnity);   // input 9 -> output 2

    Recorder r;
    m.apply_all(r);

    UF_CHECK_EQ(r.writes[a1], uf::kMixerUnity);
    UF_CHECK_EQ(r.writes[a2], uf::kMixerUnity);
    // and the defaults are still in the same push
    UF_CHECK_EQ(r.writes[uf::playback_coeff_addr(0, 0)], uf::kMixerUnity);
}

UF_TEST(apply_all_covers_the_entire_matrix_ram) {
    MixerShadow m;
    m.reset_defaults(28);
    Recorder r;
    m.apply_all(r);
    // Every cell, unconditionally: after a session start the device holds power-on garbage, so a
    // cell we believe is zero still has to be written as zero.
    UF_CHECK_EQ(r.count, MixerShadow::kCells);
    UF_CHECK_EQ(r.writes.size(), (size_t)MixerShadow::kCells);
    UF_CHECK_EQ(r.writes.begin()->first, uf::reg::kMixerRam);
    UF_CHECK_EQ(r.writes.rbegin()->first, uf::reg::kMixerRam + MixerShadow::kBytes - 4);
}

UF_TEST(setters_return_the_address_so_one_cell_can_be_pushed) {
    MixerShadow m;
    UF_CHECK_EQ(m.set_input(8, 0, uf::kMixerUnity), uf::input_coeff_addr(8, 0));
    UF_CHECK_EQ(m.set_playback(2, 3, 0x4000u), uf::playback_coeff_addr(2, 3));
    UF_CHECK_EQ(m.set_fader(1, 0x1234u), uf::output_fader_addr(1));
    UF_CHECK_EQ(m.at(uf::playback_coeff_addr(2, 3)), 0x4000u);
}

// The fader row at 0x1f80 sits INSIDE destination 31's playback block (31*0x100 + 0x80). Modelling
// the RAM flat rather than as typed src/dest arrays is what makes that representable at all; this
// records the overlap so nobody "fixes" it later.
UF_TEST(the_fader_row_overlaps_destination_31s_playback_block) {
    UF_CHECK_EQ(uf::output_fader_addr(0), uf::playback_coeff_addr(0, 31));
    MixerShadow m;
    m.set_fader(0, 0xAAAA);
    UF_CHECK_EQ(m.at(uf::playback_coeff_addr(0, 31)), 0xAAAAu);
}

UF_TEST(reset_defaults_clears_previous_state) {
    MixerShadow m;
    m.set_input(8, 0, uf::kMixerUnity);
    m.reset_defaults(28);
    UF_CHECK_EQ(m.at(uf::input_coeff_addr(8, 0)), 0u);
}

UF_TEST(nodiag_leaves_playback_muted_but_faders_up) {
    MixerShadow m;
    m.reset_defaults(28, /*diag=*/false);
    UF_CHECK_EQ(m.at(uf::playback_coeff_addr(0, 0)), 0u);
    UF_CHECK_EQ(m.at(uf::output_fader_addr(0)), uf::kMixerUnity);
}

UF_TEST_MAIN()
