// uf-capture — start an FF800 streaming session, capture its isochronous stream through the dext,
// decode it with the portable codec and write a WAV.
//
// The register sequence is the portable one (uf::stream_start_writes, spec/03 §3.6, adapted from
// snd-fireface ff800_allocate_resources/begin_session); the host-only steps live here: poll the
// device-assigned TX channel, start the OHCI IR context, then begin the session.
//
//   uf-capture [--rate=48000] [--packets=2048] [out.wav]
#include <IOKit/IOKitLib.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <unistd.h>
#include <vector>

#include "uf/protocol/channels.hpp"
#include "uf/protocol/codec.hpp"
#include "uf/protocol/rate.hpp"
#include "uf/protocol/registers.hpp"
#include "uf/protocol/stream.hpp"
#include "uf/protocol/wav.hpp"

// Must match UFFireWireOHCIShared.h.
enum { kUFOhciReadQuadlet = 0, kUFOhciWriteQuadlet = 1, kUFOhciWriteBlock = 2,
       kUFOhciIsoStart = 3, kUFOhciIsoPoll = 4, kUFOhciIsoStop = 5,
       kUFOhciIsoTxStart = 6, kUFOhciIsoTxStop = 7 };
static const uint32_t kSlotBytes  = 2048;
static const uint32_t kMemoryType = 0;
static const uint32_t kMaxPackets = 512;   // must match kUFOhciIsoMaxPackets

static io_connect_t open_dext() {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
                                                  IOServiceNameMatching("UFFireWireOHCI"));
    if (!svc) return 0;
    io_connect_t conn = 0;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    return kr == KERN_SUCCESS ? conn : 0;
}

static bool rq(io_connect_t c, uint64_t off, uint32_t* out) {
    uint64_t in = off, val = 0; uint32_t n = 1;
    if (IOConnectCallScalarMethod(c, kUFOhciReadQuadlet, &in, 1, &val, &n) != KERN_SUCCESS) return false;
    *out = (uint32_t)val;
    return true;
}
static bool wq(io_connect_t c, uint64_t off, uint32_t val) {
    uint64_t in[2] = {off, val};
    return IOConnectCallScalarMethod(c, kUFOhciWriteQuadlet, in, 2, nullptr, nullptr) == KERN_SUCCESS;
}
static bool call(io_connect_t c, uint32_t sel, const uint64_t* in, uint32_t n) {
    return IOConnectCallScalarMethod(c, sel, in, n, nullptr, nullptr) == KERN_SUCCESS;
}
static bool wblock(io_connect_t c, uint64_t off, const uint32_t* q, uint32_t n) {
    return IOConnectCallMethod(c, kUFOhciWriteBlock, &off, 1, q, n * 4,
                               nullptr, nullptr, nullptr, nullptr) == KERN_SUCCESS;
}

// Which register bank drives streaming. Default = the 0x0002 bank the factory driver uses; fc88f is
// the legacy snd-fireface/FFADO path, kept as an override (--bank=fc88f).
enum class Bank { Init0002, Fc88f };
static Bank g_bank = Bank::Init0002;
static int  g_holdSecs = 0;   // --hold=N: keep the session open N seconds so the LED is observable
static bool g_ledSweep = false;

// "Fetching mode" (snd-fireface former_switch_fetching_mode): one quadlet per playback channel at
// 0x801c0000. Zeros = fetch PCM frames (start); ones = stop fetching. Skipping this on the way out
// leaves the device mid-stream: it then never republishes its TX channel and needs a power cycle.
static bool set_fetching(io_connect_t c, uint32_t channels, bool enable) {
    std::vector<uint32_t> q(channels, enable ? 0u : 1u);
    return wblock(c, uf::reg::kStatus0, q.data(), channels);
}

// The device-side comm-stop write for the active bank. fc88f: one quadlet 0x80000000 -> fc88f010.
// 0x0002: a 3-quadlet blob -> 0x200000034.
static void device_comm_stop(io_connect_t c) {
    if (g_bank == Bank::Fc88f) {
        wq(c, uf::reg::kIsocCommStop, 0x80000000u);
    } else {
        const uint32_t blob[3] = {0u, 0u, 0u};   // the device stop is three zero quadlets
        wblock(c, uf::reg::kInitBankStop, blob, 3);
    }
}

