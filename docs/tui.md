# uf-tui — the terminal mixer

`uf-tui` is a terminal front-end for the Fireface 800's TotalMix matrix: crosspoint gains,
per-output faders, mutes, phase, loopback, stereo pairing and submix copy/clear, plus live input
and playback meters.

Like every other front-end it is a **client of `uf-daemon`**. It never opens the dext itself: it
reads the daemon's published state from the `/userface800.state` shared-memory segment and sends
changes over the daemon's control socket. If the daemon is not running, `uf-tui` starts and draws
its frame, but the header reads `no daemon state — is uf-daemon running?`, the grid is empty, and
the status line says `not connected: no daemon listening at <path> — is uf-daemon running?`.
Nothing is sent until the daemon is there and `uf-tui` is restarted: the connection is made once,
at startup.

The display refreshes on a 20 Hz timer, so changes made by another client (`uf-mix`, `uf-set`), a
rate change or the device losing lock appear on their own. For the command-line tools see
[`tools.md`](tools.md).

## Running it

```bash
build/uf-tui
```

It takes no arguments or options. Two environment variables affect it, both by way of the shared
control-socket client:

| variable | effect |
|---|---|
| `UF_CTL_SOCKET` | full path of the daemon's control socket, overriding the default |
| `TMPDIR` | directory the default socket path `userface800.ctl` is looked up in (else `/tmp/`) |

If the daemon was built from a different tree, the version checks say so rather than drawing
nonsense: a state-segment layout mismatch replaces the header, and a control-protocol mismatch is
refused at the handshake and named on the status line. Both messages ask for a rebuild.

## The screen

```
+---------------------------------------------------------------------------+
|                          userface800 — TotalMix                           |
|               48000 Hz   clock Internal (locked)   28 ch                  |
|  1 inputs    2 playback    3 outputs           submix: Analog 1   v focus |
|---------------------------------------------------------------------------|
| src              A1   A2   A3   A4   A5   A6   ...                        |
| Analog 1[ ==|     0    ·    ·   -6    ·    ·                              |
| Analog 2] ==|     ·    0    ·    ·    ·    ·                              |
| Mic 9      =      ·    ·  MUTE   ·  ø-12   ·                              |
|---------------------------------------------------------------------------|
| Analog 1  ->  Analog 1     gain set                                       |
| 1/2/3 or tab switch   arrows move   pgup/pgdn page   home/end ends  q quit |
| , . gain +/-1 dB   u or 0 unity   m mute   p phase   l loopback   ...     |
+---------------------------------------------------------------------------+
```

**Header.** The title, then one status line. While streaming it reads `<rate> Hz   clock <source>
(locked)` or `(NOT locked)`, then the channel count on the wire at that rate — 28, 20 or 12. With
the daemon up but no device streaming it reads `daemon up, not streaming (no device?)`; with no
daemon state at all it carries the attach error instead.

**Tabs.** Three views of the manual's three rows, switched with `1`, `2`, `3` or `Tab`:

- **1 inputs** — hardware inputs to hardware outputs (zero-latency monitoring).
- **2 playback** — software playback streams to hardware outputs.
- **3 outputs** — the hardware outputs themselves: fader, mute, loopback.

The **selected output** is shared by all three tabs and named in the tab bar (`submix: …`). On
tabs 1 and 2 it is the column being edited; tab 3 is how you pick it. Each source tab keeps its own
cursor, so switching back returns you to the row you left. Beside the name is the `v focus`
indicator: highlighted when submix view is on (every column but the selected one is dimmed), dim
when off.

**Tabs 1 and 2 — the matrix.** Sources down the side, hardware outputs across the top, a gain at
each crosspoint. The row labels and meters stay put while the grid scrolls sideways. Each row is:

| part | meaning |
|---|---|
| name | the channel's own name, 9 columns (`Analog 1`, `Mic 9`, `ADAT 3`, `SPDIF L`, `Phones 9`) |
| `[` / `]` | 9th column: the channel is paired into stereo, `[` on the even half, `]` on the odd |
| meter | 5 characters: `=` fills to RMS, `\|` marks peak, −60 dBFS left, 0 dBFS right |
| cells | one crosspoint per hardware output |

