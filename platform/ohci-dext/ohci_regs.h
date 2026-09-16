// ohci_regs.h — 1394 OHCI 1.1 register offsets (BAR0 MMIO), the subset phase 0/1 needs.
// From the "1394 Open Host Controller Interface Specification, Release 1.1" (public standard) and
// cross-checked against Linux firewire-ohci (drivers/firewire/ohci.c). All registers are 32-bit,
// little-endian. Set/Clear registers come in pairs (write-1-to-set / write-1-to-clear).
#ifndef UF_OHCI_REGS_H
#define UF_OHCI_REGS_H

// Identification.
#define OHCI_Version            0x000  // [23:16]=major, [7:0]=minor; expect major 1
#define OHCI_GUID_ROM           0x004
#define OHCI_ATRetries          0x008
#define OHCI_CSRData            0x00C
#define OHCI_GUID_Hi            0x024  // bus-info GUID high quadlet (node's EUI-64 hi)
#define OHCI_GUID_Lo            0x028  // bus-info GUID low quadlet

// Host controller control (set/clear pair).
#define OHCI_HCControlSet       0x050
#define OHCI_HCControlClear     0x054
#define   HCControl_softReset       (1u << 16)
#define   HCControl_linkEnable      (1u << 17)
#define   HCControl_postedWriteEnable (1u << 18)
#define   HCControl_LPS             (1u << 19)  // Link Power Status (enable link/PHY access)
#define   HCControl_noByteSwapData  (1u << 30)
#define   HCControl_BIBimageValid   (1u << 31)

// Self-ID. SelfIDCount: [31]=error, [23:16]=generation, [10:2]=quadlet size.
#define OHCI_SelfIDBuffer       0x064
#define OHCI_SelfIDCount        0x068
#define   SelfIDCount_error         (1u << 31)
#define   SelfIDCount_size_shift    2
#define   SelfIDCount_size_mask     0x000007fc
#define   SelfIDCount_gen_shift     16
#define   SelfIDCount_gen_mask      0x00ff0000

// Interrupts (set/clear pairs).
#define OHCI_IntEventSet        0x080
#define OHCI_IntEventClear      0x084
#define OHCI_IntMaskSet         0x088
#define OHCI_IntMaskClear       0x08C
#define   Int_busReset              (1u << 17)
#define   Int_selfIDComplete        (1u << 16)
#define   Int_masterEnable          (1u << 31)
#define   Int_postedWriteErr        (1u << 8)
#define   Int_regAccessFail         (1u << 18)
#define   Int_unrecoverableError    (1u << 24)
#define   Int_cycleTooLong          (1u << 25)   // hardware clears cycleMaster; we must restore it
// Isochronous completion. These two are the logical OR of the per-context IsoXmit/IsoRecvIntEvent
// bits, so they are NOT clearable through IntEventClear — the per-context register must be cleared
// instead, or the top-level bit stays asserted and the controller re-interrupts forever.
#define   Int_isochTx               (1u << 6)
#define   Int_isochRx               (1u << 7)

// PHY access + link control.
#define OHCI_PhyControl         0x0EC
#define   PhyControl_Read(addr)     (((addr) << 8) | (1u << 15))   // rdReg | regAddr
#define   PhyControl_Write(a,d)     (((a) << 8) | (d) | (1u << 14))
#define   PhyControl_ReadDone       (1u << 31)
#define   PhyControl_ReadData(r)    (((r) >> 16) & 0xff)
#define   PhyControl_WritePending   (1u << 14)
// PHY register 1: [7]=RHB [6]=IBR (initiate bus reset) [5:0]=gap_count. Never write it blind — a
// bare IBR write also zeroes gap_count, leaving our PHY disagreeing with every other node on the
// bus about gap timing (acks then go missing).
#define   PHY_REG_RESET             1
#define   PHY_IBR                   0x40
// PHY register 1 bit 7 = RHB (Root Hold-off Bit): the node with RHB set wins root arbitration on the
// next bus reset. Setting it on OUR OWN phy is the reliable way to take root — a broadcast PHY-config
// packet naming a phy id is racy because phy ids are renumbered by every bus reset.
#define   PHY_RHB                   0x80
// PHY register 4: our self-ID advertises these. Without link_active our node claims to have no link
// layer at all, and the bus manager then treats us as unusable and force-roots someone else.
#define   PHY_REG_SELF_ID           4
#define   PHY_LINK_ACTIVE           0x80
#define   PHY_CONTENDER             0x40
#define OHCI_LinkControlSet     0x0E0
#define OHCI_LinkControlClear   0x0E4
#define   LinkControl_rcvSelfID       (1u << 9)
#define   LinkControl_rcvPhyPkt       (1u << 10)
#define   LinkControl_cycleTimerEnable (1u << 20)
#define   LinkControl_cycleMaster      (1u << 21)
#define   LinkControl_cycleSource      (1u << 22)

