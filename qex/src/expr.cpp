#include "expr.h"

#include <cmath>
#include <sstream>

namespace qex {

LikeMode classify_like(const std::string &pat) {
    size_t n = pat.size();
    bool hasWild = false;
    for (char c : pat) if (c == '%' || c == '_') hasWild = true;
    if (!hasWild) return LikeMode::Exact;

    // Prefix: no wildcards except a single trailing '%'
    if (n > 0 && pat[n - 1] == '%') {
        bool ok = true;
        for (size_t i = 0; i + 1 < n; ++i)
            if (pat[i] == '%' || pat[i] == '_') { ok = false; break; }
        if (ok) return LikeMode::Prefix;
    }
    // Contains: leading and trailing '%', no other wildcards
    if (n >= 2 && pat[0] == '%' && pat[n - 1] == '%') {
        bool ok = true;
        for (size_t i = 1; i + 1 < n; ++i)
            if (pat[i] == '%' || pat[i] == '_') { ok = false; break; }
        if (ok) return LikeMode::Contains;
    }
    return LikeMode::Glob;
}

std::string like_kind_name(LikeMode m) {
    switch (m) {
        case LikeMode::Exact:    return "exact";
        case LikeMode::Prefix:   return "prefix";
        case LikeMode::Contains: return "contains";
        case LikeMode::Glob:     return "glob";
    }
    return "?";
}

// ================================================================ parser
namespace {

enum class TK {
    Eof, Ident, Number, String,
    And, Or, Not, Like, Is, Null, Select, From, Where, True, False,
    LParen, RParen, Comma, Star,
    Eq, Ne, Lt, Le, Gt, Ge, Plus, Minus, Slash, Percent, Semicolon
};

struct Tok {
    TK kind;
    std::string text;
    double num = 0;
    bool isFloat = false;
};

struct Lexer {
    const std::string &s;
    size_t pos = 0;
    explicit Lexer(const std::string &src) : s(src) {}

    [[noreturn]] void fail(const std::string &m) {
        throw ParseError("parse error at offset " + std::to_string(pos) +
                         ": " + m);
    }

    void spaces() {
        while (pos < s.size()) {
            char c = s[pos];
            if (std::isspace((unsigned char)c)) { ++pos; continue; }
            if (c == '-' && pos + 1 < s.size() && s[pos + 1] == '-') {
                pos += 2;
                while (pos < s.size() && s[pos] != '\n') ++pos;
                continue;
            }
            break;
        }
    }

