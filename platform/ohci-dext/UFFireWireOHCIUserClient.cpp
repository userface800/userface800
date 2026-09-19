// UFFireWireOHCIUserClient.cpp — forward the daemon's register ops to the OHCI driver.
#include <os/log.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOUserClient.h>
#include <DriverKit/OSData.h>

#include "UFFireWireOHCIUserClient.h"  // generated
#include "UFFireWireOHCI.h"            // generated — the provider's ReadQuadlet/WriteQuadlet
#include "UFFireWireOHCIShared.h"

struct UFFireWireOHCIUserClient_IVars {
    UFFireWireOHCI* driver;
};

bool UFFireWireOHCIUserClient::init()
{
    if (!super::init()) return false;
    ivars = IONewZero(UFFireWireOHCIUserClient_IVars, 1);
    return ivars != nullptr;
}

void UFFireWireOHCIUserClient::free()
{
    if (ivars) IOSafeDeleteNULL(ivars, UFFireWireOHCIUserClient_IVars, 1);
    super::free();
}

kern_return_t
IMPL(UFFireWireOHCIUserClient, Start)
{
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) return ret;
    ivars->driver = OSDynamicCast(UFFireWireOHCI, provider);
    if (!ivars->driver) return kIOReturnNoDevice;
    return kIOReturnSuccess;
}

kern_return_t
IMPL(UFFireWireOHCIUserClient, Stop)
{
    return Stop(provider, SUPERDISPATCH);
}

// The client maps the capture buffer read-only rather than copying packets through scalars.
kern_return_t
IMPL(UFFireWireOHCIUserClient, CopyClientMemoryForType)
{
    if (!ivars->driver)
        return CopyClientMemoryForType(type, options, memory, SUPERDISPATCH);
    if (type == kUFOhciIsoMemoryType) {          // capture: read-only
        *options = kIOUserClientMemoryReadOnly;
        return ivars->driver->CopyIsoBuffer(memory);
    }
    if (type == kUFOhciTxMemoryType) {           // playback: read-write (daemon fills it)
        *options = 0;
        return ivars->driver->CopyTxBuffer(memory);
    }
    if (type == kUFOhciStatusMemoryType) {       // interrupt status + the daemon's cursors back
        *options = 0;                            // read-write: the control half is client-written
        return ivars->driver->CopyStatusBuffer(memory);
    }
    return CopyClientMemoryForType(type, options, memory, SUPERDISPATCH);
}

