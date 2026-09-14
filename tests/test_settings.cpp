// Extended-settings CR0/CR1/CR2 assembly (spec/06, spec/09 §9.2).
#include "uf/protocol/settings.hpp"
#include "uf_test.hpp"

using namespace uf;

// The spec/09 §9.2 scenario: 48 kHz internal (master) clock, SPDIF coax, +4 dBu levels, no
// phantom, no input options. Rate is separate (STF); this is just the CR block.
UF_TEST(worked_example_9_2) {
    SettingsShadow s;  // defaults: master, coax, sync ADAT1, drive off, inputs unset
    s.input_level = InputLevel::P4dBu;
    s.output_level = OutputLevel::P4dBu;
    s.phones_level = PhonesLevel::P4dBu;
    s.clock_master = true;
    s.spdif_in_optical = false;  // coax
    s.sync_ref = SyncRef::Adat1;

    ConfBlock cb = assemble_conf_block(s);
    UF_CHECK_EQ(cb.cr0, 0x00000810u);
    UF_CHECK_EQ(cb.cr1, 0x0000001Au);
    UF_CHECK_EQ(cb.cr2, 0x80000001u);
}

UF_TEST(cr2_defaults_and_clock) {
    SettingsShadow s;
    // Autosync (slave) clears the master bit; drop-and-stop stays set.
    s.clock_master = false;
    UF_CHECK_EQ(assemble_cr2(s), 0x80000000u | cr2::kSyncRefAdat1);
    // Sync reference encodings (spec/06).
    s.clock_master = true;
    s.sync_ref = SyncRef::Spdif;
    UF_CHECK_EQ(assemble_cr2(s) & 0x00001c00u, 0x00000c00u);
    s.sync_ref = SyncRef::WordClock;
    UF_CHECK_EQ(assemble_cr2(s) & 0x00001c00u, 0x00001000u);
    s.sync_ref = SyncRef::Tco;
    UF_CHECK_EQ(assemble_cr2(s) & 0x00001c00u, 0x00001400u);
    // FREQ/DSPEED/QSSPEED intentionally 0 (decision: match snd-fireface/spec, not FFADO's 0x1E).
    s.sync_ref = SyncRef::Adat1;
    UF_CHECK_EQ(assemble_cr2(s) & 0x0000001eu, 0u);
}

UF_TEST(cr2_spdif_and_options) {
    SettingsShadow s;
    s.spdif_out_pro = true;
    s.spdif_out_emphasis = true;
    s.spdif_in_optical = true;
    s.word_clock_1x = true;
    s.disable_limiter = true;
    u32 cr2v = assemble_cr2(s);
    UF_CHECK(cr2v & cr2::kSpdifOutPro);
    UF_CHECK(cr2v & cr2::kSpdifOutEmphasis);
    UF_CHECK(cr2v & cr2::kSpdifInOptical);
    UF_CHECK(cr2v & cr2::kWordClock1x);
    UF_CHECK(cr2v & cr2::kDisableLimiter);
}

UF_TEST(phantom_and_levels) {
    SettingsShadow s;
    s.phantom7 = true;
    s.phantom10 = true;
    s.input_level = InputLevel::M10dBV;
    s.output_level = OutputLevel::HiGain;
    UF_CHECK(assemble_cr0(s) & cr0::kPhantomMic7);
    UF_CHECK(assemble_cr0(s) & cr0::kPhantomMic10);
    // -10dBV in: FPGA 0x20 + CPLD 0x03; HiGain out: FPGA 0x400 + CPLD 0x10.
    UF_CHECK(assemble_cr0(s) & cr0::kInLevelM10dBV);
    UF_CHECK_EQ(assemble_cr1(s) & 0x03u, cr1::kInLevelM10dBV);
    UF_CHECK(assemble_cr0(s) & cr0::kOutLevelHiGain);
    UF_CHECK(assemble_cr1(s) & cr1::kOutLevelHiGain);
}

UF_TEST(input1_front_filter_interaction) {
    SettingsShadow s;
    // Input 1 front, no filter -> plain front bit 0x800.
    s.input1 = InputSource::Front;
    s.filter = false;
    UF_CHECK(assemble_cr1(s) & cr1::kInput1Front);
    UF_CHECK(!(assemble_cr1(s) & cr1::kInput1FrontFilter));
    // Input 1 front, with filter -> filter-front bit 0x400 + CR0 filter LED.
    s.filter = true;
    UF_CHECK(assemble_cr1(s) & cr1::kInput1FrontFilter);
    UF_CHECK(!(assemble_cr1(s) & cr1::kInput1Front));
    UF_CHECK(assemble_cr0(s) & cr0::kFilterCh1);
    // Input 1 rear.
    s.input1 = InputSource::Rear;
    UF_CHECK(assemble_cr1(s) & cr1::kInput1Rear);
}

UF_TEST(drive_sets_both_paths) {
    SettingsShadow s;
    UF_CHECK(!(assemble_cr0(s) & cr0::kDriveCh1));       // default off
    UF_CHECK(!(assemble_cr1(s) & cr1::kInstrumentDrive));
    s.drive = true;
    UF_CHECK(assemble_cr0(s) & cr0::kDriveCh1);          // FPGA LED
    UF_CHECK(assemble_cr1(s) & cr1::kInstrumentDrive);   // CPLD
}

UF_TEST_MAIN()
