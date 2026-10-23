# SPDX-License-Identifier: MIT
"""Compare actual standalone object sections, including metadata/alignment/strings."""

import json
import os
from pathlib import Path
import re
import subprocess
import sys


def sections(path):
    darwin = sys.platform == "darwin"
    output = subprocess.check_output(
        ["size", "-m" if darwin else "-A", str(path)], text=True
    )
    ro = rw = 0
    for line in output.splitlines():
        if darwin:
            m = re.search(r"Section \(([^,]+), ([^)]+)\): (\d+)", line)
            if not m:
                continue
            seg, name, count = m.groups()
            count = int(count)
            if name in ("__text", "__compact_unwind", "__eh_frame"):
                continue
            if name in ("__const", "__cstring") or name.startswith("__literal"):
                ro += count
            elif seg == "__DATA":
                rw += count
        else:
            m = re.match(r"(\S+)\s+(\d+)\s+", line)
            if not m:
                continue
            name, count = m.groups()
            count = int(count)
            if name.startswith((".rodata", ".data.rel.ro")):
                ro += count
            elif name.startswith((".data", ".bss", ".sdata", ".sbss")):
                rw += count
    return {"readonly": ro, "writable": rw, "sections": output}


def main():
    root = Path(__file__).resolve().parents[1]
    os.chdir(root)
    dest = Path("build/wave-resources")
    dest.mkdir(parents=True, exist_ok=True)
    results = {}
    for label, extra in [("shared", []), ("wave", ["-DWAVE"])]:
        obj = dest / (label + ".o")
        subprocess.run(
            [
                os.environ.get("CC", "cc"),
                "-Os",
                "-std=c99",
                "-Ibuild/wave-44100",
                "-Isrc",
                *extra,
                "-c",
                "tests/wave_resources.c",
                "-o",
                str(obj),
            ],
            check=True,
        )
        results[label] = sections(obj)
        undefined = subprocess.check_output(["nm", "-u", str(obj)], text=True).strip()
        if undefined:
            raise AssertionError("unexpected runtime/library dependency: " + undefined)
    delta = results["wave"]["readonly"] - results["shared"]["readonly"]
    assert 50048 <= delta <= 51200, (delta, results)
    assert results["wave"]["writable"] == results["shared"]["writable"] == 0, results
    results.update(
        asset_delta=delta,
        private_cache_bytes=0,
        persistent_arena_bytes=0,
        scope="native object only; no target code-size/CPU/stack claim",
    )
    (dest / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
    print(
        f"ok: WAVE S complete readonly asset delta {delta} <=51200 B; no writable state or runtime imports"
    )


if __name__ == "__main__":
    main()
