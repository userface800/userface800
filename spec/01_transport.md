# 01 — FireWire transport & address model

## 1.1 The device is a plain IEEE-1394 node
The FF800 exposes **no AV/C, no config-ROM audio unit**. It is driven with the two primitive
FireWire transaction classes only:
- **Asynchronous** — quadlet (4-byte) and block read/write/lock to fixed 48-bit addresses. Used
  for *all* control, status, mixer, MIDI, and firmware.
- **Isochronous** — cyclic, bandwidth-reserved streams for audio. Raw quadlet payload, no CIP
  header (see `03_streaming.md`).

## 1.2 Address model
A target is a 48-bit address split into a 16-bit **high** part and a 32-bit **low** part
(`address = (high << 32) | low`). Observed high parts on the FF800:
| high | region | usage |
|---|---|---|
| `0x0000` | device register space | control/status/mixer/streaming (`0x801cxxxx`, `0xfc88fxxx`, `0x80080000`, `0x80180000`) |
| `0x0002` | device register space (upper) | `0x2_0000xxxx` — revision/LED/host-MIDI-receive base (`0x200000320`) |
| `0x0003` | flash region | firmware/settings flash (`0x3000exxxx`, `0x3fffffffx`) |
| `0xffff` | CSR / config-ROM | standard IEEE-1212 space (bus-info block, unit directory) |

All device registers are **little-endian** quadlets: a value written/read is byte-swapped to/from
LE on the wire.

## 1.3 Device identification
Match on the config-ROM **vendor OUI = `0x000A35` (RME)** plus the FF800 model. snd-fireface matches
on vendor+model in its 1394 id table; the OUI is the discriminator. The RME OUI is **not** stored as
an immediate inside the register protocol — identification is a config-ROM read only.

## 1.4 The transaction primitive (`UFAsyncIO`)
All register access reduces to three operations against a device node handle:
- `readQuadlet(addr) -> u32` / `readBlock(addr, len) -> bytes`
- `writeQuadlet(addr, u32)` / `writeBlock(addr, bytes)`
- `lock(addr, ...)` (compare-swap; rarely needed)

Values are host-order in the API and converted LE at the boundary. Reads and writes are
synchronous request/response with a completion status (retry on `busy`/bus-reset with a fresh
generation count).

### macOS realization
- The transport is this project's own userland 1394 OHCI dext (PCIDriverKit), bound to the
  Thunderbolt-tunnelled OHCI controller; there is no kernel FireWire stack to sit on.
- Async R/W: the dext's user client exposes read-quadlet, write-quadlet, read-block and
  write-block against a 48-bit device offset, issued on the AT request context and matched to
  their AR responses.
- Isochronous: IT and IR contexts programmed with descriptor lists by the dext; the daemon stages
  buffers and the controller meets the 125 us cycle deadline.
- Bus-reset handling: transactions carry the current bus generation; on reset the node is
  re-identified from self-ID and, for streaming, iso resources are re-allocated.

`UFAsyncIO` is the single foundational component; everything in `02`/`04` is expressed in terms
of its three operations. (Reference correspondence, to adapt from with attribution: the
kernel `snd_fw_transaction(unit, TCODE_*_QUADLET/BLOCK, addr, ...)` and FFADO
`read/writeRegister/readBlock` play the same role.)
