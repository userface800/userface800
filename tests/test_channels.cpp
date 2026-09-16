// Isoch packet geometry (spec/03 §3.2/§3.3/§3.7, spec/08, spec/09 §9.4/§9.5).
#include "uf/protocol/channels.hpp"
#include "uf_test.hpp"

using namespace uf;

UF_TEST(channel_totals_send_all) {
    // spec/08 §8.2: send-all totals by speed.
    UF_CHECK_EQ(pcm_channels(Speed::X1), 28u);
    UF_CHECK_EQ(pcm_channels(Speed::X2), 20u);
    UF_CHECK_EQ(pcm_channels(Speed::X4), 12u);
    // composition at 1x: 10 analog + 2 spdif + 16 adat.
    UF_CHECK_EQ(analog_channels(BwLimit::SendAll), 10u);
    UF_CHECK_EQ(spdif_channels(BwLimit::SendAll), 2u);
    UF_CHECK_EQ(adat_channels(BwLimit::SendAll, Speed::X1), 16u);
}

UF_TEST(bandwidth_limit_modes) {
    // spec/08 §8.3 totals at 1x: send-all=28, no-adat2=20, analog+spdif=12, analog-only=8.
    UF_CHECK_EQ(pcm_channels(Speed::X1, BwLimit::SendAll), 28u);
    UF_CHECK_EQ(pcm_channels(Speed::X1, BwLimit::NoAdat2), 20u);
    UF_CHECK_EQ(pcm_channels(Speed::X1, BwLimit::AnalogSpdif), 12u);
    UF_CHECK_EQ(pcm_channels(Speed::X1, BwLimit::AnalogOnly), 8u);
    // no-adat2 halves at 2x, zeroes at 4x.
    UF_CHECK_EQ(pcm_channels(Speed::X2, BwLimit::NoAdat2), 10u + 2u + 4u);
    UF_CHECK_EQ(pcm_channels(Speed::X4, BwLimit::NoAdat2), 12u);
}

UF_TEST(frames_and_payload) {
    // Full-packet block size = AMDTP syt_interval (blocking mode), HW-confirmed; corrects spec's 7/15/25.
    UF_CHECK_EQ(frames_per_packet(Speed::X1), 8u);
    UF_CHECK_EQ(frames_per_packet(Speed::X2), 16u);
    UF_CHECK_EQ(frames_per_packet(Speed::X4), 32u);
    // Full-packet payload = channels * 4 * syt_interval (blocking, HW-confirmed): 28*4*8 = 896 @ 1x.
    UF_CHECK_EQ(packet_payload_bytes(Speed::X1), 896u);
    // 20 * 4 * 16 = 1280 @ 2x; 12 * 4 * 32 = 1536 @ 4x.
    UF_CHECK_EQ(packet_payload_bytes(Speed::X2), 1280u);
    UF_CHECK_EQ(packet_payload_bytes(Speed::X4), 1536u);
}

UF_TEST(dbq_and_bandwidth) {
    // spec/09 §9.4: data_block_quadlets = 28 at 48k.
    UF_CHECK_EQ(data_block_quadlets(Speed::X1), 28u);
    UF_CHECK_EQ(pcm_channels_for_rate(48000), 28u);
    UF_CHECK_EQ(pcm_channels_for_rate(96000), 20u);
    UF_CHECK_EQ(pcm_channels_for_rate(192000), 12u);
    // bandwidth = 25 + full-packet payload = 25 + 28*4*8 = 921 units at 1x (blocking full packet).
    UF_CHECK_EQ(bandwidth_units(Speed::X1), 921u);
}

