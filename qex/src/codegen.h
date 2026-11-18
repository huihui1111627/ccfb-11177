// codegen.h - turn an optimized query AST into a self-contained C source.
#pragma once

#include "expr.h"
#include "jit.h"

namespace qex {

// Emits the full translation unit (runtime header + filter + project).
// Fills `pl` with constant bindings, used columns and string predicates.
std::string emit_translation_unit(const Query &q, JitPlan &pl);

// Precompute one string-predicate dictionary bitmap (dictSize bits).
std::vector<u64> build_spred_bitmap(const Column &c, const std::string &pat,
                                   LikeMode mode);

} // namespace qex
