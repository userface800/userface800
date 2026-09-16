// SPSC shared-ring semantics — the daemon<->plugin audio path. Pure logic, host-tested; the shared
// memory itself and the realtime timing are exercised on hardware.
#include <thread>
#include <vector>
#include "../platform/shared/uf_shm_ring.hpp"
#include "uf_test.hpp"

using uf::shm::Ring;
using uf::shm::Slot;
using uf::shm::kSlots;

UF_TEST(empty_ring_reads_nothing) {
    Ring r; r.init();
    UF_CHECK_EQ(r.depth(), 0u);
    UF_CHECK(r.acquireRead() == nullptr);       // empty -> underrun
    UF_CHECK_EQ(r.underruns.load(), 1u);
}

UF_TEST(write_then_read_roundtrips) {
    Ring r; r.init();
    Slot* w = r.acquireWrite();
    UF_CHECK(w != nullptr);
    w->frameCount = 512; w->channelCount = 28; w->audio[0] = 0x123456;
    r.commitWrite();
    UF_CHECK_EQ(r.depth(), 1u);

    const Slot* rd = r.acquireRead();
    UF_CHECK(rd != nullptr);
    UF_CHECK_EQ(rd->frameCount, 512u);
    UF_CHECK_EQ(rd->audio[0], 0x123456);
    r.commitRead();
    UF_CHECK_EQ(r.depth(), 0u);
}

UF_TEST(full_ring_reports_overrun) {
    Ring r; r.init();
    for (uint32_t i = 0; i < kSlots; ++i) { UF_CHECK(r.acquireWrite() != nullptr); r.commitWrite(); }
    UF_CHECK_EQ(r.depth(), kSlots);
    UF_CHECK(r.acquireWrite() == nullptr);      // full -> overrun
    UF_CHECK_EQ(r.overruns.load(), 1u);
}

UF_TEST(wraps_past_capacity) {
    Ring r; r.init();
    // Push and pop 3x the ring size one at a time; the index & (kSlots-1) must wrap cleanly.
    for (uint32_t i = 0; i < kSlots * 3; ++i) {
        Slot* w = r.acquireWrite(); UF_CHECK(w != nullptr);
        w->audio[0] = (int32_t)i; r.commitWrite();
        const Slot* rd = r.acquireRead(); UF_CHECK(rd != nullptr);
        UF_CHECK_EQ(rd->audio[0], (int32_t)i); r.commitRead();
    }
}

UF_TEST(concurrent_producer_consumer) {
    static Ring r; r.init();
    const int N = 100000;
    std::thread prod([&]{
        for (int i = 0; i < N; ) {
            Slot* w = r.acquireWrite();
            if (!w) { std::this_thread::yield(); continue; }
            w->audio[0] = i; r.commitWrite(); ++i;
        }
    });
    int got = 0;
    while (got < N) {
        const Slot* rd = r.acquireRead();
        if (!rd) { std::this_thread::yield(); continue; }
        UF_CHECK_EQ(rd->audio[0], got);         // SPSC preserves order
        r.commitRead(); ++got;
    }
    prod.join();
    UF_CHECK_EQ(got, N);
}

UF_TEST(timestamp_unpublished_reads_false) {
    Ring r; r.init();
    double st; uint64_t ht, seed;
    UF_CHECK(!r.readTimestamp(&st, &ht, &seed));     // nothing published yet
}

UF_TEST(timestamp_roundtrips) {
    Ring r; r.init();
    r.publishTimestamp(48000.0, 123456789ull);
    double st = 0; uint64_t ht = 0, seed = 0;
    UF_CHECK(r.readTimestamp(&st, &ht, &seed));
    UF_CHECK(st == 48000.0);
    UF_CHECK_EQ(ht, 123456789ull);
    UF_CHECK(seed != 0);
}

UF_TEST(timestamp_concurrent_never_torn) {
    static Ring r; r.init();
    std::thread pub([&]{
        for (uint64_t i = 1; i <= 200000; ++i) r.publishTimestamp((double)i, i * 1000ull);
    });
    // The consumer must only ever see a coherent pair: hostTime == sampleTime*1000. Record a tear
    // rather than throwing here — a CHECK would unwind past pub.join() and terminate on the still
    // joinable thread, turning a plain test failure into a crash with no diagnostic.
    int reads = 0, torn = 0;
    for (int i = 0; i < 500000; ++i) {
        double st; uint64_t ht, seed;
        if (r.readTimestamp(&st, &ht, &seed)) {
            if ((uint64_t)st * 1000ull != ht) ++torn;   // a torn mix of two publishes
            ++reads;
        }
    }
    pub.join();
    UF_CHECK_EQ(torn, 0);
    UF_CHECK(reads > 0);
}

UF_TEST_MAIN()