// Tear a session down the way the reference does: stop OUR streams first, then close the session on
// the device, then switch fetching off.
static void end_session(io_connect_t c, uint32_t channels) {
    call(c, kUFOhciIsoStop, nullptr, 0);
    call(c, kUFOhciIsoTxStop, nullptr, 0);
    device_comm_stop(c);
    set_fetching(c, channels, false);
    usleep(50 * 1000);
}

// Clearing a stale session before opening a new one is NOT the same as ending one: writing the
// fetch-off mask here mutes the device's channels, and it then refuses to publish a TX channel at
// all. Close the session, leave the mask alone.
static void clear_stale_session(io_connect_t c) {
    call(c, kUFOhciIsoStop, nullptr, 0);
    call(c, kUFOhciIsoTxStop, nullptr, 0);
    device_comm_stop(c);
    usleep(100 * 1000);
}

int main(int argc, char** argv) {
    uint32_t rate = 48000, packets = 2048;
    const char* path = "capture.wav";
    for (int i = 1; i < argc; ++i) {
        if (!std::strncmp(argv[i], "--rate=", 7)) rate = (uint32_t)std::atoi(argv[i] + 7);
        else if (!std::strncmp(argv[i], "--packets=", 10)) packets = (uint32_t)std::atoi(argv[i] + 10);
        else if (!std::strcmp(argv[i], "--bank=fc88f")) g_bank = Bank::Fc88f;
        else if (!std::strcmp(argv[i], "--bank=0002")) g_bank = Bank::Init0002;
        else if (!std::strncmp(argv[i], "--hold=", 7)) g_holdSecs = atoi(argv[i] + 7);
        else if (!std::strcmp(argv[i], "--led-sweep")) g_ledSweep = true;
        else path = argv[i];
    }
    std::printf("streaming bank: %s\n", g_bank == Bank::Fc88f ? "fc88f (legacy)" : "0x0002 (RME default)");

    if (packets == 0 || packets > kMaxPackets) {
        std::fprintf(stderr, "packets must be 1..%u (the dext's capture buffer)\n", kMaxPackets);
        return 2;
    }

    io_connect_t conn = open_dext();
    if (!conn) { std::fprintf(stderr, "dext user client not available\n"); return 1; }

    const uf::Speed speed = uf::speed_for_rate(rate);
    const uint32_t  dbq   = uf::data_block_quadlets(speed);   // = PCM channels per frame
    const uint32_t  rx_channel = 1;   // playback channel: we do not transmit yet, but the device
                                      // wants a channel programmed before it will start its own stream
    std::printf("rate=%u  channels=%u  frames/packet=%u  packets=%u\n",
                rate, dbq, uf::frames_per_packet(speed), packets);

    const bool s800 = true;
    const uint32_t dbqFlag = dbq | (s800 ? 0x800u : 0u);   // double-speed flag rides with dbq

    // 0. Close any session a previous run left open, before opening a new one.
    clear_stale_session(conn);

    // 1-2. rate + packet-format + tx-stream request.
    if (g_bank == Bank::Fc88f) {
        // Legacy path (snd-fireface/FFADO): separate writes into the fc88f bank; STF needs a settle.
        if (!wq(conn, uf::reg::kStf, rate)) { std::fprintf(stderr, "STF write failed\n"); return 1; }
        usleep(100 * 1000);
        for (const uf::RegWrite& w : uf::stream_start_writes(dbq, rx_channel, uf::BusSpeed::S800)) {
            if (w.addr == uf::reg::kIsocCommStart) continue;   // comm start goes last, after the IR ctx
            if (!wq(conn, w.addr, w.quad)) { std::fprintf(stderr, "write 0x%llx failed\n",
                                                          (unsigned long long)w.addr); return 1; }
        }
    } else {
        // The 0x0002 bank: one 3-quad block {rate, (dbq<<11)|rxCh, dbq[|0x800]}.
        const uint32_t init[3] = {rate, (dbq << 11) | rx_channel, dbqFlag};
        if (!wblock(conn, uf::reg::kInitBankStream, init, 3)) {
            std::fprintf(stderr, "init-bank stream write failed\n"); return 1;
        }
        usleep(100 * 1000);
    }

    // 3. the device picks its own TX channel and publishes it here.
    uint32_t tx_channel = 0xffffffff;
    for (int i = 0; i < 100 && tx_channel == 0xffffffff; ++i) {
        if (!rq(conn, uf::reg::kTxIsoChannel, &tx_channel)) break;
        if (tx_channel == 0xffffffff) usleep(20 * 1000);
    }
    if (tx_channel == 0xffffffff) {
        std::fprintf(stderr, "device never published a TX channel "
                             "(a previous run may have left it mid-stream — power cycle it)\n");
        end_session(conn, dbq);
        return 1;
    }
    std::printf("device transmits on iso channel %u\n", tx_channel);

    // 4. arm the OHCI IR context BEFORE the device starts sending.
    uint64_t startArgs[2] = {tx_channel, packets};
    if (!call(conn, kUFOhciIsoStart, startArgs, 2)) {
        std::fprintf(stderr, "IsoStart failed\n"); end_session(conn, dbq); return 1;
    }

    // 4b. and start sending it a (silent) blocking playback stream: the FF800 clocks its own transmit
    //     off the stream it receives and won't talk into silence. Blocking (ref/snd-fireface): full
    //     packets carry syt_interval frames, fullPerFour of every 4 full to hit the rate/8000 average.
    const uint32_t syt        = uf::frames_per_packet(speed);           // 8/16/32
    const uint32_t fullPayload = syt * dbq * 4;
    const uint32_t fullCount   = uf::blocking_full_count(rate, speed);  // full pkts in the 640-ring
    uint64_t txArgs[3] = {rx_channel, fullPayload, fullCount};
    if (!call(conn, kUFOhciIsoTxStart, txArgs, 3)) {
        std::fprintf(stderr, "IsoTxStart failed\n"); end_session(conn, dbq); return 1;
    }
    std::printf("playback stream: channel %u, blocking %u frames/full pkt, %u/%u full\n",
                rx_channel, syt, fullCount, uf::kBlockingRingLen);

    // 5. begin the session — the device starts transmitting audio. Same value in both banks
    //    (0x80000000 | dbq | s800-flag); only the address differs.
    const uint32_t commStart = 0x80000000u | dbqFlag;
    const uint64_t csAddr = (g_bank == Bank::Fc88f) ? uf::reg::kIsocCommStart : uf::reg::kInitBankStart;
    if (!wq(conn, csAddr, commStart)) {
        std::fprintf(stderr, "comm start failed\n"); end_session(conn, dbq); return 1;
    }

    // 5b. and tell it to actually fetch PCM frames (snd-fireface does this right after begin_session).
    if (!set_fetching(conn, dbq, true)) {
        std::fprintf(stderr, "fetch enable failed\n"); end_session(conn, dbq); return 1;
    }

    // 5c. claim the device: write 0 to the host-LED register — the handshake we were missing that
    //     should turn the front-panel HOST LED green. Untested.
    wq(conn, uf::reg::kHostLed, 0);

    // LED sweep: with the session live, write a series of values to the host-LED register and pause
    // between each so the operator can call out which one (if any) turns the LED green. The factory
    // driver only ever writes 0 here, which doesn't prove the polarity, so we brute-force it.
    if (g_ledSweep) {
        const uint32_t vals[] = {0, 1, 2, 3, 4, 0x100, 0xffff, 0xffffffff};
        for (uint32_t v : vals) {
            std::printf("  LED register 0x200000324 <- 0x%08x  (watch ~4s)\n", v); std::fflush(stdout);
            wq(conn, uf::reg::kHostLed, v);
            sleep(4);
        }
        end_session(conn, dbq);
        std::printf("led sweep done, session closed.\n");
        IOServiceClose(conn);
        return 0;
    }

    // Optional: hold the session open so the LED's steady state is observable (it reflects an active
    // hosted session, so a sub-second capture tears down before you can look).
    if (g_holdSecs > 0) {
        std::printf("holding session open for %d s — watch the HOST LED now...\n", g_holdSecs);
        std::fflush(stdout);
        sleep(g_holdSecs);
        end_session(conn, dbq);
        std::printf("session held %d s then closed.\n", g_holdSecs);
        IOServiceClose(conn);
        return 0;
    }
    std::printf("session started, capturing... (host-LED claim written)\n");

    // 6. wait for the buffer to fill.
    uint32_t got = 0;
    for (int i = 0; i < 600 && got < packets; ++i) {   // enough for a multi-second capture
        uint64_t out = 0; uint32_t n = 1;
        IOConnectCallScalarMethod(conn, kUFOhciIsoPoll, nullptr, 0, &out, &n);
        got = (uint32_t)out;
        usleep(20 * 1000);
    }

    // 7. tear the session down cleanly, whatever happened.
    end_session(conn, dbq);
    std::printf("captured %u/%u packets\n", got, packets);
    if (got == 0) {
        std::fprintf(stderr, "no isochronous packets arrived\n");
        IOServiceClose(conn);
        return 1;
    }

    // 8. map the capture buffer and decode. Each slot = 4-byte iso header, then the payload.
    mach_vm_address_t addr = 0;
    mach_vm_size_t    size = 0;
    if (IOConnectMapMemory64(conn, kMemoryType, mach_task_self(), &addr, &size,
                             kIOMapAnywhere | kIOMapReadOnly) != KERN_SUCCESS) {
        std::fprintf(stderr, "could not map the capture buffer\n");
        IOServiceClose(conn);
        return 1;
    }
    const uint8_t* buf = reinterpret_cast<const uint8_t*>(addr);

    // What did the controller actually put in the slot? Dump before trusting any layout assumption.
    if (getenv("UF_DUMP")) {
        for (uint32_t i = 0; i < 2 && i < got; ++i) {
            const uint32_t* q = reinterpret_cast<const uint32_t*>(buf + (size_t)i * kSlotBytes);
            std::printf("slot %u:", i);
            for (int j = 0; j < 10; ++j) std::printf(" %08x", q[j]);
            std::printf("\n");
        }
    }

    std::vector<int32_t> samples;   // frame-major, dbq channels per frame
    uint32_t frames = 0;
    std::vector<int32_t> f(dbq);
    int32_t peak = 0;
    for (uint32_t i = 0; i < got; ++i) {
        const uint8_t* slot = buf + (size_t)i * kSlotBytes;
        // The controller writes TWO quadlets ahead of the payload (ohci.c copy_iso_headers):
        // [0] = timestamp, [1] = the isochronous packet header, whose top 16 bits are data_length.
        const uint32_t* q   = reinterpret_cast<const uint32_t*>(slot);
        const uint32_t  len = q[1] >> 16;
        if (len == 0 || len > kSlotBytes - 8) continue;
        const uint8_t* payload = slot + 8;
        const uint32_t n = uf::frames_in_payload(len, dbq);
        for (uint32_t k = 0; k < n; ++k) {
            uf::decode_frame(payload + (size_t)k * dbq * 4, dbq, f.data());
            for (uint32_t ch = 0; ch < dbq; ++ch) {
                samples.push_back(f[ch]);
                int32_t a = f[ch] < 0 ? -f[ch] : f[ch];
                if (a > peak) peak = a;
            }
            ++frames;
        }
    }
    if (frames == 0) { std::fprintf(stderr, "packets arrived but decoded to no frames\n"); return 1; }

    std::vector<uint8_t> wav = uf::write_wav(samples, rate, (uint16_t)dbq, 24);
    FILE* fp = std::fopen(path, "wb");
    if (!fp) { std::perror("fopen"); return 1; }
    std::fwrite(wav.data(), 1, wav.size(), fp);
    std::fclose(fp);
    std::printf("peak sample: %d (%.1f dBFS)\n", peak,
                peak ? 20.0 * log10((double)peak / 8388607.0) : -144.0);

    // Per-channel levels: which input is actually carrying signal (the whole point of the capture).
    std::printf("\nper-channel peak (dBFS):\n");
    for (uint32_t ch = 0; ch < dbq; ++ch) {
        int32_t p = 0;
        for (uint32_t k = 0; k < frames; ++k) {
            int32_t v = samples[(size_t)k * dbq + ch];
            int32_t a = v < 0 ? -v : v;
            if (a > p) p = a;
        }
        const double db = p ? 20.0 * log10((double)p / 8388607.0) : -144.0;
        std::printf("  ch %2u: %7.1f  %s\n", ch + 1, db,
                    db > -60.0 ? "########" : (p ? "." : ""));
    }
    std::printf("wrote %s: %u frames, %u channels, %u Hz (%.2f s)\n",
                path, frames, dbq, rate, (double)frames / rate);

    IOConnectUnmapMemory64(conn, kMemoryType, mach_task_self(), addr);
    IOServiceClose(conn);
    return 0;
}
