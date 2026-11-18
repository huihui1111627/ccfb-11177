#include "codegen.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

namespace qex {

// ---------------------------------------------------------- spred bitmap
std::vector<u64> build_spred_bitmap(const Column &c, const std::string &pat,
                                    LikeMode mode) {
    std::vector<u64> bm(nwords(c.dict.size()), 0);
    auto matchOne = [&](const std::string &s) -> bool {
        switch (mode) {
            case LikeMode::Exact:
                return s == pat;
            case LikeMode::Prefix: {
                std::string inner = pat.substr(0, pat.size() - 1);
                return qex_str_prefix(inner.data(), (int32_t)inner.size(),
                                      s.data(), (int32_t)s.size());
            }
            case LikeMode::Contains: {
                std::string inner = pat.substr(1, pat.size() - 2);
                return qex_str_contains(inner.data(), (int32_t)inner.size(),
                                        s.data(), (int32_t)s.size());
            }
            case LikeMode::Glob:
                return qex_str_like(pat.data(), (int32_t)pat.size(),
                                    s.data(), (int32_t)s.size());
        }
        return false;
    };
    for (u32 id = 0; id < c.dict.size(); ++id)
        if (matchOne(c.dict[id])) bset(bm.data(), id);
    return bm;
}

namespace {

struct CG {
    const Query &q;
    JitPlan &pl;

    std::ostringstream fb;   // filter body
    std::ostringstream pb;   // project body
    std::map<u64, int> constIds;
    std::map<u64, int> temps;     // filter temps by fullHash
    std::set<int> usedCols;
    std::map<int, int> constCol;  // const slot -> string column namespace
    std::map<u64, std::string> projNamed;  // shared subexpr temps (all outputs)
    size_t projSeq = 0;
    size_t maxLiveTemps = 0;
    size_t tempSeq = 0;
    size_t cseEmitted = 0;

    CG(const Query &query, JitPlan &plan)
        : q(query), pl(plan) {}

    static std::string cv(int c) { return "c" + std::to_string(c); }

    int bindConst(Node *n) {
        auto it = constIds.find(n->fullHash);
        if (it != constIds.end()) return it->second;
        int id = (int)pl.consts.size();
        qex_const kc{};
        switch (n->kind) {
            case NK::LitI:
                kc.type = QEX_I64; kc.i64 = n->iv; break;
            case NK::LitD:
                kc.type = QEX_F64; kc.f64 = n->dv; break;
            case NK::LitS:
                kc.type = QEX_STR;
                kc.sptr = n->sv.data();
                kc.slen = (int32_t)n->sv.size();
                break;
            default: break;
        }
        pl.consts.push_back(kc);
        constIds[n->fullHash] = id;
        return id;
    }

    int allocTemp(DType t) {
        int id = (int)tempSeq++;
        maxLiveTemps = std::max(maxLiveTemps, tempSeq);
        const char *ct = t == DType::F64 ? "double"
                       : t == DType::STR ? "uint32_t" : "int64_t";
        fb << "    " << ct << " *restrict t" << id
           << " = (" << ct << "*)qex_scratch_align(a, &off, bs*sizeof("
           << ct << "));\n";
        fb << "    uint64_t *restrict nt" << id
           << " = (uint64_t*)qex_scratch_align(a, &off, "
              "QEX_WORDS(QEX_BATCH)*sizeof(uint64_t));\n";
        return id;
    }

