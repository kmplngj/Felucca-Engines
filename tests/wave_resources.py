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


# The collection's render callbacks run without a hosted C runtime. Match that
# boundary explicitly; distro-default SSP is a host instrumentation dependency,
# not an engine import. Real unresolved calls remain forbidden below.
PROBE_FLAGS = [
    "-Os",
    "-std=c99",
    "-ffreestanding",
    "-fno-builtin",
    "-fno-stack-protector",
]


def compile_probe(dest, label, definitions=(), flags=PROBE_FLAGS):
    obj = dest / (label + ".o")
    subprocess.run(
        [
            os.environ.get("CC", "cc"),
            *flags,
            "-Ibuild/wave-44100",
            "-Isrc",
            *definitions,
            "-c",
            "tests/wave_resources.c",
            "-o",
            str(obj),
        ],
        check=True,
    )
    return obj


def imports(obj):
    return subprocess.check_output(["nm", "-u", str(obj)], text=True).strip()


def require_no_imports(obj):
    undefined = imports(obj)
    if undefined:
        raise AssertionError("unexpected runtime/library dependency: " + undefined)


def rejected_control(obj, expected):
    undefined = imports(obj)
    if expected not in undefined:
        raise AssertionError(
            "negative control did not contain expected import: " + undefined
        )
    try:
        require_no_imports(obj)
    except AssertionError:
        return undefined
    raise AssertionError("negative control unexpectedly accepted")


def main():
    root = Path(__file__).resolve().parents[1]
    os.chdir(root)
    dest = Path("build/wave-resources")
    dest.mkdir(parents=True, exist_ok=True)
    results = {"probe_flags": PROBE_FLAGS, "host_default_imports": {}}
    for label, extra in [("shared", []), ("wave", ["-DWAVE"])]:
        # Retain the original host-default diagnostic for causal comparisons.
        hosted = compile_probe(dest, label + "-hosted", extra, ["-Os", "-std=c99"])
        results["host_default_imports"][label] = imports(hosted)
        obj = compile_probe(dest, label, extra)
        require_no_imports(obj)  # no allowlist, including stack/runtime helpers
        results[label] = sections(obj)
    delta = results["wave"]["readonly"] - results["shared"]["readonly"]
    assert 50048 <= delta <= 51200, (delta, results)
    assert results["wave"]["writable"] == results["shared"]["writable"] == 0, results
    forced_stack = compile_probe(
        dest, "stack-control", flags=[*PROBE_FLAGS, "-fstack-protector-all"]
    )
    genuine_call = compile_probe(dest, "import-control", ["-DWAVE_IMPORT_CONTROL"])
    results["rejected_controls"] = {
        "forced_host_stack_protector": rejected_control(forced_stack, "stack_chk_fail"),
        "genuine_runtime_call": rejected_control(
            genuine_call, "wave_forbidden_runtime"
        ),
    }
    results.update(
        asset_delta=delta,
        private_cache_bytes=0,
        persistent_arena_bytes=0,
        scope="native standalone object only; no target code-size/CPU/stack claim",
    )
    (dest / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
    print(
        f"ok: WAVE S complete readonly asset delta {delta} <=51200 B; "
        "no writable state or runtime imports; rejects genuine imports and forced host SSP"
    )


if __name__ == "__main__":
    main()
