#include "optimizer.h"

#include <algorithm>
#include <map>
#include <set>

namespace qex {

void Stats::build(const Table &t) {
    cols.clear();
    cols.resize(t.cols.size());
    for (size_t i = 0; i < t.cols.size(); ++i) {
        const Column &c = *t.cols[i];
        ColStat &s = cols[i];
        s.minI = c.minI; s.maxI = c.maxI;
        s.minD = c.minD; s.maxD = c.maxD;
        s.distinct = c.distinct;
        s.nullFrac = t.nrows ? (double)bcount(c.nullbm.data(), t.nrows) /
                                    (double)t.nrows
                             : 0.0;
    }
}

// ------------------------------------------------------------- CSE pass
static std::vector<Node*> postorder(Node *root) {
    std::vector<Node*> st{root}, order;
    while (!st.empty()) {
        Node *n = st.back(); st.pop_back();
        order.push_back(n);
        if (n->a) st.push_back(n->a);
        if (n->b) st.push_back(n->b);
    }
    std::reverse(order.begin(), order.end());
    return order;
}

static void cse_pass(Node *root, OptimizeReport &rep) {
    auto order = postorder(root);

    // Count occurrences of each full-hash value.
    std::map<u64, int> freq;
    for (Node *n : order) ++freq[n->fullHash];

    size_t nextId = 1;
    std::map<u64, size_t> idFor;
    std::set<u64> reported;
    for (Node *n : order) {
        bool interesting =
            n->kind != NK::Col && n->kind != NK::LitI &&
            n->kind != NK::LitD && n->kind != NK::LitS;
        if (interesting && freq[n->fullHash] > 1) {
            n->shared = true;
            auto it = idFor.find(n->fullHash);
            if (it == idFor.end()) {
                n->cseId = nextId++;
                idFor[n->fullHash] = n->cseId;
                rep.cseList.push_back(dump_node(n));
                reported.insert(n->fullHash);
            } else {
                n->cseId = it->second;
            }
        }
    }
    rep.cseCount = idFor.size();
}

// --------------------------------------------------- AND selectivity
static double clamp01(double x) { return std::max(0.0, std::min(1.0, x)); }

double selectivity(const Node *n, const Stats &st) {
    if (!n) return 1.0;
    switch (n->kind) {
        case NK::LitI: return n->iv ? 1.0 : 0.0;
        case NK::And:
            return selectivity(n->a, st) * selectivity(n->b, st);
        case NK::Or: {
            double x = selectivity(n->a, st), y = selectivity(n->b, st);
            return clamp01(x + y - x * y);
        }
        case NK::Not:
            return 1.0 - selectivity(n->a, st);
        case NK::IsNull: {
            if (n->a->kind == NK::Col)
                return st.cols[n->a->col].nullFrac;
            return 0.05;
        }
        case NK::NotNull: {
            if (n->a->kind == NK::Col)
                return 1.0 - st.cols[n->a->col].nullFrac;
            return 0.95;
        }
        case NK::Like:
            switch (n->like) {
                case LikeMode::Exact:    return 1.0 / std::max<i64>(1, st.cols[n->a->col].distinct);
                case LikeMode::Prefix:   return 0.05;
                case LikeMode::Contains: return 0.10;
                case LikeMode::Glob:     return 0.15;
            }
            return 0.1;
        case NK::Eq: case NK::Ne: {
            double base = 1.0 / std::max<i64>(1, st.cols[n->a->col].distinct);
            if (n->kind == NK::Ne) base = 1.0 - base;
            if (n->a->kind == NK::Col) {
                base *= (1.0 - st.cols[n->a->col].nullFrac);
            }
            return clamp01(base);
        }
        case NK::Lt: case NK::Le: case NK::Gt: case NK::Ge: {
            if (n->a->kind != NK::Col) return 0.3;
            const auto &s = st.cols[n->a->col];
            double frac = 0.3;
            auto numFrac = [&](double v) {
                if (s.distinct <= 1) return 0.5;
                if (s.minD != s.maxD || s.minI != s.maxI ||
                    n->a->type == DType::F64) {
                    if (s.maxD <= s.minD) return 0.5;
                    return clamp01((v - s.minD) / (s.maxD - s.minD));
                }
                if (s.maxI <= s.minI) return 0.5;
                return clamp01((v - (double)s.minI) /
                               (double)(s.maxI - s.minI));
            };
            double v = n->b->kind == NK::LitI ? (double)n->b->iv
                                              : n->b->dv;
            double f = numFrac(v);
            switch (n->kind) {
                case NK::Lt: frac = f; break;
                case NK::Le: frac = std::min(1.0, f + 1.0 / s.distinct); break;
                case NK::Gt: frac = 1.0 - f; break;
                case NK::Ge: frac = 1.0 - std::max(0.0, f - 1.0 / s.distinct); break;
                default: break;
            }
            return clamp01(frac * (1.0 - s.nullFrac));
        }
        default:
            return 0.5;  // arithmetic-heavy predicates: unknown
    }
}

static Node *reorder_and(Node *root, const Stats &st, OptimizeReport &rep) {
    // recurse first
    if (root->a) root->a = reorder_and(root->a, st, rep);
    if (root->b) root->b = reorder_and(root->b, st, rep);
    if (root->kind != NK::And) return root;

    // Flatten the AND chain, collecting both term nodes and the AND nodes
    // that hold them (the tree has exactly terms-1 AND nodes to reuse).
    std::vector<Node*> terms;
    std::vector<Node*> andNodes;
    std::vector<Node*> stack{root};
    while (!stack.empty()) {
        Node *n = stack.back(); stack.pop_back();
        if (n->kind == NK::And) {
            andNodes.push_back(n);
            stack.push_back(n->a);
            stack.push_back(n->b);
        } else {
            terms.push_back(n);
        }
    }

    std::vector<double> sel(terms.size());
    for (size_t i = 0; i < terms.size(); ++i)
        sel[i] = selectivity(terms[i], st);

    std::vector<size_t> idx(terms.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::stable_sort(idx.begin(), idx.end(),
                     [&](size_t x, size_t y) { return sel[x] < sel[y]; });

    bool changed = false;
    for (size_t i = 0; i < idx.size(); ++i)
        if (idx[i] != i) changed = true;
    if (changed) {
        ++rep.andReorders;
        std::string before, after;
        for (size_t i = 0; i < terms.size(); ++i) {
            if (i) { before += " AND "; after += " AND "; }
            before += dump_node(terms[i]);
            after += dump_node(terms[idx[i]]);
        }
        rep.orderList.push_back(before + "  ==>  " + after);
    }

    // Rebuild a left-deep chain by reusing the existing AND arena nodes.
    Node *chain = terms[idx[0]];
    for (size_t i = 1; i < terms.size(); ++i) {
        Node *an = andNodes[i - 1];
        an->a = chain;
        an->b = terms[idx[i]];
        chain = an;
    }
    return chain;
}

OptimizeReport optimize_query(Query &q, const Stats &st) {
    OptimizeReport rep;
    for (Node *n : q.all_nodes()) { n->shared = false; n->cseId = 0; }
    for (Node *p : q.proj) cse_pass(p, rep);
    cse_pass(q.where, rep);
    // CSE ids must be query-unique; renumber across the whole query.
    {
        std::map<u64, size_t> ids;
        size_t nextId = 1;
        for (Node *n : q.all_nodes()) {
            if (!n->shared) continue;
            auto it = ids.find(n->fullHash);
            if (it == ids.end()) { n->cseId = nextId; ids[n->fullHash] = nextId++; }
            else n->cseId = it->second;
        }
        rep.cseCount = ids.size();
    }
    for (Node *&p : q.proj) p = reorder_and(p, st, rep);
    q.where = reorder_and(q.where, st, rep);
    // hashes stay valid: reordering only changes AND topology hashes,
    // recompute to keep everything consistent.
    for (Node *p : q.proj) compute_hashes(p);
    compute_hashes(q.where);
    return rep;
}

} // namespace qex
