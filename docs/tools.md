# Command-line tools

Everything here builds into `build/`. Most of these tools are clients of `uf-daemon`: they send
commands to its Unix control socket and read the state it publishes in shared memory, rather
than opening the dext themselves. That is what keeps register access serialised — the dext
serialises nothing and its single AT context is shared, so two processes issuing transactions
can interleave — and what makes a setting survive the next rate change, because the daemon
holds the shadow it re-applies on every session start. With no daemon running, `uf-status`
still works: it reads the device's status registers itself and reports that the daemon is not
running; `uf-testsig` needs no hardware at all; `uf-probe`, `uf-init`, `uf-flash`, `uf-capture`,
`uf-busreset` and `uf-midi` go straight to the dext, and should not be run while the daemon is
streaming. `uf-set` and `uf-mix` (except `uf-mix raw`) need the daemon; without it they print
`no daemon listening at <path> — is uf-daemon running?` and exit 1. The terminal mixer is
documented separately in [`tui.md`](tui.md).

The socket is `$TMPDIR/userface800.ctl` (`/tmp/userface800.ctl` if `TMPDIR` is unset), and the
published state segment is `/userface800.state`. Both carry a version that each side checks, so
a tool built from a different tree is refused rather than allowed to misread a struct.

## uf-daemon

`uf-daemon` owns the FF800. It opens the dext, runs the streaming session (rate, packet format,
transmit channel, comm start, PCM fetch), creates the capture and playback shared-memory rings
the Core Audio plug-in maps, carries the CoreMIDI bridge, holds the mixer model and the settings
shadow, publishes state and meters, and re-applies all of it after a rate change, a wedge
recovery or a bus reset. It also serves the control socket, which comes up *before* it looks for
hardware: a front-end can connect and set routes while the daemon is still waiting for the
device, and those commands are applied when the device appears.

Normally it runs as a per-user LaunchAgent:

```bash
platform/launchd/install.sh            # install + start (no sudo)
platform/launchd/install.sh status     # loaded? running? a verdict from the log
platform/launchd/install.sh uninstall  # stop + remove
```

The agent starts at login, restarts if it exits (10 s throttle), logs stdout and stderr to
`~/Library/Logs/UserFace800/uf-daemon.log`, and sets `UF_QUIET=1` there. For a session you are
watching, run `./build/uf-daemon` in a terminal instead — then the once-a-second telemetry lines
(`REAL:`, `IRQ:`, `TX:`, `MARGIN:`, `ANCHOR:`, `STATUS:`) are printed too. Events — session up,
restarts, wedges, errors — are never suppressed. Ctrl-C or `SIGTERM` stops it cleanly: it saves
the mixer state, stops the session and unlinks its shared memory. The mixer model and stereo
pairing live in `~/Library/Application Support/UserFace800/state.bin`, written about a second
after they stop changing and again at shutdown.

### Environment variables

| Variable | Default | What it does |
|---|---|---|
| `UF_RATE` | `48000` | Rate the first session starts at. |
| `UF_QUIET` | unset | Drop the per-second telemetry. Events still log. |
| `UF_NO_MIDI` | unset | Do not create the CoreMIDI endpoints. |
| `UF_STATE_FILE` | `state.bin` (see above) | Where the mixer state is saved and restored. |
| `UF_CTL_SOCKET` | `$TMPDIR/userface800.ctl` | Control socket path; must match for tools. |
| `UF_NODIAG` | unset | Start empty instead of playback N to output N at unity. |
| `UF_METER_DEVICE` | unset | Also poll the device's meter region; raw, and costs bus traffic. |
| `UF_CAP_WAV` | unset | Write decoded capture to this WAV, before the shm ring. |
| `UF_CAP_SECONDS` | `10` | How much to capture for `UF_CAP_WAV`. |
| `UF_CAP_SIGNAL` | unset | Hold that capture until `SIGUSR1`; each signal restarts it. |
| `UF_WEDGE_MS` | `500` | How long capture may stall before the session is rebuilt. |
| `UF_NO_WEDGE_RECOVERY` | unset | Never rebuild a stalled session. |
| `UF_MAX_RESTART_FAILURES` | `5` | Failed rebuilds before the daemon exits to reconnect. |

