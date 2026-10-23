/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#define FELUCCA_ENGINE_API_REQUIRED 1u
#define NPART 4
#define NVOICE 8
#include "fe_tables.h"
#include "felucca_engine.h"
enum {P_ATK,P_REL,P_FTYPE,P_E0,P_E1,P_E2,P_E3,P_E4,P_E5,P_E6,P_E7,P_COUNT};
typedef struct track { int16_t p[P_COUNT];voice_t v[NVOICE]; } track_t;
#include "dsp.c"
int32_t shared_probe(uint32_t phase,int32_t x) {tsvf_t c;int32_t a=0,b=0;tsvf_coef(&c,x,0);return tsvf_lp(&c,osc_sine(phase),&a,&b);}
#ifdef WAVE
#include "eng_wave_small.c"
const engine_t *wave_probe(void){return &ENG_WAVE_SMALL;}
#endif
