#!/bin/sh
# SPDX-License-Identifier: MIT
# Intentional word splitting: compiler option list, not user data.
# shellcheck disable=SC2086
set -eu
cd "$(dirname "$0")/.."
mkdir -p build
python3 -m unittest discover -s tests -p test_wave_tables.py
# Probe compiler/runtime availability once. A real fixture failure below is fatal.
wave_san=0
wave_san_label=skipped
if [ "${SANITIZE:-1}" = 0 ]; then
    echo "skip: WAVE S strict ASan/UBSan (SANITIZE=0)"
else
    printf 'int main(void){return 0;}\n' > build/wave-san-probe.c
    if ${CC:-cc} -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
        -fno-omit-frame-pointer build/wave-san-probe.c -o build/wave-san-probe \
        > build/wave-san-probe.log 2>&1 && build/wave-san-probe >> build/wave-san-probe.log 2>&1; then
        wave_san=1
        wave_san_label=passed
    else
        echo "skip: WAVE S strict ASan/UBSan unavailable (build/wave-san-probe.log)"
    fi
fi
if [ "$wave_san" = 0 ] && [ "${WAVE_REQUIRE_SAN:-0}" = 1 ]; then
    echo "FAIL: WAVE S strict ASan/UBSan required"; exit 1
fi

wave_node=${EMSDK_NODE:-node}
if command -v emcc >/dev/null 2>&1; then
    emcc --version
    "$wave_node" --version
    echo "WAVE S Wasm runtime: $wave_node"
    # Detect toolchain entrypoint ABI mismatches before the render matrix.
    emcc -O2 -std=c99 tests/wasm_entry.c -sENVIRONMENT=node -sSINGLE_FILE=1 -o build/wasm-entry.js
    "$wave_node" build/wasm-entry.js > build/wasm-entry.txt
    printf 'wasm-main\n' | cmp - build/wasm-entry.txt
    emcc -O2 -std=c99 -DENTRY_ARGS tests/wasm_entry.c -sENVIRONMENT=node -sSINGLE_FILE=1 -o build/wasm-argv.js
    wave_status=0
    "$wave_node" build/wasm-argv.js wasm-argv > build/wasm-argv.txt || wave_status=$?
    if [ "$wave_status" != 7 ]; then echo "FAIL: Wasm argc/argv main did not return 7"; exit 1; fi
    printf 'wasm-argv\n' | cmp - build/wasm-argv.txt
    echo "ok: Wasm main(void), argc/argv main and exit status 7"
elif [ "${WAVE_REQUIRE_WASM:-0}" = 1 ]; then
    echo "FAIL: emcc required"; exit 1
fi
for fs in 32000 44100 48000 96000 192000; do
    d=build/wave-$fs
    mkdir -p "$d"
    python3 tools/gen_engine_tables.py --fs "$fs" "$d/fe_tables.h"
    python3 tools/gen_wave_tables.py --fs "$fs" "$d/fe_wave_tables.h" --manifest "$d/manifest.json"
    flags="-std=c99 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter"
    ${CC:-cc} -O2 $flags -I"$d" -DFELUCCA_ENGINE_API_REQUIRED=1u -DNPART=1 -DNVOICE=1 \
        -DCONTRACT_FULL_HOST -DCONTRACT_WAVE tests/contract_compile.c -o "$d/contract"
    "$d/contract"
    ${CC:-cc} -O2 $flags -I"$d" tests/wave_small.c -o "$d/test"
    "$d/test" > "$d/renders.txt"
    if [ "$fs" = 44100 ]; then cmp tests/wave_golden.txt "$d/renders.txt"; fi
    if [ "$wave_san" = 1 ]; then
        ${CC:-cc} -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
            $flags -I"$d" tests/wave_small.c -o "$d/test-san"
        "$d/test-san" > "$d/renders-san.txt"
        cmp "$d/renders.txt" "$d/renders-san.txt"
    fi
    ${CC:-cc} -O2 $flags -I"$d" tests/wave_host.c -o "$d/host"
    "$d/host" > "$d/host-renders.txt"
    if [ "$fs" = 44100 ]; then cmp tests/wave_host_golden.txt "$d/host-renders.txt"; fi
    if [ "$wave_san" = 1 ]; then
        ${CC:-cc} -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
            $flags -I"$d" tests/wave_host.c -o "$d/host-san"
        "$d/host-san" > "$d/host-renders-san.txt"
        cmp "$d/host-renders.txt" "$d/host-renders-san.txt"
    fi
    python3 tests/wave_quality.py "$d/test" "$d/manifest.json" > "$d/quality.json"
    echo "ok: WAVE S FS $fs native, strict sanitizers $wave_san_label, controls/bounds and 208 independent quality cases"
done
# A real host/header rate mismatch must fail rather than silently timing slew wrong.
mkdir -p build/wave-mismatch
cp build/wave-44100/fe_wave_tables.h build/wave-mismatch/fe_wave_tables.h
if ${CC:-cc} -std=c99 -Ibuild/wave-mismatch -Ibuild/wave-48000 \
    -DFELUCCA_ENGINE_API_REQUIRED=1u -DNPART=1 -DNVOICE=1 \
    -DCONTRACT_FULL_HOST -DCONTRACT_WAVE tests/contract_compile.c \
    -o build/wave-mismatch/test > build/wave-mismatch/compile.log 2>&1; then
    echo "FAIL: accepted WAVE S sample-rate mismatch"; exit 1
fi
if ! grep -F 'WAVE S assets must be generated for the host FS' build/wave-mismatch/compile.log >/dev/null; then
    cat build/wave-mismatch/compile.log;exit 1
fi
echo "ok: WAVE S rejects sample-rate mismatched assets"
# Cross-architecture exact output, not just successful compilation.
if command -v emcc >/dev/null 2>&1; then
    for fs in 32000 44100 48000 96000 192000; do
        d=build/wave-$fs
        emcc -O2 -std=c99 -I"$d" tests/wave_small.c -sENVIRONMENT=node -sSINGLE_FILE=1 -o "$d/test.js"
        "$wave_node" "$d/test.js" > "$d/renders-wasm.txt"
        cmp "$d/renders.txt" "$d/renders-wasm.txt"
        emcc -O2 -std=c99 -I"$d" tests/wave_host.c -sENVIRONMENT=node -sSINGLE_FILE=1 -o "$d/host.js"
        "$wave_node" "$d/host.js" > "$d/host-renders-wasm.txt"
        cmp "$d/host-renders.txt" "$d/host-renders-wasm.txt"
    done
    echo "ok: WAVE S Wasm/native deterministic at all five rates"
else
    if [ "${WAVE_REQUIRE_WASM:-0}" = 1 ]; then echo "FAIL: emcc required"; exit 1; fi
    echo "skip: WAVE S Wasm (emcc unavailable; required in dedicated CI job)"
fi

python3 tests/wave_resources.py