Diagnostics, for bring-up and A/B testing only: `UF_NO_IRQ`, `UF_NO_RT`, `UF_NO_TX`,
`UF_NO_SERVO`, `UF_TX_FREERUN`, `UF_TX_FPP`, `UF_TX_TONE`, `UF_TX_CH`, `UF_TX_FREQ`,
`UF_TX_PACED_TONE`, `UF_TX_VIBRATO`, `UF_TX_VIB_DEPTH`, `UF_TX_SILENT`, `UF_TX_WAV`,
`UF_MIXER_SKIP`, `UF_FETCH`, `UF_FETCH_SKIP`, `UF_FETCH_DELAY_MS`, `UF_REC`, `UF_REC_SKIP`,
`UF_FC88F`, `UF_RATE_LATCH`, `UF_MIDI_HOST`, `UF_RECLAIM_S`, `UF_STEP_PAUSE`, `UF_PROBE_ADDR`,
`UF_PROBE_VAL`, `UF_PROBE_PERIODIC`, `UF_CYCLE_RESTART`, `UF_CYCLE_GAP`, `UF_DEBUG_INBOUND`.

## uf-status

Answers "is this working, and if not what do I do?" — the system view first, then the daemon's
settings and routing, then the device's clock and per-input lock.

### Usage

```
uf-status
```

It takes no arguments. It prints whether the dext is loaded and matched, whether the daemon is
running and streaming, and the current rate and clock. If the daemon has published compatible
state, its settings shadow (the only place those values exist — the FF800's settings register is
write-only) and the non-default parts of the mixer come next: live crosspoints, mutes, phase
inversions, non-unity or muted outputs, loopbacks and stereo pairs. That half is printed whether
or not a device is attached. With the dext open it then reads the status registers and decodes
clock, sync source and per-input lock; frequencies are shown only for inputs that are locked.

Exit status is 0 for a full report, 1 if the dext could not be opened or the FF800 did not
answer the register reads — in both cases guidance naming the remedy goes to stderr.

```console
$ uf-status
dext:    loaded, matched to the adapter
daemon:  running, streaming
rate:    48000 Hz  (clock Internal, locked)
```

## uf-mix

Drives the TotalMix matrix: crosspoints, faders, mutes, phase, loopback, stereo pairing, pan and
whole-submix copy or clear. Channel numbers are 1-based, as on the front panel, and must be
within 1..28; dB may be negative or fractional and defaults to 0 (unity).

### Usage

```
uf-mix [-n] <command> [args]
```

| Command | What it does |
|---|---|
| `in <src> <dst> [dB]` | Physical input `src` to output `dst`. |
| `pb <src> <dst> [dB]` | Playback channel `src` to output `dst`. |
| `fader <out> [dB]` | Per-output master fader. |
| `mute in\|pb <src> <dst> on\|off` | Mute a crosspoint, keeping the gain under it. |
| `phase in\|pb <src> <dst> on\|off` | Invert that crosspoint 180 degrees. |
| `omute <out> on\|off` | Mute a hardware output. |
| `loopback <out> on\|off` | That output's mix replaces the input in the recorder. |
| `stereo in\|pb <ch> on\|off` | Pair `ch` with its neighbour. Host-side only. |
| `pan in\|pb <src> <dstL> <-1..1> [dB]` | Spread across the `dstL` output pair. |
| `copy <fromOut> <toOut>` | Copy a whole submix column. |
| `clear <out>` | Drop everything feeding an output; its fader is left alone. |
| `raw <addr> <value>` | Raw quadlet write, hex accepted. Direct to the device. |
| `-n` | Dry run: print the transaction, write nothing. Must come first. |

`on`/`off` also accept `1`/`0`. `pan` is constant-power (−3 dB at centre) and is written as two
ordinary crosspoints, onto `dstL` and its partner — pairs are `(1,2)`, `(3,4)` and so on, so
`pan in 9 1 -0.5` spreads input 9 across outputs 1 and 2, half left. Every command except `raw`
goes through the daemon, so it survives a rate change and a daemon restart; `raw` opens the dext
itself, shares the AT context with a running daemon, and does not survive a session restart.

