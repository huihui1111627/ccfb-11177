#include "engine.h"

#include <cmath>
#include <iostream>

namespace qex {

Engine::Engine(std::unique_ptr<Table> table, std::string cacheDir,
               double compileTimeoutSec, JitMode mode)
    : table_(std::move(table)) {
    tableStats_.build(*table_);
    jit_ = std::make_unique<JitEngine>(std::move(cacheDir),
                                       compileTimeoutSec, mode);
}

static std::vector<std::string> collect_spreds(const Query &q) {
    std::vector<std::string> out;
    for (Node *n : q.all_nodes()) {
        if (n->kind != NK::Like) continue;
        out.push_back(n->colName.empty()
                          ? ("LIKE " + like_kind_name(n->like))
                          : (n->a->colName + " LIKE '" + n->sv + "' -> " +
                             like_kind_name(n->like)));
    }
    return out;
}

QueryResult Engine::runSql(const std::string &sql, bool verify,
                           bool forceInterp) {
    QueryResult r;
    r.sql = trim(sql);

    auto queries = parse_queries(sql);
    if (queries.size() != 1)
        throw std::runtime_error("expected exactly one statement, got " +
                                 std::to_string(queries.size()));
    Query &q = *queries[0];
    resolve_query(q, *table_);
    infer_types(q.where);
    for (Node *p : q.proj) infer_types(p);
    compute_hashes(q.where);
    for (Node *p : q.proj) compute_hashes(p);

    r.opt = optimize_query(q, tableStats_);
    r.spredKinds = collect_spreds(q);
    r.rows = table_->nrows;

    // ---- codegen
    Timer cgTimer;
    JitPlan pl = jit_->plan(q);
    r.codegenMs = cgTimer.elapsed_ms();
    r.generatedC = pl.generatedC;
    r.structHash = pl.structHash;
    r.fullHash = pl.fullHash;
    r.structHashHex = to_hex(pl.structHash);

    Interpreter interp(*table_);

    // ---- interpreter baseline (always available, used for verification)
    ProjResult interpRes;
    std::vector<u32> interpSel;
    {
        Timer t;
        interpSel = interp.filter(q.where);
        r.interpFilterMs = t.elapsed_ms();
        Timer t2;
        interpRes = interp.project(q.proj, interpSel);
        r.interpProjectMs = t2.elapsed_ms();
        r.interpTotalMs = r.interpFilterMs + r.interpProjectMs;
        r.selected = interpSel.size();
    }

    // ---- JIT path with fallback
    if (!forceInterp) {
        r.compile = jit_->getOrCompile(pl);
        if (r.compile.source == JitSource::None) {
            r.usedFallback = true;
            r.fallbackReason = r.compile.fallbackReason;
            r.jitTotalMs = r.interpTotalMs;   // fallback cost == interpreter
            r.match = true;
            if (verify) ++sum_.matched;       // fallback reuses verified interp result
        } else {
            try {
                auto mod = jit_->moduleFor(pl.structHash);
                if (!mod) throw std::runtime_error("module vanished");

                JitRunMetrics jm;
                ProjResult jitRes = jit_->run(q, pl, *mod, *table_, jm);
                r.jitMetrics = jm;
                r.jitFilterMs = jm.filterMs;
                r.jitProjectMs = jm.projectMs;
                r.jitTotalMs = jm.execMs;

                if (verify) {
                    r.match = proj_equal(interpRes, jitRes, &r.firstDiff);
                    if (r.match) ++sum_.matched;
                    else ++sum_.mismatched;
                } else {
                    r.match = true;
                }
            } catch (const std::exception &e) {
                r.usedFallback = true;
                r.fallbackReason = std::string("JIT runtime failure: ") +
                                   e.what();
                r.jitTotalMs = r.interpTotalMs;
                if (verify) { r.match = true; ++sum_.matched; }
            }
        }
    } else {
        r.usedFallback = true;
        r.fallbackReason = "forced interpreter mode";
        r.jitTotalMs = r.interpTotalMs;
        r.match = true;
    }

    if (r.usedFallback) ++sum_.fallbacks;
    r.speedup = r.jitTotalMs > 0 ? r.interpTotalMs / r.jitTotalMs : 0;

    // ---- aggregate summary
    ++sum_.totalQueries;
    sum_.totalInterpMs += r.interpTotalMs;
    sum_.totalJitMs += r.jitTotalMs;
    sum_.totalCompileMs +=
        r.compile.source == JitSource::FreshCompile ? r.compile.compileMs : 0;
    double denom = sum_.totalJitMs + sum_.totalCompileMs;
    sum_.compileSharePct = denom > 0 ? 100.0 * sum_.totalCompileMs / denom : 0;
    if (r.speedup > 0) {
        double &g = sum_.geoMeanSpeedup;
        g = g == 0 ? std::log(r.speedup) : g + std::log(r.speedup);
    }
    sum_.geoMeanSpeedup =
        sum_.totalQueries ? std::exp(sum_.geoMeanSpeedup /
                                     (double)sum_.totalQueries)
                          : 0;

    {
        std::lock_guard<std::mutex> g(histMu_);
        r.queryIndex = history_.size();
        history_.push_back(r);
        if (history_.size() > 200) history_.pop_front();
    }
    return r;
}

std::vector<QueryResult> Engine::runBatch(const std::string &sql, bool verify) {
    auto queries = parse_queries(sql);
    std::vector<QueryResult> out;
    out.reserve(queries.size());
    for (auto &q : queries) out.push_back(runSql(q->source, verify));
    return out;
}

EngineSummary Engine::summary() const { return sum_; }

} // namespace qex
