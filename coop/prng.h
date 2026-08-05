#pragma once

#include <cstdint>

namespace coop
{

// Prng — a small, fast, fully deterministic PRNG (xoshiro256** seeded via splitmix64). Given the
// same seed it produces the same sequence, forever, on any machine — the property deterministic
// simulation testing is built on. A cooperator owns one (Cooperator::Rng()); in a simulation you
// seed it (CooperatorConfiguration::rngSeed) so a failing run is reproducible from its seed, and
// every source of "randomness" in the system under test draws from it rather than from the OS.
//
// It is an ordinary value type with no hidden state, so it is equally useful as a plain fast RNG
// outside simulation.
//
struct Prng
{
    uint64_t s[4];

    static Prng Seeded(uint64_t seed)
    {
        // splitmix64 expands a single 64-bit seed into the 256-bit state, so even a poor seed (0, 1)
        // gives a well-distributed start.
        //
        Prng p;
        uint64_t x = seed;
        for (int i = 0; i < 4; i++)
        {
            uint64_t z = (x += 0x9e3779b97f4a7c15ULL);
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
            p.s[i] = z ^ (z >> 31);
        }
        return p;
    }

    uint64_t Next()
    {
        const uint64_t result = Rotl(s[1] * 5, 7) * 9;
        const uint64_t t = s[1] << 17;
        s[2] ^= s[0];
        s[3] ^= s[1];
        s[1] ^= s[2];
        s[0] ^= s[3];
        s[2] ^= t;
        s[3] = Rotl(s[3], 45);
        return result;
    }

    // Uniform in [0, bound). bound == 0 returns 0. Lemire's multiply-shift — unbiased without a
    // rejection loop for all practical bounds.
    //
    uint64_t Below(uint64_t bound)
    {
        if (bound == 0) return 0;
        return static_cast<uint64_t>((static_cast<__uint128_t>(Next()) * bound) >> 64);
    }

    // Uniform double in [0, 1).
    //
    double Unit() { return (Next() >> 11) * (1.0 / 9007199254740992.0); }

    // True with probability p.
    //
    bool Chance(double p) { return Unit() < p; }

private:
    static uint64_t Rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
};

} // end namespace coop