Each command first prints what it will do (as `wq(addr, value)` where there is a register behind
it), then `sent` once the daemon has taken it. Bad arguments exit 2, a failed send exits 1. `--phantom=` takes whole channel numbers, 7 to 10, in any order and with any
separator; any other number exits 2.

```console
$ uf-mix in 3 1 -6
wq(0x80080008, 0x00004027)
sent

$ uf-mix -n loopback 5 on
loopback out 5 = on  [dry-run]
```

## uf-set

Changes the Fireface settings: clock source and sample rate, levels, phantom power, input source
select, the channel-1 instrument options and SPDIF. Everything it touches is volatile device
state — a power cycle resets it — but the daemon adopts the settings block, so it is re-applied
on every session start and remembered across daemon restarts.

### Usage

```
uf-set [options]
```

| Flag | Default | What it does |
|---|---|---|
| `--master`, `--autosync` | `--master` | Internal master clock, or slave to `--sync`. |
| `--sync=adat1\|adat2\|spdif\|wclk\|tco` | `adat1` | Reference for `--autosync`. |
| `--rate=<hz>` | `48000` | 32000 to 192000; see below. |
| `--wclk-1x` | off | Single-speed word-clock output. |
| `--in=lo\|+4\|-10` | `+4` | Input level. |
| `--out=hi\|+4\|-10` | `+4` | Output level. |
| `--phones=+4\|-10\|hi` | `+4` | Headphone level. |
| `--phantom=<list>` | all off | Phantom power on the listed mic inputs, e.g. `7,8` or `9,10`. |
| `--input1=`, `--input7=`, `--input8=` | unset | `front` or `rear` jack for that input. |
| `--filter` | off | Channel-1 instrument filter. |
| `--drive` | off | Channel-1 drive. |
| `--no-limiter` | limiter on | Disable the limiter (front instrument input only). |
| `--spdif-in=coax\|optical` | `coax` | SPDIF input. |
| `--spdif-out=coax\|optical` | `coax` | SPDIF output. |
| `--spdif-pro`, `--spdif-emphasis`, `--spdif-nonaudio` | off | SPDIF output status bits. |
| `--print` | — | Show what would be written, then stop. |
| `--help`, `-h` | — | Usage, exit 0. |

Accepted rates are 32000, 44100, 48000, 64000, 88200, 96000, 128000, 176400 and 192000; an input
left unset emits no bits for it. Flags not given take their default, so `uf-set --phantom=7` also
puts everything else back to the defaults — give the whole state you want in one invocation. An
unknown flag, an unknown `--sync` or input-jack value, or an unsupported rate exits 2; a failed
send exits 1.

```console
$ uf-set --autosync --sync=adat1 --rate=44100
clock:  autosync/slave (sync=ADAT1)   rate: 44100 Hz
inputs: 1=(unset) 7=(unset) 8=(unset)   limiter: on
conf block  0xfc88f014 <- CR0=... CR1=... CR2=...
clock cfg   0x801c0004 <- ...
sent (volatile on the device — a power cycle resets it, but the daemon re-applies
      these settings on every session start, and remembers them across restarts)
```

## uf-capture

Opens its own streaming session through the dext, captures the device's isochronous stream,
decodes it and writes a WAV, then reports peak level per channel. **Do not run it while
`uf-daemon` is streaming** — it starts and stops a session of its own.

### Usage

```
uf-capture [--rate=48000] [--packets=N] [--bank=0002|fc88f] [--hold=N] [--led-sweep] [out.wav]
```

| Flag | Default | What it does |
|---|---|---|
| `--rate=<hz>` | `48000` | Session rate; sets channel count and frames per packet. |
| `--packets=<n>` | `512` | Packets to capture. Must be 1..512 or the tool exits 2. |
| `--bank=0002` | selected | Drive streaming from the `0x0002` register bank. |
| `--bank=fc88f` | — | Use the legacy `fc88f` bank instead. |
| `--hold=<sec>` | `0` | Hold the session open, then close it and exit. |
| `--led-sweep` | off | Sweep the host-LED register, ~4 s a value, then exit. |
| *(positional)* | `capture.wav` | Output path; any non-flag argument is taken as it. |

