#include "games_rng.h"

static uint32_t s_state = 0x12345678u;

void games_rng_seed(uint32_t seed)
{
    s_state = seed != 0u ? seed : 0x12345678u;
}

uint32_t games_rng(uint32_t bound)
{
    uint32_t x = s_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_state = x;
    return bound != 0u ? x % bound : x;
}
