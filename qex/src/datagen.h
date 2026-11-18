// datagen.h - synthetic log generation and columnar table storage.
#pragma once

#include "common.h"

namespace qex {

struct GenSpec {
    size_t rows = 10'000'000;
    uint64_t seed = 20261119;
    double nullFrac = 0.015;
};

// Deterministic, allocation-light PRNG (splitmix64).
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t next() {
        s += 0x9e3779b97f4a7c15ull;
        uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    uint64_t below(uint64_t n) { return n ? next() % n : 0; }
    double uniform() { return (next() >> 11) * (1.0 / 9007199254740992.0); }
};

// Build the synthetic "logs" table entirely in memory.
std::unique_ptr<Table> generate_table(const GenSpec &spec);

// Binary columnar format: header + per-column payloads.
void save_table(const Table &t, const std::string &path);
std::unique_ptr<Table> load_table(const std::string &path);

} // namespace qex
