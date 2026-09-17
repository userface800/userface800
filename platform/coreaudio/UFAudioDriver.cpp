// UFAudioDriver — the UserFace800 AudioServerPlugIn (runs inside coreaudiod).
//
// Layer 1: declare the FF800 to CoreAudio as a 28-in / 28-out, 48 kHz, 24-bit device so it appears in
// Audio MIDI Setup. Built on libASPL. The IO handler produces
// silence for now — real PCM arrives via a shared-memory ring from the daemon (layer 3), which owns
// the FF800 through our OHCI dext. This plugin never touches FireWire directly (the plugin
// only moves PCM; the daemon owns the device).
#include <CoreAudio/AudioHardware.h>   // clock-source selectors (AudioServerPlugIn.h only pulls Base)
#include <aspl/Driver.hpp>
#include <aspl/Device.hpp>
#include <aspl/Plugin.hpp>
#include <aspl/Tracer.hpp>
#include <aspl/IORequestHandler.hpp>
#include <os/log.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <mach/mach_time.h>
#include <iterator>
#include "uf/protocol/channels.hpp"
#include "uf/protocol/rate.hpp"
#include "../shared/uf_control.hpp"
#include "../shared/uf_shm_ring.hpp"
#include "../shared/uf_zero_timestamp.hpp"

// The FF800 at single speed: 28 PCM channels each way (analog 10 + SPDIF 2 + ADAT 16), 48 kHz.
static constexpr double   kSampleRate  = 48000.0;
static constexpr uint32_t kChannels    = 28;

static AudioStreamBasicDescription ff800_format(uint32_t channels) {
    AudioStreamBasicDescription f = {};
    f.mSampleRate       = kSampleRate;
    f.mFormatID         = kAudioFormatLinearPCM;
    f.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    f.mBitsPerChannel   = 32;              // top-justified 24-bit, i.e. the ring's int32 slots
    f.mChannelsPerFrame = channels;
    f.mBytesPerFrame    = channels * 4;
    f.mFramesPerPacket  = 1;
    f.mBytesPerPacket   = channels * 4;
    return f;
}

// Keeps the shared-memory rings attached across daemon restarts.
//
// The daemon shm_unlinks and re-creates both rings every time it starts (uf-daemon make_ring), so a
// mapping taken once at plugin load silently becomes an orphan — a private copy of a ring nobody
// writes to any more — the moment the daemon is restarted. That is the mechanism behind the standing
// "every daemon restart needs sudo killall coreaudiod" rule: the plugin goes on happily reading and
// writing a ring the device end has abandoned, so the DAW plays into nothing.
//
// A background thread re-opens each ring by name and compares the object's identity (st_dev/st_ino)
// with what is currently mapped; a different object means the daemon restarted, so we map the new one
// and publish it atomically. Nothing here runs on the realtime thread — that side only ever does a
// relaxed atomic load. Retired mappings are unmapped after a grace period rather than immediately,
// because an IO callback that started before the swap may still be inside the old ring; a callback
// runs for microseconds, so seconds of grace is not a subtle race.
class ShmMapper {
public:
    ShmMapper() {
        refresh(capture_);
        refresh(playback_);
        thread_ = std::thread([this] { poll(); });
    }
    ~ShmMapper() {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
    }
    uf::shm::Ring* capture()  const { return capture_.ring.load(std::memory_order_acquire); }
    uf::shm::Ring* playback() const { return playback_.ring.load(std::memory_order_acquire); }

    // Called on the background thread each time a NEW capture ring is attached — i.e. whenever the
    // daemon restarts. The daemon always comes up at its own default rate with no idea what
    // CoreAudio settled on, and nothing else ever tells it: the plugin only sends a rate request
    // when the rate CHANGES. So a daemon restarted while the host sits at 96 kHz streams 48 kHz
    // indefinitely, and playback runs at half speed. This is the hook that closes that.
    void setOnCaptureAttach(std::function<void()> fn) { onAttach_ = std::move(fn); }

private:
    struct Mapping {
        const char* name;
        std::atomic<uf::shm::Ring*> ring{nullptr};
    };
    struct Retired {
        void* addr;
        std::chrono::steady_clock::time_point when;
    };

    void poll() {
        while (!stop_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            refresh(capture_);
            refresh(playback_);
            reap();
        }
    }

