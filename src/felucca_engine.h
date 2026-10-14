/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Felucca-Engines: the engine interface. The types an engine (eng_*.c) and its host share: parameter
 * descriptors, a voice, its per-block modulation, presets and the engine table entry.
 *
 * The engines are built as one translation unit with their host: the host includes, in this order,
 *   its dimension / API macros, the tables, this header, track_t / trk / P_*, dsp.c, then eng_*.c.
 * Every function is static: nothing here is linked separately.
 *
 * Contract v1: FELUCCA_ENGINE_API_VERSION is this header's contract revision, not a firmware release
 * or a binary ABI version. The host defines FELUCCA_ENGINE_API_REQUIRED as the revision it implements
 * before including this header. Only an exact match is supported; this is a compile-time handshake.
 *
 * What the host provides (before this header):
 *   FELUCCA_ENGINE_API_REQUIRED  1u
 *   NVOICE, NPART          macros: voices per part, parts (WHEEL keeps NPART x NVOICE voices of its own state)
 *                          both positive integer constants; capacity and memory limits belong to the host
 *   FS, CTL                macros: 44100 samples/second, 32 samples/control tick; other rates/block sizes
 *                          are unsupported (engine coefficients and ramps assume these values)
 * What the host provides (before the first eng_*.c):
 *   track_t                a typedef of `struct track`, with at least
 *                            int16_t p[];          the part's parameters, indexed by the P_* below
 *                            voice_t v[NVOICE];    its voices
 *   trk                    the array of the parts' track_t (WHEEL finds a part's number as t - trk)
 *   P_E0 .. P_E7           enum constants: the engine's eight parameters in t->p[] (EDIT 1 / EDIT 2)
 *   P_ATK, P_REL           the amplitude envelope's attack and release (an engine's HOME knobs may name them)
 *   P_FTYPE                ANALOG's filter type: 0 low-pass, 1 band-pass, 2 high-pass (a per-part parameter)
 *   the tables             SINE[1025], PITCH_OCT and pitch_inc(), SVF_G[128], TANH_Q15[257]
 *                          (tools/gen_engine_tables.py writes them as fe_tables.h)
 *
 * What the engines expect of the host's voices (the host's voice allocator and envelope):
 *   - note_on is called as a voice starts (or is retriggered); ph[] and s[] are zero before a voice first
 *     sounds (also when assigning a slot to a different engine). A retrigger of a sounding, non-sampled
 *     voice saves its old state, runs note_on with the new note / age / gate, then puts back ph[0..2] and each s[k] (k = 0, 1,
 *     4..7) whose bit k of engine_t.keep is set; s[2] and s[3] are as note_on left them.
 *     For sampled engines no phases are restored; keep still applies to s[]. Unset keep slots retain
 *     note_on's result. The five engines here are non-sampled. The host owns active / gate / stage.
 *   - render adds n samples (1 <= n <= CTL) to an int32_t accumulator: add, never clear or overwrite.
 *     The host supplies at least n writable elements, clears a fresh mix, and prevents signed overflow
 *     while accumulating voices. Output uses dsp.c's VOICE_FS scale (24000 for a full-scale Q15 oscillator
 *     at unit amplitude), with engine-dependent headroom; it is not clipped to int16_t. Keep output
 *     separate from track / voice / modulation state: state writeback can overwrite aliased samples.
 *     Normal blocks are CTL long. A short render still advances block-rate state once and uses the
 *     CTL-sample ramp; splitting a block into multiple renders is not equivalent to one render.
 *   - vmod_t carries the block's pitch, amplitude ramp (amp0 -> amp1) and cutoff / shape modulation;
 *     the units and neutral values are documented below. Parameters stay within descriptor ranges.
 *   - env is the ADSR value in Q24 (1 << 24 = full), stage 0 off, 1 attack, 2 decay / sustain, 3 release,
 *     4 given up (fading out); active is 1 while the voice sounds, gate 1 while its key is held.
 *     LOFI's stepped envelope (its amp hook) reads and ends them; WHEEL reads gate and the other voices
 *     of the part.
 *   - age is a counter the host advances per voice start (a noise seed); note and fine as the host set them.
 *   - amp, desc, block: optional hooks (0 = none), see engine_t.
 *   - callbacks for one part are serialized by its owner: note_on and parameter / gate changes finish
 *     before block; block runs once per control tick, even with no active voices, before amp / render.
 *     amp runs after the host's envelope tick; skip render if amp ended the voice. Different parts may
 *     run in parallel when each owns its trk element, voices and output buffer. WHEEL's private arrays
 *     are indexed by part and voice: t must be &trk[part], v must be &t->v[voice], and trk is a contiguous
 *     array of NPART track_t. No worker may mutate another part or clear WHEEL's arrays during playback.
 *     This contract does not permit concurrent callbacks within a part or define a thread runtime.
 * The layout of these types is Felucca's: a host keeps it as it is (the field order, the sizes). The file names
 * in the comments below (voice.c, engines.c, mod.c, fx.c, ...) are Felucca's, the host these engines came from. */