`--led-sweep` and `--hold` both close the session and exit without writing a WAV, and
`UF_DUMP` in the environment dumps the head of the first two capture slots before decoding. The
session is always torn down before exit. It exits 1 if the dext is unavailable, if the device
never published a transmit channel, if no packets arrived, or if they decoded to no frames.

```console
$ uf-capture --packets=512 in.wav
streaming bank: 0x0002 (RME default)
rate=48000  channels=28  frames/packet=8  packets=512
device transmits on iso channel <n>
playback stream: channel 1, blocking 8 frames/full pkt, 480/640 full
session started, capturing... (host-LED claim written)
captured 512/512 packets
```

A per-channel peak table in dBFS and `wrote <path>: ... frames, ... channels, ... Hz` follow.

## uf-testsig

Generates a self-describing test signal, and analyses a recording of it. Every sample encodes
its own channel and frame index, so a recording can be checked for dropped, duplicated or
reordered samples and for channel shear. Needs no hardware and no daemon.

### Usage

```
uf-testsig gen   <out.wav> [--rate=96000] [--channels=8] [--seconds=30]
uf-testsig check <recorded.wav> [--channel=0] [--expect=<channel id>]
```

| Flag | Default | What it does |
|---|---|---|
| `--rate=<hz>` | `96000` | Sample rate of the generated WAV. |
| `--channels=<n>` | `8` | Channels to generate. Out of range exits 2. |
| `--seconds=<n>` | `30` | Length; frames written are rate x seconds. |
| `--channel=<n>` | `0` | Which column of the recording to analyse. |
| `--expect=<id>` | `--channel` | Encoded channel id that column should carry. |

A `--channel` beyond the file's channel count exits 2. `check` reads 24-bit PCM only — a
converted file is not bit-exact and is rejected — and the path must be bit-transparent end to
end: DAW at unity, no plugins, no dither, mixer at unity. It exits 0 when the recording is
clean, 1 when faults were found or no encoded signal was present at all.

```console
$ uf-testsig gen sig.wav --channels=2 --seconds=5
wrote sig.wav: 2 ch, 96000 Hz, 480000 frames (5 s), 24-bit

$ uf-testsig check rec.wav --channel=0
rec.wav: 8 ch, 96000 Hz, 2880000 frames (30.00 s)
column 0 (expecting channel id 0): 2880000 frames checked
  gaps=0 (dropped 0 frames)  repeats=0  backwards=0  wrong-channel=0  not-encoded=0
CLEAN — bit-exact end to end
```

## uf-midi

A standalone CoreMIDI bridge: a destination `Fireface 800 MIDI Out` (apps to FF800) and a source
`Fireface 800 MIDI In` (FF800 to apps). MIDI out block-writes to the device through the dext;
MIDI in is hardware-gated and has never received a packet. **Do not run it while `uf-daemon` is
running** — the daemon carries the same bridge, and two processes with their own user clients
can interleave inside one transaction.

### Usage

```
uf-midi
```

No arguments. The endpoints are created whether or not the dext is available; with no dext it
prints `uf-midi: dext not available yet — endpoints will be idle` and the bridge is inert.
Ctrl-C or `SIGTERM` stops it.

## uf-probe

Reads a region of the FF800's address space quadlet by quadlet, or watches it for changes. It
writes nothing, but it opens the dext directly, so it shares the AT context with a running
daemon.

### Usage

```
uf-probe [--watch] [--period=MS] <addr> [count]
```

| Flag | Default | What it does |
|---|---|---|
| `--watch` | off | Loop, printing only quadlets that changed. Ctrl-C to stop. |
| `--period=<ms>` | `100` | Interval between reads in watch mode. |
| `<addr>` | — | Base address, hex accepted. Required. |
| `[count]` | `16` | Quadlets to read. Less than 1 exits 2. |

A one-shot read prints how many quadlets answered and dumps them four per line; a quadlet whose
transaction failed prints as `--------`. It exits 1 if no quadlet in the region answered, or if
the dext user client is unavailable. Watch mode runs until interrupted.

