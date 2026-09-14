// Register address + bitfield constant sanity (facts vs spec/02, spec/06, spec/09).
#include "uf/protocol/registers.hpp"
#include "uf_test.hpp"

using namespace uf;

UF_TEST(addresses_match_spec) {
    UF_CHECK_EQ(reg::kStf, 0xfc88f000u);
    UF_CHECK_EQ(reg::kConfBlock, 0xfc88f014u);
    UF_CHECK_EQ(reg::kStatus0, 0x801c0000u);
    UF_CHECK_EQ(reg::kClockConfig, 0x801c0004u);
    UF_CHECK_EQ(reg::kTxIsoChannel, 0x801c0008u);
    UF_CHECK_EQ(reg::kMixerRam, 0x80080000u);
    UF_CHECK_EQ(reg::kMidiOut, 0x80180000u);
    UF_CHECK_EQ(reg::kFirmwareRev, 0x200000100ull);
    UF_CHECK_EQ(reg::kMidiHighAddr, 0x200000320ull);
}

UF_TEST(addr_hi_lo_split) {
    // High-part 0x0000 register.
    UF_CHECK_EQ(addr_hi(reg::kStatus0), 0x0000u);
    UF_CHECK_EQ(addr_lo(reg::kStatus0), 0x801c0000u);
    // High-part 0x0002 register (spec/01 §1.2): 0x200000320 = (0x0002 << 32) | 0x00000320.
    UF_CHECK_EQ(addr_hi(reg::kMidiHighAddr), 0x0002u);
    UF_CHECK_EQ(addr_lo(reg::kMidiHighAddr), 0x00000320u);
    UF_CHECK_EQ(addr_hi(reg::kFirmwareRev), 0x0002u);
    UF_CHECK_EQ(addr_lo(reg::kFirmwareRev), 0x00000100u);
}

UF_TEST(cr_and_sr_masks) {
    // CR2 defaults + clock master (spec/09 §9.2: CR2 master|drop-and-stop = 0x80000001).
    UF_CHECK_EQ(cr2::kClockMaster | cr2::kDropAndStop, 0x80000001u);
    UF_CHECK_EQ(cr2::kSyncRefSpdif, 0x00000c00u);
    // CR0 +4dBu in/out (spec/09 §9.2: 0x00000810).
    UF_CHECK_EQ(cr0::kInLevelP4dBu | cr0::kOutLevelP4dBu, 0x00000810u);
    // CR1 +4dBu in/out (spec/09 §9.2: 0x0000001a).
    UF_CHECK_EQ(cr1::kInLevelP4dBu | cr1::kOutLevelP4dBu, 0x0000001au);
    // SR0 external-rate decode weight.
    UF_CHECK_EQ(sr0::kExtRateUnit, 250u);
    UF_CHECK_EQ(sr0::kSpdifLock, 0x00100000u);
}

UF_TEST_MAIN()
