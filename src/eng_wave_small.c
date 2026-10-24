/* SPDX-License-Identifier: MIT */
/* Original WAVE S. See docs/wave-small.md for ownership, units and bounds.
 * Include host-generated fe_wave_tables.h only when selecting this engine. */
#include "fe_wave_tables.h"
#if FE_WAVE_FS != FS
#error "WAVE S assets must be generated for the host FS"
#endif

static const char *const WS_BANKS[] = {"HARM", "HOLLOW", "FORMANT", "GLASS"};

typedef struct {
    uint32_t level, blend;
    int32_t gain, taper;
    uint32_t mult, bank;
} ws_config_t;

static int32_t ws_lerp(int32_t a, int32_t b, uint32_t q15)
{
    /* Values within +-29490: difference * Q15 fits signed 32 bits. */
    return a + (int32_t)(((int64_t)(b - a) * q15) >> 15);
}

static int32_t ws_lookup(uint32_t bank, uint32_t frame, uint32_t level, uint32_t ph)
{
    const int16_t *w = FE_WAVE[bank][frame][level];
    uint32_t i = ph >> 25;
    return ws_lerp(w[i], w[i + 1], (ph >> 10) & 32767u);
}

static int32_t ws_frame(const ws_config_t *c, uint32_t ph, int32_t scan)
{
    /* 0..127 Q16 -> 0..7 Q16. Reciprocal ceil(2^32/127), exact endpoints. */
    uint32_t pos = (uint32_t)(((uint64_t)(uint32_t)scan * 7u * 33818641u) >> 32);
    uint32_t frame = pos >> 16, next = frame < 7u ? frame + 1u : 7u;
    int32_t a = ws_lerp(ws_lookup(c->bank, frame, c->level, ph),
                        ws_lookup(c->bank, next, c->level, ph), (pos & 65535u) >> 1);
    if (c->blend) {
        int32_t b = ws_lerp(ws_lookup(c->bank, frame, c->level + 1u, ph),
                            ws_lookup(c->bank, next, c->level + 1u, ph), (pos & 65535u) >> 1);
        a = ws_lerp(a, b, c->blend);
    }
    return a;
}

static int32_t ws_warp_k(int32_t warp)
{
    /* Q16 control +-63 -> k Q15 +-24576, < 2^46 product. */
    return clamp((int32_t)(((int64_t)warp * 12484) >> 21), -24576, 24576);
}

static uint32_t ws_bend(uint32_t ph, int32_t k)
{
    uint32_t x;
    int32_t curve, delta;
    if (!k)
        return ph; /* exact unwarped phase, including low 16 bits */
    x = ph >> 16;
    curve = (int32_t)((x * (65536u - x)) >> 16); /* 0..16384 Q16; product <=2^30 */
    delta = (curve * k) >> 15;                   /* abs(product) <=402,653,184 */
    return ph + (uint32_t)(delta * 65536);       /* unsigned modulo, derivative .25..1.75 */
}

static ws_config_t ws_config(uint32_t packed, uint32_t root, int32_t maxwarp)
{
    static const uint32_t ends[5] = {30678337u, 62346299u, 128849018u, 276105040u, 644245094u};
    ws_config_t c;
    uint64_t freq, safe;
    uint32_t j;
    c.bank = packed & 3u;
    c.mult = (packed >> 2) + 1u;
    c.level = 5;
    c.blend = 0;
    freq = (uint64_t)root * c.mult; /* BEFORE modulo multiplication: max <2^34 */
    c.gain = freq <= 1717986918ull   ? 32768
             : freq >= 1932735283ull ? 0
                                     : (int32_t)(((uint32_t)(1932735283ull - freq) * 5u) >> 15);
    c.taper = freq <= 429496730ull   ? 32768
              : freq >= 858993459ull ? 0
                                     : (int32_t)(((uint32_t)(858993459ull - freq) * 5u) >> 16);
    /* Conservative maximum bend slope for the whole call, before table choice. */
    safe = (freq * (32768u + (uint32_t)(((int64_t)maxwarp * c.taper) >> 15))) >> 15;
    for (j = 0; j < 5u; j++) {
        uint32_t end = ends[j], start = end - end / 4u;
        if (safe < end) {
            c.level = j;
            if (safe > start)
            /* safe < end < 2^30: numerator bounded by 2^30. Q15
             * ratio via 15 fixed restoring steps, no 64-bit divide helper. */
            {
                uint32_t rem = (uint32_t)safe - start, den = end - start, bit;
                for (bit = 0; bit < 15u; bit++) {
                    rem *= 2u;
                    c.blend *= 2u;
                    if (rem >= den) {
                        rem -= den;
                        c.blend++;
                    }
                }
            }
            break;
        }
    }
    return c;
}

