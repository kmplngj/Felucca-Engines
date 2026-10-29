# SPDX-License-Identifier: MIT
import copy
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import tempfile
import subprocess
import sys
import unittest

spec = importlib.util.spec_from_file_location(
    "wavegen", Path(__file__).resolve().parents[1] / "tools/gen_wave_tables.py"
)
g = importlib.util.module_from_spec(spec)
spec.loader.exec_module(g)


class Tables(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.recipe = json.loads(g.RECIPE.read_text())
        cls.tables, cls.slew, _ = g.generate(cls.recipe)

    def test_content(self):
        for bank in self.tables:
            for frame in bank:
                for limit, level in zip(g.LIMITS, frame):
                    self.assertEqual(len(level), 129)
                    self.assertEqual(level[0], level[-1])
                    self.assertLessEqual(max(map(abs, level)), 29490)
                    self.assertLessEqual(abs(sum(level[:-1])), 128)
                    self.assertEqual(level[0], 0)
                    self.assertGreater(level[1], 0)
                    for n in range(1, 128):
                        self.assertEqual(level[n], -level[128 - n])
                    for k in range(limit + 1, 65):
                        re = (
                            sum(
                                x * math.cos(2 * math.pi * k * n / 128)
                                for n, x in enumerate(level[:-1])
                            )
                            / 64
                        )
                        im = (
                            sum(
                                x * math.sin(2 * math.pi * k * n / 128)
                                for n, x in enumerate(level[:-1])
                            )
                            / 64
                        )
                        self.assertLess(math.hypot(re, im), 1.1)

    def test_independent_normalization(self):
        for recipe, bank in zip(self.recipe["banks"], self.tables):
            raw = [
                [
                    [
                        sum(
                            p["amplitude"]
                            * math.sin(2 * math.pi * n * p["harmonic"] / 128)
                            for p in frame["partials"]
                            if p["harmonic"] <= limit
                        )
                        for n in range(128)
                    ]
                    for limit in g.LIMITS
                ]
                for frame in recipe["frames"]
            ]
            peak = max(abs(x) for frame in raw for level in frame for x in level)
            for rf, qf in zip(raw, bank):
                for rl, ql in zip(rf, qf):
                    for x, q in zip(rl, ql):
                        self.assertLessEqual(abs(q - x * 29490 / peak), 0.501)

    def test_deterministic_and_budget(self):
        with tempfile.TemporaryDirectory() as d:
            a, b = Path(d) / "a.h", Path(d) / "b.h"
            g.export(g.RECIPE, a, manifest=Path(d) / "a.json")
            g.export(g.RECIPE, b)
            self.assertEqual(a.read_bytes(), b.read_bytes())
            m = json.loads((Path(d) / "a.json").read_text())
            self.assertEqual(m["sample_bytes"], 49536)
            self.assertLessEqual(m["asset_bytes"], 51200)
            self.assertEqual(
                m["samples_sha256_le"],
                Path(__file__).with_name("wave_assets.sha256").read_text().strip(),
            )

    def test_slew_time_constants(self):
        for fs in (32000, 44100, 48000, 96000, 192000):
            _, slew, _ = g.generate(self.recipe, fs)
            self.assertEqual(slew[0], 0)
            for i in (1, 32, 64, 127):
                tau = -1 / (fs * math.log1p(-slew[i] / 2**24)) * 1000
                self.assertAlmostEqual(tau, 1 + (i - 1) * 199 / 126, delta=0.1)

    def test_cli_invalid(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "bad.json"
            out = Path(d) / "out.h"
            for text in ("{", "{}"):
                path.write_text(text)
                r = subprocess.run(
                    [sys.executable, g.__file__, str(out), "--recipes", str(path)],
                    capture_output=True,
                )
                self.assertNotEqual(r.returncode, 0)
                self.assertIn(b"error:", r.stderr)
                self.assertFalse(out.exists())
            r = subprocess.run(
                [
                    sys.executable,
                    g.__file__,
                    str(out),
                    "--recipes",
                    str(Path(d) / "absent"),
                ],
                capture_output=True,
            )
            self.assertNotEqual(r.returncode, 0)
            self.assertIn(b"error:", r.stderr)

    def test_invalid_container_shapes(self):
        cases = [(None, "recipe"), (42, "recipe")]
        for value in (None, 42):
            for field in ("banks", "bank", "frames", "frame", "partials", "partial"):
                recipe = copy.deepcopy(self.recipe)
                if field == "banks":
                    recipe["banks"] = value
                elif field == "bank":
                    recipe["banks"][0] = value
                elif field == "frames":
                    recipe["banks"][0]["frames"] = value
                elif field == "frame":
                    recipe["banks"][0]["frames"][0] = value
                elif field == "partials":
                    recipe["banks"][0]["frames"][0]["partials"] = value
                else:
                    recipe["banks"][0]["frames"][0]["partials"][0] = value
                cases.append((recipe, field))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad-shape.json"
            for recipe, field in cases:
                with self.subTest(field=field, recipe=recipe):
                    with self.assertRaisesRegex(ValueError, field):
                        g.generate(recipe)
                    path.write_text(json.dumps(recipe))
                    result = subprocess.run(
                        [
                            sys.executable,
                            g.__file__,
                            str(Path(directory) / "out.h"),
                            "--recipes",
                            str(path),
                        ],
                        capture_output=True,
                        text=True,
                    )
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(field, result.stderr)
                    self.assertNotIn("Traceback", result.stderr)

    def test_invalid(self):
        for mutate in (
            lambda r: r.update(schema=2),
            lambda r: r["banks"].pop(),
            lambda r: r["banks"][0].update(id=1),
            lambda r: r["banks"][0]["frames"].pop(),
            lambda r: r["banks"][0]["frames"][0]["partials"][0].update(harmonic=64),
            lambda r: r["banks"][0]["frames"][0]["partials"][0].update(
                amplitude=262145
            ),
            lambda r: r["banks"][0]["frames"][0]["partials"][0].update(phase128=1),
            lambda r: r["banks"][0]["frames"][0]["partials"].append(
                r["banks"][0]["frames"][0]["partials"][0]
            ),
        ):
            r = copy.deepcopy(self.recipe)
            mutate(r)
            with self.assertRaises(ValueError):
                g.generate(r)
        for fs in (31999, 192001):
            with self.assertRaises(ValueError):
                g.generate(self.recipe, fs)


if __name__ == "__main__":
    unittest.main()
