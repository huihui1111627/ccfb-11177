// engine.h - orchestration: parse -> optimize -> interpret/JIT -> verify.
#pragma once

#include "jit.h"
#include "datagen.h"
#include <deque>

namespace qex {

struct QueryResult {
    // identity
    std::string sql;
    size_t queryIndex = 0;
    u64 structHash = 0;
    u64 fullHash = 0;
    std::string structHashHex;

    // optimization
    OptimizeReport opt;
    double codegenMs = 0;
    std::string generatedC;

    // compilation / cache
    JitCompileInfo compile;
    bool usedFallback = false;
    std::string fallbackReason;

    // execution timings (ms) and results
    double interpFilterMs = 0, interpProjectMs = 0, interpTotalMs = 0;
    double jitFilterMs = 0, jitProjectMs = 0, jitTotalMs = 0;
    double speedup = 0;
    size_t rows = 0, selected = 0;
    bool match = false;
    size_t firstDiff = 0;
    JitRunMetrics jitMetrics;

    // LIKE specialization summary
    std::vector<std::string> spredKinds;
};

struct EngineSummary {
    uint64_t totalQueries = 0;
    uint64_t fallbacks = 0;
    uint64_t matched = 0, mismatched = 0;
    double geoMeanSpeedup = 0;
    double totalInterpMs = 0, totalJitMs = 0, totalCompileMs = 0;
    double compileSharePct = 0;   // compile time / (jit exec + compile)
};

class Engine {
public:
    Engine(std::unique_ptr<Table> table, std::string cacheDir,
           double compileTimeoutSec, JitMode mode = JitMode::Normal);

    // Run one SQL statement end to end. verify=false skips interpreter
    // comparison (used by the live web console).
    QueryResult runSql(const std::string &sql, bool verify,
                       bool forceInterp = false);

    // Run a batch (query file), in order.
    std::vector<QueryResult> runBatch(const std::string &sql, bool verify);

    EngineSummary summary() const;
    JitEngine &jit() { return *jit_; }
    const Table &table() const { return *table_; }
    const Stats &stats() const { return tableStats_; }
    const std::deque<QueryResult> &history() const { return history_; }
    void clearHistory() { history_.clear(); }

private:
    std::unique_ptr<Table> table_;
    Stats tableStats_;
    std::unique_ptr<JitEngine> jit_;
    std::deque<QueryResult> history_;
    EngineSummary sum_{};
    std::mutex histMu_;
};

} // namespace qex
