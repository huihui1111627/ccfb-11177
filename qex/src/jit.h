// jit.h - runtime C code generation, native compile, dlopen, disk cache.
#pragma once

#include "qex_runtime.h"
#include "expr.h"
#include "optimizer.h"
#include "interpreter.h"

#include <mutex>
#include <unordered_map>

namespace qex {

enum class JitMode {
    Normal,     // compile and load
    ForceFail   // force compilation failure -> interpreter fallback
};

enum class JitSource {
    FreshCompile,   // compiled this run
    MemoryCache,    // shared object already dlopen'ed
    DiskCache,      // loaded from on-disk cache
    None            // fell back to interpreter
};

struct StringPred {
    int col;            // string column index
    size_t spredIdx;    // slot in args->spred
    std::string pattern;
    LikeMode mode;
};

struct StringConst {
    int constSlot;      // index in JitPlan::consts
    int col;            // dictionary namespace
    std::string value;
};

struct JitPlan {
    u64 structHash = 0;             // cache key (constants excluded)
    u64 fullHash = 0;
    std::string generatedC;         // full translation unit
    std::vector<qex_const> consts;  // runtime constant bindings
    std::vector<int> usedCols;      // column indices referenced
    std::vector<StringPred> spreds;
    std::vector<StringConst> sconsts;
    size_t scratchBytes = 0;
    size_t tempCount = 0;           // per-batch temp arrays
    size_t cseCount = 0;
};

struct JitCompileInfo {
    JitSource source = JitSource::None;
    double compileMs = 0;
    double codegenMs = 0;
    std::string cacheDir;
    std::string soPath;
    std::string cPath;
    std::string structHashHex;
    std::string compiler;
    std::vector<std::string> vecRemarks;   // vectorization lines
    bool vecFilter = false;
    int vecLoopCount = 0;
    std::string fallbackReason;
};

struct JitRunMetrics {
    double execMs = 0;
    double filterMs = 0;
    double projectMs = 0;
    size_t rows = 0;
    size_t selected = 0;
    double growsPerSec = 0;
};

// One loaded shared object.
struct JitModule {
    u64 structHash = 0;
    void *handle = nullptr;
    qex_filter_fn filter = nullptr;
    qex_project_fn project = nullptr;
    std::string soPath;
};

class JitEngine {
public:
    JitEngine(std::string cacheDir, double timeoutSec, JitMode mode);
    ~JitEngine();

    // Build a codegen plan for the query (where + projections together).
    JitPlan plan(const Query &q) const;

    // Get (or compile/load) the module for a plan's structural hash.
    JitCompileInfo getOrCompile(const JitPlan &plan);

    // Resolve an already-compiled module (after getOrCompile succeeded).
    std::shared_ptr<JitModule> moduleFor(u64 structHash);

    // Execute filter + projection through JIT. Throws on runtime failure.
    ProjResult run(const Query &q, const JitPlan &plan, const JitModule &mod,
                   const Table &t, JitRunMetrics &m);

    // Stats
    struct Stats {
        uint64_t lookups = 0, memHits = 0, diskHits = 0, compiles = 0;
        uint64_t fallbacks = 0;
        double totalCompileMs = 0;
        double totalSavedMsDisk = 0;   // compile time avoided by disk hits
        double totalSavedMsMem = 0;    // compile+load time avoided by mem hits
        double lastCompileMs = 0;
        double avgCompileMs() const {
            return compiles ? totalCompileMs / (double)compiles : 0.0;
        }
    };
    Stats stats() const;
    void clearDiskCache();
    std::string compilerFingerprint() const { return fingerprint_; }
    std::string cacheDir() const { return cacheDir_; }

private:
    std::string cacheDir_;
    double timeoutSec_;
    JitMode mode_;
    std::string fingerprint_;
    mutable std::mutex mu_;
    std::unordered_map<u64, std::shared_ptr<JitModule>> modules_;
    Stats stats_;
    double typicalCompileMs_ = 50.0;   // estimate used for "time saved"

    std::shared_ptr<JitModule> compile_locked(const JitPlan &plan,
                                              JitCompileInfo &info);
    std::string detectCompiler();
};

const char *jit_source_name(JitSource s);

} // namespace qex
