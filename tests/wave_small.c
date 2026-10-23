/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define FELUCCA_ENGINE_API_REQUIRED 1u
#define NPART 4
#define NVOICE 8
#include "fe_tables.h"
#include "../src/felucca_engine.h"
enum { P_ATK, P_REL, P_FTYPE, P_E0, P_E1, P_E2, P_E3, P_E4, P_E5, P_E6, P_E7, P_COUNT };
typedef struct track {
    int16_t p[P_COUNT];
    voice_t v[NVOICE];
} track_t;
static track_t trk[NPART];
#include "../src/dsp.c"
#include "../src/eng_wave_small.c"
static void init(track_t *t, uint32_t preset)
{
    uint32_t j;
    memset(t, 0, sizeof *t);
    for (j = 0; j < 8; j++)
        t->p[P_E0 + j] = WS_PRESETS[preset].e[j];
    t->v[0].note = 60;
    t->v[0].pitch16 = 960;
    ws_note_on(t, &t->v[0]);
}
static vmod_t mod(uint32_t inc)
{
    vmod_t m;
    memset(&m, 0, sizeof m);
    m.inc = inc;
    m.shape = 64 * 256;
    m.amp0 = m.amp1 = 32768;
    return m;
}
static uint64_t hash(uint64_t h, int32_t *x, uint32_t n)
{
    uint32_t i, j;
    for (i = 0; i < n; i++)
        for (j = 0; j < 4; j++) {
            h ^= ((uint32_t)x[i] >> (j * 8)) & 255;
            h *= 1099511628211ull;
        }
    return h;
}
static void bounds(void)
{
    uint32_t bank, mult, scan, warp, n, i, f;
    const uint32_t scans[] = {0, 1, 18, 36, 54, 72, 90, 108, 126, 127};
    const uint32_t freq[] = {0,         1,          10000,      1000000,    33554432,  429496730,
                             858993459, 1717986918, 1932735283, 0x7fffffff, 0xffffffff};
    for (bank = 0; bank < 4; bank++)
        for (mult = 1; mult <= 4; mult++)
            for (scan = 0; scan < NELEM(scans); scan++)
                for (warp = 0; warp < 3; warp++) {
                    track_t *t = &trk[0];
                    init(t, 0);
                    t->p[P_E0] = (int16_t)bank;
                    t->p[P_E1] = (int16_t)scans[scan];
                    t->p[P_E2] = (int16_t)((int)warp * 63 - 63);
                    t->p[P_E3] = (int16_t)mult;
                    t->p[P_E5] = 127;
                    t->p[P_E6] = 127;
                    t->p[P_E7] = 127;
                    for (f = 0; f < NELEM(freq); f++)
                        for (n = 1; n <= 32; n++) {
                            int32_t a[34], b[34];
                            voice_t saved = t->v[0], after;
                            vmod_t m = mod(freq[f]);
                            m.cutoff = (n & 1) ? INT32_MIN : INT32_MAX;
                            m.shape = (n & 1) ? 0 : 32512;
                            m.amp0 = (n & 1) ? 0 : 32768;
                            m.amp1 = (n & 1) ? 32768 : 0;
                            for (i = 0; i < 34; i++) {
                                a[i] = 1234;
                                b[i] = 0;
                            }
                            ws_render(t, &t->v[0], a + 1, n, &m);
                            after = t->v[0];
                            t->v[0] = saved;
                            ws_render(t, &t->v[0], b + 1, n, &m);
                            assert(memcmp(&after, &t->v[0], sizeof after) == 0);
                            assert(a[0] == 1234 && a[n + 1] == 1234);
                            for (i = 1; i <= n; i++) {
                                assert(a[i] - b[i] == 1234);
                                assert(abs(b[i]) <= 24000);
                            }
                            assert(t->v[0].ph[0] == saved.ph[0] + m.inc * n);
                        }
                }
}
static void contracts(void)
{
    track_t *a = &trk[0], *b = &trk[1];
    uint32_t i;
    int32_t x[32], y[32];
    vmod_t m = mod(12345678);
    voice_t expected;
    init(a, 2);
    init(b, 2);
    for (i = 0; i < 100; i++) {
        memset(x, 0, sizeof x);
        memset(y, 0, sizeof y);
        a->p[P_E1] = b->p[P_E1] = (int16_t)(i % 128);
        a->p[P_E2] = b->p[P_E2] = (int16_t)((int)i % 127 - 63);
        ws_render(a, &a->v[0], x, 32, &m);
        ws_render(b, &b->v[0], y, 32, &m);
        assert(memcmp(x, y, sizeof x) == 0);
    }
    expected = a->v[0];
    ws_note_on(a, &a->v[0]);
    assert(a->v[0].ph[0] == 0 && a->v[0].s[0] == 0);
    /* The host restores phases and keep slots on a sounding retrigger. */
    for (i = 0; i < 3; i++)
        a->v[0].ph[i] = expected.ph[i];
    for (i = 0; i < 8; i++)
        if (i != 2 && i != 3 && (ENG_WAVE_SMALL.keep & (1u << i)))
            a->v[0].s[i] = expected.s[i];
    assert(a->v[0].s[0] == expected.s[0] && a->v[0].s[4] == expected.s[4]);
    assert(a->v[0].s[2] == a->p[P_E1] * 65536 && a->v[0].s[3] == a->p[P_E2] * 65536);
    for (i = 0; i < 65536; i += 17)
        assert(ws_bend(i * 65536u, 0) == i * 65536u);
    assert(ws_warp_k(63 * 65536) == 24576 && ws_warp_k(-63 * 65536) == -24576);
    assert(ws_config(12, 0xffffffff, 24576).gain == 0);
    assert(ws_config(0, 429496730, 24576).taper == 32768);
    assert(ws_config(0, 858993459, 24576).taper == 0);
    assert(sizeof FE_WAVE + sizeof FE_WAVE_SLEW <= 51200);
    assert(ENG_WAVE_SMALL.mem == 0 && ENG_WAVE_SMALL.npresets == 8);
}
static void controls(void)
{
    track_t *t = &trk[2], *u = &trk[3];
    voice_t start;
    int32_t x[32], y[32];
    uint32_t i;
    vmod_t m = mod(12345678);
    init(t, 0);
    t->p[P_E1] = 127;
    t->p[P_E2] = -63;
    memset(x, 0, sizeof x);
    ws_render(t, &t->v[0], x, 32, &m);
    assert(t->v[0].s[5] == 127 * 65536 && t->v[0].s[6] == -63 * 65536);
    m.shape = 0;
    memset(x, 0, sizeof x);
    ws_render(t, &t->v[0], x, 32, &m);
    assert(t->v[0].s[5] == 63 * 65536); /* SHP offset, not replacement */
    init(t, 0);
    t->p[P_E7] = 127;
    t->p[P_E1] = 127;
    for (i = 0; i < 256; i++) {
        int32_t before = t->v[0].s[5];
        memset(x, 0, sizeof x);
        ws_render(t, &t->v[0], x, 32, &m);
        assert(t->v[0].s[5] >= before && t->v[0].s[5] <= 63 * 65536);
    }
    assert(t->v[0].s[5] > 0 && t->v[0].s[5] < 63 * 65536);
    init(t, 0);
    init(u, 0);
    t->v[0].ph[0] = u->v[0].ph[0] = 0x23456789u;
    start = t->v[0];
    t->p[P_E0] = 3;
    t->p[P_E3] = 4;
    memset(x, 0, sizeof x);
    memset(y, 0, sizeof y);
    m.shape = 64 * 256;
    ws_render(t, &t->v[0], x, 32, &m);
    ws_render(u, &u->v[0], y, 32, &m);
    assert(x[0] == y[0]);
    assert(t->v[0].s[4] == 15);
    assert(t->v[0].ph[0] == start.ph[0] + m.inc * 32 && t->v[0].ph[1] == (m.inc >> 1) * 32);
    /* Main muted for both multipliers; audible SUB remains root-relative. */
    init(t, 0);
    init(u, 0);
    t->p[P_E3] = 3;
    u->p[P_E3] = 4;
    t->p[P_E6] = u->p[P_E6] = 127;
    ws_note_on(t, &t->v[0]);
    ws_note_on(u, &u->v[0]);
    m = mod(858993460u);
    memset(x, 0, sizeof x);
    memset(y, 0, sizeof y);
    ws_render(t, &t->v[0], x, 32, &m);
    ws_render(u, &u->v[0], y, 32, &m);
    assert(memcmp(x, y, sizeof x) == 0);
    assert(x[8] != 0);
    /* Actual SCAN/WARP one-pole step reaches 1-e^-1 after tau, allowing the
     * mandatory 32-sample input ramp and integer coefficient precision. */
    for (i = 0; i < 2; i++) {
        uint32_t samples = 0, tau = i ? 200u : 1u, count = (FS * tau + 500u) / 1000u + 16u;
        int32_t ratio;
        init(t, 0);
        t->p[P_E7] = (int16_t)(i ? 127 : 1);
        t->p[P_E1] = 127;
        t->p[P_E2] = 63;
        m = mod(12345678);
        while (samples < count) {
            uint32_t n = count - samples > 32 ? 32 : count - samples;
            memset(x, 0, sizeof x);
            ws_render(t, &t->v[0], x, n, &m);
            samples += n;
        }
        ratio = t->v[0].s[5] / 127;
        assert(ratio > 40000 && ratio < 43000);
        ratio = t->v[0].s[6] / 63;
        assert(ratio > 40000 && ratio < 43000);
    }
    /* One-pole quantization must not leave a permanent nonzero warp at zero. */
    {
        int32_t below = -100, above = 100;
        uint32_t j;
        for (j = 0; j < 100; j++) {
            below = ws_follow(below, 0, 1);
            above = ws_follow(above, 0, 1);
        }
        assert(!below && !above);
        init(t, 0);
        t->v[0].s[5] = 12345;
        t->v[0].s[6] = -12345;
        m = mod(12345678);
        memset(x, 0, sizeof x);
        ws_render(t, &t->v[0], x, 32, &m);
        assert(t->v[0].s[5] == 0 && t->v[0].s[6] == 0); /* exact 32-sample ramp endpoint */
    }
    /* Bend phase is monotone across the closed cycle at both depth extremes. */
    for (i = 0; i < 2; i++) {
        uint32_t j, last = 0;
        int32_t k = i ? 24576 : -24576;
        for (j = 0; j < 65536; j++) {
            uint32_t bent = ws_bend(j * 65536u, k);
            assert(bent >= last);
            last = bent;
        }
    }
}