// ── Channel ordering ─────────────────────────────────────────────────────────────────────
UF_TEST(capture_map_1x) {
    auto m = channel_map(Direction::Capture, Speed::X1);
    UF_CHECK_EQ(m.size(), static_cast<size_t>(28));
    // slots 0-7 analog 1-8.
    UF_CHECK(m[0].kind == ChannelKind::Analog && m[0].index == 1);
    UF_CHECK(m[7].kind == ChannelKind::Analog && m[7].index == 8);
    // slots 8-9 analog 9,10 (capture has no phones).
    UF_CHECK(m[8].kind == ChannelKind::Analog && m[8].index == 9);
    UF_CHECK(m[9].kind == ChannelKind::Analog && m[9].index == 10);
    // slots 10-11 SPDIF 1,2.
    UF_CHECK(m[10].kind == ChannelKind::Spdif && m[10].index == 1);
    UF_CHECK(m[11].kind == ChannelKind::Spdif && m[11].index == 2);
    // slots 12.. ADAT 1..16.
    UF_CHECK(m[12].kind == ChannelKind::Adat && m[12].index == 1);
    UF_CHECK(m[27].kind == ChannelKind::Adat && m[27].index == 16);
}

UF_TEST(playback_map_phones) {
    auto m = channel_map(Direction::Playback, Speed::X1);
    UF_CHECK_EQ(m.size(), static_cast<size_t>(28));
    // slots 8-9 are phones L/R on playback (not analog 9/10).
    UF_CHECK(m[8].kind == ChannelKind::Phones && m[8].index == 1);
    UF_CHECK(m[9].kind == ChannelKind::Phones && m[9].index == 2);
    UF_CHECK_EQ(channel_name(m[8]), std::string("Phones 9"));
    UF_CHECK_EQ(channel_name(m[9]), std::string("Phones 10"));
}

UF_TEST(map_speed_and_mode) {
    // ADAT count shrinks with speed.
    UF_CHECK_EQ(channel_map(Direction::Capture, Speed::X2).size(), static_cast<size_t>(20));
    UF_CHECK_EQ(channel_map(Direction::Capture, Speed::X4).size(), static_cast<size_t>(12));
    // 4x has no ADAT: last slot is SPDIF 2.
    auto q = channel_map(Direction::Capture, Speed::X4);
    UF_CHECK(q.back().kind == ChannelKind::Spdif && q.back().index == 2);
    // Analog-only: 8 analog channels, no phones/spdif/adat.
    auto a = channel_map(Direction::Playback, Speed::X1, BwLimit::AnalogOnly);
    UF_CHECK_EQ(a.size(), static_cast<size_t>(8));
    for (auto& sl : a) UF_CHECK(sl.kind == ChannelKind::Analog);
}

// The full 28-entry tables, verbatim as the device ships them. A DAW session saved against the
// factory driver remembers its inputs by name, so any drift here silently
// breaks reopening those projects — which makes this worth pinning entry by entry rather than
// spot-checking a few.
UF_TEST(channel_names_match_the_factory_driver) {
    static const char* kIn[28] = {
        "Analog 1", "Analog 2", "Analog 3", "Analog 4", "Analog 5", "Analog 6", "Analog 7",
        "Analog 8", "Mic 9", "Mic 10", "SPDIF L", "SPDIF R",
        "ADAT 1", "ADAT 2", "ADAT 3", "ADAT 4", "ADAT 5", "ADAT 6", "ADAT 7", "ADAT 8",
        "ADAT 9", "ADAT 10", "ADAT 11", "ADAT 12", "ADAT 13", "ADAT 14", "ADAT 15", "ADAT 16"};
    static const char* kOut[28] = {
        "Analog 1", "Analog 2", "Analog 3", "Analog 4", "Analog 5", "Analog 6", "Analog 7",
        "Analog 8", "Phones 9", "Phones 10", "SPDIF L", "SPDIF R",
        "ADAT 1", "ADAT 2", "ADAT 3", "ADAT 4", "ADAT 5", "ADAT 6", "ADAT 7", "ADAT 8",
        "ADAT 9", "ADAT 10", "ADAT 11", "ADAT 12", "ADAT 13", "ADAT 14", "ADAT 15", "ADAT 16"};

    const auto in = channel_map(Direction::Capture, Speed::X1);
    const auto out = channel_map(Direction::Playback, Speed::X1);
    UF_CHECK_EQ(in.size(), static_cast<size_t>(28));
    UF_CHECK_EQ(out.size(), static_cast<size_t>(28));
    for (size_t i = 0; i < 28; ++i) {
        UF_CHECK_EQ(channel_name(in[i]), std::string(kIn[i]));
        UF_CHECK_EQ(channel_name(out[i]), std::string(kOut[i]));
    }
}