static void ws_note_on(track_t *t, voice_t *v)
{
    uint32_t i;
    for (i = 0; i < 3u; i++)
        v->ph[i] = 0;
    for (i = 0; i < 8u; i++)
        v->s[i] = 0;
    v->s[2] = v->s[5] = t->p[P_E1] * 65536;
    v->s[3] = v->s[6] = t->p[P_E2] * 65536;
    v->s[4] = t->p[P_E0] + 4 * (t->p[P_E3] - 1);
}

/* Round a Q16 control's Q24 one-pole step symmetrically. A one-LSB minimum
 * prevents a zero-WARP target retaining a permanent fractional bend. */
static int32_t ws_follow(int32_t current, int32_t target, uint32_t alpha)
{
    int32_t delta = target - current, distance = delta < 0 ? -delta : delta;
    int32_t step = (int32_t)(((int64_t)distance * alpha + 8388608) >> 24);
    if (!step && distance)
        step = 1;
    return current + (delta < 0 ? -step : step);
}

static void ws_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    int32_t targetscan = clamp(t->p[P_E1] * 256 + m->shape - 64 * 256, 0, 127 * 256) * 256;
    int32_t targetwarp = t->p[P_E2] * 65536;
    int32_t scan = v->s[5], warp = v->s[6], rs = v->s[2], rw = v->s[3];
    int32_t ds = (targetscan - rs) / 32, dw = (targetwarp - rw) / 32;
    int32_t maxwarp = ws_warp_k(warp), k2 = ws_warp_k(targetwarp);
    int32_t prior = ws_warp_k(rw);
    int32_t ic1 = v->s[0], ic2 = v->s[1], sub = t->p[P_E6] * 32768 / (254 + t->p[P_E6]);
    uint32_t alpha = FE_WAVE_SLEW[t->p[P_E7]], ph = v->ph[0], sp = v->ph[1], i;
    uint32_t packed = (uint32_t)(t->p[P_E0] + 4 * (t->p[P_E3] - 1));
    int changing = packed != (uint32_t)v->s[4];
    ws_config_t old, c, sc;
    tsvf_t filter;
    int64_t cut = (int64_t)t->p[P_E4] * 256 + m->cutoff;
    if (!alpha) {
        /* With no extra lag the preserved sounding state is the ramp origin,
         * including a retrigger (API v1 resets s[2]/s[3]). */
        rs = scan;
        rw = warp;
        ds = (targetscan - rs) / 32;
        dw = (targetwarp - rw) / 32;
    }
    if (maxwarp < 0)
        maxwarp = -maxwarp;
    if (k2 < 0)
        k2 = -k2;
    if (k2 > maxwarp)
        maxwarp = k2;
    if (prior < 0)
        prior = -prior;
    if (prior > maxwarp)
        maxwarp = prior;
    old = ws_config((uint32_t)v->s[4], m->inc, maxwarp);
    c = ws_config(packed, m->inc, maxwarp);
    sc = ws_config(0, m->inc >> 1, 0);
    tsvf_coef(&filter, (int32_t)(cut < 0 ? 0 : cut > 32512 ? 32512 : cut), t->p[P_E5]);
    for (i = 0; i < n; i++) {
        int32_t k, y, main, low;
        rs += ds;
        rw += dw;
        if (i == 31u) {
            rs = targetscan;
            rw = targetwarp;
        } /* exact endpoint despite integer step truncation */
        if (alpha) {
            scan = ws_follow(scan, rs, alpha);
            warp = ws_follow(warp, rw, alpha);
        } else {
            scan = rs;
            warp = rw;
        }
        k = ws_warp_k(warp);
        main = ws_frame(&c, ws_bend(ph * c.mult, (k * c.taper) >> 15), scan);
        main = (main * c.gain) >> 15;
        if (changing) {
            int32_t a = ws_frame(&old, ws_bend(ph * old.mult, (k * old.taper) >> 15), scan);
            a = (a * old.gain) >> 15;
            main = ws_lerp(a, main, i * 1024u);
        }
        low = (osc_sine(sp) * sc.gain) >> 15;
        y = (int32_t)(((int64_t)main * (32768 - sub) + (int64_t)low * sub) >> 15);
        y = tsvf_lp(&filter, y, &ic1, &ic2);
        y = clamp(y, -32768, 32767); /* shared resonant filter headroom -> voice_amp safe domain */
        out[i] += voice_amp(y, m, i);
        ph += m->inc;
        sp += m->inc >> 1;
    }
    v->ph[0] = ph;
    v->ph[1] = sp;
    v->s[0] = ic1;
    v->s[1] = ic2;
    v->s[5] = scan;
    v->s[6] = warp;
    v->s[4] = (int32_t)packed;
    v->s[2] = targetscan;
    v->s[3] = targetwarp;
}

