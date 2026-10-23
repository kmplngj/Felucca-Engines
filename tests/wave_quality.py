# SPDX-License-Identifier: MIT
"""Independent floating Fourier/16x oversampled oscillator reference (not PCM parity).
No production C helper or generated quantized sample is used in the reference.
"""

import cmath, json, math, struct, subprocess, sys
from pathlib import Path


def fft(x, inverse=False):
    x = list(x)
    n = len(x)
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j ^= bit
        if i < j:
            x[i], x[j] = x[j], x[i]
    size = 2
    while size <= n:
        w0 = cmath.exp((2j if inverse else -2j) * math.pi / size)
        for i in range(0, n, size):
            w = 1
            for j in range(i, i + size // 2):
                v = x[j + size // 2] * w
                u = x[j]
                x[j] = u + v
                x[j + size // 2] = u - v
                w *= w0
        size *= 2
    return [v / n for v in x] if inverse else x


def check(binary, manifest):
    m = json.loads(Path(manifest).read_text())
    fs = m["fs"]
    n = 4096
    os = 16
    recipes = json.loads(Path("assets/wave_small/recipes.json").read_text())
    report = []
    cases = [
        (bank, note, warp, 1, 127)
        for bank in range(4)
        for note in (36, 60, 84, 108, 127)
        for warp in (0, -63, 63)
    ]
    cases += [
        (bank, note, warp, 4, 127)
        for bank in range(4)
        for note in (60, 108)
        for warp in (0, -63, 63)
    ]
    cases += [(bank, 84, warp, 2, 127) for bank in range(4) for warp in (0, -63, 63)]
    cases += [
        (bank, note, warp, 1, round(frame * 127 / 7))
        for bank in range(4)
        for frame in range(7)
        for note, warp in ((36, 0), (84, 0), (60, -63), (60, 63))
    ]
    for bank, note, warp, mult, scan in cases:
        bin = max(1, round(440 * 2 ** ((note - 69) / 12) / fs * n))
        freq = bin * mult / n
        inc = bin * (2**32 // n)
        raw = subprocess.check_output(
            [
                binary,
                str(bank),
                str(scan),
                str(warp),
                str(mult),
                str(inc),
                str(n // 32),
                "osc",
            ]
        )
        got = [v / 32768 for v in struct.unpack("<%di" % n, raw)]
        k = 0.75 * warp / 63
        taper = max(0, min(1, (0.2 - freq) / 0.1))
        k *= taper
        safe = freq * (1 + abs(k))
        limits = (63, 31, 15, 7, 3, 1)
        level = 5
        blend = 0
        for j, h in enumerate(limits[:-1]):
            end = 0.45 / h
            start = 0.75 * end
            if safe < end:
                level = j
                blend = max(0, (safe - start) / (end - start))
                break
        position = scan * 7 / 127
        first = int(position)
        fraction = position - first
        partials = {}
        for frame, weight in ((first, 1 - fraction), (min(7, first + 1), fraction)):
            for part in recipes["banks"][bank]["frames"][frame]["partials"]:
                h = part["harmonic"]
                partials[h] = partials.get(h, 0) + part["amplitude"] * weight
        # Independent analytic Fourier recipe sampled densely, then ideal low-pass
        # before decimation. This measures alias/error against an oversampled donor-free reference.
        grid = 16384
        amps = []
        scale = 29490 / m["bank_peak_q40"][bank] * (2**40) / 32768
        for h, a in partials.items():
            weight = (1 - blend) * (h <= limits[level]) + blend * (
                h <= limits[min(level + 1, 5)]
            )
            if weight:
                amps.append((h, a * weight * scale))
        gain = max(0, min(1, (0.45 - freq) / 0.05))
        if not gain:
            reference = [0j] * n
        elif not k:
            # Exact analytic band-limited Fourier samples: no nonlinear oversampling needed.
            spectrum = [0j] * n
            for h, a in amps:
                j = h * bin * mult
                assert j < n // 2
                spectrum[j] += -1j * a * n / 2 * gain
                spectrum[-j] += 1j * a * n / 2 * gain
            reference = fft(spectrum, True)
        else:
            spectrum = [0j] * grid
            for h, a in amps:
                spectrum[h] = -1j * a * grid / 2
                spectrum[-h] = 1j * a * grid / 2
            wave = [v.real for v in fft(spectrum, True)]
            ref = []
            gain = max(0, min(1, (0.45 - freq) / 0.05))
            for i in range(n * os):
                phase = (i * bin * mult / (n * os)) % 1
                phase += k * phase * (1 - phase)
                pos = (phase % 1) * grid
                j = int(pos)
                f = pos - j
                ref.append((wave[j] * (1 - f) + wave[(j + 1) % grid] * f) * gain)
            spectrum = fft(ref)
            for j in range(n // 2, len(spectrum) - n // 2 + 1):
                spectrum[j] = 0
            reference = fft(spectrum, True)[::os]
        residual = math.sqrt(sum((a - b.real) ** 2 for a, b in zip(got, reference)) / n)
        db = 20 * math.log10(max(residual, 1e-12))
        threshold = -45 if warp == 0 else -30
        report.append(
            dict(
                bank=bank,
                note=note,
                warp=warp,
                mult=mult,
                scan=scan,
                residual_dbfs=round(db, 2),
            )
        )
        if db > threshold:
            raise AssertionError(report[-1])
    print(
        json.dumps(
            {
                "fs": fs,
                "cases": len(report),
                "worst_static_dbfs": max(
                    r["residual_dbfs"] for r in report if not r["warp"]
                ),
                "worst_warp_dbfs": max(r["residual_dbfs"] for r in report if r["warp"]),
                "results": report,
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    check(sys.argv[1], sys.argv[2])