// The four groups RME publishes as separate streams, and the boundaries channels vanish along as the
// rate rises. Slots 0-7 analog, 8-9 mic/phones, 10-11 SPDIF, 12+ ADAT.
UF_TEST(channel_groups) {
    const auto in = channel_map(Direction::Capture, Speed::X1);
    const auto out = channel_map(Direction::Playback, Speed::X1);
    UF_CHECK_EQ(channel_group_name(in[0]), std::string("Analog"));
    UF_CHECK_EQ(channel_group_name(in[7]), std::string("Analog"));
    UF_CHECK_EQ(channel_group_name(in[8]), std::string("Mic"));      // capture 9/10 are the mic ins
    UF_CHECK_EQ(channel_group_name(out[8]), std::string("Phones"));  // playback 9/10 are the phones
    UF_CHECK_EQ(channel_group_name(in[11]), std::string("SPDIF"));
    UF_CHECK_EQ(channel_group_name(in[12]), std::string("ADAT"));
    UF_CHECK_EQ(channel_group_name(in[27]), std::string("ADAT"));

    // At 4x only the three non-ADAT groups survive: 8 analog + 2 mic/phones + 2 SPDIF = 12, the
    // channel count the protocol fixes for that speed class.
    const auto quad = channel_map(Direction::Capture, Speed::X4);
    UF_CHECK_EQ(quad.size(), static_cast<size_t>(12));
    for (const auto& sl : quad)
        if (channel_group_name(sl) == "ADAT") UF_FAIL("ADAT should be gone at 4x");
}

// One CoreAudio stream per group, so these (start, count) pairs are the stream layout — the same
// shape RME's per-model table holds. Getting a start wrong silently offsets a whole block of
// channels, so they are pinned.
UF_TEST(channel_groups_are_the_stream_layout) {
    auto g = channel_groups(Direction::Playback, Speed::X1);
    UF_CHECK_EQ(g.size(), static_cast<size_t>(4));
    UF_CHECK_EQ(g[0].name, std::string("Analog"));  UF_CHECK_EQ(g[0].start, 1u);  UF_CHECK_EQ(g[0].count, 8u);
    UF_CHECK_EQ(g[1].name, std::string("Phones"));  UF_CHECK_EQ(g[1].start, 9u);  UF_CHECK_EQ(g[1].count, 2u);
    UF_CHECK_EQ(g[2].name, std::string("SPDIF"));   UF_CHECK_EQ(g[2].start, 11u); UF_CHECK_EQ(g[2].count, 2u);
    UF_CHECK_EQ(g[3].name, std::string("ADAT"));    UF_CHECK_EQ(g[3].start, 13u); UF_CHECK_EQ(g[3].count, 16u);

    // Capture is the same shape with the mic inputs where the phones sit.
    auto in = channel_groups(Direction::Capture, Speed::X1);
    UF_CHECK_EQ(in.size(), static_cast<size_t>(4));
    UF_CHECK_EQ(in[1].name, std::string("Mic"));

    // 2x halves the ADAT group; 4x removes it and leaves three.
    auto dbl = channel_groups(Direction::Playback, Speed::X2);
    UF_CHECK_EQ(dbl.size(), static_cast<size_t>(4));
    UF_CHECK_EQ(dbl[3].count, 8u);
    auto quad = channel_groups(Direction::Playback, Speed::X4);
    UF_CHECK_EQ(quad.size(), static_cast<size_t>(3));
    for (const auto& x : quad)
        if (x.name == "ADAT") UF_FAIL("ADAT group should not exist at 4x");

    // Groups must tile the channel map exactly — no gaps, no overlaps, nothing dropped.
    for (auto speed : {Speed::X1, Speed::X2, Speed::X4})
        for (auto dir : {Direction::Capture, Direction::Playback}) {
            const auto gs = channel_groups(dir, speed);
            u32 expect = 1;
            for (const auto& x : gs) {
                if (x.start != expect) UF_FAIL("channel groups do not tile the map");
                expect += x.count;
            }
            UF_CHECK_EQ(expect - 1, (u32)channel_map(dir, speed).size());
        }
}

UF_TEST_MAIN()
