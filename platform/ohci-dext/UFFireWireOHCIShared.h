// UFFireWireOHCIShared.h — the user-client ABI shared by the dext and the UserFace800 daemon.
//
// The daemon (UFAsyncIO) opens an IOUserClient connection to the dext and calls these external
// methods to move register transactions to the FF800. Offsets are the full 48-bit FireWire target
// address (== uf::reg::* / uf::Addr); the dext already knows the FF800's node id + speed from the
// self-ID / config-ROM identify pass, so the daemon just passes address + value.
#ifndef UF_OHCI_SHARED_H
#define UF_OHCI_SHARED_H

#include <stdint.h>

enum UFOhciMethod {
    kUFOhciReadQuadlet  = 0,  // in:  scalar[0]=offset48            out: scalar[0]=value
    kUFOhciWriteQuadlet = 1,  // in:  scalar[0]=offset48, [1]=value out: (none)
    kUFOhciWriteBlock   = 2,  // in:  scalar[0]=offset48, structureInput = the quadlets
    kUFOhciIsoStart     = 3,  // in:  scalar[0]=iso channel, [1]=packets to capture
    kUFOhciIsoPoll      = 4,  // out: scalar[0]=packets received so far
    kUFOhciIsoStop      = 5,
    kUFOhciIsoTxStart   = 6,  // in: scalar[0]=iso channel, [1]=payload bytes per packet
    kUFOhciIsoTxStop    = 7,
    kUFOhciIsoStartCont = 8,  // in: scalar[0]=channel   -- continuous capture ring
    kUFOhciIsoCompleted = 9,  // out: scalar[0]=monotonic total packets written by the controller
    kUFOhciIsoRelease   = 10, // in: scalar[0]=release packets up to this monotonic count
    kUFOhciMidiInEnable = 11, // in: scalar[0]=high-address index
    // out: scalar[0]=count (0..8), scalar[1]=that many MIDI bytes packed low-byte-first.
    // Scalars rather than a structure output because MIDI is slow — 31250 baud is ~3 bytes per
    // millisecond, so eight per poll is several ticks of headroom — and a fixed-size scalar reply
    // avoids the OSData lifetime dance a structure output needs on the DriverKit side.
    kUFOhciMidiInPoll   = 12,
    kUFOhciMidiInDisable= 13,
    kUFOhciReadCycleTimer = 14, // out: scalar[0]=OHCI IsochronousCycleTimer (bus clock)
    kUFOhciIsoTxSent    = 15, // out: scalar[0]=monotonic total packets the controller has transmitted
    kUFOhciIsoTxRefill  = 16, // in:  scalar[0]=the daemon has rewritten tx slots up to this count
    // One round trip for a whole pump tick. Every external method is a cross-process IPC into the
    // dext, and the pump runs at 1 kHz — doing release/refill/poll/poll separately costs 4000 round
    // trips a second, which is real CPU in exactly the loaded-DAW case we are trying to survive.
    // in: scalar[0]=release capture packets up to, [1]=tx slots refilled up to (0 = skip)
    // out: scalar[0]=capture packets completed, [1]=tx packets transmitted
    kUFOhciPump         = 17,
    kUFOhciIsoTxSetBytes = 18, // in: scalar[0]=slot, [1]=payload bytes (clock servo: 6- vs 7-frame packet)
    kUFOhciDebugInbound = 19,  // DIAGNOSTIC: log inbound async requests from the device
    // TEST HOOK: force a FireWire bus reset (PHY IBR). A bus reset is what happens in the field
    // whenever the topology changes — another device plugged in, a cable nudged, something powering
    // on — and it renumbers every node, invalidating cached node ids. Triggering it deliberately is
    // the only way to test that path without a second FireWire device to plug in.
    kUFOhciForceBusReset = 21,
    // ASYNC. Called once with IOConnectCallAsyncScalarMethod; the wake port then receives a message
    // every time the isochronous receive context completes a marked descriptor. Never completes in
    // the ordinary sense — it is a standing subscription, not a request. See the status page below.
    kUFOhciIsoWake      = 20,
    kUFOhciMethodCount
};

// Block writes carry their payload as a structure input: the conf block is 3 quadlets, but the
// fetch-PCM-frames register takes one quadlet per playback channel (28 at single speed).
#define kUFOhciMaxBlockQuadlets 32

