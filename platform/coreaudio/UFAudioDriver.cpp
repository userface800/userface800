// UFAudioDriver — the UserFace800 AudioServerPlugIn (runs inside coreaudiod).
//
// Layer 1: declare the FF800 to CoreAudio as a 28-in / 28-out, 48 kHz, 24-bit device so it appears in
// Audio MIDI Setup. Built on libASPL. The IO handler produces
// silence for now — real PCM arrives via a shared-memory ring from the daemon (layer 3), which owns
// the FF800 through our OHCI dext. This plugin never touches FireWire directly (the plugin
// only moves PCM; the daemon owns the device).
#include <aspl/Driver.hpp>
#include <aspl/Device.hpp>
#include <aspl/Plugin.hpp>
#include <aspl/Tracer.hpp>
#include <aspl/IORequestHandler.hpp>
#include <os/log.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include "../shared/uf_shm_ring.hpp"

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

// Bridges CoreAudio's realtime IO to the daemon's shared-memory rings. Capture (device->host) is
// popped from kCaptureName; playback (host->device) is pushed to kPlaybackName. The rings hold
// interleaved int32 (top-justified 24-bit), which is exactly our declared stream format, so the RT
// path is a plain memcpy with no conversion. If the daemon is not running the rings are absent and
// we produce/consume silence.
class UFIOHandler : public aspl::IORequestHandler {
public:
    UFIOHandler() {
        capture_  = map(uf::shm::kCaptureName);
        playback_ = map(uf::shm::kPlaybackName);
    }
    // Frame-accurate so it works for any CoreAudio IO buffer size, not just one that equals the ring
    // slot: we pull exactly the requested frames, spanning slot boundaries, holding a partial slot
    // across calls (which correctly back-pressures the producer until the slot is fully consumed).
    void OnReadClientInput(const std::shared_ptr<aspl::Client>&,
                           const std::shared_ptr<aspl::Stream>&,
                           Float64, Float64, void* bytes, UInt32 bytesCount) override {
        std::memset(bytes, 0, bytesCount);
        if (!capture_) return;
        const UInt32 ch = uf::shm::kMaxChannels;
        const UInt32 frames = bytesCount / (ch * (UInt32)sizeof(int32_t));
        int32_t* out = static_cast<int32_t*>(bytes);
        for (UInt32 f = 0; f < frames; ++f) {
            if (!rdSlot_ || rdFrame_ >= rdSlot_->frameCount) {
                if (rdSlot_) { capture_->commitRead(); rdSlot_ = nullptr; }
                rdSlot_ = capture_->acquireRead();
                rdFrame_ = 0;
                if (!rdSlot_) return;                     // underrun -> rest stays silent
            }
            std::memcpy(out + (size_t)f * ch,
                        rdSlot_->audio + (size_t)rdFrame_ * ch, ch * sizeof(int32_t));
            ++rdFrame_;
        }
    }
    void OnWriteMixedOutput(const std::shared_ptr<aspl::Stream>&,
                            Float64, Float64, const void* bytes, UInt32 bytesCount) override {
        if (!playback_) return;
        const UInt32 ch = uf::shm::kMaxChannels;
        const UInt32 frames = bytesCount / (ch * (UInt32)sizeof(int32_t));
        const int32_t* in = static_cast<const int32_t*>(bytes);
        for (UInt32 f = 0; f < frames; ++f) {
            if (!wrSlot_) { wrSlot_ = playback_->acquireWrite(); wrFrame_ = 0;
                            if (!wrSlot_) return; }       // overrun -> drop the rest
            std::memcpy(wrSlot_->audio + (size_t)wrFrame_ * ch,
                        in + (size_t)f * ch, ch * sizeof(int32_t));
            if (++wrFrame_ >= uf::shm::kFramesPerSlot) {
                wrSlot_->frameCount = wrFrame_; wrSlot_->channelCount = ch;
                playback_->commitWrite(); wrSlot_ = nullptr;
            }
        }
    }
private:
    static uf::shm::Ring* map(const char* name) {
        int fd = shm_open(name, O_RDWR, 0);
        if (fd < 0) return nullptr;
        void* p = mmap(nullptr, sizeof(uf::shm::Ring), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (p == MAP_FAILED) return nullptr;
        auto* r = static_cast<uf::shm::Ring*>(p);
        return r->magic == uf::shm::kMagic ? r : nullptr;   // daemon must have initialised it
    }
    uf::shm::Ring* capture_ = nullptr;
    uf::shm::Ring* playback_ = nullptr;
    const uf::shm::Slot* rdSlot_ = nullptr;   // partial capture slot held across IO calls
    uint32_t rdFrame_ = 0;
    uf::shm::Slot* wrSlot_ = nullptr;         // partial playback slot being filled
    uint32_t wrFrame_ = 0;
};

// The seven native sample rates.
static const double kRates[] = {32000, 44100, 48000, 88200, 96000, 176400, 192000};

// FF800 PCM channel count per rate: the ADAT ports drop out as speed rises (28 @ 1x, 20 @ 2x, 12 @ 4x).
static uint32_t channelsForRate(double r) { return r <= 48000 ? 28 : r <= 96000 ? 20 : 12; }

// A stream that advertises our format at every native rate. Apple's HAL only exposes a device rate
// if some stream has a matching format, so the device rate list alone (UFDevice) isn't enough —
// the streams must enumerate the rates too. Overriding the getters works at construction (the Async
// setters need a live HAL connection). Channel count stays 28 (per-rate counts are a follow-up).
class UFStream : public aspl::Stream {
public:
    using aspl::Stream::Stream;
    std::vector<AudioStreamRangedDescription> formats() const {
        std::vector<AudioStreamRangedDescription> v;
        for (double r : kRates) {
            AudioStreamRangedDescription d = {};
            d.mFormat = ff800_format(channelsForRate(r)); d.mFormat.mSampleRate = r;
            d.mSampleRateRange = {r, r};
            v.push_back(d);
        }
        return v;
    }
    std::vector<AudioStreamRangedDescription> GetAvailablePhysicalFormats() const override { return formats(); }
    std::vector<AudioStreamRangedDescription> GetAvailableVirtualFormats() const override { return formats(); }
};

// Device that locks CoreAudio's timeline to the FF800 by reading the daemon's device-paced clock
// (uf::shm::Ring::readTimestamp) instead of libASPL's default free-running one. This is the userland
// analogue of the factory driver's timestamp-on-ring-wrap. Falls back to the base
// (free-running) clock until the daemon has published a timestamp, so the device still works solo.
class UFDevice : public aspl::Device {
public:
    UFDevice(std::shared_ptr<aspl::Context> ctx, const aspl::DeviceParameters& p, uf::shm::Ring* cap)
        : aspl::Device(ctx, p), capture_(cap) {}

