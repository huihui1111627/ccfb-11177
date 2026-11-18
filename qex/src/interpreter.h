// interpreter.h - classic row-at-a-time virtual-dispatch executor (baseline).
#pragma once

#include "expr.h"
#include "optimizer.h"

namespace qex {

// SQL three-valued-logic scalar.
struct Val {
    DType type = DType::I64;
    bool isnull = true;
    i64 i = 0;
    double d = 0;
    u32 sid = UINT32_MAX;   // string: dictionary id

    static Val null_val(DType t) { Val v; v.type = t; v.isnull = true; return v; }
    static Val from_i(i64 x) { Val v; v.type = DType::I64; v.isnull = false; v.i = x; return v; }
    static Val from_d(double x) { Val v; v.type = DType::F64; v.isnull = false; v.d = x; return v; }
    static Val from_s(u32 id) { Val v; v.type = DType::STR; v.isnull = false; v.sid = id; return v; }

    double as_f64() const { return type == DType::F64 ? d : (double)i; }
};

struct ProjResult {
    std::vector<DType> types;
    std::vector<std::vector<i64>> i64cols;
    std::vector<std::vector<double>> f64cols;
    std::vector<std::vector<u32>> strcols;
    std::vector<std::vector<u64>> nullbms;
    size_t nrows = 0;
};

class Interpreter {
public:
    explicit Interpreter(const Table &t) : tbl_(t) {}

    // Run WHERE over all rows. Returns selected row ids (sorted ascending).
    std::vector<u32> filter(const Node *where);

    // Evaluate projection expressions over the given selected rows.
    ProjResult project(const std::vector<Node*> &proj,
                       const std::vector<u32> &sel);

    // Convenience: full WHERE count without storing ids.
    size_t count_filter(const Node *where);

private:
    const Table &tbl_;
    Val eval(const Node *n, size_t row);
};

bool proj_equal(const ProjResult &a, const ProjResult &b,
                size_t *firstDiff = nullptr);

} // namespace qex
