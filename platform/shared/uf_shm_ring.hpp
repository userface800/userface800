// uf_shm_ring.hpp — lock-free single-producer/single-consumer ring in POSIX shared memory, the
// audio path between the daemon (owns the FF800 via the dext) and the AudioServerPlugIn (in
// coreaudiod). One instance per direction: capture (daemon writes, plugin reads) and playback
// (plugin writes, daemon reads).
//
// The plugin's DoIOOperation runs on the CoreAudio realtime thread: it may only push/pop slots here
// — no locks, no allocation, no syscalls. The whole region is a POD so it lives directly in the
// shared mapping; head/tail are the only synchronisation.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace uf::shm {

inline constexpr uint32_t kCacheLine   = 64;
inline constexpr uint32_t kMaxChannels = 28;               // FF800 single speed
inline constexpr uint32_t kFramesPerSlot = 512;            // one CoreAudio IO period
inline constexpr uint32_t kBytesPerSample = 4;             // int32 in the ring (top-justified 24-bit)
inline constexpr uint32_t kSlotFrameBytes = kMaxChannels * kBytesPerSample;
inline constexpr uint32_t kSlotBytes    = kFramesPerSlot * kSlotFrameBytes;
inline constexpr uint32_t kSlots        = 8;               // power of two

// Frames between clock anchors — the device's ZeroTimeStampPeriod, and the interval at which the
// daemon publishes {sampleTime, hostTime}. BOTH sides must use this: the plugin quantises sample
// times to it, so if the daemon publishes on a different boundary the host time in each pair belongs
// to a different instant than the sample count and the HAL's rate estimate wanders.
//
// It also sets the largest IO buffer a DAW can choose. Measured on macOS: the HAL caps the buffer at
// **ZeroTimeStampPeriod * 3/8** (512 -> 192, 2048 -> 768, and Loopback Audio's 3072 implies 8192),
// limited to 4096. At 512 the ceiling was 192 frames, which is 1 ms at 192 kHz and audibly too tight;
// 2048 puts 512-frame buffers within reach at every rate.
//
// Deliberately independent of kFramesPerSlot, which it used to share. They answer different
// questions — one is how much audio a ring slot holds, the other how often the clock is stamped —
// and tying them together is why the buffer ceiling was stuck.
inline constexpr uint32_t kAnchorFrames = 2048;
static_assert((kSlots & (kSlots - 1)) == 0, "kSlots must be a power of two");

inline constexpr uint32_t kMagic   = 0x55465233;           // "UFR3" — bumped when the HAL clock telemetry was added
inline constexpr char     kCaptureName[]  = "/userface800.capture";
inline constexpr char     kPlaybackName[] = "/userface800.playback";

struct Slot {
    uint32_t frameCount;                                   // valid frames in this slot
    uint32_t channelCount;
    int32_t  audio[kFramesPerSlot * kMaxChannels];         // frame-major, top-justified 24-bit
};

// A slot ring. head = next slot the producer will write; tail = next the consumer will read.
// Empty when head == tail; full when head - tail == kSlots.
struct Ring {
    uint32_t magic;
    uint32_t slots;
    // Which daemon run created this ring. The plugin uses it to notice that the daemon restarted and
    // that its mapping is now an orphan.
    //
    // It has to live in the ring because macOS gives POSIX shared memory no usable identity from the
    // outside: fstat() on an shm fd returns st_dev=0 and st_ino=0 for every object, always. An
    // identity check built on those — which is the obvious thing, and works on Linux — compares equal
    // to itself forever and never detects anything. That silently disables re-attach entirely: the
    // plugin keeps writing into a ring nobody reads, which does not present as "no sound" but as
    // damaged audio and a crawling transport.
    uint64_t instance;
    uint32_t framesPerSlot;
    uint32_t channelCount;
    alignas(kCacheLine) std::atomic<uint64_t> head;        // producer cursor
    alignas(kCacheLine) std::atomic<uint64_t> tail;        // consumer cursor
    alignas(kCacheLine) std::atomic<uint32_t> overruns;    // producer dropped a slot (consumer behind)
    std::atomic<uint32_t> underruns;                       // consumer found the ring empty

