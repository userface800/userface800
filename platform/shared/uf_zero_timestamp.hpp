// uf_zero_timestamp.hpp — the AudioServerPlugIn zero-timestamp clock, as pure logic.
//
// CoreAudio drives its whole IO timeline from GetZeroTimeStamp: the plugin returns the (sampleTime,
// hostTime) pair of the most recent period boundary, and the HAL schedules every IO cycle by
// extrapolating from it. That makes the function a loaded gun. If the pair stops advancing — a wedged
// device, a daemon restart that orphans the plugin's shm mapping — every wake time the HAL computes
// from it is already in the past, so it runs IO cycles back to back with no delay and coreaudiod
// pegs a core. That is not a theoretical failure: it hung the machine, and it survived
// `killall coreaudiod` because the restarted coreaudiod re-mapped the same stale ring.
//
// So the device clock is treated as an *input to be validated*, never as the answer. This class owns
// the timeline and guarantees, for any sequence of inputs whatsoever:
//
//   1. sampleTime is always a non-negative multiple of the zero-timestamp period;
//   2. sampleTime never decreases;
//   3. hostTime never decreases, and is NEVER more than staleTicks behind `now` — this is the
//      invariant that makes a busy-loop unreachable no matter what the daemon publishes;
//   4. seed changes only on a genuine discontinuity (gaining or losing the device clock), never
//      per-call — the HAL re-anchors its rate estimate whenever the seed moves, so a seed that
//      bumps every call never lets it lock a rate.
//
// With a healthy daemon the output IS the device clock, so CoreAudio's resampler runs at the FF800's
// real rate — the same trick the factory driver plays, timestamping on the DMA program's wrap. With
// a sick daemon it degrades to a free-running nominal-rate clock, which sounds wrong but keeps the
// machine alive.
//
// Pure C++ (no mach, no CoreAudio) precisely so the invariants above are unit-testable with no
// coreaudiod in the loop — see tests/test_zero_timestamp.cpp. A spin takes the whole system with it,
// so they are proven here before the bundle is ever installed.
#pragma once
#include <cmath>
#include <cstdint>

namespace uf {

class ZeroTimeStampClock {
public:
    struct Stamp {
        double   sampleTime;   // frame index of a period boundary; a multiple of the period
        uint64_t hostTime;     // mach_absolute_time at which the device reached that frame
        uint64_t seed;         // bumps only when the timeline is discontinuous
    };

    // period: frames per zero-timestamp period (the device's ZeroTimeStampPeriod).
    // ticksPerFrame: host clock ticks per audio frame at the nominal rate — the free-run slope.
    // ticksPerSecond: host clock ticks per second, so the staleness window below can be a real
    // duration rather than a frame count. Constant for the life of the process.
    ZeroTimeStampClock(uint32_t period, double ticksPerFrame, double ticksPerSecond)
        : period_(period ? period : 1) {
        const double st = ticksPerSecond * (double)kStaleNanos / 1.0e9;
        staleTicks_ = st >= 1.0 ? (uint64_t)st : 1;
        setTicksPerFrame(ticksPerFrame);
    }

    // The staleness window in host ticks — the same duration at every sample rate.
    uint64_t staleTicks() const { return staleTicks_; }

    // Follows a nominal-rate change. Only affects free-running; while locked the device sets the rate.
    void setTicksPerFrame(double ticksPerFrame) {
        double t = ticksPerFrame * (double)period_;
        ticksPerPeriod_ = t >= 1.0 ? (uint64_t)t : 1;
    }

    // How far behind `now` the anchor may fall before we stop believing it. Also the freshness bound
    // on the device pair, so a plausible device anchor can never violate invariant 3.
    //
    // Specified in TIME, not in periods. What has to fit inside this window is the daemon's
    // publishing jitter — scheduling, a pump tick, the IPC — all of which are roughly constant in
    // milliseconds. A period is 512 frames, so a fixed count of them shrinks as the rate rises:
    // 43 ms at 48 kHz but 21 ms at 96 kHz and 11 ms at 192 kHz, tightening the tolerance exactly
    // where the deadlines are already tightest. That is the same mistake as sizing the safety offset
    // in frames, and it showed up on hardware at 96 kHz as a momentary stall on a routing change —
    // the clock dropping lock, free-running, and re-anchoring.
    //
    // ~43 ms is what four periods gave at 48 kHz, which rode out DAW load fine, so it is kept as the
    // absolute figure at every rate.
    static constexpr uint64_t kStaleNanos = 43'000'000;

