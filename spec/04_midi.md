# 04 — MIDI over FireWire

Authoritative reference: snd-fireface `ff-transaction.c` + `ff-protocol-former.c` + the FF800 spec
in `ff.c`. MIDI is **not** muxed into the isochronous audio stream: FF800 MIDI uses **asynchronous
transactions**, entirely separate from the audio stream, at fixed addresses that need no
config-register setup.

## 4.1 Ports & framing
- FF800 has **1 MIDI-in port and 1 MIDI-out port** (physical DIN). **[F]**
- **Framing (both directions): one MIDI byte per 32-bit little-endian quadlet.** The MIDI byte is
  the low 8 bits of the quadlet; the upper 24 bits are unused. No length/status header.

## 4.2 MIDI OUT (host → device)
- Target address: **`midi_rx_addrs[0] = 0x000080180000`** (FF800).
- Take up to **9** pending MIDI bytes (`SND_FF_MAXIMIM_MIDI_QUADS`) from the host queue, expand each
  byte into one LE quadlet, and **block-write** them to the target address. **[F]**
- Flow control: only issue the next block after the previous async write completes; track bytes
  in-flight and any write error, and re-schedule if the queue is non-empty. Respect a minimum
  inter-transaction interval (the reference throttles via a per-port timestamp). **[F]**

## 4.3 MIDI IN (device → host)
The device delivers incoming MIDI by performing async **writes into a host-local FireWire address**
that the host advertises to it:
1. **Allocate a host-local address region** whose **low 32 bits are zero** (device requirement),
   size = `midi_addr_range = 12` bytes for FF800, with a write callback.
2. **Advertise it to the device**: write `(node_id << 16) | (region_offset >> 32)` to the
   **MIDI host-receive register `midi_high_addr = 0x000200000320`** (FF800). Only the high 4 bytes
   are registered; the FF800 does not accept a low-address registration. **[F]**
3. The device then async-writes incoming MIDI to `(registered_high << 32) | 0`. The write callback
   receives a block; for each quadlet, `midi_byte = le32_to_host(quadlet) & 0xFF`, delivered to the
   host MIDI-in port.
4. **To stop MIDI-in**, write `0` to `midi_high_addr` (clearing the high address suppresses the
   device's async transactions). **[F]**

Re-advertise after a bus reset (the node id changes). **[F]**

## 4.4 SysEx / running status
Handle MIDI stream framing at the port layer (SysEx `F0..F7` spanning multiple transactions,
running status). The reference keeps a per-port `on_sysex` flag; a macOS port should likewise not
assume message boundaries align to transaction boundaries. **[F]**

## 4.5 macOS realization (CoreMIDI)
MIDI is **independent of the Core Audio path** — it lives in the daemon (which owns the device), not
in the AudioServerPlugIn.
- Create a CoreMIDI **virtual source** (device→host) and **virtual destination** (host→device) for
  the FF800, or a CoreMIDI driver providing one entity with one source + one destination.
- **OUT:** on bytes from the destination, pack one-byte-per-LE-quadlet and `UFAsyncIO.writeBlock`
  to `0x80180000`, honoring the completion/throttle rule (§4.2).
- **IN:** the device writes MIDI to a host address; the transport accepts those inbound writes at a
  local address whose low 32 bits are zero, and advertises its high address to the device by writing
  to `0x200000320` (§4.3). On each device write, unpack quadlets → bytes → push to the CoreMIDI
  source. Clear `0x200000320` on teardown.

## 4.6 Status & scope
MIDI is optional — it does not block audio. It is **fully specified** here (addresses + framing +
lifecycle), so the only remaining `[?]` is minor throttle timing, resolvable empirically.
