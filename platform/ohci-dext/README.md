# UFFireWireOHCI — userland 1394 OHCI driver (PCIDriverKit dext)

The **transport layer** for UserFace800 on Apple Silicon / macOS Tahoe, where Apple's kernel
`IOFireWireFamily` no longer exists. This dext binds the Thunderbolt→FireWire adapter's OHCI
controller directly and (across phases) exposes async read/write/lock + isochronous DMA to the
userland driver. `UFAsyncIO` targets this dext's user client instead of `IOFireWireLib`. Everything
above the wire (the tested portable protocol core) sits on top unchanged.

## Supported hardware

Requires a Thunderbolt→FireWire adapter whose 1394 OHCI controller enumerates as an unclaimed
`IOPCIDevice`, tunneled over Thunderbolt. Verified with the Apple TB→FireWire adapter:

| field | value |
|---|---|
| vendor-id | `0x11c1` (LSI / Agere) |
| device-id | `0x5901` (LSI **FW643** PCIe→1394b OHCI) |
| class-code | `0x0c0010` (Serial-Bus / FireWire / OHCI) |
| BAR0 | 256 KB register space |
| FireWire nubs | **none** — Apple's stack is gone, controller is free to bind |

FW643 is well-covered by Linux `firewire-ohci`, so the port reference is ideal.

## Files

- `UFFireWireOHCI.iig` — DriverKit class interface (IOService lifecycle).
- `UFFireWireOHCI.cpp` — phase 0: claim the IOPCIDevice, enable memory/bus-master, map BAR0, read
  the OHCI `Version` + `GUID` registers, log them — the minimum that shows the controller is
  reachable.
- `ohci_regs.h` — 1394 OHCI 1.1 register map (the subset phases 0/1 need).
- `Info.plist` — `IOPCIClassMatch 0x0c001000&0xffffff00` (any 1394 OHCI) + `IOPCITunnelCompatible`
  (required for a Thunderbolt-tunneled device).
- `UFFireWireOHCI.entitlements` — DriverKit + the **managed** PCI-transport entitlement.
- `build.sh` — CLI compile-check (see caveat below).

## Building — it builds ✅

`UFFireWireOHCI.xcodeproj` is checked in and **builds a real `.dext`** (Xcode 26 / DriverKit 25,
Apple Silicon). Compile + link succeed; the product is a 76 KB arm64 driver
extension with the `IOPCIClassMatch 0x0c0010` personality embedded.

```
cd platform/ohci-dext
xcodebuild -project UFFireWireOHCI.xcodeproj -scheme UFFireWireOHCI -configuration Debug \
    build CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO
# → ** BUILD SUCCEEDED **   (add a signing identity to produce a loadable, signed build)
```

Notes: the standalone `build.sh` hand-drives `iig` and is superseded by the Xcode project, which
passes the `-D__IIG=1` that `iig` requires. The `HEADER_SEARCH_PATHS` points at the
repo `include/` so the dext uses the host-tested `uf::ohci::configrom` identity logic directly.

## Loading the dext locally (no Apple approval needed)

`com.apple.developer.driverkit.transport.pci` is a *restricted* entitlement **only for the
sanctioned path** (notarized, provisioning-profile-backed). For a driver built and signed on your
own machine you relax enforcement instead — no Developer account, no provisioning profile. Full
recipe: `platform/loader/LOADING.md`.

1. **Reduced/Permissive Security** (Apple Silicon): recoveryOS → Startup Security Utility (or
   `bputil`), lower the security policy so custom boot-args + third-party extensions are allowed.
2. **SIP off**: `csrutil disable` (in recovery).
3. **Boot-arg**: `sudo nvram boot-args="-arm64e_preview_abi"`. Do **not** set
   `amfi_get_out_of_my_way` — with AMFI out of the way nothing vouches for the dext and the kernel
   refuses to exec it (`Exec format error`).
4. **Developer mode**: `systemextensionsctl developer on` (load a locally-built, non-notarized dext).
5. **Ad-hoc sign**: `codesign -s - --entitlements UFFireWireOHCI.entitlements <bundle>`; activate the
   dext from a small host app via the SystemExtensions framework.

The personality sets `IOPCITunnelCompatible` so matching the Thunderbolt-tunneled controller is
permitted. Trade-offs: this lowers the machine's security globally (fine for a dev box), and
SIP-off + dev-mode is the standard driver-dev path, but Apple keeps tightening DriverKit, so it may
need revisiting on newer releases. Sonoma (Apple's FireWire stack via
`IOFireWireLib`, no dext) remains a fallback only if this proves impossible.

## Roadmap (port from 1394 OHCI 1.1 spec + Linux `firewire-ohci` + Apple's open `IOFireWireFamily`)

- **OHCI-0 ✅ (source):** bind IOPCIDevice + read Version/GUID.
- **OHCI-1 ✅ (source):** software reset, LPS/link enable, self-ID DMA buffer + MSI interrupt → on bus
  reset, decode the self-ID buffer and log the node topology (`UFFireWireOHCI.cpp` `Start` /
  `InterruptOccurred`). Portable self-ID math is host-tested (`uf::ohci::selfid`).
  *"plug in → dext logs the controller + the FF800's node."* **Builds via the checked-in xcodeproj; needs signing + device to run.**
- **OHCI-2 🟢 (source, control-rate complete):** synchronous async register I/O → backs `UFAsyncIO`.
  Host-tested: packet marshalling (`uf::ohci::async`), AT header repack (`build_at_header`), AT block
  construction (`ohci_build_at_block`), config-ROM identity (`uf::ohci::configrom`). Dext
  (builds): AT submit+poll (`uf_at_run`), `uf_read_quadlet`/`uf_write_quadlet`, AT block DMA +
  AR response context (single self-looping INPUT descriptor, control-rate). **Wired end-to-end in
  source:** bus reset → self-ID → for each link-active node, read its config ROM → `rom_is_ff800` →
  log "FF800 found". *This is device identification against real hardware, in source.* Remaining before it's
  robust: proper AR ring (multi-buffer + trailer/ack) for reliability + write-response rcode checks.
- **OHCI-2.5 ✅ (builds):** expose `uf_read/write_quadlet` + block/lock through an IOUserClient so the
  UserFace800 daemon (UFAsyncIO) drives it → the whole control core (rate/clock/settings/status/
  mixer) runs on the FF800.
- **OHCI-3:** isochronous IT/IR DMA contexts → feeds the packet codec → capture/playback.
- User client: expose the transport ops (read/write/lock + isoch frames) to the UserFace800 daemon.

### Portable (host-tested) vs dext-glue split so far

| area | portable + tested | dext glue (builds via xcodebuild) |
|---|---|---|
| async packets | `uf::ohci::async` ✅ | AT/AR DMA contexts (OHCI-2) |
| self-ID / topology | `uf::ohci::selfid` ✅ | self-ID buffer wiring ✅ (`InterruptOccurred`) |
| controller bring-up | — | reset / LPS / link / MSI ✅ (`Start`) |

## References

- 1394 Open Host Controller Interface Spec, Release 1.1 (public standard) — the register/DMA map.
- Linux `drivers/firewire/ohci.c` (GPL) — complete modern OHCI driver; primary port source.
- Apple `IOFireWireFamily` (APSL, opensource.apple.com) — the macOS-side quirks for this hardware.
