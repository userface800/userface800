// UFFireWireOHCI.cpp — OHCI-0 (bind) + OHCI-1 (reset, LPS/link enable, self-ID DMA + interrupt).
//
// Confirmed target: LSI FW643, PCI 0x11c1:0x5901, class 0x0c0010, tunneled over Thunderbolt,
// unclaimed (Tahoe has no IOFireWireFamily). Ported from Linux firewire ohci.c (GPL-2.0):
// software_reset(), ohci_enable() init sequence, and the self-ID buffer decode in bus_reset_work().
//
// REVIEW-ONLY: this compiles in an Xcode DriverKit target (CLI iig mismatches the SDK). The exact
// DriverKit DMA/interrupt call signatures (marked "// VERIFY") should be checked with the SDK; the
// OHCI logic + register sequence are the faithful part. The pure self-ID field math is the same as
// the host-tested uf::ohci::selfid (kept inline here to avoid pulling std::vector into the dext).
#include <os/log.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOService.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/IOInterruptDispatchSource.h>
#include <DriverKit/IOUserClient.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/OSAction.h>
#include <PCIDriverKit/PCIDriverKit.h>

#include "UFFireWireOHCI.h"  // generated from the .iig by iig
#include "ohci_regs.h"
#include "ohci_dma.h"
#include "ohci_async.h"
#include "UFFireWireOHCIShared.h"
// Portable identity logic (pure cstdint, no std containers — dext-safe). Xcode target needs the
// repo include/ on its header search path.
#include "uf/ohci/configrom.hpp"
#include "uf/protocol/channels.hpp"   // is_full_packet / blocking cadence

struct UFFireWireOHCI_IVars;
static void ohci_identify(UFFireWireOHCI_IVars* v, uint16_t node, uint32_t speed);
static kern_return_t uf_send_phy_config(UFFireWireOHCI_IVars* v, uint8_t root_phy_id);
// Defined down with the isochronous contexts, whose ring constants it needs; called from the
// interrupt handler well above them.
static void uf_iso_completion(UFFireWireOHCI_IVars* v);
static void uf_it_refill(UFFireWireOHCI_IVars* v, uint64_t upTo);
static constexpr uint32_t kIRContext = 0;   // the one isochronous receive context we use

static constexpr uint32_t kSelfIDBufferBytes = 2048;  // OHCI self-ID buffer, 2 KB aligned
static constexpr uint16_t kARBufferBytes    = 4096;  // AR response receive buffer

struct UFFireWireOHCI_IVars {
    IOPCIDevice*               pci;
    uint8_t                    barIndex;
    IOBufferMemoryDescriptor*  selfIdBuf;
    IODMACommand*              selfIdDMA;
    IOMemoryMap*               selfIdMap;
    uint64_t                   selfIdDeviceAddr;   // IOVA the controller writes self-IDs to
    uint32_t*                  selfIdCPU;          // CPU view of the same buffer
    IOInterruptDispatchSource* intSource;
    OSAction*                  intAction;
    IODispatchQueue*           queue;
    uint32_t                   generation;

    // OHCI-2 async transmit/receive-response contexts (control-rate register I/O).
    IOBufferMemoryDescriptor*  atBuf;      // AT request descriptor block (4 descriptors + payload)
    IODMACommand*              atDMA;
    IOMemoryMap*               atMap;
    uint32_t*                  atCPU;
    uint64_t                   atAddr;     // device address of the AT block
    IOBufferMemoryDescriptor*  arBuf;      // AR response receive DATA buffer
    IODMACommand*              arDMA;
    IOMemoryMap*               arMap;
    uint32_t*                  arCPU;
    uint64_t                   arAddr;
    // OHCI-3 isochronous receive: one context, a slot per packet, mapped read-only by the client.
    IOBufferMemoryDescriptor*  isoBuf;
    IODMACommand*              isoDMA;
    IOMemoryMap*               isoMap;
    uint8_t*                   isoCPU;
    uint64_t                   isoAddr;
    IOBufferMemoryDescriptor*  isoDescBuf;
    IODMACommand*              isoDescDMA;
    IOMemoryMap*               isoDescMap;
    uint32_t*                  isoDescCPU;
    uint64_t                   isoDescAddr;
    uint32_t                   isoPackets;   // one-shot: how many packets this run should capture
    bool                       isoRunning;
    bool                       isoContinuous;
    uint64_t                   isoWriteCursor;   // monotonic packets completed (continuous mode)
    uint64_t                   isoReadCursor;    // monotonic packets the daemon has released

    // OHCI-3 isochronous transmit: a ring of silent packets, so the device sees an incoming stream.
    IOBufferMemoryDescriptor*  itDescBuf;
    IODMACommand*              itDescDMA;
    IOMemoryMap*               itDescMap;
    uint32_t*                  itDescCPU;
    uint64_t                   itDescAddr;
    IOBufferMemoryDescriptor*  itPayBuf;    // one zeroed payload, shared by every packet
    IODMACommand*              itPayDMA;
    IOMemoryMap*               itPayMap;
    uint64_t                   itPayAddr;
    bool                       itRunning;
    uint64_t                   itSentCursor;   // monotonic packets the controller has transmitted
    uint64_t                   itFillCursor;   // monotonic packets the daemon has refilled
    uint32_t                   itFullCount;    // full packets per ring lap (the blocking cadence)

    // Interrupt-driven pump: the status page the isoch completion handler publishes into, and the
    // client + action it signals afterwards (see UFFireWireOHCIShared.h).
    IOBufferMemoryDescriptor*  statusBuf;
    IOMemoryMap*               statusMap;
    struct UFOhciStatus*       status;
    IOUserClient*              wakeClient;
    OSAction*                  wakeAction;
    uint32_t                   isoIrqEvery;    // capture packets between completion interrupts
    uint64_t                   isoIrqCount;

    IOBufferMemoryDescriptor*  payloadBuf; // AT block-request payload (device reads it by DMA)
    IODMACommand*              payloadDMA;
    IOMemoryMap*               payloadMap;
    uint32_t*                  payloadCPU;
    uint64_t                   payloadAddr;
    IOBufferMemoryDescriptor*  arReqBuf;   // AR REQUEST receive buffer (inbound requests: peers
    IODMACommand*              arReqDMA;   // reading OUR config ROM land here)
    IOMemoryMap*               arReqMap;
    uint32_t*                  arReqCPU;
    uint64_t                   arReqAddr;
    IOBufferMemoryDescriptor*  arReqDescBuf;
    IODMACommand*              arReqDescDMA;
    IOMemoryMap*               arReqDescMap;
    uint32_t*                  arReqDescCPU;
    uint64_t                   arReqDescAddr;
    bool                       isRoot;
    bool                       forcedFF800Root;   // sent the force-root PHY config once (no reset storm)
    IOBufferMemoryDescriptor*  romBuf;     // the config ROM we publish to the bus (big-endian image)
    IODMACommand*              romDMA;
    IOMemoryMap*               romMap;
    uint32_t*                  romCPU;
    uint64_t                   romAddr;
    uint32_t                   romHeader;  // host-order ROM header, written after self-ID
    IOBufferMemoryDescriptor*  arDescBuf;  // AR INPUT descriptor (controller reads this)
    IODMACommand*              arDescDMA;
    IOMemoryMap*               arDescMap;
    uint32_t*                  arDescCPU;
    uint64_t                   arDescAddr;
    uint8_t                    tlabel;     // rolling transaction label

    // The identified FF800 (from self-ID + config-ROM), the target for user-client register ops.
    bool                       ff800Found;
    bool                       ff800Cmc;      // FF800 advertises cycle-master-capable (bus-info cmc bit)
    uint16_t                   ff800Node;
    uint32_t                   ff800Speed;
    uint16_t                   localNodeId;   // our own node id (0xffc0 | phy), from self-ID

    // MIDI in (device->host async writes land in the AR-request context).
    uint16_t                   midiHighIndex; // the high-address index we advertised to the FF800
    bool                       midiInOn;
    uint32_t                   arReqReadBytes; // how far we have parsed the AR-request buffer
    uint32_t                   dbgArReadBytes; // DIAGNOSTIC: separate cursor for the inbound-request logger

    // Power management. `started` says Start() has finished, so the buffers uf_hw_bring_up() reads
    // out of ivars actually exist. `wasPoweredDown` distinguishes a WAKE from the initial power-on:
    // only a wake needs the controller rebuilt, because the initial one is Start()'s own job.
    bool                       started;
    bool                       wasPoweredDown;
};

// ── The sleep gate ────────────────────────────────────────────────────────────────────────────
// macOS powers the Thunderbolt/PCIe bridge down when the system sleeps. Touching a BAR whose device
// has gone away is a PCIe bus error, and on Apple Silicon that is an immediate kernel panic — not an
// error return, not a 0xffffffff read. It is how this driver killed the machine twice on lid close:
// the faulting read was OHCI_IntEventClear (0x084), the first register InterruptOccurred reads.
//
// The gate lives in the two accessors below rather than in the handful of paths anyone thought to
// check, because EVERY MMIO access in this driver funnels through them. That matters: the interrupt
// handler is only one offender. The daemon polls IsoTxSent/IsoTxRefill hundreds of times a second
// through the user client, on a different thread, and each of those reads registers too — gating
// only the interrupt would have left the far busier path still able to panic the machine.
//
// Relaxed atomics are enough. The flag goes false in SetPowerState(Off), which the system calls
// BEFORE it cuts power, and true in SetPowerState(On) after power is back — so a racing reader that
// sees a stale `true` is still talking to a device that is genuinely powered. What must never happen
// is a reader seeing a stale `true` *after* power is gone, and the SetPowerState ordering below
// (quiesce on the interrupt queue, then return) is what rules that out.
static bool g_powered = true;
static inline bool ohci_powered(void)  { return __atomic_load_n(&g_powered, __ATOMIC_RELAXED); }
static inline void ohci_set_powered(bool on) { __atomic_store_n(&g_powered, on, __ATOMIC_RELAXED); }

// ── Register access (OHCI BAR0 MMIO) — reg_read/reg_write equivalents ─────────────────────────
// A read while powered down returns all-ones, which is what a real absent device reads as, so
// callers that already treat 0xffffffff as "not there" (InterruptOccurred does) behave sensibly.
static inline uint32_t reg_read(IOPCIDevice* pci, uint8_t bar, uint32_t off) {
    if (!ohci_powered()) return 0xffffffffu;
    uint32_t v = 0;
    pci->MemoryRead32(bar, off, &v);
    return v;
}
static inline void reg_write(IOPCIDevice* pci, uint8_t bar, uint32_t off, uint32_t v) {
    if (!ohci_powered()) return;
    pci->MemoryWrite32(bar, off, v);
}