    Tok next() {
        spaces();
        if (pos >= s.size()) return {TK::Eof, ""};
        char c = s[pos];

        if (std::isalpha((unsigned char)c) || c == '_') {
            size_t st = pos;
            while (pos < s.size() &&
                   (std::isalnum((unsigned char)s[pos]) || s[pos] == '_'))
                ++pos;
            std::string w = s.substr(st, pos - st);
            std::string lw = to_lower(w);
            static const std::unordered_map<std::string, TK> kw = {
                {"and", TK::And}, {"or", TK::Or}, {"not", TK::Not},
                {"like", TK::Like}, {"is", TK::Is}, {"null", TK::Null},
                {"select", TK::Select}, {"from", TK::From},
                {"where", TK::Where}, {"true", TK::True},
                {"false", TK::False},
            };
            auto it = kw.find(lw);
            if (it != kw.end()) return {it->second, w};
            return {TK::Ident, w};
        }
        if (std::isdigit((unsigned char)c)) {
            size_t st = pos;
            bool flt = false;
            while (pos < s.size() && std::isdigit((unsigned char)s[pos])) ++pos;
            if (pos < s.size() && s[pos] == '.') {
                flt = true;
                ++pos;
                while (pos < s.size() && std::isdigit((unsigned char)s[pos])) ++pos;
            }
            if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
                flt = true; ++pos;
                if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) ++pos;
                while (pos < s.size() && std::isdigit((unsigned char)s[pos])) ++pos;
            }
            std::string w = s.substr(st, pos - st);
            Tok t{TK::Number, w, std::stod(w), flt};
            return t;
        }
        if (c == '\'' || c == '"') {
            char q = c;
            ++pos;
            std::string v;
            while (pos < s.size() && s[pos] != q) {
                if (s[pos] == '\\' && pos + 1 < s.size()) {
                    ++pos;
                    char e = s[pos++];
                    switch (e) {
                        case 'n': v.push_back('\n'); break;
                        case 't': v.push_back('\t'); break;
                        case 'r': v.push_back('\r'); break;
                        default: v.push_back(e); break;
                    }
                } else if (s[pos] == q && pos + 1 < s.size() &&
                           s[pos + 1] == q) {
                    v.push_back(q);
                    pos += 2;
                } else {
                    v.push_back(s[pos++]);
                }
            }
            if (pos >= s.size()) fail("unterminated string");
            ++pos;
            return {TK::String, v};
        }
        ++pos;
        switch (c) {
            case '(': return {TK::LParen, "("};
            case ')': return {TK::RParen, ")"};
            case ',': return {TK::Comma, ","};
            case '*': return {TK::Star, "*"};
            case '+': return {TK::Plus, "+"};
            case '-': return {TK::Minus, "-"};
            case '/': return {TK::Slash, "/"};
            case '%': return {TK::Percent, "%"};
            case ';': return {TK::Semicolon, ";"};
            case '=': return {TK::Eq, "="};
            case '<':
                if (pos < s.size() && s[pos] == '=') { ++pos; return {TK::Le, "<="}; }
                if (pos < s.size() && s[pos] == '>') { ++pos; return {TK::Ne, "<>"}; }
                return {TK::Lt, "<"};
            case '>':
                if (pos < s.size() && s[pos] == '=') { ++pos; return {TK::Ge, ">="}; }
                return {TK::Gt, ">"};
            case '!':
                if (pos < s.size() && s[pos] == '=') { ++pos; return {TK::Ne, "!="}; }
                fail("expected !=");
        }
        fail(std::string("unexpected character '") + c + "'");
    }
};

struct Parser {
    Lexer lx;
    Tok cur;
    Query &q;

    Parser(const std::string &src, Query &query) : lx(src), q(query) {
        cur = lx.next();
    }

    [[noreturn]] void fail(const std::string &m) {
        throw ParseError("parse error near '" + cur.text + "': " + m);
    }
    void accept() { cur = lx.next(); }
    bool eat(TK k) {
        if (cur.kind == k) { accept(); return true; }
        return false;
    }
    void expect(TK k, const std::string &what) {
        if (cur.kind != k) fail("expected " + what);
        accept();
    }

    Node *mk(NK k) { return q.mk(k); }

    Node *parse_expr() { return parse_or(); }

