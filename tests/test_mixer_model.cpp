// The logical mixer above the device matrix (uf_mixer_model.hpp).
//
// The whole reason this layer exists is that rendering is LOSSY: a muted crosspoint becomes 0 and a
// phase-inverted one becomes negative, so gain, mute and phase cannot all live in the rendered
// quadlet. Most of what follows is that one property, checked from several directions.
#include "../platform/shared/uf_mixer_model.hpp"
#include "uf_test.hpp"

using namespace uf;

UF_TEST(mute_preserves_the_gain_underneath_it) {
    MixerModel m;
    m.cell(MixerSrcKind::Input, 8, 0).gain = kMixerUnity;
    UF_CHECK_EQ(m.render_cell(MixerSrcKind::Input, 8, 0).value, kMixerUnity);

    m.cell(MixerSrcKind::Input, 8, 0).flags = kMfMuted;
    UF_CHECK_EQ(m.render_cell(MixerSrcKind::Input, 8, 0).value, kMixerMute);   // device sees silence
    UF_CHECK_EQ(m.cell(MixerSrcKind::Input, 8, 0).gain, kMixerUnity);          // ...we still know it

    m.cell(MixerSrcKind::Input, 8, 0).flags = kMfNone;
    UF_CHECK_EQ(m.render_cell(MixerSrcKind::Input, 8, 0).value, kMixerUnity);  // and can restore it
}

UF_TEST(phase_invert_is_a_negative_coefficient) {
    MixerModel m;
    m.cell(MixerSrcKind::Playback, 3, 5).gain  = kMixerUnity;
    m.cell(MixerSrcKind::Playback, 3, 5).flags = kMfInverted;
    const uint32_t q = m.render_cell(MixerSrcKind::Playback, 3, 5).value;
    UF_CHECK_EQ(quadlet_coeff(q), -(int32_t)kMixerUnity);
    UF_CHECK_EQ(coeff_magnitude(quadlet_coeff(q)), kMixerUnity);
}

// FFADO's workaround, and it is a hardware behaviour rather than a tidiness rule: on the FF800 a
// 0 -> -1 transition makes the device run the volume far up before settling ~100 ms later. A muted
// AND inverted crosspoint therefore renders as -1 (-90 dB, inaudible) rather than 0.
UF_TEST(a_muted_inverted_cell_renders_as_minus_one_not_zero) {
    MixerModel m;
    m.cell(MixerSrcKind::Input, 0, 0).gain  = kMixerUnity;
    m.cell(MixerSrcKind::Input, 0, 0).flags = kMfMuted | kMfInverted;
    UF_CHECK_EQ(quadlet_coeff(m.render_cell(MixerSrcKind::Input, 0, 0).value), -1);

    UF_CHECK_EQ(signed_coeff(0, false), 0);
    UF_CHECK_EQ(signed_coeff(0, true), -1);
    UF_CHECK_EQ(signed_coeff(kMixerUnity, false), (int32_t)kMixerUnity);
}

UF_TEST(rendering_addresses_match_the_spec_09_worked_example) {
    MixerModel m;
    // spec/09 §9.6: input 3 (src index 2) -> output 1 (dest index 0).
    UF_CHECK_EQ(m.render_cell(MixerSrcKind::Input, 2, 0).addr, 0x80080008ull);
    UF_CHECK_EQ(m.render_cell(MixerSrcKind::Playback, 0, 0).addr, 0x80080080ull);
    UF_CHECK_EQ(m.render_output(0).addr, 0x80081f80ull);
}

UF_TEST(defaults_match_what_the_daemon_has_always_come_up_with) {
    MixerModel m;
    m.reset_defaults(28);
    MixerShadow sh;
    m.render(sh);
    // Each playback channel to its own output at unity; faders up; inputs silent.
    UF_CHECK_EQ(sh.at(playback_coeff_addr(0, 0)), kMixerUnity);
    UF_CHECK_EQ(sh.at(playback_coeff_addr(1, 1)), kMixerUnity);
    UF_CHECK_EQ(sh.at(playback_coeff_addr(0, 1)), kMixerMute);
    UF_CHECK_EQ(sh.at(output_fader_addr(5)), kMixerUnity);
    UF_CHECK_EQ(sh.at(input_coeff_addr(8, 0)), kMixerMute);

    // UF_NODIAG: faders only, no playback diagonal.
    MixerModel n;
    n.reset_defaults(28, /*diag=*/false);
    MixerShadow sh2;
    n.render(sh2);
    UF_CHECK_EQ(sh2.at(playback_coeff_addr(0, 0)), kMixerMute);
    UF_CHECK_EQ(sh2.at(output_fader_addr(0)), kMixerUnity);
}

// The fader row at 0x1f80 sits INSIDE destination 31's playback block. Destination 31 does not exist
// on a 28-channel device, but render() must still write the faders after the matrix or the loop
// would stamp over them.
UF_TEST(the_fader_row_survives_a_full_render) {
    MixerModel m;
    m.reset_defaults(28);
    m.output(27).gain = 0x1234;
    MixerShadow sh;
    m.render(sh);
    UF_CHECK_EQ(sh.at(output_fader_addr(27)), 0x1234u);
    UF_CHECK_EQ(output_fader_addr(27), reg::kMixerRam + 0x1f80 + 27 * 4);
}

