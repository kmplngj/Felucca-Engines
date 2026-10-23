<!-- SPDX-License-Identifier: MIT -->
# WAVE S portable contract

WAVE S is original MIT harmonic synthesis, independent of DUELL WAVE. No external
waveform, recording, preset or DSP source is included. Recipes and generated assets
are MIT under the repository LICENSE. Bank IDs HARM=0, HOLLOW=1, FORMANT=2,
GLASS=3 and frame IDs 0..7 are stable sound contracts.

## State allocation decided before DSP implementation

No shared type/layout change, persistent arena or per-voice side array is needed.
Private part cache: **0 bytes**. The host serializes callbacks for a part.

| Existing field | Units / meaning | Fresh note | Sounding retrigger |
| --- | --- | --- | --- |
| ph[0] | Q32 root phase, MULT applied at lookup | zero | host preserves |
| ph[1] | Q32 root/2 SUB phase | zero | host preserves |
| ph[2] | unused | zero | host preserves |
| s[0..1] | shared SVF integrators | zero | keep bits 0,1 |
| s[2] | SCAN smoothed Q16 control | base SCAN | note_on result per API |
| s[3] | WARP smoothed Q16 control | base WARP | note_on result per API |
| s[4] | previous BANK + 4*(MULT-1) | base configuration | keep bit4 |
| s[5] | last SCAN effective target Q16 | base SCAN | keep bit5 |
| s[6] | last WARP target Q16 | base WARP | keep bit6 |
| s[7] | unused | zero | reset |

`.keep = 0x73`. Host envelope, release, steal, pitch and sounding-retrigger phase
preservation remain API v1 responsibilities. Each short call advances block state
once; it is bounded and follows the 32-sample ramp, not partition invariant.

## Original assets

`assets/wave_small/recipes.json` is the readable source. Each partial has an integer
harmonic, positive amplitude (1..262144) and phase128=0: every frame and level has the same
positive-going sine origin. HARM progressively adds partials; HOLLOW uses odd
partials with moving notches; FORMANT has two moving triangular harmonic peaks;
GLASS uses sparse integer clusters. A common reciprocal-harmonic envelope
keeps 128-point linear-interpolation images beneath the oscillator quality gates;
HARM/HOLLOW consequently decay approximately as 1/h². No runtime baking or asset copies occur.

The generator uses 80-digit Decimal sine Taylor sums and half-away-from-zero
rounding, independent of platform libm. Opposite samples are antisymmetric, so DC
is exactly zero. One common peak normalizer per bank across all frames/levels
bounds every sample to 29490 (<0.9 Q15). Levels retain harmonics 63/31/15/7/3/1.
Every level has 128 samples plus a repeated first sample, including sine-only.
Samples use 49,536 bytes, rate-specific Q24 slew coefficients 512 bytes: **50,048
bytes** total (below 50 KiB). Shared DSP tables and engine descriptors/presets are
not newly generated tables. The object-section regression also counts all new
labels, descriptors, presets, pointers and alignment against the 50 KiB total.
Generate this header only when WAVE S is selected.

SLEW=0 is the 32-sample control ramp only. 1..127 adds a per-sample one-pole with
linear time constant 1..200 ms, generated for FS. 32..96 kHz is the quality range;
96..192 kHz builds under v1 with the shared filter quantization limits.

Firmware adoption, stable global IDs, native MOD/persistence/client wiring and
real FM-1 deadlines/stack/audio qualification remain Felucca #363/#365. This
collection does not select an engine in firmware or provide a flashable package.

