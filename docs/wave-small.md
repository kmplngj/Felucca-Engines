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
| s[2] | last SCAN effective target Q16 | base SCAN | note_on result per API |
| s[3] | last WARP target Q16 | base WARP | note_on result per API |
| s[4] | previous BANK + 4*(MULT-1) | base configuration | keep bit4 |
| s[5] | SCAN smoothed Q16 control | base SCAN | keep bit5 |
| s[6] | WARP smoothed Q16 control | base WARP | keep bit6 |
| s[7] | unused | zero | reset |

`.keep = 0x73`. Host envelope, release, steal, pitch and sounding-retrigger phase
preservation remain API v1 responsibilities. Smoothed states occupy keep-supported
slots; without extra lag, the ramp starts from that preserved state on retrigger. Each short call advances block state
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
A single unguarded level is 8,192 bytes. Samples use 49,536 bytes, rate-specific Q24 slew coefficients 512 bytes: **50,048
bytes** total (below 50 KiB). Shared DSP tables and engine descriptors/presets are
not newly generated tables. The object-section regression also counts all new
labels, descriptors, presets, pointers and alignment against the 50 KiB total.
Generate this header only when WAVE S is selected.

The native object probe uses `-Os -std=c99 -ffreestanding -fno-builtin
-fno-stack-protector` for both the shared baseline and WAVE S: hosted compiler
stack-protector defaults do not model the bare-metal runtime. All undefined
imports remain forbidden. The test records host-default imports separately and
proves rejection with forced stack protection and a genuine unresolved-call
control; renderer ASan/UBSan checks retain their full instrumentation.

SLEW=0 is the 32-sample control ramp only. 1..127 adds a per-sample one-pole with
linear time constant 1..200 ms, generated for FS. 32..96 kHz is the quality range;
96..192 kHz builds under v1 with the shared filter quantization limits.

Firmware adoption, stable global IDs, native MOD/persistence/client wiring and
real FM-1 deadlines/stack/audio qualification remain Felucca #363/#365. This
collection does not select an engine in firmware or provide a flashable package.

## Controls and rendering

| Control | Range / default | Behavior |
| --- | --- | --- |
| BANK | 0..3 / 0 | HARM, HOLLOW, FORMANT, GLASS |
| SCAN | 0..127 / 0 | Linear interpolation across frames 0..7, no wrap |
| WARP | -63..63 / 0 | Monotone quadratic phase bend, exact zero bypass |
| MULT | 1..4 / 1 | Main frequency multiplier; SUB remains root/2 |
| CUT | 0..127 / 127 | Shared low-pass cutoff plus vmod.cutoff Q8 offset |
| RES | 0..127 / 0 | Shared low-pass resonance |
| SUB | 0..127 / 0 | Raw weight 0..0.5 relative to main, normalized to total 1 |
| SLEW | 0..127 / 0 | Extra SCAN/WARP smoothing; see above |

SHP offsets SCAN: clamp(baseSCAN*256 + shape - 64*256, 0, 127*256).
PITCH is the supplied increment; AMP follows the supplied 32-sample ramp. No
host envelope, allocator, mixer, MOD matrix, filesystem or scheduler is added.
BANK and MULT crossfade together from the previous pair to the new pair over
one control tick, with at most two configurations. Continuous targets ramp over
32 samples before optional smoothing. Main and normalized SUB feed the shared
per-voice SVF, then bounded Q15 output feeds voice_amp and adds to the host mix.
Resonant output clamps to Q15 before voice_amp to preserve its product bounds.

The band selector accounts for the maximum absolute current, prior ramp and new
bend target, then taper, so an in-flight slow ramp cannot underestimate slope.
Each harmonic level fades to its successor between 75% and 100% of its 0.45 FS
harmonic limit. Main fades 0.40..0.45 FS; bend tapers 0.10..0.20 FS. Root frequency
is multiplied in uint64 before modulo phase lookup; even UINT32_MAX*4 is <2^34.
SUB uses its own root/2 frequency limit and never inherits main's multiplier.

## Arithmetic and bounded work

- Phase interpolation uses Q15 fraction; samples stay within +-29490. A difference
  times 32768 is <2^31. SCAN position uses a fixed reciprocal, exact at both ends.
