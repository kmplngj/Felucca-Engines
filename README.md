# Felucca-Engines

Five synthesis engines of [Felucca](https://github.com/hugelton/Felucca), with the DSP helpers
and the tables they share, under the MIT licence. Plain C99, fixed point only (no float, no
allocation, no library calls), written for a small 32-bit microcontroller at 44.1 kHz.

| Engine | File | What it is |
| --- | --- | --- |
| ANALOG | `src/eng_analog.c` | Two band-limited oscillators (saw, square, triangle, sine, PWM; hard sync and a sub oscillator), noise, drive, a trapezoidal state-variable filter (low-, band- or high-pass) |
| LOFI | `src/eng_lofi.c` | A chip voice: pulse, triangle, saw, shift-register noise and a 32 x 4-bit wave memory (16 tables), bit depth, sample hold, pitch sweep, arpeggio, a stepped volume envelope |
| TRIO | `src/eng_trio.c` | Three oscillators with ring modulation, hard sync and combined (AND) waves into a 12 dB/oct multimode filter |
| WHEEL | `src/eng_wheel.c` | A tonewheel-style additive organ: nine drawbar partials, 16 registrations, percussion, key click, drive and a rotary effect |
| NOISE | `src/eng_noise.c` | White / pink / brown noise, random impulses and shift-register noise through a resonant filter, with drift and a bit crusher |

Shared:

- `src/felucca_engine.h`: the engine interface (parameter descriptors, the voice, its per-block
  modulation, presets, the engine table entry) and what a host must provide.
- `src/dsp.c`: oscillators with polyBLEP, the state-variable filter, soft clipping, noise, the
  amplitude ramp (all `static inline`).
- `tools/gen_engine_tables.py`: the lookup tables (`SINE`, `PITCH_OCT` / `pitch_inc()`, `SVF_G`,
  `TANH_Q15`, `FS`, `CTL`) as a C header.

## Using the engines

The engines are compiled as one translation unit with their host, in this order:

```c
#define FELUCCA_ENGINE_API_REQUIRED 1u /* host implements contract v1 */
#define NVOICE 8                    /* voices per part */
#define NPART 4                     /* parts */
#include "fe_tables.h"              /* python3 tools/gen_engine_tables.py fe_tables.h */
#include "felucca_engine.h"
enum { P_ATK, P_REL, P_FTYPE, P_E0, P_E1, P_E2, P_E3, P_E4, P_E5, P_E6, P_E7, P_COUNT };
typedef struct track { int16_t p[P_COUNT]; voice_t v[NVOICE]; } track_t;
static track_t trk[NPART];
#include "dsp.c"
#include "eng_analog.c"             /* ... the engines you want */
```

Each engine is an `engine_t` (`ENG_ANALOG`, `ENG_LOFI`, `ENG_TRIO`, `ENG_WHEEL`, `ENG_NOISE`):
its eight parameters with their ranges and names, its presets, and the functions the host calls
per voice (`note_on`, `render`, optional `amp`) and per part/block (optional `block`). `render` adds one block (at most
`CTL` = 32 samples) of a voice to a buffer. The header documents the rest of the contract: the
host's track and parameter numbers, how a voice is started and retriggered, and the envelope
fields that LOFI and WHEEL read.

### Contract v1

`src/felucca_engine.h` defines `FELUCCA_ENGINE_API_VERSION` as `1u`. Before including it, a host
must define `FELUCCA_ENGINE_API_REQUIRED` to exactly `1u`, positive integer `NPART` and `NVOICE`,
and `FS = 44100`, `CTL = 32` (normally supplied by the generated tables). A missing requirement,
version mismatch, unsupported rate/block size or nonpositive dimension is a compile error.
The revision describes the source integration contract, not a firmware release or a binary ABI.
The shared type layouts are unchanged. There is no implied compatibility with a newer revision.

These rate limits describe the existing DSP: the amplitude and WHEEL gain ramps use shifts for
32 samples, and WHEEL's rotor/fold constants and NOISE's rate coefficients assume 44.1 kHz.
Changing generated tables alone cannot support another rate. Dimensions have no arbitrary
upper cap in this library; the host must fit its arrays and mix headroom to its platform.

- Pitch is in 1/16 semitone (`pitch16 = MIDI note * 16`, table range 0..2047); phase increments
  are `frequency / FS * 2^32` per sample. `fine` is an increment correction in 1/4096 units.
  Envelope state is Q24 (`1 << 24` = full); amplitude and modulation envelope values are Q15
  (`32768` = full). `cutoff` is a signed offset, 256 per cutoff table index, with 0 neutral.
  `shape` is an absolute 0..127*256 control with 64*256 neutral. Engine parameters use the
  descriptor's range, rather than normalized floats.
- Render adds to a separate `int32_t` output accumulator. The host clears a fresh mix, provides
  at least `n` elements and prevents accumulation overflow. The per-voice scale is `VOICE_FS`
  (24000 at unit oscillator amplitude), with engine-dependent headroom; clipping and output
  conversion belong to the host. Normal renders use 32 samples; `1 <= n <= 32` is supported.
  A short call advances block-rate state once and follows the 32-sample amplitude ramp, whose
  endpoint is the next block boundary. Multiple short calls do not reproduce one full block.
  Output overlapping engine state is not a usable output buffer: state writeback can overwrite it.
  This clarifies earlier alias comments: engines read voice state into locals, then write it back
  after accumulating samples.
- Zero a slot's phases and engine state before its first start or reassignment to a different
  engine. For a sounding voice's retrigger, save its state, set the new note/gate/age, call
  `note_on`, restore all three phases for a non-sampled engine, and restore `s[k]` where bit `k`
  of `keep` is set, except `s[2]` and `s[3]`, which always retain `note_on`'s result. Sampled
  engines keep no phases. All five engines here are non-sampled.
- One owner serializes a part's parameter changes, note/gate events and callbacks. Run `block`
  once per control tick before its voices, including idle parts; run an optional `amp` after
  the host's envelope tick and skip render if it ends the voice. Different parts can run in
  parallel with separate output buffers and exclusive ownership of each part's state. WHEEL
  indexes private state by `t - trk` and `v - t->v`, so pass actual elements of the contiguous
  `trk[NPART]` and `t->v[NVOICE]` arrays. Do not clear its arrays while any part is running.
  Callbacks within one part are not covered by this parallel ownership guarantee.

`tests/fe_host.c` is a complete, minimal host: it plays every preset through a short phrase.

## Tests

```sh
sh tests/run.sh
```

checks positive and negative C99 compile fixtures for the host handshake and requirements,
then verifies additive output, short-buffer bounds, retrigger state and interleaved part ownership.
It builds the tables and the host, renders 108 phrases and compares a hash of each with
`tests/golden.txt`. A change of any engine's output fails. Felucca runs the same host with its own
generated tables, so the two cannot drift apart.
Set `CC=gcc` or `CC=clang` to select a compiler. Contract fixtures do not change the golden file.

## In Felucca

Felucca includes this repository as a git submodule (`engines/`, next to its `src/`). Its
`engines.c` includes `dsp.c` and the five `eng_*.c` files from here, its `core.h` includes
`felucca_engine.h`, and its table generator imports `tools/gen_engine_tables.py`. The firmware
built from them is GPL-3.0-only as a whole; the files of this repository stay MIT.

Clone Felucca with `git clone --recurse-submodules` (or run `git submodule update --init` in a
clone): its complete source includes this repository.

## Credits

The state-variable filter follows Andrew Simper's trapezoidal design; the pink noise filter uses
Paul Kellet's coefficients (the "economy" version), in fixed point. polyBLEP and xorshift32 are
the well-known techniques.

## Licence

MIT, see [LICENSE](LICENSE). Copyright (c) 2026 Leo Kuroshita (@kurogedelic), Hügelton
Instruments.

Changes to these files are welcome as pull requests here, under the MIT licence.
