// optimizer.h - common subexpression elimination, AND reordering, stats.
#pragma once

#include "expr.h"

namespace qex {

struct OptimizeReport {
    size_t cseCount = 0;          // number of shared subexpressions
    size_t andReorders = 0;       // number of AND chains reordered
    std::vector<std::string> cseList;
    std::vector<std::string> orderList;  // before -> after, per AND chain
};

// Table statistics used for selectivity estimates.
struct Stats {
    struct ColStat {
        i64 minI = 0, maxI = 0;
        double minD = 0, maxD = 0;
        i64 distinct = 1;
        double nullFrac = 0;
    };
    std::vector<ColStat> cols;

    void build(const Table &t);
};

// Run CSE + AND reordering over every expression of the query.
OptimizeReport optimize_query(Query &q, const Stats &st);

// Selectivity estimate in [0,1], for an AND/OR top-level predicate.
double selectivity(const Node *n, const Stats &st);

} // namespace qex
