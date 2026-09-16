// uf-midi — bridge the FF800's MIDI to CoreMIDI through the OHCI dext.
//
// Creates two virtual CoreMIDI endpoints (the daemon's UFMidi bridge): a destination
// "Fireface 800 MIDI Out" (apps -> FF800) and a source "Fireface 800 MIDI In" (FF800 -> apps).
// Framing is the portable uf::midi (one MIDI byte per little-endian quadlet, spec/04).
//
// OUT is fully real and testable on hardware: it block-writes to 0x80180000 via the dext.
// IN is HARDWARE-GATED: it advertises a host receive address and polls the dext, but the dext's
// AR-request receive path has never delivered a packet, so nothing has been received yet.
//
// DO NOT RUN THIS ALONGSIDE uf-daemon. The daemon now carries the same MIDI bridge internally, and
// it is the better place for it: every register transaction goes through the dext's single AT
// context, and two processes each holding their own user client can have their external methods
// dispatched on separate queues, so a MIDI write and an audio-path register write can interleave
// inside one transaction and corrupt both. This binary remains for MIDI-only work with no audio
// session running (and as the smallest possible harness for debugging the MIDI-in path).
#include <CoreMIDI/CoreMIDI.h>
#include <IOKit/IOKitLib.h>
#include <unistd.h>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <atomic>
#include <vector>

#include "uf/protocol/midi.hpp"
#include "uf/protocol/registers.hpp"

enum { kWQ = 1, kWB = 2, kMidiInEnable = 11, kMidiInPoll = 12, kMidiInDisable = 13 };

static std::atomic<bool> g_run{true};
static void on_signal(int) { g_run.store(false); }
static io_connect_t g_conn = 0;

static io_connect_t open_dext() {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
                                                  IOServiceNameMatching("UFFireWireOHCI"));
    if (!svc) return 0;
    io_connect_t c = 0;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &c);
    IOObjectRelease(svc);
    return kr == KERN_SUCCESS ? c : 0;
}

// MIDI OUT: pack bytes one-per-quadlet and block-write to 0x80180000, in <=9-byte transactions,
// paced at the DIN-MIDI rate (uf::midi_throttle_ns).
static void send_to_ff800(const uint8_t* bytes, size_t n) {
    if (!g_conn) return;
    size_t i = 0;
    while (i < n) {
        const size_t chunk = (n - i) < uf::kMidiMaxQuads ? (n - i) : uf::kMidiMaxQuads;
        std::vector<uint32_t> quads = uf::midi_out_quadlets(bytes + i, chunk);
        uint64_t off = uf::kMidiOutAddr;
        IOConnectCallMethod(g_conn, kWB, &off, 1, quads.data(), quads.size() * 4,
                            nullptr, nullptr, nullptr, nullptr);
        usleep((useconds_t)(uf::midi_throttle_ns((uint32_t)chunk) / 1000));
        i += chunk;
    }
}

// CoreMIDI calls this on the MIDIServer thread when an app sends to our destination.
static void read_proc(const MIDIPacketList* pktlist, void*, void*) {
    const MIDIPacket* p = &pktlist->packet[0];
    for (unsigned i = 0; i < pktlist->numPackets; ++i) {
        send_to_ff800(p->data, p->length);
        p = MIDIPacketNext(p);
    }
}

int main() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // The endpoints exist regardless of the device; bridging is inert until the dext is present.
    g_conn = open_dext();
    if (!g_conn) std::fprintf(stderr, "uf-midi: dext not available yet — endpoints will be idle\n");

    MIDIClientRef client = 0;
    MIDIClientCreate(CFSTR("UserFace800"), nullptr, nullptr, &client);

    MIDIEndpointRef dest = 0;   // apps -> us -> FF800
    MIDIDestinationCreate(client, CFSTR("Fireface 800 MIDI Out"), read_proc, nullptr, &dest);

    MIDIEndpointRef src = 0;    // FF800 -> us -> apps
    MIDISourceCreate(client, CFSTR("Fireface 800 MIDI In"), &src);

    // Advertise a host receive address so the FF800 sends us MIDI (high index 1 -> writes land at
    // 0x1_00000000). HARDWARE-GATED: nothing is received until the dext AR-request path works.
    const uint32_t highIndex = 1;
    if (g_conn) { uint64_t hi = highIndex;
        IOConnectCallScalarMethod(g_conn, kMidiInEnable, &hi, 1, nullptr, nullptr); }

    std::printf("uf-midi: endpoints up (MIDI Out real; MIDI In gated). Ctrl-C to stop.\n");

    // Poll the dext for received MIDI and forward to the CoreMIDI source.
    while (g_run.load()) {
        if (!g_conn) { usleep(50000); continue; }
        // out: scalar[0] = count (0..8), scalar[1] = that many bytes packed low-byte-first.
        uint64_t out[2] = {0, 0}; uint32_t n = 2;
        if (IOConnectCallScalarMethod(g_conn, kMidiInPoll, nullptr, 0, out, &n) == KERN_SUCCESS &&
            out[0] > 0 && out[0] <= 8) {
            const uint32_t count = (uint32_t)out[0];
            uint8_t bytes[8];
            for (uint32_t i = 0; i < count; ++i) bytes[i] = (uint8_t)((out[1] >> (8 * i)) & 0xff);
            uint8_t pktbuf[256];
            MIDIPacketList* pl = reinterpret_cast<MIDIPacketList*>(pktbuf);
            MIDIPacket* cur = MIDIPacketListInit(pl);
            cur = MIDIPacketListAdd(pl, sizeof(pktbuf), cur, 0, count, bytes);
            if (cur) MIDIReceived(src, pl);
        }
        usleep(2000);
    }

    if (g_conn) { IOConnectCallScalarMethod(g_conn, kMidiInDisable, nullptr, 0, nullptr, nullptr);
                  IOServiceClose(g_conn); }
    MIDIClientDispose(client);
    return 0;
}
