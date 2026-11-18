// expr.h - expression AST, SQL subset parser, structural hashing, types.
#pragma once

#include "common.h"
#include <memory>

namespace qex {

enum class NK {
    LitI, LitD, LitS, Col,
    And, Or, Not,
    Eq, Ne, Lt, Le, Gt, Ge,
    Add, Sub, Mul, Div, Mod, Neg,
    Like, IsNull, NotNull
};

// LIKE pattern classification, decided once at parse time.
enum class LikeMode {
    Exact,    // 'abc'        -> equality
    Prefix,   // 'abc%'
    Contains, // '%abc%'
    Glob      // anything with '_' or multiple '%'
};

inline bool is_compare(NK k) {
    return k == NK::Eq || k == NK::Ne || k == NK::Lt || k == NK::Le ||
           k == NK::Gt || k == NK::Ge;
}
inline bool is_arith(NK k) {
    return k == NK::Add || k == NK::Sub || k == NK::Mul ||
           k == NK::Div || k == NK::Mod;
}

struct Node {
    NK kind;
    DType type = DType::I64;      // result type (bools are I64 0/1)
    DType cmpType = DType::I64;   // operand promotion type for compares/arith

    // leaf payloads
    int col = -1;
    std::string colName;
    i64 iv = 0;
    double dv = 0.0;
    std::string sv;
    LikeMode like = LikeMode::Glob;

    Node *a = nullptr, *b = nullptr;

    // hashes (filled by compute_hashes)
    u64 structHash = 0;   // structure only: same shape, different constants -> equal
    u64 fullHash = 0;     // structure + literal values + column ids

    // CSE bookkeeping (filled by optimizer)
    size_t cseId = 0;
    bool shared = false;

    explicit Node(NK k) : kind(k) {}
};

LikeMode classify_like(const std::string &pat);
std::string like_kind_name(LikeMode m);

struct Query {
    std::string source;
    std::vector<Node*> proj;
    bool selectAll = false;
    std::string table;
    Node *where = nullptr;
    std::vector<std::unique_ptr<Node>> arena;

    Node *mk(NK k) {
        arena.emplace_back(std::make_unique<Node>(k));
        return arena.back().get();
    }
    // all nodes in source order
    std::vector<Node*> all_nodes() const;
};

struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Parse "SELECT ... FROM t [WHERE ...]".  Multiple ';'-separated statements
// are returned as multiple queries.
std::vector<std::unique_ptr<Query>> parse_queries(const std::string &sql);

// Bind column names to table indices; expands SELECT *.
void resolve_query(Query &q, const Table &t);

// Bottom-up hashes.
void compute_hashes(Node *n);

// Bottom-up result types. Throws on type errors.
void infer_types(Node *n);

// Human-readable AST (for the web UI / debugging).
std::string dump_node(const Node *n);

} // namespace qex