// IEEE-1212 config-ROM block CRC (Linux fw_compute_block_crc uses crc_itu_t = CRC-16-CCITT, poly
// 0x1021, init 0, computed over the big-endian bytes of the block). A ROM reader that verifies the
// CRC — which a full 1394 stack does — rejects a block whose stored CRC is wrong, so our old ROM
// (CRC left 0) reads as invalid. `quads` are host-order; the CRC runs over their bus-order bytes.
static uint16_t uf_rom_crc16(const uint32_t* quads, int len)
{
    uint16_t crc = 0;
    for (int i = 0; i < len; ++i) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            crc ^= (uint16_t)((quads[i] >> shift) & 0xff) << 8;
            for (int b = 0; b < 8; ++b)
                crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

bool UFFireWireOHCI::init()
{
    if (!super::init()) return false;
    ivars = IONewZero(UFFireWireOHCI_IVars, 1);
    return ivars != nullptr;
}

void UFFireWireOHCI::free()
{
    if (ivars) {
        // Every DriverKit object below was created with a +1 retain that is ours to drop. Leaking
        // them keeps the user server alive, so the dext process outlives its Stop() and a replacing
        // upgrade hangs in "terminating for upgrade" forever.
        OSSafeReleaseNULL(ivars->intAction);
        OSSafeReleaseNULL(ivars->intSource);
        OSSafeReleaseNULL(ivars->queue);
        OSSafeReleaseNULL(ivars->selfIdDMA);
        OSSafeReleaseNULL(ivars->selfIdMap);
        OSSafeReleaseNULL(ivars->selfIdBuf);
        OSSafeReleaseNULL(ivars->atDMA);
        OSSafeReleaseNULL(ivars->atMap);
        OSSafeReleaseNULL(ivars->atBuf);
        OSSafeReleaseNULL(ivars->arDMA);
        OSSafeReleaseNULL(ivars->arMap);
        OSSafeReleaseNULL(ivars->arBuf);
        OSSafeReleaseNULL(ivars->itDescDMA);
        OSSafeReleaseNULL(ivars->itDescMap);
        OSSafeReleaseNULL(ivars->itDescBuf);
        OSSafeReleaseNULL(ivars->itPayDMA);
        OSSafeReleaseNULL(ivars->itPayMap);
        OSSafeReleaseNULL(ivars->itPayBuf);
        OSSafeReleaseNULL(ivars->isoDMA);
        OSSafeReleaseNULL(ivars->isoMap);
        OSSafeReleaseNULL(ivars->isoBuf);
        OSSafeReleaseNULL(ivars->isoDescDMA);
        OSSafeReleaseNULL(ivars->isoDescMap);
        OSSafeReleaseNULL(ivars->isoDescBuf);
        OSSafeReleaseNULL(ivars->payloadDMA);
        OSSafeReleaseNULL(ivars->payloadMap);
        OSSafeReleaseNULL(ivars->payloadBuf);
        OSSafeReleaseNULL(ivars->arReqDMA);
        OSSafeReleaseNULL(ivars->arReqMap);
        OSSafeReleaseNULL(ivars->arReqBuf);
        OSSafeReleaseNULL(ivars->arReqDescDMA);
        OSSafeReleaseNULL(ivars->arReqDescMap);
        OSSafeReleaseNULL(ivars->arReqDescBuf);
        OSSafeReleaseNULL(ivars->romDMA);
        OSSafeReleaseNULL(ivars->romMap);
        OSSafeReleaseNULL(ivars->romBuf);
        OSSafeReleaseNULL(ivars->arDescDMA);
        OSSafeReleaseNULL(ivars->arDescMap);
        OSSafeReleaseNULL(ivars->arDescBuf);
        OSSafeReleaseNULL(ivars->wakeAction);
        OSSafeReleaseNULL(ivars->wakeClient);
        OSSafeReleaseNULL(ivars->statusMap);
        OSSafeReleaseNULL(ivars->statusBuf);
        IOSafeDeleteNULL(ivars, UFFireWireOHCI_IVars, 1);
    }
    super::free();
}

// PHY register access via PhyControl (OHCI 1.1 §5.10), as ohci.c read_phy_reg/write_phy_reg.
static kern_return_t phy_read(IOPCIDevice* pci, uint8_t bar, uint8_t addr, uint8_t* out)
{
    reg_write(pci, bar, OHCI_PhyControl, PhyControl_Read(addr));
    for (int i = 0; i < 100; ++i) {
        uint32_t val = reg_read(pci, bar, OHCI_PhyControl);
        if (val == 0xffffffff) return kIOReturnNoDevice;
        if (val & PhyControl_ReadDone) { *out = (uint8_t)PhyControl_ReadData(val); return kIOReturnSuccess; }
        IOSleep(1);
    }
    return kIOReturnTimeout;
}

static kern_return_t phy_write(IOPCIDevice* pci, uint8_t bar, uint8_t addr, uint8_t data)
{
    reg_write(pci, bar, OHCI_PhyControl, PhyControl_Write(addr, data));
    for (int i = 0; i < 100; ++i) {
        const uint32_t val = reg_read(pci, bar, OHCI_PhyControl);
        if (val == 0xffffffff) return kIOReturnNoDevice;   // gated (asleep) or ejected
        if (!(val & PhyControl_WritePending)) return kIOReturnSuccess;
        IOSleep(1);
    }
    return kIOReturnTimeout;
}

// software_reset() — ohci.c: assert HCControl.softReset, poll until it clears (~500 ms budget).
static kern_return_t software_reset(IOPCIDevice* pci, uint8_t bar)
{
    reg_write(pci, bar, OHCI_HCControlSet, HCControl_softReset);
    for (int i = 0; i < 500; ++i) {
        uint32_t val = reg_read(pci, bar, OHCI_HCControlSet);
        if (val == 0xffffffff) return kIOReturnNoDevice;      // card ejected
        if (!(val & HCControl_softReset)) return kIOReturnSuccess;
        IOSleep(1);
    }
    return kIOReturnBusy;
}

// ── Controller bring-up ───────────────────────────────────────────────────────────────────────
// Every register the controller needs to be a working 1394 node, and nothing else. It reads the
// buffer addresses out of ivars but allocates nothing, which is exactly what makes it callable
// twice: once from Start() after the allocations, and again from SetPowerState(On) after sleep has
// wiped the controller clean while leaving our memory and DMA mappings perfectly valid.
//
// Sleep takes the lot — link, self-ID buffer address, interrupt masks, request filters, config ROM,
// PHY state and every DMA context — so this replays all of it rather than trying to guess what
// survived. It is idempotent: a soft reset is the first thing it does.
//
// The AR descriptors are rewritten each time, not just re-pointed. The controller writes status
// back into them as it consumes packets, so the copies in memory are dirty from the previous life.
static kern_return_t uf_hw_bring_up(UFFireWireOHCI_IVars* v)
{
    // D3->D0 can clear these, and without them every access below is a bus error.
    uint16_t cmd = 0;
    v->pci->ConfigurationRead16(0x04, &cmd);
    v->pci->ConfigurationWrite16(0x04, cmd | 0x0006);   // Memory Space | Bus Master

    kern_return_t ret = software_reset(v->pci, v->barIndex);
    if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: reset failed"); return ret; }

    // Enable LPS + posted writes; wait for Link Power Status to come up (ohci.c: 50 ms x3).
    reg_write(v->pci, v->barIndex, OHCI_HCControlSet, HCControl_LPS | HCControl_postedWriteEnable);
    uint32_t lps = 0;
    for (int i = 0; i < 3 && !lps; ++i) {
        IOSleep(50);
        lps = reg_read(v->pci, v->barIndex, OHCI_HCControlSet) & HCControl_LPS;
    }
    if (!lps) { os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: LPS never set"); return kIOReturnIOError; }

    // Keep quadlet data in bus order (no controller byte-swap), matching ohci.c.
    reg_write(v->pci, v->barIndex, OHCI_HCControlClear, HCControl_noByteSwapData);

    reg_write(v->pci, v->barIndex, OHCI_SelfIDBuffer, (uint32_t)v->selfIdDeviceAddr);

    // Link control: enable self-ID + phy-packet receive, cycle timer/master (ohci.c).
    reg_write(v->pci, v->barIndex, OHCI_LinkControlSet,
              LinkControl_rcvSelfID | LinkControl_rcvPhyPkt |
              LinkControl_cycleTimerEnable | LinkControl_cycleMaster);
    reg_write(v->pci, v->barIndex, OHCI_ATRetries, OHCI_ATRetries_value);

    reg_write(v->pci, v->barIndex, OHCI_IntEventClear, ~0u);
    reg_write(v->pci, v->barIndex, OHCI_IntMaskClear, ~0u);

    // AR response context: one INPUT_MORE|STATUS descriptor branching to itself.
    if (v->arDescCPU) {
        struct ohci_descriptor* ard = (struct ohci_descriptor*)v->arDescCPU;
        ard->req_count      = kARBufferBytes;
        ard->control        = (uint16_t)(DESC_INPUT_MORE | DESC_STATUS | DESC_BRANCH_ALWAYS);
        ard->data_address   = (uint32_t)v->arAddr;
        ard->branch_address = (uint32_t)v->arDescAddr | 1;   // loop to self, Z=1
        ard->res_count = 0; ard->transfer_status = 0;
        reg_write(v->pci, v->barIndex, CTX_COMMAND_PTR(OHCI_AR_RSP_BASE),
                  (uint32_t)v->arDescAddr | 1);
        reg_write(v->pci, v->barIndex, CTX_CONTROL_SET(OHCI_AR_RSP_BASE), CTX_RUN);
    }
    // AR request context: same shape, for inbound requests (the FF800 probing us).
    if (v->arReqDescCPU) {
        struct ohci_descriptor* qd = (struct ohci_descriptor*)v->arReqDescCPU;
        qd->req_count      = kARBufferBytes;
        qd->control        = (uint16_t)(DESC_INPUT_MORE | DESC_STATUS | DESC_BRANCH_ALWAYS);
        qd->data_address   = (uint32_t)v->arReqAddr;
        qd->branch_address = (uint32_t)v->arReqDescAddr | 1;
        // res_count is the space REMAINING, which the controller decrements as it fills — so it
        // starts at the full buffer size (Linux ohci.c: d->res_count = cpu_to_le16(PAGE_SIZE)).
        // Starting it at 0 makes `written = kARBufferBytes - res_count` read 4096 forever, i.e. the
        // buffer always looks completely full, and the parser walks 4 KB of stale memory as packets.
        qd->res_count = kARBufferBytes; qd->transfer_status = 0;
        v->arReqReadBytes = 0; v->dbgArReadBytes = 0;
        reg_write(v->pci, v->barIndex, CTX_COMMAND_PTR(OHCI_AR_REQ_BASE),
                  (uint32_t)v->arReqDescAddr | 1);
        reg_write(v->pci, v->barIndex, CTX_CONTROL_SET(OHCI_AR_REQ_BASE), CTX_RUN);
    }

    // Our self-ID must advertise a link layer (and that we can contend for IRM). Without link_active
    // the bus manager reads us as link-off, decides the root is unusable and force-roots someone else
    // with a PHY config packet + bus reset — a reset storm our transmits then die in.
    uint8_t phy4 = 0;
    if (phy_read(v->pci, v->barIndex, PHY_REG_SELF_ID, &phy4) == kIOReturnSuccess)
        phy_write(v->pci, v->barIndex, PHY_REG_SELF_ID,
                  (uint8_t)(phy4 | PHY_LINK_ACTIVE | PHY_CONTENDER));

    // Republish the config ROM. See Start()'s original comment for why every block carries a CRC and
    // why max_rom=2 matters; the GUID is re-read from the controller rather than cached, because it
    // is a hardware register and survives the reset either way.
    if (v->romCPU) {
        const uint32_t guidHi = reg_read(v->pci, v->barIndex, OHCI_GUID_Hi);
        const uint32_t guidLo = reg_read(v->pci, v->barIndex, OHCI_GUID_Lo);
        uint32_t busOptions = reg_read(v->pci, v->barIndex, OHCI_BusOptions);
        busOptions |=  (1u << 30) | (1u << 29) | (2u << 8);   // cmc | isc | max_rom=2
        busOptions &= ~((1u << 31) | (1u << 28));             // irmc | bmc off

        uint32_t rom[7];
        rom[1] = 0x31333934;               // "1394"
        rom[2] = busOptions;
        rom[3] = guidHi;
        rom[4] = guidLo;
        const uint16_t bibCrc = uf_rom_crc16(&rom[1], 4);
        rom[0] = (4u << 24) | (4u << 16) | bibCrc;
        rom[6] = 0x0c0083c0;               // Node_Capabilities (immediate root-dir entry)
        const uint16_t rootCrc = uf_rom_crc16(&rom[6], 1);
        rom[5] = (1u << 16) | rootCrc;
        v->romHeader = rom[0];
        for (unsigned i = 0; i < 7; ++i) v->romCPU[i] = __builtin_bswap32(rom[i]);

        // ohci.c writes a zero header until self-ID completes, so a peer never reads a ROM that is
        // about to change generation underneath it.
        reg_write(v->pci, v->barIndex, OHCI_ConfigROMhdr, 0);
        reg_write(v->pci, v->barIndex, OHCI_BusOptions, busOptions);
        reg_write(v->pci, v->barIndex, OHCI_ConfigROMmap, (uint32_t)v->romAddr);
    }

    reg_write(v->pci, v->barIndex, OHCI_FairnessControl, 0);
    reg_write(v->pci, v->barIndex, OHCI_PhyUpperBound, 0x00010000);

    // Accept asynchronous requests from every node — after the soft reset this filter is all zeros,
    // so the link silently drops every inbound request (including reads of the ROM we just built).
    reg_write(v->pci, v->barIndex, OHCI_AsReqFilterHiSet, 0x80000000);

    // Enable the interrupts we handle + master enable, then bring the link up. isochRx is what makes
    // the daemon's pump interrupt-driven rather than a 1 ms poll; isochTx is enabled only so a stray
    // transmit completion gets acknowledged.
    reg_write(v->pci, v->barIndex, OHCI_IntMaskSet,
              Int_busReset | Int_selfIDComplete | Int_postedWriteErr | Int_regAccessFail |
              Int_unrecoverableError | Int_cycleTooLong | Int_isochRx | Int_isochTx |
              Int_masterEnable);
    reg_write(v->pci, v->barIndex, OHCI_HCControlSet,
              HCControl_linkEnable | HCControl_BIBimageValid);

    // Trigger a bus reset so we receive self-IDs. Read-modify-write: PHY reg 1 holds gap_count in its
    // low 6 bits, so writing a bare IBR would reset our gap count to 0 while the rest of the bus keeps
    // the default 63 — the two ends then disagree on gap timing and our requests are transmitted but
    // never acked. A link soft-reset does NOT reset the PHY, so a burned-in gap_count survives.
    uint8_t phy1 = 0;
    ret = phy_read(v->pci, v->barIndex, PHY_REG_RESET, &phy1);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: PHY read failed (0x%x)", ret);
        return ret;
    }
    phy_write(v->pci, v->barIndex, PHY_REG_RESET,
              (uint8_t)((phy1 & ~0x3f) | 0x3f | PHY_IBR));
    return kIOReturnSuccess;
}

kern_return_t
IMPL(UFFireWireOHCI, Start)
{
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) return ret;

    ivars->pci = OSDynamicCast(IOPCIDevice, provider);
    if (!ivars->pci) { Stop(provider, SUPERDISPATCH); return kIOReturnNoDevice; }

    ret = ivars->pci->Open(this, 0);
    if (ret != kIOReturnSuccess) { Stop(provider, SUPERDISPATCH); return ret; }

    // Enable Memory Space + Bus Master (PCI command register, config offset 0x04).
    uint16_t cmd = 0;
    ivars->pci->ConfigurationRead16(0x04, &cmd);
    ivars->pci->ConfigurationWrite16(0x04, cmd | 0x0006);
    ivars->barIndex = 0;  // BAR0 = OHCI register space. VERIFY with GetBARInfo if needed.

    // ── OHCI-0: identify ──────────────────────────────────────────────────────────────────────
    uint32_t version = reg_read(ivars->pci, ivars->barIndex, OHCI_Version);
    uint32_t guidHi  = reg_read(ivars->pci, ivars->barIndex, OHCI_GUID_Hi);
    uint32_t guidLo  = reg_read(ivars->pci, ivars->barIndex, OHCI_GUID_Lo);
    os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: OHCI Version=0x%08x (major %u) GUID=%08x%08x",
           version, (version >> 16) & 0xff, guidHi, guidLo);

    // ── OHCI-1: allocate first, then program ──────────────────────────────────────────────────
    // Everything below this point that touches a register now lives in uf_hw_bring_up(), called once
    // the allocations are done. Allocation touches no controller state, so the order is safe — and
    // the split is what lets SetPowerState(On) replay the programming after sleep without
    // reallocating buffers the daemon still has mapped.

    // ── Allocate the self-ID receive DMA buffer (2 KB, controller-writable) ───────────────────
    // VERIFY DriverKit signatures against the SDK; this is the standard IOBufferMemoryDescriptor +
    // IODMACommand pattern.
    ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, kSelfIDBufferBytes,
                                           kSelfIDBufferBytes /*alignment*/, &ivars->selfIdBuf);
    if (ret != kIOReturnSuccess) return ret;

    ret = ivars->selfIdBuf->CreateMapping(0, 0, 0, 0, 0, &ivars->selfIdMap);
    if (ret != kIOReturnSuccess) return ret;
    ivars->selfIdCPU = reinterpret_cast<uint32_t*>(ivars->selfIdMap->GetAddress());

    IODMACommandSpecification spec = {};
    spec.maxAddressBits = 32;  // OHCI SelfIDBuffer is a 32-bit device address
    ret = IODMACommand::Create(ivars->pci, 0, &spec, &ivars->selfIdDMA);
    if (ret != kIOReturnSuccess) return ret;

    uint64_t dmaFlags = 0;
    uint32_t segCount = 1;
    IOAddressSegment seg = {};
    ret = ivars->selfIdDMA->PrepareForDMA(0, ivars->selfIdBuf, 0, kSelfIDBufferBytes,
                                          &dmaFlags, &segCount, &seg);
    if (ret != kIOReturnSuccess || segCount != 1) return kIOReturnNoMemory;
    ivars->selfIdDeviceAddr = seg.address;

    // The status page the isochronous interrupt publishes into. Plain host memory the client maps —
    // the controller never touches it, so it needs no IODMACommand.
    ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, kUFOhciStatusBytes, 8,
                                           &ivars->statusBuf);
    if (ret != kIOReturnSuccess) return ret;
    ret = ivars->statusBuf->CreateMapping(0, 0, 0, 0, 0, &ivars->statusMap);
    if (ret != kIOReturnSuccess) return ret;
    ivars->status = reinterpret_cast<struct UFOhciStatus*>(ivars->statusMap->GetAddress());
    // The whole page, not just the status struct: the control block further in is read by the
    // interrupt handler, and uninitialised memory there would be applied as a cursor.
    memset(ivars->status, 0, kUFOhciStatusBytes);

    // ── Wire the MSI interrupt (IOInterruptDispatchSource) ────────────────────────────────────
    ret = IODispatchQueue::Create("UFFireWireOHCI.irq", 0, 0, &ivars->queue);
    if (ret != kIOReturnSuccess) return ret;
    // interruptIndex 0 = the device's first (MSI) interrupt. VERIFY the index for this device.
    ret = IOInterruptDispatchSource::Create(ivars->pci, 0, ivars->queue, &ivars->intSource);
    if (ret != kIOReturnSuccess) return ret;
    ret = CreateActionInterruptOccurred(0, &ivars->intAction);
    if (ret != kIOReturnSuccess) return ret;
    ret = ivars->intSource->SetHandler(ivars->intAction);
    if (ret != kIOReturnSuccess) return ret;
    ivars->intSource->SetEnable(true);

    // ── OHCI-2: allocate the AT request block + AR response buffer, run the AR context ────────
    // REVIEW-ONLY + SIMPLIFIED (control-rate). Mirrors the self-ID DMA pattern above.
    {
        uint64_t f = 0; uint32_t n = 1; IOAddressSegment s = {};
        // AT block: 4 descriptors (64 B) + small inline payload headroom, 16-byte aligned.
        IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, 256, 16, &ivars->atBuf);
        ivars->atBuf->CreateMapping(0, 0, 0, 0, 0, &ivars->atMap);
        ivars->atCPU = reinterpret_cast<uint32_t*>(ivars->atMap->GetAddress());
        IODMACommandSpecification atspec = {}; atspec.maxAddressBits = 32;
        IODMACommand::Create(ivars->pci, 0, &atspec, &ivars->atDMA);
        ivars->atDMA->PrepareForDMA(0, ivars->atBuf, 0, 256, &f, &n, &s);
        ivars->atAddr = s.address;

        // Payload for block requests: the controller DMAs it out as the packet body.
        f = 0; n = 1; s = {};
        IOBufferMemoryDescriptor::Create(kIOMemoryDirectionOut, 256, 16, &ivars->payloadBuf);
        ivars->payloadBuf->CreateMapping(0, 0, 0, 0, 0, &ivars->payloadMap);
        ivars->payloadCPU = reinterpret_cast<uint32_t*>(ivars->payloadMap->GetAddress());
        IODMACommandSpecification pspec = {}; pspec.maxAddressBits = 32;
        IODMACommand::Create(ivars->pci, 0, &pspec, &ivars->payloadDMA);
        ivars->payloadDMA->PrepareForDMA(0, ivars->payloadBuf, 0, 256, &f, &n, &s);
        ivars->payloadAddr = s.address;

        // AR response receive buffer + a single INPUT_MORE descriptor at its head.
        f = 0; n = 1; s = {};
        IOBufferMemoryDescriptor::Create(kIOMemoryDirectionIn, kARBufferBytes, 16, &ivars->arBuf);
        ivars->arBuf->CreateMapping(0, 0, 0, 0, 0, &ivars->arMap);
        ivars->arCPU = reinterpret_cast<uint32_t*>(ivars->arMap->GetAddress());
        IODMACommandSpecification arspec = {}; arspec.maxAddressBits = 32;
        IODMACommand::Create(ivars->pci, 0, &arspec, &ivars->arDMA);
        ivars->arDMA->PrepareForDMA(0, ivars->arBuf, 0, kARBufferBytes, &f, &n, &s);
        ivars->arAddr = s.address;

        // AR response context: a single INPUT_MORE|STATUS descriptor that branches to itself, so the
        // controller DMAs each response packet into arBuf. SIMPLIFIED (control-rate): one response is
        // read out before the next request is issued; the full ohci.c AR ring (multi-page wrap +
        // per-buffer trailer/status advance) is the robust version for OHCI-3.
        f = 0; n = 1; s = {};
        IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, 64, 16, &ivars->arDescBuf);
        ivars->arDescBuf->CreateMapping(0, 0, 0, 0, 0, &ivars->arDescMap);
        ivars->arDescCPU = reinterpret_cast<uint32_t*>(ivars->arDescMap->GetAddress());
        IODMACommandSpecification adspec = {}; adspec.maxAddressBits = 32;
        IODMACommand::Create(ivars->pci, 0, &adspec, &ivars->arDescDMA);
        ivars->arDescDMA->PrepareForDMA(0, ivars->arDescBuf, 0, 64, &f, &n, &s);
        ivars->arDescAddr = s.address;

        ivars->tlabel = 0;   // the descriptor itself is written by uf_hw_bring_up()

        // Isochronous receive buffer: one slot per packet, plus its descriptor ring.
        f = 0; n = 1; s = {};
        IOBufferMemoryDescriptor::Create(kIOMemoryDirectionIn, kUFOhciIsoBufferBytes, 16,
                                         &ivars->isoBuf);
        ivars->isoBuf->CreateMapping(0, 0, 0, 0, 0, &ivars->isoMap);
        ivars->isoCPU = reinterpret_cast<uint8_t*>(ivars->isoMap->GetAddress());
        IODMACommandSpecification ispec = {}; ispec.maxAddressBits = 32;
        IODMACommand::Create(ivars->pci, 0, &ispec, &ivars->isoDMA);
        ret = ivars->isoDMA->PrepareForDMA(0, ivars->isoBuf, 0, kUFOhciIsoBufferBytes, &f, &n, &s);
        if (ret != kIOReturnSuccess || n != 1) {
            // Every descriptor's data_address is computed as isoAddr + i * slot, so a scattered
            // mapping would silently point packets at the wrong memory.
            os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: iso buffer not contiguous (ret=0x%x segs=%u)",
                   ret, n);
            return kIOReturnNoMemory;
        }
        ivars->isoAddr = s.address;

        f = 0; n = 1; s = {};
        IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut,
                                         kUFOhciIsoMaxPackets * sizeof(struct ohci_descriptor), 16,
                                         &ivars->isoDescBuf);
        ivars->isoDescBuf->CreateMapping(0, 0, 0, 0, 0, &ivars->isoDescMap);
        ivars->isoDescCPU = reinterpret_cast<uint32_t*>(ivars->isoDescMap->GetAddress());
        IODMACommandSpecification idspec = {}; idspec.maxAddressBits = 32;
        IODMACommand::Create(ivars->pci, 0, &idspec, &ivars->isoDescDMA);
        ret = ivars->isoDescDMA->PrepareForDMA(0, ivars->isoDescBuf, 0,
                                               kUFOhciIsoMaxPackets * sizeof(struct ohci_descriptor),
                                               &f, &n, &s);
        // MUST be one contiguous segment: we compute every descriptor address as base + i*stride, so a
        // scatter-gather mapping would send the controller to bogus addresses -> CONTEXT_DEAD.
        if (ret != kIOReturnSuccess || n != 1) {
            os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: isoDesc PrepareForDMA failed ret=0x%x n=%u", ret, n);
            return kIOReturnNoMemory;
        }
        ivars->isoDescAddr = s.address;

        // IT ring: kITPackets blocks of 3 descriptors. STRIDE IS 4 DESCRIPTORS (64 B), not 3 (48 B):
        // 48 does not divide the 4 KB page size, so a 48 B stride makes blocks straddle page
        // boundaries; 64 divides 4096 so no block can. The 4th descriptor is unused padding (Linux
        // pads its AT blocks the same way). Z stays 3 — only 3 descriptors per block are executed.
        f = 0; n = 1; s = {};
        IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, kUFOhciTxSlots * 4 * sizeof(struct ohci_descriptor),
                                         4096, &ivars->itDescBuf);
        ivars->itDescBuf->CreateMapping(0, 0, 0, 0, 0, &ivars->itDescMap);
        ivars->itDescCPU = reinterpret_cast<uint32_t*>(ivars->itDescMap->GetAddress());
        IODMACommandSpecification tdspec = {}; tdspec.maxAddressBits = 32;
        IODMACommand::Create(ivars->pci, 0, &tdspec, &ivars->itDescDMA);
        ret = ivars->itDescDMA->PrepareForDMA(0, ivars->itDescBuf, 0,
                                              kUFOhciTxSlots * 4 * sizeof(struct ohci_descriptor), &f, &n, &s);
        if (ret != kIOReturnSuccess || n != 1) {
            os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: itDesc PrepareForDMA failed ret=0x%x n=%u", ret, n);
            return kIOReturnNoMemory;
        }
        ivars->itDescAddr = s.address;
        os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: itDescAddr=0x%llx itDescLen=%llu (stride 64B, %u blocks)",
               ivars->itDescAddr, s.length, kUFOhciTxSlots);

        // Transmit payload RING: one slot per IT packet block, so the daemon can feed distinct PCM to
        // each cycle (not one shared silent buffer). Mapped read-write to the daemon (mem type 1).
        f = 0; n = 1; s = {};
        IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, kUFOhciTxBufferBytes, 16,
                                         &ivars->itPayBuf);
        ivars->itPayBuf->CreateMapping(0, 0, 0, 0, 0, &ivars->itPayMap);
        memset(reinterpret_cast<void*>(ivars->itPayMap->GetAddress()), 0, kUFOhciTxBufferBytes);
        IODMACommandSpecification tpspec = {}; tpspec.maxAddressBits = 32;
        IODMACommand::Create(ivars->pci, 0, &tpspec, &ivars->itPayDMA);
        ret = ivars->itPayDMA->PrepareForDMA(0, ivars->itPayBuf, 0, kUFOhciTxBufferBytes, &f, &n, &s);
        if (ret != kIOReturnSuccess || n != 1) return kIOReturnNoMemory;   // contiguous: block i @ i*slot
        ivars->itPayAddr = s.address;

        // Both AR contexts are described and started by uf_hw_bring_up(), below.

        // The AR REQUEST context, same shape. Inbound requests (a peer reading the config ROM we
        // publish) are DMA'd here; without a running context they are dropped no matter what the
        // request filter says, and the peer concludes we are not a usable node.
        f = 0; n = 1; s = {};
        IOBufferMemoryDescriptor::Create(kIOMemoryDirectionIn, kARBufferBytes, 16, &ivars->arReqBuf);
        ivars->arReqBuf->CreateMapping(0, 0, 0, 0, 0, &ivars->arReqMap);
        ivars->arReqCPU = reinterpret_cast<uint32_t*>(ivars->arReqMap->GetAddress());
        IODMACommandSpecification qspec = {}; qspec.maxAddressBits = 32;
        IODMACommand::Create(ivars->pci, 0, &qspec, &ivars->arReqDMA);
        ivars->arReqDMA->PrepareForDMA(0, ivars->arReqBuf, 0, kARBufferBytes, &f, &n, &s);
        ivars->arReqAddr = s.address;

        f = 0; n = 1; s = {};
        IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, 64, 16, &ivars->arReqDescBuf);
        ivars->arReqDescBuf->CreateMapping(0, 0, 0, 0, 0, &ivars->arReqDescMap);
        ivars->arReqDescCPU = reinterpret_cast<uint32_t*>(ivars->arReqDescMap->GetAddress());
        IODMACommandSpecification qdspec = {}; qdspec.maxAddressBits = 32;
        IODMACommand::Create(ivars->pci, 0, &qdspec, &ivars->arReqDescDMA);
        ivars->arReqDescDMA->PrepareForDMA(0, ivars->arReqDescBuf, 0, 64, &f, &n, &s);
        ivars->arReqDescAddr = s.address;
    }

    // ── Allocate the config-ROM buffer; uf_hw_bring_up() fills and publishes it ───────────────
    // The OHCI requires ConfigROMhdr + BusOptions to be valid BEFORE the link is enabled, and a peer
    // acting as bus manager reads this ROM to decide whether we are fit to be root. With no ROM it
    // concludes we are not, and resets the bus at us.
    {
        uint64_t f = 0; uint32_t n = 1; IOAddressSegment sg = {};
        ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, 1024, 16, &ivars->romBuf);
        if (ret != kIOReturnSuccess) return ret;
        ivars->romBuf->CreateMapping(0, 0, 0, 0, 0, &ivars->romMap);
        ivars->romCPU = reinterpret_cast<uint32_t*>(ivars->romMap->GetAddress());
        IODMACommandSpecification romspec = {}; romspec.maxAddressBits = 32;
        IODMACommand::Create(ivars->pci, 0, &romspec, &ivars->romDMA);
        ret = ivars->romDMA->PrepareForDMA(0, ivars->romBuf, 0, 1024, &f, &n, &sg);
        if (ret != kIOReturnSuccess || n != 1) return kIOReturnNoMemory;
        ivars->romAddr = sg.address;
    }

    // ── Everything is allocated: program the controller ───────────────────────────────────────
    ret = uf_hw_bring_up(ivars);
    if (ret != kIOReturnSuccess) return ret;

    // Only now may SetPowerState touch the hardware: everything it reads out of ivars exists.
    ivars->started = true;
    ivars->wasPoweredDown = false;

    os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: link enabled; waiting for self-ID (bus reset)");
    RegisterService();
    return kIOReturnSuccess;
}