    // Advertise every native rate. Overriding the getter works at construction time; the Async
    // setter does not (it needs a live HAL connection, which doesn't exist yet in CreateDriver).
    std::vector<AudioValueRange> GetAvailableSampleRates() const override {
        return {{32000, 32000}, {44100, 44100}, {48000, 48000},
                {88200, 88200}, {96000, 96000}, {176400, 176400}, {192000, 192000}};
    }

    OSStatus GetZeroTimeStamp(AudioObjectID objID, UInt32 clientID,
                              Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed) override {
        if (capture_) {
            double sampleTime; uint64_t seqseed; uint64_t hostTime;
            for (int retry = 0; retry < 4; ++retry) {
                if (capture_->readTimestamp(&sampleTime, &hostTime, &seqseed)) {
                    *outSampleTime = sampleTime; *outHostTime = hostTime;
                    // The CoreAudio seed marks timeline DISCONTINUITIES — it must stay constant while
                    // the clock is continuous. (The ring's seqlock seed, which bumps every publish, is
                    // only for torn-read detection and must NOT leak out here, or the HAL re-anchors
                    // every call and never locks the rate.)
                    *outSeed = 1;
                    return kAudioHardwareNoError;
                }
            }
        }
        return aspl::Device::GetZeroTimeStamp(objID, clientID, outSampleTime, outHostTime, outSeed);
    }
private:
    uf::shm::Ring* capture_;
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
    // We publish the clock anchor once per ring slot (kFramesPerSlot), so that is the period between
    // successive GetZeroTimeStamp sample times.
    dev.ZeroTimeStampPeriod         = uf::shm::kFramesPerSlot;

    // Map the daemon's capture ring so the device clock can read the timestamp (harmless if absent).
    uf::shm::Ring* clockRing = nullptr;
    { int fd = shm_open(uf::shm::kCaptureName, O_RDWR, 0);
      if (fd >= 0) { void* m = mmap(nullptr, sizeof(uf::shm::Ring), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                     close(fd);
                     if (m != MAP_FAILED && static_cast<uf::shm::Ring*>(m)->magic == uf::shm::kMagic)
                         clockRing = static_cast<uf::shm::Ring*>(m); } }

    auto device = std::make_shared<UFDevice>(context, dev, clockRing);

    // Presentation latency / safety offset, matching the factory driver's order of magnitude
    // (~40 out, ~140 in). One ring slot of headroom absorbs the daemon<->plugin scheduling jitter.
    device->SetLatencyAsync(uf::shm::kFramesPerSlot);

    aspl::StreamParameters out;
    out.Direction       = aspl::Direction::Output;   // playback: host -> FF800
    out.StartingChannel = 1;
    out.Format          = ff800_format(kChannels);
    device->AddStreamAsync(std::make_shared<UFStream>(context, device, out));

    aspl::StreamParameters in;
    in.Direction       = aspl::Direction::Input;     // capture: FF800 -> host
    in.StartingChannel = 1;
    in.Format          = ff800_format(kChannels);
    device->AddStreamAsync(std::make_shared<UFStream>(context, device, in));

    auto io = std::make_shared<UFIOHandler>();
    device->SetIOHandler(io);

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
