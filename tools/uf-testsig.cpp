// uf-testsig — write a self-describing test-signal WAV, and analyse a recording of it.
//
// The point of a FILE rather than a driver mode: the identical signal can be played through our
// driver and through RME's, on different machines, with nothing privileged installed on either. Play
// it, loop the output back to an input, record, and analyse both recordings the same way. Any
// difference between the two drivers is then a difference in the numbers, not in what someone heard.
//
//   uf-testsig gen  <out.wav> [--rate=96000] [--channels=8] [--seconds=30]
//   uf-testsig check <recorded.wav> [--channel=0] [--expect=0]
//
// The recording must be bit-transparent end to end: DAW at unity with no plugins or dither, and the
// FF800 mixer at unity (which uf-daemon sets by default). If `check` reports mostly "not encoded",
// that is what went wrong — fix it before reading anything into the other counters.
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#include "uf/protocol/testsig.hpp"
#include "uf/protocol/wav.hpp"

static void usage() {
    std::printf(
        "usage: uf-testsig gen   <out.wav> [--rate=96000] [--channels=8] [--seconds=30]\n"
        "       uf-testsig check <recorded.wav> [--channel=0] [--expect=<channel id>]\n"
        "\n"
        "  gen    writes a 24-bit WAV whose every sample encodes its own channel and frame index\n"
        "  check  reports dropped / duplicated / reordered samples and channel shear\n"
        "\n"
        "The path must be bit-transparent: DAW at unity, no plugins, no dither, mixer at unity.\n");
}

// Minimal WAV reader: enough for what a DAW writes. 24-bit PCM only, which is what the FF800 is.
static bool read_wav(const char* path, std::vector<uf::i32>& out, uint32_t& channels,
                     uint32_t& rate, uint32_t& bits) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::perror("open"); return false; }
    std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> b((size_t)n);
    if (std::fread(b.data(), 1, b.size(), f) != b.size()) { std::fclose(f); return false; }
    std::fclose(f);
    if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4)) {
        std::fprintf(stderr, "not a RIFF/WAVE file\n"); return false;
    }
    // Walk the chunks rather than assuming the canonical 44-byte layout — DAWs add their own.
    size_t p = 12; size_t dataOff = 0, dataLen = 0;
    channels = rate = bits = 0;
    while (p + 8 <= b.size()) {
        char id[5] = {0}; std::memcpy(id, &b[p], 4);
        uint32_t sz; std::memcpy(&sz, &b[p + 4], 4);
        if (!std::memcmp(id, "fmt ", 4) && p + 8 + 16 <= b.size()) {
            uint16_t ch, bps; uint32_t sr;
            std::memcpy(&ch, &b[p + 10], 2);
            std::memcpy(&sr, &b[p + 12], 4);
            std::memcpy(&bps, &b[p + 22], 2);
            channels = ch; rate = sr; bits = bps;
        } else if (!std::memcmp(id, "data", 4)) {
            dataOff = p + 8; dataLen = sz;
        }
        p += 8 + sz + (sz & 1);
    }
    if (!channels || !dataLen) { std::fprintf(stderr, "no fmt/data chunk\n"); return false; }
    if (bits != 24) {
        std::fprintf(stderr, "need 24-bit PCM, got %u-bit — a converted file is not bit-exact\n", bits);
        return false;
    }
    if (dataOff + dataLen > b.size()) dataLen = b.size() - dataOff;
    const size_t samples = dataLen / 3;
    out.resize(samples);
    for (size_t i = 0; i < samples; ++i) {
        const uint8_t* q = &b[dataOff + i * 3];
        uint32_t v = (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16);
        out[i] = (v & 0x800000u) ? (int32_t)(v | 0xFF000000u) : (int32_t)v;
    }
    return true;
}