kern_return_t
UFFireWireOHCIUserClient::ExternalMethod(uint64_t selector,
                                         IOUserClientMethodArguments* arguments,
                                         const IOUserClientMethodDispatch* dispatch,
                                         OSObject* target,
                                         void* reference)
{
    if (!ivars->driver) return kIOReturnNotReady;

    switch (selector) {
        case kUFOhciReadQuadlet: {
            if (arguments->scalarInputCount < 1 || arguments->scalarInput == nullptr)
                return kIOReturnBadArgument;
            uint64_t offset = arguments->scalarInput[0];
            uint32_t value = 0;
            kern_return_t r = ivars->driver->ReadQuadlet(offset, &value);
            if (r != kIOReturnSuccess) return r;
            if (arguments->scalarOutput && arguments->scalarOutputCount >= 1) {
                arguments->scalarOutput[0] = value;
                arguments->scalarOutputCount = 1;
            }
            return kIOReturnSuccess;
        }
        case kUFOhciWriteQuadlet: {
            if (arguments->scalarInputCount < 2 || arguments->scalarInput == nullptr)
                return kIOReturnBadArgument;
            uint64_t offset = arguments->scalarInput[0];
            uint32_t value = (uint32_t)arguments->scalarInput[1];
            return ivars->driver->WriteQuadlet(offset, value);
        }
        case kUFOhciIsoStart: {
            if (arguments->scalarInputCount < 2 || arguments->scalarInput == nullptr)
                return kIOReturnBadArgument;
            return ivars->driver->IsoStart((uint32_t)arguments->scalarInput[0],
                                           (uint32_t)arguments->scalarInput[1]);
        }
        case kUFOhciIsoPoll: {
            uint32_t received = 0;
            kern_return_t r = ivars->driver->IsoPoll(&received);
            if (r != kIOReturnSuccess) return r;
            if (arguments->scalarOutput && arguments->scalarOutputCount >= 1) {
                arguments->scalarOutput[0] = received;
                arguments->scalarOutputCount = 1;
            }
            return kIOReturnSuccess;
        }
        case kUFOhciIsoStop:
            return ivars->driver->IsoStop();
        case kUFOhciIsoStartCont: {
            if (arguments->scalarInputCount < 1 || arguments->scalarInput == nullptr)
                return kIOReturnBadArgument;
            return ivars->driver->IsoStartContinuous((uint32_t)arguments->scalarInput[0]);
        }
        case kUFOhciIsoCompleted: {
            uint64_t total = 0;
            kern_return_t r = ivars->driver->IsoCompletedCount(&total);
            if (r != kIOReturnSuccess) return r;
            if (arguments->scalarOutput && arguments->scalarOutputCount >= 1) {
                arguments->scalarOutput[0] = total; arguments->scalarOutputCount = 1;
            }
            return kIOReturnSuccess;
        }
        case kUFOhciIsoRelease: {
            if (arguments->scalarInputCount < 1 || arguments->scalarInput == nullptr)
                return kIOReturnBadArgument;
            return ivars->driver->IsoRelease(arguments->scalarInput[0]);
        }
        case kUFOhciMidiInEnable: {
            if (arguments->scalarInputCount < 1 || arguments->scalarInput == nullptr)
                return kIOReturnBadArgument;
            return ivars->driver->MidiInEnable((uint32_t)arguments->scalarInput[0]);
        }
        case kUFOhciMidiInPoll: {
            uint8_t bytes[8] = {0};
            uint32_t count = sizeof(bytes);
            kern_return_t r = ivars->driver->MidiInPoll(bytes, &count);
            if (r != kIOReturnSuccess) return r;
            if (arguments->scalarOutput && arguments->scalarOutputCount >= 2) {
                uint64_t packed = 0;
                for (uint32_t i = 0; i < count && i < 8; ++i)
                    packed |= (uint64_t)bytes[i] << (8 * i);
                arguments->scalarOutput[0] = count;
                arguments->scalarOutput[1] = packed;
                arguments->scalarOutputCount = 2;
            }
            return kIOReturnSuccess;
        }
        case kUFOhciMidiInDisable:
            return ivars->driver->MidiInDisable();
        case kUFOhciIsoTxStart: {
            if (arguments->scalarInputCount < 3 || arguments->scalarInput == nullptr)
                return kIOReturnBadArgument;
            return ivars->driver->IsoTxStart((uint32_t)arguments->scalarInput[0],
                                             (uint32_t)arguments->scalarInput[1],
                                             (uint32_t)arguments->scalarInput[2]);
        }
        case kUFOhciIsoTxStop:
            return ivars->driver->IsoTxStop();
        case kUFOhciIsoTxSent: {
            uint64_t sent = 0;
            kern_return_t r = ivars->driver->IsoTxSent(&sent);
            if (r != kIOReturnSuccess) return r;
            if (arguments->scalarOutput && arguments->scalarOutputCount >= 1) {
                arguments->scalarOutput[0] = sent; arguments->scalarOutputCount = 1;
            }
            return kIOReturnSuccess;
        }
        case kUFOhciIsoTxRefill: {
            if (arguments->scalarInputCount < 1 || arguments->scalarInput == nullptr)
                return kIOReturnBadArgument;
            return ivars->driver->IsoTxRefill(arguments->scalarInput[0]);
        }
        case kUFOhciIsoTxSetBytes: {
            if (arguments->scalarInputCount < 2 || arguments->scalarInput == nullptr)
                return kIOReturnBadArgument;
            return ivars->driver->IsoTxSetSlotBytes((uint32_t)arguments->scalarInput[0],
                                                    (uint32_t)arguments->scalarInput[1]);
        }
        case kUFOhciDebugInbound:
            return ivars->driver->DebugPollInbound();
        case kUFOhciForceBusReset:
            return ivars->driver->ForceBusReset();
        case kUFOhciIsoWake: {
            // A standing subscription, not a request: we keep the completion action and re-fire it
            // on every isochronous completion interrupt, so this method never "returns" a result.
            // Called with a null completion (an ordinary synchronous call) it unsubscribes.
            return ivars->driver->SetIsoWake(this, arguments->completion);
        }
        case kUFOhciPump: {
            // A whole pump tick in one IPC: hand back the slots we consumed, then report where the
            // controller is on both contexts. Per-call failures are swallowed on purpose — with
            // transmit disabled the tx half is simply not running, and that must not fail the poll
            // the capture path depends on.
            if (arguments->scalarInputCount < 2 || arguments->scalarInput == nullptr)
                return kIOReturnBadArgument;
            ivars->driver->IsoRelease(arguments->scalarInput[0]);
            if (arguments->scalarInput[1]) ivars->driver->IsoTxRefill(arguments->scalarInput[1]);
            uint64_t completed = 0, sent = 0;
            ivars->driver->IsoCompletedCount(&completed);
            ivars->driver->IsoTxSent(&sent);
            if (arguments->scalarOutput && arguments->scalarOutputCount >= 2) {
                arguments->scalarOutput[0] = completed;
                arguments->scalarOutput[1] = sent;
                arguments->scalarOutputCount = 2;
            }
            return kIOReturnSuccess;
        }
        case kUFOhciReadCycleTimer: {
            uint32_t value = 0;
            kern_return_t r = ivars->driver->ReadCycleTimer(&value);
            if (r != kIOReturnSuccess) return r;
            if (arguments->scalarOutput && arguments->scalarOutputCount >= 1) {
                arguments->scalarOutput[0] = value; arguments->scalarOutputCount = 1;
            }
            return kIOReturnSuccess;
        }
        case kUFOhciWriteBlock: {
            if (arguments->scalarInputCount < 1 || arguments->scalarInput == nullptr ||
                arguments->structureInput == nullptr)
                return kIOReturnBadArgument;
            uint64_t offset = arguments->scalarInput[0];
            const uint32_t bytes = (uint32_t)arguments->structureInput->getLength();
            const uint32_t count = bytes / 4;
            if (count == 0 || count > kUFOhciMaxBlockQuadlets || (bytes % 4) != 0)
                return kIOReturnBadArgument;
            const uint32_t* quads =
                (const uint32_t*)arguments->structureInput->getBytesNoCopy();
            if (!quads) return kIOReturnBadArgument;
            return ivars->driver->WriteBlock(offset, quads, count);
        }
        case kUFOhciReadBlock: {
            if (arguments->scalarInputCount < 2 || arguments->scalarInput == nullptr)
                return kIOReturnBadArgument;
            const uint64_t offset = arguments->scalarInput[0];
            const uint32_t count  = (uint32_t)arguments->scalarInput[1];
            if (count == 0 || count > kUFOhciMaxReadQuadlets) return kIOReturnBadArgument;

            uint32_t quads[kUFOhciMaxReadQuadlets];
            kern_return_t r = ivars->driver->ReadBlock(offset, quads, count);
            if (r != kIOReturnSuccess) return r;

            // Structure output rather than scalars: 254 quadlets does not fit in the scalar array,
            // and the point of a block read is to get the whole region in one round trip.
            OSData* data = OSData::withBytes(quads, count * 4);
            if (!data) return kIOReturnNoMemory;
            arguments->structureOutput = data;
            return kIOReturnSuccess;
        }
        default:
            return kIOReturnUnsupported;
    }
}