UF_TEST(loopback_is_a_per_output_bit_in_the_record_mask) {
    MixerModel m;
    auto mask = m.rec_mask();
    UF_CHECK_EQ(mask[0], 0u);
    UF_CHECK_EQ(mask.size(), (size_t)28);

    m.output(6).flags = kMfRec;
    mask = m.rec_mask();
    UF_CHECK_EQ(mask[6], 1u);
    UF_CHECK_EQ(mask[7], 0u);
    // Loopback must not leak into the matrix — it lives in a different register entirely.
    UF_CHECK_EQ(m.render_output(6).value, m.output(6).gain);
}

UF_TEST(stereo_pairing_is_per_pair_not_per_channel) {
    MixerModel m;
    UF_CHECK(!m.stereo(MixerSrcKind::Input, 0));
    m.set_stereo(MixerSrcKind::Input, 1, true);   // setting either side pairs both
    UF_CHECK(m.stereo(MixerSrcKind::Input, 0));
    UF_CHECK(m.stereo(MixerSrcKind::Input, 1));
    UF_CHECK(!m.stereo(MixerSrcKind::Input, 2));
    UF_CHECK_EQ(MixerModel::partner(0), 1u);
    UF_CHECK_EQ(MixerModel::partner(1), 0u);
    m.set_stereo(MixerSrcKind::Input, 0, false);
    UF_CHECK(!m.stereo(MixerSrcKind::Input, 1));
}

// The manual keeps them separate (§25.3): Stereo is a per-channel setting on the input and playback
// rows, and hardware outputs are always stereo. One shared mask would mean pairing input 1/2
// silently paired playback 1/2 as well.
UF_TEST(inputs_and_playback_pair_independently) {
    MixerModel m;
    m.set_stereo(MixerSrcKind::Input, 0, true);
    UF_CHECK(m.stereo(MixerSrcKind::Input, 1));
    UF_CHECK(!m.stereo(MixerSrcKind::Playback, 1));
    m.set_stereo(MixerSrcKind::Playback, 4, true);
    UF_CHECK(m.stereo(MixerSrcKind::Playback, 5));
    UF_CHECK(!m.stereo(MixerSrcKind::Input, 5));
}

UF_TEST(pan_is_two_crosspoints_with_a_minus_3_dB_centre) {
    MixerModel m;
    m.set_pan(MixerSrcKind::Input, 8, /*destL=*/0, /*pan=*/0.0, kMixerUnity);
    const uint32_t l = m.cell(MixerSrcKind::Input, 8, 0).gain;
    const uint32_t r = m.cell(MixerSrcKind::Input, 8, 1).gain;
    UF_CHECK_EQ(l, r);
    // -3.01 dB either side: 0x8000 * cos(pi/4) = 23170.
    UF_CHECK(l > 23100u && l < 23240u);

    m.set_pan(MixerSrcKind::Input, 8, 0, -1.0, kMixerUnity);      // hard left
    UF_CHECK_EQ(m.cell(MixerSrcKind::Input, 8, 0).gain, kMixerUnity);
    UF_CHECK_EQ(m.cell(MixerSrcKind::Input, 8, 1).gain, 0u);

    m.set_pan(MixerSrcKind::Input, 8, 0, 1.0, kMixerUnity);       // hard right
    UF_CHECK_EQ(m.cell(MixerSrcKind::Input, 8, 0).gain, 0u);
    UF_CHECK_EQ(m.cell(MixerSrcKind::Input, 8, 1).gain, kMixerUnity);
}

// Pan is DERIVED, never stored — so a pan set here and a crosspoint edited by hand in a matrix view
// cannot disagree with each other.
UF_TEST(pan_round_trips_through_the_crosspoints) {
    MixerModel m;
    for (double p : {-1.0, -0.5, 0.0, 0.37, 1.0}) {
        m.set_pan(MixerSrcKind::Playback, 2, 4, p, kMixerUnity);
        const double back = m.pan_of(MixerSrcKind::Playback, 2, 4);
        UF_CHECK(back > p - 0.01 && back < p + 0.01);
    }
    // A silent pair has no pan to show rather than a misleading centre-by-accident.
    UF_CHECK(m.pan_of(MixerSrcKind::Input, 20, 10) == 0.0);
}

UF_TEST(copy_and_clear_submix_work_on_a_whole_column) {
    MixerModel m;
    m.cell(MixerSrcKind::Input, 8, 0).gain     = kMixerUnity;
    m.cell(MixerSrcKind::Input, 9, 0).flags    = kMfInverted;
    m.cell(MixerSrcKind::Playback, 1, 0).gain  = 0x4000;

    m.copy_submix(0, 6);
    UF_CHECK_EQ(m.cell(MixerSrcKind::Input, 8, 6).gain, kMixerUnity);
    UF_CHECK_EQ(m.cell(MixerSrcKind::Input, 9, 6).flags, kMfInverted);
    UF_CHECK_EQ(m.cell(MixerSrcKind::Playback, 1, 6).gain, 0x4000u);
    UF_CHECK_EQ(m.cell(MixerSrcKind::Input, 8, 0).gain, kMixerUnity);   // source untouched

    // Clearing drops the sources feeding an output but leaves its fader alone — the manual's Clear
    // Submix, which is about routing, not about turning the speakers down.
    m.output(6).gain = kMixerUnity;
    m.clear_submix(6);
    UF_CHECK_EQ(m.cell(MixerSrcKind::Input, 8, 6).gain, 0u);
    UF_CHECK_EQ(m.output(6).gain, kMixerUnity);
}