- WARP uses Q16 control and Q15 k, clamped to +-24576. For x=phase>>16,
  x*(65536-x) <=2^30; its Q16 curve <=16384 times k <=402653184.
  Delta times 65536 fits signed 32 bits. The phase addition is unsigned modulo.
  Quantized phase remains monotone at both extremes; zero keeps all 32 phase bits.
  Signed interpolation/warp shifts round down, consistent with shared DSP. Slew
  steps round symmetrically to nearest with a one-Q16-LSB minimum toward target,
  so zero WARP cannot retain a permanent fractional bend. The 32-sample raw
  ramp explicitly lands on its endpoint despite integer step truncation.
- Slew products are int64: target difference <2^24 times Q24 alpha <2^24
  fits <2^48. Normalized source mixing is bounded Q15. Filter coefficients are
  calculated once per call using shared DSP; all divisions occur outside the
  sample loop. Band blend uses exactly 15 restoring steps, avoiding a 64-bit
  division runtime helper. Level selection has at most five iterations.
- Every sample has at most two configurations, two levels, two frames and two
  phase samples each. Render loops exactly n<=32; note initialization has fixed
  3/8-iteration loops. There is no allocation, floating point, runtime baking,
  external service, CPU2 dependency or mutable global state in the engine.

## Source and release inventory

Include LICENSE, src/eng_wave_small.c, shared src/felucca_engine.h and src/dsp.c,
tools/gen_engine_tables.py, tools/gen_wave_tables.py, assets/wave_small/recipes.json
and this guide when distributing source. The manifest contains the exact recipe
SHA-256, canonical little-endian sample SHA-256, bank normalizers, FS, dimensions
and byte costs. tests/wave_assets.sha256 freezes the approved original sample
content; tests/wave_golden.txt freezes the eight control-sweep phrases and
tests/wave_host_golden.txt freezes eight phrases from the unchanged v1 test host,
including its real note-on/retrigger/release-envelope path. Generated
headers, manifests, WAVs and logs stay under ignored build/; immutable headers are
reproducible inputs, not runtime files. Existing shared sources keep their notices.

## Offline user audition

Use the reviewed collection revision, Python 3, a C99 compiler, and optionally
Emscripten plus Node for the cross-architecture gate. From the repository root:

1. Run `sh tests/run.sh`. Expect existing 122 goldens unchanged, asset/content checks,
   five-rate conformance, available strict sanitizers and oscillator quality checks.
   Local `SANITIZE=0` opts out; unavailable sanitizer compiler/runtime support is
   reported once. `WAVE_REQUIRE_SAN=1` rejects either case and is set for every CI
   job. A failed real sanitized fixture never becomes an availability skip. An absent
   local Emscripten is an explicit skip; `WAVE_REQUIRE_WASM=1 sh tests/wave_small.sh`
   requires it. No FM-1 is connected or flashed by these commands.
2. Run `python3 tools/wave_audition.py`. Open build/wave-0-pure.wav through
   build/wave-7-root.wav with headphones at low level. These are mono 44.1 kHz
   fixtures: notes 36/60/84/108, SCAN modulation and cutoff sweep, simultaneous
   BANK/MULT changes at block 300, return with negative WARP at block 600, and a
   release-amplitude tail after block 800. PURE should start sine-like; HOLLOW has
   odd-partial color, VOWEL moves two spectral peaks, GLASS has sparse clusters,
   ROOT preserves low SUB while main MULT changes. DRIFT demonstrates slow scan.
3. Report revision, compiler/rate, preset, time and audible result for any click,
   unexpected silence or unstable tail. Recreate artifacts with the same commands.
   Preview fixtures use the supplied amplitude ramp, not a firmware envelope.

The independent quality fixture uses exact analytic Fourier samples for static
controls. For nonlinear controls it synthesizes original Fourier coefficients on
a dense grid, applies continuous phase bend at 16x sample rate, ideally low-passes
and decimates. Its 208 cases per rate compare the oscillator before the shared filter at coherent
notes near MIDI 36/60/84/108/127, all banks/eight frame regions and warp 0/+-63, plus MULT 2/4 boundaries, using absolute residual
thresholds -45 dBFS static / -30 dBFS warped. It includes interpolation error and
nonlinear alias residual; it does not assert PCM identity, model device analog
output, or qualify all modulation, native profiles or target CPU/stack budgets.
Eight deterministic complete-path fixtures separately exercise filtering,
modulation, multipliers, sub, discrete transitions and release amplitude.

There is no flashable firmware or panel route in this collection. Real FM-1 sound,
resource/deadline/stack acceptance and native save/load/restart remain pending in
Felucca #363/#365. The higher-rate API builds retain the shared filter's documented
fixed-point limitations. Host test success does not establish firmware acceptance.
