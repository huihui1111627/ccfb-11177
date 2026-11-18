/* qex_runtime.h - C ABI shared between generated JIT code and the C++ engine.
 *
 * This header is deliberately C-compatible and self-contained: the JIT
 * compiler prepends the exact contents of this file to every generated
 * translation unit, so generated code never depends on engine internals.
 */
#ifndef QEX_RUNTIME_H
#define QEX_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QEX_BATCH 1024u
#define QEX_WORDS(batch) (((batch) + 63u) / 64u)

typedef enum {
    QEX_I64 = 0,
    QEX_F64 = 1,
    QEX_STR = 2
} qex_dtype;

/* One input column, laid out columnar.  Strings are dictionary encoded:
 * every row holds a dictionary id; the dictionary stores byte ptr + length.
 */
typedef struct {
    int      type;          /* qex_dtype */
    int      ndict;         /* dictionary size (string columns) */
    const int64_t  *i64;    /* nrows values */
    const double   *f64;    /* nrows values */
    const uint32_t *sid;    /* nrows dictionary ids, UINT32_MAX = null row */
    const char *const *dict_ptr;
    const int32_t      *dict_len;
    const uint64_t     *nullbm;  /* materialized null bitmap, nwords */
    size_t   nrows;
    size_t   dlen;          /* bytes of backing storage (informational) */
} qex_col;

/* Runtime constants.  Literal values never get baked into the shared
 * object: queries with identical structure but different constants reuse
 * the same compiled code, binding different constants here. */
typedef struct {
    int type;               /* qex_dtype */
    int64_t i64;
    double  f64;
    const char *sptr;
    int32_t     slen;
} qex_const;

/* Arguments passed to every generated filter function. */
typedef struct {
    const qex_col    **cols;
    size_t             ncols;
    const qex_const  *consts;
    size_t             nconsts;
    uint64_t          *out;       /* output bitmap, nwords */
    size_t             nwords;
    size_t             nrows;
    void              *scratch;   /* work buffer, dlen bytes */
    size_t             dlen;
    /* precomputed string-predicate bitmaps: nspred groups of spred_nwords words */
    const uint64_t    *spred;
    size_t             nspred;
    const uint32_t    *spred_sid; /* dict id per row for spred columns */
    size_t             spred_nwords; /* words per predicate (dictionary sized) */
    /* dict id for each string constant, indexed by its qex_const slot:
     * litsid[constSlot] = UINT32_MAX when absent from that column's dict */
    const uint32_t    *litsid;
} qex_filter_args;

/* Arguments passed to every generated projection function.
 * It runs only over rows that survived the filter. */
typedef struct {
    const qex_col    **cols;
    size_t             ncols;
    const qex_const  *consts;
    size_t             nconsts;
    const uint32_t   *sel;       /* selected row ids, nsel entries */
    size_t             nsel;
    void              *scratch;
    size_t             dlen;
    int               *out_type; /* one qex_dtype per output expression */
    int64_t           *out_i64;
    double            *out_f64;
    uint32_t          *out_sid;  /* [nout*nsel] dictionary ids, direct */
    char            ***out_sptr; /* [nout][nsel], optional diagnostics */
    int32_t          **out_slen; /* [nout][nsel], optional diagnostics */
    uint64_t          *out_null; /* nout * nwords(nsel) words */
    size_t             nout;
    /* string-predicate bitmaps shared with the filter */
    const uint64_t    *spred;
    size_t             nspred;
    size_t             spred_nwords;
    const uint32_t    *litsid;
} qex_project_args;

typedef int (*qex_filter_fn)(qex_filter_args *a);
typedef int (*qex_project_fn)(qex_project_args *a);

static inline size_t qex_nwords(size_t nrows) { return (nrows + 63u) / 64u; }
static inline int qex_bget(const uint64_t *bm, size_t i) {
    return (int)((bm[i >> 6] >> (i & 63)) & 1ull);
}
static inline void qex_bset(uint64_t *bm, size_t i) {
    bm[i >> 6] |= 1ull << (i & 63);
}

/* ---- string matching helpers (general LIKE patterns) ---- */

static inline int qex_str_eq(const char *p, int32_t plen,
                             const char *s, int32_t slen) {
    return plen == slen && (plen == 0 || memcmp(p, s, (size_t)plen) == 0);
}

static inline int qex_str_prefix(const char *p, int32_t plen,
                                 const char *s, int32_t slen) {
    return slen >= plen && (plen == 0 || memcmp(p, s, (size_t)plen) == 0);
}

static inline int qex_str_contains(const char *p, int32_t plen,
                                   const char *s, int32_t slen) {
    if (plen == 0) return 1;
    if (slen < plen) return 0;
    const char *end = s + (slen - plen);
    for (const char *q = s; q <= end; ++q) {
        if (memcmp(q, p, (size_t)plen) == 0) return 1;
    }
    return 0;
}

/* Backtracking LIKE matcher: '%' matches any run, '_' matches one char. */
static inline int qex_str_like(const char *pat, int32_t patlen,
                               const char *s, int32_t slen) {
    int32_t pi = 0, si = 0, star_p = -1, star_s = 0;
    while (si < slen) {
        if (pi < patlen && (pat[pi] == '_' || pat[pi] == s[si])) {
            ++pi; ++si;
        } else if (pi < patlen && pat[pi] == '%') {
            star_p = pi++;
            star_s = si;
        } else if (star_p >= 0) {
            pi = star_p + 1;
            si = ++star_s;
        } else {
            return 0;
        }
    }
    while (pi < patlen && pat[pi] == '%') ++pi;
    return pi == patlen;
}

/* Scratch allocator used by generated functions (bump pointer). */
static inline void *qex_scratch_align(qex_filter_args *a, size_t *off,
                                      size_t bytes) {
    size_t aligned = (*off + 63u) & ~(size_t)63u;
    if (aligned + bytes > a->dlen) return NULL;
    void *p = (char *)a->scratch + aligned;
    *off = aligned + bytes;
    return p;
}

#ifdef __cplusplus
}
#endif
#endif /* QEX_RUNTIME_H */