    Node *parse_or() {
        Node *x = parse_and();
        while (cur.kind == TK::Or) {
            accept();
            Node *n = mk(NK::Or); n->a = x; n->b = parse_and(); x = n;
        }
        return x;
    }
    Node *parse_and() {
        Node *x = parse_not();
        while (cur.kind == TK::And) {
            accept();
            Node *n = mk(NK::And); n->a = x; n->b = parse_not(); x = n;
        }
        return x;
    }
    Node *parse_not() {
        if (cur.kind == TK::Not) {
            accept();
            Node *n = mk(NK::Not); n->a = parse_not(); return n;
        }
        return parse_cmp();
    }
    Node *parse_cmp() {
        Node *x = parse_add();
        if (cur.kind == TK::Like) {
            accept();
            if (cur.kind != TK::String) fail("LIKE needs a string pattern");
            Node *n = mk(NK::Like);
            n->a = x;
            n->sv = cur.text;
            n->like = classify_like(cur.text);
            accept();
            return n;
        }
        NK op;
        switch (cur.kind) {
            case TK::Eq: op = NK::Eq; break;
            case TK::Ne: op = NK::Ne; break;
            case TK::Lt: op = NK::Lt; break;
            case TK::Le: op = NK::Le; break;
            case TK::Gt: op = NK::Gt; break;
            case TK::Ge: op = NK::Ge; break;
            default: return x;
        }
        accept();
        Node *n = mk(op);
        n->a = x;
        n->b = parse_add();
        return n;
    }
    Node *parse_add() {
        Node *x = parse_mul();
        while (cur.kind == TK::Plus || cur.kind == TK::Minus) {
            NK op = cur.kind == TK::Plus ? NK::Add : NK::Sub;
            accept();
            Node *n = mk(op); n->a = x; n->b = parse_mul(); x = n;
        }
        return x;
    }
    Node *parse_mul() {
        Node *x = parse_unary();
        while (cur.kind == TK::Star || cur.kind == TK::Slash ||
               cur.kind == TK::Percent) {
            NK op = cur.kind == TK::Star ? NK::Mul
                  : cur.kind == TK::Slash ? NK::Div : NK::Mod;
            accept();
            Node *n = mk(op); n->a = x; n->b = parse_unary(); x = n;
        }
        return x;
    }
    Node *parse_unary() {
        if (cur.kind == TK::Minus) {
            accept();
            Node *n = mk(NK::Neg); n->a = parse_unary(); return n;
        }
        if (cur.kind == TK::Plus) { accept(); return parse_unary(); }
        return parse_primary();
    }
    Node *parse_primary() {
        if (cur.kind == TK::Number) {
            Node *n;
            if (cur.isFloat) { n = mk(NK::LitD); n->dv = cur.num; }
            else { n = mk(NK::LitI); n->iv = (i64)cur.num; }
            accept();
            return n;
        }
        if (cur.kind == TK::String) {
            Node *n = mk(NK::LitS); n->sv = cur.text; accept();
            return n;
        }
        if (cur.kind == TK::True) {
            accept(); Node *n = mk(NK::LitI); n->iv = 1; return n;
        }
        if (cur.kind == TK::False) {
            accept(); Node *n = mk(NK::LitI); n->iv = 0; return n;
        }
        if (cur.kind == TK::LParen) {
            accept();
            Node *n = parse_expr();
            expect(TK::RParen, ")");
            return n;
        }
        if (cur.kind == TK::Ident) {
            Node *n = mk(NK::Col);
            n->colName = cur.text;
            accept();
            if (eat(TK::Is)) {
                bool neg = eat(TK::Not);
                expect(TK::Null, "NULL after IS");
                Node *isn = mk(NK::IsNull); isn->a = n;
                if (neg) { Node *nn = mk(NK::NotNull); nn->a = isn; return nn; }
                return isn;
            }
            return n;
        }
        fail("expected expression");
    }

    void parse_select() {
        expect(TK::Select, "SELECT");
        if (eat(TK::Star)) { q.selectAll = true; }
        if (!q.selectAll) {
            for (;;) {
                q.proj.push_back(parse_expr());
                if (!eat(TK::Comma)) break;
            }
        }
        if (eat(TK::From)) {
            if (cur.kind != TK::Ident) fail("table name after FROM");
            q.table = cur.text;
            accept();
        }
        if (eat(TK::Where)) q.where = parse_expr();
        else { Node *t = mk(NK::LitI); t->iv = 1; q.where = t; }
    }
};

} // namespace