    // Emit expression n into a temp; return its temp id.
    // strNs is the string column that supplies dictionary ids for any
    // LitS inside a string compare/LIKE subtree.
    int emitFilter(const Node *n, int strNs = -1) {
        auto it = temps.find(n->fullHash);
        if (it != temps.end()) return it->second;

        if (n->kind == NK::Col) {
            usedCols.insert(n->col);
            int id = allocTemp(n->type);
            const char *acc = n->type == DType::I64 ? "i64"
                            : n->type == DType::F64 ? "f64" : "sid";
            fb << "    for (uint32_t i = 0; i < bs; ++i) t" << id
               << "[i] = " << cv(n->col) << "->" << acc << "[bi+i];\n";
            fb << "    for (uint32_t w = 0; w < QEX_WORDS(QEX_BATCH); ++w) nt"
               << id << "[w] = (w*64 < bs) ? " << cv(n->col)
               << "->nullbm[basew+w] : 0;\n";
            temps[n->fullHash] = id;
            return id;
        }
        if (n->kind == NK::LitI || n->kind == NK::LitD) {
            int cid = bindConst(const_cast<Node*>(n));
            int id = allocTemp(n->type);
            const char *acc = n->kind == NK::LitI ? "i64" : "f64";
            fb << "    for (uint32_t i = 0; i < bs; ++i) t" << id
               << "[i] = a->consts[" << cid << "]." << acc << ";\n";
            fb << "    memset(nt" << id
               << ", 0, QEX_WORDS(QEX_BATCH)*sizeof(uint64_t));\n";
            temps[n->fullHash] = id;
            return id;
        }
        if (n->kind == NK::LitS) {
            int cid = bindConst(const_cast<Node*>(n));
            int id = allocTemp(DType::STR);
            pl.sconsts.push_back({cid, strNs, n->sv});
            fb << "    for (uint32_t i = 0; i < bs; ++i) t" << id
               << "[i] = a->litsid[" << cid << "];\n";
            fb << "    for (uint32_t w = 0; w < QEX_WORDS(QEX_BATCH); ++w) nt"
               << id << "[w] = (w*64 < bs) && a->litsid[" << cid
               << "] == UINT32_MAX ? (~0ull) : 0;\n";
            temps[n->fullHash] = id;
            return id;
        }

        int id = allocTemp(n->type);
        switch (n->kind) {
            case NK::Neg: {
                int x = emitFilter(n->a, strNs);
                fb << "    for (uint32_t i = 0; i < bs; ++i) t" << id
                   << "[i] = -t" << x << "[i];\n";
                fb << "    memcpy(nt" << id << ", nt" << x
                   << ", QEX_WORDS(QEX_BATCH)*sizeof(uint64_t));\n";
                break;
            }
            case NK::Add: case NK::Sub: case NK::Mul:
            case NK::Div: case NK::Mod: {
                int x = emitFilter(n->a, strNs), y = emitFilter(n->b, strNs);
                const char *op = n->kind == NK::Add ? "+" :
                                 n->kind == NK::Sub ? "-" :
                                 n->kind == NK::Mul ? "*" :
                                 n->kind == NK::Div ? "/" : "%";
                fb << "    for (uint32_t i = 0; i < bs; ++i) {\n";
                if (n->kind == NK::Div || n->kind == NK::Mod)
                    fb << "        if (t" << y << "[i] == 0) { t" << id
                       << "[i] = 0; continue; }\n";
                fb << "        t" << id << "[i] = t" << x << "[i] " << op
                   << " t" << y << "[i];\n    }\n";
                fb << "    for (uint32_t w = 0; w < QEX_WORDS(QEX_BATCH); ++w)\n"
                   << "        nt" << id << "[w] = nt" << x << "[w] | nt"
                   << y << "[w];\n";
                if (n->kind == NK::Div || n->kind == NK::Mod) {
                    fb << "    for (uint32_t i = 0; i < bs; ++i)\n"
                       << "        if (t" << y << "[i] == 0) nt" << id
                       << "[i>>6] |= 1ull << (i&63);\n";
                }
                break;
            }
            case NK::Not: {
                int x = emitFilter(n->a, strNs);
                fb << "    for (uint32_t i = 0; i < bs; ++i) t" << id
                   << "[i] = t" << x << "[i] ? 0 : 1;\n";
                fb << "    memcpy(nt" << id << ", nt" << x
                   << ", QEX_WORDS(QEX_BATCH)*sizeof(uint64_t));\n";
                break;
            }
            case NK::And: case NK::Or: {
                int x = emitFilter(n->a, strNs), y = emitFilter(n->b, strNs);
                if (n->kind == NK::And) {
                    fb << "    for (uint32_t i = 0; i < bs; ++i) t" << id
                       << "[i] = t" << x << "[i] & t" << y << "[i];\n";
                    fb << "    for (uint32_t w = 0; w < QEX_WORDS(QEX_BATCH); ++w) {\n"
                       << "        uint64_t fx = (~t" << x << "[w*64] * 0); (void)fx;\n"
                       << "        uint64_t falsew = 0, truew = 0;\n"
                       << "        for (uint32_t b = 0; b < 64 && w*64+b < bs; ++b) {\n"
                       << "            uint32_t i = w*64+b;\n"
                       << "            if (!t" << x << "[i] && !qex_bget(nt" << x
                       << ",i)) falsew |= 1ull<<b;\n"
                       << "            if (!t" << y << "[i] && !qex_bget(nt" << y
                       << ",i)) falsew |= 1ull<<b;\n"
                       << "        }\n"
                       << "        nt" << id << "[w] = (nt" << x << "[w] | nt"
                       << y << "[w]) & ~falsew;\n    }\n";
                } else {
                    fb << "    for (uint32_t i = 0; i < bs; ++i) t" << id
                       << "[i] = t" << x << "[i] | t" << y << "[i];\n";
                    fb << "    for (uint32_t w = 0; w < QEX_WORDS(QEX_BATCH); ++w) {\n"
                       << "        uint64_t truew = 0;\n"
                       << "        for (uint32_t b = 0; b < 64 && w*64+b < bs; ++b) {\n"
                       << "            uint32_t i = w*64+b;\n"
                       << "            if (t" << x << "[i] && !qex_bget(nt" << x
                       << ",i)) truew |= 1ull<<b;\n"
                       << "            if (t" << y << "[i] && !qex_bget(nt" << y
                       << ",i)) truew |= 1ull<<b;\n"
                       << "        }\n"
                       << "        nt" << id << "[w] = (nt" << x << "[w] | nt"
                       << y << "[w]) & ~truew;\n    }\n";
                }
                break;
            }
            case NK::Eq: case NK::Ne: case NK::Lt: case NK::Le:
            case NK::Gt: case NK::Ge:
                emitCompare(n, id);
                break;
            case NK::Like:
                emitStringPred(n, id);
                break;
            case NK::IsNull: case NK::NotNull: {
                int x = emitFilter(n->a, strNs);
                if (n->kind == NK::IsNull)
                    fb << "    for (uint32_t i = 0; i < bs; ++i) t" << id
                       << "[i] = qex_bget(nt" << x << ", i);\n";
                else
                    fb << "    for (uint32_t i = 0; i < bs; ++i) t" << id
                       << "[i] = qex_bget(nt" << x << ", i) ? 0 : 1;\n";
                fb << "    memset(nt" << id
                   << ", 0, QEX_WORDS(QEX_BATCH)*sizeof(uint64_t));\n";
                break;
            }
            default: break;
        }
        if (n->shared) ++cseEmitted;
        temps[n->fullHash] = id;
        return id;
    }

