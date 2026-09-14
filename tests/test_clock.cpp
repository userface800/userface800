// Clock-config register assembly (spec/02 §2.3).
#include "uf/protocol/clock.hpp"
#include "uf_test.hpp"

using namespace uf;

UF_TEST(clock_config_source_field) {
    UF_CHECK_EQ(clock_config_source_bits(SyncRef::Adat1), 0x0000u);
    UF_CHECK_EQ(clock_config_source_bits(SyncRef::Adat2), 0x0400u);
    UF_CHECK_EQ(clock_config_source_bits(SyncRef::Spdif), 0x0c00u);
    UF_CHECK_EQ(clock_config_source_bits(SyncRef::WordClock), 0x1000u);
    // Clock-config puts TCO at 0x1800 — distinct from CR2 SYNC_REF's 0x1400.
    UF_CHECK_EQ(clock_config_source_bits(SyncRef::Tco), 0x1800u);
    UF_CHECK(clock_config_source_bits(SyncRef::Tco) != sync_ref_bits(SyncRef::Tco));
}

UF_TEST(clock_config_master_and_rate) {
    // Master @48k, ADAT1: mode bit 0x1 + rate code 0x06, no source bits.
    u32 v = assemble_clock_config(true, SyncRef::Adat1, 48000);
    UF_CHECK(v & clockcfg::kClockModeMaster);
    UF_CHECK_EQ(v & clockcfg::kRateMask, 0x06u);
    UF_CHECK_EQ(v & clockcfg::kSourceMask, 0x0000u);
    // Slave to SPDIF @44.1k: no master bit, source 0xc00, rate code 0x00.
    v = assemble_clock_config(false, SyncRef::Spdif, 44100);
    UF_CHECK(!(v & clockcfg::kClockModeMaster));
    UF_CHECK_EQ(v & clockcfg::kSourceMask, 0x0c00u);
    UF_CHECK_EQ(v & clockcfg::kRateMask, 0x00u);
    // Unsupported rate leaves the rate field 0.
    v = assemble_clock_config(true, SyncRef::WordClock, 50000);
    UF_CHECK_EQ(v & clockcfg::kRateMask, 0u);
    UF_CHECK_EQ(v & clockcfg::kSourceMask, 0x1000u);
}

UF_TEST(cr2_mirror_consistency) {
    // The CR2 surface (settings.hpp) mirrors the same logical selection: master + sync ref.
    SettingsShadow s;
    s.clock_master = false;
    s.sync_ref = SyncRef::Spdif;
    u32 cr2 = assemble_cr2(s);
    UF_CHECK(!(cr2 & cr2::kClockMaster));
    UF_CHECK_EQ(cr2 & 0x1c00u, sync_ref_bits(SyncRef::Spdif));
}

UF_TEST_MAIN()
