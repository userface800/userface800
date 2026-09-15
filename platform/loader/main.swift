// UFLoader — activate the UFFireWireOHCI dext and test-read the FF800 through its user client.
//
// A DriverKit dext is loaded by a host app via the SystemExtensions framework: the .dext is embedded
// in this app's bundle (Contents/Library/SystemExtensions/) and we request activation. Once matched
// to the OHCI controller, we open the dext's IOUserClient and call ReadQuadlet on the FF800's
// firmware register — the end-to-end "plug in → something happens" proof.
import Foundation
import SystemExtensions
import IOKit

let dextIdentifier = "com.userface800.UFLoader.UFFireWireOHCI"

// Must match UFFireWireOHCIShared.h.
let kUFOhciReadQuadlet: UInt32 = 0
let kUFOhciWriteQuadlet: UInt32 = 1

// FF800 registers (uf::reg::*): firmware revision + status 0.
let kFirmwareRev: UInt64 = 0x200000100
let kStatus0: UInt64 = 0x801c0000

func testUserClient() {
    // A dext publishes as class IOUserService named after its personality, so match the name —
    // IOServiceMatching() matches the class and would never find it.
    let service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceNameMatching("UFFireWireOHCI"))
    guard service != 0 else {
        print("• dext service not found — is the OHCI controller present and the dext matched?")
        return
    }
    var conn: io_connect_t = 0
    let kr = IOServiceOpen(service, mach_task_self_, 0, &conn)
    IOObjectRelease(service)
    guard kr == KERN_SUCCESS else { print("• IOServiceOpen failed: \(String(format: "0x%x", kr))"); return }
    defer { IOServiceClose(conn) }

    func readQuadlet(_ offset: UInt64) -> UInt64? {
        var input: [UInt64] = [offset]
        var output = [UInt64](repeating: 0, count: 1)
        var outCount: UInt32 = 1
        let r = IOConnectCallScalarMethod(conn, kUFOhciReadQuadlet, &input, 1, &output, &outCount)
        return r == KERN_SUCCESS ? output[0] : nil
    }

    if let fw = readQuadlet(kFirmwareRev), let sr0 = readQuadlet(kStatus0) {
        print(String(format: "✓ RME FF800: firmware register 0x%08llx, status 0x%08llx", fw, sr0))
    } else {
        print("• ReadQuadlet failed (FF800 not yet identified, or AR path issue)")
    }
}

final class Delegate: NSObject, OSSystemExtensionRequestDelegate {
    func request(_ request: OSSystemExtensionRequest,
                 actionForReplacingExtension existing: OSSystemExtensionProperties,
                 withExtension ext: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {
        return .replace
    }
    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        print("• Needs approval: System Settings → General → Login Items & Extensions → Driver Extensions.")
    }
    func request(_ request: OSSystemExtensionRequest, didFinishWithResult result: OSSystemExtensionRequest.Result) {
        print("✓ Activation result: \(result.rawValue) (0 = completed)")
        testUserClient()
        exit(0)
    }
    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        // Still try the user client: an already-running dext serves it even when a re-activation
        // request is refused (e.g. a previous copy stuck in terminating_for_upgrade).
        print("✗ Activation failed: \(error)")
        testUserClient()
        exit(1)
    }
}

// What sysextd sees: it locates the requesting app via the Security framework, not argv[0].
var selfCode: SecCode?
SecCodeCopySelf([], &selfCode)
var selfPath: CFURL?
if let c = selfCode { SecCodeCopyPath(c as! SecStaticCode, [], &selfPath) }
print("• Bundle.main: \(Bundle.main.bundleURL.path)")
print("• SecCode path: \((selfPath as URL?)?.path ?? "<none>")")

let delegate = Delegate()
let req = OSSystemExtensionRequest.activationRequest(forExtensionWithIdentifier: dextIdentifier, queue: .main)
req.delegate = delegate
OSSystemExtensionManager.shared.submitRequest(req)
print("→ Submitted activation request for \(dextIdentifier) …")
RunLoop.main.run()
