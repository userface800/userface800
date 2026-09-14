// Status decode (spec/06 §SR0/§SR1, spec/09 §9.3).
#include "uf/protocol/status.hpp"
#include "uf_test.hpp"

using namespace uf;

UF_TEST(freq_index) {
    UF_CHECK_EQ(freq_index_to_hz(1), 32000u);
    UF_CHECK_EQ(freq_index_to_hz(2), 44100u);
    UF_CHECK_EQ(freq_index_to_hz(3), 48000u);
    UF_CHECK_EQ(freq_index_to_hz(6), 96000u);
    UF_CHECK_EQ(freq_index_to_hz(9), 192000u);
    UF_CHECK_EQ(freq_index_to_hz(0), 0u);
    UF_CHECK_EQ(freq_index_to_hz(10), 0u);
}

UF_TEST(ext_rate_decode) {
    // spec/09 §9.3: locked_ext_rate = (sr0 & 0x3ff) * 250. 192 * 250 = 48000.
    auto s = decode_status(0x000000C0, 0);
    UF_CHECK_EQ(s.ext_rate_hz, 48000u);
    // 176 * 250 = 44000 (closest 10-bit code to 44.1k); just check the arithmetic.
    UF_CHECK_EQ(decode_status(176, 0).ext_rate_hz, 44000u);
}

UF_TEST(locks_and_source) {
    // SPDIF locked+synced @96k, word-clock locked, over, selected source = SPDIF.
    u32 sr0 = sr0::kSpdifLock | sr0::kSpdifSync | sr0::kWordClockLock | sr0::kOver |
              sr0::kSyncSrcSpdif | (6u << sr0::kSpdifFreqShift) | (3u << sr0::kInputFreqShift);
    auto s = decode_status(sr0, 0);
    UF_CHECK(s.spdif_lock);
    UF_CHECK(s.spdif_sync);
    UF_CHECK(s.wclk_lock);
    UF_CHECK(!s.wclk_sync);
    UF_CHECK(s.over);
    UF_CHECK(s.sync_source == SyncSource::Spdif);
    UF_CHECK_EQ(s.spdif_freq_hz, 96000u);
    UF_CHECK_EQ(s.input_freq_hz, 48000u);
}

UF_TEST(sync_source_variants) {
    UF_CHECK(decode_status(sr0::kSyncSrcAdat1, 0).sync_source == SyncSource::Adat1);
    UF_CHECK(decode_status(sr0::kSyncSrcAdat2, 0).sync_source == SyncSource::Adat2);
    UF_CHECK(decode_status(sr0::kSyncSrcWordClock, 0).sync_source == SyncSource::WordClock);
    UF_CHECK(decode_status(sr0::kSyncSrcTco, 0).sync_source == SyncSource::Tco);
    UF_CHECK(decode_status(sr0::kSyncSrcNone, 0).sync_source == SyncSource::None);
}

UF_TEST(sr1_decode) {
    // spec/09 §9.3: master = sr1 & 0x1.
    auto s = decode_status(0, sr1::kClockMaster | sr1::kTcoLock);
    UF_CHECK(s.clock_master);
    UF_CHECK(s.tco_lock);
    UF_CHECK(!s.tco_sync);
    UF_CHECK(!decode_status(0, 0).clock_master);
}

UF_TEST_MAIN()