std::vector<std::unique_ptr<Query>> parse_queries(const std::string &sql) {
    std::vector<std::unique_ptr<Query>> out;
    size_t i = 0;
    auto skipSpaceComments = [&](size_t p) -> size_t {
        while (p < sql.size()) {
            if (std::isspace((unsigned char)sql[p])) { ++p; continue; }
            if (p + 1 < sql.size() && sql[p] == '-' && sql[p + 1] == '-') {
                p += 2;
                while (p < sql.size() && sql[p] != '\n') ++p;
                continue;
            }
            break;
        }
        return p;
    };
    while (i < sql.size()) {
        i = skipSpaceComments(i);
        if (i >= sql.size()) break;
        size_t st = i;
        // statements end at ';' at depth 0
        int depth = 0;
        bool inStr = false;
        char qc = 0;
        while (i < sql.size()) {
            char c = sql[i];
            if (inStr) {
                if (c == '\\' && i + 1 < sql.size()) { i += 2; continue; }
                if (c == qc) {
                    if (i + 1 < sql.size() && sql[i + 1] == qc) { i += 2; continue; }
                    inStr = false;
                }
                ++i;
                continue;
            }
            if (c == '\'' || c == '"') { inStr = true; qc = c; ++i; continue; }
            if (c == '(') ++depth;
            if (c == ')') --depth;
            if (c == ';' && depth == 0) break;
            ++i;
        }
        std::string stmt = sql.substr(st, i - st);
        if (i < sql.size() && sql[i] == ';') ++i;
        if (trim(stmt).empty()) continue;

        auto q = std::make_unique<Query>();
        q->source = trim(stmt);
        Parser p(stmt, *q);
        p.parse_select();
        out.push_back(std::move(q));
    }
    return out;
}

void resolve_query(Query &q, const Table &t) {
    if (q.selectAll) {
        q.proj.clear();
        for (auto &c : t.cols) {
            Node *n = q.mk(NK::Col);
            n->colName = c->name;
            n->col = (int)(&c - &t.cols[0]);
            q.proj.push_back(n);
        }
        q.selectAll = false;
    }
    for (Node *n : q.all_nodes()) {
        if (n->kind != NK::Col) continue;
        int idx = t.indexOf(n->colName);
        if (idx < 0) throw ParseError("unknown column: " + n->colName);
        n->col = idx;
        n->type = t.cols[idx]->type;
    }
}

// ------------------------------------------------------------- hashing
static u64 kind_hash(NK k) { return (u64)k * 1099511628211ull + 7; }

static void hash_walk(Node *n) {
    u64 h = kind_hash(n->kind);
    auto mix = [&](u64 x) { h = fnv1a64_u64(x, h); };

    if (n->a) mix(n->a->structHash);
    if (n->b) mix(n->b->structHash);
    // LIKE shape includes the pattern *class* (prefix/contains/glob/exact)
    // so different code paths get different shared objects.
    if (n->kind == NK::Like) mix((u64)n->like);
    n->structHash = h;   // leaves/constants: kind only -> structural reuse

    u64 fh = h;
    auto fmix = [&](u64 x) { fh = fnv1a64_u64(x, fh); };
    if (n->a) fmix(n->a->fullHash);
    if (n->b) fmix(n->b->fullHash);
    switch (n->kind) {
        case NK::LitI: fmix(fnv1a64_u64((u64)n->iv)); break;
        case NK::LitD: fmix(fnv1a64(&n->dv, sizeof(double))); break;
        case NK::LitS: fmix(fnv1a64_str(n->sv)); break;
        case NK::Col:  fmix((u64)n->col); break;
        case NK::Like:
            fmix(fnv1a64_str(n->sv));
            fmix((u64)n->like);
            break;
        default: break;
    }
    n->fullHash = fh;
}

void compute_hashes(Node *root) {
    // iterative postorder
    std::vector<Node*> st{root}, order;
    while (!st.empty()) {
        Node *n = st.back(); st.pop_back();
        order.push_back(n);
        if (n->a) st.push_back(n->a);
        if (n->b) st.push_back(n->b);
    }
    for (auto it = order.rbegin(); it != order.rend(); ++it) hash_walk(*it);
}