// ── Interrupt handler: read + clear IntEvent, handle bus reset / self-ID complete ─────────────
void
IMPL(UFFireWireOHCI, InterruptOccurred)
{
    // IntEventClear reads back the MASKED event set (IntEventSet is the raw one, including events we
    // never enabled). busReset must survive until self-ID has been processed — clearing it here just
    // re-fires the interrupt (OHCI 1.1 clause 7.2.3.2), so mask it and clear it once, below.
    uint32_t event = reg_read(ivars->pci, ivars->barIndex, OHCI_IntEventClear);
    if (event == 0 || event == 0xffffffff) return;
    reg_write(ivars->pci, ivars->barIndex, OHCI_IntEventClear,
              event & ~(Int_busReset | Int_postedWriteErr));

    // busReset stays asserted for the whole reset, so clearing it just re-fires the interrupt. Mask
    // it (ohci.c does the same) and re-enable once self-ID has been processed.
    if (event & Int_busReset) {
        reg_write(ivars->pci, ivars->barIndex, OHCI_IntMaskClear, Int_busReset);
        os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: bus reset");
    }

    if (event & (Int_postedWriteErr | Int_regAccessFail | Int_unrecoverableError))
        os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: controller error event 0x%08x", event);

    // Isochronous DMA completion — the audio clock. Both bits are the OR of their per-context event
    // registers and are not clearable via IntEventClear, so the per-context registers are cleared
    // here; miss that and the controller re-interrupts on the same event forever.
    if (event & (Int_isochRx | Int_isochTx)) {
        if (event & Int_isochTx) {
            uint32_t tx = reg_read(ivars->pci, ivars->barIndex, OHCI_IsoXmitIntEventClear);
            if (tx) reg_write(ivars->pci, ivars->barIndex, OHCI_IsoXmitIntEventClear, tx);
        }
        if (event & Int_isochRx) {
            uint32_t rx = reg_read(ivars->pci, ivars->barIndex, OHCI_IsoRecvIntEventClear);
            if (rx) reg_write(ivars->pci, ivars->barIndex, OHCI_IsoRecvIntEventClear, rx);
            if (rx & (1u << kIRContext)) uf_iso_completion(ivars);
        }
    }

    if (event & Int_cycleTooLong) {
        // The hardware clears cycleMaster on this event; ohci.c puts it straight back — but only if
        // we are the root/cycle-master. When the FF800 is cycle-master this event isn't ours to fix.
        if (ivars->isRoot) {
            reg_write(ivars->pci, ivars->barIndex, OHCI_LinkControlSet, LinkControl_cycleMaster);
            os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: cycle too long, cycleMaster restored");
        }
    }

    if (event & Int_selfIDComplete) {
        uint32_t reg = reg_read(ivars->pci, ivars->barIndex, OHCI_SelfIDCount);
        if (reg & SelfIDCount_error) {
            os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: self-ID error");
            reg_write(ivars->pci, ivars->barIndex, OHCI_IntMaskSet, Int_busReset);
            return;
        }

        // Don't transmit until the controller has a valid node number, and give the bus a moment to
        // settle after the reset — a request sent too early is answered with evt_missing_ack.
        uint32_t nodeReg = 0;
        for (int i = 0; i < 50; ++i) {
            nodeReg = reg_read(ivars->pci, ivars->barIndex, OHCI_NodeID);
            if (nodeReg & NodeID_idValid) break;
            IOSleep(1);
        }
        if (!(nodeReg & NodeID_idValid)) {
            os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: node ID never became valid");
            reg_write(ivars->pci, ivars->barIndex, OHCI_IntMaskSet, Int_busReset);
            return;
        }
        bool isNewRoot = (nodeReg & NodeID_root) != 0;
        ivars->isRoot = isNewRoot;
        // The FF800 advertises cmc=0 — it can NEVER be cycle-master. So WE must be, always. Only the
        // root may be cycle-master, so if the bus handed root to the FF800 we must take root back
        // (forced below) or the bus ends up with NO cycle-master: no cycle-start packets, isoch dead,
        // and the device never publishes its tx channel ("no tx channel" = the wedge). This mirrors
        // Linux bm_work: !root_device->cmc => stand_for_root with the LOCAL node.
        reg_write(ivars->pci, ivars->barIndex, OHCI_LinkControlSet, LinkControl_cycleMaster);

        IOSleep(200);   // let the bus (and the FF800) finish coming back after the reset

        // Buffer layout (ohci.c bus_reset_work): [0]=header(generation), then each self-ID quadlet
        // stored as (id, ~id). size field is in quadlets; node count = size >> 1.
        uint32_t sizeQuads = (reg & SelfIDCount_size_mask) >> SelfIDCount_size_shift;
        uint32_t count = sizeQuads >> 1;
        ivars->generation = (ivars->selfIdCPU[0] >> 16) & 0xff;

        os_log(OS_LOG_DEFAULT,
               "UFFireWireOHCI: self-ID complete, gen=%u, %u node packet(s), NodeID=0x%08x "
               "(local phy=%u%s)",
               ivars->generation, count, nodeReg, nodeReg & NodeID_nodeNumber,
               (nodeReg & NodeID_root) ? ", root" : "");

        os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: selfID raw hdr=%08x q=%08x %08x %08x %08x",
               ivars->selfIdCPU[0], ivars->selfIdCPU[1], ivars->selfIdCPU[2],
               ivars->selfIdCPU[3], ivars->selfIdCPU[4]);
        for (uint32_t j = 0; j < count; ++j) {
            uint32_t id  = ivars->selfIdCPU[1 + 2 * j];
            uint32_t id2 = ivars->selfIdCPU[2 + 2 * j];
            if (id != ~id2) { os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: self-ID checksum bad"); break; }
            if (((id >> 30) & 0x3) != 2) continue;   // not a self-ID packet identifier
            if (id & 0x00800000) continue;           // extended (continuation) packet
            // Same field math as uf::ohci::selfid (host-tested).
            uint32_t phy_id      = (id >> 24) & 0x3f;
            bool     link_active = id & 0x00400000;
            uint32_t scode       = (id >> 14) & 0x3;
            uint32_t gap_count   = (id >> 16) & 0x3f;   // self-ID zero, bits 21:16
            bool     contender   = id & 0x00000800;
            uint16_t node_id     = 0xffc0 | phy_id;
            // Every node's gap count must agree: a mismatch makes the controller see a subaction gap
            // before the ack arrives (evt_missing_ack). core-topology.c compares exactly this field.
            os_log(OS_LOG_DEFAULT,
                   "UFFireWireOHCI:   node phy=%u node_id=0x%04x link=%d S%u gap_count=%u%s",
                   phy_id, node_id, link_active, 100u << scode, gap_count,
                   contender ? " contender" : "");
            // Identify each link-active node by reading its config ROM (skip our own controller).
            uint32_t localNodeId = nodeReg & NodeID_nodeNumber;
            ivars->localNodeId = 0xffc0 | localNodeId;
            if (link_active && phy_id != localNodeId)
                ohci_identify(ivars, node_id, scode);
        }
        // The ROM we publish is stable again for this generation (ohci.c bus_reset_work).
        reg_write(ivars->pci, ivars->barIndex, OHCI_ConfigROMhdr, ivars->romHeader);
        reg_write(ivars->pci, ivars->barIndex, OHCI_IntEventClear, Int_busReset);
        reg_write(ivars->pci, ivars->barIndex, OHCI_IntMaskSet, Int_busReset);

        // Bus management (Linux bm_work): the root must be cycle-master-capable. The FF800 is cmc=0,
        // so if the bus handed root to the FF800 there is NO cycle-master at all — no cycle-start
        // packets, isoch dead, the device never publishes its tx channel. In that case take root back
        // by force-rooting OURSELVES (PHY config naming our own phy id) and resetting once.
        if (ivars->isRoot) {
            ivars->forcedFF800Root = false;   // we hold root + cycleMaster: nothing to do, re-arm
        } else if (!ivars->forcedFF800Root) {
            ivars->forcedFF800Root = true;    // fire once per episode — no reset storm
            // Set RHB on OUR OWN phy so we win root on the next reset, then reset. (A broadcast PHY
            // config packet is racy: phy ids are renumbered by the reset, so the forced id lands on
            // the wrong node — observed as root ping-ponging between generations.)
            uint8_t phy1 = 0;
            phy_read(ivars->pci, ivars->barIndex, PHY_REG_RESET, &phy1);
            os_log(OS_LOG_DEFAULT,
                   "UFFireWireOHCI: not root and FF800 is cmc=0 -> no cycle-master; setting RHB "
                   "(phy1 0x%02x -> 0x%02x) + bus reset to take root",
                   phy1, (unsigned)((phy1 & ~0x3f) | 0x3f | PHY_RHB | PHY_IBR));
            phy_write(ivars->pci, ivars->barIndex, PHY_REG_RESET,
                      (uint8_t)((phy1 & ~0x3f) | 0x3f | PHY_RHB | PHY_IBR));
        }
    }
}