#ifndef FELUCCA_ENGINE_H
#define FELUCCA_ENGINE_H
#include <stdint.h>

#define FELUCCA_ENGINE_API_VERSION 1u
#ifndef FELUCCA_ENGINE_API_REQUIRED
#error "felucca_engine.h: host must define FELUCCA_ENGINE_API_REQUIRED before including the header"
#elif FELUCCA_ENGINE_API_REQUIRED != FELUCCA_ENGINE_API_VERSION
#error "felucca_engine.h: incompatible FELUCCA_ENGINE_API_REQUIRED (supported contract is 1u)"
#endif

#ifndef NVOICE
#error "felucca_engine.h: the host defines NVOICE (voices per part) first"
#elif NVOICE <= 0
#error "felucca_engine.h: NVOICE must be a positive integer constant"
#endif
#ifndef NPART
#error "felucca_engine.h: the host defines NPART (parts) first"
#elif NPART <= 0
#error "felucca_engine.h: NPART must be a positive integer constant"
#endif
#ifndef FS
#error "felucca_engine.h: the host defines FS (sample rate) first"
#elif FS != 44100
#error "felucca_engine.h: contract v1 requires FS == 44100"
#endif
#ifndef CTL
#error "felucca_engine.h: the host defines CTL (samples per control tick) first"
#elif CTL != 32
#error "felucca_engine.h: contract v1 requires CTL == 32"
#endif

#ifndef NELEM
#define NELEM(a) (sizeof(a) / sizeof((a)[0]))
#endif

/* ------------------------------------------------------- parameters --- */
enum {
    F_INT, F_PCT, F_BIPCT, F_TIME, F_LFOHZ, F_CUTOFF, F_DB, F_SEMI, F_ENUM, F_BPM, F_NOTE,
    F_ONOFF, F_OCT, F_STEPS
};

typedef struct {
    const char *label;
    uint8_t fmt;
    int16_t min, max, def;
    const char *const *names;   /* F_ENUM */
    const char *unit;           /* F_INT / F_ENUM optional unit */
} param_desc_t;

/* ------------------------------------------------- voices, engines --- */
typedef struct {
    uint8_t note, vel, gate, active;
    uint8_t stage;               /* env: 0 off, 1 attack, 2 decay/sustain, 3 release */
    uint8_t side;                /* SPREAD (fx.c mix_spread): 0 left of PAN, 1 right; set as the voice starts */
    int32_t env;                 /* Q24 */
    int32_t env_out;             /* last control-rate amplitude, Q15 */
    int32_t pitch16, pitch_cur;  /* 1/16 semitone, with glide */
    int32_t gstep;               /* glide TIME mode: 1/16 st per control tick, 0 = RATE mode */
    int32_t fine;                /* unison detune: phase increment * (1 + fine / 4096) */
    uint32_t ph[3];
    int32_t s[8];                /* engine state (filters, envs) */
    uint32_t age;
    uint8_t mvel;                /* mod.c: the note-on velocity (before UNISON scaling) and RAND of the note */
    int16_t mrnd;
} voice_t;

