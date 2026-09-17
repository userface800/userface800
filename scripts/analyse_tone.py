#!/usr/bin/env python3
"""Find out what a recorded test tone was done to.

Built for one question: our 96 kHz playback sounds "ring modulated" and RME's does not. Ring
modulation is amplitude modulation, which puts sidebands at f +/- F around a pure tone f. Measuring
F names the mechanism — 750 Hz would be the IO cycle rate at 96k/128 frames, 2 Hz the clock servo,
and so on — where listening only tells you it sounds wrong.

Works on an ANALOG loopback recording, which is why it exists alongside uf-testsig: that tool needs a
bit-exact digital path, and a D/A -> A/D round trip is never bit-exact. This one only needs the
spectrum, so a jack cable is enough.

    analyse_tone.py <recording.wav> [--channel=N]

Reports the fundamental, any sidebands with their spacing, harmonic distortion, and the noise floor.
"""
import struct
import sys

import numpy as np


def read_wav(path):
    b = open(path, "rb").read()
    if b[:4] != b"RIFF" or b[8:12] != b"WAVE":
        sys.exit("not a RIFF/WAVE file")
    p, fmt, data = 12, None, None
    while p + 8 <= len(b):
        cid = b[p : p + 4]
        (sz,) = struct.unpack("<I", b[p + 4 : p + 8])
        if cid == b"fmt ":
            tag, ch, rate, _, _, bits = struct.unpack("<HHIIHH", b[p + 8 : p + 24])
            fmt = (tag, ch, rate, bits)
        elif cid == b"data":
            data = b[p + 8 : p + 8 + sz]
        p += 8 + sz + (sz & 1)
    if not fmt or data is None:
        sys.exit("missing fmt/data chunk")
    tag, ch, rate, bits = fmt
    if bits == 24:
        raw = np.frombuffer(data[: len(data) // 3 * 3], dtype=np.uint8).reshape(-1, 3)
        v = (raw[:, 0].astype(np.int32)
             | (raw[:, 1].astype(np.int32) << 8)
             | (raw[:, 2].astype(np.int32) << 16))
        v = np.where(v & 0x800000, v - 0x1000000, v).astype(np.float64) / float(1 << 23)
    elif bits == 32 and tag == 3:                     # float
        v = np.frombuffer(data, dtype="<f4").astype(np.float64)
    elif bits == 32:
        v = np.frombuffer(data, dtype="<i4").astype(np.float64) / float(1 << 31)
    elif bits == 16:
        v = np.frombuffer(data, dtype="<i2").astype(np.float64) / float(1 << 15)
    else:
        sys.exit(f"unsupported: {bits}-bit tag {tag}")
    return v.reshape(-1, ch), rate


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    chan = 0
    for a in sys.argv[2:]:
        if a.startswith("--channel="):
            chan = int(a.split("=", 1)[1])

    x, rate = read_wav(path)
    if chan >= x.shape[1]:
        sys.exit(f"file has {x.shape[1]} channels")
    x = x[:, chan]

    # Trim leading/trailing silence so the window is all signal.
    nz = np.nonzero(np.abs(x) > 1e-6)[0]
    if nz.size < rate // 10:
        sys.exit("almost no signal in that channel")
    x = x[nz[0] : nz[-1] + 1]
    print(f"{path}: {rate} Hz, channel {chan}, {len(x)} frames ({len(x)/rate:.2f} s)")

    # Long window, Blackman-Harris: sidebands only a few Hz from a strong carrier need both the
    # resolution and the low leakage to be visible at all.
    n = 1 << int(np.floor(np.log2(min(len(x), rate * 8))))
    seg = x[:n] * np.blackman(n)
    spec = np.abs(np.fft.rfft(seg))
    freqs = np.fft.rfftfreq(n, 1.0 / rate)
    spec /= spec.max()
    db = 20 * np.log10(np.maximum(spec, 1e-12))

    k0 = int(np.argmax(spec))
    f0 = freqs[k0]
    print(f"fundamental: {f0:.2f} Hz")
    binhz = rate / n
    print(f"resolution:  {binhz:.3f} Hz/bin")

    # Anything within 40 dB of the carrier that is not the carrier itself.
    peaks = []
    for k in range(2, len(spec) - 2):
        if db[k] < -80:
            continue
        if spec[k] > spec[k - 1] and spec[k] >= spec[k + 1] and spec[k] > spec[k - 2] and spec[k] >= spec[k + 2]:
            peaks.append((freqs[k], db[k]))
    peaks = [p for p in peaks if abs(p[0] - f0) > 3 * binhz]
    peaks.sort(key=lambda p: -p[1])

    print(f"\nnoise floor: {np.median(db):.1f} dB")
    if not peaks:
        print("no significant peaks besides the fundamental — clean tone")
        return

    print(f"\n{'freq (Hz)':>12} {'dB':>8}  {'offset from f0':>16}  note")
    for f, d in peaks[:16]:
        off = f - f0
        note = ""
        if abs(f - round(f / f0) * f0) < 3 * binhz and round(f / f0) >= 2:
            note = f"harmonic x{round(f / f0)}"
        else:
            note = f"SIDEBAND, spacing {abs(off):.2f} Hz"
        print(f"{f:12.2f} {d:8.1f}  {off:+16.2f}  {note}")

    # Recovering the modulation frequency from sideband positions is unreliable: square-wave AM
    # puts energy at k*F +/- f0, so the smallest offset from the carrier is not F. Demodulate
    # instead — the envelope of an amplitude-modulated tone contains F directly.
    analytic = np.fft.rfft(x[:n] )
    full = np.zeros(n, dtype=complex)
    full[: len(analytic)] = analytic
    full[1 : (n + 1) // 2] *= 2                 # one-sided spectrum -> analytic signal
    env = np.abs(np.fft.ifft(full))
    env -= env.mean()
    ew = env * np.blackman(n)
    espec = np.abs(np.fft.rfft(ew))
    espec[: max(2, int(2.0 / binhz))] = 0        # ignore DC and very slow drift
    kf = int(np.argmax(espec))
    F = freqs[kf]
    depth = espec[kf] / max(np.abs(x[:n]).mean(), 1e-12)

    print(f"\nenvelope analysis:")
    if espec[kf] <= 0 or depth < 1e-3:
        print("  no significant amplitude modulation detected")
        return
    print(f"  modulation frequency = {F:.2f} Hz")
    print(f"  for reference at {rate} Hz:")
    for name, val in (("IO cycle @128 frames", rate / 128),
                      ("IO cycle @256 frames", rate / 256),
                      ("IO cycle @512 frames", rate / 512),
                      ("ring slot (512 frames)", rate / 512),
                      ("isochronous cycle", 8000.0),
                      ("device packet rate", 6000.0),
                      ("interrupt rate", 750.0)):
        mark = "  ** matches" if abs(F - val) < max(2.0, 0.02 * val) else "     "
        print(f"  {mark} {name} = {val:.1f} Hz")


if __name__ == "__main__":
    main()
