#include "datagen.h"

#include <cmath>
#include <fstream>

namespace qex {

namespace {

const std::vector<std::string> kServices = {
    "auth", "billing", "cart", "catalog", "checkout", "edge-gateway",
    "inventory", "notify", "order", "payment", "profile", "recommend",
    "search", "session", "shipping", "storage"
};
const std::vector<std::string> kLevels = {
    "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};
const std::vector<std::string> kRegions = {
    "cn-north", "cn-east", "cn-south", "us-west", "us-east",
    "eu-central", "ap-southeast"
};
const std::vector<std::string> kHosts = [] {
    std::vector<std::string> h;
    static const char *az[] = {"a", "b", "c"};
    for (int i = 1; i <= 24; ++i)
        for (const char *z : az)
            h.push_back("host-" + std::to_string(i) + "-" + z);
    return h;
}();
const std::vector<std::string> kMsgs = {
    "request completed successfully",
    "connection timeout while upstream call",
    "user login succeeded",
    "user login failed: bad password",
    "cache miss for key, falling back to database",
    "cache hit served from memory",
    "payment transaction authorized",
    "payment transaction declined by bank",
    "database query slow, plan degraded",
    "database connection pool exhausted",
    "retry attempt failed, scheduling backoff",
    "circuit breaker opened for downstream service",
    "rate limit exceeded for client token",
    "object stored in object storage",
    "object not found in object storage",
    "background job enqueued",
    "background job finished with warnings",
    "TLS handshake completed",
    "TLS handshake failed: certificate expired",
    "configuration reloaded from control plane",
    "memory usage above warning threshold",
    "disk usage above critical threshold",
    "scheduled snapshot started",
    "scheduled snapshot completed",
    "message published to event bus",
    "consumer lag detected on event bus",
    "index rebuild completed",
    "index rebuild failed with lock error",
    "permission denied for resource access",
    "token refreshed successfully",
    "token refresh failed, retrying",
    "batch import started",
    "batch import finished",
    "webhook delivery succeeded",
    "webhook delivery failed after retries"
};

u32 intern(Column &c, const std::string &s) {
    auto it = c.dictIndex.find(s);
    if (it != c.dictIndex.end()) return it->second;
    u32 id = (u32)c.dict.size();
    c.dict.push_back(s);
    c.dictIndex.emplace(s, id);
    return id;
}

Column *newStr(Table &t, const std::string &name) {
    auto c = std::make_unique<Column>();
    c->name = name;
    c->type = DType::STR;
    Column *raw = c.get();
    t.cols.push_back(std::move(c));
    return raw;
}
Column *newNum(Table &t, const std::string &name, DType ty) {
    auto c = std::make_unique<Column>();
    c->name = name;
    c->type = ty;
    Column *raw = c.get();
    t.cols.push_back(std::move(c));
    return raw;
}

} // namespace

std::unique_ptr<Table> generate_table(const GenSpec &spec) {
    auto tbl = std::make_unique<Table>();
    tbl->name = "logs";
    tbl->nrows = spec.rows;
    Rng rng(spec.seed);

    Column *cTs = newNum(*tbl, "ts", DType::I64);
    Column *cSvc = newStr(*tbl, "service");
    Column *cLvl = newStr(*tbl, "level");
    Column *cReg = newStr(*tbl, "region");
    Column *cHost = newStr(*tbl, "host");
    Column *cMsg = newStr(*tbl, "msg");
    Column *cStatus = newNum(*tbl, "status", DType::I64);
    Column *cLat = newNum(*tbl, "latency_us", DType::I64);
    Column *cBytes = newNum(*tbl, "resp_bytes", DType::I64);
    Column *cCpu = newNum(*tbl, "cpu_pct", DType::F64);
    Column *cUid = newNum(*tbl, "user_id", DType::I64);

    // pre-intern dictionary columns
    for (auto &s : kServices) intern(*cSvc, s);
    for (auto &s : kLevels) intern(*cLvl, s);
    for (auto &s : kRegions) intern(*cReg, s);
    for (auto &s : kHosts) intern(*cHost, s);
    for (auto &s : kMsgs) intern(*cMsg, s);

    for (auto *c : {cSvc, cLvl, cReg, cHost, cMsg}) {
        c->sid.resize(spec.rows);
        c->nullbm.assign(nwords(spec.rows), 0);
        c->distinct = (i64)c->dict.size();
    }
    for (auto *c : {cTs, cStatus, cLat, cBytes, cUid}) {
        c->i64v.resize(spec.rows);
        c->nullbm.assign(nwords(spec.rows), 0);
    }
    cCpu->f64v.resize(spec.rows);
    cCpu->nullbm.assign(nwords(spec.rows), 0);

    const i64 baseTs = 1'700'000'000;
    for (size_t i = 0; i < spec.rows; ++i) {
        bool isnull = rng.uniform() < spec.nullFrac;

        cTs->i64v[i] = baseTs + (i64)i + (i64)rng.below(3);

        cSvc->sid[i] = (u32)rng.below(kServices.size());
        cReg->sid[i] = (u32)rng.below(kRegions.size());
        cHost->sid[i] = (u32)rng.below(kHosts.size());

        // level distribution: INFO heavy, FATAL rare
        uint64_t lr = rng.below(100);
        if      (lr < 62) cLvl->sid[i] = 0;  // DEBUG
        else if (lr < 87) cLvl->sid[i] = 1;  // INFO
        else if (lr < 96) cLvl->sid[i] = 2;  // WARN
        else if (lr < 99) cLvl->sid[i] = 3;  // ERROR
        else              cLvl->sid[i] = 4;  // FATAL

        // status: mostly 2xx, some 4xx/5xx
        uint64_t sr = rng.below(100);
        cStatus->i64v[i] = sr < 82 ? 200 + rng.below(6)
                          : sr < 93 ? 400 + rng.below(18)
                          : 500 + rng.below(4);

        // latency: lognormal-ish, occasional spikes; wide range so the
        // benchmark's high-threshold predicates still select rows.
        double g = 0;
        for (int k = 0; k < 4; ++k) g += rng.uniform();
        i64 lat = (i64)(2000 * std::exp((g - 2) * 1.35)) + rng.below(50);
        cLat->i64v[i] = lat;
        cBytes->i64v[i] = (i64)(rng.below(8000) * std::exp(rng.uniform() * 0.7));
        cCpu->f64v[i] = std::min(100.0, rng.uniform() * 98.0 +
                                 (rng.below(20) == 0 ? 20.0 : 0.0));
        cUid->i64v[i] = (i64)rng.below(1'000'000);

        // msg correlates with status: 5xx picks from the error-ish templates
        if (cStatus->i64v[i] >= 500) {
            static const u32 errIds[] = {1, 6, 7, 8, 9, 10, 11, 18, 26,
                                         27, 28, 33};
            cMsg->sid[i] = errIds[rng.below(sizeof(errIds) / sizeof(u32))];
        } else {
            cMsg->sid[i] = (u32)rng.below(kMsgs.size());
        }

        // nulls only on nullable measurement columns
        if (isnull) {
            int pick = (int)rng.below(4);
            Column *targets[] = {cLat, cBytes, cCpu, cUid};
            Column *tc = targets[pick];
            bset(tc->nullbm.data(), i);
            if (tc->type == DType::F64) tc->f64v[i] = 0.0;
            else tc->i64v[i] = 0;
        }
    }

    // Sort every string dictionary lexicographically and remap row ids so
    // that dictionary-id order equals string order (enables id compares).
    for (auto *c : {cSvc, cLvl, cReg, cHost, cMsg}) {
        std::vector<std::string> sorted = c->dict;
        std::sort(sorted.begin(), sorted.end());
        std::unordered_map<std::string, u32> remap;
        for (u32 i = 0; i < sorted.size(); ++i) remap[sorted[i]] = i;
        for (size_t i = 0; i < spec.rows; ++i) {
            if (!c->isnull(i))
                c->sid[i] = remap[c->dict[c->sid[i]]];
        }
        c->dict = std::move(sorted);
        c->dictIndex = remap;
        c->distinct = (i64)c->dict.size();
    }

    // stats
    for (auto &cp : tbl->cols) {
        Column &c = *cp;
        if (c.type == DType::I64) {
            bool any = false;
            for (size_t i = 0; i < spec.rows; ++i) {
                if (c.isnull(i)) continue;
                if (!any || c.i64v[i] < c.minI) c.minI = c.i64v[i];
                if (!any || c.i64v[i] > c.maxI) c.maxI = c.i64v[i];
                any = true;
            }
            if (!any) { c.minI = c.maxI = 0; }
            // rough distinct estimate for numerics
            c.distinct = std::min<i64>((i64)spec.rows,
                                       std::max<i64>(1, c.maxI - c.minI + 1));
        } else if (c.type == DType::F64) {
            bool any = false;
            for (size_t i = 0; i < spec.rows; ++i) {
                if (c.isnull(i)) continue;
                if (!any || c.f64v[i] < c.minD) c.minD = c.f64v[i];
                if (!any || c.f64v[i] > c.maxD) c.maxD = c.f64v[i];
                any = true;
            }
            if (!any) c.minD = c.maxD = 0;
            c.distinct = (i64)std::min<double>(spec.rows, 100000);
        }
    }
    return tbl;
}

// =========================================================== binary format
namespace {
template <class T> void put(std::ofstream &f, T v) {
    f.write(reinterpret_cast<const char*>(&v), sizeof(T));
}
template <class T> T get(std::ifstream &f) {
    T v{};
    f.read(reinterpret_cast<char*>(&v), sizeof(T));
    return v;
}
void putStr(std::ofstream &f, const std::string &s) {
    put<uint32_t>(f, (uint32_t)s.size());
    f.write(s.data(), (std::streamsize)s.size());
}
std::string getStr(std::ifstream &f) {
    uint32_t n = get<uint32_t>(f);
    std::string s(n, '\0');
    f.read(s.data(), n);
    return s;
}
} // namespace

void save_table(const Table &t, const std::string &path) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("cannot write " + path);
    put<uint32_t>(f, 0x51455831); // "QEX1"
    put<uint64_t>(f, t.nrows);
    put<uint32_t>(f, (uint32_t)t.cols.size());
    for (auto &cp : t.cols) {
        const Column &c = *cp;
        putStr(f, c.name);
        put<int>(f, (int)c.type);
        put<i64>(f, c.minI); put<i64>(f, c.maxI);
        put<double>(f, c.minD); put<double>(f, c.maxD);
        put<i64>(f, c.distinct);
        f.write(reinterpret_cast<const char*>(c.nullbm.data()),
                (std::streamsize)(nwords(t.nrows) * sizeof(u64)));
        if (c.type == DType::I64)
            f.write(reinterpret_cast<const char*>(c.i64v.data()),
                    (std::streamsize)(t.nrows * sizeof(i64)));
        else if (c.type == DType::F64)
            f.write(reinterpret_cast<const char*>(c.f64v.data()),
                    (std::streamsize)(t.nrows * sizeof(double)));
        else {
            f.write(reinterpret_cast<const char*>(c.sid.data()),
                    (std::streamsize)(t.nrows * sizeof(u32)));
            put<uint32_t>(f, (uint32_t)c.dict.size());
            for (auto &s : c.dict) putStr(f, s);
        }
    }
}

std::unique_ptr<Table> load_table(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    if (get<uint32_t>(f) != 0x51455831)
        throw std::runtime_error("bad magic in " + path);
    auto t = std::make_unique<Table>();
    t->name = "logs";
    t->nrows = get<uint64_t>(f);
    uint32_t nc = get<uint32_t>(f);
    for (uint32_t ci = 0; ci < nc; ++ci) {
        auto c = std::make_unique<Column>();
        c->name = getStr(f);
        c->type = (DType)get<int>(f);
        c->minI = get<i64>(f); c->maxI = get<i64>(f);
        c->minD = get<double>(f); c->maxD = get<double>(f);
        c->distinct = get<i64>(f);
        c->nullbm.resize(nwords(t->nrows));
        f.read(reinterpret_cast<char*>(c->nullbm.data()),
               (std::streamsize)(nwords(t->nrows) * sizeof(u64)));
        if (c->type == DType::I64) {
            c->i64v.resize(t->nrows);
            f.read(reinterpret_cast<char*>(c->i64v.data()),
                   (std::streamsize)(t->nrows * sizeof(i64)));
        } else if (c->type == DType::F64) {
            c->f64v.resize(t->nrows);
            f.read(reinterpret_cast<char*>(c->f64v.data()),
                   (std::streamsize)(t->nrows * sizeof(double)));
        } else {
            c->sid.resize(t->nrows);
            f.read(reinterpret_cast<char*>(c->sid.data()),
                   (std::streamsize)(t->nrows * sizeof(u32)));
            uint32_t nd = get<uint32_t>(f);
            c->dict.resize(nd);
            for (uint32_t k = 0; k < nd; ++k) {
                c->dict[k] = getStr(f);
                c->dictIndex.emplace(c->dict[k], k);
            }
        }
        t->cols.push_back(std::move(c));
    }
    return t;
}

} // namespace qex
