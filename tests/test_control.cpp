// UFControl shadow + register-write planning (spec/02 §2.2, spec/09 §9.2).
#include "uf/protocol/control.hpp"
#include "uf_test.hpp"

using namespace uf;

// A UFControl configured for the spec/09 §9.2 scenario should plan exactly the §9.2 writes.
UF_TEST(full_rewrite_matches_9_2) {
    UFControl c;
    c.set_sample_rate(48000);
    SettingsShadow s;
    s.input_level = InputLevel::P4dBu;
    s.output_level = OutputLevel::P4dBu;
    s.clock_master = true;
    s.sync_ref = SyncRef::Adat1;
    s.spdif_in_optical = false;
    c.set_settings(s);

    auto writes = c.full_rewrite();
    UF_CHECK_EQ(writes.size(), static_cast<size_t>(2));

    // 1) STF raw-Hz rate.
    UF_CHECK(writes[0].kind == RegWrite::Kind::Quadlet);
    UF_CHECK_EQ(writes[0].addr, reg::kStf);
    UF_CHECK_EQ(writes[0].quad, 0xBB80u);  // 48000

    // 2) CR0/CR1/CR2 block to the conf register.
    UF_CHECK(writes[1].kind == RegWrite::Kind::Block);
    UF_CHECK_EQ(writes[1].addr, reg::kConfBlock);
    UF_CHECK_EQ(writes[1].quads.size(), static_cast<size_t>(3));
    UF_CHECK_EQ(writes[1].quads[0], 0x00000810u);
    UF_CHECK_EQ(writes[1].quads[1], 0x0000001Au);
    UF_CHECK_EQ(writes[1].quads[2], 0x80000001u);
}

UF_TEST(shadow_defaults_and_mutation) {
    UFControl c;
    UF_CHECK_EQ(c.sample_rate(), 44100u);  // default
    c.set_sample_rate(96000);
    UF_CHECK_EQ(c.sample_rate(), 96000u);
    // Mutate the shadow in place.
    c.settings().phantom7 = true;
    auto cb = c.conf_block_write();
    UF_CHECK(cb.quads[0] & cr0::kPhantomMic7);
}

UF_TEST(clock_config_surface) {
    UFControl c;
    c.set_sample_rate(48000);
    c.settings().clock_master = true;
    c.settings().sync_ref = SyncRef::Adat1;
    auto w = c.clock_config_write();
    UF_CHECK_EQ(w.addr, reg::kClockConfig);
    UF_CHECK(w.quad & clockcfg::kClockModeMaster);
    UF_CHECK_EQ(w.quad & clockcfg::kRateMask, 0x06u);  // 48k code
}

UF_TEST_MAIN()