// ── OHCI-2: synchronous async transactor (control-rate register I/O) ──────────────────────────
// REVIEW-ONLY + SIMPLIFIED: one outstanding transaction at a time, poll-based completion. This is
// sufficient for UFAsyncIO's register reads/writes (low rate); isoch (OHCI-3) needs the fuller
// multi-descriptor engine. The descriptor-block construction (ohci_build_at_block) and the header
// packing below mirror the host-tested uf::ohci::async / ohci_async.h.

// Build the OHCI AT immediate header for a request (same layout as uf::ohci::build_at_header).
static unsigned uf_at_header(uint32_t* q, uint32_t tcode, uint8_t tlabel, uint16_t dest,
                             uint64_t offset, uint32_t data_or_len, uint32_t speed)
{
    q[0] = ((speed & 0x7) << 16) | ((tlabel & 0x3f) << 10) | (1u << 8) | ((tcode & 0xf) << 4);
    q[1] = ((uint32_t)dest << 16) | (uint32_t)((offset >> 32) & 0xffff);
    q[2] = (uint32_t)(offset & 0xffffffff);
    q[3] = data_or_len;
    // read-quadlet request has a 3-quadlet header; writes / blocks are 4.
    return (tcode == 0x4 /*read quadlet req*/) ? 3 : 4;
}

// Stop a context and wait for it to go idle. CommandPtr may only be written while RUN=0 — writing it
// on a running context is ignored, which silently turns every transaction after the first into a
// no-op.
static void uf_ctx_stop(UFFireWireOHCI_IVars* v, uint32_t base)
{
    reg_write(v->pci, v->barIndex, CTX_CONTROL_CLEAR(base), CTX_RUN);
    for (int i = 0; i < 100; ++i) {
        const uint32_t ctrl = reg_read(v->pci, v->barIndex, CTX_CONTROL_SET(base));
        if (ctrl == 0xffffffff) return;                    // gated (asleep) or ejected
        if (!(ctrl & CTX_ACTIVE)) break;
        IOSleep(1);
    }
    // Clear the whole control register, not just RUN: dead + the event code are sticky, and a
    // context left dead can never be revived (ohci.c context_run writes CONTROL_CLEAR = ~0).
    reg_write(v->pci, v->barIndex, CTX_CONTROL_CLEAR(base), ~0u);
}

// Re-arm the AR response context so the next response lands at the head of arBuf. The context is a
// self-looping INPUT_MORE descriptor: left running, it appends each packet further into the buffer
// (res_count shrinks), so at control rate we simply restart it per transaction and always read the
// head.
static void uf_ar_rearm(UFFireWireOHCI_IVars* v)
{
    uf_ctx_stop(v, OHCI_AR_RSP_BASE);
    for (int i = 0; i < 8; ++i) v->arCPU[i] = 0;

    struct ohci_descriptor* ard = (struct ohci_descriptor*)v->arDescCPU;
    ard->req_count       = kARBufferBytes;
    ard->res_count       = 0;            // the controller fills this in as it writes
    ard->transfer_status = 0;
    ard->control         = (uint16_t)(DESC_INPUT_MORE | DESC_STATUS | DESC_BRANCH_ALWAYS);
    ard->data_address    = (uint32_t)v->arAddr;
    ard->branch_address  = (uint32_t)v->arDescAddr | 1;

    reg_write(v->pci, v->barIndex, CTX_COMMAND_PTR(OHCI_AR_RSP_BASE),
                          (uint32_t)v->arDescAddr | 1);
    reg_write(v->pci, v->barIndex, CTX_CONTROL_SET(OHCI_AR_RSP_BASE), CTX_RUN);
}

