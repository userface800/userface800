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
    double   tsSampleTime;                                 // frame count at tsHostTime
    uint64_t tsHostTime;                                   // mach_absolute_time at that frame

    alignas(kCacheLine) Slot slot[kSlots];

    void init() noexcept {
        magic = kMagic; slots = kSlots; framesPerSlot = kFramesPerSlot; channelCount = kMaxChannels;
        head.store(0, std::memory_order_relaxed);
        tail.store(0, std::memory_order_relaxed);
        overruns.store(0, std::memory_order_relaxed);
        underruns.store(0, std::memory_order_relaxed);
        tsSeed.store(0, std::memory_order_relaxed);
        tsSampleTime = 0; tsHostTime = 0;
    }

    // Producer (daemon) publishes the device-paced clock anchor. seed brackets the write.
    void publishTimestamp(double sampleTime, uint64_t hostTime) noexcept {
        const uint64_t s = tsSeed.load(std::memory_order_relaxed) + 1;
        tsSeed.store(s, std::memory_order_release);        // odd => write in progress
        std::atomic_thread_fence(std::memory_order_release);
        tsSampleTime = sampleTime; tsHostTime = hostTime;
        tsSeed.store(s + 1, std::memory_order_release);    // even => stable
    }

    // Consumer (plugin, RT thread) reads a coherent {sampleTime, hostTime, seed}. Returns false on a
    // torn read (caller retries) or before the first publish.
    bool readTimestamp(double* sampleTime, uint64_t* hostTime, uint64_t* seed) const noexcept {
        const uint64_t s0 = tsSeed.load(std::memory_order_acquire);
        if (s0 == 0 || (s0 & 1)) return false;
        std::atomic_thread_fence(std::memory_order_acquire);
        *sampleTime = tsSampleTime; *hostTime = tsHostTime;
        const uint64_t s1 = tsSeed.load(std::memory_order_acquire);
        if (s1 != s0) return false;
        *seed = s0;
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

}  // namespace uf::shm