```console
$ uf-probe --watch --period=50 0x80100000 254
watching 0x080100000 (254 quadlets), 50 ms period — Ctrl-C to stop
(printing only quadlets whose value changes)
```

## uf-init

**Writes to the device.** Assembles the settings conf block from the default shadow (internal
master clock, +4 dBu in and out, coax SPDIF) and block-writes it, then proves the write landed
by checking that the clock-master bit flipped. What it writes is volatile — a power cycle resets
it — and it writes the register directly rather than going through the daemon, so do not run it
while the daemon is streaming.

### Usage

```
uf-init
```

No arguments, no flags. It prints the status registers before and after, and exits 0 if the
master bit went high (`*** the device switched to internal master clock — the write landed ***`)
or 1 if it did not, or if the write itself failed.

## uf-flash

Reads — and, gated, erases — the FF800's on-device flash, where the unit persists its settings
and mixer state for standalone use.

### Usage

```
uf-flash rev
uf-flash read <addr> <nquads>
uf-flash dump-settings
uf-flash dump-mixer [nquads]
uf-flash erase-settings --force
```

| Subcommand | What it does |
|---|---|
| `rev` | Print the firmware revision, hex and decimal. |
| `read <addr> <nquads>` | Dump quadlets from a flash address; hex accepted. |
| `dump-settings` | Dump the settings record: 64 quadlets, one 256-byte sector. |
| `dump-mixer [nquads]` | Dump the mixer-shadow region; `nquads` defaults to 64. |
| `erase-settings --force` | **Destructive.** Erase the settings block. |

Reads are safe, and `read` requires both arguments. `erase-settings` is gated: without `--force`
it prints `erase is DESTRUCTIVE — pass --force. UNTESTED on hardware.` and exits 2 without
touching anything. With `--force` it writes the erase register, then polls until the flash
reports ready, printing `erased (flash ready)`, or exiting 1 with `timeout waiting for flash
ready`. It is
untested on hardware, and there is no write subcommand. Like the other dext tools it opens the
user client directly: exit 1 if that fails, 2 for an unknown or incomplete command.

```console
$ uf-flash rev
firmware revision: 0x00000001 (1)
```

## uf-busreset

**Acts on the bus.** Forces a FireWire bus reset through the dext, which renumbers every node —
the same thing that happens when a cable is nudged or another device powers up. It exists to
exercise topology-change recovery on demand, and is meant to be run while audio is playing.

### Usage

```
uf-busreset [count]
```

`count` defaults to 1; anything below 1 is treated as 1. There are no flags. With more than one,
it waits 2 s between resets so the bus can settle and the daemon can react. Each reset prints
its kernel return code, and the tool exits 0 only if every reset succeeded.

Expect, in order: the dext logs a bus reset and re-identifies the FF800, capture stalls briefly,
and the daemon reports `RESTART (capture stalled — device wedged)` and rebuilds. Silence with no
recovery, or a permanent `no tx channel`, is a fault.

```console
$ uf-busreset 2
bus reset 1/2 -> 0x0
bus reset 2/2 -> 0x0
```

Every reset line ends with `  (FAILED)` when the call did not succeed.

## Troubleshooting

| Message | Cause |
|---|---|
| `no daemon listening at <path>` | No daemon, or `UF_CTL_SOCKET`/`TMPDIR` differ. |
| `protocol version mismatch` | Tool and daemon from different trees; rebuild both. |
| `dext user client not available` | Dext not loaded, or no adapter attached. |
| `dext: NOT LOADED` (uf-status) | Driver never activated; see `platform/loader/LOADING.md`. |
| `dext: activated, but NOT matched` | Attach the adapter, or an old dext is still bound. |
| `the FF800 is not answering register reads` | Check power and the FireWire cable. |
| `no tx channel` (daemon log) | The device is wedged and needs a power cycle. |
| `device never published a TX channel` | A previous run left it mid-stream; power-cycle it. |
| `packets must be 1..512` | `uf-capture --packets` is out of range. |
| `master bit did NOT flip` | `uf-init`'s write was acked but did not take effect. |
| `mostly unencoded` (uf-testsig) | The path is not bit-transparent: gain, dither or resampling. |
| `no encoded signal found` | Silence on that column, or the same non-transparent path. |
