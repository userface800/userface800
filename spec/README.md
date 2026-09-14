# Fireface 800 macOS userland driver — specification

This folder is the **authoritative, implementation-facing specification** for a userland macOS
driver for the RME Fireface 800 (FF800). It states *what to build* in normative terms.

## Reuse policy
**Reuse and adapt the reference drivers freely** — they are working, correct code.
This spec is the **map, the register/packet facts, and the validation**; the
reference drivers are the **implementation and safety net**:
- `ref/snd-fireface` — Linux kernel, **GPL-2.0** — primary reference (working FF800 audio+MIDI+clock).
- `ref/libffado-2.4.9` — **GPL** — settings + mixer.

Guidance: **port/adapt** the reference logic into UserFace800's own structure and `UF` naming; keep
attribution. When the spec and a working driver disagree, **the driver wins** — fix the spec.
Licensing and attribution: `../COPYRIGHT`.

## Provenance & confidence
Protocol facts here are cross-checked where possible against the Linux **FFADO** driver
(`ref/libffado-2.4.9/src/rme`) and the Linux kernel **snd-fireface** driver (`ref/snd-fireface`).
Untagged statements are settled. Tags:
- **[F]** from one OSS reference (FFADO or snd-fireface), not independently corroborated here.
- **[?]** open / unverified / references disagree — must be resolved before relying on it.

## Documents
- `01_transport.md` — FireWire async + isochronous primitives, the 48-bit address model.
- `02_control_registers.md` — full register map, the shadow-config write model, clock/sync, mixer.
- `03_streaming.md` — iso packet format, per-speed channel maps, sample encoding, stream lifecycle.
- `04_midi.md` — MIDI transport over FireWire (the one partially-open area).
- `06_bitfields.md` — complete CR0/CR1/CR2 + SR0/SR1 bit tables (transcribed).
- `07_mixer.md` — matrix mixer addressing + coefficient/dB encoding.
- `08_channel_maps.md` — per-speed / per-bandwidth-mode channel maps + slot ordering.
- `09_worked_examples.md` — concrete register sequences / test vectors to validate against.

## Scope
- **In scope:** a userland (no kext) macOS driver exposing the FF800 to Core Audio and CoreMIDI —
  control, status, isochronous full-duplex audio, mixer, MIDI.
- **Out of scope (for now):** firmware flashing, the TCO time-code option card beyond basic sync,
  TotalMix GUI parity.
- **Non-goals:** a kernel extension; supporting the FF400/UFX/UCX (the code should not preclude
  them, but only the FF800 is specified/tested).

## Key invariants (do not violate)
1. The FF800 speaks **no AV/C**. All control is memory-mapped registers over async R/W; audio is
   raw isochronous quadlets (no CIP/AMDTP). Never route it through AV/C classes.
2. Everything runs **userland**. The 125 µs isoch deadline is met by the OHCI controller executing
   a DCL program; software only stages buffers.
3. The **daemon owns the device**; the AudioServerPlugIn (in `coreaudiod`) only declares formats
   and moves bytes through a lock-free shared-memory ring. No blocking in the RT IO path.
4. FFADO is GPL. Code ported from `ref/libffado` inherits GPL — relevant only if distributed.