int main(int argc, char** argv) {
    if (argc < 3) { usage(); return 2; }
    const std::string cmd = argv[1];
    const char* path = argv[2];

    auto opt = [&](const char* name, long def) -> long {
        for (int i = 3; i < argc; ++i) {
            const size_t n = std::strlen(name);
            if (!std::strncmp(argv[i], name, n)) return std::strtol(argv[i] + n, nullptr, 10);
        }
        return def;
    };

    if (cmd == "gen") {
        const uint32_t rate = (uint32_t)opt("--rate=", 96000);
        const uint32_t channels = (uint32_t)opt("--channels=", 8);
        const uint32_t seconds = (uint32_t)opt("--seconds=", 30);
        if (channels < 1 || channels > uf::kTestSigMaxChannels) {
            std::fprintf(stderr, "channels must be 1..%u\n", uf::kTestSigMaxChannels); return 2;
        }
        const uint64_t frames = (uint64_t)rate * seconds;
        auto pcm = uf::testsig_generate(channels, frames);
        auto bytes = uf::write_wav(pcm, rate, (uint16_t)channels);
        FILE* f = std::fopen(path, "wb");
        if (!f) { std::perror("open"); return 1; }
        std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
        std::printf("wrote %s: %u ch, %u Hz, %llu frames (%u s), 24-bit\n",
                    path, channels, rate, (unsigned long long)frames, seconds);
        std::printf("index wraps every %u frames (%.2f s at this rate)\n",
                    uf::kTestSigPeriod, (double)uf::kTestSigPeriod / rate);
        return 0;
    }

    if (cmd == "check") {
        std::vector<uf::i32> pcm; uint32_t channels = 0, rate = 0, bits = 0;
        if (!read_wav(path, pcm, channels, rate, bits)) return 1;
        const uint32_t column = (uint32_t)opt("--channel=", 0);
        const uint32_t expect = (uint32_t)opt("--expect=", (long)column);
        if (column >= channels) { std::fprintf(stderr, "file has %u channels\n", channels); return 2; }
        const uint64_t frames = pcm.size() / channels;
        std::printf("%s: %u ch, %u Hz, %llu frames (%.2f s)\n",
                    path, channels, rate, (unsigned long long)frames, (double)frames / rate);

        auto r = uf::testsig_analyse(pcm.data(), frames, channels, column, expect);
        if (!r.started) {
            std::printf("column %u: no encoded signal found — silence, or the path is not "
                        "bit-transparent\n", column);
            return 1;
        }
        std::printf("column %u (expecting channel id %u): %llu frames checked\n",
                    column, expect, (unsigned long long)r.framesChecked);
        std::printf("  gaps=%llu (dropped %llu frames)  repeats=%llu  backwards=%llu  "
                    "wrong-channel=%llu  not-encoded=%llu\n",
                    (unsigned long long)r.gaps, (unsigned long long)r.droppedFrames,
                    (unsigned long long)r.repeats, (unsigned long long)r.backwards,
                    (unsigned long long)r.wrongChannel, (unsigned long long)r.notEncoded);
        if (r.notEncoded > r.framesChecked / 100)
            std::printf("  ** mostly unencoded: the path is NOT bit-transparent (gain, dither,\n"
                        "     resampling or a fader off unity). Fix that before reading the rest.\n");
        for (const auto& e : r.events)
            std::printf("    %-38s at frame %llu (%.3f s)%s\n",
                        uf::testsig_fault_name(e.fault).c_str(),
                        (unsigned long long)e.frame, (double)e.frame / rate,
                        e.fault == uf::TestSigFault::WrongChannel
                            ? (" saw channel " + std::to_string(e.sawChannel)).c_str() : "");
        if (r.events.size() >= uf::kTestSigMaxEvents) std::printf("    ... (event list capped)\n");
        std::printf("%s\n", r.clean() ? "CLEAN — bit-exact end to end" : "FAULTS FOUND (see above)");
        return r.clean() ? 0 : 1;
    }

    usage();
    return 2;
}