UF_TEST(out_of_range_channels_are_absorbed_not_written_past) {
    MixerModel m;
    m.cell(MixerSrcKind::Input, 999, 0).gain = kMixerUnity;      // must not corrupt anything
    m.output(999).flags = kMfRec;
    m.set_stereo(MixerSrcKind::Input, 999, true);
    UF_CHECK_EQ(m.rec_mask()[0], 0u);
    UF_CHECK_EQ(m.stereo_bits(MixerSrcKind::Input), 0u);
    for (uint32_t d = 0; d < MixerModel::kCh; ++d)
        UF_CHECK_EQ(m.cell(MixerSrcKind::Input, 0, d).gain, 0u);
}

UF_TEST(the_blob_round_trips_gains_flags_and_pairing) {
    MixerModel a;
    a.reset_defaults(28);
    a.cell(MixerSrcKind::Input, 8, 0).gain  = 0x1234;
    a.cell(MixerSrcKind::Input, 8, 0).flags = kMfMuted;
    a.cell(MixerSrcKind::Playback, 2, 3).flags = kMfInverted;
    a.output(4).flags = kMfRec;
    a.set_stereo(MixerSrcKind::Playback, 10, true);

    MixerModel b;
    b.from_blob(a.to_blob());
    UF_CHECK_EQ(b.cell(MixerSrcKind::Input, 8, 0).gain, 0x1234u);
    UF_CHECK_EQ(b.cell(MixerSrcKind::Input, 8, 0).flags, kMfMuted);
    UF_CHECK_EQ(b.cell(MixerSrcKind::Playback, 2, 3).flags, kMfInverted);
    UF_CHECK_EQ(b.rec_mask()[4], 1u);
    UF_CHECK(b.stereo(MixerSrcKind::Playback, 11));
    UF_CHECK(!b.stereo(MixerSrcKind::Input, 11));
    UF_CHECK_EQ(b.cell(MixerSrcKind::Playback, 0, 0).gain, kMixerUnity);
}

// The upgrade path: a v1 state file is the rendered matrix, and adopting it must keep the routing
// rather than resetting someone's monitoring because the format moved on.
UF_TEST(a_rendered_matrix_can_be_adopted_back_into_the_model) {
    MixerModel a;
    a.reset_defaults(28);
    a.cell(MixerSrcKind::Input, 8, 0).gain  = kMixerUnity;
    a.cell(MixerSrcKind::Input, 9, 1).gain  = 0x4000;
    a.cell(MixerSrcKind::Input, 9, 1).flags = kMfInverted;
    MixerShadow sh;
    a.render(sh);

    uint32_t cells[MixerShadow::kCells];
    for (uint32_t i = 0; i < MixerShadow::kCells; ++i) cells[i] = sh.cell(i);

    MixerModel b;
    b.import_cells(cells, MixerShadow::kCells);
    UF_CHECK_EQ(b.cell(MixerSrcKind::Input, 8, 0).gain, kMixerUnity);
    UF_CHECK_EQ(b.cell(MixerSrcKind::Input, 9, 1).gain, 0x4000u);
    UF_CHECK_EQ(b.cell(MixerSrcKind::Input, 9, 1).flags, kMfInverted);   // sign recovered
    UF_CHECK_EQ(b.output(3).gain, kMixerUnity);
    UF_CHECK_EQ(b.cell(MixerSrcKind::Playback, 7, 7).gain, kMixerUnity);
}

// What adoption CANNOT recover, stated as a test so the limit is deliberate rather than discovered:
// a muted cell rendered to 0 comes back as gain 0, unmuted. It sounds identical, which is why this
// is the safe reading.
UF_TEST(adopting_a_rendered_matrix_cannot_recover_a_mute) {
    MixerModel a;
    a.cell(MixerSrcKind::Input, 5, 5).gain  = kMixerUnity;
    a.cell(MixerSrcKind::Input, 5, 5).flags = kMfMuted;
    MixerShadow sh;
    a.render(sh);
    uint32_t cells[MixerShadow::kCells];
    for (uint32_t i = 0; i < MixerShadow::kCells; ++i) cells[i] = sh.cell(i);

    MixerModel b;
    b.import_cells(cells, MixerShadow::kCells);
    UF_CHECK_EQ(b.cell(MixerSrcKind::Input, 5, 5).gain, 0u);
    UF_CHECK_EQ(b.cell(MixerSrcKind::Input, 5, 5).flags, kMfNone);
}

UF_TEST_MAIN()
