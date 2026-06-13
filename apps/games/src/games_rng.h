#pragma once

/* games_rng — xorshift32 PRNG (avoids rand/esp_random symbol dependencies). */

#include <stdint.h>

void     games_rng_seed(uint32_t seed);
uint32_t games_rng(uint32_t bound);   /* uniform [0, bound); bound 0 = raw u32 */