// Isochronous capture: the dext DMAs each received packet into its own slot of a shared buffer, which
// the client maps read-only (CopyClientMemoryForType type 0). A slot holds the 4-byte isochronous
// header followed by the packet payload; the FF800's biggest packet is 28 ch x 4 B x 25 frames.
// 512 packets is ~85 ms of headroom at 48 kHz — plenty for a continuously-drained streaming ring
// (the daemon reads and releases each packet within ~1 ms). = 1 MB, down from the 48 MB the old 4 s
// one-shot WAV capture used.
#define kUFOhciIsoSlotBytes   2048
#define kUFOhciIsoMaxPackets  512
#define kUFOhciIsoBufferBytes (kUFOhciIsoSlotBytes * kUFOhciIsoMaxPackets)
#define kUFOhciIsoMemoryType  0

// Isochronous transmit (playback): a ring of 640 payload slots the daemon fills with encoded PCM,
// one per IT packet block (640 = the exact blocking-cadence period, kBlockingRingLen). The client maps it read-write (CopyClientMemoryForType type 1). 2 KB/slot
// covers the largest packet at any rate (192 kHz: 12 ch x 25 frames x 4 B = 1200 B).
#define kUFOhciTxSlotBytes    2048
#define kUFOhciTxSlots        640            // = kBlockingRingLen (exact 44.1k cadence)
#define kUFOhciTxBufferBytes  (kUFOhciTxSlotBytes * kUFOhciTxSlots)
#define kUFOhciTxMemoryType   1

// ── The status page (CopyClientMemoryForType type 2) ──────────────────────────────────────────
// Written by the dext's isochronous interrupt handler, read by the daemon's pump. It carries what
// the pump used to learn by polling: where both DMA contexts are, and — the part that cannot be
// obtained any other way — WHEN the controller got there.
//
// The timestamps are taken inside the interrupt handler, so they are the moment the DMA completed
// rather than the moment the pump happened to wake up and ask. That is the whole point: sampling
// them from the daemon adds its scheduling jitter to every clock anchor, and that jitter is what
// CoreAudio's rate estimator sees as the device's clock wobbling. It is the same trick the factory
// driver plays by taking its timestamp inside its own DMA callback.
//
// Lock-free by seqlock: the writer bumps `seq` to odd, writes, bumps to even. A reader samples seq,
// reads, and retries if seq changed or was odd. One writer (the interrupt) and one reader (the
// pump), so nothing stronger is needed.
#define kUFOhciStatusMemoryType 2

struct UFOhciStatus {
    volatile uint64_t seq;          // even = stable, odd = write in progress
    volatile uint64_t irqCount;     // isochronous completion interrupts since start (liveness)
    volatile uint64_t rxCompleted;  // monotonic capture packets the controller has written
    volatile uint64_t txSent;       // monotonic transmit packets the controller has put on the wire
    volatile uint64_t hostTime;     // mach_absolute_time() inside the interrupt handler
    volatile uint32_t cycleTimer;   // OHCI IsochronousCycleTimer at that same instant (bus clock)
    volatile uint32_t irqEvery;     // capture packets between interrupts (the wake granularity)
};

// The other direction, in the same page: the daemon's cursors, applied by the interrupt handler.
//
// These are what used to require a synchronous round trip into the dext on every pump tick, purely
// to say "I have consumed up to here, re-arm those descriptors". Handing them over through memory
// and letting the completion interrupt apply them removes the last IPC from the steady-state pump —
// wake, read this page, work the mapped buffers, write these two numbers, sleep.
//
// Safe because the interrupt is the only writer of the cursors these feed, and the polled fallback
// path (kUFOhciPump) is chosen once at daemon startup, so the two are never both live.
#define kUFOhciControlOffset 256        // separate cache line from the status block above

struct UFOhciControl {
    volatile uint64_t releaseUpTo;  // capture packets the daemon has finished reading
    volatile uint64_t txFillUpTo;   // transmit slots the daemon has rewritten
};

#define kUFOhciStatusBytes 4096

// Capture packets between completion interrupts. Each isochronous packet is one 125 us bus cycle, so
// 8 packets is a 1 ms wake — the same cadence the polling pump ran at, but phase-locked to the DMA
// instead of to a host timer. Lower means finer pacing and more interrupts; it must stay well below
// the daemon's transmit lead (kTxLeadPkts) or the pump cannot refill ahead of the transmit head.
#define kUFOhciIrqEvery 8

#endif /* UF_OHCI_SHARED_H */
