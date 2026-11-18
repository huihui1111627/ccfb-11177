#include "jit.h"
#include "codegen.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace qex {

const char *jit_source_name(JitSource s) {
    switch (s) {
        case JitSource::FreshCompile: return "fresh-compile";
        case JitSource::MemoryCache:  return "memory-cache";
        case JitSource::DiskCache:    return "disk-cache";
        case JitSource::None:         return "fallback-interpreter";
    }
    return "?";
}

// ------------------------------------------------------- compiler detect
std::string JitEngine::detectCompiler() {
    std::string cc = ::getenv("QEX_CC") ? ::getenv("QEX_CC") : "cc";
    int p[2];
    if (pipe(p) != 0) return cc + " (version unknown)";
    pid_t pid = fork();
    if (pid == 0) {
        dup2(p[1], 1);
        dup2(p[1], 2);
        close(p[0]); close(p[1]);
        execlp(cc.c_str(), cc.c_str(), "--version", (char*)nullptr);
        _exit(127);
    }
    close(p[1]);
    std::string ver;
    char buf[512];
    ssize_t n;
    while ((n = read(p[0], buf, sizeof(buf))) > 0) ver.append(buf, n);
    close(p[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    auto nl = ver.find('\n');
    std::string first = nl == std::string::npos ? ver : ver.substr(0, nl);
    // fingerprint: compiler id + full version text hash + target triple
    std::string target;
#if defined(__APPLE__) && defined(__MACH__)
    target = "darwin-";
#elif defined(__linux__)
    target = "linux-";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    target += "arm64";
#elif defined(__x86_64__)
    target += "x86_64";
#else
    target += "unknown";
#endif
    fingerprint_ = target + " | " + trim(first) + " | " +
                   to_hex(fnv1a64_str(ver));
    return cc;
}

JitEngine::JitEngine(std::string cacheDir, double timeoutSec, JitMode mode)
    : cacheDir_(std::move(cacheDir)), timeoutSec_(timeoutSec), mode_(mode) {
    ensure_dir(cacheDir_);
    std::string cc = detectCompiler();
    ensure_dir(join_path(cacheDir_, to_hex(fnv1a64_str(fingerprint_))));
    // record fingerprint for the UI
    write_file(join_path(cacheDir_, "fingerprint.txt"),
               fingerprint_ + "\ncompiler=" + cc + "\n");
}

JitEngine::~JitEngine() {
    std::lock_guard<std::mutex> g(mu_);
    for (auto &kv : modules_)
        if (kv.second->handle) dlclose(kv.second->handle);
}

JitEngine::Stats JitEngine::stats() const {
    std::lock_guard<std::mutex> g(mu_);
    return stats_;
}

// ------------------------------------------------------------- planning
JitPlan JitEngine::plan(const Query &q) const {
    JitPlan pl;
    pl.generatedC = emit_translation_unit(q, pl);
    return pl;
}

// ------------------------------------------------------- process runner
namespace {

struct ProcResult {
    int exitCode = -1;
    bool timedOut = false;
    std::string output;
};

ProcResult run_compiler(const std::vector<std::string> &argv, double timeout) {
    ProcResult pr;
    int outp[2];
    if (pipe(outp) != 0) { pr.output = "pipe failed"; return pr; }
    pid_t pid = fork();
    if (pid < 0) { pr.output = "fork failed"; return pr; }
    if (pid == 0) {
        dup2(outp[1], 1);
        dup2(outp[1], 2);
        close(outp[0]); close(outp[1]);
        std::vector<char*> av;
        av.reserve(argv.size() + 1);
        for (auto &s : argv) av.push_back(const_cast<char*>(s.c_str()));
        av.push_back(nullptr);
        execvp(av[0], av.data());
        _exit(127);
    }
    close(outp[1]);

    auto deadline = Clock::now() +
                    std::chrono::milliseconds((long long)(timeout * 1000));
    char buf[4096];
    while (true) {
        int st;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) {
            ssize_t n;
            while ((n = read(outp[0], buf, sizeof(buf))) > 0)
                pr.output.append(buf, n);
            pr.exitCode = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
            break;
        }
        if (r < 0) { pr.output = "waitpid failed"; break; }
        if (Clock::now() > deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            pr.timedOut = true;
            ssize_t n;
            while ((n = read(outp[0], buf, sizeof(buf))) > 0)
                pr.output.append(buf, n);
            break;
        }
        // drain pipe to avoid blocking on verbose output
        fcntl(outp[0], F_SETFL, O_NONBLOCK);
        ssize_t n = read(outp[0], buf, sizeof(buf));
        if (n > 0) pr.output.append(buf, n);
        usleep(2000);
    }
    close(outp[0]);
    return pr;
}

std::string so_extension() {
#if defined(__APPLE__) && defined(__MACH__)
    return ".dylib";
#else
    return ".so";
#endif
}

} // namespace

// ------------------------------------------------------- cache layout
namespace {

} // namespace

std::shared_ptr<JitModule> JitEngine::compile_locked(const JitPlan &pl,
                                                     JitCompileInfo &info) {
    std::string fpDir = join_path(cacheDir_, to_hex(fnv1a64_str(fingerprint_)));
    ensure_dir(fpDir);
    std::string base = join_path(fpDir, to_hex(pl.structHash));
    std::string cPath = base + ".c";
    std::string metaPath = base + ".meta";
    std::string soPath = base + so_extension();
    info.cPath = cPath;
    info.soPath = soPath;
    info.structHashHex = to_hex(pl.structHash);

    // The generated TU includes qex_runtime.h by name; place the source next
    // to a symlink/copy of the header inside the fingerprint directory.
    std::string hdrDst = join_path(fpDir, "qex_runtime.h");
    if (!file_exists(hdrDst)) {
        // engine locates the canonical header next to the binary's source
        const char *envH = ::getenv("QEX_RUNTIME_H");
        std::string hdrSrc = envH ? envH : "src/qex_runtime.h";
        if (file_exists(hdrSrc)) write_file(hdrDst, read_file(hdrSrc));
    }

    write_file(cPath, pl.generatedC);

    std::string cc = ::getenv("QEX_CC") ? ::getenv("QEX_CC") : "cc";
    std::vector<std::string> argv{
        cc, "-O3", "-std=c11", "-fPIC",
        "-I", fpDir,
        "-Rpass=loop-vectorize", "-Rpass-missed=loop-vectorize",
        "-shared", "-o", soPath, cPath
    };
#if defined(__APPLE__) && defined(__MACH__)
    // -fPIC is implied on darwin but harmless
#endif
    if (mode_ == JitMode::ForceFail)
        argv = {cc, "-this-flag-does-not-exist", cPath};

    auto t0 = Clock::now();
    ProcResult pr = run_compiler(argv, timeoutSec_);
    info.compileMs = ms_since(t0);

    if (pr.timedOut) {
        info.fallbackReason = "compile timeout after " +
                              std::to_string(timeoutSec_) + "s";
        ++stats_.fallbacks;
        return nullptr;
    }
    if (pr.exitCode != 0 || !file_exists(soPath)) {
        info.fallbackReason = "compiler error (exit " +
                              std::to_string(pr.exitCode) + ")";
        ++stats_.fallbacks;
        // surface first meaningful error lines to the UI
        std::istringstream is(pr.output);
        std::string line, shortErr;
        size_t lines = 0;
        while (std::getline(is, line) && lines < 6) {
            shortErr += line + "\n"; ++lines;
        }
        info.fallbackReason += ": " + shortErr;
        return nullptr;
    }

    // parse vectorization remarks
    std::istringstream is(pr.output);
    std::string line;
    while (std::getline(is, line)) {
        if (line.find("vectorized") != std::string::npos ||
            line.find("loop-vectorize") != std::string::npos) {
            info.vecRemarks.push_back(trim(line));
            if (line.find("vectorized loop") != std::string::npos ||
                line.find("remark:") != std::string::npos)
                info.vecFilter = true;
        }
    }
    info.vecLoopCount = (int)info.vecRemarks.size();

    void *handle = dlopen(soPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        info.fallbackReason = std::string("dlopen failed: ") + dlerror();
        ++stats_.fallbacks;
        return nullptr;
    }
    auto mod = std::make_shared<JitModule>();
    mod->handle = handle;
    mod->structHash = pl.structHash;
    mod->soPath = soPath;
    mod->filter = (qex_filter_fn)dlsym(handle, "qex_filter");
    mod->project = (qex_project_fn)dlsym(handle, "qex_project");
    if (!mod->filter || !mod->project) {
        info.fallbackReason = "dlsym failed";
        ++stats_.fallbacks;
        dlclose(handle);
        return nullptr;
    }

    // metadata for cross-restart diagnostics
    std::ostringstream meta;
    meta << "struct_hash=" << to_hex(pl.structHash) << "\n"
         << "fingerprint=" << fingerprint_ << "\n"
         << "compile_ms=" << info.compileMs << "\n"
         << "vec_loops=" << info.vecLoopCount << "\n";
    write_file(metaPath, meta.str());

    stats_.totalCompileMs += info.compileMs;
    stats_.lastCompileMs = info.compileMs;
    typicalCompileMs_ = 0.7 * typicalCompileMs_ + 0.3 * info.compileMs;
    ++stats_.compiles;
    return mod;
}

JitCompileInfo JitEngine::getOrCompile(const JitPlan &pl) {
    std::lock_guard<std::mutex> g(mu_);
    JitCompileInfo info;
    info.compiler = fingerprint_;
    info.cacheDir = cacheDir_;
    info.structHashHex = to_hex(pl.structHash);
    ++stats_.lookups;

    auto mit = modules_.find(pl.structHash);
    if (mit != modules_.end()) {
        info.source = JitSource::MemoryCache;
        info.soPath = mit->second->soPath;
        ++stats_.memHits;
        stats_.totalSavedMsMem += typicalCompileMs_;
        return info;
    }

    std::string fpDir = join_path(cacheDir_, to_hex(fnv1a64_str(fingerprint_)));
    std::string soPath = join_path(fpDir, to_hex(pl.structHash)) + so_extension();
    if (file_exists(soPath)) {
        void *handle = dlopen(soPath.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle) {
            auto mod = std::make_shared<JitModule>();
            mod->handle = handle;
            mod->structHash = pl.structHash;
            mod->soPath = soPath;
            mod->filter = (qex_filter_fn)dlsym(handle, "qex_filter");
            mod->project = (qex_project_fn)dlsym(handle, "qex_project");
            if (mod->filter && mod->project) {
                modules_[pl.structHash] = mod;
                info.source = JitSource::DiskCache;
                ++stats_.diskHits;
                stats_.totalSavedMsDisk += typicalCompileMs_;
                std::string metaPath =
                    join_path(fpDir, to_hex(pl.structHash)) + ".meta";
                if (file_exists(metaPath)) {
                    auto meta = read_file(metaPath);
                    std::istringstream is(meta);
                    std::string ln;
                    while (std::getline(is, ln)) {
                        if (starts_with(ln, "vec_loops="))
                            info.vecLoopCount = std::stoi(ln.substr(10));
                        if (starts_with(ln, "compile_ms="))
                            info.vecRemarks.push_back("cached compile_ms=" +
                                                      ln.substr(11));
                    }
                    info.vecFilter = info.vecLoopCount > 0;
                }
                return info;
            }
            dlclose(handle);
        }
        // corrupt cache entry: remove and recompile
        ::unlink(soPath.c_str());
    }

    auto mod = compile_locked(pl, info);
    if (!mod) {
        info.source = JitSource::None;
        return info;
    }
    modules_[pl.structHash] = mod;
    info.source = JitSource::FreshCompile;
    return info;
}

// ------------------------------------------------------------- execution
std::shared_ptr<JitModule> JitEngine::moduleFor(u64 structHash) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = modules_.find(structHash);
    return it == modules_.end() ? nullptr : it->second;
}

