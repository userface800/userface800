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
static_assert((kSlots & (kSlots - 1)) == 0, "kSlots must be a power of two");

inline constexpr uint32_t kMagic   = 0x55465231;           // "UFR1"
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

    alignas(kCacheLine) Slot slot[kSlots];

    void init() noexcept {
        magic = kMagic; slots = kSlots; framesPerSlot = kFramesPerSlot; channelCount = kMaxChannels;
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
