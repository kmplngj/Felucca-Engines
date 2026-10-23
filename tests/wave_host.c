/* SPDX-License-Identifier: MIT */
/* Reuse the existing v1 host's allocator/retrigger/envelope/part fixtures. */
#define CONTRACT_NO_MAIN
#include "contract_runtime.c"
#include "../src/eng_wave_small.c"

int main(void)
{
    uint32_t k;
    int audible = 0;
    for (k = 0; k < 4u; k++)
        CHECK(additive_and_bounds(&ENG_WAVE_SMALL, (int32_t)k, &audible) == 0);
    CHECK(audible);
    CHECK(retrigger(&ENG_WAVE_SMALL, ENG_WAVE_SMALL.keep) == 0);
    CHECK(retrigger(&ENG_WAVE_SMALL, 0xff) == 0);
    CHECK(part_ownership(&ENG_WAVE_SMALL) == 0);
    for (k = 0; k < ENG_WAVE_SMALL.npresets; k++) {
        result_t r = render(&ENG_WAVE_SMALL, &WS_PRESETS[k], k % NPART, 0);
        printf("wave-host/%u %016llx %d %s\n", k, (unsigned long long)r.h, r.peak,
               WS_PRESETS[k].name);
    }
    return 0;
}
