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
            uint8_t bytes[256];
            uint32_t count = sizeof(bytes);
            kern_return_t r = ivars->driver->MidiInPoll(bytes, &count);
            if (r != kIOReturnSuccess) return r;
            if (arguments->structureOutput) {
                const uint32_t cap = (uint32_t)arguments->structureOutput->getLength();
                const uint32_t nb = count < cap ? count : cap;
                memcpy((void*)arguments->structureOutput->getBytesNoCopy(), bytes, nb);
                // structureOutputDescriptor length reflects what we wrote (best-effort).
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
        default:
            return kIOReturnUnsupported;
    }
}
