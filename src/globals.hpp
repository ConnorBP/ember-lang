// ember global initializer evaluation (the load-time gap the v1.0 integration
// found). ember does NOT run script code at load — so a global's `= <expr>`
// initializer is parsed + sema'd but, before this helper, never evaluated into
// the globals block (the block was zero-initialized; a host or @entry had to
// seed globals). This helper evaluates CONST initializers (literals + constexpr-
// foldable exprs: 10, 2.5f, 1+2, -x where x is a literal, etc.) at load and
// writes them into the globals block, so `global g : f32 = 10.0f;` starts at 10.
//
// Non-const initializers (global g = some_fn(); or g = other_global;) are NOT
// evaluable at load (the JIT'd fns don't exist yet, and other globals' values
// aren't known) — those stay zero-initialized (the host or @entry seeds them,
// same as before). Only what try_eval_const_* folds is baked here.
//
// The host calls this AFTER sema + after sizing the globals block, BEFORE the
// first script call. Idempotent + safe to call even with no globals.
#pragma once
#include "ast.hpp"          // Program, GlobalDecl, Type, Prim
#include "sema.hpp"          // try_eval_const_i64/f32/bool
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <unordered_map>

namespace ember {

// The globals block layout a host uses (mirrors codegen.hpp's GlobalsBlock,
// kept here as a simple {index, types} pair so this helper is standalone).
// `bytes` is the raw 8-bytes-per-global buffer (zero-initialized by the host);
// this helper writes folded initializer values into it by global index.
struct GlobalInitCtx {
    std::vector<uint8_t>& bytes;                              // the globals block (8 * #globals)
    const std::unordered_map<std::string, uint32_t>& index;   // name -> global index
    const std::unordered_map<std::string, const Type*>& types;// name -> Type (from the host's GlobalsBlock)
};

// Evaluate every global's const initializer and write it into `gic.bytes`.
// Returns the count of globals whose initializer was folded + baked. A global
// with no init, or a non-const init, is left as-is (zero from the host's init).
inline size_t eval_global_initializers(const Program& prog, GlobalInitCtx gic) {
    size_t baked = 0;
    for (const auto& g : prog.globals) {
        if (!g.init) continue;                       // no initializer -> leave zero
        auto iit = gic.index.find(g.name);
        if (iit == gic.index.end()) continue;        // host didn't register this global's slot
        uint32_t idx = iit->second;
        uint8_t* slot = gic.bytes.data() + size_t(idx) * 8;

        // fold by the global's declared type. An int-typed global can be seeded
        // from an i64 fold; a float global from an f32 fold; bool from bool.
        const Type* t = nullptr;
        auto tit = gic.types.find(g.name);
        if (tit != gic.types.end()) t = tit->second;

        if (t && t->is_int()) {
            int64_t v;
            if (try_eval_const_i64(*g.init, v)) {
                std::memcpy(slot, &v, 8);            // i64 into the 8-byte slot
                ++baked;
            }
        } else if (t && t->is_float()) {
            float v;
            if (try_eval_const_f32(*g.init, v)) {
                std::memcpy(slot, &v, 4);            // f32 into the low 4 bytes
                ++baked;
            }
        } else if (t && t->is_bool()) {
            bool v;
            if (try_eval_const_bool(*g.init, v)) {
                int64_t iv = v ? 1 : 0;
                std::memcpy(slot, &iv, 8);
                ++baked;
            }
        }
        // else: struct/array/slice globals can't be const-seeded (no literal form);
        // left zero. A real struct global would need a StructLit initializer path,
        // which is a separate feature (the host owns struct state via natives today).
    }
    return baked;
}

} // namespace ember
