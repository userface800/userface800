// The zero-timestamp clock's safety invariants. These exist because a bad (sampleTime, hostTime)
// pair does not degrade gracefully: the HAL extrapolates every IO wake-up from it, so an anchor that
// stops advancing makes coreaudiod run IO cycles back to back and peg a core (it hung the
// machine, and survived `killall coreaudiod` because the restarted daemon re-mapped the same stale
// ring). That takes the whole system with it, so the invariants are proven here,
// headless, before the bundle is installed.
#include <vector>
#include "../platform/shared/uf_zero_timestamp.hpp"
#include "uf_test.hpp"

using uf::ZeroTimeStampClock;

// Work in nanoseconds-as-ticks: the class is unit-agnostic, and 48 kHz/512 frames makes one period
// 10.667 ms, matching the real device.
static constexpr uint32_t kPeriod        = 512;
static constexpr double   kTicksPerFrame = 1.0e9 / 48000.0;
static constexpr uint64_t kPeriodTicks   = (uint64_t)(kTicksPerFrame * kPeriod);

// Ticks are nanoseconds here, so ticksPerSecond is 1e9.
static ZeroTimeStampClock make() { return ZeroTimeStampClock(kPeriod, kTicksPerFrame, 1.0e9); }
static const uint64_t kStaleTicks = ZeroTimeStampClock(kPeriod, kTicksPerFrame, 1.0e9).staleTicks();

// The real anchor interval is a fixed FRAME count, so its duration changes with the rate — 42.7 ms
// at 48 kHz, 10.7 ms at 192 kHz. The staleness window has to cover it at BOTH ends: too small and
// the clock unlocks on every anchor at low rates, too large and a dead daemon goes unnoticed for
// longer than it should. These are the rates and the period the driver actually ships.
static constexpr uint32_t kRealAnchor = 2048;
static ZeroTimeStampClock makeAt(double rate) {
    return ZeroTimeStampClock(kRealAnchor, 1.0e9 / rate, 1.0e9);
}

// Every invariant, checked over a whole sequence, so no test has to remember to assert them all.
struct Invariants {
    double   lastSample = -1;
    uint64_t lastHost   = 0;
    uint64_t seedMoves  = 0;
    uint64_t lastSeed   = 0;
    bool     first      = true;

    void feed(const ZeroTimeStampClock::Stamp& s, uint64_t now) {
        if (std::fmod(s.sampleTime, (double)kPeriod) != 0.0)
            UF_FAIL("sampleTime is not a multiple of the period");
        if (s.sampleTime < 0) UF_FAIL("sampleTime went negative");
        if (!first && s.sampleTime < lastSample) UF_FAIL("sampleTime went backwards");
        if (!first && s.hostTime < lastHost) UF_FAIL("hostTime went backwards");
        // The anti-busy-loop invariant: the HAL must never be handed an anchor so far in the past
        // that every wake time it computes from it is already overdue.
        if (now > s.hostTime && now - s.hostTime > kStaleTicks)
            UF_FAIL("hostTime fell further behind now than the staleness bound");
        if (!first && s.seed != lastSeed) ++seedMoves;
        lastSample = s.sampleTime; lastHost = s.hostTime; lastSeed = s.seed; first = false;
    }
};

// ── the healthy case: the output IS the device clock ──────────────────────────────────────────

UF_TEST(locked_output_tracks_the_device_clock) {
    auto c = make();
    Invariants inv;
    uint64_t t0 = 1'000'000'000;
    // The daemon publishes one anchor per period; the HAL asks several times per period.
    double devSample = 0;
    uint64_t devHost = t0;
    for (int p = 0; p < 200; ++p) {
        for (int k = 0; k < 4; ++k) {
            uint64_t now = devHost + k * (kPeriodTicks / 4);
            inv.feed(c.next(now, true, devSample, devHost), now);
        }
        devSample += kPeriod;
        devHost   += kPeriodTicks;
    }
    UF_CHECK(c.locked());
    // The lock happens on the very first call, so across the whole run the HAL is handed a constant
    // seed — no discontinuity at all. A seed that keeps moving stops it ever locking a rate.
    UF_CHECK_EQ(inv.seedMoves, 0u);
    // And the timeline advanced one period per published anchor, at the device's rate rather than
    // some free-run guess. (It sits one period ahead of the raw device count: gaining the lock
    // always steps forward, so the timeline can never stall on the anchor it locked to.)
    UF_CHECK(inv.lastSample == 200.0 * kPeriod);
}

