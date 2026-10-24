#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Make eight original WAVE S preview WAVs using the already-built standalone fixture."""

import argparse
from pathlib import Path
import subprocess
import struct
import wave


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--host", default="build/wave-44100/test")
    a = p.parse_args()
    subprocess.run([str(Path(a.host).resolve()), "audition"], check=True)
    raw = Path("build/wave-audition-s32le.raw").read_bytes()
    frames = 1024 * 32
    for i, name in enumerate(
        ("pure", "bright", "drift", "hollow", "notch", "vowel", "glass", "root")
    ):
        values = struct.unpack(
            "<%di" % frames, raw[i * frames * 4 : (i + 1) * frames * 4]
        )
        path = Path("build") / f"wave-{i}-{name}.wav"
        with wave.open(str(path), "wb") as f:
            f.setnchannels(1)
            f.setsampwidth(2)
            f.setframerate(44100)
            f.writeframes(struct.pack("<%dh" % frames, *values))
        print(path)


if __name__ == "__main__":
    main()
