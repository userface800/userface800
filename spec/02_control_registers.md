# 02 — Control, status, clock & mixer registers

All addresses are FF800 device-register offsets (high part `0x0000` unless noted). Quadlets are
little-endian. Two independent references model control slightly differently; both are captured,
with a recommendation.

## 2.1 Register map (FF800)
| Address | Dir | Width | Purpose | Src |
|---|---|---|---|---|
| `0xfc88f000` | W | 1 quad | **STF** — set streaming sample rate (write rate in Hz) | snd-fireface [F] |
| `0xfc88f004` | W | 1 quad | **RX packet format** — `(data_block_quadlets<<3)<<8 \| rx_iso_channel` | snd-fireface |
| `0xfc88f008` | W | 1 quad | **Allocate TX stream** — write `data_block_quadlets` | snd-fireface [F] |
| `0xfc88f00c` | W | 1 quad | **ISOC comm start** — `0x80000000 \| data_block_quadlets \| (S800?0x800:0)` | snd-fireface |
| `0xfc88f010` | W | 1 quad | **ISOC comm stop** — `0x80000000` | snd-fireface [F] |
| `0xfc88f014` | W | 3 quad | **CONF block CR0/CR1/CR2** — full analog/clock settings | FFADO |
| `0x801c0000` | R | 1 quad | **Status register 0** (lock/sync/ext-rate); also W = channel-mute mask | all |
| `0x801c0004` | R/W | 1 quad | **Clock config / status 1** (rate code, clock mode, clock source) | snd-fireface |
| `0x801c0008` | R | 1 quad | **TX iso channel** — device-assigned capture channel (`0xffffffff` until ready) | snd-fireface |
| `0x801c001c` | R | 1 quad | status 3 | FFADO [F] |
| `0x801c0080` | W | — | output/record mask | FFADO |
| `0x80080000` | W | matrix | **matrix mixer RAM** | FFADO |
| `0x80180000` | W | 1 quad/byte | **MIDI-out (host→device)** — see `04_midi.md` | snd-fireface |
| `0x200000320` | W | 1 quad | **MIDI host-receive high-addr register** — see `04_midi.md` | snd-fireface [F] |
| `0x200000100` | R | 1 quad | firmware revision | — |
| `0x200000324` | W | 1 quad | host LED | FFADO [F] |
| `0x801f0000`/`20` | R/W | — | TCO time-code option (FF800 only) | FFADO [?] |

Discrepancy note **[?]**: FFADO also documents an alternate FF800 streaming bank at high-part
`0x0002` (`0x2_0000001c` init / `0x28` start / `0x34` end). The device honours both banks;
snd-fireface uses only `0xfc88f0xx`. **Recommendation: follow snd-fireface's `0xfc88f000`–`0x010`
sequence** (current, mainline, and confirmed on hardware); treat the `0x0002` bank as legacy unless
hardware proves otherwise.

## 2.2 Two control surfaces
The device accepts control through two overlapping surfaces:

1. **Streaming/clock surface (minimal, sufficient to play audio)** — snd-fireface:
   - Sample rate: write the rate in **Hz** to STF `0xfc88f000`.
   - Clock config/read at `0x801c0004`: rate code (bits `0x0000001e`), clock mode (bit `0x00000001`,
     master vs slave/autosync), clock source (bits `0x00001c00`).
   - Status read at `0x801c0000` (§2.4).
2. **Full-settings surface (extended analog control)** — FFADO's CR0/CR1/CR2 block written to
   `0xfc88f014` (driver-confirmed). Needed for phantom power, input/output level ranges, SPDIF
   format, instrument/filter/limiter, sync reference selection, etc.

**Model:** keep a host-side shadow of the complete configuration; on any change, rebuild and
block-write the affected surface. Most bits are write-only (no read-back); status is read from the
dedicated status registers.

## 2.3 Sample-rate / clock-config codes
Clock-config rate field (`0x801c0004`, mask `0x0000001e`), from snd-fireface [F]:
| Hz | code | speed |
|---|---|---|
| 32000 | `0x02` | 1x |
| 44100 | `0x00` | 1x |
| 48000 | `0x06` | 1x |
| 64000 | `0x0a` | 2x |
| 88200 | `0x08` | 2x |
| 96000 | `0x0e` | 2x |
| 128000 | `0x12` | 4x |
| 176400 | `0x10` | 4x |
| 192000 | `0x16` | 4x |

Clock source (`0x801c0004`, mask `0x00001c00`): ADAT1 `0x0000`, ADAT2 `0x0400`, SPDIF `0x0c00`,
WordClock `0x1000`, LTC/TCO `0x1800`. Clock mode: bit `0x00000001`. **[F]**

Speed multiplier for all downstream sizing: `rate<64000 → 1x`, `<128000 → 2x`, else `4x`
(equivalently FFADO's 68100/136200 software thresholds).

## 2.4 Status registers
Read `0x801c0000` (SR0: per-source lock/sync, selected source, external rate = low-10-bits × 250)
and `0x801c0004` (SR1: clock-mode-master, TCO lock/sync). **Complete bit tables in
[`06_bitfields.md`](06_bitfields.md).** Poll these to report lock/sync and drive the clock state
machine.

## 2.5 CR0/CR1/CR2 settings block (`0xfc88f014`)
The three-quadlet block carrying all extended analog settings (phantom, input/output/phones level,
input options, filter/drive, SPDIF format, clock mode, sync ref, limiter, drop-and-stop). The
**sample rate is NOT here** — it is a separate raw-Hz write (§2.2, `03 §3.6`); CR2's FREQ/speed bits
are left 0. **Complete bit tables + assembly rules in [`06_bitfields.md`](06_bitfields.md).**

## 2.6 Mixer (`0x80080000`)
matrix mixer RAM — per-output input/playback coefficients + output faders, plus mute/rec
masks (`0x801c0000`/`0x801c0080`). 0 dB = `0x8000`. Not required for playback/record; needed for
zero-latency monitoring. **Complete addressing + coefficient encoding in [`07_mixer.md`](07_mixer.md).**