    // Find the string column referenced inside an expression subtree.
    static int findStrCol(const Node *n) {
        if (!n) return -1;
        std::vector<const Node*> st{n};
        while (!st.empty()) {
            const Node *x = st.back(); st.pop_back();
            if (x->kind == NK::Col && x->type == DType::STR) return x->col;
            if (x->a) st.push_back(x->a);
            if (x->b) st.push_back(x->b);
        }
        return -1;
    }

    void emitCompare(const Node *n, int id) {
        if (n->cmpType == DType::STR) {
            int strNs = findStrCol(n);
            // Equality / inequality ride dictionary bitmaps (one bit per
            // dict id); ordering compares dict ids, which are assigned in
            // lexicographic order by the table builder.
            if (n->kind == NK::Eq || n->kind == NK::Ne) {
                Node like = *n;
                like.kind = NK::Like;
                like.like = LikeMode::Exact;
                if (n->b->kind == NK::LitS) like.sv = n->b->sv;
                emitStringPred(&like, id);
                if (n->kind == NK::Ne) {
                    fb << "    for (uint32_t i = 0; i < bs; ++i) t" << id
                       << "[i] = t" << id << "[i] ? 0 : 1;\n";
                }
                return;
            }
            const char *op = n->kind == NK::Lt ? "<" :
                             n->kind == NK::Le ? "<=" :
                             n->kind == NK::Gt ? ">" : ">=";
            int x = emitFilter(n->a, strNs), y = emitFilter(n->b, strNs);
            fb << "    for (uint32_t i = 0; i < bs; ++i) t" << id
               << "[i] = (t" << x << "[i] " << op << " t" << y
               << "[i]);\n";
            fb << "    for (uint32_t w = 0; w < QEX_WORDS(QEX_BATCH); ++w) nt"
               << id << "[w] = nt" << x << "[w] | nt" << y << "[w];\n";
            return;
        }
        const char *op = n->kind == NK::Eq ? "==" :
                         n->kind == NK::Ne ? "!=" :
                         n->kind == NK::Lt ? "<" :
                         n->kind == NK::Le ? "<=" :
                         n->kind == NK::Gt ? ">" : ">=";
        int x = emitFilter(n->a), y = emitFilter(n->b);
        fb << "    #pragma clang loop vectorize(enable)\n";
        fb << "    for (uint32_t i = 0; i < bs; ++i) t" << id
           << "[i] = (t" << x << "[i] " << op << " t" << y << "[i]);\n";
        fb << "    for (uint32_t w = 0; w < QEX_WORDS(QEX_BATCH); ++w) nt"
           << id << "[w] = nt" << x << "[w] | nt" << y << "[w];\n";
    }

