// main.cpp - qex command line entry points.
#include "engine.h"
#include "datagen.h"
#include "webserver.h"

#include <atomic>
#include <csignal>
#include <iostream>
#include <signal.h>
#include <unistd.h>

using namespace qex;

namespace {

std::unique_ptr<WebServer> g_srv;

void onSignal(int) {
    if (g_srv) g_srv->stop();
}

int cmdGen(int argc, char **argv) {
    GenSpec spec;
    std::string out = "var/logs.qex";
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&] {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
            return std::string(argv[++i]);
        };
        if (a == "--rows") spec.rows = std::stoull(next());
        else if (a == "--seed") spec.seed = std::stoull(next());
        else if (a == "--null-frac") spec.nullFrac = std::stod(next());
        else if (a == "-o" || a == "--out") out = next();
        else throw std::runtime_error("unknown arg " + a);
    }
    std::cout << "generating " << spec.rows << " log rows ..." << std::endl;
    Timer t;
    auto tbl = generate_table(spec);
    std::cout << "generated in " << t.elapsed_ms() << " ms, memory "
              << human_bytes(tbl->byteSize()) << std::endl;
    ensure_dir(out.substr(0, out.find_last_of('/')));
    save_table(*tbl, out);
    std::cout << "saved to " << out << std::endl;
    return 0;
}

int cmdInfo(int argc, char **argv) {
    std::string path = argc > 2 ? argv[2] : "var/logs.qex";
    auto tbl = load_table(path);
    std::cout << "table " << tbl->name << ": " << tbl->nrows << " rows, "
              << human_bytes(tbl->byteSize()) << "\n";
    for (auto &c : tbl->cols) {
        std::cout << "  " << c->name << " " << dtype_name(c->type);
        if (c->type == DType::STR)
            std::cout << " dict=" << c->dict.size();
        else if (c->type == DType::I64)
            std::cout << " [" << c->minI << "," << c->maxI << "]";
        else
            std::cout << " [" << c->minD << "," << c->maxD << "]";
        size_t nn = bcount(c->nullbm.data(), tbl->nrows);
        std::cout << " nulls=" << nn << "\n";
    }
    return 0;
}

struct CliOpts {
    std::string data = "var/logs.qex";
    std::string cache = "var/jitcache";
    std::string queries;
    double timeout = 10.0;
    bool verify = true;
    bool forceFail = false;
    bool forceInterp = false;
    int port = 8080;
};

CliOpts parseOpts(int argc, char **argv) {
    CliOpts o;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&] {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
            return std::string(argv[++i]);
        };
        if (a == "--data") o.data = next();
        else if (a == "--cache") o.cache = next();
        else if (a == "--queries") o.queries = next();
        else if (a == "--timeout") o.timeout = std::stod(next());
        else if (a == "--no-verify") o.verify = false;
        else if (a == "--force-fail") o.forceFail = true;
        else if (a == "--interp") o.forceInterp = true;
        else if (a == "--port") o.port = std::stoi(next());
        else throw std::runtime_error("unknown arg " + a);
    }
    return o;
}

void printResult(const QueryResult &r) {
    printf("  #%zu  %.90s\n", r.queryIndex, r.sql.c_str());
    printf("      struct=%s  source=%s  selected=%zu  match=%s\n",
           r.structHashHex.substr(0, 16).c_str(),
           jit_source_name(r.compile.source), r.selected,
           r.match ? "OK" : "MISMATCH");
    printf("      interp %.2f ms | jit %.2f ms | speedup %.2fx | codegen %.2f ms",
           r.interpTotalMs, r.jitTotalMs, r.speedup, r.codegenMs);
    if (r.compile.source == JitSource::FreshCompile)
        printf(" | compile %.1f ms (vec loops %d)", r.compile.compileMs,
               r.compile.vecLoopCount);
    if (r.usedFallback)
        printf("\n      FALLBACK: %s", r.fallbackReason.c_str());
    printf("\n");
}

int cmdRun(int argc, char **argv) {
    CliOpts o = parseOpts(argc, argv);
    if (o.queries.empty()) throw std::runtime_error("--queries <file> required");
    auto tbl = load_table(o.data);
    JitMode mode = o.forceFail ? JitMode::ForceFail : JitMode::Normal;
    Engine engine(std::move(tbl), o.cache, o.timeout, mode);
    std::string sql = read_file(o.queries);
    auto results = engine.runBatch(sql, o.verify);

    std::cout << "\n=== query results (" << results.size() << ") ===\n";
    for (auto &r : results) printResult(r);

    auto sm = engine.summary();
    auto js = engine.jit().stats();
    printf("\n=== summary ===\n");
    printf("queries=%zu matched=%llu/%llu fallbacks=%llu\n",
           results.size(), (unsigned long long)sm.matched,
           (unsigned long long)sm.totalQueries,
           (unsigned long long)sm.fallbacks);
    printf("interpreter total %.1f ms | JIT total %.1f ms | geo speedup %.2fx\n",
           sm.totalInterpMs, sm.totalJitMs, sm.geoMeanSpeedup);
    printf("compiles=%llu memHits=%llu diskHits=%llu avgCompile=%.1f ms "
           "compileShare=%.1f%%\n",
           (unsigned long long)js.compiles,
           (unsigned long long)js.memHits,
           (unsigned long long)js.diskHits, js.avgCompileMs(),
           sm.compileSharePct);
    printf("time saved by memory cache %.1f ms, disk cache %.1f ms\n",
           js.totalSavedMsMem, js.totalSavedMsDisk);
    if (sm.mismatched) {
        printf("RESULT MISMATCH DETECTED\n");
        return 2;
    }
    return 0;
}

int cmdWeb(int argc, char **argv) {
    CliOpts o = parseOpts(argc, argv);
    auto tbl = load_table(o.data);
    JitMode mode = o.forceFail ? JitMode::ForceFail : JitMode::Normal;
    auto engine = std::make_unique<Engine>(std::move(tbl), o.cache,
                                           o.timeout, mode);
    g_srv = std::make_unique<WebServer>(*engine, o.port);
    g_srv->start();
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    std::cout << "qex dashboard: http://127.0.0.1:" << g_srv->port()
              << "  (Ctrl-C to stop)" << std::endl;
    while (g_srv) {
        // WebServer owns its accept thread; wait for stop signal.
        pause();
        if (!g_srv) break;
    }
    return 0;
}

void usage() {
    std::cout <<
R"USAGE(qex - query executor with runtime code generation

usage:
  qex gen [--rows N] [--seed S] [--null-frac F] [-o var/logs.qex]
  qex info [--data var/logs.qex]
  qex run --queries queries/bench.sql [--data ...] [--cache ...]
          [--timeout SEC] [--no-verify] [--force-fail] [--interp]
  qex web [--data ...] [--cache ...] [--port 8080] [--force-fail]

environment:
  QEX_CC           compiler used for JIT (default: cc)
  QEX_RUNTIME_H    path to qex_runtime.h for generated sources
)USAGE";
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }
    try {
        std::string cmd = argv[1];
        if (cmd == "gen") return cmdGen(argc, argv);
        if (cmd == "info") return cmdInfo(argc, argv);
        if (cmd == "run") return cmdRun(argc, argv);
        if (cmd == "web") return cmdWeb(argc, argv);
        if (cmd == "-h" || cmd == "--help" || cmd == "help") { usage(); return 0; }
        usage();
        return 1;
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << std::endl;
        return 1;
    }
}