// Submit the AT block currently in atCPU and wait for the controller to actually send it. Completion
// is the ack code the controller writes into the last descriptor's transfer_status — NOT the ACTIVE
// bit, which reads 0 both before the context starts and after it finishes.
static kern_return_t uf_at_run(UFFireWireOHCI_IVars* v, unsigned z, unsigned lastIdx)
{
    struct ohci_descriptor* atd = (struct ohci_descriptor*)v->atCPU;
    atd[lastIdx].transfer_status = 0;

    uf_ctx_stop(v, OHCI_AT_REQ_BASE);
    reg_write(v->pci, v->barIndex, CTX_COMMAND_PTR(OHCI_AT_REQ_BASE),
                          (uint32_t)v->atAddr | z);
    reg_write(v->pci, v->barIndex, CTX_CONTROL_SET(OHCI_AT_REQ_BASE), CTX_RUN);

    for (int i = 0; i < 100; ++i) {
        uint32_t ctrl = reg_read(v->pci, v->barIndex, CTX_CONTROL_SET(OHCI_AT_REQ_BASE));
        if (ctrl & CTX_DEAD) {
            os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: AT context DEAD (ctl=0x%08x xfer=0x%04x)",
                   ctrl, atd[lastIdx].transfer_status);
            return kIOReturnIOError;
        }
        uint16_t st = atd[lastIdx].transfer_status;
        if (st != 0) {
            // evt >= 0x10 are ack codes (0x11 ack_complete, 0x12 ack_pending — a read request is
            // acked pending, its response arrives separately). Below 0x10 is an error event.
            unsigned evt = st & 0x1f;
            if (evt == 0x11 || evt == 0x12) return kIOReturnSuccess;
            os_log(OS_LOG_DEFAULT,
                   "UFFireWireOHCI: AT evt=0x%02x (xfer=0x%04x) IntEvent=0x%08x NodeID=0x%08x",
                   evt, st, reg_read(v->pci, v->barIndex, OHCI_IntEventSet),
                   reg_read(v->pci, v->barIndex, OHCI_NodeID));
            return kIOReturnIOError;
        }
        IOSleep(1);
    }
    return kIOReturnTimeout;
}

// Send a PHY configuration packet (IEEE-1394 §16.3.2.1) forcing `root_phy_id` to become bus root on
// the next bus reset (R bit). We do this so the FF800 — never a FireWire clock slave (its clock refs
// are only ADAT/SPDIF/word/TCO) — becomes bus cycle-master, i.e. the 8 kHz isoch cycle is derived
// from the FF800's audio crystal. Our controller then slaves its cycle timer to the FF800's
// cycle-start packets (cycleTimerEnable, cycleMaster cleared), which phase-locks our isoch TRANSMIT
// to the device clock. Without this the transmit free-runs on our crystal and playback drifts.
// The packet rides an OHCI LINK_INTERNAL (tcode 0xE) async block: the controller
// puts header[1]/header[2] on the wire as the two phy-packet quadlets (data, ~data). T=0 here — we
// leave gap_count alone (the reset restores the default) rather than optimise it.
static kern_return_t uf_send_phy_config(UFFireWireOHCI_IVars* v, uint8_t root_phy_id)
{
    uint32_t data = ((uint32_t)(root_phy_id & 0x3f) << 24)   // [29:24] root_ID
                  | (1u << 23);                              // [23] R: set force-root on that node
    uint32_t q[3];
    q[0] = ((v->ff800Speed & 0x7) << 16) | (0xEu << 4);      // LINK_INTERNAL (phy packet) + speed
    q[1] = data;
    q[2] = ~data;
    unsigned z = ohci_build_at_block((struct ohci_descriptor*)v->atCPU, q, 3, 0, 0);   // z==2
    kern_return_t ret = uf_at_run(v, z, 0);
    os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: PHY config sent (force root=phy%u) ret=0x%x", root_phy_id, ret);
    return ret;
}

// Read a quadlet register on `dest` at 48-bit `offset`. Returns the value via `out`.
// VERIFY: AR-response parsing here reads the response header from the AR buffer start; the real AR
// context stores a trailer with ack/status and can wrap — port ar_context handling from ohci.c.
static kern_return_t uf_read_quadlet(UFFireWireOHCI_IVars* v, uint16_t dest, uint64_t offset,
                                     uint32_t speed, uint32_t* out)
{
    uint32_t q[4];
    uint8_t tl = v->tlabel++ & 0x3f;
    unsigned hq = uf_at_header(q, 0x4 /*read quadlet req*/, tl, dest, offset, 0, speed);
    unsigned z = ohci_build_at_block((struct ohci_descriptor*)v->atCPU, q, hq, 0, 0);

    // Restart the AR context so this request's response lands at the head of the buffer.
    uf_ar_rearm(v);

    kern_return_t ret = kIOReturnError;
    for (int attempt = 0; attempt < 3; ++attempt) {   // a post-reset request can miss its ack
        ret = uf_at_run(v, z, (z == 3) ? 2 : 0);
        if (ret == kIOReturnSuccess) break;
        IOSleep(10);
        uf_ar_rearm(v);
    }
    struct ohci_descriptor* atd = (struct ohci_descriptor*)v->atCPU;
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: AT failed ret=0x%x atctl=0x%08x xfer=0x%04x",
               ret, reg_read(v->pci, v->barIndex, CTX_CONTROL_SET(OHCI_AT_REQ_BASE)),
               atd[0].transfer_status);
        return ret;
    }

    for (int i = 0; i < 100; ++i) {              // await the read-response packet in the AR buffer
        if ((v->arCPU[0] >> 4 & 0xf) == 0x6 /*read quadlet resp*/ &&
            (v->arCPU[0] >> 10 & 0x3f) == tl) {
            // Each received packet is followed by a trailer quadlet holding the ack/event code. A
            // read-quadlet-response is 4 header+data quadlets, so the trailer is arCPU[4]
            // (ohci.c handle_ar_packet: status = buffer[length], evt = (status >> 16) & 0x1f).
            unsigned evt = (v->arCPU[4] >> 16) & 0x1f;
            if (evt != 0x11 /*ack_complete*/) {
                os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: AR evt=0x%02x (trailer=0x%08x)",
                       evt, v->arCPU[4]);
                return kIOReturnIOError;
            }
            uint32_t rcode = (v->arCPU[1] >> 12) & 0xf;
            if (rcode != 0) {
                os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: read resp rcode=%u (dest=0x%04x off=0x%llx)",
                       rcode, dest, offset);
                return kIOReturnIOError;
            }
            // The controller leaves the data quadlet exactly as it came off the wire (ohci.c does not
            // byte-swap header[3]). Byte order is the PROTOCOL's business, not the transport's: the
            // FF800's registers are little-endian (snd-fireface: le32_to_cpu), so on this
            // little-endian host the buffer word is already the value. The config ROM is big-endian
            // (IEEE-1212) and swaps in ohci_identify. Swapping here corrupted every register.
            *out = v->arCPU[3];
            return kIOReturnSuccess;
        }
        IOSleep(1);
    }
    // No response landed. Report what the hardware actually did: the AT descriptor's transfer_status
    // holds the ack/evt code for the request; the AR context state + buffer head say whether anything
    // was received at all.
    struct ohci_descriptor* ard = (struct ohci_descriptor*)v->arDescCPU;
    os_log(OS_LOG_DEFAULT,
           "UFFireWireOHCI: no resp (dest=0x%04x off=0x%llx spd=%u tl=%u) "
           "AT xfer=0x%04x atctl=0x%08x | AR ctl=0x%08x res=%u xfer=0x%04x ar[0..3]=%08x %08x %08x %08x",
           dest, offset, speed, tl, atd[0].transfer_status,
           reg_read(v->pci, v->barIndex, CTX_CONTROL_SET(OHCI_AT_REQ_BASE)),
           reg_read(v->pci, v->barIndex, CTX_CONTROL_SET(OHCI_AR_RSP_BASE)),
           ard->res_count, ard->transfer_status,
           v->arCPU[0], v->arCPU[1], v->arCPU[2], v->arCPU[3]);
    return kIOReturnTimeout;
}

// Write a quadlet register.
static kern_return_t uf_write_quadlet(UFFireWireOHCI_IVars* v, uint16_t dest, uint64_t offset,
                                      uint32_t speed, uint32_t value)
{
    uint32_t q[4];
    uint8_t tl = v->tlabel++ & 0x3f;
    unsigned hq = uf_at_header(q, 0x0 /*write quadlet req*/, tl, dest, offset, value, speed);
    unsigned z = ohci_build_at_block((struct ohci_descriptor*)v->atCPU, q, hq, 0, 0);
    return uf_at_run(v, z, (z == 3) ? 2 : 0);
}

// Write a block of quadlets (tcode 0x1). The FF800 wants its conf block as one block transaction.
static kern_return_t uf_write_block(UFFireWireOHCI_IVars* v, uint16_t dest, uint64_t offset,
                                    uint32_t speed, const uint32_t* quads, unsigned count)
{
    if (count == 0 || count > kUFOhciMaxBlockQuadlets) return kIOReturnBadArgument;
    const uint32_t bytes = count * 4;

    // Little-endian on the wire (snd-fireface: cpu_to_le32), i.e. straight through on this host.
    for (unsigned i = 0; i < count; ++i) v->payloadCPU[i] = quads[i];

    uint32_t q[4];
    uint8_t tl = v->tlabel++ & 0x3f;
    // For a block request the 4th header quadlet is data_length:16 | extended_tcode:16 — a header
    // field, not payload, so it is NOT byte-swapped.
    unsigned hq = uf_at_header(q, 0x1 /*write block req*/, tl, dest, offset, bytes << 16, speed);
    unsigned z = ohci_build_at_block((struct ohci_descriptor*)v->atCPU, q, hq,
                                     (uint32_t)v->payloadAddr, bytes);

    uf_ar_rearm(v);
    kern_return_t ret = kIOReturnError;
    for (int attempt = 0; attempt < 3; ++attempt) {
        ret = uf_at_run(v, z, (z == 3) ? 2 : 0);
        if (ret == kIOReturnSuccess) break;
        IOSleep(10);
        uf_ar_rearm(v);
    }
    // Only log FAILURES. This fires on every register write, which includes every MIDI-out
    // message — one unified-log line per MIDI event, written to disk, forever.
    if (ret != kIOReturnSuccess)
      os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: write_block FAILED off=0x%llx count=%u -> 0x%x",
           offset, count, ret);
    return ret;
}

kern_return_t UFFireWireOHCI::WriteBlock(uint64_t offset, const uint32_t* quads, uint32_t count)
{
    if (!ivars->ff800Found) return kIOReturnNotReady;
    return uf_write_block(ivars, ivars->ff800Node, offset, ivars->ff800Speed, quads, count);
}

// ── OHCI-3: isochronous receive ───────────────────────────────────────────────────────────────
// Packet-per-buffer mode with ISOCH_HEADER: each packet lands in its own slot, prefixed by its
// 4-byte isochronous header. The descriptor program is a straight run of `packets` INPUT_LAST
// descriptors that ends (branch = 0) rather than looping, so a capture stops by itself and the
// client can read the whole buffer without racing the controller.

kern_return_t UFFireWireOHCI::IsoStart(uint32_t channel, uint32_t packets)
{
    if (packets == 0 || packets > kUFOhciIsoMaxPackets) return kIOReturnBadArgument;
    const uint32_t base = OHCI_IR_CTX_BASE(kIRContext);

    uf_ctx_stop(ivars, base);

    struct ohci_descriptor* d = (struct ohci_descriptor*)ivars->isoDescCPU;
    for (uint32_t i = 0; i < packets; ++i) {
        d[i].req_count       = kUFOhciIsoSlotBytes;
        d[i].res_count       = 0;
        d[i].transfer_status = 0;
        d[i].control         = (uint16_t)(DESC_INPUT_LAST | DESC_STATUS | DESC_BRANCH_ALWAYS);
        d[i].data_address    = (uint32_t)(ivars->isoAddr + (uint64_t)i * kUFOhciIsoSlotBytes);
        // Chain to the next descriptor (Z=1); the last one branches nowhere, stopping the context.
        d[i].branch_address  = (i + 1 < packets)
                             ? (uint32_t)(ivars->isoDescAddr + (uint64_t)(i + 1) * sizeof(*d)) | 1
                             : 0;
    }
    ivars->isoPackets = packets;
    ivars->isoRunning = true;

    reg_write(ivars->pci, ivars->barIndex, OHCI_IsoRecvIntEventClear, 1u << kIRContext);
    // Accept every tag (the FF800's raw-quadlet stream is not CIP/AMDTP tagged like an AV/C device).
    reg_write(ivars->pci, ivars->barIndex, CTX_MATCH(base), IR_MATCH(0xf, 0, channel));
    reg_write(ivars->pci, ivars->barIndex, CTX_COMMAND_PTR(base), (uint32_t)ivars->isoDescAddr | 1);
    reg_write(ivars->pci, ivars->barIndex, CTX_CONTROL_SET(base), CTX_RUN | IR_CTX_ISOCH_HEADER);

    os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: iso capture started, channel %u, %u packets",
           channel, packets);
    return kIOReturnSuccess;
}