typedef struct {                 /* per-voice control-rate modulation, computed in voice.c */
    uint32_t inc;                /* phase increment/sample: frequency / FS * 2^32, including fine */
    int32_t pitch16;             /* MIDI note * 16 (1/16 semitone); table pitch range 0..2047 */
    int32_t amp0, amp1;          /* Q15 (32768 = 1): sample i uses amp0 + (amp1-amp0)*i/CTL,
                                 * delta rounded toward zero; amp1 is the next block boundary */
    int32_t cutoff;              /* signed offset to the engine's cutoff, 256 per table index; 0 neutral */
    int32_t shape;               /* absolute shape control, 0..127*256; 64*256 neutral */
    int32_t envq15;              /* Q15 envelope value (32768 = 1), for envelope modulation */
    int32_t fine;                /* the residual below 1/16 semitone in inc: unison detune, TUNE, bend (1/4096) */
} vmod_t;

typedef struct {
    const char *name;
    int8_t e[8];                 /* P_E0..P_E7 (signed: an interval below the note; every value fits) */
    uint8_t env[4];              /* ATK DEC SUS REL */
    int8_t fenv;                 /* ENV -> FILTER amount (-64..63) */
    uint8_t mono;                /* 1 = MONO (bass / lead), 0 = POLY */
    /* the rest of the patch; each value is stored + 1, 0 = the default */
    uint8_t fx[4];               /* DIST, CHORUS, DELAY, REVERB sends */
    uint8_t pat;                 /* suggested pattern (PATTERNS[pat - 1], 0 = none): only a hint. A sound load
                                  * never touches the steps; SEQ > PATTERNS loads a pattern (ui.c pat_load). The
                                  * arp is the track's too: a preset does not set it */
} preset_t;
#define FX(d, c, dl, r) .fx = {(d) + 1, (c) + 1, (dl) + 1, (r) + 1}
#define PAT(n) .pat = (n)

struct track;
typedef struct {                 /* an engine (engines.c ENGINES[]; the eng_*.c files) */
    const char *name;            /* "ANALOG" (PRESETS, the editor) */
    const char *page_title[2];   /* EDIT 1 and EDIT 2 */
    param_desc_t edit[8];        /* P_E0..P_E7 */
    const preset_t *presets;
    uint8_t npresets;
    uint8_t knob[4];             /* HOME: the four parameters on KNOB 1..4 */
    uint8_t poly;                /* voice cap for POLY and UNISON, 0 = NVOICE */
    uint8_t sampled;             /* 1 = plays recorded material (a position, not a phase): voice.c keeps
                                  * no phases over a retrigger, spreads none for UNISON, renders at SUS 0 */
    uint8_t keep;                /* bit k: s[k] is kept when a sounding voice is retriggered (filters) */
    uint8_t oneshot;             /* 1 = hits (DRUM): always POLY, no glide; amp ends its voices (eng_drum.c) */
    void (*note_on)(struct track *t, voice_t *v);
    void (*render)(struct track *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m);
    /* optional (0 = none): the voice amplitude instead of the ADSR curve, once per control tick;
     * gets the ADSR value (Q15, env_tick already ran: it still gates the voice), returns Q15 */
    int32_t (*amp)(struct track *t, voice_t *v, int32_t adsr);
    /* optional: a mode-dependent descriptor of EDIT k (the same range and default as edit[k],
     * another label / value names), 0 = edit[k] */
    const param_desc_t *(*desc)(const struct track *t, uint32_t k);
    /* optional: once per block and part, before its voices (also with no voice sounding) */
    void (*block)(struct track *t);
    /* optional: the note of key k (0 = the lowest) when the engine maps the keys itself, else -1
     * (then the scale keyboard of seq.c kb_map) */
    int32_t (*keys)(const struct track *t, uint32_t k);
    /* 1 = the engine's own envelopes are the voice's amplitude (FM6): voice.c renders it at 1.0 (no ADSR, no
     * velocity; LFO -> AMP, the matrix's AMP and the fades still apply; ENV is 0) and the voice ends when
     * done() says so (once per control tick, before the render), not at the end of the ADSR's release */
    uint8_t ownenv;
    int (*done)(struct track *t, voice_t *v);
    /* bytes of the part's engine memory (engines.c eng_mem, at most ENG_MEM_PART) the engine keeps its state
     * in, 0 = none: a part plays one engine at a time, so they share one region; on a switch to the engine
     * (after the old one's fade) the region is zeroed when another engine had it (eng_mem_claim) */
    uint16_t mem;
} engine_t;

#endif /* FELUCCA_ENGINE_H */