    // Device-paced sample clock: the daemon publishes {sampleTime, hostTime} as capture DMA advances
    // (the isoch stream is the timebase). The plugin's GetZeroTimeStamp reads the latest pair so
    // CoreAudio locks its timeline to the FF800 instead of guessing. seed bumps on each update; read
    // it before and after the pair and retry on a mismatch (torn read). This is the SPSC audio-clock
    // equivalent of the factory driver's timestamp-on-ring-wrap.
    alignas(kCacheLine) std::atomic<uint64_t> tsSeed;      // increments on every timestamp publish
    std::atomic<double>   tsSampleTime;                    // frame count at tsHostTime
    std::atomic<uint64_t> tsHostTime;                      // mach_absolute_time at that frame

    // Rate agreement. CoreAudio picks the sample rate; the daemon owns the device that has to be at
    // it. Without this channel the two silently disagree — the HAL sets its nominal rate to 44100,
    // the daemon keeps streaming 48000, and since our sample times count REAL device frames the HAL
    // reads the device as running 8.8% fast. That is the "44.1k sounds robotic" symptom.
    //
    // deviceRate is the truth (what is actually on the wire) and only the daemon writes it.
    // requestedRate/requestSeq are the plugin asking for a change; the daemon acts on a new seq and
    // republishes deviceRate when the session is back up. The plugin must not present device-paced
    // timestamps while the two disagree — mid-switch, the frames it would be reporting are in the
    // wrong unit.
    alignas(kCacheLine) std::atomic<uint32_t> deviceRate;    // daemon -> plugin: rate on the wire
    std::atomic<uint32_t> requestedRate;                     // plugin -> daemon: rate CoreAudio wants
    std::atomic<uint64_t> requestSeq;                        // plugin -> daemon: bumps per request

    // Control plane. Same shape as the rate channel: the daemon owns the device and publishes what
    // is true, the plugin publishes what CoreAudio asked for, and a sequence number distinguishes a
    // new request from a repeat. clockSource is the source the DEVICE reports it is locked to, not
    // the one we asked for — an external clock can vanish and leave it back on its crystal, and the
    // right thing to show a user is what is actually driving the converters. Values are
    // uf::ctl::ClockSource.
    alignas(kCacheLine) std::atomic<uint32_t> clockSource;   // daemon -> plugin: ACTIVE source
    std::atomic<uint32_t> clockLocked;                       // daemon -> plugin: 1 = locked+synced
    std::atomic<uint32_t> requestedClockSource;              // plugin -> daemon
    std::atomic<uint64_t> controlSeq;                        // plugin -> daemon: bumps per request

    // HAL clock telemetry, plugin -> daemon, diagnostics only. coreaudiod writes nothing to the
    // unified log (verified: zero lines from it at any level), so os_log from the
    // plugin is unreadable. The daemon's stdout is the only channel we can actually see, and it
    // already shares this mapping — so the plugin publishes its clock state here and the daemon
    // prints it, letting an audible glitch be lined up against a clock re-lock.
    alignas(kCacheLine) std::atomic<uint32_t> halLocked;    // 1 = following the device clock
    std::atomic<uint32_t> halUnlocks;                       // times it has LOST lock
    std::atomic<uint64_t> halSeed;                          // discontinuity seed handed to CoreAudio

    alignas(kCacheLine) Slot slot[kSlots];