static void renders(int audition)
{
    uint32_t p, b, i;
    int32_t out[32];
    FILE *wav = 0;
    if (audition)
        wav = fopen("build/wave-audition-s32le.raw", "wb");
    for (p = 0; p < 8; p++) {
        uint64_t h = 14695981039346656037ull;
        track_t *t = &trk[p % 4];
        init(t, p);
        for (b = 0; b < 1024; b++) {
            vmod_t m = mod(pitch_inc((uint32_t)(36 + (b / 256) * 24) * 16));
            m.shape = (int32_t)(b % 128) * 256;
            m.cutoff = ((int32_t)(b % 128) - 64) * 128;
            m.amp0 = m.amp1 = b > 800 ? (int32_t)((1024 - b) * 32768 / 224) : 32768;
            if (b == 300) {
                t->p[P_E0] = (int16_t)((p + 1) % 4);
                t->p[P_E3] = 4;
            }
            if (b == 600) {
                t->p[P_E0] = (int16_t)(p % 4);
                t->p[P_E3] = 1;
                t->p[P_E2] = -63;
            }
            memset(out, 0, sizeof out);
            ws_render(t, &t->v[0], out, 32, &m);
            h = hash(h, out, 32);
            if (wav)
                for (i = 0; i < 32; i++)
                    fwrite(&out[i], sizeof out[i], 1, wav);
        }
        printf("wave/%u %016llx %s\n", p, (unsigned long long)h, WS_PRESETS[p].name);
    }
    if (wav)
        fclose(wav);
}
/* Parse once before initialization/output; strtoul alone accepts negative values. */
static int block_count(const char *arg, uint32_t *count)
{
    const char *digits = arg + (arg[0] == '+');
    char *end;
    unsigned long value;
    if (*digits < '0' || *digits > '9')
        return 0;
    errno = 0;
    value = strtoul(arg, &end, 10);
    if (errno || *end || value > UINT32_MAX)
        return 0;
    *count = (uint32_t)value;
    return 1;
}
int main(int argc, char **argv)
{
    if (argc == 7 || argc == 8) {
        uint32_t b, blocks;
        track_t *t = &trk[0];
        vmod_t m;
        int32_t out[32];
        if (!block_count(argv[6], &blocks)) {
            fprintf(stderr, "invalid block count: expected 0..4294967295\n");
            return 2;
        }
        init(t, 0);
        t->p[P_E0] = (int16_t)atoi(argv[1]);
        t->p[P_E1] = (int16_t)atoi(argv[2]);
        t->p[P_E2] = (int16_t)atoi(argv[3]);
        t->p[P_E3] = (int16_t)atoi(argv[4]);
        ws_note_on(t, &t->v[0]);
        m = mod((uint32_t)strtoul(argv[5], 0, 10));
        for (b = 0; b < blocks; b++) {
            memset(out, 0, sizeof out);
            if (argc == 8) {
                uint32_t j;
                int32_t k = ws_warp_k(t->p[P_E2] * 65536);
                ws_config_t c =
                    ws_config((uint32_t)(t->p[P_E0] + 4 * (t->p[P_E3] - 1)), m.inc, k < 0 ? -k : k);
                for (j = 0; j < 32; j++) {
                    out[j] = (ws_frame(&c, ws_bend(t->v[0].ph[0] * c.mult, (k * c.taper) >> 15),
                                       t->p[P_E1] * 65536) *
                              c.gain) >>
                             15;
                    t->v[0].ph[0] += m.inc;
                }
            } else
                ws_render(t, &t->v[0], out, 32, &m);
            if (fwrite(out, sizeof *out, 32, stdout) != 32) {
                fprintf(stderr, "render output write failed\n");
                return 3;
            }
        }
        if (fflush(stdout)) {
            fprintf(stderr, "render output flush failed\n");
            return 3;
        }
        return 0;
    }
    bounds();
    contracts();
    controls();
    renders(argc > 1);
    return fflush(stdout) ? 3 : 0;
}
