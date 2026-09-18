// uf-busreset — force a FireWire bus reset, to test topology-change recovery on demand.
//
// A bus reset is what happens in the field whenever the topology changes: another device plugged in,
// a cable nudged, something on the chain powering up. Every node is renumbered, so any cached node
// id goes stale — which is exactly the state bus-reset mid-stream recovery has to survive.
//
// Testing that normally needs a second FireWire device to plug in. This does it in software instead,
// which is both repeatable and safe to run mid-stream — the point being to see what the daemon does.
//
// Run it WHILE audio is playing. Expected, in order: the dext logs "bus reset" and re-identifies the
// FF800; capture stalls briefly; the daemon reports RESTART (capture stalled — device wedged) and
// rebuilds. What must NOT happen is silence with no recovery, or a permanent "no tx channel".
#include <IOKit/IOKitLib.h>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

// The selector enum comes from the shared header rather than a local copy, so the two cannot drift.
#include "../platform/ohci-dext/UFFireWireOHCIShared.h"

int main(int argc, char** argv) {
    int count = (argc > 1) ? atoi(argv[1]) : 1;
    if (count < 1) count = 1;

    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
                                                  IOServiceNameMatching("UFFireWireOHCI"));
    if (!svc) { std::fprintf(stderr, "uf-busreset: dext not found\n"); return 1; }
    io_connect_t conn = 0;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) { std::fprintf(stderr, "uf-busreset: open failed 0x%x\n", kr); return 1; }

    for (int i = 0; i < count; ++i) {
        kr = IOConnectCallScalarMethod(conn, kUFOhciForceBusReset, nullptr, 0, nullptr, nullptr);
        std::printf("bus reset %d/%d -> 0x%x%s\n", i + 1, count, kr,
                    kr == KERN_SUCCESS ? "" : "  (FAILED)");
        std::fflush(stdout);
        if (i + 1 < count) sleep(2);   // let the bus settle and the daemon react between resets
    }
    IOServiceClose(conn);
    return kr == KERN_SUCCESS ? 0 : 1;
}
