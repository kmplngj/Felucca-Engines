#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Original WAVE S assets. Decimal arithmetic makes output independent of libm."""

import argparse
from decimal import Decimal, localcontext, ROUND_HALF_UP
import hashlib
import json
from pathlib import Path
import struct

LIMITS = (63, 31, 15, 7, 3, 1)
RECIPE = Path(__file__).resolve().parents[1] / "assets/wave_small/recipes.json"


def rounded(x):
    return int(x.to_integral_value(rounding=ROUND_HALF_UP))


def validate(recipe):
    if not isinstance(recipe, dict):
        raise ValueError("recipe must be an object")
    if (
        set(recipe) != {"schema", "license", "authorship", "banks"}
        or type(recipe["schema"]) is not int
        or recipe["schema"] != 1
        or recipe["license"] != "MIT"
    ):
        raise ValueError("unsupported recipe schema or license")
    if not isinstance(recipe["banks"], list) or len(recipe["banks"]) != 4:
        raise ValueError("expected four banks")
    for b, bank in enumerate(recipe["banks"]):
        if not isinstance(bank, dict):
            raise ValueError(f"bank {b} must be an object")
        if (
            set(bank) != {"id", "name", "frames"}
            or type(bank["id"]) is not int
            or bank["id"] != b
            or bank["name"] != ("HARM", "HOLLOW", "FORMANT", "GLASS")[b]
        ):
            raise ValueError("bank IDs/order/names are a stable contract")
        if not isinstance(bank["frames"], list):
            raise ValueError(f"bank {b} frames must be an array")
        if len(bank["frames"]) != 8:
            raise ValueError("expected eight frames")
        for f, frame in enumerate(bank["frames"]):
            if not isinstance(frame, dict):
                raise ValueError(f"bank {b} frame {f} must be an object")
            if not isinstance(frame.get("partials"), list):
                raise ValueError(f"bank {b} frame {f} partials must be an array")
            if (
                set(frame) != {"id", "partials"}
                or type(frame["id"]) is not int
                or frame["id"] != f
                or not frame["partials"]
            ):
                raise ValueError("invalid frame ID or empty partials")
            seen = set()
            for part in frame["partials"]:
                if not isinstance(part, dict):
                    raise ValueError(f"bank {b} frame {f} partial must be an object")
                if set(part) != {"harmonic", "amplitude", "phase128"} or any(
                    type(v) is not int for v in part.values()
                ):
                    raise ValueError(
                        "partial requires integer harmonic/amplitude/phase128"
                    )
                h, a, phase = (part[k] for k in ("harmonic", "amplitude", "phase128"))
                if not 1 <= h <= 63 or h in seen or not 1 <= a <= 262144 or phase != 0:
                    raise ValueError(
                        "partial range, duplicate harmonic or unaligned phase"
                    )
                seen.add(h)
            if 1 not in seen:
                raise ValueError("every frame needs the fundamental at every level")


def generate(recipe, fs=44100):
    validate(recipe)
    if type(fs) is not int or not 32000 <= fs <= 192000:
        raise ValueError("FS must be 32000..192000")
    with localcontext() as ctx:
        ctx.prec = 80
        pi = Decimal(
            "3.14159265358979323846264338327950288419716939937510582097494459230781640628620899"
        )
        sine = []
        for n in range(65):
            x = pi * n / 64
            term = total = x
            for k in range(1, 100):
                term *= -x * x / ((2 * k) * (2 * k + 1))
                total += term
            sine.append(rounded(total * (1 << 40)))
        sine += [-sine[128 - n] for n in range(65, 128)]
        tables, peaks = [], []
        for bank in recipe["banks"]:
            raw = [
                [
                    [
                        sum(
                            p["amplitude"] * sine[(n * p["harmonic"]) % 128]
                            for p in frame["partials"]
                            if p["harmonic"] <= limit
                        )
                        for n in range(128)
                    ]
                    for limit in LIMITS
                ]
                for frame in bank["frames"]
            ]
            peak = max(abs(x) for frame in raw for level in frame for x in level)
            peaks.append(peak)
            levels = []
            for frame in raw:
                values = []
                for level in frame:
                    q = [rounded(Decimal(x) * 29490 / peak) for x in level]
                    values.append(q + q[:1])
                levels.append(values)
            tables.append(levels)
        slew = [0]
        for i in range(1, 128):
            tau = Decimal(1) + Decimal(i - 1) * 199 / 126
            slew.append(rounded((1 - (-1000 / (Decimal(fs) * tau)).exp()) * (1 << 24)))
    return tables, slew, peaks


def export(recipe_path, output, fs=44100, manifest=None):
    data = Path(recipe_path).read_bytes()
    recipe = json.loads(data)
    tables, slew, peaks = generate(recipe, fs)
    packed = b"".join(
        struct.pack("<h", x) for b in tables for f in b for l in f for x in l
    )
    header = "/* SPDX-License-Identifier: MIT; original generated WAVE S assets. */\n#pragma once\n#include <stdint.h>\n"
    header += (
        "#define FE_WAVE_FS %d\nstatic const int16_t FE_WAVE[4][8][6][129] = {\n" % fs
    )
    for bank in tables:
        header += "{\n"
        for frame in bank:
            header += (
                "{\n"
                + "".join("{" + ",".join(map(str, l)) + "},\n" for l in frame)
                + "},\n"
            )
        header += "},\n"
    header += (
        "};\nstatic const uint32_t FE_WAVE_SLEW[128] = {"
        + ",".join(map(str, slew))
        + "};\n"
    )
    Path(output).write_text(header, encoding="ascii")
    if manifest:
        Path(manifest).write_text(
            json.dumps(
                {
                    "schema": 1,
                    "fs": fs,
                    "recipe_sha256": hashlib.sha256(data).hexdigest(),
                    "samples_sha256_le": hashlib.sha256(packed).hexdigest(),
                    "sample_bytes": len(packed),
                    "slew_bytes": 512,
                    "asset_bytes": len(packed) + 512,
                    "limits": LIMITS,
                    "bank_peak_q40": peaks,
                    "tables": tables,
                },
                indent=2,
            )
            + "\n"
        )


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("output")
    p.add_argument("--recipes", default=RECIPE)
    p.add_argument("--manifest")
    p.add_argument("--fs", type=int, default=44100)
    a = p.parse_args()
    try:
        export(a.recipes, a.output, a.fs, a.manifest)
    except (ValueError, TypeError, KeyError, OSError) as e:
        p.error(str(e))


if __name__ == "__main__":
    main()