// How many packets have landed: a descriptor the controller has finished has a non-zero status.
kern_return_t UFFireWireOHCI::IsoPoll(uint32_t* received)
{
    struct ohci_descriptor* d = (struct ohci_descriptor*)ivars->isoDescCPU;
    uint32_t n = 0;
    for (uint32_t i = 0; i < ivars->isoPackets; ++i) {
        if (d[i].transfer_status == 0) break;
        ++n;
    }
    *received = n;

    // Is the context even running, and did the controller notice anything on the channel? RUN/ACTIVE
    // separate "our descriptor program is broken" from "the device is not transmitting".
    const uint32_t base = OHCI_IR_CTX_BASE(kIRContext);
    os_log(OS_LOG_DEFAULT,
           "UFFireWireOHCI: iso poll: got=%u ctl=0x%08x (run=%d active=%d dead=%d) "
           "match=0x%08x event=0x%08x d0.res=%u d0.xfer=0x%04x",
           n, reg_read(ivars->pci, ivars->barIndex, CTX_CONTROL_SET(base)),
           (reg_read(ivars->pci, ivars->barIndex, CTX_CONTROL_SET(base)) & CTX_RUN) != 0,
           (reg_read(ivars->pci, ivars->barIndex, CTX_CONTROL_SET(base)) & CTX_ACTIVE) != 0,
           (reg_read(ivars->pci, ivars->barIndex, CTX_CONTROL_SET(base)) & CTX_DEAD) != 0,
           reg_read(ivars->pci, ivars->barIndex, CTX_MATCH(base)),
           reg_read(ivars->pci, ivars->barIndex, OHCI_IsoRecvIntEventClear),
           d[0].res_count, d[0].transfer_status);
    return kIOReturnSuccess;
}

kern_return_t UFFireWireOHCI::IsoStop()
{
    if (!ivars->isoRunning) return kIOReturnSuccess;
    reg_write(ivars->pci, ivars->barIndex, OHCI_IsoRecvIntMaskClear, 1u << kIRContext);
    uf_ctx_stop(ivars, OHCI_IR_CTX_BASE(kIRContext));
    ivars->isoRunning = false;
    ivars->isoContinuous = false;
    os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: iso capture stopped");
    return kIOReturnSuccess;
}

// Program every slot as a self-wrapping ring (last branch -> first) so the controller never stops.
// The daemon reads completed slots and releases them; a re-armed slot's status goes back to 0.
kern_return_t UFFireWireOHCI::IsoStartContinuous(uint32_t channel)
{
    const uint32_t base = OHCI_IR_CTX_BASE(kIRContext);
    const uint32_t n = kUFOhciIsoMaxPackets;
    uf_ctx_stop(ivars, base);

    struct ohci_descriptor* d = (struct ohci_descriptor*)ivars->isoDescCPU;
    for (uint32_t i = 0; i < n; ++i) {
        d[i].req_count       = kUFOhciIsoSlotBytes;
        d[i].res_count       = 0;
        d[i].transfer_status = 0;
        // Every kUFOhciIrqEvery-th descriptor raises the completion interrupt that drives the pump.
        // Marking all of them would be 8000 interrupts a second for no extra timing information;
        // marking none is what left the daemon with nothing to block on but a timer.
        d[i].control         = (uint16_t)(DESC_INPUT_LAST | DESC_STATUS | DESC_BRANCH_ALWAYS |
                                          ((i % kUFOhciIrqEvery) == 0 ? DESC_IRQ_ALWAYS : 0));
        d[i].data_address    = (uint32_t)(ivars->isoAddr + (uint64_t)i * kUFOhciIsoSlotBytes);
        d[i].branch_address  = (uint32_t)(ivars->isoDescAddr +
                                          (uint64_t)((i + 1) % n) * sizeof(*d)) | 1;   // wrap
    }
    ivars->isoPackets     = n;
    ivars->isoWriteCursor = 0;
    ivars->isoReadCursor  = 0;
    ivars->isoRunning     = true;
    ivars->isoContinuous  = true;
    ivars->isoIrqEvery    = kUFOhciIrqEvery;

    reg_write(ivars->pci, ivars->barIndex, OHCI_IsoRecvIntEventClear, 1u << kIRContext);
    // Unmask this context's completion interrupt. The top-level Int_isochRx enabled in Start() is
    // only the OR of these per-context bits; without this one set, no isoch interrupt ever fires.
    reg_write(ivars->pci, ivars->barIndex, OHCI_IsoRecvIntMaskSet, 1u << kIRContext);
    reg_write(ivars->pci, ivars->barIndex, CTX_MATCH(base), IR_MATCH(0xf, 0, channel));
    reg_write(ivars->pci, ivars->barIndex, CTX_COMMAND_PTR(base), (uint32_t)ivars->isoDescAddr | 1);
    reg_write(ivars->pci, ivars->barIndex, CTX_CONTROL_SET(base), CTX_RUN | IR_CTX_ISOCH_HEADER);
    os_log(OS_LOG_DEFAULT,
           "UFFireWireOHCI: continuous iso capture on channel %u (%u-slot ring, irq every %u)",
           channel, n, kUFOhciIrqEvery);
    return kIOReturnSuccess;
}

// Advance the write cursor over descriptors the controller has completed (status != 0), starting at
// the current cursor. Bounded by one ring-length so we never overtake unreleased slots.
kern_return_t UFFireWireOHCI::IsoCompletedCount(uint64_t* total)
{
    struct ohci_descriptor* d = (struct ohci_descriptor*)ivars->isoDescCPU;
    const uint32_t n = ivars->isoPackets;
    while (ivars->isoWriteCursor - ivars->isoReadCursor < n) {
        const uint32_t i = (uint32_t)(ivars->isoWriteCursor % n);
        if (d[i].transfer_status == 0) break;   // controller hasn't filled this slot yet
        ++ivars->isoWriteCursor;
    }
    *total = ivars->isoWriteCursor;
    return kIOReturnSuccess;
}

// The daemon has consumed packets up to `upTo`; re-arm those descriptors for the controller to reuse.
// Split from the external method so the completion interrupt can apply the daemon's cursor straight
// from the shared control block, without a round trip.
static void uf_iso_release(UFFireWireOHCI_IVars* v, uint64_t upTo)
{
    struct ohci_descriptor* d = (struct ohci_descriptor*)v->isoDescCPU;
    const uint32_t n = v->isoPackets;
    if (!n) return;
    if (upTo > v->isoWriteCursor) upTo = v->isoWriteCursor;
    while (v->isoReadCursor < upTo) {
        const uint32_t i = (uint32_t)(v->isoReadCursor % n);
        d[i].res_count       = 0;
        d[i].transfer_status = 0;   // re-arm; the wrapping branch brings the controller back here
        ++v->isoReadCursor;
    }
}

kern_return_t UFFireWireOHCI::IsoRelease(uint64_t upTo)
{
    uf_iso_release(ivars, upTo);
    return kIOReturnSuccess;
}

// Hand the client a read-only mapping of the capture buffer.
kern_return_t UFFireWireOHCI::CopyIsoBuffer(IOMemoryDescriptor** memory)
{
    if (!ivars->isoBuf) return kIOReturnNotReady;
    ivars->isoBuf->retain();
    *memory = ivars->isoBuf;
    return kIOReturnSuccess;
}

// Hand the daemon a read-write mapping of the transmit payload ring (it fills the slots).
kern_return_t UFFireWireOHCI::CopyTxBuffer(IOMemoryDescriptor** memory)
{
    if (!ivars->itPayBuf) return kIOReturnNotReady;
    ivars->itPayBuf->retain();
    *memory = ivars->itPayBuf;
    return kIOReturnSuccess;
}

// ── OHCI-3: isochronous transmit (CIP blocking playback stream) ───────────────────────────────
// The FF800 will not start transmitting into silence and clocks its output off the stream it
// receives (ref/snd-fireface). It streams in CIP BLOCKING mode: every packet carries either
// `fullPayloadBytes` (syt_interval frames) or 0 (empty), and empty packets pad down to the rate/8000
// average. `fullPerFour` full packets out of every 4 hit that average (3 of 4 for 48/96/192 kHz).
// The ring is 64 self-looping 3-descriptor blocks; packet i transmits slot i of the payload ring.
static constexpr uint32_t kITContext = 0;
static constexpr uint32_t kITPackets = kUFOhciTxSlots;   // 640: exact blocking period

// ── The isochronous completion interrupt: the audio clock ─────────────────────────────────────
// Runs on the interrupt queue every kUFOhciIrqEvery captured packets, i.e. paced by the FF800's own
// transmit cadence rather than by a host timer. It does the pump's read half — advance both DMA
// cursors, stamp when the controller actually got there — publishes that to the status page, and
// wakes the daemon.
//
// Taking the timestamp HERE is the point of the exercise. The daemon's 1 ms polling loop learned the
// same cursors up to a tick late and with whatever jitter the scheduler added, and that jitter went
// straight into the clock anchor CoreAudio uses to estimate the device rate. This is the userland
// equivalent of the factory driver timestamping inside its own DMA callback.
static void uf_iso_completion(UFFireWireOHCI_IVars* v)
{
    if (!v->status) return;
    // Both samples first, before any scanning work, so they describe the completion rather than the
    // time we finished bookkeeping. hostTime is the CPU clock CoreAudio speaks; cycleTimer is the
    // FireWire bus clock our transmit is paced by, and the daemon's servo compares the two.
    const uint64_t host  = mach_absolute_time();
    const uint32_t cycle = reg_read(v->pci, v->barIndex, OHCI_IsochronousCycleTimer);

    // Apply the daemon's cursors from the shared control block: re-arm the capture descriptors it
    // has finished reading and hand back the transmit slots it has rewritten. Doing it here is what
    // lets the steady-state pump run without a single external method call — the round trip that
    // used to carry these was the last per-tick IPC. Done BEFORE the scans below so this interrupt
    // already reflects the work.
    {
        struct UFOhciControl* c =
            (struct UFOhciControl*)((uint8_t*)v->status + kUFOhciControlOffset);
        const uint64_t rel = c->releaseUpTo, fill = c->txFillUpTo;
        if (rel) uf_iso_release(v, rel);
        if (fill) uf_it_refill(v, fill);
    }

    if (v->isoRunning && v->isoContinuous) {
        struct ohci_descriptor* d = (struct ohci_descriptor*)v->isoDescCPU;
        const uint32_t n = v->isoPackets;
        while (v->isoWriteCursor - v->isoReadCursor < n) {
            const uint32_t i = (uint32_t)(v->isoWriteCursor % n);
            if (d[i].transfer_status == 0) break;      // controller hasn't filled this slot yet
            ++v->isoWriteCursor;
        }
    }
    if (v->itRunning) {
        struct ohci_descriptor* d = (struct ohci_descriptor*)v->itDescCPU;
        while (v->itSentCursor < v->itFillCursor) {
            const uint32_t i = (uint32_t)(v->itSentCursor % kITPackets);
            if (uf::is_full_packet(i, v->itFullCount, kITPackets) &&
                d[i * 4 + 2].transfer_status == 0)
                break;
            ++v->itSentCursor;
        }
    }

    // Seqlock publish: odd while writing, even when the payload is coherent.
    struct UFOhciStatus* s = v->status;
    s->seq = s->seq + 1;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    s->irqCount    = ++v->isoIrqCount;
    s->rxCompleted = v->isoWriteCursor;
    s->txSent      = v->itSentCursor;
    s->hostTime    = host;
    s->cycleTimer  = cycle;
    s->irqEvery    = v->isoIrqEvery;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    s->seq = s->seq + 1;

    // Wake the pump. The message is only an edge — everything it needs is in the status page above,
    // so the daemon never has to decode the async payload on its realtime thread.
    if (v->wakeClient && v->wakeAction) {
        uint64_t data[1] = { v->isoIrqCount };
        v->wakeClient->AsyncCompletion(v->wakeAction, kIOReturnSuccess, data, 1);
    }
}

kern_return_t UFFireWireOHCI::CopyStatusBuffer(IOMemoryDescriptor** memory)
{
    if (!ivars->statusBuf) return kIOReturnNotReady;
    ivars->statusBuf->retain();
    *memory = ivars->statusBuf;
    return kIOReturnSuccess;
}

// The daemon's standing subscription to the completion interrupt. One client at a time; a second
// subscribe replaces the first, which is what a daemon restart looks like from here.
kern_return_t UFFireWireOHCI::SetIsoWake(IOUserClient* client, OSAction* action)
{
    OSSafeReleaseNULL(ivars->wakeAction);
    OSSafeReleaseNULL(ivars->wakeClient);
    if (action) action->retain();
    if (client) client->retain();
    ivars->wakeAction = action;
    ivars->wakeClient = client;
    os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: iso wake %s", action ? "subscribed" : "cleared");
    return kIOReturnSuccess;
}

