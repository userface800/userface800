# 03 — Isochronous audio streaming

Primary reference: snd-fireface `amdtp-ff` + `ff-protocol-former` (FF800 = "former" protocol),
corroborated by FFADO and by the factory driver's behaviour. All facts; adapt with attribution
(`../COPYRIGHT`).

## 3.1 Stream model
Two isochronous streams, named from the **device's** point of view:
- **rx stream** = device receives = **host playback** (host → device).
- **tx stream** = device transmits = **host capture** (device → host).

Host allocates bus resources for the rx (playback) stream; the FF800 **assigns its own tx channel**
(capture), which the host reads back (§3.5).

## 3.2 Packet format
- **No CIP / no AMDTP negotiation.** Packets are configured with `NO_HEADER + BLOCKING +
  UNAWARE_SYT` semantics: raw isochronous data blocks, no 8-byte CIP header, fixed data-block size,
  no SYT timestamp handshake.
- One **data block = one audio frame** = `data_block_quadlets` quadlets, where
  `data_block_quadlets = pcm_channels` (no extra MIDI/status quadlet — MIDI is async, see `04`).
- Each channel occupies **one little-endian 32-bit quadlet**.
- **Frames per packet** by speed: 1x = **7**, 2x = **15**, 4x = **25**. Packet audio payload =
  `pcm_channels × 4 × frames_per_packet` bytes → 784 / 1200 / 1200 B at the max channel counts.
  Confirmed against the factory driver's behaviour.

## 3.3 Channel counts (FF800)
| speed | rates | pcm_channels |
|---|---|---|
| 1x | 32/44.1/48 k | **28** |
| 2x | 88.2/96 k | **20** |
| 4x | 176.4/192 k | **12** |
Same for capture and playback. Composition: analog 10 + SPDIF 2 + ADAT (16 at 1x, 8 at 2x, 0 at
4x). Bandwidth-limit modes reduce this (analog-only = channels 1–8).

## 3.4 Sample encoding
Per channel, one LE 32-bit quadlet holds **24-bit audio in the top 24 bits**; the low byte is
housekeeping/unused for audio. The kernel driver DMAs raw quadlets; conversion is the host's job.
- **Capture (device→host):** `sample24 = (le32_to_host(quadlet) >> 8) & 0x00FFFFFF`, sign-extend
  from bit 23 (i.e. if `quadlet & 0x80000000`, set high byte). Present as S32 (24-bit in high bytes)
  or convert to float `sample24 / 8388607.0`.
- **Playback (host→device):** place the 24-bit sample in the top 24 bits, write the quadlet LE.
  (snd-fireface exposes S32 and writes the sample directly; the device consumes the top 24 bits.)

## 3.5 Channel/frame layout
Within a frame, channel slot index → quadlet index (byte offset = index × 4):
`analog 1..N → phones L,R → SPDIF 1,2 → ADAT 1..N`. On playback the phones are two of the analog
outputs; on capture there are no phones. Reproduce FFADO `addDirPorts` ordering (fact) in the
CoreAudio channel map. **[F]**

## 3.6 Stream lifecycle (FF800 — authoritative sequence)
Registers per `02_control_registers.md`. Sequence (snd-fireface `ff800_allocate_resources` /
`ff800_begin_session` / `ff800_finish_session`):

**Start:**
1. Write sample rate (Hz) to **STF `0xfc88f000`**; wait ~100 ms (rate change is ignored if comm
   starts immediately). **[F]**
2. Allocate **rx (playback)** iso resources host-side — bus bandwidth for the max rx payload +
   an iso channel — at the bus's max speed.
3. Write **RX packet format** to `0xfc88f004`: `((data_block_quadlets << 3) << 8) | rx_channel`.
   (Do this before tx allocation to avoid periodic noise.)
4. Write **`data_block_quadlets`** to **ALLOC_TX_STREAM `0xfc88f008`**. **[F]**
5. Poll **TX iso channel `0x801c0008`** until it is not `0xffffffff`; that value is the
   **device-assigned tx (capture) channel**. Start an isochronous receive context on it.
6. **Begin session:** write `0x80000000 | data_block_quadlets | (bus==S800 ? 0x800 : 0)` to
   **ISOC_COMM_START `0xfc88f00c`**. Streaming is now live.
7. Enable PCM fetching (former "fetching mode") as needed at `0x801c0000`. **[F]**

**Stop:**
1. Write `0x80000000` to **ISOC_COMM_STOP `0xfc88f010`**; disable fetching mode. **[F]**
2. Tear down iso contexts; free the host-allocated rx bandwidth/channel. (The tx channel is the
   device's; the OHCI IR context is just stopped.) **[F]**

Bus generation: if a bus reset occurred since allocation, refresh iso resources before begin.

## 3.7 Bandwidth
Reserve `25 + pcm_channels × 4 × frames_per_packet` allocation units for the playback stream
(S400: 1 unit = 1 byte; the +25 is protocol overhead the bus adds). Confirmed against the factory
driver's behaviour.

## 3.8 Alternate channel-allocation strategy (informative)
The device also accepts the host allocating **both** channels itself and writing `(tx<<8)|rx` to it.
snd-fireface and FFADO instead let the device pick tx and read it back (§3.6 step 5). **Use the
snd-fireface poll approach** — it is the current mainline behaviour; the host-allocated variant is
worth noting only if the poll path proves unreliable on hardware.
