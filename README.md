# UserFace800

A userland macOS driver for the **RME Fireface 800** FireWire audio interface. No kernel extension.

Apple removed the FireWire audio class driver from modern macOS, and on Apple Silicon there is no
kernel FireWire stack at all. UserFace800 supplies the whole path from user space:

| Piece | What it is |
|---|---|
| `platform/ohci-dext/` | a PCIDriverKit dext implementing 1394 OHCI — async transactions, isochronous receive + transmit, self-ID/bus reset, config ROM — bound to the Thunderbolt→FireWire adapter's controller |
| `platform/daemon/` | `uf-daemon`, which owns the FF800 through the dext, runs the streaming session, and bridges audio over lock-free shared memory |
| `platform/coreaudio/` | `UFAudioDriver`, an AudioServerPlugIn that presents the FF800 to Core Audio |
| `platform/midi/` | a CoreMIDI bridge for the FF800's DIN ports (also carried inside `uf-daemon`) |
| `tools/`, `tui/` | command-line tools and a terminal mixer |

`include/uf/protocol/` is the portable protocol core — registers, packet codec, channel maps, MIDI
framing, mixer math — pure C++ with no macOS dependencies, unit-tested with `ctest`.

## Status

Working on hardware:

- **Capture and playback** at every native rate (44.1/48, 88.2/96, 176.4/192 kHz), full channel
  count for the speed class (28/20/12).
- **MIDI** in and out, as a CoreMIDI source and destination.
- **matrix mixer** — crosspoint gains, phase, mutes, output faders, per-output loopback. Routes
  survive a rate change, a bus reset and a daemon restart.
- **Metering** — per-channel peak and RMS for inputs and playback.
- **Sleep/wake** — the dext rebuilds the controller on wake and the daemon rides the gap out.
- **Bus-reset recovery** — topology changes mid-stream are re-identified and the session re-armed.

## Requirements

- An **Apple Silicon Mac** running **macOS 26 (Tahoe)**.
- A **Thunderbolt→FireWire adapter** (Apple's, or an equivalent with a tunnelled 1394 OHCI
  controller).
- An **RME Fireface 800**.
- The machine must be set up to load a self-signed DriverKit extension: **Reduced Security**, **SIP
  disabled**, `systemextensionsctl developer on`, and the `-arm64e_preview_abi` boot-arg. Do **not**
  set `amfi_get_out_of_my_way` — it stops the dext executing.

  This lowers the machine's security globally; it is a development-machine setup.
  **Read [`platform/loader/LOADING.md`](platform/loader/LOADING.md) before you start.**

## Build

Build natively on macOS (Xcode + CMake, C++23).

```bash
bash scripts/fetch-refs.sh                 # reference drivers -> ref/ (first time only)

cmake -B build && cmake --build build && (cd build && ctest)
# the portable core and its tests everywhere; on macOS also every CLI tool, uf-tui and uf-daemon

bash platform/loader/build-and-sign.sh     # the OHCI dext + UFLoader.app, ad-hoc signed

cmake -S platform/coreaudio -B platform/coreaudio/build
cmake --build platform/coreaudio/build     # -> UFAudioDriver.driver
```

Everything from the top-level build lands in `build/`.

## Install

1. **The dext.** `build-and-sign.sh` installs `UFLoader.app` into `/Applications`; run it once to
   activate the extension, then approve it in System Settings → General → Login Items & Extensions →
   Driver Extensions. `systemextensionsctl list` is the real state.
2. **The daemon.** `platform/launchd/install.sh` installs it as a per-user LaunchAgent (no sudo);
   `uninstall` and `status` are the other two subcommands.
3. **The Core Audio plug-in.** Copy `platform/coreaudio/build/UFAudioDriver.driver` into
   `/Library/Audio/Plug-Ins/HAL/` and restart `coreaudiod`.

## Tools

All build into `build/`. They are clients of `uf-daemon` — they talk to its control socket and read
its published state rather than opening the dext themselves, which is what keeps two processes from
interleaving register transactions and what lets a setting survive the next rate change.

| tool | what it does |
|---|---|
| `uf-daemon` | owns the device, streams isochronous audio, bridges to the plug-in and to CoreMIDI |
| `uf-tui` | terminal mixer: the live matrix, arrows to move, `u`/`m`/`+`/`-` to set |
| `uf-status` | decode and print the device's status registers — clock, lock, sync, rate |
| `uf-set` | change settings: clock source, sample rate, phantom, input/output levels, SPDIF |
| `uf-mix` | drive the matrix from the command line: routes, faders, mutes, phase, loopback, pan |
| `uf-capture` | record raw isochronous capture to a WAV file |
| `uf-midi` | standalone MIDI bridge — do not run it while `uf-daemon` is running, which carries the same bridge |
| `uf-probe` | read/write arbitrary registers; config-ROM identification |
| `uf-init` | run the power-on init sequence by hand |
| `uf-flash` | read the on-device settings/mixer flash (writes are destructive and gated) |
| `uf-busreset` | force a FireWire bus reset, to exercise topology-change recovery |
| `uf-testsig` | generate and verify a self-describing bit-exactness test signal (needs no hardware) |

Every flag, subcommand and environment variable is documented in
[`docs/tools.md`](docs/tools.md); the terminal mixer has its own page,
[`docs/tui.md`](docs/tui.md).

## Protocol reference

[`spec/`](spec/) documents the FF800 protocol the driver implements: the 48-bit register map, the
CR/SR bitfields, the isochronous packet format and channel maps, the streaming lifecycle, MIDI
framing, the matrix mixer maths, metering and the on-device flash. Facts still carrying a `[?]`
are flagged there.

## Licence

**GPL-2.0-only.** Parts are ported from snd-fireface, the Linux firewire core and FFADO;
snd-fireface is GPL-2.0-*only*, which binds the whole. See [`COPYRIGHT`](COPYRIGHT) for the upstream
sources and per-file attribution, and [`LICENSE`](LICENSE) for the full text.