    void emitStringPred(const Node *n, int id) {
        int col = n->a->col;
        usedCols.insert(col);
        size_t sp = pl.spreds.size();
        pl.spreds.push_back({col, sp, n->sv, n->like});
        fb << "    for (uint32_t i = 0; i < bs; ++i) {\n"
           << "        uint32_t d = " << cv(col) << "->sid[bi+i];\n"
           << "        t" << id
           << "[i] = (d != UINT32_MAX) && qex_bget(a->spred + "
           << sp << " * a->spred_nwords, d);\n    }\n";
        fb << "    for (uint32_t w = 0; w < QEX_WORDS(QEX_BATCH); ++w) nt"
           << id << "[w] = (w*64 < bs) ? " << cv(col)
           << "->nullbm[basew+w] : 0;\n";
    }

    // ---------------- projection ----------------
    std::string scalar(const Node *n, const std::string &idx) {
        switch (n->kind) {
            case NK::Col: {
                const char *acc = n->type == DType::I64 ? "i64"
                                : n->type == DType::F64 ? "f64" : "sid";
                usedCols.insert(n->col);
                return cv(n->col) + "->" + acc + "[" + idx + "]";
            }
            case NK::LitI:
                return "a->consts[" +
                       std::to_string(bindConst(const_cast<Node*>(n))) + "].i64";
            case NK::LitD:
                return "a->consts[" +
                       std::to_string(bindConst(const_cast<Node*>(n))) + "].f64";
            default:
                return "pt" + std::to_string((uintptr_t)n) + "[" + idx + "]";
        }
    }

    std::string nullExpr(const Node *n, const std::string &idx = "r") {
        switch (n->kind) {
            case NK::Col:
                return "qex_bget(" + cv(n->col) + "->nullbm, " + idx + ")";
            case NK::LitI: case NK::LitD: case NK::LitS:
                return "0";
            case NK::IsNull: case NK::NotNull:
                return "0";
            case NK::Neg: case NK::Not:
                return nullExpr(n->a, idx);
            case NK::Div: case NK::Mod:
                return "(" + nullExpr(n->a, idx) + " || " +
                       nullExpr(n->b, idx) + " || " +
                       scalar(n->b, idx) + " == 0)";
            default:
                if (n->a && n->b)
                    return "(" + nullExpr(n->a, idx) + " || " +
                           nullExpr(n->b, idx) + ")";
                if (n->a) return nullExpr(n->a, idx);
                return "0";
        }
    }