    // Map `m`'s ring if it is absent or has been replaced by a newer one.
    //
    // Identity comes from a value the daemon writes INTO the ring, not from the shm object: on macOS
    // fstat() reports st_dev=0 and st_ino=0 for every POSIX shm object, so the obvious identity check
    // compares equal to itself forever and never fires. That is not a subtle failure — it made this
    // whole class a no-op, and the plugin went on writing into rings nobody was reading.
    //
    // So each poll maps a fresh probe and reads the run id out of it. Cheap at 2 Hz, and the only
    // thing that actually distinguishes one daemon run from the next.
    void refresh(Mapping& m) {
        int fd = shm_open(m.name, O_RDWR, 0);
        if (fd < 0) return;                       // daemon not running; keep whatever we have
        void* p = mmap(nullptr, sizeof(uf::shm::Ring), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (p == MAP_FAILED) return;
        auto* probe = static_cast<uf::shm::Ring*>(p);
        if (probe->magic != uf::shm::kMagic) {    // still initialising, or an incompatible layout
            munmap(p, sizeof(uf::shm::Ring));
            return;
        }
        if (uf::shm::Ring* cur = m.ring.load(std::memory_order_relaxed)) {
            if (cur->instance == probe->instance) {   // the ring we already have
                munmap(p, sizeof(uf::shm::Ring));
                return;
            }
        }
        void* old = m.ring.exchange(probe, std::memory_order_acq_rel);
        if (old) {
            std::lock_guard<std::mutex> lock(retiredMutex_);
            retired_.push_back({old, std::chrono::steady_clock::now()});
        }
        os_log(OS_LOG_DEFAULT, "UFAudioDriver: attached ring %s", m.name);
        if (&m == &capture_ && onAttach_) onAttach_();
    }

    void reap() {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(retiredMutex_);
        for (auto it = retired_.begin(); it != retired_.end();) {
            if (now - it->when > std::chrono::seconds(5)) {
                munmap(it->addr, sizeof(uf::shm::Ring));
                it = retired_.erase(it);
            } else ++it;
        }
    }

    Mapping capture_{uf::shm::kCaptureName};
    Mapping playback_{uf::shm::kPlaybackName};
    std::vector<Retired> retired_;
    std::mutex retiredMutex_;
    std::function<void()> onAttach_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

// Bridges CoreAudio's realtime IO to the daemon's shared-memory rings. Capture (device->host) is
// popped from kCaptureName; playback (host->device) is pushed to kPlaybackName. The rings hold
// interleaved int32 (top-justified 24-bit), which is exactly our declared stream format, so the RT
// path is a plain memcpy with no conversion. If the daemon is not running the rings are absent and
// we produce/consume silence.
class UFIOHandler : public aspl::IORequestHandler {
public:
    explicit UFIOHandler(ShmMapper* shm) : shm_(shm) {
        inStage_.resize((size_t)kMaxCycleFrames * uf::shm::kMaxChannels);
        outStage_.resize((size_t)kMaxCycleFrames * uf::shm::kMaxChannels);
    }

    // How many output streams make up one cycle. Set once the device's streams exist; the flush
    // below uses it to publish a cycle as soon as the last group has contributed instead of waiting
    // for the next cycle to arrive, which would cost a full IO buffer of output latency.
    void setOutputStreamCount(uint32_t n) { outStreamCount_ = n; }

    // ── Why this is staged rather than written straight through ──────────────────────────────
    // We publish one stream per channel group (Analog / Phones / SPDIF / ADAT), and libASPL calls
    // these handlers ONCE PER STREAM PER CYCLE — every call carrying the same cycle sample time and
    // the same frame range, but only its own channels (Device::DoIOOperationImpl passes
    // ioCycleInfo->mOutputTime.mSampleTime to each). So no single call has a complete frame, and
    // advancing the ring per call would hand each group a different slice of time.
    //
    // Instead a cycle is assembled in a 28-wide staging buffer and moved to the ring in one piece:
    // capture fills the stage from the ring on the first stream of a cycle and every stream reads
    // its columns out of it; playback zeroes the stage, every stream writes its columns in, and the
    // whole cycle goes to the ring once the last one has. Zeroing up front is what makes a group
    // that is idle this cycle come out silent instead of replaying the previous lap.
    void OnReadClientInput(const std::shared_ptr<aspl::Client>&,
                           const std::shared_ptr<aspl::Stream>& stream,
                           Float64, Float64 sampleTime, void* bytes, UInt32 bytesCount) override {
        std::memset(bytes, 0, bytesCount);
        uf::shm::Ring* capture = shm_->capture();
        if (!capture) return;
        // A slot held from a previous callback points into whichever ring we were reading then; if
        // the daemon restarted since, that ring is retired and the pointer means nothing.
        if (capture != lastCapture_) {
            lastCapture_ = capture; rdSlot_ = nullptr; rdFrame_ = 0; inCycle_ = -1;
        }

        const UInt32 chan = stream->GetPhysicalFormat().mChannelsPerFrame;
        const UInt32 base = firstChannel(stream);
        if (chan == 0 || base >= uf::shm::kMaxChannels) return;
        UInt32 frames = bytesCount / (chan * (UInt32)sizeof(int32_t));
        if (frames > kMaxCycleFrames) frames = kMaxCycleFrames;

        if (sampleTime != inCycle_) {          // first stream of this cycle: pull it from the ring
            inCycle_ = sampleTime;
            fillStageFromRing(capture, frames);
        }

        const UInt32 avail = uf::shm::kMaxChannels - base;
        const UInt32 n = chan < avail ? chan : avail;
        int32_t* out = static_cast<int32_t*>(bytes);
        for (UInt32 f = 0; f < frames; ++f)
            std::memcpy(out + (size_t)f * chan,
                        &inStage_[(size_t)f * uf::shm::kMaxChannels + base],
                        n * sizeof(int32_t));
    }

    void OnWriteMixedOutput(const std::shared_ptr<aspl::Stream>& stream,
                            Float64, Float64 sampleTime, const void* bytes,
                            UInt32 bytesCount) override {
        uf::shm::Ring* playback = shm_->playback();
        if (!playback) return;
        if (playback != lastPlayback_) {
            lastPlayback_ = playback; wrSlot_ = nullptr; wrFrame_ = 0;
            outCycle_ = -1; outFrames_ = 0;
        }

        const UInt32 chan = stream->GetPhysicalFormat().mChannelsPerFrame;
        const UInt32 base = firstChannel(stream);
        if (chan == 0 || base >= uf::shm::kMaxChannels) return;
        UInt32 frames = bytesCount / (chan * (UInt32)sizeof(int32_t));
        if (frames > kMaxCycleFrames) frames = kMaxCycleFrames;

        if (sampleTime != outCycle_) {
            // A cycle we never finished — a stream went idle mid-cycle, so its columns stayed
            // silent. Publish what we have rather than losing it.
            flushStageToRing(playback);
            outCycle_ = sampleTime;
            outFrames_ = frames;
            outStreamsSeen_ = 0;
            std::memset(outStage_.data(), 0,
                        (size_t)frames * uf::shm::kMaxChannels * sizeof(int32_t));
        }

        const UInt32 avail = uf::shm::kMaxChannels - base;
        const UInt32 n = chan < avail ? chan : avail;
        const int32_t* in = static_cast<const int32_t*>(bytes);
        for (UInt32 f = 0; f < frames; ++f)
            std::memcpy(&outStage_[(size_t)f * uf::shm::kMaxChannels + base],
                        in + (size_t)f * chan, n * sizeof(int32_t));

        if (++outStreamsSeen_ >= outStreamCount_) {   // last group in: the cycle is complete
            flushStageToRing(playback);
            outCycle_ = -1;
        }
    }

private:
    // Biggest IO buffer we will stage. CoreAudio buffer sizes are well under this; anything larger
    // is clamped, which costs the tail of that cycle rather than a buffer overrun.
    static constexpr UInt32 kMaxCycleFrames = 4096;

    // The 0-based channel offset of a stream within the ring's 28-wide frame. With one stream per
    // channel group this is what keeps each group in its own columns.
    static UInt32 firstChannel(const std::shared_ptr<aspl::Stream>& s) {
        const UInt32 start = s->GetStartingChannel();     // 1-based
        return start ? start - 1 : 0;
    }

    // Pull one cycle of full-width frames out of the capture ring. Short reads leave the remainder
    // zeroed, so an underrun is silence rather than stale audio.
    void fillStageFromRing(uf::shm::Ring* capture, UInt32 frames) {
        UInt32 f = 0;
        for (; f < frames; ++f) {
            if (!rdSlot_ || rdFrame_ >= rdSlot_->frameCount) {
                if (rdSlot_) { capture->commitRead(); rdSlot_ = nullptr; }
                rdSlot_ = capture->acquireRead();
                rdFrame_ = 0;
                if (!rdSlot_) break;                      // underrun
            }
            std::memcpy(&inStage_[(size_t)f * uf::shm::kMaxChannels],
                        rdSlot_->audio + (size_t)rdFrame_ * uf::shm::kMaxChannels,
                        uf::shm::kMaxChannels * sizeof(int32_t));
            ++rdFrame_;
        }
        if (f < frames)
            std::memset(&inStage_[(size_t)f * uf::shm::kMaxChannels], 0,
                        (size_t)(frames - f) * uf::shm::kMaxChannels * sizeof(int32_t));
    }

    // Move the staged cycle into the playback ring, committing each slot as it fills.
    void flushStageToRing(uf::shm::Ring* playback) {
        for (UInt32 f = 0; f < outFrames_; ++f) {
            if (!wrSlot_) {
                wrSlot_ = playback->acquireWrite(); wrFrame_ = 0;
                if (!wrSlot_) break;                      // ring full: drop the rest of the cycle
            }
            std::memcpy(wrSlot_->audio + (size_t)wrFrame_ * uf::shm::kMaxChannels,
                        &outStage_[(size_t)f * uf::shm::kMaxChannels],
                        uf::shm::kMaxChannels * sizeof(int32_t));
            if (++wrFrame_ >= uf::shm::kFramesPerSlot) {
                wrSlot_->frameCount = wrFrame_;
                wrSlot_->channelCount = uf::shm::kMaxChannels;
                playback->commitWrite();
                wrSlot_ = nullptr;
            }
        }
        outFrames_ = 0;
    }

    ShmMapper* shm_;
    const uf::shm::Ring* lastCapture_ = nullptr;    // which ring the held slots point into
    const uf::shm::Ring* lastPlayback_ = nullptr;

    std::vector<int32_t> inStage_, outStage_;       // one cycle, full ring width

    const uf::shm::Slot* rdSlot_ = nullptr;
    uint32_t rdFrame_ = 0;
    Float64 inCycle_ = -1;

    uf::shm::Slot* wrSlot_ = nullptr;
    uint32_t wrFrame_ = 0;
    Float64 outCycle_ = -1;
    UInt32 outFrames_ = 0;
    uint32_t outStreamsSeen_ = 0;
    uint32_t outStreamCount_ = 1;
};

// The seven native sample rates.
static const double kRates[] = {32000, 44100, 48000, 88200, 96000, 176400, 192000};

// The safety cushion, in frames at `rate`.
//
// It has to be specified in TIME, not in frames. What it absorbs is the daemon's scheduling jitter
// and the IPC between us — both roughly constant in milliseconds — so a fixed frame count would
// shrink the cushion exactly as the rate rises and the deadlines get tighter. One ring slot at
// 48 kHz is 10.7 ms; the same 512 frames at 192 kHz is 2.7 ms, four times less headroom for the same
// underlying jitter. Hold the milliseconds and let the frame count follow the rate.
static constexpr double kSafetyMsDefault =
    (double)uf::shm::kFramesPerSlot * 1000.0 / 48000.0;   // 10.67 ms — one slot at the base rate

static UInt32 safetyFrames(double rate) {
    const double ms = getenv("UF_SAFETY_OFFSET_MS") ? atof(getenv("UF_SAFETY_OFFSET_MS"))
                                                    : kSafetyMsDefault;
    return (UInt32)(ms * rate / 1000.0 + 0.5);
}

// A stream that advertises its format at every native rate. Apple's HAL only exposes a device rate
// if some stream has a matching format, so the device rate list alone (UFDevice) isn't enough —
// the streams must enumerate the rates too. Overriding the getters works at construction (the Async
// setters need a live HAL connection).
//
// The channel count here is THIS STREAM'S GROUP, not the device's. That distinction cost a hardware
// session: while there was one wide stream, advertising the whole 28 was right, but once each stream
// became a channel group the HAL validated stream 1's 8-channel format against an available list
// that claimed 28, took the 28, and laid the streams out end to end — 28+2+2+16 = 48 channels, with
// ADAT pushed to 33-48 and everything between overlapping and silent.
class UFStream : public aspl::Stream {
public:
    UFStream(std::shared_ptr<const aspl::Context> ctx, std::shared_ptr<aspl::Device> dev,
             const aspl::StreamParameters& p, uint32_t channels)
        : aspl::Stream(std::move(ctx), std::move(dev), p), channels_(channels) {}

    // The SAME channel count at every rate, which is what the factory driver does — TotalMix shows
    // 20 channels at 96 kHz while CoreAudio still sees 28.
    //
    // The device really does carry fewer channels as the rate rises (ADAT halves at 2x, vanishes at
    // 4x), so shrinking the streams to match is the more truthful description. It is also worse to
    // use: a DAW stores its output routing by channel index, so channels disappearing under a rate
    // change silently sends tracks nowhere and the user has to rebuild the routing — which is
    // exactly what happened here when the count went 48 -> 28. Holding the layout fixed means a
    // session survives a rate change, and the channels the hardware cannot carry are simply silent:
    // the daemon only transmits the first `dbq` ring columns, so the surplus goes nowhere on its own.
    //
    // It also removes stream add/remove from a live property-set, which is the prime suspect for the
    // one-off Ableton crash on the first 48 -> 96 switch.
    std::vector<AudioStreamRangedDescription> formats() const {
        std::vector<AudioStreamRangedDescription> v;
        for (double r : kRates) {
            AudioStreamRangedDescription d = {};
            d.mFormat = ff800_format(channels_);
            d.mFormat.mSampleRate = r;
            d.mSampleRateRange = {r, r};
            v.push_back(d);
        }
        return v;
    }
    std::vector<AudioStreamRangedDescription> GetAvailablePhysicalFormats() const override { return formats(); }
    std::vector<AudioStreamRangedDescription> GetAvailableVirtualFormats() const override { return formats(); }

private:
    uint32_t channels_;
};

// Per-channel names — "Analog 1", "Mic 9", "SPDIF L", "ADAT 3" instead of "Input 1..28".
//
// The factory driver publishes a fixed per-model table of channel names, which the HAL then
// republishes as kAudioObjectPropertyElementName. We are the plugin, so we answer that property
// directly; the strings come from uf::channel_name() over the same channel map the rest of the
// driver uses (spec/08 §8.2), which means they stay right at 2x/4x where the ADAT channels drop out
// — a flat 28-entry table has to be re-indexed by hand to manage that.
//
// libASPL has no hook for this, so it goes in as a property override. Element is the 1-based channel
// number; element 0 (master) falls through to the base, which names the object itself.
// Also answers the element CATEGORY name — the group label ("Analog", "Phones", "SPDIF", "ADAT")
// that Audio MIDI Setup puts above a block of channels. The factory driver gets those four groups by
// publishing one stream per group; we publish one wide stream and label the channels instead, which
// costs nothing on the realtime path. Whether AMS actually groups on the category or only on stream
// boundaries is the open question — if it does, the four-stream layout is moot.
class UFNamedChannels {
public:
    static bool isChannelName(const AudioObjectPropertyAddress* a) {
        return a && (a->mSelector == kAudioObjectPropertyElementName ||
                     a->mSelector == kAudioObjectPropertyElementCategoryName) &&
               a->mElement != kAudioObjectPropertyElementMain;
    }
    // Direction comes from the address scope so one implementation serves both streams.
    static CFStringRef copyName(const AudioObjectPropertyAddress* a) {
        const auto dir = (a->mScope == kAudioObjectPropertyScopeInput)
                       ? uf::Direction::Capture : uf::Direction::Playback;
        // Name against the widest (1x) map: the HAL may ask about channels the current rate does not
        // carry, and a stable name is friendlier than an error.
        const auto map = uf::channel_map(dir, uf::Speed::X1);
        const UInt32 idx = a->mElement;                       // 1-based
        if (idx == 0 || idx > map.size()) return nullptr;
        const std::string n = (a->mSelector == kAudioObjectPropertyElementCategoryName)
                            ? uf::channel_group_name(map[idx - 1])
                            : uf::channel_name(map[idx - 1]);
        return CFStringCreateWithCString(kCFAllocatorDefault, n.c_str(), kCFStringEncodingUTF8);
    }
};

// Mixes the channel-name property into any aspl object (we need it on both streams).
template <class Base>
class UFWithChannelNames : public Base {
public:
    using Base::Base;

    Boolean HasProperty(AudioObjectID objectID, pid_t clientPID,
                        const AudioObjectPropertyAddress* address) const override {
        if (UFNamedChannels::isChannelName(address)) return true;
        return Base::HasProperty(objectID, clientPID, address);
    }

    OSStatus GetPropertyDataSize(AudioObjectID objectID, pid_t clientPID,
                                 const AudioObjectPropertyAddress* address,
                                 UInt32 qualifierDataSize, const void* qualifierData,
                                 UInt32* outDataSize) const override {
        if (UFNamedChannels::isChannelName(address)) {
            *outDataSize = sizeof(CFStringRef);
            return kAudioHardwareNoError;
        }
        return Base::GetPropertyDataSize(objectID, clientPID, address, qualifierDataSize,
                                         qualifierData, outDataSize);
    }

    OSStatus GetPropertyData(AudioObjectID objectID, pid_t clientPID,
                             const AudioObjectPropertyAddress* address,
                             UInt32 qualifierDataSize, const void* qualifierData,
                             UInt32 inDataSize, UInt32* outDataSize, void* outData) const override {
        if (UFNamedChannels::isChannelName(address)) {
            if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
            CFStringRef name = UFNamedChannels::copyName(address);
            if (!name) return kAudioHardwareUnknownPropertyError;
            // Ownership passes to the HAL, which releases it — the same convention as the device's
            // own name property.
            *static_cast<CFStringRef*>(outData) = name;
            *outDataSize = sizeof(CFStringRef);
            return kAudioHardwareNoError;
        }
        return Base::GetPropertyData(objectID, clientPID, address, qualifierDataSize, qualifierData,
                                     inDataSize, outDataSize, outData);
    }
};

// Both objects a DAW might ask: most read channel names off the device with an input/output scope,
// some ask the stream.
using UFNamedStream = UFWithChannelNames<UFStream>;

// Device that locks CoreAudio's timeline to the FF800 by reading the daemon's device-paced clock
// (uf::shm::Ring::readTimestamp) instead of libASPL's default free-running one. This is the userland
// analogue of the factory driver's timestamp-on-ring-wrap. Everything that decides what to do
// with the daemon's anchor — validate it, follow it, or free-run past it — lives in
// uf::ZeroTimeStampClock, where it is unit-tested without coreaudiod in the loop.
class UFDevice : public UFWithChannelNames<aspl::Device> {
public:
    UFDevice(std::shared_ptr<aspl::Context> ctx, const aspl::DeviceParameters& p, ShmMapper* shm)
        : UFWithChannelNames<aspl::Device>(ctx, p), shm_(shm), clock_(p.ZeroTimeStampPeriod, hostTicksPerFrame(p.SampleRate), hostTicksPerSecond()) {}

    // Advertise every native rate. Overriding the getter works at construction time; the Async
    // setter does not (it needs a live HAL connection, which doesn't exist yet in CreateDriver).
    std::vector<AudioValueRange> GetAvailableSampleRates() const override {
        return {{32000, 32000}, {44100, 44100}, {48000, 48000},
                {88200, 88200}, {96000, 96000}, {176400, 176400}, {192000, 192000}};
    }

    // Called by the HAL's IO thread, one per device, so the clock needs no lock of its own. Never
    // delegates to the base implementation: mixing two timelines would itself be a discontinuity,
    // and ZeroTimeStampClock already free-runs at the nominal rate when the daemon is absent.
    OSStatus GetZeroTimeStamp(AudioObjectID objID, UInt32, Float64* outSampleTime,
                              UInt64* outHostTime, UInt64* outSeed) override {
        if (objID != GetID()) return kAudioHardwareBadObjectError;

        const double nominal = GetNominalSampleRate();
        clock_.setTicksPerFrame(hostTicksPerFrame(nominal));

        // The ring's seqlock seed only detects torn reads; it bumps on every publish and must NOT be
        // confused with CoreAudio's discontinuity seed, which the clock owns.
        bool valid = false;
        double devSample = 0; uint64_t devHost = 0, seqseed = 0;
        uint32_t devRate = 0;
        if (uf::shm::Ring* cap = shm_->capture()) {
            // Only follow the device clock while the device is at the rate CoreAudio thinks it is.
            // Our sample times count REAL frames off the wire, so if the daemon is still streaming
            // 48 k while the HAL has moved to 44.1 k, every anchor we hand over reads as the device
            // running 8.8% fast — audibly, a pitch shift. Free-running at the nominal rate through
            // the switch is wrong by a few tens of milliseconds; following it is wrong by 8.8%
            // indefinitely.
            devRate = cap->deviceRate.load(std::memory_order_acquire);
            if (devRate != 0 && (double)devRate == nominal)
                for (int retry = 0; retry < 4 && !valid; ++retry)
                    valid = cap->readTimestamp(&devSample, &devHost, &seqseed);
        }

        const auto s = clock_.next(mach_absolute_time(), valid, devSample, devHost);

        // Log only TRANSITIONS — losing or regaining the device clock, and every discontinuity seed
        // the HAL is handed. Both are rare, so this costs nothing on the IO thread in steady state,
        // and it answers the question the counters cannot: whether an audible burst of gating
        // coincides with the clock re-locking. A re-lock makes the HAL re-estimate its rate, and
        // while that estimate is wrong it delivers audio at the wrong rate — the plugin's writes and
        // the daemon's reads then slide through each other, which is what gating looks like.
        // Publish clock state to the daemon through the ring rather than os_log: coreaudiod writes
        // nothing to the unified log (verified — zero lines from it at any level),
        // so anything logged here is invisible. The daemon's stdout we can read. Plain relaxed
        // stores of three words, only on a transition; nothing here blocks the IO thread.
        const bool nowLocked = clock_.locked();
        if (nowLocked != wasLocked_ || s.seed != lastSeed_) {
            if (uf::shm::Ring* cap = shm_->capture()) {
                cap->halLocked.store(nowLocked ? 1u : 0u, std::memory_order_relaxed);
                cap->halSeed.store(s.seed, std::memory_order_relaxed);
                if (wasLocked_ && !nowLocked)
                    cap->halUnlocks.fetch_add(1, std::memory_order_relaxed);
            }
            wasLocked_ = nowLocked;
            lastSeed_  = s.seed;
        }

        *outSampleTime = s.sampleTime;
        *outHostTime   = s.hostTime;
        *outSeed       = s.seed;
        return kAudioHardwareNoError;
    }

    // CoreAudio has selected a rate; ask the daemon to put the device there. The HAL is told the
    // change succeeded immediately (it has to be), and the clock rides the gap free-running until
    // the daemon republishes a matching deviceRate — which is exactly the case GetZeroTimeStamp
    // above declines to lock in.
    OSStatus SetNominalSampleRateImpl(Float64 rate) override {
        if (uf::shm::Ring* cap = shm_->capture()) {
            cap->requestedRate.store((uint32_t)rate, std::memory_order_release);
            cap->requestSeq.fetch_add(1, std::memory_order_acq_rel);
        }
        const OSStatus st = aspl::Device::SetNominalSampleRateImpl(rate);

        // Carry the rate — and with it the channel count, which changes across speed classes — down
        // to the streams. libASPL does not propagate device rate to stream formats, and the IO
        // handler strides by the stream format, so without this the buffers stay laid out for the
        // old rate. SetPhysicalFormatImpl only stores the value, so there is no recursion back
        // into this method.
        // Rate only — the channel layout is deliberately fixed across speed classes (see UFStream),
        // so there is nothing to add, remove or resize here.
        for (auto dir : {aspl::Direction::Output, aspl::Direction::Input})
            for (UInt32 i = 0; i < GetStreamCount(dir); ++i)
                if (auto s = GetStreamByIndex(dir, i)) {
                    AudioStreamBasicDescription f = s->GetPhysicalFormat();
                    f.mSampleRate = rate;
                    s->SetPhysicalFormatAsync(f);
                }

        // The cushion is a duration, so its frame count has to move with the rate — otherwise
        // switching 48k -> 192k would quietly cut it to a quarter of the time it was sized for.
        SetSafetyOffsetAsync(safetyFrames(rate));
        SetLatencyAsync(safetyFrames(rate));
        return st;
    }

    static double hostTicksPerFrame(double rate) {
        mach_timebase_info_data_t tb; mach_timebase_info(&tb);
        return (1.0e9 / rate) * (double)tb.denom / (double)tb.numer;
    }
    static double hostTicksPerSecond() {
        mach_timebase_info_data_t tb; mach_timebase_info(&tb);
        return 1.0e9 * (double)tb.denom / (double)tb.numer;
    }

    // ── Clock source ──────────────────────────────────────────────────────────────────────────
    // The FF800 can run off its own crystal or slave to word clock / ADAT / SPDIF / TCO, and until
    // now that was reachable only by env var or the uf-set CLI. These four properties are what puts
    // it in Audio MIDI Setup's "Clock Source" menu, which is where a user expects it.
    //
    // libASPL implements none of them, so they are handled here and everything else is delegated.
    // The value we report is the source the DEVICE says it is locked to, forwarded by the daemon —
    // not the one last requested. An external clock can disappear and leave the FF800 back on its
    // crystal, and showing the request rather than the truth would hide exactly the fault a user is
    // looking at the menu to diagnose.
    static bool isClockProp(const AudioObjectPropertyAddress* a) {
        return a && (a->mSelector == kAudioDevicePropertyClockSource ||
                     a->mSelector == kAudioDevicePropertyClockSources ||
                     a->mSelector == kAudioDevicePropertyClockSourceNameForIDCFString);
    }

    // ── IO buffer size: the RANGE only, deliberately read-only ────────────────────────────────
    // Ableton caps at 128 frames, which is 2.7 ms at 48 kHz but 1.33 ms at 96 kHz — the knob you
    // reach for when playback struggles, pinned shut. libASPL publishes neither the size nor its
    // range, so the HAL uses its own defaults.
    //
    // An earlier attempt implemented BOTH the range and a settable size, and it hung the machine:
    // coreaudiod pegged a core during device publication and took loginwindow and every audio client
    // with it. The likely mechanism was the setter REJECTING values outside 32..1024 with
    // kAudioHardwareIllegalOperationError — a negotiation that can never succeed is exactly the shape
    // of a spin. (Suggestive, not proven: the plugin never got far enough to report its own default
    // of 512, so it wedged before answering anything.)
    //
    // Hence: publish the range, implement NO setter, and let the HAL keep ownership of the actual
    // size. There is then no negotiation to fail and no loop to enter — the worst case is that the
    // HAL ignores the range and nothing changes. The ceiling matches kMaxCycleFrames, so any size the
    // HAL picks inside the range is one the IO handler can already stage in a single cycle.
    static constexpr UInt32 kMinBufferFrames = 32;
    static constexpr UInt32 kMaxBufferFrames = 4096;   // == UFIOHandler::kMaxCycleFrames

    static bool isBufferRangeProp(const AudioObjectPropertyAddress* a) {
        return a && a->mSelector == kAudioDevicePropertyBufferFrameSizeRange;
    }

    Boolean HasProperty(AudioObjectID objectID, pid_t clientPID,
                        const AudioObjectPropertyAddress* address) const override {
        if (isClockProp(address) || isBufferRangeProp(address)) return true;
        return UFWithChannelNames<aspl::Device>::HasProperty(objectID, clientPID, address);
    }

    OSStatus IsPropertySettable(AudioObjectID objectID, pid_t clientPID,
                                const AudioObjectPropertyAddress* address,
                                Boolean* outIsSettable) const override {
        if (isClockProp(address)) {
            *outIsSettable = (address->mSelector == kAudioDevicePropertyClockSource);
            return kAudioHardwareNoError;
        }
        if (isBufferRangeProp(address)) {
            *outIsSettable = false;                 // read-only on purpose; see above
            return kAudioHardwareNoError;
        }
        return UFWithChannelNames<aspl::Device>::IsPropertySettable(objectID, clientPID, address,
                                                                    outIsSettable);
    }

    OSStatus GetPropertyDataSize(AudioObjectID objectID, pid_t clientPID,
                                 const AudioObjectPropertyAddress* address,
                                 UInt32 qualifierDataSize, const void* qualifierData,
                                 UInt32* outDataSize) const override {
        switch (address ? address->mSelector : 0) {
            case kAudioDevicePropertyClockSource:
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyClockSources:
                *outDataSize = (UInt32)(sizeof(UInt32) * std::size(uf::ctl::kAllClockSources));
                return kAudioHardwareNoError;
            case kAudioDevicePropertyClockSourceNameForIDCFString:
                *outDataSize = sizeof(AudioValueTranslation);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyBufferFrameSizeRange:
                *outDataSize = sizeof(AudioValueRange);
                return kAudioHardwareNoError;
            default: break;
        }
        return UFWithChannelNames<aspl::Device>::GetPropertyDataSize(
            objectID, clientPID, address, qualifierDataSize, qualifierData, outDataSize);
    }

    OSStatus GetPropertyData(AudioObjectID objectID, pid_t clientPID,
                             const AudioObjectPropertyAddress* address,
                             UInt32 qualifierDataSize, const void* qualifierData,
                             UInt32 inDataSize, UInt32* outDataSize, void* outData) const override {
        switch (address ? address->mSelector : 0) {
            case kAudioDevicePropertyClockSource: {
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                uint32_t src = 0;
                if (uf::shm::Ring* cap = shm_->capture())
                    src = cap->clockSource.load(std::memory_order_acquire);
                *static_cast<UInt32*>(outData) = src;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            }
            case kAudioDevicePropertyClockSources: {
                const UInt32 n = (UInt32)std::size(uf::ctl::kAllClockSources);
                const UInt32 fit = inDataSize / sizeof(UInt32);
                const UInt32 count = fit < n ? fit : n;
                UInt32* out = static_cast<UInt32*>(outData);
                for (UInt32 i = 0; i < count; ++i)
                    out[i] = (UInt32)uf::ctl::kAllClockSources[i];
                *outDataSize = count * sizeof(UInt32);
                return kAudioHardwareNoError;
            }
            case kAudioDevicePropertyClockSourceNameForIDCFString: {
                if (inDataSize < sizeof(AudioValueTranslation))
                    return kAudioHardwareBadPropertySizeError;
                auto* t = static_cast<AudioValueTranslation*>(outData);
                if (!t->mInputData || t->mInputDataSize < sizeof(UInt32) || !t->mOutputData)
                    return kAudioHardwareIllegalOperationError;
                const UInt32 id = *static_cast<const UInt32*>(t->mInputData);
                if (!uf::ctl::is_valid_clock_source(id)) return kAudioHardwareIllegalOperationError;
                const char* n = uf::ctl::clock_source_name((uf::ctl::ClockSource)id);
                *static_cast<CFStringRef*>(t->mOutputData) =
                    CFStringCreateWithCString(kCFAllocatorDefault, n, kCFStringEncodingUTF8);
                *outDataSize = sizeof(AudioValueTranslation);
                return kAudioHardwareNoError;
            }
            case kAudioDevicePropertyBufferFrameSizeRange: {
                if (inDataSize < sizeof(AudioValueRange)) return kAudioHardwareBadPropertySizeError;
                auto* r = static_cast<AudioValueRange*>(outData);
                r->mMinimum = kMinBufferFrames;
                r->mMaximum = kMaxBufferFrames;
                *outDataSize = sizeof(AudioValueRange);
                return kAudioHardwareNoError;
            }
            default: break;
        }
        return UFWithChannelNames<aspl::Device>::GetPropertyData(
            objectID, clientPID, address, qualifierDataSize, qualifierData, inDataSize, outDataSize,
            outData);
    }

    OSStatus SetPropertyData(AudioObjectID objectID, pid_t clientPID,
                             const AudioObjectPropertyAddress* address,
                             UInt32 qualifierDataSize, const void* qualifierData,
                             UInt32 inDataSize, const void* inData) override {
        if (address && address->mSelector == kAudioDevicePropertyClockSource) {
            if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            const UInt32 id = *static_cast<const UInt32*>(inData);
            if (!uf::ctl::is_valid_clock_source(id)) return kAudioHardwareIllegalOperationError;
            uf::shm::Ring* cap = shm_->capture();
            if (!cap) return kAudioHardwareNotRunningError;   // no daemon: nothing owns the device
            cap->requestedClockSource.store(id, std::memory_order_release);
            cap->controlSeq.fetch_add(1, std::memory_order_acq_rel);
            return kAudioHardwareNoError;
        }
        return UFWithChannelNames<aspl::Device>::SetPropertyData(
            objectID, clientPID, address, qualifierDataSize, qualifierData, inDataSize, inData);
    }

    void setIOHandler(std::shared_ptr<UFIOHandler> io) { io_ = std::move(io); }
    uf::shm::Ring* captureRing() const { return shm_->capture(); }

private:
    ShmMapper* shm_;
    uf::ZeroTimeStampClock clock_;
    bool     wasLocked_ = false;   // for edge-triggered clock logging above
    uint64_t lastSeed_  = 0;
    std::shared_ptr<UFIOHandler> io_;   // for keeping its output-stream count current
};

static std::shared_ptr<aspl::Driver> CreateDriver() {
    auto tracer  = std::make_shared<aspl::Tracer>(aspl::Tracer::Mode::Syslog, aspl::Tracer::Style::Flat);
    auto context = std::make_shared<aspl::Context>(tracer);

    aspl::DeviceParameters dev;
    dev.Name                        = "RME Fireface 800";
    dev.Manufacturer                = "UserFace800";
    dev.DeviceUID                   = "UserFace800:FF800";
    dev.CanBeDefault                = true;
    dev.CanBeDefaultForSystemSounds = true;
    dev.SampleRate                  = kSampleRate;
    dev.ChannelCount                = kChannels;
    // The interval between clock anchors, matching what the daemon publishes. It is also what caps
    // the IO buffer sizes a DAW can offer — the HAL allows up to period * 3/8 (see kAnchorFrames).
    dev.ZeroTimeStampPeriod         = uf::shm::kAnchorFrames;

    // Presentation latency and safety offset. The safety offset is what tells the HAL how close to
    // "now" it may read or write; report too little and it mixes right up against the daemon's
    // cursor and underruns on any jitter, which is the choppiness.
    //
    // We need more than the factory driver does: it is in-kernel writing the DMA buffer directly,
    // while our frames cross a process boundary and an SPSC ring on the way. libASPL reports a single
    // device-wide value rather than per-direction, so this is the coarser of the two. The right
    // number is empirical; UF_SAFETY_OFFSET_MS exists to find it on hardware.
    dev.Latency                     = safetyFrames(kSampleRate);
    dev.SafetyOffset                = safetyFrames(kSampleRate);

    // Both rings, kept attached across daemon restarts. Lives for the life of the plugin; the device
    // clock and the IO handler share it.
    static ShmMapper shm;

    // Start at whatever the daemon is actually streaming, so the very first IO cycle already agrees
    // with the device instead of resampling until something changes it.
    if (uf::shm::Ring* cap = shm.capture())
        if (uint32_t r = cap->deviceRate.load(std::memory_order_acquire))
            dev.SampleRate = r;

    auto device = std::make_shared<UFDevice>(context, dev, &shm);

    // One stream per channel group rather than a single wide one — Analog 1-8, Phones/Mic 9-10,
    // SPDIF, ADAT — which is what the factory driver publishes too, and what makes Audio MIDI Setup
    // show four labelled groups instead of a flat list of 28. It also lets
    // CoreAudio enable a group independently, and it is the natural shape for the rate behaviour:
    // ADAT halves at 2x and disappears at 4x while the other groups are untouched.
    const auto addGroups = [&](aspl::Direction dir, uf::Direction ufDir) {
        uint32_t n = 0;
        for (const auto& g : uf::channel_groups(ufDir, uf::Speed::X1)) {
            aspl::StreamParameters p;
            p.Direction       = dir;
            p.StartingChannel = g.start;
            p.Format          = ff800_format(g.count);
            device->AddStreamAsync(std::make_shared<UFNamedStream>(context, device, p, g.count));
            ++n;
        }
        return n;
    };
    addGroups(aspl::Direction::Input, uf::Direction::Capture);
    const uint32_t outStreams = addGroups(aspl::Direction::Output, uf::Direction::Playback);

    auto io = std::make_shared<UFIOHandler>(&shm);
    // The handler publishes a cycle once this many output streams have contributed; without it the
    // cycle could only be closed by the arrival of the next one, costing a whole IO buffer of
    // output latency.
    io->setOutputStreamCount(outStreams);
    device->SetIOHandler(io);
    device->setIOHandler(io);

    // A fresh capture ring means a restarted daemon, which has no idea what rate CoreAudio is on.
    // Re-send the current nominal rate so it follows us instead of streaming its own default.
    shm.setOnCaptureAttach([dev = device.get()] {
        if (uf::shm::Ring* cap = dev->captureRing()) {
            cap->requestedRate.store((uint32_t)dev->GetNominalSampleRate(), std::memory_order_release);
            cap->requestSeq.fetch_add(1, std::memory_order_acq_rel);
        }
    });

    auto plugin = std::make_shared<aspl::Plugin>(context);
    plugin->AddDevice(device);
    return std::make_shared<aspl::Driver>(context, plugin);
}

extern "C" void* UFAudioDriverEntryPoint(CFAllocatorRef, CFUUIDRef typeUUID) {
    if (!CFEqual(typeUUID, kAudioServerPlugInTypeUUID)) return nullptr;
    static std::shared_ptr<aspl::Driver> driver = CreateDriver();
    if (!driver) { os_log_error(OS_LOG_DEFAULT, "UFAudioDriver: CreateDriver failed"); return nullptr; }
    os_log(OS_LOG_DEFAULT, "UFAudioDriver: entry point, driver ready");
    return driver->GetReference();
}
