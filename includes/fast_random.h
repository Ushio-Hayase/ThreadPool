//
// Created by UshioHayase on 2025-12-24.
//

#ifndef THREAD_POOL_FAST_RANDOM_H
#define THREAD_POOL_FAST_RANDOM_H
#include <random>

struct XorShift32
{
    uint32_t state;
    uint32_t next()
    {
        uint32_t x = state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state = x;
        return x;
    }
};

inline uint32_t seedRandom()
{
    thread_local std::random_device rd;
    thread_local uint32_t seed = rd();
    if (seed == 0)
        seed = 1;
    return seed;
}

inline thread_local XorShift32 t_rng = {seedRandom()};

inline uint32_t getFastRandom() { return t_rng.next(); }

#endif // THREAD_POOL_FAST_RANDOM_H
