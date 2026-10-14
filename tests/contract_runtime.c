/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Exercise the same complete host used for the PCM goldens, without changing its phrase. */
#define main golden_main
#include "fe_host.c"
#undef main

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "contract check failed at line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

typedef struct {
    track_t tracks[NPART];
    drw_trk_t wheel_parts[NPART];
    drw_vc_t wheel_voices[NPART][NVOICE];
    uint32_t age;
} snapshot_t;

static void save(snapshot_t *s)
{
    memcpy(s->tracks, trk, sizeof trk);
    memcpy(s->wheel_parts, drw_t, sizeof drw_t);
    memcpy(s->wheel_voices, drw_v, sizeof drw_v);
    s->age = vage;
}

static void restore(const snapshot_t *s)
{
    memcpy(trk, s->tracks, sizeof trk);
    memcpy(drw_t, s->wheel_parts, sizeof drw_t);
    memcpy(drw_v, s->wheel_voices, sizeof drw_v);
    vage = s->age;
}

static void reset(void)
{
    memset(trk, 0, sizeof trk);
    memset(drw_t, 0, sizeof drw_t);
    memset(drw_v, 0, sizeof drw_v);
    vage = 0;
}

static void setup_part(const engine_t *e, uint32_t part, int32_t mode)
{
    track_t *t = &trk[part];
    uint32_t k;
    for (k = 0; k < 8; k++)
        t->p[P_E0 + k] = e->edit[k].def;
    t->p[P_E0] = (int16_t)mode;
    t->p[P_REL] = 40;
    voice_on(t, e, &t->v[0], 48 + part * 7, 0);
    voice_on(t, e, &t->v[1], 67 + part * 7, 0);
}

static vmod_t modulation(const voice_t *v)
{
    vmod_t m;
    memset(&m, 0, sizeof m);
    m.pitch16 = v->pitch16;
    m.inc = pitch_inc((uint32_t)m.pitch16);
    m.amp0 = 8192;
    m.amp1 = 24576;
    m.envq15 = 24576;
    m.shape = 64 * 256;
    return m;
}

/* For identical engine state, a nonzero mix must change only the sum, not state or bounds. */
static int additive_and_bounds(const engine_t *e, int32_t mode, int *audible)
{
    const uint32_t lengths[] = {1, 7, CTL - 1, CTL};
    snapshot_t before, zero_state, biased_state;
    uint32_t l, i;
    for (l = 0; l < NELEM(lengths); l++) {
        int32_t zero[CTL + 2], biased[CTL + 2];
        uint32_t n = lengths[l];
        vmod_t m;
        reset();
        setup_part(e, 0, mode);
        if (e->block)
            e->block(&trk[0]);
        m = modulation(&trk[0].v[0]);
        save(&before);
        for (i = 0; i < CTL + 2; i++) {
            zero[i] = i > 0 && i <= n ? 0 : 0x13579;
            biased[i] = i > 0 && i <= n ? (int32_t)i * 37 - 500 : 0x13579;
        }
        e->render(&trk[0], &trk[0].v[0], zero + 1, n, &m);
        save(&zero_state);
        restore(&before);
        e->render(&trk[0], &trk[0].v[0], biased + 1, n, &m);
        save(&biased_state);
        CHECK(memcmp(&zero_state, &biased_state, sizeof zero_state) == 0);
        for (i = 0; i < CTL + 2; i++) {
            if (i > 0 && i <= n) {
                CHECK(biased[i] == zero[i] + (int32_t)i * 37 - 500);
                *audible |= zero[i] != 0;
            } else {
                CHECK(zero[i] == 0x13579 && biased[i] == 0x13579);
            }
        }
    }
    return 0;
}

