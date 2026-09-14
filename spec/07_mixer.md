# 07 — TotalMix matrix mixer

Transcribed facts from FFADO `fireface_hw.cpp::set_hardware_mixergain`. The mixer is
optional (needed only for zero-latency monitoring). snd-fireface does not implement it.

## 7.1 Coefficient value
Each mixer element is a single **quadlet** written to the matrix RAM.
- Range `0x00000` (mute) … `0x10000` (+6 dB). **`0x8000` = 0 dB.**
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
- **Output-record mask** — `0x801c0080`.

## 7.4 Example
Route physical **input 3** to **output 1** at 0 dB:
`writeQuadlet(0x80080000 + 0·0x100 + 4·2, 0x8000)` → `writeQuadlet(0x80080008, 0x00008000)`
(input 3 → src index 2; output 1 → dest index 0). See `09_worked_examples.md`.
