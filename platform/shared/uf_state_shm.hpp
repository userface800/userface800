// uf_state_shm.hpp — the daemon's published state, for anything that wants to READ it.
//
// The daemon->clients half of the control plane. One writer, N readers: cost is
// O(1) in the number of UIs, and a stalled or crashed reader structurally cannot backpressure the
// realtime pump — which is exactly why reads go here and commands go over the socket.
//
// A SEPARATE shm object from the audio ring, deliberately. Growing uf_shm_ring.hpp would change its
// ABI and force the AudioServerPlugIn to be rebuilt and reinstalled in lockstep, for data the plugin
// has no interest in. We have already had two sessions derailed by exactly that (a daemon built
// against a bumped kMagic against an older installed plugin: device present, transport frozen, no
// audio). Keeping them independent means a UI change can never break coreaudiod.
//
// Readers must use the seqlock: read seq, read the payload, read seq again, retry if it changed or
// is odd. The publisher never blocks and never waits for a reader.
#pragma once
#include <atomic>
#include <cstdint>

#include "uf_meters.hpp"
#include "uf/protocol/settings.hpp"

namespace uf::state {

inline constexpr char     kName[]  = "/userface800.state";
inline constexpr uint32_t kMagic   = 0x55465331;   // "UFS1"

// Bump whenever the layout changes. READERS MUST CHECK THIS, not just the magic: the magic is
// unchanged across layout revisions, so a new client attaching to an older daemon's segment (or the
// reverse) would map a struct whose fields have moved and read confident nonsense — a matrix of
// garbage gains, drawn as if it were the truth. The daemon unlinks and recreates the segment at
// startup, so the mismatch window is exactly "one side has been rebuilt and the other has not",
// which is the normal state of affairs mid-development.
inline constexpr uint32_t kVersion = 3;
inline constexpr uint32_t kCells   = 0x2000 / 4;   // the matrix, same shape as MixerShadow

struct Shared {
    uint32_t magic;
    uint32_t version;

    // Odd => a write is in progress. Bracketing the payload with this is what lets a reader detect
    // that it saw a half-updated matrix, without the writer ever taking a lock a realtime thread
    // could block on.
    std::atomic<uint64_t> seq;

    uint32_t deviceRate;      // 0 = not streaming
    uint32_t clockSource;     // uf::ctl::ClockSource, as the DEVICE reports it
    uint32_t clockLocked;     // 1 = locked and synced
    uint32_t dbq;             // channels on the wire at this rate: 28 / 20 / 12

    uint32_t cells[kCells];   // the mixer matrix as the DEVICE has it, exactly as MixerShadow holds it

    // The logical layer above those cells (uf_mixer_model.hpp). Published separately because it is
    // not recoverable from the rendered matrix: a muted crosspoint renders to 0, so `cells` alone
    // cannot tell a UI whether to draw a mute button lit or a fader at -inf, nor what gain to
    // restore when it is un-muted.
    //
    // Flat [src*kMixerCh + dest], inputs then playback, then one per output. uint8 each — this is
    // uf::kMf* bits, and the array is 1596 bytes rather than the 6 KB a uint32 would cost.
    static constexpr uint32_t kMixerCh = 28;
    uint8_t inputFlags[kMixerCh * kMixerCh];
    uint8_t playbackFlags[kMixerCh * kMixerCh];
    uint8_t outputFlags[kMixerCh];

    // Gains kept apart from the rendered cells for the same reason: the gain under a mute.
    uint32_t inputGain[kMixerCh * kMixerCh];
    uint32_t playbackGain[kMixerCh * kMixerCh];
    uint32_t outputGain[kMixerCh];

    // One bit per stereo PAIR (bit n covers channels 2n and 2n+1). Host-side only — the device has
    // no notion of pairing — but every front-end has to agree about it. Separate masks for inputs
    // and playback because the manual keeps them separate; hardware outputs are always stereo.
    uint32_t stereoIn;
    uint32_t stereoPb;

    // Level meters, republished ~30x a second (uf_meters.hpp).
    //
    // Inputs and playback are computed by the daemon from the PCM it already carries — exact, free,
    // and needing no calibration. The HARDWARE OUTPUT row cannot be: those are summed by the FF800's
    // own DSP and never come back to us, which is what the raw device region below is for.
    ::uf::MeterValue inputMeters[kMixerCh];
    ::uf::MeterValue playbackMeters[kMixerCh];
    uint32_t meterFrames;      // frames behind the last reading; 0 = not streaming

    // The device's own meter region at 0x80100000, RAW and UNCALIBRATED (spec/10 §10.3). Published
    // as the 8-byte slots come off the wire because the scaling is still an open question — the u64
    // accumulator does not close against the 2^-55 scaling without an unknown block-average N, and no
    // peak has been located on the wire at all. Publishing the raw values is what lets that be
    // settled against a known-level tone without another daemon build.
    uint64_t deviceMeters[32];
    uint32_t deviceMeterSeq;   // bumped per successful poll; 0 = never polled

    // The Fireface-Settings half: phantom, input/output levels, input sources, SPDIF, clock mode.
    // Published for the same reason as the matrix — the FF800's settings register is WRITE-ONLY, so
    // the daemon's shadow is the only place the current values exist. Without this a UI could set a
    // setting but never show one, and a change made by uf-set would be invisible to every other
    // client.
    ::uf::SettingsShadow settings;

    // True when this segment is one we can read. Both halves matter — see kVersion.
    bool compatible() const noexcept { return magic == kMagic && version == kVersion; }

    void init() noexcept {
        magic = kMagic;
        version = kVersion;
        seq.store(0, std::memory_order_relaxed);
        deviceRate = clockSource = clockLocked = dbq = 0;
        for (uint32_t i = 0; i < kCells; ++i) cells[i] = 0;
        for (uint32_t i = 0; i < kMixerCh * kMixerCh; ++i) {
            inputFlags[i] = playbackFlags[i] = 0;
            inputGain[i] = playbackGain[i] = 0;
        }
        for (uint32_t i = 0; i < kMixerCh; ++i) {
            outputFlags[i] = 0; outputGain[i] = 0;
            inputMeters[i] = ::uf::MeterValue{};
            playbackMeters[i] = ::uf::MeterValue{};
        }
        stereoIn = stereoPb = 0;
        meterFrames = 0;
        for (uint32_t i = 0; i < 32; ++i) deviceMeters[i] = 0;
        deviceMeterSeq = 0;
        settings = ::uf::SettingsShadow{};
    }
};

// Writer side. Called from the pump, so it must not allocate, block, or syscall — it does none of
// those. The fences are what make the seqlock work on arm64: without them the payload stores can be
// reordered past the seq stores and a reader validates values it never actually read.
template <class FillFn>
inline void publish(Shared* s, FillFn&& fill) noexcept {
    const uint64_t n = s->seq.load(std::memory_order_relaxed) + 1;
    s->seq.store(n, std::memory_order_relaxed);          // odd: write in progress
    std::atomic_thread_fence(std::memory_order_release);
    fill(s);
    s->seq.store(n + 1, std::memory_order_release);      // even: stable
}

// Reader side. Returns false on a torn read; the caller retries. Copies rather than handing back a
// pointer, because anything read without the seq check around it is meaningless.
inline bool read(const Shared* s, Shared* out) noexcept {
    const uint64_t a = s->seq.load(std::memory_order_acquire);
    if (a & 1) return false;
    __builtin_memcpy(out, s, sizeof(Shared));
    std::atomic_thread_fence(std::memory_order_acquire);
    return s->seq.load(std::memory_order_relaxed) == a;
}

}  // namespace uf::state