Column headings are abbreviated to a group initial plus the number — `A1`, `M9`, `P9`, `SL`, `D3`.
The full names of whatever the cursor is on are spelled out on the line below the grid.

A cell shows `0` at unity, a centred `·` when it is off, `MUTE` when muted, otherwise the gain in
whole dB. A phase-inverted crosspoint spends the sign column on a `ø` marker. Muted cells are blue,
inverted cells red, anything else carrying gain is green. The meter goes yellow near full scale and
red once a sample hits 0 dBFS.

**Tab 3 — the outputs.** One line per hardware output: name, fader level (same notation as a
cell), `MUTE` when muted, `LOOPBACK` when loopback is on, and a count of how many unmuted
sources — inputs
and playback together — currently feed that output. There is no meter column: hardware outputs are
summed by the device's own DSP and never come back to the host.

**Bottom.** The line under the grid names what the cursor is on in full (`Analog 1  ->  Analog 1`,
or `output Phones 9`), followed by the result of the last action — `gain set`, `mute toggled`,
`submix copied`, or an error such as `not connected: …`. Two dim lines of key hints follow.

## Keys

The gain and unity keys act on the **crosspoint under the cursor** on tabs 1 and 2, and on the
**selected output's fader** on tab 3; so does `m`.

| key | what it does |
|---|---|
| `1` / `2` / `3` | switch to the inputs, playback or outputs tab |
| `Tab` | cycle inputs → playback → outputs → inputs |
| `↑` / `↓` | move one row: the source on tabs 1 and 2, the selected output on tab 3 |
| `PgUp` / `PgDn` | move 12 rows |
| `Home` / `End` | jump to the first or last row |
| `←` / `→` | move the selected output (the submix), on every tab |
| `.` or `+` or `=` | gain up 1 dB |
| `,` or `-` or `_` | gain down 1 dB |
| `u` or `0` | set unity (0 dB) |
| `m` | toggle mute — of the crosspoint, or of the output on tab 3 |
| `p` | toggle phase invert of the crosspoint (tabs 1 and 2) |
| `l` | toggle loopback for the selected output; on tabs 1 and 2 it says it is an output setting |
| `s` | pair the cursor's channel into stereo, or split it — tabs 1 and 2 only |
| `c` | copy submix: press once on the source output, then again on the destination |
| `x` | clear submix — drop everything feeding the selected output, leaving its fader alone |
| `v` | toggle submix view (dim every output but the selected one) |
| `q` or `Esc` | quit |

There is no fine/coarse modifier: the step is always 1 dB, on a whole-dB grid, between −65 dB and
+6 dB. The ends are special, so a route can be switched on and off from the keyboard: stepping
**up** from a silent crosspoint goes straight to unity, and stepping **down** from the bottom of
the range turns it off rather than settling at −65 dB.

## What a change does

Every edit is a message to `uf-daemon`, never a register write from this process. The daemon folds
it into its mixer model, renders the affected cells to the device's matrix RAM (coalesced and rate
limited, so a submix copy lands over a few milliseconds), republishes the state every client reads,
and hands a snapshot of the model to a thread that writes it to disk. Because the model — not the
rendered matrix — is what is kept and re-applied, a mute keeps the gain underneath it, and the
whole mixer is restored on every session start: after a rate change, a wedge recovery, a bus reset,
and after the daemon itself is restarted.

## Notes

- The grid is always 28 sources by 28 outputs, the 1x channel map. At 88.2/96 and 176.4/192 kHz
  fewer channels exist on the wire (the `ch` count in the header) but the rows and columns are not
  reduced.
- The header shows the sample rate, the clock source and whether it is locked; none of the three
  can be changed here, and phantom power, input and output levels and the SPDIF settings are not
  shown at all. Use `uf-set` for those.
- Pan is not an edit this UI offers. A source panned across a stereo output pair is just its two
  crosspoints at different gains, which can be set cell by cell here, or in one step with `uf-mix`.
- Stereo pairing is host-side only. Nothing is written to the device; it changes how a channel is
  drawn and is published and persisted so every front-end agrees about it.
- There are no hardware-output meters, no solo, and no undo.
- The connection to the daemon is made once at startup. If the daemon is started or restarted
  afterwards, restart `uf-tui` as well.