    void init(uint64_t instanceId) noexcept {
        magic = kMagic; slots = kSlots; framesPerSlot = kFramesPerSlot; channelCount = kMaxChannels;
        instance = instanceId;
        head.store(0, std::memory_order_relaxed);
        tail.store(0, std::memory_order_relaxed);
        overruns.store(0, std::memory_order_relaxed);
        underruns.store(0, std::memory_order_relaxed);
        tsSeed.store(0, std::memory_order_relaxed);
        tsSampleTime.store(0, std::memory_order_relaxed);
        tsHostTime.store(0, std::memory_order_relaxed);
        deviceRate.store(0, std::memory_order_relaxed);     // 0 = the daemon has not said yet
        requestedRate.store(0, std::memory_order_relaxed);
        requestSeq.store(0, std::memory_order_relaxed);
        clockSource.store(0, std::memory_order_relaxed);    // Internal
        clockLocked.store(0, std::memory_order_relaxed);
        requestedClockSource.store(0, std::memory_order_relaxed);
        controlSeq.store(0, std::memory_order_relaxed);
        halLocked.store(0, std::memory_order_relaxed);
        halUnlocks.store(0, std::memory_order_relaxed);
        halSeed.store(0, std::memory_order_relaxed);
    }

    // Producer (daemon) publishes the device-paced clock anchor. seed brackets the write.
    void publishTimestamp(double sampleTime, uint64_t hostTime) noexcept {
        const uint64_t s = tsSeed.load(std::memory_order_relaxed) + 1;
        tsSeed.store(s, std::memory_order_relaxed);        // odd => write in progress
        std::atomic_thread_fence(std::memory_order_release);   // odd marker lands before the payload
        tsSampleTime.store(sampleTime, std::memory_order_relaxed);
        tsHostTime.store(hostTime, std::memory_order_relaxed);
        tsSeed.store(s + 1, std::memory_order_release);    // even => stable, payload published
    }

    // Consumer (plugin, RT thread) reads a coherent {sampleTime, hostTime, seed}. Returns false on a
    // torn read (caller retries) or before the first publish.
    //
    // The trailing fence is load-bearing and must sit BEFORE the second seed load: an acquire *load*
    // only orders what follows it, so without the fence both the compiler and arm64 may sink the two
    // payload loads past it and the seed comparison then validates values it never actually read.
    bool readTimestamp(double* sampleTime, uint64_t* hostTime, uint64_t* seed) const noexcept {
        const uint64_t s0 = tsSeed.load(std::memory_order_acquire);
        if (s0 == 0 || (s0 & 1)) return false;
        const double   st = tsSampleTime.load(std::memory_order_relaxed);
        const uint64_t ht = tsHostTime.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (tsSeed.load(std::memory_order_relaxed) != s0) return false;
        *sampleTime = st; *hostTime = ht; *seed = s0;
        return true;
    }

    // Producer: claim the next writable slot, or nullptr if the ring is full (consumer too slow).
    Slot* acquireWrite() noexcept {
        const uint64_t h = head.load(std::memory_order_relaxed);
        const uint64_t t = tail.load(std::memory_order_acquire);
        if (h - t >= slots) { overruns.fetch_add(1, std::memory_order_relaxed); return nullptr; }
        return &slot[h & (kSlots - 1)];
    }
    void commitWrite() noexcept { head.fetch_add(1, std::memory_order_release); }

    // Consumer: the next readable slot, or nullptr if the ring is empty (producer hasn't caught up).
    const Slot* acquireRead() noexcept {
        const uint64_t t = tail.load(std::memory_order_relaxed);
        const uint64_t h = head.load(std::memory_order_acquire);
        if (t == h) { underruns.fetch_add(1, std::memory_order_relaxed); return nullptr; }
        return &slot[t & (kSlots - 1)];
    }
    void commitRead() noexcept { tail.fetch_add(1, std::memory_order_release); }

    uint64_t depth() const noexcept {
        return head.load(std::memory_order_acquire) - tail.load(std::memory_order_acquire);
    }
};

// The Ring is mapped by two processes, so every atomic in it must be genuinely lock-free: a libc++
// lock-table fallback would take a per-process lock and synchronise nothing across the mapping.
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<double>::is_always_lock_free);

}  // namespace uf::shm
