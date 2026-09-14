# 08 — Channel maps (per speed & bandwidth-limit mode)

Transcribed facts from FFADO `rme_avdevice.cpp` (`addDirPorts` + channel-count logic).
Frame **slot index** = quadlet index within a data block = byte offset / 4. `data_block_quadlets`
(used by `RX_PACKET_FORMAT`, `03 §3.6`) = the **total** count in the active mode.

## 8.1 FF800 composition
- Analog: **10** (capture) — RME analog inputs 1–10 (mics on 7–10, instrument on 1; see `06`).
  On **playback** two of the ten are the **phones** (L/R), so playback = 8 analog + phones L/R.
- SPDIF: **2**.
- ADAT: **16** at 1×, **8** at 2×, **0** at 4× (ADAT collapses with speed).

Slot ordering (both directions): `analog… → phones(L,R, playback only) → SPDIF 1,2 → ADAT 1…N`.

## 8.2 "Send all channels" (default) — slot table
| slot | capture | playback |
|---|---|---|
| 0–7 | analog 1–8 | analog 1–8 |
| 8–9 | analog 9–10 | phones L, R |
| 10–11 | SPDIF 1–2 | SPDIF 1–2 |
| 12… | ADAT 1…N | ADAT 1…N |

Totals (`data_block_quadlets`):
| speed | analog | phones(pb) | SPDIF | ADAT | **total** |
|---|---|---|---|---|---|
| 1× (32/44.1/48k) | 10 | (2) | 2 | 16 | **28** |
| 2× (88.2/96k) | 10 | (2) | 2 | 8 | **20** |
| 4× (176.4/192k) | 10 | (2) | 2 | 0 | **12** |
(Capture and playback have the **same** total and slot count; only slots 8–9 differ in label:
capture = analog 9/10, playback = phones L/R.)

## 8.3 Bandwidth-limit modes (reduce the channel set)
Applied *before* the speed-based ADAT reduction:
| mode | analog | SPDIF | ADAT (1×) | notes |
|---|---|---|---|---|
| Send all channels (default) | 10 | 2 | 16 | as §8.2 |
| No ADAT2 (FF800) | 10 | 2 | 8 | only ADAT1 |
| Analog + SPDIF only | 10 | 2 | 0 | |
| Analog only | 8 | 0 | 0 | channels **1–8**; phones inactive on playback |

Then ADAT is still halved at 2× and zeroed at 4×. Example totals at 1×: send-all=28, no-ADAT2=20,
analog+SPDIF=12, analog-only=8.

## 8.4 Implementation
- Compute `pcm_channels` for the active (speed, bw-limit) combo; that is `data_block_quadlets`.
- Build a slot→(kind, index) map per §8.2 for CoreAudio channel naming.
- Decode/encode iterate `pcm_channels` quadlets per frame (`03 §3.4`).