    void emitProjectOne(const Node *n, size_t outIdx) {
        // allocate temps for non-leaf nodes postorder
        std::vector<const Node*> order;
        {
            std::vector<const Node*> st{n};
            while (!st.empty()) {
                const Node *x = st.back(); st.pop_back();
                if (x->kind == NK::Col || x->kind == NK::LitI ||
                    x->kind == NK::LitD || x->kind == NK::LitS) continue;
                order.push_back(x);
                if (x->a) st.push_back(x->a);
                if (x->b) st.push_back(x->b);
            }
        }
        std::map<u64, std::string> &named = projNamed;
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            const Node *x = *it;
            if (named.count(x->fullHash)) continue;   // CSE: reuse temp
            std::string nm = "pt" + std::to_string(projSeq++);
            named[x->fullHash] = nm;
            const char *ct = x->type == DType::F64 ? "double" : "int64_t";
            pb << "    " << ct << " *restrict " << nm
               << " = (" << ct << "*)qex_scratch_align_p(a, &poff, a->nsel*sizeof("
               << ct << "));\n";
            auto sv = [&](const Node *m) -> std::string {
                if (m->kind == NK::Col || m->kind == NK::LitI ||
                    m->kind == NK::LitD)
                    return scalar(m, "r");
                return named[m->fullHash] + "[i]";
            };
            switch (x->kind) {
                case NK::Neg:
                    pb << "    for (uint32_t i = 0; i < a->nsel; ++i) { uint32_t r = a->sel[i]; "
                       << nm << "[i] = " << nullExpr(x->a, "r") << " ? 0 : -"
                       << sv(x->a) << "; }\n";
                    break;
                case NK::Add: case NK::Sub: case NK::Mul:
                case NK::Div: case NK::Mod: {
                    const char *op = x->kind == NK::Add ? "+" :
                                     x->kind == NK::Sub ? "-" :
                                     x->kind == NK::Mul ? "*" :
                                     x->kind == NK::Div ? "/" : "%";
                    pb << "    for (uint32_t i = 0; i < a->nsel; ++i) { uint32_t r = a->sel[i]; "
                       << nm << "[i] = (" << nullExpr(x->a, "r") << " || "
                       << nullExpr(x->b, "r") << ") ? 0 : ("
                       << sv(x->a) << " " << op << " " << sv(x->b) << "); }\n";
                    break;
                }
                case NK::Eq: case NK::Ne: case NK::Lt: case NK::Le:
                case NK::Gt: case NK::Ge: {
                    const char *op = x->kind == NK::Eq ? "==" :
                                     x->kind == NK::Ne ? "!=" :
                                     x->kind == NK::Lt ? "<" :
                                     x->kind == NK::Le ? "<=" :
                                     x->kind == NK::Gt ? ">" : ">=";
                    pb << "    for (uint32_t i = 0; i < a->nsel; ++i) { uint32_t r = a->sel[i]; "
                       << nm << "[i] = (" << nullExpr(x->a, "r") << " || "
                       << nullExpr(x->b, "r") << ") ? 0 : ("
                       << sv(x->a) << " " << op << " " << sv(x->b) << "); }\n";
                    break;
                }
                case NK::Not:
                    pb << "    for (uint32_t i = 0; i < a->nsel; ++i) { uint32_t r = a->sel[i]; "
                       << nm << "[i] = " << sv(x->a) << " ? 0 : 1; }\n";
                    break;
                case NK::IsNull:
                    pb << "    for (uint32_t i = 0; i < a->nsel; ++i) { uint32_t r = a->sel[i]; "
                       << nm << "[i] = " << nullExpr(x->a, "r") << "; }\n";
                    break;
                case NK::NotNull:
                    pb << "    for (uint32_t i = 0; i < a->nsel; ++i) { uint32_t r = a->sel[i]; "
                       << nm << "[i] = " << nullExpr(x->a, "r") << " ? 0 : 1; }\n";
                    break;
                case NK::Like: {
                    size_t sp = pl.spreds.size();
                    pl.spreds.push_back({x->a->col, sp, x->sv, x->like});
                    usedCols.insert(x->a->col);
                    pb << "    for (uint32_t i = 0; i < a->nsel; ++i) {\n"
                       << "        uint32_t r = a->sel[i], d = "
                       << cv(x->a->col) << "->sid[r];\n"
                       << "        " << nm
                       << "[i] = (d != UINT32_MAX) && qex_bget(a->spred + "
                       << sp << " * a->spred_nwords, d); }\n";
                    break;
                }
                default: break;
            }
        }

        if (n->type == DType::STR && n->kind == NK::Col) {
            usedCols.insert(n->col);
            // emit dictionary ids directly: no char* materialization, no
            // reverse lookup on the host side.
            pb << "    for (uint32_t i = 0; i < a->nsel; ++i) {\n"
               << "        uint32_t r = a->sel[i];\n"
               << "        a->out_sid[" << outIdx
               << " * (size_t)a->nsel + i] = " << cv(n->col)
               << "->sid[r];\n"
               << "        if (qex_bget(" << cv(n->col)
               << "->nullbm, r)) a->out_null[" << outIdx
               << " * ((a->nsel+63)/64) + (i>>6)] |= 1ull<<(i&63);\n    }\n";
            pb << "    a->out_type[" << outIdx << "] = QEX_STR;\n";
            return;
        }

