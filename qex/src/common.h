// common.h - shared engine types, columnar table, bitmaps, utilities.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <chrono>
#include <stdexcept>

namespace qex {

using i64 = int64_t;
using u64 = uint64_t;
using u32 = uint32_t;

enum class DType { I64 = 0, F64 = 1, STR = 2 };

inline const char *dtype_name(DType t) {
    switch (t) {
        case DType::I64: return "i64";
        case DType::F64: return "f64";
        case DType::STR: return "str";
    }
    return "?";
}

// ---------------------------------------------------------------- bitmap
inline size_t nwords(size_t nrows) { return (nrows + 63) / 64; }

inline bool bget(const u64 *bm, size_t i) {
    return (bm[i >> 6] >> (i & 63)) & 1ull;
}
inline void bset(u64 *bm, size_t i) { bm[i >> 6] |= 1ull << (i & 63); }
inline void bclear(u64 *bm, size_t n) {
    std::memset(bm, 0, nwords(n) * sizeof(u64));
}
inline size_t bcount(const u64 *bm, size_t nrows) {
    size_t c = 0;
    size_t w = nwords(nrows);
    for (size_t i = 0; i < w; ++i) c += (size_t)__builtin_popcountll(bm[i]);
    return c;
}

// ------------------------------------------------------------- columns
struct Column {
    std::string name;
    DType type = DType::I64;

    // numeric data
    std::vector<i64> i64v;
    std::vector<double> f64v;

    // dictionary-encoded strings
    std::vector<u32> sid;                       // row -> dict id, UINT32_MAX=null
    std::vector<std::string> dict;              // id -> string
    std::unordered_map<std::string, u32> dictIndex;

    // null bitmap, materialized for every column (strings included)
    std::vector<u64> nullbm;

    // stats (for selectivity-based AND reordering)
    i64 minI = 0, maxI = 0;
    double minD = 0, maxD = 0;
    i64 distinct = 0;

    size_t size() const {
        if (type == DType::I64) return i64v.size();
        if (type == DType::F64) return f64v.size();
        return sid.size();
    }
    bool isnull(size_t row) const { return bget(nullbm.data(), row); }

    // rough per-column byte footprint (for the web UI)
    size_t byteSize() const {
        size_t b = i64v.capacity() * sizeof(i64)
                 + f64v.capacity() * sizeof(double)
                 + sid.capacity() * sizeof(u32)
                 + nullbm.capacity() * sizeof(u64);
        for (auto &s : dict) b += s.size();
        return b;
    }
};

struct Table {
    std::string name;
    size_t nrows = 0;
    std::vector<std::unique_ptr<Column>> cols;

    Column *find(const std::string &nm) const {
        for (auto &c : cols) if (c->name == nm) return c.get();
        return nullptr;
    }
    int indexOf(const std::string &nm) const {
        for (size_t i = 0; i < cols.size(); ++i)
            if (cols[i]->name == nm) return (int)i;
        return -1;
    }
    size_t byteSize() const {
        size_t b = 0;
        for (auto &c : cols) b += c->byteSize();
        return b;
    }
};

// --------------------------------------------------------------- timing
using Clock = std::chrono::steady_clock;

inline double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

struct Timer {
    Clock::time_point t0 = Clock::now();
    double elapsed_ms() const { return ms_since(t0); }
};

// ------------------------------------------------------------ utilities
uint64_t fnv1a64(const void *data, size_t len, uint64_t seed = 1469598103934665603ull);
uint64_t fnv1a64_str(const std::string &s, uint64_t seed = 1469598103934665603ull);
uint64_t fnv1a64_u64(uint64_t x, uint64_t seed = 1469598103934665603ull);

std::string to_hex(uint64_t x);
std::string human_bytes(size_t n);
std::string trim(const std::string &s);
std::vector<std::string> split_lines(const std::string &text);
bool starts_with(const std::string &s, const std::string &pfx);
bool ends_with(const std::string &s, const std::string &sfx);
std::string to_lower(std::string s);

// file helpers
bool file_exists(const std::string &path);
std::string read_file(const std::string &path);
void write_file(const std::string &path, const std::string &content);
void ensure_dir(const std::string &path);
std::string join_path(const std::string &a, const std::string &b);

// minimal JSON escaping
std::string json_escape(const std::string &s);

} // namespace qex
