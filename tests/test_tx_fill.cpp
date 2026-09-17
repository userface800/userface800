// The transmit fill loop — what goes into a packet when the playback ring has data, and what goes
// in when it does not. These cover the per-tick `dry` latch with no device attached; see
// uf_tx_fill.hpp for the bug they pin down.
#include <vector>
#include "../platform/shared/uf_tx_fill.hpp"
#include "uf_test.hpp"

using uf::TxFill;
using uf::shm::Ring;
using uf::shm::Slot;

namespace {

// A ring in ordinary memory. The real one lives in shared memory, but nothing here depends on that.
struct Fixture {
    std::vector<unsigned char> mem = std::vector<unsigned char>(sizeof(Ring), 0);
    Ring* ring() { return reinterpret_cast<Ring*>(mem.data()); }
    Fixture() { ring()->init(1); }

    // Push one slot whose channel c of frame f is value(f, c).
    template <class F>
    void push(uint32_t frames, uint32_t channels, F value) {
        Slot* s = ring()->acquireWrite();
        UF_CHECK(s != nullptr);
        s->frameCount = frames;
        s->channelCount = channels;
        for (uint32_t f = 0; f < frames; ++f)
            for (uint32_t c = 0; c < channels; ++c)
                s->audio[(std::size_t)f * channels + c] = value(f, c);
        ring()->commitWrite();
    }
};

constexpr uint32_t kCh = 12;   // 192 kHz: 12 channels on the wire

}  // namespace

UF_TEST(frames_come_from_the_ring_when_it_has_data) {
    Fixture fx;
    fx.push(4, kCh, [](uint32_t f, uint32_t c) { return (int32_t)(1000 * (f + 1) + c); });

    TxFill fill(kCh);
    fill.beginTick();
    for (uint32_t f = 0; f < 4; ++f) {
        fill.beginPacket();
        const int32_t* fr = fill.frame(*fx.ring());
        for (uint32_t c = 0; c < kCh; ++c)
            UF_CHECK_EQ(fr[c], (int32_t)(1000 * (f + 1) + c));
    }
    UF_CHECK(!fill.dryTick());
    UF_CHECK_EQ(fill.held(), 0u);
}

UF_TEST(empty_ring_holds_the_last_frame_rather_than_zeroing) {
    Fixture fx;
    fx.push(1, kCh, [](uint32_t, uint32_t c) { return (int32_t)(1 << 20) + (int32_t)c; });

    TxFill fill(kCh);
    fill.beginTick();
    fill.beginPacket();
    const int32_t* good = fill.frame(*fx.ring());
    const int32_t held0 = good[0];
    UF_CHECK_EQ(held0, (int32_t)(1 << 20));

    // Ring is now empty. The next frame must be the held value, NOT silence.
    fill.beginPacket();
    const int32_t* h = fill.frame(*fx.ring());
    UF_CHECK(h[0] != 0);
    UF_CHECK(h[0] == held0);          // first held frame repeats exactly
    UF_CHECK(fill.dryTick());
    UF_CHECK_EQ(fill.held(), 1u);
}

// THE REGRESSION TEST. The old code latched `dry` for a whole tick, so once the ring came up empty
// every later frame in that tick was zeroed even after the producer refilled it. Here the ring is
// empty for exactly one packet and then has data again: the frames after the refill must come from
// the ring, not be collateral damage from the earlier miss.
UF_TEST(a_miss_does_not_silence_the_rest_of_the_tick) {
    Fixture fx;
    fx.push(1, kCh, [](uint32_t, uint32_t c) { return (int32_t)(0x111000 + c); });

    TxFill fill(kCh);
    fill.beginTick();

    fill.beginPacket();
    UF_CHECK_EQ(fill.frame(*fx.ring())[0], (int32_t)0x111000);

    fill.beginPacket();                       // ring empty: this packet is held
    fill.frame(*fx.ring());
    UF_CHECK_EQ(fill.held(), 1u);

    // Producer catches up mid-tick.
    fx.push(1, kCh, [](uint32_t, uint32_t c) { return (int32_t)(0x222000 + c); });

    fill.beginPacket();                       // must pick the new data up immediately
    UF_CHECK_EQ(fill.frame(*fx.ring())[0], (int32_t)0x222000);
    UF_CHECK_EQ(fill.held(), 1u);            // and cost nothing further
}