// ---------------------------------------------------------- type system
static DType infer(Node *n) {
    switch (n->kind) {
        case NK::LitI: n->type = DType::I64; return n->type;
        case NK::LitD: n->type = DType::F64; return n->type;
        case NK::LitS: n->type = DType::STR; return n->type;
        case NK::Col:
            // column type assigned at resolve time via a second pass below
            return n->type;
        case NK::Neg: {
            DType t = infer(n->a);
            if (t == DType::STR) throw ParseError("unary minus on string");
            n->type = t;
            return n->type;
        }
        case NK::Add: case NK::Sub: case NK::Mul: case NK::Div: case NK::Mod: {
            DType x = infer(n->a), y = infer(n->b);
            if (x == DType::STR || y == DType::STR)
                throw ParseError("arithmetic on string");
            if (n->kind == NK::Mod && (x == DType::F64 || y == DType::F64))
                throw ParseError("MOD requires integers");
            DType r = (x == DType::F64 || y == DType::F64) ? DType::F64 : DType::I64;
            n->cmpType = r;
            n->type = r;
            return r;
        }
        case NK::Eq: case NK::Ne: case NK::Lt: case NK::Le:
        case NK::Gt: case NK::Ge: {
            DType x = infer(n->a), y = infer(n->b);
            if (x == DType::STR || y == DType::STR) {
                if (x != y) throw ParseError("compare string with non-string");
                n->cmpType = DType::STR;
            } else {
                n->cmpType =
                    (x == DType::F64 || y == DType::F64) ? DType::F64 : DType::I64;
            }
            n->type = DType::I64;
            return n->type;
        }
        case NK::Like: {
            DType x = infer(n->a);
            if (x != DType::STR) throw ParseError("LIKE requires string operand");
            if (n->b) infer(n->b);
            n->type = DType::I64;
            return n->type;
        }
        case NK::And: case NK::Or: {
            infer(n->a); infer(n->b);
            n->type = DType::I64;
            return n->type;
        }
        case NK::Not: case NK::IsNull: case NK::NotNull: {
            infer(n->a);
            n->type = DType::I64;
            return n->type;
        }
    }
    throw ParseError("unknown node kind");
}

void infer_types(Node *root) {
    // Column nodes already carry their table type from resolve_query().
    infer(root);
}

std::vector<Node*> Query::all_nodes() const {
    std::vector<Node*> out;
    auto walk = [&](Node *r) {
        if (!r) return;
        std::vector<Node*> st{r};
        while (!st.empty()) {
            Node *n = st.back(); st.pop_back();
            out.push_back(n);
            if (n->a) st.push_back(n->a);
            if (n->b) st.push_back(n->b);
        }
    };
    for (Node *p : proj) walk(p);
    walk(where);
    return out;
}

// -------------------------------------------------------------- printer
static const char *opname(NK k) {
    switch (k) {
        case NK::And: return "AND"; case NK::Or: return "OR"; case NK::Not: return "NOT";
        case NK::Eq: return "="; case NK::Ne: return "!=";
        case NK::Lt: return "<"; case NK::Le: return "<=";
        case NK::Gt: return ">"; case NK::Ge: return ">=";
        case NK::Add: return "+"; case NK::Sub: return "-";
        case NK::Mul: return "*"; case NK::Div: return "/"; case NK::Mod: return "%";
        case NK::Neg: return "-";
        default: return "?";
    }
}

std::string dump_node(const Node *n) {
    if (!n) return "";
    std::ostringstream os;
    switch (n->kind) {
        case NK::LitI: os << n->iv; break;
        case NK::LitD: os << n->dv; break;
        case NK::LitS: os << "'" << n->sv << "'"; break;
        case NK::Col: os << n->colName; break;
        case NK::Like:
            os << "(" << dump_node(n->a) << " LIKE '" << n->sv
               << "' [" << like_kind_name(n->like) << "])";
            break;
        case NK::IsNull: os << "(" << dump_node(n->a) << " IS NULL)"; break;
        case NK::NotNull: os << "(" << dump_node(n->a) << " IS NOT NULL)"; break;
        case NK::Neg: os << "(-" << dump_node(n->a) << ")"; break;
        case NK::Not: os << "(NOT " << dump_node(n->a) << ")"; break;
        case NK::And: case NK::Or: case NK::Eq: case NK::Ne:
        case NK::Lt: case NK::Le: case NK::Gt: case NK::Ge:
        case NK::Add: case NK::Sub: case NK::Mul: case NK::Div: case NK::Mod:
            os << "(" << dump_node(n->a) << " " << opname(n->kind) << " "
               << dump_node(n->b) << ")";
            break;
    }
    os << ":" << dtype_name(n->type);
    return os.str();
}

} // namespace qex