kern_return_t UFFireWireOHCI::IsoTxStart(uint32_t channel, uint32_t fullPayloadBytes, uint32_t fullCount)
{
    if (fullPayloadBytes == 0 || fullPayloadBytes > kUFOhciTxSlotBytes || fullCount > kITPackets)
        return kIOReturnBadArgument;
    const uint32_t base = OHCI_IT_CTX_BASE(kITContext);
    uf_ctx_stop(ivars, base);

    struct ohci_descriptor* d = (struct ohci_descriptor*)ivars->itDescCPU;
    for (uint32_t i = 0; i < kITPackets; ++i) {
        // 4-descriptor (64 B) stride so no block straddles a 4 KB page; only 3 are executed (Z=3).
        struct ohci_descriptor* b = &d[i * 4];
        const uint64_t bAddr = ivars->itDescAddr + (uint64_t)i * 4 * sizeof(*d);
        const uint64_t nAddr = ivars->itDescAddr +
                               (uint64_t)((i + 1) % kITPackets) * 4 * sizeof(*d);
        memset(b, 0, 4 * sizeof(*b));

        // Blocking cadence: Bresenham-distribute fullCount full packets across the ring.
        const bool full = uf::is_full_packet(i, fullCount, kITPackets);
        const uint32_t payloadBytes = full ? fullPayloadBytes : 0;

        // b[0]: immediate key, 8 bytes of header living in b[1]. Its branch points at itself, which
        // makes the controller skip a cycle on an underrun instead of dropping the packet (ohci.c).
        b[0].control        = (uint16_t)DESC_KEY_IMMEDIATE;
        b[0].req_count      = 8;
        b[0].branch_address = (uint32_t)bAddr | 3;

        uint32_t* hdr = (uint32_t*)&b[1];
        hdr[0] = IT_HDR_Q0(ivars->ff800Speed, 0 /*tag 0 = TAG_NO_CIP_HEADER (snd-fireface); A/B-tested vs 1, no difference*/, channel, 0 /*sy*/);
        hdr[1] = IT_HDR_Q1(payloadBytes);   // 0 for an empty (no-data) blocking packet

        // b[2]: the payload — packet i transmits slot i of the payload ring (the daemon fills these).
        b[2].control        = (uint16_t)(DESC_OUTPUT_LAST | DESC_STATUS | DESC_BRANCH_ALWAYS);
        b[2].req_count      = (uint16_t)payloadBytes;
        b[2].data_address   = (uint32_t)(ivars->itPayAddr + (uint64_t)i * kUFOhciTxSlotBytes);
        b[2].branch_address = (uint32_t)nAddr | 3;   // ring: Z=3 blocks, loops forever
    }

    reg_write(ivars->pci, ivars->barIndex, OHCI_IsoXmitIntEventClear, 1u << kITContext);
    reg_write(ivars->pci, ivars->barIndex, CTX_COMMAND_PTR(base), (uint32_t)ivars->itDescAddr | 3);
    reg_write(ivars->pci, ivars->barIndex, CTX_CONTROL_SET(base), CTX_RUN);
    ivars->itRunning    = true;
    ivars->itFullCount  = fullCount;
    // The whole ring starts filled (with silence) and status-cleared by the memset above, so the
    // daemon's fill cursor starts one full lap ahead of the transmit head. See IsoTxSent.
    ivars->itSentCursor = 0;
    ivars->itFillCursor = kITPackets;

    // Health check (no sleeps — this is on the start path). run(0x8000)/active(0x400)/DEAD(0x800) and
    // evt = ctl & 0x1f. DEAD with evt 0x6 (evt_descriptor_read) means a bad descriptor address; that
    // is what a non-contiguous descriptor mapping or a page-straddling block stride produces.
    {
        uint32_t ctl = reg_read(ivars->pci, ivars->barIndex, CTX_CONTROL_SET(base));
        os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: IT ctx ctl=0x%08x run=%d active=%d DEAD=%d evt=0x%02x",
               ctl, (ctl >> 15) & 1, (ctl >> 10) & 1, (ctl >> 11) & 1, ctl & 0x1f);
    }

    os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: iso TX started, channel %u, %u B/full packet, %u/%u full",
           channel, fullPayloadBytes, fullCount, kITPackets);
    return kIOReturnSuccess;
}

// ── Transmit pacing: where the controller actually is in the ring ─────────────────────────────
// The IT ring self-loops forever — unlike capture, the controller never waits for us, so a daemon
// that fills at its own pace eventually walks into the transmit head and the audio tears. These two
// calls are the transmit mirror of IsoCompletedCount/IsoRelease and let the daemon hold a fixed lead.
//
// Each packet's OUTPUT_LAST descriptor carries DESCRIPTOR_STATUS, so the controller writes a
// non-zero transfer_status once the packet is on the wire; ohci.c handle_it_packet polls exactly
// this field. The daemon clears it again as it refills the slot, which is what makes the *next* lap
// detectable — a status that we have not cleared is stale and says nothing about this lap, hence the
// scan never runs past the fill cursor.
//
// Invariant: itSentCursor <= itFillCursor <= itSentCursor + kITPackets, and the difference is the
// lead in packets (125 us each). Both start one lap apart because IsoTxStart leaves the entire ring
// filled with silence and status-cleared.
kern_return_t UFFireWireOHCI::IsoTxSent(uint64_t* sent)
{
    if (!ivars->itRunning) { *sent = 0; return kIOReturnNotReady; }
    struct ohci_descriptor* d = (struct ohci_descriptor*)ivars->itDescCPU;
    while (ivars->itSentCursor < ivars->itFillCursor) {
        const uint32_t i = (uint32_t)(ivars->itSentCursor % kITPackets);
        // Empty (0-byte) packets are counted as gone without consulting their status: whether the
        // controller writes a status back for a zero-length OUTPUT_LAST is not worth depending on,
        // and the following full packet pins the cursor anyway (so this overshoots by at most one).
        if (uf::is_full_packet(i, ivars->itFullCount, kITPackets) &&
            d[i * 4 + 2].transfer_status == 0)
            break;
        ++ivars->itSentCursor;
    }
    *sent = ivars->itSentCursor;
    return kIOReturnSuccess;
}

// The daemon has written payload into every slot below `upTo`; clear those descriptors' status so
// the next lap's transmit is visible. Clamped to one ring lap ahead of the transmit head.
static void uf_it_refill(UFFireWireOHCI_IVars* v, uint64_t upTo)
{
    if (!v->itRunning) return;
    struct ohci_descriptor* d = (struct ohci_descriptor*)v->itDescCPU;
    while (v->itFillCursor < upTo &&
           v->itFillCursor - v->itSentCursor < kITPackets) {
        const uint32_t i = (uint32_t)(v->itFillCursor % kITPackets);
        d[i * 4 + 2].transfer_status = 0;
        ++v->itFillCursor;
    }
}

kern_return_t UFFireWireOHCI::IsoTxRefill(uint64_t upTo)
{
    if (!ivars->itRunning) return kIOReturnNotReady;
    uf_it_refill(ivars, upTo);
    return kIOReturnSuccess;
}

// Clock servo: change one slot's transmitted payload size. Both the DMA length (b[2].req_count) and
// the isochronous header's data-length field (hdr[1]) must agree, or the controller emits a packet
// whose header and payload disagree. Only the daemon's own frame geometry decides the byte count; the
// dext just writes it. Safe because the daemon only ever calls this for a slot behind the transmit
// head (one it is refilling), exactly like IsoTxRefill.
kern_return_t UFFireWireOHCI::IsoTxSetSlotBytes(uint32_t slot, uint32_t payloadBytes)
{
    if (!ivars->itRunning) return kIOReturnNotReady;
    if (slot >= kITPackets || payloadBytes > kUFOhciTxSlotBytes) return kIOReturnBadArgument;
    struct ohci_descriptor* d = (struct ohci_descriptor*)ivars->itDescCPU;
    struct ohci_descriptor* b = &d[slot * 4];
    b[2].req_count = (uint16_t)payloadBytes;
    ((uint32_t*)&b[1])[1] = IT_HDR_Q1(payloadBytes);   // hdr[1] = length << 16
    return kIOReturnSuccess;
}

kern_return_t UFFireWireOHCI::IsoTxStop()
{
    if (!ivars->itRunning) return kIOReturnSuccess;
    uf_ctx_stop(ivars, OHCI_IT_CTX_BASE(kITContext));
    ivars->itRunning = false;
    os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: iso TX stopped");
    return kIOReturnSuccess;
}

// ── MIDI in (device -> host) ──────────────────────────────────────────────────────────────────
// The FF800 sends MIDI by async-writing to a host address we advertise via 0x200000320. Those write
// requests land in the AR-request DMA context. HARDWARE-GATED: the AR-request receive path has never
// actually delivered a packet; this parser follows ohci.c handle_ar_packet but is unverified.
static constexpr uint64_t kMidiHighAddr = 0x200000320ull;

kern_return_t UFFireWireOHCI::MidiInEnable(uint32_t highIndex)
{
    if (!ivars->ff800Found) return kIOReturnNotReady;
    ivars->midiHighIndex = (uint16_t)highIndex;
    ivars->arReqReadBytes = 0;
    ivars->midiInOn = true;
    // Advertise (our node id << 16) | highIndex so the device writes MIDI to (highIndex << 32) | 0.
    uint32_t value = ((uint32_t)ivars->localNodeId << 16) | (highIndex & 0xffff);
    kern_return_t r = uf_write_quadlet(ivars, ivars->ff800Node, kMidiHighAddr, ivars->ff800Speed, value);
    os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: MIDI in enabled, high=0x%x local=0x%04x -> 0x%x",
           highIndex, ivars->localNodeId, r);
    return r;
}

kern_return_t UFFireWireOHCI::MidiInDisable()
{
    ivars->midiInOn = false;
    if (ivars->ff800Found)
        uf_write_quadlet(ivars, ivars->ff800Node, kMidiHighAddr, ivars->ff800Speed, 0);
    return kIOReturnSuccess;
}

// TEST HOOK: force a bus reset. Read-modify-write of PHY register 1 so gap_count is preserved —
// writing a bare IBR resets our gap count to 0 while every other node keeps 63, and the two ends
// then disagree about gap timing (see the same care taken in uf_hw_bring_up).
kern_return_t UFFireWireOHCI::ForceBusReset()
{
    if (!ivars->pci) return kIOReturnNotReady;
    uint8_t phy1 = 0;
    kern_return_t r = phy_read(ivars->pci, ivars->barIndex, PHY_REG_RESET, &phy1);
    if (r != kIOReturnSuccess) return r;
    os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: FORCED bus reset (phy1=0x%02x)", phy1);
    return phy_write(ivars->pci, ivars->barIndex, PHY_REG_RESET,
                     (uint8_t)((phy1 & ~0x3f) | 0x3f | PHY_IBR));
}

// DIAGNOSTIC: log every inbound async request the FF800 (or any peer) sends us — the host-node
// probe we are trying to detect. Walks the AR-request buffer from a private cursor and os_logs each
// packet's tcode + source node + 48-bit target address (+ read length). tcodes: 0=wq 1=wb 4=rq 5=rb.
// Reads of our config-ROM / CSR space are exactly what would gate the device's "host present/green".
kern_return_t UFFireWireOHCI::DebugPollInbound()
{
    struct ohci_descriptor* qd = (struct ohci_descriptor*)ivars->arReqDescCPU;
    const uint32_t written = kARBufferBytes - qd->res_count;
    const uint32_t* buf = ivars->arReqCPU;
    uint32_t off = ivars->dbgArReadBytes;
    while (off + 16 <= written) {
        const uint32_t* h = buf + off / 4;
        const unsigned tcode = (h[0] >> 4) & 0xf;
        const unsigned src   = h[1] >> 16;
        const uint64_t addr  = ((uint64_t)(h[1] & 0xffff) << 32) | h[2];
        unsigned adv = 0, len = 0;
        switch (tcode) {
            case 0x0: adv = 16 + 4; break;                              // write quadlet req
            case 0x1: len = h[3] >> 16; adv = 16 + ((len + 3) & ~3u) + 4; break;  // write block req
            case 0x4: adv = 12 + 4; break;                              // read quadlet req
            case 0x5: len = h[3] >> 16; adv = 16 + 4; break;            // read block req (header only)
            default:  adv = 0; break;
        }
        os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: INBOUND tcode=%u src=0x%04x addr=0x%llx len=%u",
               tcode, src, addr, len);
        if (adv == 0) { off = written; break; }                       // unknown: skip the rest
        off += adv;
    }
    ivars->dbgArReadBytes = off;
    return kIOReturnSuccess;
}

// Drain MIDI bytes received since the last poll. Walks the AR-request buffer from our read cursor to
// wherever the controller has written (buffer size - res_count), parsing async write requests to our
// MIDI region and unpacking one byte per quadlet (little-endian, low 8 bits).
// Re-arm the AR REQUEST context from a known-aligned start. Used both when the controller has
// filled the buffer and when our walk loses packet alignment — in either case the only safe resume
// point is a fresh descriptor, because a desynced cursor cannot be repaired by guessing.
//
// Stopping the context first matters: rewriting a descriptor the controller is actively consuming is
// a race, and this path now runs while inbound traffic is arriving rather than only once it has
// stopped.
static void uf_arreq_rearm(UFFireWireOHCI_IVars* v)
{
    uf_ctx_stop(v, OHCI_AR_REQ_BASE);
    struct ohci_descriptor* qd = (struct ohci_descriptor*)v->arReqDescCPU;
    qd->req_count       = kARBufferBytes;
    qd->res_count       = kARBufferBytes;   // remaining space, NOT bytes written — see bring-up
    qd->transfer_status = 0;
    qd->control         = (uint16_t)(DESC_INPUT_MORE | DESC_STATUS | DESC_BRANCH_ALWAYS);
    qd->data_address    = (uint32_t)v->arReqAddr;
    qd->branch_address  = (uint32_t)v->arReqDescAddr | 1;
    v->arReqReadBytes = 0;
    v->dbgArReadBytes = 0;
    reg_write(v->pci, v->barIndex, CTX_COMMAND_PTR(OHCI_AR_REQ_BASE),
              (uint32_t)v->arReqDescAddr | 1);
    reg_write(v->pci, v->barIndex, CTX_CONTROL_SET(OHCI_AR_REQ_BASE), CTX_RUN);
}