// Within ONE packet a miss does stay latched — that is deliberate, so acquireRead() is attempted
// once per packet and the ring's underrun counter keeps meaning something.
UF_TEST(the_latch_is_scoped_to_a_packet) {
    Fixture fx;
    TxFill fill(kCh);
    fill.beginTick();
    fill.beginPacket();
    fill.frame(*fx.ring());                   // miss, latches for this packet
    fx.push(1, kCh, [](uint32_t, uint32_t c) { return (int32_t)(0x333000 + c); });
    fill.frame(*fx.ring());                   // same packet: still held, ring untouched
    UF_CHECK_EQ(fill.held(), 2u);
    UF_CHECK_EQ(fx.ring()->depth(), 1u);     // the pushed slot was NOT consumed

    fill.beginPacket();                       // new packet: retries and finds it
    UF_CHECK_EQ(fill.frame(*fx.ring())[0], (int32_t)0x333000);
}

UF_TEST(a_long_dry_spell_decays_to_silence_not_dc) {
    Fixture fx;
    fx.push(1, kCh, [](uint32_t, uint32_t) { return (int32_t)(1 << 24); });

    TxFill fill(kCh);
    fill.beginTick();
    fill.beginPacket();
    fill.frame(*fx.ring());

    // 20000 held frames — a tenth of a second at 192 kHz, far longer than any real gap.
    const int32_t* fr = nullptr;
    for (int i = 0; i < 20000; ++i) { fill.beginPacket(); fr = fill.frame(*fx.ring()); }

    // Below 256 the encoder's >>8 makes it a literal zero on the wire, which is the point: no DC
    // parked on the analog outputs.
    UF_CHECK(fr[0] >= 0);
    UF_CHECK(fr[0] < 256);
    UF_CHECK_EQ(fr[0] >> 8, 0);
}

UF_TEST(slots_are_consumed_in_order_across_packets) {
    Fixture fx;
    for (int s = 0; s < 3; ++s)
        fx.push(2, kCh, [s](uint32_t f, uint32_t) { return (int32_t)(s * 10 + f); });

    TxFill fill(kCh);
    fill.beginTick();
    for (int s = 0; s < 3; ++s)
        for (uint32_t f = 0; f < 2; ++f) {
            fill.beginPacket();
            UF_CHECK_EQ(fill.frame(*fx.ring())[0], (int32_t)(s * 10 + f));
        }
    UF_CHECK_EQ(fill.held(), 0u);
    // The final slot is still held: release is lazy, happening on the next frame() or on release().
    // That is deliberate — committing eagerly would hand the slot back before we know the packet
    // was actually built from it.
    UF_CHECK_EQ(fx.ring()->depth(), 1u);
    fill.release(*fx.ring());
    UF_CHECK_EQ(fx.ring()->depth(), 0u);
}

UF_TEST(release_hands_the_held_slot_back) {
    Fixture fx;
    fx.push(4, kCh, [](uint32_t, uint32_t) { return 7; });

    TxFill fill(kCh);
    fill.beginTick(); fill.beginPacket();
    fill.frame(*fx.ring());                   // consumes 1 of 4 frames, still holding the slot
    UF_CHECK_EQ(fx.ring()->depth(), 1u);
    fill.release(*fx.ring());
    UF_CHECK_EQ(fx.ring()->depth(), 0u);     // consumer cursor advanced past it
}

UF_TEST(narrow_slots_zero_fill_the_channels_they_lack) {
    Fixture fx;
    fx.push(1, 4, [](uint32_t, uint32_t c) { return (int32_t)(c + 1); });   // only 4 channels

    TxFill fill(kCh);
    fill.beginTick(); fill.beginPacket();
    const int32_t* fr = fill.frame(*fx.ring());
    for (uint32_t c = 0; c < 4; ++c) UF_CHECK_EQ(fr[c], (int32_t)(c + 1));
    for (uint32_t c = 4; c < kCh; ++c) UF_CHECK_EQ(fr[c], 0);
}

UF_TEST_MAIN()