        std::string val = (n->kind == NK::Col || n->kind == NK::LitI ||
                           n->kind == NK::LitD)
                              ? scalar(n, "r")
                              : named[n->fullHash] + "[i]";
        const char *arr = n->type == DType::F64 ? "out_f64" : "out_i64";
        pb << "    for (uint32_t i = 0; i < a->nsel; ++i) {\n"
           << "        uint32_t r = a->sel[i];\n"
           << "        a->" << arr << "[" << outIdx
           << " * (size_t)a->nsel + i] = " << val << ";\n"
           << "        if (" << nullExpr(n, "r")
           << ") a->out_null[" << outIdx
           << " * ((a->nsel+63)/64) + (i>>6)] |= 1ull << (i&63);\n    }\n";
        pb << "    a->out_type[" << outIdx << "] = "
           << (n->type == DType::F64 ? "QEX_F64" : "QEX_I64") << ";\n";
    }
};

const char *PROJ_HELPER = R"RAW(
static inline void *qex_scratch_align_p(qex_project_args *a, size_t *off,
                                        size_t bytes) {
    size_t aligned = (*off + 63u) & ~(size_t)63u;
    if (aligned + bytes > a->dlen) return NULL;
    void *p = (char *)a->scratch + aligned;
    *off = aligned + bytes;
    return p;
}
)RAW";

} // namespace

std::string emit_translation_unit(const Query &q, JitPlan &pl) {
    CG cg(q, pl);
    for (Node *n : q.all_nodes())
        if (n->kind == NK::Col) cg.usedCols.insert(n->col);

    std::ostringstream out;
    out << "/* qex JIT generated code */\n";
    out << "#include \"qex_runtime.h\"\n";
    out << PROJ_HELPER;

    // ---------------- filter
    out << "int qex_filter(qex_filter_args *a) {\n";
    out << "    uint32_t bi = 0;\n";
    out << "    while (bi < (uint32_t)a->nrows) {\n";
    out << "        uint32_t bs = (uint32_t)((a->nrows - bi) < QEX_BATCH ? a->nrows - bi : QEX_BATCH);\n";
    out << "        size_t basew = (size_t)(bi >> 6);\n";
    out << "        size_t off = 0;\n";
    for (int c : cg.usedCols)
        out << "        const qex_col *" << CG::cv(c)
            << " = a->cols[" << c << "];\n";

    int root = cg.emitFilter(q.where);
    out << cg.fb.str();
    out << "        for (uint32_t w = 0; w < QEX_WORDS(QEX_BATCH); ++w) {\n";
    out << "            if (w*64 >= bs) break;\n";
    out << "            uint64_t vals = 0, valid = 0;\n";
    out << "            uint32_t lim = bs - w*64 < 64 ? bs - w*64 : 64;\n";
    out << "            for (uint32_t b = 0; b < lim; ++b) {\n";
    out << "                uint32_t i = w*64+b;\n";
    out << "                vals |= ((uint64_t)(t" << root
        << "[i] != 0)) << b;\n";
    out << "                valid |= 1ull << b;\n";
    out << "            }\n";
    out << "            a->out[basew+w] |= vals & ~nt" << root
        << "[w] & valid;\n";
    out << "        }\n";
    out << "        bi += bs;\n";
    out << "    }\n";
    out << "    return 0;\n";
    out << "}\n\n";

    // ---------------- projection
    out << "int qex_project(qex_project_args *a) {\n";
    out << "    size_t poff = 0;\n";
    for (int c : cg.usedCols)
        out << "    const qex_col *" << CG::cv(c)
            << " = a->cols[" << c << "];\n";
    for (size_t k = 0; k < q.proj.size(); ++k)
        cg.emitProjectOne(q.proj[k], k);
    out << cg.pb.str();
    out << "    return 0;\n";
    out << "}\n";

    pl.usedCols.assign(cg.usedCols.begin(), cg.usedCols.end());
    std::sort(pl.usedCols.begin(), pl.usedCols.end());
    pl.tempCount = cg.tempSeq;
    pl.cseCount = cg.cseEmitted;

    // structural hash: AST shapes only (literals excluded by expr.cpp)
    u64 h = q.where->structHash;
    for (Node *e : q.proj) h = fnv1a64_u64(e->structHash, h);
    pl.structHash = h;
    u64 fh = q.where->fullHash;
    for (Node *e : q.proj) fh = fnv1a64_u64(e->fullHash, fh);
    pl.fullHash = fh;

    // generous scratch: per-batch temps + projection temps over nsel
    size_t perBatchTemp = QEX_BATCH * 16 + QEX_WORDS(QEX_BATCH) * 8;
    pl.scratchBytes = (cg.tempSeq + 8) * perBatchTemp + (1u << 18);
    return out.str();
}

} // namespace qex
