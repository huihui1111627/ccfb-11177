#include "interpreter.h"
#include "qex_runtime.h"

#include <cmath>
#include <iostream>

namespace qex {

// ------------------------------------------------ virtual-dispatch nodes
// Every expression node becomes one object whose eval() is a virtual call.
// The baseline engine pays one indirect branch per node, per row — exactly
// the cost profile the JIT path is meant to eliminate.
class ExprBase {
public:
    virtual ~ExprBase() = default;
    virtual Val eval(size_t row) = 0;
};
using ExprPtr = std::unique_ptr<ExprBase>;

class LiteralI : public ExprBase {
    i64 v_;
public:
    explicit LiteralI(i64 v) : v_(v) {}
    Val eval(size_t) override { return Val::from_i(v_); }
};
class LiteralD : public ExprBase {
    double v_;
public:
    explicit LiteralD(double v) : v_(v) {}
    Val eval(size_t) override { return Val::from_d(v_); }
};
class LiteralS : public ExprBase {
    int col_;       // dictionary namespace of the compared column (-1=any)
    u32 sid_;
public:
    LiteralS(int col, u32 sid) : col_(col), sid_(sid) {}
    Val eval(size_t) override {
        Val v = Val::from_s(sid_);
        (void)col_;
        return v;
    }
    int dictCol() const { return col_; }
};
class ColumnRef : public ExprBase {
    int col_;
    const Column &c_;
public:
    explicit ColumnRef(int col, const Column &c) : col_(col), c_(c) {}
    Val eval(size_t row) override {
        if (c_.isnull(row)) return Val::null_val(c_.type);
        if (c_.type == DType::I64) return Val::from_i(c_.i64v[row]);
        if (c_.type == DType::F64) return Val::from_d(c_.f64v[row]);
        Val v = Val::from_s(c_.sid[row]);
        (void)col_;
        return v;
    }
    int dictCol() const { return col_;
};
};

struct Ctx {
    const Table &tbl;
    std::unordered_map<u64, ExprPtr> cse;  // shared subexpression objects
};

static ExprPtr build(const Node *n, Ctx &ctx);

class IsNullExpr : public ExprBase {
    ExprPtr a_;
public:
    explicit IsNullExpr(ExprPtr a) : a_(std::move(a)) {}
    Val eval(size_t r) override { return Val::from_i(a_->eval(r).isnull ? 1 : 0); }
};
class NotNullExpr : public ExprBase {
    ExprPtr a_;
public:
    explicit NotNullExpr(ExprPtr a) : a_(std::move(a)) {}
    Val eval(size_t r) override { return Val::from_i(a_->eval(r).isnull ? 0 : 1); }
};
class NotExpr : public ExprBase {
    ExprPtr a_;
public:
    explicit NotExpr(ExprPtr a) : a_(std::move(a)) {}
    Val eval(size_t r) override {
        Val x = a_->eval(r);
        if (x.isnull) return Val::null_val(DType::I64);
        return Val::from_i(x.i ? 0 : 1);
    }
};
class NegExpr : public ExprBase {
    ExprPtr a_;
public:
    explicit NegExpr(ExprPtr a) : a_(std::move(a)) {}
    Val eval(size_t r) override {
        Val x = a_->eval(r);
        if (x.isnull) return Val::null_val(x.type);
        return x.type == DType::F64 ? Val::from_d(-x.d) : Val::from_i(-x.i);
    }
};

// boolean AND/OR with SQL 3VL
static Val logic3(NK k, const Val &x, const Val &y) {
    auto truth = [](const Val &v) -> int {
        if (v.isnull) return -1;       // unknown
        return v.i != 0 ? 1 : 0;
    };
    int a = truth(x), b = truth(y);
    if (k == NK::And) {
        if (a == 0 || b == 0) return Val::from_i(0);
        if (a == 1 && b == 1) return Val::from_i(1);
        return Val::null_val(DType::I64);
    } else {
        if (a == 1 || b == 1) return Val::from_i(1);
        if (a == 0 && b == 0) return Val::from_i(0);
        return Val::null_val(DType::I64);
    }
}

class BoolOp : public ExprBase {
    NK k_; ExprPtr a_, b_;
public:
    BoolOp(NK k, ExprPtr a, ExprPtr b)
        : k_(k), a_(std::move(a)), b_(std::move(b)) {}
    Val eval(size_t r) override {
        Val x = a_->eval(r);
        // short-circuit where 3VL allows
        if (k_ == NK::And && !x.isnull && x.i == 0) return Val::from_i(0);
        if (k_ == NK::Or && !x.isnull && x.i != 0) return Val::from_i(1);
        Val y = b_->eval(r);
        return logic3(k_, x, y);
    }
};

class ArithOp : public ExprBase {
    NK k_; ExprPtr a_, b_;
public:
    ArithOp(NK k, ExprPtr a, ExprPtr b)
        : k_(k), a_(std::move(a)), b_(std::move(b)) {}
    Val eval(size_t r) override {
        Val x = a_->eval(r), y = b_->eval(r);
        if (x.isnull || y.isnull) return Val::null_val(x.isnull ? x.type : y.type);
        if (x.type == DType::F64 || y.type == DType::F64) {
            double a = x.as_f64(), b = y.as_f64();
            switch (k_) {
                case NK::Add: return Val::from_d(a + b);
                case NK::Sub: return Val::from_d(a - b);
                case NK::Mul: return Val::from_d(a * b);
                case NK::Div:
                    if (b == 0.0) return Val::null_val(DType::F64);
                    return Val::from_d(a / b);
                default: return Val::null_val(DType::F64);
            }
        }
        switch (k_) {
            case NK::Add: return Val::from_i(x.i + y.i);
            case NK::Sub: return Val::from_i(x.i - y.i);
            case NK::Mul: return Val::from_i(x.i * y.i);
            case NK::Div:
                if (y.i == 0) return Val::null_val(DType::I64);
                return Val::from_i(x.i / y.i);
            case NK::Mod:
                if (y.i == 0) return Val::null_val(DType::I64);
                return Val::from_i(x.i % y.i);
            default: return Val::null_val(DType::I64);
        }
    }
};

static int cmp_num(NK k, double a, double b) {
    bool res;
    switch (k) {
        case NK::Eq: res = a == b; break;
        case NK::Ne: res = a != b; break;
        case NK::Lt: res = a < b;  break;
        case NK::Le: res = a <= b; break;
        case NK::Gt: res = a > b;  break;
        case NK::Ge: res = a >= b; break;
        default: res = false;
    }
    return res ? 1 : 0;
}

class CompareOp : public ExprBase {
    NK k_; ExprPtr a_, b_;
    const Column *scol_;   // string namespace when comparing strings
    u32 litSid_ = UINT32_MAX;  // resolved dict id of a string literal operand
    bool bIsLit_ = false;
public:
    CompareOp(NK k, ExprPtr a, ExprPtr b, const Column *scol,
             u32 litSid, bool bIsLit)
        : k_(k), a_(std::move(a)), b_(std::move(b)), scol_(scol),
          litSid_(litSid), bIsLit_(bIsLit) {}
    Val eval(size_t r) override {
        Val x = a_->eval(r), y = b_->eval(r);
        if (x.isnull || y.isnull) return Val::null_val(DType::I64);
        if (x.type == DType::STR) {
            u32 ys = bIsLit_ ? litSid_ : y.sid;
            if (ys == UINT32_MAX) return Val::from_i(k_ == NK::Ne ? 1 : 0);
            int c = scol_->dict[x.sid].compare(scol_->dict[ys]);
            bool res;
            switch (k_) {
                case NK::Eq: res = c == 0; break;
                case NK::Ne: res = c != 0; break;
                case NK::Lt: res = c < 0;  break;
                case NK::Le: res = c <= 0; break;
                case NK::Gt: res = c > 0;  break;
                case NK::Ge: res = c >= 0; break;
                default: res = false;
            }
            return Val::from_i(res ? 1 : 0);
        }
        return Val::from_i(cmp_num(k_, x.as_f64(), y.as_f64()));
    }
};

class LikeExpr : public ExprBase {
    ExprPtr a_;
    std::string pat_;
    LikeMode mode_;
    const Column *scol_;
    static bool match(LikeMode m, const std::string &s, const std::string &p) {
        switch (m) {
            case LikeMode::Exact:    return s == p;
            case LikeMode::Prefix: {
                std::string inner = p.substr(0, p.size() - 1);
                return qex_str_prefix(inner.data(), (int32_t)inner.size(),
                                      s.data(), (int32_t)s.size());
            }
            case LikeMode::Contains: {
                std::string inner = p.substr(1, p.size() - 2);
                return qex_str_contains(inner.data(), (int32_t)inner.size(),
                                        s.data(), (int32_t)s.size());
            }
            case LikeMode::Glob:
                return qex_str_like(p.data(), (int32_t)p.size(),
                                    s.data(), (int32_t)s.size());
        }
        return false;
    }
public:
    LikeExpr(ExprPtr a, const std::string &pat, LikeMode m, const Column *scol)
        : a_(std::move(a)), pat_(pat), mode_(m), scol_(scol) {}
    Val eval(size_t r) override {
        Val x = a_->eval(r);
        if (x.isnull) return Val::null_val(DType::I64);
        return Val::from_i(match(mode_, scol_->dict[x.sid], pat_) ? 1 : 0);
    }
};

// Shared subexpression wrapper: evaluate once per row, cache the Val.
class CseExpr : public ExprBase {
    ExprPtr inner_;
    size_t row_ = SIZE_MAX;
    Val cache_;
public:
    explicit CseExpr(ExprPtr inner) : inner_(std::move(inner)) {}
    Val eval(size_t r) override {
        if (r != row_) { row_ = r; cache_ = inner_->eval(r); }
        return cache_;
    }
};

static ExprPtr build(const Node *n, Ctx &ctx) {
    // Find the string column a string expression operates on (for dict ids).
    auto strColOf = [&](const Node *e) -> Column* {
        std::vector<const Node*> st{e};
        while (!st.empty()) {
            const Node *x = st.back(); st.pop_back();
            if (x->kind == NK::Col && x->type == DType::STR)
                return ctx.tbl.cols[x->col].get();
            if (x->a) st.push_back(x->a);
            if (x->b) st.push_back(x->b);
        }
        return nullptr;
    };
    // Dictionary namespace of a string literal: the string column of the
    // enclosing compare/LIKE node. `parent` is passed when building leaves.
    auto literalDict = [&](const Node *parent, const std::string &sv)
                           -> std::pair<Column*, u32> {
        Column *sc = parent ? strColOf(parent) : nullptr;
        if (!sc) return {nullptr, UINT32_MAX};
        auto it2 = sc->dictIndex.find(sv);
        u32 sid = it2 != sc->dictIndex.end() ? it2->second : UINT32_MAX;
        return {sc, sid};
    };

    // shared subexpressions reuse one object (still a virtual call per use)
    if (n->shared) {
        auto it = ctx.cse.find(n->fullHash);
        if (it != ctx.cse.end()) {
            // caller needs an ExprPtr to the same object: hand out a
            // non-owning alias through a small holder
            struct Alias : ExprBase {
                CseExpr *t;
                explicit Alias(CseExpr *x) : t(x) {}
                Val eval(size_t r) override { return t->eval(r); }
            };
            return ExprPtr(new Alias(static_cast<CseExpr*>(it->second.get())));
        }
    }
    ExprPtr made;
    switch (n->kind) {
        case NK::LitI: made = std::make_unique<LiteralI>(n->iv); break;
        case NK::LitD: made = std::make_unique<LiteralD>(n->dv); break;
        case NK::LitS: {
            // LiteralS is only reachable inside a string compare/LIKE whose
            // column dictionary resolves the id; a free-standing string has
            // no dict id and is treated as null-equivalent (never matches).
            u32 sid = UINT32_MAX;
            made = std::make_unique<LiteralS>(-1, sid);
            (void)literalDict;
            break;
        }
        case NK::Col:
            made = std::make_unique<ColumnRef>(n->col, *ctx.tbl.cols[n->col]);
            break;
        case NK::IsNull: made = std::make_unique<IsNullExpr>(build(n->a, ctx)); break;
        case NK::NotNull: made = std::make_unique<NotNullExpr>(build(n->a, ctx)); break;
        case NK::Not: made = std::make_unique<NotExpr>(build(n->a, ctx)); break;
        case NK::Neg: made = std::make_unique<NegExpr>(build(n->a, ctx)); break;
        case NK::And: case NK::Or:
            made = std::make_unique<BoolOp>(n->kind, build(n->a, ctx),
                                            build(n->b, ctx));
            break;
        case NK::Add: case NK::Sub: case NK::Mul: case NK::Div: case NK::Mod:
            made = std::make_unique<ArithOp>(n->kind, build(n->a, ctx),
                                             build(n->b, ctx));
            break;
        case NK::Eq: case NK::Ne: case NK::Lt: case NK::Le:
        case NK::Gt: case NK::Ge: {
            Column *sc = n->cmpType == DType::STR ? strColOf(n) : nullptr;
            u32 litSid = UINT32_MAX;
            bool bIsLit = sc && n->b && n->b->kind == NK::LitS;
            if (bIsLit) {
                auto it2 = sc->dictIndex.find(n->b->sv);
                if (it2 != sc->dictIndex.end()) litSid = it2->second;
            }
            made = std::make_unique<CompareOp>(n->kind, build(n->a, ctx),
                                               build(n->b, ctx), sc,
                                               litSid, bIsLit);
            break;
        }
        case NK::Like:
            made = std::make_unique<LikeExpr>(build(n->a, ctx), n->sv,
                                              n->like, strColOf(n));
            break;
    }
    if (n->shared) {
        auto cse = std::make_unique<CseExpr>(std::move(made));
        CseExpr *raw = cse.get();
        ctx.cse.emplace(n->fullHash, std::move(cse));
        struct Alias : ExprBase {
            CseExpr *t;
            explicit Alias(CseExpr *x) : t(x) {}
            Val eval(size_t r) override { return t->eval(r); }
        };
        return ExprPtr(new Alias(raw));
    }
    return made;
}

// ================================================================ engine
std::vector<u32> Interpreter::filter(const Node *where) {
    Ctx ctx{tbl_, {}};
    ExprPtr root = build(where, ctx);
    std::vector<u32> out;
    out.reserve(tbl_.nrows / 4);
    for (size_t r = 0; r < tbl_.nrows; ++r) {
        Val v = root->eval(r);
        if (!v.isnull && v.i != 0) out.push_back((u32)r);
    }
    return out;
}

size_t Interpreter::count_filter(const Node *where) {
    Ctx ctx{tbl_, {}};
    ExprPtr root = build(where, ctx);
    size_t c = 0;
    for (size_t r = 0; r < tbl_.nrows; ++r) {
        Val v = root->eval(r);
        if (!v.isnull && v.i != 0) ++c;
    }
    return c;
}

ProjResult Interpreter::project(const std::vector<Node*> &proj,
                                const std::vector<u32> &sel) {
    Ctx ctx{tbl_, {}};
    std::vector<ExprPtr> roots;
    roots.reserve(proj.size());
    for (Node *p : proj) roots.push_back(build(p, ctx));

    ProjResult res;
    res.nrows = sel.size();
    res.types.resize(proj.size());
    res.i64cols.resize(proj.size());
    res.f64cols.resize(proj.size());
    res.strcols.resize(proj.size());
    res.nullbms.resize(proj.size(),
                       std::vector<u64>(nwords(sel.size()), 0));
    for (size_t k = 0; k < proj.size(); ++k) {
        res.types[k] = proj[k]->type;
        if (proj[k]->type == DType::I64) res.i64cols[k].resize(sel.size());
        else if (proj[k]->type == DType::F64) res.f64cols[k].resize(sel.size());
        else res.strcols[k].resize(sel.size());
    }
    for (size_t i = 0; i < sel.size(); ++i) {
        size_t r = sel[i];
        for (size_t k = 0; k < roots.size(); ++k) {
            Val v = roots[k]->eval(r);
            if (v.isnull) { bset(res.nullbms[k].data(), i); continue; }
            switch (res.types[k]) {
                case DType::I64: res.i64cols[k][i] = v.i; break;
                case DType::F64: res.f64cols[k][i] = v.d; break;
                case DType::STR: res.strcols[k][i] = v.sid; break;
            }
        }
    }
    return res;
}

bool proj_equal(const ProjResult &a, const ProjResult &b, size_t *firstDiff) {
    if (a.types != b.types || a.nrows != b.nrows) return false;
    for (size_t k = 0; k < a.types.size(); ++k) {
        if (a.nullbms[k] != b.nullbms[k]) {
            if (firstDiff) *firstDiff = 0;
            return false;
        }
        size_t n = a.nrows;
        switch (a.types[k]) {
            case DType::I64:
                for (size_t i = 0; i < n; ++i)
                    if (a.i64cols[k][i] != b.i64cols[k][i]) {
                        if (firstDiff) *firstDiff = i;
                        return false;
                    }
                break;
            case DType::F64:
                for (size_t i = 0; i < n; ++i) {
                    double x = a.f64cols[k][i], y = b.f64cols[k][i];
                    if (x != y && !(std::isnan(x) && std::isnan(y))) {
                        if (firstDiff) *firstDiff = i;
                        return false;
                    }
                }
                break;
            case DType::STR:
                for (size_t i = 0; i < n; ++i)
                    if (a.strcols[k][i] != b.strcols[k][i]) {
                        if (firstDiff) *firstDiff = i;
                        return false;
                    }
                break;
        }
    }
    return true;
}

} // namespace qex