    // `now` = mach_absolute_time(). devValid is false when the ring is unmapped or the seqlock read
    // tore; devSample/devHost are the ring's latest published anchor.
    Stamp next(uint64_t now, bool devValid, double devSample, uint64_t devHost) {
        if (!started_) { started_ = true; host_ = now; }

        if (devValid && plausible(now, devSample, devHost)) {
            // Quantise before use: the timeline's "multiple of the period" invariant must not depend
            // on the daemon getting its own quantisation right.
            const double s = std::floor(devSample / (double)period_) * (double)period_;
            if (!locked_ || s < devSampleSeen_) {
                // Gaining the device clock, or the device timeline restarted under us (a daemon
                // restart makes a fresh ring whose frame count begins at 0 again). Graft it onto our
                // own monotone timeline rather than following it backwards, and tell the HAL.
                offset_ = sample_ + (double)period_ - s;
                locked_ = true;
                ++seed_;
            }
            devSampleSeen_ = s;
            // Adopt only a wholly newer anchor. Half of one — a frame count that advanced against an
            // older host time, or the reverse — is not a correspondence that ever happened, and
            // taking it piecemeal is how the pair stops meaning anything.
            const double out = s + offset_;
            if (out > sample_ && devHost >= host_) { sample_ = out; host_ = devHost; }
        }

        // The anchor we are about to hand back is what matters, whatever produced it — so the
        // staleness test is applied HERE rather than only to the ring's input. That distinction is
        // load-bearing: a wedged FF800 leaves the daemon publishing a fresh host time against a
        // frame count that has stopped advancing, so the input looks alive on every read while the
        // anchor we actually return sits still. Testing the input alone stays "locked" forever and
        // hands CoreAudio an ever-more-overdue wake time, which is the busy-loop.
        //
        // Falling behind is therefore the single definition of having lost the device clock, and it
        // covers the lot: unmapped ring, torn read, frozen publisher, wedged device. It also means a
        // stray torn read costs nothing — the anchor is still fresh, so we hold it and stay locked
        // rather than bouncing the seed on ordinary seqlock contention.
        if (now > host_) {
            const uint64_t lag = now - host_;
            if (locked_ && lag > staleTicks_) { locked_ = false; ++seed_; }
            if (!locked_) {
                const uint64_t periods = lag / ticksPerPeriod_;   // free-run at the nominal rate
                if (periods) {
                    sample_ += (double)periods * (double)period_;
                    host_   += periods * ticksPerPeriod_;
                }
            }
        }
        return {sample_, host_, seed_};
    }

    bool locked() const { return locked_; }

private:
    // A device anchor we are willing to believe: published at all, not in the future by more than a
    // period (the daemon stamps it slightly before we read it), and recent enough that adopting its
    // host time cannot put us in violation of invariant 3.
    bool plausible(uint64_t now, double devSample, uint64_t devHost) const {
        if (devHost == 0 || !std::isfinite(devSample) || devSample < 0) return false;
        if (devHost > now) return devHost - now <= ticksPerPeriod_;
        return now - devHost <= staleTicks_;
    }

    const uint32_t period_;
    uint64_t ticksPerPeriod_ = 1;
    uint64_t staleTicks_ = 1;
    double   sample_  = 0;      // our timeline: always a multiple of period_, never decreasing
    uint64_t host_    = 0;
    uint64_t seed_    = 1;      // CoreAudio treats a change as "timeline discontinuity, re-anchor"
    double   offset_  = 0;      // device frame count -> our timeline
    double   devSampleSeen_ = 0;
    bool     locked_  = false;
    bool     started_ = false;
};

}  // namespace uf
