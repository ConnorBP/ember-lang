// stmt_walk.hpp — shared IfStmt traversal helper for prescan/count passes.
//
// Both codegen.cpp and thin_lower.cpp replicate the same if/else walking
// pattern across 11 prescan / count_*_stmt sites:
//   <cond_fn>(*is->cond, <args>);
//   <block_fn>(is->then_b, <args>);
//   if (is->has_else) <block_fn>(is->else_b, <args>);
//
// The cond/block visitors differ per site (prescan, count_struct/arr/str/
// logical/pin_refs) and carry different trailing args (total, counts, or
// none). This template factors out the common walk without coupling to any
// specific visitor signature — callers pass lambdas that capture their own
// trailing args. Behavior is identical to the inlined pattern.
#ifndef EMBER_STMT_WALK_HPP
#define EMBER_STMT_WALK_HPP

#include "ast.hpp"

namespace ember {

template <typename CondFn, typename BlockFn>
inline void walk_if(const IfStmt& is, CondFn cond_fn, BlockFn block_fn) {
    cond_fn(*is.cond);
    block_fn(is.then_b);
    if (is.has_else) block_fn(is.else_b);
}

} // namespace ember

#endif // EMBER_STMT_WALK_HPP
