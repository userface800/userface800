# Loading UFFireWireOHCI on your own machine

`UFLoader.app` embeds the `UFFireWireOHCI.dext` and activates it via SystemExtensions, then reads the
FF800's firmware + status registers through the dext's user client — the end-to-end "plug in →
something happens" proof. Everything here is **build-your-own + self-signed**; no Apple approval.

This is for an **Apple-silicon Mac with the TB→FireWire adapter**. It lowers that machine's security
globally — dev box only.

## 1. Lower the Mac's security (one time)

DriverKit won't honor a self-signed restricted entitlement (`driverkit.transport.pci`) unless SIP is
off and the extension is ad-hoc signed with developer mode on.

1. Boot to **recoveryOS** (hold the power button → Options), open **Startup Security Utility**, pick
   your disk → **Reduced Security**, and tick **"Allow user management of kernel extensions…"**.
2. Still in recovery, disable SIP: `csrutil disable`.
3. Reboot to macOS, then set the boot-arg that lets a locally-signed dext run on Apple silicon:
   ```
   sudo nvram boot-args="-arm64e_preview_abi"
   ```
   Reboot again.
4. Enable dext developer mode:
   ```
   systemextensionsctl developer on
   ```

**Do NOT set `amfi_get_out_of_my_way`.** It is the obvious-looking move (and earlier notes here
recommended it), but it is wrong: with AMFI out of the way *nothing* vouches for the dext binary, and
the kernel refuses to exec it. The dext stages and activates fine, then dies at launch with
`DK: … failed to launch server` / `Exec format error` (POSIX 8) in the log, which looks nothing like a
boot-arg problem. Ad-hoc signing + SIP off + developer mode is what makes this work; AMFI-off breaks
it. Verify with `nvram boot-args` before you reboot — it must not mention `amfi`.

## 2. Build + install it

```
bash platform/loader/build-and-sign.sh
# → /Applications/UFLoader.app   (dext embedded, both ad-hoc signed)
```

The script installs to `/Applications` on purpose — **sysextd refuses to activate an extension whose
host app lives anywhere else** — and drops the build-dir copy from the LaunchServices database so the
app can't resolve to a stale path. Don't run the app out of `platform/loader/build`.

## 3. Run it

Plug in the FF800 (via the TB→FireWire adapter), then:

```
open /Applications/UFLoader.app
# or run the binary directly to see the log inline:
/Applications/UFLoader.app/Contents/MacOS/UFLoader
```

The first run needs approval: **System Settings → General → Login Items & Extensions → Driver
Extensions**. Ground truth for the extension's state is `systemextensionsctl list`, not that pane —
the toggle reads "off" whenever the dext isn't *running*, which is normal when no FireWire adapter is
plugged in for it to match.

Expected: `→ Submitted activation request …`, then `✓ Activation result: 0`, then
`✓ RME FF800: firmware register 0x……, status 0x……`.

Watch the dext's own logs alongside:
```
log stream --predicate 'sender == "UFFireWireOHCI"' --style compact
```
You should see `OHCI Version=…`, `self-ID complete`, node lines, and `*** FF800 found at node … ***`.

## Troubleshooting

`OSSystemExtensionErrorDomain Code=4 "Extension not found in App bundle"` is a catch-all — it means
sysextd declined to consider the extension, and almost never that the file is missing. The three
causes, all now handled by `build-and-sign.sh`:

- **The dext's bundle id must be prefixed by the host app's bundle id.** `com.userface800.UFLoader.UFFireWireOHCI`
  under `com.userface800.UFLoader`. A sibling id (`com.userface800.UFFireWireOHCI`) is silently skipped.
- **The dext's bundle *directory* must be named after its `CFBundleIdentifier`** —
  `com.userface800.UFLoader.UFFireWireOHCI.dext`. xcodebuild names it after the target
  (`UFFireWireOHCI.dext`), which sysextd skips without a word. This is the one that bites.
- **The host app must be in `/Applications`.**

Ignore `no policy, cannot allow apps outside /Applications` in the sysextd log — it is emitted on any
Mac with no MDM policy, regardless of where the app lives, and is not the rejection.

Other:
- **`systemextensionsctl list`** shows the real activation state.
- **dext activated but no service** → nothing matched: check the OHCI enumerates
  (`scripts/probe-fw-adapter.sh`) and look for `DK:` lines from the kernel in the log.
- **ReadQuadlet fails but the dext loaded** → the async AR path needs hardware iteration (the AR
  ring is the simplified control-rate version; see `ohci-dext/README.md`).
