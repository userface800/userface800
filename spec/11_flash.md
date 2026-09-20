# 11 — On-device flash (persistent settings / NVRAM)

The FF800 stores its settings and the mixer state in on-board flash so they survive power-off
and drive the unit standalone (RME's "Store in Flash Memory" button). Facts transcribed from FFADO
`fireface_flash.cpp` / `fireface_def.h` (`[F]`); the on-wire protocol is single async R/W, same
transport as every other register. **Not yet exercised on hardware** (`[?]` on layout).

## 11.1 Addresses (48-bit FireWire) `[F]`
| region | address | notes |
|---|---|---|
| settings record | `0x3_000f0000` | the persisted device settings |
| mixer shadow | `0x3_000e0000` | mixer state, `0x2000` bytes |
| mixer volume / pan / hw | `0x3_000e2000` / `…2800` / `…3000` | |
| firmware revision (read) | `0x2_00000100` | `uf::reg::kFirmwareRev` |
| erase settings | `0x3_fffffff0` | write 0 to erase |
| erase volume / firmware / config | `0x3_fffffff4` / `…f8` / `…fc` | write 0 to erase |

Flash sector = **256 bytes (64 quadlets)**; read/write the data regions in sector-sized chunks.

## 11.2 Operations `[F]`
- **Read**: async reads of the region. (Our dext has only quadlet reads; a block-read selector would
  make dumping a sector one call — TODO.)
- **Erase**: `writeQuadlet(erase_reg, 0)`, then poll ready (§11.3), then wait ~20 ms (other drivers do,
  reason unclear — mirror it).
- **Write**: block-write the data in ≤ sector chunks, poll ready after each. Our dext `WriteBlock`
  caps at 32 quads, so two writes per 64-quad sector.
- **Store settings** = erase settings block, then write the settings record to `0x3_000f0000`.

## 11.3 Ready / busy `[F]`
After an erase or write, poll **SR1 (`0x801c0004`)**: **bit 30 (`0x40000000`) set = flash ready**.
FFADO retries up to 25× with a per-iteration delay (500 ms after erase, 5 ms after a write).

## 11.4 Status & plan
- `tools/uf-flash.cpp` implements **read** (`rev`, `read`, `dump-settings`, `dump-mixer`) and a guarded
  **erase-settings** (`--force`). Read is safe; erase/write are DESTRUCTIVE + hardware-gated.
- **The settings-record layout** is FFADO's `FF_device_flash_settings_t`, which mirrors the flash
  exactly. A write ("store my settings") is feasible from that layout, but it is unverified against
  the device `[?]`; read-first remains the safe path.
- Confirm on hardware, then promote §11.1/11.3 to `[HW-CONFIRMED]`.
