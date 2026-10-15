#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
# Invoked from the repository root by tests/run.sh; build/ already exists.
set -e

compile() {
    ${CC:-cc} -std=c99 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter \
        "$@" -Ibuild -o build/contract_compile tests/contract_compile.c
}

good='-DFELUCCA_ENGINE_API_REQUIRED=1u -DNVOICE=8 -DNPART=4 -DFS=44100 -DCTL=32'
compile $good -DCONTRACT_FULL_HOST
build/contract_compile
compile -DFELUCCA_ENGINE_API_REQUIRED=1u -DNVOICE=1 -DNPART=1 -DFS=44100 -DCTL=32 -DCONTRACT_FULL_HOST
build/contract_compile

# Use real rate-specific tables so these fixtures compile every engine, including the rate constants.
for fs in 32000 48000 96000 192000; do
    d=build/contract-fs$fs
    mkdir -p "$d"
    python3 tools/gen_engine_tables.py --fs "$fs" "$d/fe_tables.h"
    compile -DFELUCCA_ENGINE_API_REQUIRED=1u -DNVOICE=8 -DNPART=4 -I"$d" -DCONTRACT_FULL_HOST
    build/contract_compile
done

expect_failure() {
    label=$1
    diagnostic=$2
    shift 2
    if compile "$@" > build/contract_compile.log 2>&1; then
        echo "FAIL: contract compile unexpectedly accepted $label"
        exit 1
    fi
    if ! grep -F "$diagnostic" build/contract_compile.log > /dev/null; then
        echo "FAIL: contract compile gave the wrong diagnostic for $label"
        cat build/contract_compile.log
        exit 1
    fi
}

expect_failure 'missing version' 'host must define FELUCCA_ENGINE_API_REQUIRED' \
    -DNVOICE=8 -DNPART=4 -DFS=44100 -DCTL=32
for version in 0u 2u; do
    expect_failure "version $version" 'incompatible FELUCCA_ENGINE_API_REQUIRED' \
        $good -UFELUCCA_ENGINE_API_REQUIRED -DFELUCCA_ENGINE_API_REQUIRED=$version
done
for dim in NPART NVOICE; do
    expect_failure "missing $dim" "the host defines $dim" $good -U$dim
    for value in 0 -1; do
        expect_failure "$dim=$value" "$dim must be a positive integer constant" \
            $good -U$dim -D$dim=$value
    done
done
expect_failure 'missing FS' 'the host defines FS' $good -UFS
for fs in 31999 192001; do
    expect_failure "FS=$fs" 'FS must be from 32000 to 192000' $good -UFS -DFS=$fs
done
expect_failure 'missing CTL' 'the host defines CTL' $good -UCTL
for ctl in 16 64; do
    expect_failure "CTL=$ctl" 'contract v1 requires CTL == 32' $good -UCTL -DCTL=$ctl
done
echo 'ok: contract compile (6 positive, 15 negative fixtures)'