// ATRetries: ohci.c ohci_enable() writes 0xf | (0x2 << 4) | (0x8 << 8) | (200 << 16). Our old
// 0x888 left cycleLimit at 0, disabling the dual-phase retry timer.
#define OHCI_ATRetries_value    0x00C8082F

// Isochronous cycle timer (OHCI 1.1 §5.13): the bus clock. [31:25] cycleSeconds, [24:12] cycleCount
// (8000 Hz), [11:0] cycleOffset (24.576 MHz, wraps at 3072). Sampling this at capture-buffer
// completion recovers the FF800's audio clock against the bus clock (the Win driver's
// ISOCH_QUERY_CYCLE_TIME / the Mac driver's DCL-wrap timestamp).
#define OHCI_IsochronousCycleTimer 0x0F0

// Node identity + bus config.
#define OHCI_NodeID             0x0E8
#define   NodeID_idValid            (1u << 31)  // our node number is assigned + usable
#define   NodeID_root               (1u << 30)
#define   NodeID_nodeNumber         0x3f
#define OHCI_ConfigROMhdr       0x018
#define OHCI_BusID              0x01C
#define OHCI_BusOptions         0x020
#define OHCI_ConfigROMmap       0x034

// Request filters. After a soft reset every bit is 0, i.e. the link accepts NOTHING: inbound async
// requests (a peer reading our config ROM, for instance) are simply ignored. ohci.c opens the async
// request filter to all nodes (AsReqFilterHiSet = 0x80000000).
#define OHCI_AsReqFilterHiSet   0x100
#define OHCI_AsReqFilterHiClear 0x104
#define OHCI_AsReqFilterLoSet   0x108
#define OHCI_AsReqFilterLoClear 0x10C
#define OHCI_PhyReqFilterHiSet  0x110
#define OHCI_PhyReqFilterLoSet  0x118
#define OHCI_PhyUpperBound      0x120
#define OHCI_FairnessControl    0x0DC
#define OHCI_InitialChannelsAvailableHi 0x0B4

// Isochronous receive (OHCI 1.1 §10). One context is enough for capture.
#define OHCI_IsoRecvIntEventClear  0x0A4
#define OHCI_IsoRecvIntMaskSet     0x0A8
#define OHCI_IsoRecvIntMaskClear   0x0AC
#define OHCI_IR_CTX_BASE(n)        (0x400 + 32 * (n))   // SET=+0 CLEAR=+4 CMDPTR=+12 MATCH=+16
#define OHCI_IsoXmitIntEventClear  0x094
#define OHCI_IsoXmitIntMaskSet     0x098
#define OHCI_IsoXmitIntMaskClear   0x09C
#define OHCI_IT_CTX_BASE(n)        (0x200 + 16 * (n))   // SET=+0 CLEAR=+4 CMDPTR=+12
// IT packet header (ohci.h OHCI1394_IT_DATA_*): q0 = spd|tag|channel|tcode|sy, q1 = length<<16.
#define IT_HDR_Q0(spd, tag, ch, sy) \
    ((((spd) & 0x7) << 16) | (((tag) & 0x3) << 14) | (((ch) & 0x3f) << 8) | (0xAu << 4) | ((sy) & 0xf))
#define IT_HDR_Q1(len)             (((len) & 0xffff) << 16)
#define CTX_MATCH(base)            ((base) + 16)
#define   IR_CTX_BUFFER_FILL        0x80000000
#define   IR_CTX_ISOCH_HEADER       0x40000000          // prepend the 4-byte iso header to each packet
#define   IR_CTX_MULTI_CHANNEL      0x10000000
// ContextMatch: tags:4 (bit per tag) << 28 | sync:4 << 8 | channel:6
#define   IR_MATCH(tags, sync, ch)  (((tags) << 28) | ((sync) << 8) | ((ch) & 0x3f))

// Asynchronous request/response DMA context control (set/clear), phase 2.
#define OHCI_AsReqTrContextControlSet   0x180
#define OHCI_AsReqTrContextControlClear 0x184
#define OHCI_AsRspTrContextControlSet   0x1A0
#define OHCI_AsReqRcvContextControlSet  0x1C0
#define OHCI_AsRspRcvContextControlSet  0x1E0

#endif /* UF_OHCI_REGS_H */