ProjResult JitEngine::run(const Query &q, const JitPlan &pl,
                          const JitModule &mod,
                          const Table &t, JitRunMetrics &m) {
    const size_t nrows = t.nrows;
    const size_t nw = nwords(nrows);

    // ---- build qex_col array for referenced columns
    std::vector<qex_col> jcols(t.cols.size());
    std::vector<std::vector<const char*>> dictPtrs(t.cols.size());
    std::vector<std::vector<int32_t>> dictLens(t.cols.size());
    for (size_t ci = 0; ci < t.cols.size(); ++ci) {
        const Column &c = *t.cols[ci];
        qex_col &jc = jcols[ci];
        jc.type = (int)c.type;
        jc.nrows = nrows;
        jc.nullbm = c.nullbm.data();
        if (c.type == DType::I64) jc.i64 = c.i64v.data();
        if (c.type == DType::F64) jc.f64 = c.f64v.data();
        if (c.type == DType::STR) {
            jc.sid = c.sid.data();
            jc.ndict = (int)c.dict.size();
            dictPtrs[ci].resize(c.dict.size());
            dictLens[ci].resize(c.dict.size());
            for (size_t k = 0; k < c.dict.size(); ++k) {
                dictPtrs[ci][k] = c.dict[k].data();
                dictLens[ci][k] = (int32_t)c.dict[k].size();
            }
            jc.dict_ptr = dictPtrs[ci].data();
            jc.dict_len = dictLens[ci].data();
        }
    }
    std::vector<const qex_col*> colPtrs(t.cols.size());
    for (size_t i = 0; i < t.cols.size(); ++i) colPtrs[i] = &jcols[i];

    // ---- string predicate bitmaps, dictionary sized
    size_t dictWordsMax = 1;
    for (auto &sp : pl.spreds)
        dictWordsMax = std::max(dictWordsMax,
                                nwords(t.cols[sp.col]->dict.size()));
    std::vector<u64> spred(pl.spreds.size() * dictWordsMax, 0);
    for (auto &sp : pl.spreds) {
        auto bm = build_spred_bitmap(*t.cols[sp.col], sp.pattern, sp.mode);
        std::copy(bm.begin(), bm.end(),
                  spred.begin() + sp.spredIdx * dictWordsMax);
    }

    // constants: strings need stable backing storage
    std::vector<std::string> constStrs(pl.consts.size());
    std::vector<qex_const> consts = pl.consts;
    for (size_t i = 0; i < pl.consts.size(); ++i) {
        if (pl.consts[i].type == QEX_STR) {
            constStrs[i] = std::string(pl.consts[i].sptr, pl.consts[i].slen);
            consts[i].sptr = constStrs[i].data();
        }
    }
    // dictionary id of every string constant, in its column's namespace
    std::vector<u32> litsid(pl.consts.size(), UINT32_MAX);
    for (const auto &sc : pl.sconsts) {
        if (sc.col < 0 || (size_t)sc.col >= t.cols.size()) continue;
        const auto &idx = t.cols[sc.col]->dictIndex;
        auto it = idx.find(sc.value);
        if (it != idx.end()) litsid[sc.constSlot] = it->second;
    }

    std::vector<u64> outbm(nw, 0);
    // Filter needs per-batch temps; projection needs per-selected-row temps.
    size_t filterNeed = (pl.tempCount + 4) *
                        (QEX_BATCH * 16 + QEX_WORDS(QEX_BATCH) * 8) +
                        (1u << 18);
    std::vector<unsigned char> scratch(filterNeed, 0);

    qex_filter_args fa{};
    fa.cols = colPtrs.data();
    fa.ncols = colPtrs.size();
    fa.consts = consts.data();
    fa.nconsts = consts.size();
    fa.out = outbm.data();
    fa.nwords = nw;
    fa.nrows = nrows;
    fa.scratch = scratch.data();
    fa.dlen = scratch.size();
    fa.spred = spred.data();
    fa.nspred = pl.spreds.size();
    fa.spred_sid = nullptr;
    fa.spred_nwords = dictWordsMax;
    fa.litsid = litsid.data();

    // ---- filter
    auto t0 = Clock::now();
    int rc = mod.filter(&fa);
    if (rc != 0) throw std::runtime_error("JIT filter returned " +
                                          std::to_string(rc));
    double filterMs = ms_since(t0);

    std::vector<u32> sel;
    sel.reserve(bcount(outbm.data(), nrows));
    for (size_t i = 0; i < nrows; ++i)
        if (bget(outbm.data(), i)) sel.push_back((u32)i);
    double projectMs = 0;

    // grow scratch for projection's per-selected-row temp arrays
    size_t projNeed = (q.proj.size() * 8 + 8) * (sel.size() * 16 + 256);
    if (scratch.size() < filterNeed + projNeed)
        scratch.resize(filterNeed + projNeed, 0);

    // ---- projection
    size_t nout = q.proj.size();
    ProjResult res;
    res.nrows = sel.size();
    res.types.resize(nout);
    res.i64cols.resize(nout);
    res.f64cols.resize(nout);
    res.strcols.resize(nout);
    res.nullbms.resize(nout);

    std::vector<int> outType(nout);
    std::vector<i64> outI64(nout * sel.size());
    std::vector<double> outF64(nout * sel.size());
    std::vector<u32> outSid(nout * sel.size(), UINT32_MAX);
    std::vector<char**> outSPtrPtrs(nout, nullptr);
    std::vector<int32_t*> outSLenPtrs(nout, nullptr);
    size_t pnw = nwords(sel.size());
    std::vector<u64> outNull(nout * pnw, 0);

    qex_project_args pa{};
    pa.cols = colPtrs.data();
    pa.ncols = colPtrs.size();
    pa.consts = consts.data();
    pa.nconsts = consts.size();
    pa.sel = sel.data();
    pa.nsel = sel.size();
    pa.scratch = scratch.data();
    pa.dlen = scratch.size();
    pa.out_type = outType.data();
    pa.out_i64 = outI64.data();
    pa.out_f64 = outF64.data();
    pa.out_sptr = outSPtrPtrs.data();
    pa.out_slen = outSLenPtrs.data();
    pa.out_sid = outSid.data();
    pa.out_null = outNull.data();
    pa.nout = nout;
    pa.spred = spred.data();
    pa.nspred = pl.spreds.size();
    pa.spred_nwords = dictWordsMax;
    pa.litsid = litsid.data();

    auto tp0 = Clock::now();
    if (!sel.empty()) {
        rc = mod.project(&pa);
        if (rc != 0) throw std::runtime_error("JIT project returned " +
                                              std::to_string(rc));
    }
    projectMs = ms_since(tp0);
    double totalMs = ms_since(t0);

    // ---- adopt generated outputs (move, no per-element copy)
    for (size_t k = 0; k < nout; ++k) {
        DType ty = (DType)outType[k];
        res.types[k] = ty;
        res.nullbms[k].assign(outNull.begin() + k * pnw,
                              outNull.begin() + (k + 1) * pnw);
        if (ty == DType::I64)
            res.i64cols[k].assign(outI64.begin() + k * sel.size(),
                                  outI64.begin() + (k + 1) * sel.size());
        else if (ty == DType::F64)
            res.f64cols[k].assign(outF64.begin() + k * sel.size(),
                                  outF64.begin() + (k + 1) * sel.size());
        else
            res.strcols[k].assign(outSid.begin() + k * sel.size(),
                                  outSid.begin() + (k + 1) * sel.size());
    }

    m.rows = nrows;
    m.selected = sel.size();
    m.execMs = totalMs;
    m.filterMs = filterMs;
    m.projectMs = projectMs;
    m.growsPerSec = (nrows / std::max(1e-9, filterMs / 1000.0)) / 1e9;
    return res;
}

void JitEngine::clearDiskCache() {
    std::lock_guard<std::mutex> g(mu_);
    for (auto &kv : modules_)
        if (kv.second->handle) dlclose(kv.second->handle);
    modules_.clear();
    std::string fpDir = join_path(cacheDir_, to_hex(fnv1a64_str(fingerprint_)));
    std::error_code ec;
    std::filesystem::remove_all(fpDir, ec);
    ensure_dir(fpDir);
}

} // namespace qex
