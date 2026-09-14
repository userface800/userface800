# 06 — Register bitfields (complete, transcribed)

Complete FF800 bit definitions, transcribed as facts from FFADO `fireface_def.h` +
`fireface_hw.cpp` assembly and snd-fireface. Supersedes the summaries in `02 §2.4/§2.5`.

**How settings are applied:** build three quadlets `CR0,CR1,CR2` from the settings shadow and
**block-write 3 quadlets to `0xfc88f014`** (`writeBlock`). The **sample rate is NOT in these
quadlets** — it is written separately as raw Hz to the rate register (`03 §3.6`, `09`). FREQ0/FREQ1/
DSPEED/QSSPEED bits in CR2 exist but are left **0** here. **[?]** — *conflict:* FFADO
`set_hardware_params` actually ORs `FREQ0|FREQ1|DSPEED|QSSPEED` (=`0x1E`, "hardwired in other
drivers") **and** always sets `WORD_CLOCK_1x` via an `=`/`==` assignment bug. snd-fireface (primary)
never writes the CR block, and §9.2 gives `CR2=0x80000001`. We follow snd-fireface/§9.2 (FREQ/speed
= 0); **confirm on hardware** whether the FF800 needs or ignores `0x1E`.

Bit encodings for multi-bit fields are **one-hot / value** as noted; assemble by OR-ing.

## CR0 (quadlet 0) — FF800
| bit | mask | FF800 meaning |
|---|---|---|
| 0 | `0x00000001` | Phantom power mic **7** |
| 1 | `0x00000002` | Phantom power mic **9** |
| 2 | `0x00000004` | **Filter** (speaker emulation, ch1) — FPGA |
| 3 | `0x00000008` | Input-level FPGA ctrl0 |
| 4 | `0x00000010` | Input-level FPGA ctrl1 |
| 5 | `0x00000020` | Input-level FPGA ctrl2 |
| 6 | `0x00000040` | (zero) |
| 7 | `0x00000080` | Phantom power mic **8** |
| 8 | `0x00000100` | Phantom power mic **10** |
| 9 | `0x00000200` | **Drive** (instrument, ch1) — FPGA |
| 10 | `0x00000400` | Output-level FPGA ctrl0 |
| 11 | `0x00000800` | Output-level FPGA ctrl1 |
| 12 | `0x00001000` | Output-level FPGA ctrl2 |
| 16 | `0x00010000` | Phones-level ctrl0 |
| 17 | `0x00020000` | Phones-level ctrl1 |

Multi-bit encodings in CR0:
- **Input level (FPGA, FF800 LED mirror — also set CR1 CPLD):** LoGain=`0x08`, +4dBu=`0x10`, −10dBV=`0x20` (one-hot).
- **Output level (FPGA, FF800 LED mirror — also set CR1 CPLD):** HiGain=`0x400`, +4dBu=`0x800`, −10dBV=`0x1000`.
- **Phones level:** +4dBu=`0`, −10dBV=`0x10000`, HiGain=`0x20000`.

## CR1 (quadlet 1) — FF800
| bit | mask | FF800 meaning |
|---|---|---|
| 0 | `0x00000001` | Input-level CPLD ctrl0 |
| 1 | `0x00000002` | Input-level CPLD ctrl1 |
| 2 | `0x00000004` | Input 1 **rear** (opt0 B) |
| 3 | `0x00000008` | Output-level CPLD ctrl0 |
| 4 | `0x00000010` | Output-level CPLD ctrl1 |
| 5 | `0x00000020` | Input 7 **front** (opt1 A) |
| 6 | `0x00000040` | Input 7 **rear** (opt1 B) |
| 7 | `0x00000080` | Input 8 **front** (opt2 A) |
| 8 | `0x00000100` | Input 8 **rear** (opt2 B) |
| 9 | `0x00000200` | Instrument **drive** (CPLD) |
| 10 | `0x00000400` | Input 1 front **with filter** (opt0 A1) |
| 11 | `0x00000800` | Input 1 **front** (opt0 A0) |

Multi-bit encodings in CR1 (these are the *actual* level; CR0's FPGA bits are the FF800 LED mirror,
set both):
- **Input level (CPLD):** LoGain=`0`, +4dBu=`0x02`, −10dBV=`0x03`.
- **Output level (CPLD):** −10dBV=`0x08`, HiGain=`0x10`, +4dBu=`0x18`.
- **Input 1 front:** normally `0x800` (opt0 A0); when the filter/speaker-emulation is on use `0x400`
  (opt0 A1) instead; rear = `0x04` (opt0 B).

## CR2 (quadlet 2)
| bit | mask | meaning |
|---|---|---|
| 0 | `0x00000001` | Clock mode: **1=master**, 0=autosync |
| 1 | `0x00000002` | FREQ0 *(leave 0)* |
| 2 | `0x00000004` | FREQ1 *(leave 0)* |
| 3 | `0x00000008` | DSPEED *(leave 0)* |
| 4 | `0x00000010` | QSSPEED *(leave 0)* |
| 5 | `0x00000020` | SPDIF out **Professional** |
| 6 | `0x00000040` | SPDIF out **Emphasis** |
| 7 | `0x00000080` | SPDIF out **Non-audio** |
| 8 | `0x00000100` | SPDIF out **optical** (on ADAT2 port) |
| 9 | `0x00000200` | SPDIF in **optical** (ADAT2); 0=coax |
| 10 | `0x00000400` | SYNC_REF0 |
| 11 | `0x00000800` | SYNC_REF1 |
| 12 | `0x00001000` | SYNC_REF2 |
| 13 | `0x00002000` | Word-clock 1x (single-speed) |
| 14 | `0x00004000` | Toggle TCO *(normally 0)* |
| 16 | `0x00010000` | Disable soft-limiter (P12dB) *(normally 0)* |
| 30 | `0x40000000` | TMS *(normally 0)* |
| 31 | `0x80000000` | **Drop-and-stop** *(normally SET)* |

**Sync reference** (SYNC_REF bits): ADAT1=`0`, ADAT2=`0x0400`, SPDIF=`0x0c00`, WordClock=`0x1000`,
TCO=`0x1400`.
**Defaults:** always set Drop-and-stop (`0x80000000`); TMS/TCO-toggle/limiter-disable = 0.

## SR0 — status register 0 (read `0x801c0000`)
| mask | meaning |
|---|---|
| `0x00000400` | ADAT1 lock |
| `0x00000800` | ADAT2 lock |
| `0x00001000` | ADAT1 sync |
| `0x00002000` | ADAT2 sync |
| `0x00004000`..`0x00020000` | SPDIF freq F0..F3 |
| `0x00040000` | SPDIF sync |
| `0x00080000` | Over (clipping) |
| `0x00100000` | SPDIF lock |
| `0x00400000` | Selected-sync-ref bit0 |
| `0x00800000` | Selected-sync-ref bit1 |
| `0x01000000` | Selected-sync-ref bit2 |
| `0x02000000`..`0x10000000` | Input freq bits 0..3 (autosync source freq) |
| `0x20000000` | WordClock sync |
| `0x40000000` | WordClock lock |
| `0x000003ff` | **External sample rate = value × 250** (when locked to external clock) |

Decode helpers:
- **Selected sync source** (bits 22–24): ADAT1=`0`, ADAT2=`0x400000`, SPDIF=`0xc00000`,
  WordClock=`0x1000000`, TCO=`0x1400000`, none=`0x1800000`.
- **Autosync freq** (bits 25–28): 32k=`1`,44.1k=`2`,48k=`3`,64k=`4`,88.2k=`5`,96k=`6`,128k=`7`,
  176.4k=`8`,192k=`9` (× the INP_FREQ0 weight `0x02000000`).
- **SPDIF freq** (bits 14–17): same 1..9 ordering × the SPDIF_F0 weight `0x00004000`.

## SR1 — status register 1 (read `0x801c0004`; write side = clock config, see `02 §2.3`)
| mask | meaning |
|---|---|
| `0x00000001` | Clock mode = master |
| `0x00400000` | TCO sync |
| `0x00800000` | TCO lock |
