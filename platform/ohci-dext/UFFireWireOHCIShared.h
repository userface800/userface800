// UFFireWireOHCIShared.h — the user-client ABI shared by the dext and the UserFace800 daemon.
//
// The daemon (UFAsyncIO) opens an IOUserClient connection to the dext and calls these external
// methods to move register transactions to the FF800. Offsets are the full 48-bit FireWire target
// address (== uf::reg::* / uf::Addr); the dext already knows the FF800's node id + speed from the
// self-ID / config-ROM identify pass, so the daemon just passes address + value.
#ifndef UF_OHCI_SHARED_H
#define UF_OHCI_SHARED_H

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
    kUFOhciMidiInPoll   = 12, // out: structureOutput = received MIDI bytes
    kUFOhciMidiInDisable= 13,
    kUFOhciReadCycleTimer = 14, // out: scalar[0]=OHCI IsochronousCycleTimer (bus clock)
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

#endif /* UF_OHCI_SHARED_H */
