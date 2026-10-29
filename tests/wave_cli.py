#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bounded CLI validation for the real native or Wasm render harness."""
import hashlib
import json
from pathlib import Path
import resource
import signal
import subprocess
import sys
import tempfile


def limits(maxbytes):
    # A regression must never emit hundreds of GiB or leave a core dump.
    resource.setrlimit(resource.RLIMIT_FSIZE, (maxbytes, maxbytes))
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    if maxbytes == 0:
        signal.signal(signal.SIGXFSZ, signal.SIG_IGN)


def run(command, maxbytes=4096):
    with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
        child = subprocess.Popen(command, stdout=stdout, stderr=stderr, preexec_fn=lambda: limits(maxbytes))
        try:
            status = child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()
            raise AssertionError("CLI exceeded five seconds: " + repr(command[-2:]))
        stdout.seek(0)
        stderr.seek(0)
        return status, stdout.read(4097), stderr.read(4097)


def main():
    fs, command = sys.argv[1], sys.argv[2:]
    golden = json.loads(Path(__file__).with_name("wave_cli_golden.json").read_text())
    failed = []
    cases = 0
    for mode, tail in (("render", []), ("oscillator", ["osc"])):
        for blocks in ("0", "+0", "0000", "1", "+1", "0001"):
            status, data, error = run(command + golden["command_args"] + [blocks] + tail)
            expected = 0 if int(blocks) == 0 else 128
            if status or error or len(data) != expected or (
                expected and hashlib.sha256(data).hexdigest() != golden["one_block_sha256"][fs][mode]
            ):
                failed.append(f"{mode} valid {blocks!r}: exit{status},stdout{len(data)},stderr{error!r}")
            cases += 1
        for blocks in ("-1", "-0", " -1", "\t-1", "\n-1", "", " ", "\t", "+", "text", "1x", "1 ", "1.0", "0x10",
                       "4294967296", "18446744073709551616", "9" * 100):
            status, data, error = run(command + golden["command_args"] + [blocks] + tail)
            if status == 0 or data or b"invalid block count" not in error:
                failed.append(f"{mode} invalid {blocks!r}: exit{status},stdout{len(data)},stderr{error[:100]!r}")
            cases += 1
        status, data, _ = run(command + golden["command_args"] + ["1"] + tail, maxbytes=0)
        if status != 3 or data:
            failed.append(f"{mode} write failure: exit{status},stdout{len(data)}")
        cases += 1
    if failed:
        raise AssertionError("\n".join(failed))
    print(f"ok: WAVE S CLI FS {fs}: {cases} real-process cases; negative output bounded to 4096 B")


if __name__ == "__main__":
    main()
