/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* All host requirements come from compiler -D flags: the negative fixtures omit or change one. */
#ifdef CONTRACT_FULL_HOST
#include "fe_tables.h"
#endif
#include "../src/felucca_engine.h"

#if FELUCCA_ENGINE_API_VERSION != 1u
#error "contract fixture expects revision 1u"
#endif

enum { P_ATK, P_REL, P_FTYPE, P_E0, P_E1, P_E2, P_E3, P_E4, P_E5, P_E6, P_E7, P_COUNT };
typedef struct track {
    int16_t p[P_COUNT];
    voice_t v[NVOICE];
} track_t;

/* Positive dimensions must permit the required contiguous part/voice arrays in C99. */
static track_t trk[NPART];
typedef char phase_slots[sizeof trk[0].v[0].ph / sizeof(uint32_t) == 3 ? 1 : -1];
typedef char state_slots[sizeof trk[0].v[0].s / sizeof(int32_t) == 8 ? 1 : -1];

#ifdef CONTRACT_FULL_HOST
/* Compile all engines at the smallest positive dimensions as well as the normal host dimensions. */
#include "../src/dsp.c"
#include "../src/eng_analog.c"
#include "../src/eng_lofi.c"
#include "../src/eng_trio.c"
#include "../src/eng_wheel.c"
#include "../src/eng_noise.c"
#ifdef CONTRACT_WAVE
#include "../src/eng_wave_small.c"
#endif
#endif

int main(void)
{
#ifdef CONTRACT_FULL_HOST
    const engine_t *engines[] = {&ENG_ANALOG, &ENG_LOFI, &ENG_TRIO, &ENG_WHEEL, &ENG_NOISE};
    uint32_t i;
#ifdef CONTRACT_WAVE
    if (!ENG_WAVE_SMALL.note_on || !ENG_WAVE_SMALL.render) return 1;
#endif
    for (i = 0; i < NELEM(engines); i++)
        if (!engines[i]->note_on || !engines[i]->render)
            return 1;
#endif
    return trk[0].v[0].active;
}
