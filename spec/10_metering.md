# 10 — Level metering (FF800)

Status: the **transport is confirmed on hardware** (`[HW-CONFIRMED]`): the FF800 exposes a
**pollable meter region at `0x80100000`** — plain async quadlet reads return live per-channel level
data that tracks signal, with no session and no registration needed. FFADO does not implement FF800
metering, so there is no reference to cross-check against. The exact dB calibration is still `[?]`.

## 10.1 Meter element (decoded format)
Per channel, the meter is a pair:

| field | type | meaning |
|---|---|---|
| `peak` | `u32` | peak sample magnitude, **Q27** (full-scale = `2^27` = 0x8000000) |
| `rms_acc` | `u64` | running sum of squares, Σ(sample²) |

Conversions (the scaling the FF800's own metering uses):
```
peak_norm = peak    * 2^-27          // → [0,1] linear
meansq    = rms_acc * 2^-55          // → mean-square, linear
dB_peak   = 20 * log10(peak_norm)    // clamp -inf at peak==0
dB_rms    = 10 * log10(meansq)       // power already squared → 10·log10
```
The `2^-55` = `2^-54 · ½` implies `rms_acc` is Σ(sample²) with sample in Q27 and an implicit ÷2 (or ÷N
block-average) normalisation; the value of `N` is `[?]`.

## 10.2 Bank / channel layout `[?]` for the wire
A full metering surface is **5 banks × 32 channels** of the §10.1 element: five `u32[32]` peak
sub-arrays then five `u64[32]` rms sub-arrays. The 5 taps are pre/post-fader + submix; an FF800
(28 ch @ 1×, fewer at 2×/4×) occupies the low channels of each bank. The exact FF800 tap→bank and
channel→index mapping is `[?]` until confirmed on hardware.

## 10.3 Transport — pollable at `0x80100000` `[HW-CONFIRMED]`
The FF800 meters are **pollable** at `0x80100000`. Read it with ordinary async quadlet reads (no
session, no async-push registration needed); the values update continuously.

### On-wire layout `[HW-CONFIRMED]` (differs from the §10.2 format)
- **Stride = 8 bytes/channel** (2 quadlets), NOT the 12-byte `{u32 peak, u64 rms}` element of §10.2.
- **Channel N is at offset `N*8`** (0-indexed): ch1 @ 0x00, ch9 @ 0x40, …
- Only the **first ~10 slots** (through ~offset 0x50) carry data — the FF800's **10 analog inputs**.
  The location of the ADAT/SPDIF meters is `[?]`.

### The 8-byte slot `[?]` (format inference, not calibrated)
The 8 bytes read as a **little-endian `u64`**: a silent channel holds a small value (order
`0x0000_0000_03xx_xxxx`), a channel carrying signal one many orders of magnitude larger (order
1e17). That matches the **`rms_acc` (Σsample²) u64** of §10.1 — a loud channel accumulates a vastly
larger sum. Interpreting the low word as a Q27 *peak* fails: a loud channel's low word exceeds Q27
full-scale, so this region is **not** the `{peak, rms}` pair — it looks like **rms_acc only**. Where
the **peak** lives on the wire (a separate region? a different bank?) is `[?]`, as is the
`rms_acc → dB` calibration (the `2^-55` constant and any block-average `N`, §10.1).

## 10.4 Implementation notes
- **Reading:** the dext exposes only single-quadlet reads today, so a meter poll is `N` sequential
  `ReadQuadlet`s (as `uf-probe` does). For a real meter display, add a **block-read** selector
  (`kUFOhciReadBlock`) so one call fetches the whole ~80-byte region — a quadlet-at-a-time poll of 10+
  channels at meter rates is a lot of round-trips.
- **Decode module** `uf::meters`: read the region as `u64 rms_acc[channel]` (offset `N*8`, LE), then
  `rms_db(acc) = 10*log10(acc * 2^-55)` per §10.1. The constant and `N` are uncalibrated `[?]`, so the
  result is relative, not absolute dB. Peak is not in this region, so there is no `peak_db()`.
- **Still open `[?]`:** the dB calibration (the `2^-55` constant and any block-average `N`); where the
  peak data lives; the ADAT/SPDIF input and output/playback meter slots. A `uf-meters` live display
  and decode test vectors in `spec/09` do not exist yet.