static const preset_t WS_PRESETS[] = {
    {"PURE", {0, 0, 0, 1, 127, 0, 0, 0}, {0, 40, 100, 40}, 0, 0, FX(0, 0, 0, 0), PAT(0)},
    {"BRIGHT", {0, 110, 0, 1, 115, 10, 0, 0}, {0, 40, 100, 40}, 0, 0, FX(0, 0, 0, 0), PAT(0)},
    {"DRIFT", {0, 64, 20, 1, 100, 15, 12, 100}, {40, 70, 110, 70}, 0, 0, FX(0, 0, 0, 0), PAT(0)},
    {"HOLLOW", {1, 80, -24, 1, 110, 20, 20, 0}, {0, 40, 100, 40}, 0, 0, FX(0, 0, 0, 0), PAT(0)},
    {"NOTCH", {1, 120, 40, 2, 100, 32, 0, 40}, {0, 40, 100, 40}, 0, 0, FX(0, 0, 0, 0), PAT(0)},
    {"VOWEL", {2, 64, 15, 1, 110, 10, 0, 70}, {20, 60, 100, 50}, 0, 0, FX(0, 0, 0, 0), PAT(0)},
    {"GLASS", {3, 100, 0, 2, 120, 0, 0, 0}, {0, 70, 60, 80}, 0, 0, FX(0, 0, 0, 0), PAT(0)},
    {"ROOT", {3, 40, -35, 4, 100, 20, 127, 30}, {0, 40, 100, 60}, 0, 0, FX(0, 0, 0, 0), PAT(0)},
};
static const engine_t ENG_WAVE_SMALL = {
    .name = "WAVE S",
    .page_title = {"WAVES", "COLOR"},
    .edit = {{"BANK", F_ENUM, 0, 3, 0, WS_BANKS, 0},
             {"SCAN", F_PCT, 0, 127, 0, 0, 0},
             {"WARP", F_BIPCT, -63, 63, 0, 0, 0},
             {"MULT", F_INT, 1, 4, 1, 0, 0},
             {"CUT", F_CUTOFF, 0, 127, 127, 0, 0},
             {"RES", F_PCT, 0, 127, 0, 0, 0},
             {"SUB", F_PCT, 0, 127, 0, 0, 0},
             {"SLEW", F_PCT, 0, 127, 0, 0, 0}},
    .presets = WS_PRESETS,
    .npresets = NELEM(WS_PRESETS),
    .knob = {P_E1, P_E2, P_E4, P_REL},
    .keep = 0x73,
    .note_on = ws_note_on,
    .render = ws_render,
};