UF_TEST(device_rate_is_followed_not_the_nominal_rate) {
    // The FF800's crystal runs ~48002, not 48000 (that is the whole point of the servo). The clock
    // must report the device's real slope so CoreAudio resamples correctly.
    auto c = make();
    const double realRate = 48002.0;
    const uint64_t realPeriodTicks = (uint64_t)(1.0e9 / realRate * kPeriod);
    uint64_t devHost = 1'000'000'000;
    double devSample = 0;
    ZeroTimeStampClock::Stamp first{}, last{};
    for (int p = 0; p < 500; ++p) {
        auto s = c.next(devHost, true, devSample, devHost);
        if (p == 0) first = s;
        last = s;
        devSample += kPeriod;
        devHost   += realPeriodTicks;
    }
    const double frames = last.sampleTime - first.sampleTime;
    const double secs   = (double)(last.hostTime - first.hostTime) / 1.0e9;
    UF_CHECK(std::abs(frames / secs - realRate) < 1.0);
}

// ── the failure that hung the machine ─────────────────────────────────────────────────────────

UF_TEST(frozen_device_anchor_cannot_stall_the_timeline) {
    // A wedged device (or an orphaned shm mapping) leaves the ring publishing one pair forever.
    auto c = make();
    Invariants inv;
    const uint64_t t0 = 1'000'000'000;
    c.next(t0, true, 0, t0);
    // Ten seconds of wall clock against an anchor that never moves again.
    for (uint64_t now = t0; now < t0 + 10'000'000'000ull; now += kPeriodTicks / 4)
        inv.feed(c.next(now, true, 0, t0), now);
    UF_CHECK(!c.locked());                       // the stale anchor was rejected, not believed
    // The timeline kept moving at roughly the nominal rate rather than freezing.
    const double expected = 10.0 * 48000.0;
    UF_CHECK(inv.lastSample > expected * 0.9 && inv.lastSample < expected * 1.1);
    // Losing the device clock is exactly one discontinuity, not one per call.
    UF_CHECK_EQ(inv.seedMoves, 1u);
}

UF_TEST(daemon_restart_never_moves_the_timeline_backwards) {
    // A daemon restart shm_unlinks and re-creates the ring, so the frame count begins again at 0
    // while the HAL has already been handed a large sample time.
    auto c = make();
    Invariants inv;
    uint64_t now = 1'000'000'000;
    double devSample = 0;
    uint64_t devHost = now;
    for (int p = 0; p < 100; ++p) {              // run for a while
        inv.feed(c.next(devHost, true, devSample, devHost), devHost);
        devSample += kPeriod; devHost += kPeriodTicks;
    }
    const double before = inv.lastSample;
    now = devHost + 3'000'000'000ull;            // three seconds of gap while the daemon restarts
    devSample = 0; devHost = now;                // ...and the new ring counts from zero
    for (int p = 0; p < 100; ++p) {
        inv.feed(c.next(devHost, true, devSample, devHost), devHost);
        devSample += kPeriod; devHost += kPeriodTicks;
    }
    UF_CHECK(c.locked());                        // re-locked to the new device timeline
    UF_CHECK(inv.lastSample > before);           // and kept going forwards across the seam
}

UF_TEST(no_daemon_at_all_free_runs_at_the_nominal_rate) {
    // The device must still work solo: no ring mapped, ever.
    auto c = make();
    Invariants inv;
    const uint64_t t0 = 5'000'000'000;
    for (uint64_t now = t0; now < t0 + 2'000'000'000ull; now += kPeriodTicks / 3)
        inv.feed(c.next(now, false, 0, 0), now);
    UF_CHECK(!c.locked());
    const double expected = 2.0 * 48000.0;
    UF_CHECK(inv.lastSample > expected * 0.9 && inv.lastSample < expected * 1.1);
    UF_CHECK_EQ(inv.seedMoves, 0u);              // never locked, so never discontinuous
}

// ── garbage in, safe out ──────────────────────────────────────────────────────────────────────

UF_TEST(garbage_device_anchors_are_rejected) {
    auto c = make();
    Invariants inv;
    const uint64_t t0 = 1'000'000'000;
    struct { const char* what; double sample; uint64_t host; } bad[] = {
        {"never published",      0,          0},
        {"host far in future",   1024,       t0 + 60'000'000'000ull},
        {"host far in past",     1024,       t0 - 60'000'000'000ull},
        {"negative sample",      -4096,      t0},
        {"NaN sample",           std::nan(""), t0},
        {"infinite sample",      HUGE_VAL,   t0},
    };
    uint64_t now = t0;
    for (auto& b : bad) {
        for (int k = 0; k < 8; ++k, now += kPeriodTicks / 2)
            inv.feed(c.next(now, true, b.sample, b.host), now);
        if (c.locked()) UF_FAIL(std::string("believed a bad anchor: ") + b.what);
    }
}

UF_TEST(a_torn_read_between_good_anchors_is_ridden_out) {
    // readTimestamp returns false on a torn seqlock read; a few in a row must not drop the lock or
    // move the seed, or the HAL re-anchors on ordinary contention.
    auto c = make();
    Invariants inv;
    uint64_t devHost = 1'000'000'000;
    double devSample = 0;
    for (int p = 0; p < 60; ++p) {
        const bool torn = (p % 7) == 3;          // occasional failed read
        inv.feed(c.next(devHost, !torn, devSample, devHost), devHost);
        devSample += kPeriod; devHost += kPeriodTicks;
    }
    UF_CHECK(c.locked());                        // still locked, including after a torn last read
    UF_CHECK_EQ(inv.seedMoves, 0u);              // and the HAL never saw a discontinuity
}

// ── the property test: nothing at all can break the invariants ────────────────────────────────

UF_TEST(adversarial_input_sequence_holds_every_invariant) {
    // Deterministic pseudo-random abuse: valid/invalid, jumping, frozen, backwards, huge, tiny.
    auto c = make();
    Invariants inv;
    uint64_t rng = 0x9e3779b97f4a7c15ull;
    auto rand = [&] { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; };

    uint64_t now = 1'000'000'000;
    double devSample = 0;
    uint64_t devHost = now;
    for (int i = 0; i < 20000; ++i) {
        now += (rand() % (kPeriodTicks * 2)) + 1;          // time always moves forwards
        switch (rand() % 6) {
            case 0: devSample += kPeriod; devHost = now; break;        // healthy
            case 1: break;                                              // frozen anchor
            case 2: devSample = 0; devHost = now; break;                // daemon restart
            case 3: devHost = now - (rand() % 30'000'000'000ull); break;// arbitrarily stale
            case 4: devSample = (double)(rand() % 1'000'000'000); break;// nonsense jump
            case 5: devHost = now + (rand() % 10'000'000'000ull); break;// host in the future
        }
        inv.feed(c.next(now, (rand() & 3) != 0, devSample, devHost), now);
    }
}

// ── the shipping anchor interval, at both ends of the rate range ──────────────────────────────

UF_TEST(anchors_at_the_real_interval_never_unlock) {
    // A daemon publishing exactly on time, one anchor per period, must never drop the lock. This is
    // the case an earlier version got wrong: the window was a fixed duration that did not account
    // for the interval, so at 48 kHz — where one interval is already 42.7 ms — every anchor looked
    // overdue and the clock unlocked continuously.
    for (double rate : {48000.0, 96000.0, 192000.0}) {
        auto c = makeAt(rate);
        Invariants inv;
        const uint64_t interval = (uint64_t)(kRealAnchor * 1.0e9 / rate);   // ns
        uint64_t devHost = 1'000'000'000;
        double devSample = 0;
        for (int p = 0; p < 300; ++p) {
            // The HAL asks several times per interval; the anchor only moves once.
            for (int k = 0; k < 4; ++k) {
                const uint64_t now = devHost + k * (interval / 4);
                inv.feed(c.next(now, true, devSample, devHost), now);
            }
            devSample += kRealAnchor;
            devHost   += interval;
        }
        if (!c.locked()) UF_FAIL("unlocked while anchors arrived exactly on schedule");
        UF_CHECK_EQ(inv.seedMoves, 0u);      // no discontinuity the HAL could see
    }
}

UF_TEST(a_late_anchor_within_the_margin_is_tolerated) {
    // One interval of jitter on top of the interval itself must still be fine — that is what the
    // fixed margin buys, and DAW load routinely costs that much.
    auto c = makeAt(48000.0);
    const uint64_t interval = (uint64_t)(kRealAnchor * 1.0e9 / 48000.0);
    uint64_t devHost = 1'000'000'000;
    double devSample = 0;
    for (int p = 0; p < 50; ++p) {
        c.next(devHost, true, devSample, devHost);
        devSample += kRealAnchor;
        devHost += interval + (p == 25 ? 30'000'000ull : 0);   // one 30 ms hiccup
    }
    UF_CHECK(c.locked());
}

UF_TEST(a_dead_daemon_is_still_noticed_at_every_rate) {
    // The window must not have grown so far that a stopped publisher goes unseen. Whatever the rate,
    // a frozen anchor has to be abandoned and the timeline keep moving — that is the anti-spin
    // guarantee, and widening the window for the anchor interval must not have cost it.
    for (double rate : {48000.0, 192000.0}) {
        auto c = makeAt(rate);
        const uint64_t t0 = 1'000'000'000;
        c.next(t0, true, 0, t0);
        double last = 0;
        for (uint64_t now = t0; now < t0 + 3'000'000'000ull; now += 5'000'000ull) {
            auto s = c.next(now, true, 0, t0);      // anchor never moves again
            last = s.sampleTime;
        }
        if (c.locked()) UF_FAIL("still locked to an anchor frozen for three seconds");
        if (!(last > 0)) UF_FAIL("timeline stalled instead of free-running");
    }
}

UF_TEST_MAIN()
