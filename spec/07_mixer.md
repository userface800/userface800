# 07 — TotalMix matrix mixer

Transcribed facts from FFADO `fireface_hw.cpp::set_hardware_mixergain`. The mixer is
optional (needed only for zero-latency monitoring). snd-fireface does not implement it.

## 7.1 Coefficient value
Each mixer element is a single **quadlet** written to the matrix RAM.
- The coefficient is **SIGNED**: the magnitude is the gain, and a **negative value inverts the phase
  by 180°**. FFADO validates `abs(val) <= 0x10000` (`set_hardware_mixergain`) and negates the value
  when a crosspoint carries `FF_SWPARAM_MF_INVERTED`; the TotalMix Matrix draws such a crosspoint red
  (manual §26.2).
- Range `0x00000` (mute) … `0x10000` (+6 dB), and the negatives of those. **`0x8000` = 0 dB.**
- **Hardware quirk `[from FFADO]`:** going from `0` (−inf) to `-1` (−90 dB) makes the FF800 run the
  volume far UP before dropping to the set point ~100 ms later. So a crosspoint that is both muted
  and inverted is written as `-1`, not `0` — see `uf::signed_coeff`.
- `dB = 20 · log10(val / 32768)`. So `val = round(32768 · 10^(dB/20))`, clamped to `[0, 0x10000]`.
- (Note: a UI using `0x4000` as its 0 dB reference must double before writing — the hardware
  reference is `0x8000`.)

## 7.2 Matrix RAM addressing (FF800)
Base `MIXER_RAM = 0x80080000`. Per-output **block size = `0x80`** (32 quadlet slots; FF800 has 28
channels, the block is padded to 32). `src`/`dest` are **0-based** channel indices.

| element | address |
|---|---|
| **Input → output** coefficient | `0x80080000 + dest·0x100 + 4·src` |
| **Playback → output** coefficient | `0x80080000 + dest·0x100 + 0x80 + 4·src` |
| **Output fader** (per output) | `0x80080000 + 0x1f80 + 4·src` |

Notes:
- Each output destination owns a `0x100`-byte region: the first `0x80` holds its 28(+pad) **input**
  coefficients, the second `0x80` holds its 28(+pad) **playback** coefficients.
- `dest·0x100` = `dest·2·0x80` (the `2` = input half + playback half).
- Write each element as a single quadlet (`writeQuadlet`).
- Channel index order matches the streaming frame map (`08_channel_maps.md`).

## 7.3 Mute / record masks
- **Channel-mute mask** — block-write to `0x801c0000` (write side; read side is SR0). One quadlet per
  channel (28 for FF800). Muting can also be done by writing coefficient `0`.
- **Output-record mask** — `0x801c0080`. **This is TotalMix's "Loopback"** (manual §27.5): a set
  output sends its MIX to the recording software in place of the corresponding hardware input, which
  still reaches the mixer. 28 quadlets, one per output, block-written. FFADO only ever writes it
  all-on or all-off (`set_hardware_output_rec`); the register is per-channel, so per-output loopback
  needs nothing invented. **Re-apply it at every session start** alongside the matrix, or a rate
  change silently drops it.

## 7.3a Which TotalMix features are device state
Sorting the manual (§25–§27) against FFADO's register code, only the first group exists in hardware:

| | |
|---|---|
| **DEVICE** | crosspoint gains (signed), per-output faders, channel-mute mask `0x801c0000`, output-record mask `0x801c0080` |
| **HOST** | stereo pairing, pan, mute, solo, width, M/S, trim, cue, talkback, groups, snapshots, workspaces |

Everything in the second group is TotalMix computing crosspoints. **Pan** is the clearest case: a
source panned across a stereo output pair is just its two crosspoints at different gains (−3 dB at
centre), which is why the manual can say the Matrix "operates monaural" (§26.3) and still be a
complete view of the mixer.

**But mute and phase cannot be stored as rendered coefficients.** Muting writes `0`, so the gain
underneath is lost and un-muting cannot restore it; inverting negates, so the magnitude must survive
separately. FFADO keeps `input_faders[]` beside `input_mixerflags[]` for exactly this. Ours is
`platform/shared/uf_mixer_model.hpp`.

**Stereo pairing is per row.** Stereo is a per-channel setting on the input and playback rows
(§25.3); "Hardware Outputs are always stereo". Two masks, not one, and no third.

## 7.4 Example
Route physical **input 3** to **output 1** at 0 dB:
`writeQuadlet(0x80080000 + 0·0x100 + 4·2, 0x8000)` → `writeQuadlet(0x80080008, 0x00008000)`
(input 3 → src index 2; output 1 → dest index 0). See `09_worked_examples.md`.