kern_return_t UFFireWireOHCI::MidiInPoll(uint8_t* out, uint32_t* outCount)
{
    const uint32_t capacity = *outCount;
    uint32_t n = 0;
    if (!ivars->midiInOn) { *outCount = 0; return kIOReturnSuccess; }

    struct ohci_descriptor* qd = (struct ohci_descriptor*)ivars->arReqDescCPU;
    const uint32_t written = kARBufferBytes - qd->res_count;   // bytes the controller has filled
    const uint32_t* buf = ivars->arReqCPU;

    // Walk the buffer. `desync` means we can no longer trust where packet boundaries are, which is
    // NOT the same as "nothing left to read" and must never be handled by simply stopping: this loop
    // used to `break` on an unrecognised tcode, which froze `off` permanently. The recycle below is
    // gated on `off >= written`, so a frozen cursor also blocked the re-arm — and MIDI in stayed
    // dead until the daemon was restarted. Observed on hardware with a DX7, whose continuous Active
    // Sensing (0xFE) fills the buffer fast; the debug walker was visibly reading MIDI payload as
    // packet headers ("addr=0xfe...") shortly before everything stopped.
    uint32_t off = ivars->arReqReadBytes;
    bool desync = false;
    while (off + 16 <= written && n < capacity) {
        const uint32_t* h = buf + off / 4;
        const unsigned tcode = (h[0] >> 4) & 0xf;
        const uint64_t offset = ((uint64_t)(h[1] & 0xffff) << 32) | h[2];
        uint32_t adv;
        if (tcode == 0x1 /*write block req*/) {
            const uint32_t dataLen = h[3] >> 16;
            // header(16) + payload(padded to quadlet) + trailing status quadlet.
            adv = 16 + ((dataLen + 3) & ~3u) + 4;
            // A length that cannot fit is proof the header is not a header.
            if (dataLen > kARBufferBytes || off + adv > written) { desync = true; break; }
            const uint32_t* payload = h + 4;
            if ((offset >> 32) == ivars->midiHighIndex) {
                for (uint32_t i = 0; i * 4 < dataLen && n < capacity; ++i)
                    out[n++] = (uint8_t)(payload[i] & 0xff);
            }
        } else if (tcode == 0x0 /*write quadlet req*/) {
            adv = 16 + 4;   // quadlet-write header includes the data; + trailing status
            if (off + adv > written) break;              // simply not all here yet — not a desync
            if ((offset >> 32) == ivars->midiHighIndex && n < capacity)
                out[n++] = (uint8_t)(h[3] & 0xff);
        } else {
            desync = true; break;   // read/lock request, or garbage: we cannot size it
        }
        off += adv;
    }
    ivars->arReqReadBytes = off;

    if (desync) {
        os_log(OS_LOG_DEFAULT,
               "UFFireWireOHCI: AR-request desync at off=%u of %u — re-arming", off, written);
        uf_arreq_rearm(ivars);
        *outCount = n;
        return kIOReturnSuccess;
    }

    // Recycle the buffer once the controller has filled it and we have parsed everything in it.
    // The AR-request context is a SINGLE self-branching descriptor: when res_count reaches 0 the
    // controller has nowhere left to put incoming requests and simply stops accepting them, and
    // because nothing ever reset the descriptor, MIDI-in would die permanently after the first 4 KB
    // of inbound traffic and never come back. Re-arm and wake the context instead.
    // Recycle once the controller has run out of room. Deliberately NOT conditioned on having
    // parsed everything: if the buffer is full there is nowhere for new requests to go, so waiting
    // for the cursor to catch up risks trading a few unread bytes for a permanently dead port.
    if (qd->res_count == 0) uf_arreq_rearm(ivars);

    *outCount = n;
    return kIOReturnSuccess;
}

// Read a node's config ROM (CSR base 0xffff_f0000400) and identify it via the portable identity gate.
static void ohci_identify(UFFireWireOHCI_IVars* v, uint16_t node, uint32_t speed)
{
    uint32_t rom[24];
    for (int i = 0; i < 24; ++i) rom[i] = 0;
    const uint64_t base = 0xfffff0000400ull;

    // A node does not necessarily answer a config-ROM read the moment self-ID completes — the FF800
    // needs longer than the bus does, and an early request simply goes unacked. Retry at the self-ID
    // speed first; only then step the speed down (self-ID reports a node's capability, not what the
    // link between us can actually carry).
    uint32_t val = 0;
    bool got = false;
    for (uint32_t spd = speed; !got; --spd) {
        for (int tries = 0; tries < 10 && !got; ++tries) {
            if (uf_read_quadlet(v, node, base, spd, &val) == kIOReturnSuccess) {
                speed = spd;
                got = true;
                break;
            }
            IOSleep(50);
        }
        if (got || spd == 0) break;
        os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: node 0x%04x no answer at S%u, trying S%u",
               node, 100u << spd, 100u << (spd - 1));
    }
    if (!got) {
        os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: node 0x%04x unreachable at any speed", node);
        return;
    }
    // The config ROM is big-endian (IEEE-1212), unlike the FF800's own registers — swap it here.
    rom[0] = __builtin_bswap32(val);
    for (int i = 1; i < 24; ++i) {
        if (uf_read_quadlet(v, node, base + (uint64_t)i * 4, speed, &val) != kIOReturnSuccess) break;
        rom[i] = __builtin_bswap32(val);
    }
    os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: node 0x%04x ROM %08x %08x %08x %08x %08x %08x %08x %08x",
           node, rom[0], rom[1], rom[2], rom[3], rom[4], rom[5], rom[6], rom[7]);
    uf::ohci::RomIdentity id = uf::ohci::parse_config_rom(rom, 24);
    if (uf::ohci::rom_is_ff800(id)) {
        v->ff800Found = true;
        v->ff800Node = node;
        v->ff800Speed = speed;
        // Bus-info-block capabilities quadlet (ROM offset 0x408 = rom[2]): cmc bit 30. Only a
        // cycle-master-capable node may be made root/cycle-master — Linux bm_work guards on exactly
        // this (core-card.c: !root_device->cmc => the host stands for root instead). If the FF800 is
        // not cmc, we must NOT force it root (that would leave the bus with no cycle master).
        v->ff800Cmc = (rom[2] & 0x40000000u) != 0;
        os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: *** FF800 found at node 0x%04x (model 0x%06x ver 0x%06x) cmc=%d ***",
               node, id.model_id, id.version, v->ff800Cmc);
    } else
        os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: node 0x%04x vendor 0x%06x (not FF800)",
               node, id.vendor_id);
}

// ── OHCI-2.5: user-client transport methods (LOCALONLY — called by the user client) ──────────
kern_return_t UFFireWireOHCI::ReadQuadlet(uint64_t offset, uint32_t* value)
{
    if (!ivars->ff800Found) return kIOReturnNotReady;
    return uf_read_quadlet(ivars, ivars->ff800Node, offset, ivars->ff800Speed, value);
}

kern_return_t UFFireWireOHCI::WriteQuadlet(uint64_t offset, uint32_t value)
{
    if (!ivars->ff800Found) return kIOReturnNotReady;
    return uf_write_quadlet(ivars, ivars->ff800Node, offset, ivars->ff800Speed, value);
}

// Read the OHCI isochronous cycle timer (the bus clock). The daemon samples this at capture-buffer
// completion to recover the FF800 audio clock against the bus (see ohci_regs.h). No device round-trip
// — it's a local controller register.
kern_return_t UFFireWireOHCI::ReadCycleTimer(uint32_t* value)
{
    if (!ivars->pci) return kIOReturnNotReady;
    *value = reg_read(ivars->pci, ivars->barIndex, OHCI_IsochronousCycleTimer);
    return kIOReturnSuccess;
}

// Hand out a user-client connection (the Info.plist personality names the IOUserClass).
kern_return_t
IMPL(UFFireWireOHCI, NewUserClient)
{
    IOService* client = nullptr;
    kern_return_t ret = Create(this, "UserClientProperties", &client);
    if (ret != kIOReturnSuccess) return ret;
    *userClient = OSDynamicCast(IOUserClient, client);
    if (*userClient == nullptr) { client->release(); return kIOReturnError; }
    return kIOReturnSuccess;
}

// ── System sleep / wake ───────────────────────────────────────────────────────────────────────
// Why this exists: macOS powers the Thunderbolt/PCIe bridge down on sleep, and touching a BAR whose
// device has gone away is a PCIe bus error — an immediate kernel panic on Apple Silicon, not an
// error return. See the sleep gate above reg_read.
//
// This handler is on a short leash and every rule below was learned by panicking the machine:
//
//   * IT MUST RETURN PROMPTLY. IOServicePM waits ~20 s and then panics the kernel outright.
//     So the wake-time rebuild — a soft reset, a 150 ms LPS wait, a bus reset, PHY polling — runs
//     on our own queue via DispatchAsync and this returns immediately.
//
//   * NO DispatchSync ONTO ivars->queue. If PM delivers this call on that queue, dispatching
//     synchronously back onto it deadlocks, which presents as exactly the same PM timeout panic.
//     Disabling the interrupt source and closing the gate is enough; power is still on until we
//     return, so an in-flight handler that already passed the gate is reading a live device.
//
//   * kIOServicePowerCapabilityOn ALSO ARRIVES AT INITIAL POWER-UP, not just on wake. Treating it
//     as "we just woke" ran a full bring-up racing Start(), and plugging the adapter in panicked
//     the machine every single time. wasPoweredDown is what tells the two apart: it is set only on
//     the way down, so the first power-on — which never had a way down — is left to Start().
//
// Never returns failure: a refused power transition is worse than a degraded one, and the daemon
// already recovers from a dead session through its wedge detection.
kern_return_t
IMPL(UFFireWireOHCI, SetPowerState)
{
    const bool on = (powerFlags & kIOServicePowerCapabilityOn) != 0;
    if (!ivars) return SetPowerState(powerFlags, SUPERDISPATCH);

    if (!on) {
        // Touch NOTHING on the way down.
        //
        // The tidy thing — mask interrupts, stop the DMA contexts, drop the link — is what panicked
        // the machine on the fifth attempt: the faulting write was the AR-response
        // ContextControlClear (0x1e4), i.e. our own uf_ctx_stop(). By the time this handler runs the
        // PCIe bridge is ALREADY unreachable, so every one of those courteous writes is a bus error,
        // which on Apple Silicon is an instant panic.
        //
        // There is nothing worth preserving anyway: the controller is losing power, and the wake
        // path opens with a full soft reset that rebuilds all of it from scratch. So the only thing
        // that actually has to happen here is that WE stop touching MMIO — which is the gate, and it
        // closes FIRST so an interrupt handler already in flight reads 0xffffffff and bails out
        // through the check it already has.
        ohci_set_powered(false);
        if (ivars->intSource) ivars->intSource->SetEnable(false);
        ivars->itRunning = false;
        ivars->isoRunning = false;
        ivars->isoContinuous = false;
        ivars->wasPoweredDown = true;
        os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: power off — MMIO gated, no register access");
        return SetPowerState(powerFlags, SUPERDISPATCH);
    }

    // Open the gate first — everything the rebuild does is register access, and with the gate shut
    // every write would be silently dropped and we would come back up dead.
    ohci_set_powered(true);

    if (!ivars->started || !ivars->wasPoweredDown) {
        // Initial power-on. Start() owns the bring-up; doing it here as well would race it.
        return SetPowerState(powerFlags, SUPERDISPATCH);
    }
    ivars->wasPoweredDown = false;

    // A real wake. Rebuild off the PM thread so this call returns now, not in half a second.
    if (ivars->pci && ivars->queue) {
        ivars->queue->DispatchAsync(^{
            // We may have been queued before another sleep landed. Running now would push every
            // register write into a closed gate and leave the controller dead, silently.
            if (!ohci_powered() || !ivars->started) {
                os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: wake rebuild skipped — powered down again");
                return;
            }
            const kern_return_t ret = uf_hw_bring_up(ivars);
            if (ret != kIOReturnSuccess) {
                os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: wake bring-up FAILED (0x%x)", ret);
                return;
            }
            if (ivars->intSource) ivars->intSource->SetEnable(true);
            os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: wake — controller re-initialised");
        });
    }
    return SetPowerState(powerFlags, SUPERDISPATCH);
}

kern_return_t
IMPL(UFFireWireOHCI, Stop)
{
    if (ivars) {
        // Shut SetPowerState out first: once we start tearing down, the buffers it would hand the
        // controller are on their way to being freed.
        ivars->started = false;
        // Quiesce the controller before we let go of it: no interrupts, no running DMA contexts,
        // link down.
        if (ivars->pci) {
            reg_write(ivars->pci, ivars->barIndex, OHCI_IntMaskClear, ~0u);
            reg_write(ivars->pci, ivars->barIndex, OHCI_IntEventClear, ~0u);
            uf_ctx_stop(ivars, OHCI_AT_REQ_BASE);
            uf_ctx_stop(ivars, OHCI_AR_RSP_BASE);
            reg_write(ivars->pci, ivars->barIndex, OHCI_HCControlClear, HCControl_linkEnable);
        }
        // Break the interrupt retain cycle. CreateActionInterruptOccurred() returns an OSAction that
        // RETAINS this driver so it can dispatch into it — so releasing the action in free() is
        // useless: free() can never run while the action holds the last reference. The cycle must be
        // broken here, in Stop(), or the object never dies, the user server never finalizes, the
        // dext process outlives its Stop, and a replacing upgrade wedges in
        // "terminating for upgrade via delegate" forever.
        if (ivars->intSource) {
            ivars->intSource->SetEnable(false);
            ivars->intSource->Cancel(^{});
        }
        OSSafeReleaseNULL(ivars->intSource);
        OSSafeReleaseNULL(ivars->intAction);
        OSSafeReleaseNULL(ivars->queue);
        if (ivars->selfIdDMA) ivars->selfIdDMA->CompleteDMA(0);
        if (ivars->atDMA)     ivars->atDMA->CompleteDMA(0);
        if (ivars->arDMA)     ivars->arDMA->CompleteDMA(0);
        if (ivars->arDescDMA) ivars->arDescDMA->CompleteDMA(0);
        if (ivars->pci)       ivars->pci->Close(this, 0);
    }
    os_log(OS_LOG_DEFAULT, "UFFireWireOHCI: Stop");
    return Stop(provider, SUPERDISPATCH);
}