/* Compare retrigger with a raw note_on: preserved slots differ only where the contract says. */
static int retrigger(const engine_t *source, uint8_t keep)
{
    engine_t e = *source;
    snapshot_t before;
    voice_t old, fresh;
    voice_t *v = &trk[0].v[0];
    uint32_t k;
    e.keep = keep;
    reset();
    setup_part(&e, 0, e.edit[0].def);
    v->stage = 2;
    for (k = 0; k < 3; k++)
        v->ph[k] = 0x12345678u * (k + 1);
    for (k = 0; k < 8; k++)
        v->s[k] = 1000 + (int32_t)k;
    old = *v;
    save(&before);
    v->note = 72;
    v->fine = 11;
    v->age = vage + 1;
    e.note_on(&trk[0], v);
    fresh = *v;
    restore(&before);
    voice_on(&trk[0], &e, v, 72, 11);
    CHECK(v->note == 72 && v->fine == 11 && v->gate == 1 && v->active == 1);
    CHECK(v->age == before.age + 1 && v->stage == 1);
    for (k = 0; k < 3; k++)
        CHECK(v->ph[k] == old.ph[k]);
    for (k = 0; k < 8; k++) {
        int preserved = k != 2 && k != 3 && ((keep >> k) & 1u);
        CHECK(v->s[k] == (preserved ? old.s[k] : fresh.s[k]));
    }
    return 0;
}

static void part_tick(const engine_t *e, uint32_t part, uint32_t tick, int32_t *out)
{
    track_t *t = &trk[part];
    uint32_t k;
    if (tick == 3) {
        /* Assign deterministic event ages so the test's serial reference has the same seeds. */
        vage = 100 + tick * NPART + part;
        voice_on(t, e, &t->v[0], 55 + part * 7, 0);
    }
    if (tick == 5)
        for (k = 0; k < 2; k++) {
            t->v[k].gate = 0;
            t->v[k].stage = 3;
        }
    if (e->block)
        e->block(t);
    memset(out, 0, CTL * sizeof *out);
    for (k = 0; k < 2; k++) {
        voice_t *v = &t->v[k];
        vmod_t m = modulation(v);
        int32_t amp = env_tick(v);
        if (e->amp)
            amp = e->amp(t, v, amp);
        m.amp0 = m.amp1 = amp;
        if (v->active)
            e->render(t, v, out, CTL, &m);
    }
}

/* Interleave two parts and compare both PCM and private state to isolated serial runs.
 * This detects shared mutable part state; it does not claim to test a threading runtime. */
static int part_ownership(const engine_t *e)
{
    enum { TICKS = 8 };
    int32_t expected[2][TICKS][CTL], actual[CTL];
    snapshot_t initial, expected_state[2], actual_state;
    uint32_t p, b;
    reset();
    setup_part(e, 0, e->edit[0].def);
    setup_part(e, 1, e->edit[0].max);
    save(&initial);
    for (p = 0; p < 2; p++) {
        restore(&initial);
        for (b = 0; b < TICKS; b++)
            part_tick(e, p, b, expected[p][b]);
        save(&expected_state[p]);
    }
    restore(&initial);
    for (b = 0; b < TICKS; b++)
        for (p = 0; p < 2; p++) {
            part_tick(e, p, b, actual);
            CHECK(memcmp(actual, expected[p][b], sizeof actual) == 0);
        }
    save(&actual_state);
    for (p = 0; p < 2; p++) {
        CHECK(memcmp(&actual_state.tracks[p], &expected_state[p].tracks[p], sizeof(track_t)) == 0);
        CHECK(memcmp(&actual_state.wheel_parts[p], &expected_state[p].wheel_parts[p], sizeof(drw_trk_t)) == 0);
        CHECK(memcmp(actual_state.wheel_voices[p], expected_state[p].wheel_voices[p], sizeof drw_v[0]) == 0);
    }
    return 0;
}

int main(void)
{
    uint32_t k;
    for (k = 0; k < NELEM(ENGS); k++) {
        const engine_t *e = ENGS[k].e;
        int32_t mode;
        int audible = 0;
        for (mode = e->edit[0].min; mode <= e->edit[0].max; mode++)
            CHECK(additive_and_bounds(e, mode, &audible) == 0);
        CHECK(audible); /* a silent mode (e.g. DUST at density 0) is valid, but test real addition too */
        CHECK(retrigger(e, e->keep) == 0);
        CHECK(retrigger(e, 0xff) == 0); /* even bits 2/3 cannot keep note-local seed/sweep state */
        CHECK(part_ownership(e) == 0);
    }
    puts("ok: contract runtime (additive output, bounds, retrigger, part ownership)");
    return 0;
}
