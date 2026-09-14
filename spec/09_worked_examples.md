# 09 — Worked examples / test vectors

Concrete register transactions to validate an implementation against. `wq(addr,val)` =
write-quadlet, `rq(addr)` = read-quadlet, `wb(addr,[q0,q1,…])` = write-block. All values are the
host-order quadlet contents (the wire is little-endian; `UFAsyncIO` handles the swap). Derived from
the specs; verify the `[?]` items on hardware.

## 9.1 Identify
```
rq(0x200000100)          -> firmware revision
rq(0x801c0000)           -> SR0 (decode per 06 §SR0)
```

## 9.2 Set 48 kHz, internal (master) clock, SPDIF coax, +4 dBu levels
Sample rate (raw Hz to the rate register; snd-fireface path — prefer this):
```
wq(0xfc88f000, 0x0000BB80)     // 48000
```
Settings block CR0/CR1/CR2 (assembled per 06):
```
CR0 = 0x00000810   // input +4dBu FPGA(0x10) | output +4dBu FPGA(0x800)
CR1 = 0x0000001A   // input +4dBu CPLD(0x02) | output +4dBu CPLD(0x18)   (+ input-option bits as desired)
CR2 = 0x80000001   // master(0x01) | drop-and-stop(0x80000000); sync-ref ADAT1=0; SPDIF coax=0
wb(0xfc88f014, [0x00000810, 0x0000001A, 0x80000001])
```
(Alternate rate register, FFADO path, if `0xfc88f000` proves wrong on HW: `wq(0x20000001c, 48000)`.)

## 9.3 Read + decode status
```
sr0 = rq(0x801c0000)
  locked_ext_rate = (sr0 & 0x3ff) * 250      // when slaved to external clock
  spdif_lock = sr0 & 0x00100000
  wclk_lock  = sr0 & 0x40000000
sr1 = rq(0x801c0004)
  master = sr1 & 0x00000001
```

## 9.4 Start streaming @48 kHz (28 channels, S400)
`data_block_quadlets = 28 (0x1C)`.
```
// 1. rate already set (9.2); allow settle
// 2. discover capture channel the device assigns:
loop: tx_ch = rq(0x801c0008); until tx_ch != 0xffffffff       // e.g. 0
// 3. host-allocate a playback (rx) iso channel + bandwidth (25 + 28*4*7 = 809 units); say rx_ch=1
// 4. tell device the rx packet format + channel:
wq(0xfc88f004, ((28<<3)<<8) | 1)   = 0x0000E001
// 5. allocate tx stream:
wq(0xfc88f008, 0x0000001C)         // 28
// 6. begin session (S400):
wq(0xfc88f00c, 0x80000000 | 28)    = 0x8000001C     // S800 bus: | 0x800 -> 0x8000081C
// start OHCI receive context on tx_ch, transmit context on rx_ch
```
Stop:
```
wq(0xfc88f010, 0x80000000)
```

## 9.5 Packet decode/encode sanity (48 kHz, 28 ch)
- Payload per packet = `28 * 4 * 7 = 784` bytes (7 frames/packet at 1×).
- Frame = 28 quadlets; channel `c` at byte `c*4`.
- Capture: `sample24 = (le32(quad) >> 8) & 0x00FFFFFF`, sign-extend bit 23.
- Playback: `quad = (sample24 << 8)`, write LE.

## 9.6 Mixer: input 3 → output 1 at 0 dB
```
wq(0x80080000 + 0*0x100 + 4*2, 0x00008000)   -> wq(0x80080008, 0x00008000)
```
(src input3 = index 2; dest output1 = index 0; 0 dB = 0x8000.)

## 9.7 MIDI out — Note On (ch1, note 60, vel 127)
One MIDI byte per LE quadlet, block-write to the MIDI-out address:
```
wb(0x80180000, [0x00000090, 0x0000003C, 0x0000007F])
// wire bytes: 90 00 00 00 | 3C 00 00 00 | 7F 00 00 00
```

## 9.8 MIDI in — register a host receive address
Allocate a host-local FireWire address with low 32 bits = 0, e.g. region offset `0x0001_00000000`
(index i=1), install a write handler, then advertise it:
```
wq(0x200000320, (node_id << 16) | 0x0001)
// device now async-writes incoming MIDI (1 byte/quadlet) to 0x0001_00000000
// to stop: wq(0x200000320, 0)
```

## 9.9 Notes on `[?]` items to confirm on hardware
- Rate register: `0xfc88f000` (snd-fireface) vs `0x20000001c` (FFADO) — both write raw Hz; confirm which the FF800 honours.
- CR2 vs `0x801c0004` clock/rate overlap — confirm the CR block alone (with rate via 9.2) suffices.
- Exact CR0/CR1 default level bits for your unit — adjust per 06.
- MIDI-out inter-transaction throttle interval.
