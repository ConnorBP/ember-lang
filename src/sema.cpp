#include "sema.hpp"
#include "binding_builder.hpp"  // PERM_FFI constant (v0.4 sema gating)
#include <cassert>
#include <climits>
#include <cstring>
#include <unordered_set>
#include <set>
#include <string>
#include <algorithm>
#include <cstdio>

namespace ember {

// Bit-preserving uint64_t -> int64_t conversion (L-§10-3 portability):
// `int64_t(uint64_t(x))` for an out-of-range x is implementation-defined per
// [conv.integral]. memcpy reinterprets the bit pattern with defined behavior
// and is identical on every two's-complement target.
static int64_t bit_cast_i64(uint64_t u) {
    int64_t i;
    std::memcpy(&i, &u, sizeof(i));
    return i;
}

// Per-frame byte budget (red-team V6-DoS mitigation): a single local or the
// running frame total must not exceed this, or a script can declare a huge
// fixed array and exhaust the host stack (the confirmed u8[65536] SIGSEGV).
// Host-configurable later (docs/planning/DESIGN.md v0.4 budgets); a fixed default now.
// 32 KB is generous for real game-logic scripts and well under a guard page.
static constexpr int64_t MAX_FRAME_BYTES = 32 * 1024;
// Max fixed-array length (red-team V6-overflow mitigation): array_len is a
// uint32_t set by the parser with no bound; a field/local like i64[1073741824]
// makes byte_size() overflow int32 and wrap to a 0-byte frame slot. Cap the
// declared length so the product length*elem_size cannot overflow int32.
static constexpr int64_t MAX_ARRAY_LEN = INT32_MAX / 8; // any elem <= 8 bytes

// Frame byte width of a local, mirroring codegen's local_width_bytes so the
// sema-time budget check matches the actual emitted frame layout. Returns
// the byte size of one local slot (16 for a slice, N*elem for a fixed array,
// the struct layout size for a registered struct, 8 for a scalar/handle).
// Used only for the per-local frame-budget check (V6-DoS).
static int64_t frame_byte_width(const Type& t, const StructLayoutTable* structs) {
    if (t.is_slice) return 16;
    if (t.array_len > 0) {
        if (int64_t(t.array_len) > MAX_ARRAY_LEN || !t.elem) return INT64_MAX;
        int64_t eb = frame_byte_width(*t.elem, structs);
        if (eb <= 0 || eb > INT64_MAX / int64_t(t.array_len)) return INT64_MAX;
        return int64_t(t.array_len) * eb;
    }
    if (!t.struct_name.empty()) {
        if (structs) {
            auto it = structs->find(t.struct_name);
            if (it != structs->end()) return int64_t(it->second.size);
        }
        return -1;
    }
    size_t n = t.byte_size();
    return n ? int64_t(n) : 0;
}

StructLayoutTable build_struct_layouts(const Program& prog) {
    StructLayoutTable out;
    std::unordered_map<std::string, const StructDecl*> decls;
    std::unordered_set<std::string> active;
    for (auto& sd : prog.structs) decls[sd.name] = &sd;

    std::function<int64_t(const Type&)> size_of;
    std::function<bool(const std::string&)> build = [&](const std::string& name) {
        if (out.count(name)) return true;
        auto di = decls.find(name);
        if (di == decls.end() || !active.insert(name).second) return false;
        StructLayout layout;
        int64_t off = 0;
        for (auto& fd : di->second->fields) {
            int64_t sz = size_of(*fd.ty);
            if (sz < 0 || sz > INT32_MAX || off > INT32_MAX - sz) {
                active.erase(name);
                return false;
            }
            layout.fields[fd.name] = StructFieldLayout{fd.ty.get(), int32_t(off)};
            layout.field_names.push_back(fd.name);
            off += sz;
        }
        active.erase(name);
        layout.size = int32_t(off);
        out[name] = std::move(layout);
        return true;
    };
    size_of = [&](const Type& t) -> int64_t {
        if (t.is_slice) return 16;
        if (t.array_len) {
            if (!t.elem || t.array_len > uint32_t(MAX_ARRAY_LEN)) return -1;
            int64_t es = size_of(*t.elem);
            if (es < 0 || es > INT32_MAX / int64_t(t.array_len)) return -1;
            return es * int64_t(t.array_len);
        }
        if (!t.struct_name.empty()) {
            // If it's a script-declared struct, recurse into its layout.
            // If it's NOT in decls (an opaque host handle like "string",
            // "vec3" — not a script struct), treat it as 8 bytes (i64 handle).
            auto di = decls.find(t.struct_name);
            if (di == decls.end()) return 8;  // opaque handle
            if (!build(t.struct_name)) return -1;
            return out.at(t.struct_name).size;
        }
        int64_t size = int64_t(t.byte_size());
        return size > 0 ? size : -1;
    };
    for (auto& sd : prog.structs) build(sd.name);
    return out;
}

namespace {

// Close the anonymous namespace so the three const-folders below are
// ember::-scope (declared in sema.hpp for global-initializer evaluation).
// An anon-namespace function would be a separate symbol from the declared
// one, making the recursive calls inside ambiguous. The rest of sema.cpp's
// internals reopen the anon namespace after the folders.
} // namespace

// literal adaptation: if one side is an IntLit/FloatLit (untyped) and the
// other has a concrete numeric type, the literal adopts the concrete type
// (if its value fits). This is the one ergonomic exception to the strict
// same-type rule (docs/spec/TYPE_SYSTEM.md Section 6: literals are context-influenced).
// Variables never implicitly promote (Section 7).

// --- compile-time integer constant evaluation (used by both the array/slice
// bounds-folding below and assert_eq_* constant folding). Mirrors codegen.cpp's
// CG::eval BinExpr constant-folding EXACTLY (same op set, same uint64_t-wrap
// semantics for Add/Sub/Mul, same 0-63 shift-count masking for Shl/Shr, same
// deliberate exclusion of Div/Mod - a literal-zero divisor is a runtime trap,
// not a fold), so a value this resolves to is guaranteed to match what the
// runtime path would have computed had it not been folded away.
//
// Unlike codegen's fold (deliberately single-level: only a BinExpr whose
// DIRECT operands are literals folds, everything else is left as a genuine
// runtime computation - see constant_folding_edge_cases.ember), this one
// recurses through nested BinExpr/UnaryExpr trees of constants. That's safe
// to do here even though codegen won't fold as deep: this function's result
// is only ever used to (a) decide whether a compile-time bounds/assert check
// applies at all, never to change what codegen emits for the value itself,
// and (b) when it does apply, the "in-range"/"equal" verdict is checked
// against the exact same arithmetic codegen would separately (and
// independently) compute at runtime for that same expression - so there is
// no way for this to fold something codegen would compute differently.
bool try_eval_const_i64(const Expr& e, int64_t& out) {
    if (auto* lit = dynamic_cast<const IntLit*>(&e)) { out = lit->v; return true; }
    if (auto* u = dynamic_cast<const UnaryExpr*>(&e)) {
        int64_t v;
        if (!try_eval_const_i64(*u->operand, v)) return false;
        switch (u->op) {
        case UnaryExpr::Op::Neg:    out = bit_cast_i64(uint64_t(0) - uint64_t(v)); return true;
        case UnaryExpr::Op::BitNot: out = ~v; return true;
        default: return false; // Not (!) is bool-only, never reaches an int context
        }
    }
    if (auto* b = dynamic_cast<const BinExpr*>(&e)) {
        int64_t l, r;
        if (!try_eval_const_i64(*b->lhs, l)) return false;
        if (!try_eval_const_i64(*b->rhs, r)) return false;
        switch (b->op) {
        case BinExpr::Op::Add: out = bit_cast_i64(uint64_t(l) + uint64_t(r)); return true;
        case BinExpr::Op::Sub: out = bit_cast_i64(uint64_t(l) - uint64_t(r)); return true;
        case BinExpr::Op::Mul: out = bit_cast_i64(uint64_t(l) * uint64_t(r)); return true;
        case BinExpr::Op::And: out = l & r; return true;
        case BinExpr::Op::Or:  out = l | r; return true;
        case BinExpr::Op::Xor: out = l ^ r; return true;
        case BinExpr::Op::Shl: out = bit_cast_i64(uint64_t(l) << (r & 63)); return true;
        // Arithmetic right shift as an unsigned shift plus explicit sign fill
        // (L-§10-3): signed `int64_t >> count` of a negative value is
        // implementation-defined per [expr.shift]; computing from the
        // unsigned bit pattern and OR-ing the sign bits is defined behavior
        // and matches x64 sar. (This evaluator never feeds codegen emission -
        // see the comment above - so it needs no normalize_rax, only the
        // portability hardening.)
        case BinExpr::Op::Shr: {
            int sh = int(r & 63);
            uint64_t ur = uint64_t(l) >> sh;
            if (sh != 0 && l < 0) ur |= ~((1ULL << (64 - sh)) - 1);
            out = bit_cast_i64(ur); return true;
        }
        // Div/Mod excluded on purpose (see comment above) - a literal-zero
        // divisor must still hit the real runtime trap, never a compile-time
        // fold that would either misbehave or need to replicate the trap here.
        default: return false;
        }
    }
    return false; // Ident/CallExpr/etc: a genuine runtime value, never foldable
}

// Same idea for f32/f64 literal constants (assert_eq_f32 folding). Mirrors
// codegen's FloatLit constant-fold exactly (always narrows through f32,
// per CG::eval's FloatLit case and its BinExpr Float fold comment).
bool try_eval_const_f32(const Expr& e, float& out) {
    if (auto* lit = dynamic_cast<const FloatLit*>(&e)) { out = float(lit->v); return true; }
    if (auto* u = dynamic_cast<const UnaryExpr*>(&e)) {
        float v;
        if (u->op != UnaryExpr::Op::Neg) return false;
        if (!try_eval_const_f32(*u->operand, v)) return false;
        out = -v; return true;
    }
    if (auto* b = dynamic_cast<const BinExpr*>(&e)) {
        float l, r;
        if (!try_eval_const_f32(*b->lhs, l)) return false;
        if (!try_eval_const_f32(*b->rhs, r)) return false;
        switch (b->op) {
        case BinExpr::Op::Add: out = l + r; return true;
        case BinExpr::Op::Sub: out = l - r; return true;
        case BinExpr::Op::Mul: out = l * r; return true;
        case BinExpr::Op::Div: out = l / r; return true; // well-defined (IEEE754), safe to fold
        default: return false;
        }
    }
    return false;
}

// f64-preserving variant: folds a FloatLit/UnaryExpr/BinExpr at FULL double
// precision without the f32 narrowing try_eval_const_f32 applies. Used by the
// global-initializer path (globals.hpp) so an `f64` global's literal
// initializer (e.g. `global g : f64 = 0.1;`) bakes the exact double bytes into
// the globals block — codegen's FloatLit f64 case already stores the full
// double for an f64 local, so without this the global would hold a
// float-narrowed value and compare unequal to an equal-precision local
// (the C6 defect). Mirrors try_eval_const_f32's shape; the only difference is
// the operand type + the absence of the `float(...)` cast on the literal.
bool try_eval_const_f64(const Expr& e, double& out) {
    if (auto* lit = dynamic_cast<const FloatLit*>(&e)) { out = lit->v; return true; }
    if (auto* u = dynamic_cast<const UnaryExpr*>(&e)) {
        double v;
        if (u->op != UnaryExpr::Op::Neg) return false;
        if (!try_eval_const_f64(*u->operand, v)) return false;
        out = -v; return true;
    }
    if (auto* b = dynamic_cast<const BinExpr*>(&e)) {
        double l, r;
        if (!try_eval_const_f64(*b->lhs, l)) return false;
        if (!try_eval_const_f64(*b->rhs, r)) return false;
        switch (b->op) {
        case BinExpr::Op::Add: out = l + r; return true;
        case BinExpr::Op::Sub: out = l - r; return true;
        case BinExpr::Op::Mul: out = l * r; return true;
        case BinExpr::Op::Div: out = l / r; return true; // well-defined (IEEE754), safe to fold
        default: return false;
        }
    }
    return false;
}

bool try_eval_const_bool(const Expr& e, bool& out) {
    if (auto* lit = dynamic_cast<const BoolLit*>(&e)) { out = lit->v; return true; }
    if (auto* u = dynamic_cast<const UnaryExpr*>(&e)) {
        if (u->op != UnaryExpr::Op::Not) return false;
        bool v;
        if (!try_eval_const_bool(*u->operand, v)) return false;
        out = !v; return true;
    }
    // Comparison operators over compile-time integer constants: fold both
    // sides via try_eval_const_i64 and compare. This is what lets a
    // static_assert condition like `1 + 1 == 2` or `square(7) == 49` (after
    // the constexpr-call pre-pass rewrites square(7) to an IntLit) resolve to
    // a compile-time bool. Safe to add to the shared folder: the only
    // pre-existing caller is assert_eq_bool's constant-operand folding, where
    // this STRICTLY widens what folds (a constant comparison arg now elides
    // instead of leaving a runtime call) — matching assert_eq's existing
    // "a passing constant assertion costs nothing once elided" philosophy.
    // A non-foldable side (Ident, runtime call, etc.) returns false and the
    // expression stays a genuine runtime value, exactly as before.
    if (auto* b = dynamic_cast<const BinExpr*>(&e)) {
        switch (b->op) {
        case BinExpr::Op::Eq: case BinExpr::Op::Neq:
        case BinExpr::Op::Lt: case BinExpr::Op::Le:
        case BinExpr::Op::Gt: case BinExpr::Op::Ge: {
            int64_t l, r;
            if (!try_eval_const_i64(*b->lhs, l)) return false;
            if (!try_eval_const_i64(*b->rhs, r)) return false;
            switch (b->op) {
            case BinExpr::Op::Eq: out = (l == r); break;
            case BinExpr::Op::Neq: out = (l != r); break;
            case BinExpr::Op::Lt: out = (l <  r); break;
            case BinExpr::Op::Le: out = (l <= r); break;
            case BinExpr::Op::Gt: out = (l >  r); break;
            case BinExpr::Op::Ge: out = (l >= r); break;
            default: return false; // unreachable (switch above gates these)
            }
            return true;
        }
        case BinExpr::Op::LAnd: case BinExpr::Op::LOr: {
            bool l, r;
            if (!try_eval_const_bool(*b->lhs, l)) return false;
            if (!try_eval_const_bool(*b->rhs, r)) return false;
            out = (b->op == BinExpr::Op::LAnd) ? (l && r) : (l || r);
            return true;
        }
        default: break; // arithmetic/bitwise ops are not bool-producing here
        }
    }
    return false;
}

namespace {  // reopen anonymous namespace for the rest of sema.cpp's internals

struct Checker {
    const std::unordered_map<std::string, NativeSig>* natives;
    const std::unordered_map<std::string, int>* script_slots;
    uint32_t perms;
    std::vector<SemaError> errs;
    std::vector<SemaError> warns;
    Program* prog = nullptr;
    const OpOverloadTable* overloads = nullptr;
    const StructLayoutTable* structs = nullptr;
    const ModuleExportTable* module_exports = nullptr;  // v0.5 cross-module exports (mod::fn resolution)

    // scope stack for locals/params
    // array_elem_ty: the inferred element type (u8/f32/i64) when this Var holds
    // an array<T> handle from the array extension (set when the let-initializer
    // is array_new(esz, ...) or an alias of one). Null otherwise. Used by the
    // ForEachStmt check (the iterable() hook) to type the loop variable and
    // by codegen to select the array_get_* variant. See ForEachStmt in ast.hpp.
    struct Var { std::string name; const Type* ty; bool is_const; bool local_array_view; const Type* array_elem_ty = nullptr;
        // #20 lambda capture: this Var is a by-value capture living in the
        // lambda's env struct (not a frame slot). env_offset is the byte offset
        // within env. Sema declares captures this way in the synthetic fn's
        // scope; codegen loads them from [env_ptr + env_offset].
        // env_capture_by_ref: a by-REF capture's env slot holds a POINTER to
        // the captured variable's storage (not a copy), so codegen
        // double-dereferences on read and stores-through on write. A by-ref
        // capture is also mutable (is_const=false) so the body can mutate the
        // original through the pointer.
        bool is_env_capture = false; int32_t env_offset = 0; bool env_capture_by_ref = false; };
    std::vector<std::vector<Var>> scopes;

    // #20 nested-lambda capture: a snapshot of the ENCLOSING scope chain at
    // the moment a LambdaExpr is created (saved in check_lambda, keyed by the
    // synthetic fn name). check_lambda_func restores it before pushing the
    // lambda's own scope, so that a NESTED lambda's collect_captures (which
    // runs during the enclosing lambda's body check) can see not only the
    // enclosing lambda's params/captures but ALSO the scopes above it (e.g.
    // main's locals). Without this, check_func's scopes.clear() wipes the
    // outer scopes before the nested lambda is checked, so a capture from a
    // grandparent scope (like main's `base`) is missed.
    std::unordered_map<std::string, std::vector<std::vector<Var>>> lambda_scope_snapshots;

    struct GlobalVar { const Type* ty; bool is_const; };
    std::unordered_map<std::string, GlobalVar> globals;
    int loop_depth = 0;
    int switch_depth = 0;
    int check_depth = 0;  // recursion depth guard (DoS prevention — audit HIGH finding)
    static constexpr int MAX_CHECK_DEPTH = 3000;  // allows 2000-term flat expr chains; prevents 100k-level stack overflow
    const Expr* aggregate_cast_init = nullptr;

    // Tier 1 enums (docs/planning/plan_ENUMS.md Section 4): (enum_name -> (variant -> i32 value)),
    // built by the enum-resolution pass (pass 1.5, before any function body is
    // checked). `enum_names` is the set of registered enum names, used by the
    // type-position enum-name rejection (Section 4.5: `let x: Color = ...` is a
    // clean error in v1 for an UNTYPED enum, the hook the typed-enum upgrade
    // flips to accept for a typed one).
    std::unordered_map<std::string, std::unordered_map<std::string, int64_t>> enum_values;
    std::unordered_set<std::string> enum_names;

    bool is_enum_name(const std::string& n) const { return enum_names.count(n) != 0; }

    // Tier 1 typed enums (docs/planning/plan_ENUMS.md §6): `enum E : T { ... }`
    // makes E a real type backed by the integer T. typed_enum_backing maps the
    // enum name to its backing Prim; typed_enum_types caches the interned
    // Type (prim=backing, struct_name="", enum_name=E) handed out to type
    // positions + EnumAccessExpr rewrites. Populated by register_typed_enums
    // (a pre-pass BEFORE resolve_type, so a `let c: Color` annotation resolves
    // to the typed enum type — not the opaque-handle I64 the untyped path
    // would produce). An enum name NOT in this map is untyped (backward compat:
    // variants are plain i32, the name is not a type).
    std::unordered_map<std::string, Prim> typed_enum_backing;
    std::unordered_map<std::string, const Type*> typed_enum_types;
    bool is_typed_enum(const std::string& n) const { return typed_enum_backing.count(n) != 0; }

    // script function signatures: name -> (ret type, param types)
    //
    // borrowed_params / retained_params (slice-escape safety Stage 2, C5):
    // a per-fn escape analysis computed in a pre-pass BEFORE any function
    // body is checked (so a call to fn F in fn G's body — checked before F's
    // body — sees F's sets). Only SLICE params are tracked (a non-slice param
    // is passed by value and copied; only a slice (ptr+len into a frame) can
    // dangle). Indexing matches call-site arg indexing: param i <-> c->args[i]
    // for a direct named script call.
    //
    //   borrowed_params  — the slice params whose value flows into the fn's
    //     RETURN value (the fn `return s;` / `return s[..];` / `return relay(s);`
    //     where relay borrows its arg). For these the CALL RESULT is itself a
    //     stack-backed slice: is_local_array_view() recognizes such a CallExpr
    //     and tags the result localview, so the caller's OWN escape guards (C1
    //     return / C2a global-store / C2b field-store) catch the escape at the
    //     actual escape point. This is what lets the legitimate synchronous
    //     pattern `return_slice_defer(return_values[..])` (fn returns its slice
    //     arg, caller reads r[0]/r[1] within the caller's own frame) still work:
    //     the call result is tagged localview, but the synchronous reads are not
    //     escape sites, so no guard fires.
    //
    //   retained_params   — the slice params the fn STORES to a GLOBAL inside
    //     its body (`g = s;` / `g = relay(s);` where relay retains). The escape
    //     is INSIDE the fn, invisible to the caller's own guards (the param is
    //     not localview from the fn's perspective — it's declared
    //     local_array_view=false in check_func). So the reject happens at the
    //     CALL SITE: passing a stack-backed slice to a retaining param is a
    //     compile error, because the callee would store the dangling ptr into a
    //     global that outlives the caller's frame.
    //
    // A param can be in BOTH sets (the fn returns it AND stores it to a global):
    // the retained reject at the call site takes precedence (the fn does escape
    // it, regardless of what the caller does with the result). return_slice_defer
    // is borrowed-only (it returns `values`, never stores it) so it is NOT
    // rejected at the call site. See demo/SLICE_ESCAPE_SAFETY_INVESTIGATION.md
    // §6.3 and the ROADMAP's slice-escape Stage 2 entry.
    struct ScriptSig {
        const Type* ret;
        std::vector<const Type*> params;
        std::vector<const DefaultValue*> defaults; // parallel to params; Kind::None = required
        size_t required_count = 0;                 // count of leading non-defaulted params
        std::set<size_t> borrowed_params;  // C5: slice params flowing into the return value
        std::set<size_t> retained_params;  // C5: slice params stored to a global inside the fn
    };
    std::unordered_map<std::string, ScriptSig> script_sigs;
    std::unordered_set<std::string> namespace_names;  // Tier 1: namespace names (for Foo::bar resolution)
    std::string current_ns;  // Tier 1: the namespace of the fn currently being checked ("" = top-level)
    FuncDecl* current_func = nullptr;  // #21: the fn currently being checked (for yield->coroutine marking)

    // owning type store (lives on the Program so it outlives sema + codegen;
    // raw `ty` pointers stashed on AST nodes stay valid until codegen finishes).
    const Type* intern(Type t) {
        prog->type_store.push_back(std::make_shared<Type>(std::move(t)));
        return prog->type_store.back().get();
    }

    void err(const std::string& m, uint32_t l, uint32_t c) {
        errs.push_back({m, l, c});
    }
    // Non-fatal deprecation notice (the CLI prints these to stderr but still runs).
    void warn(const std::string& m, uint32_t l, uint32_t c) {
        warns.push_back({m, l, c});
    }

    // Returns the scope-stack record itself (type, constness, provenance), or
    // nullptr if `n` isn't a local/param.
    Var* lookup_local_var(const std::string& n) {
        for (int i = int(scopes.size())-1; i >= 0; --i)
            for (auto& v : scopes[i]) if (v.name == n) return &v;
        return nullptr;
    }
    const Var* lookup_local_var(const std::string& n) const {
        for (int i = int(scopes.size())-1; i >= 0; --i)
            for (const auto& v : scopes[i]) if (v.name == n) return &v;
        return nullptr;
    }

    const Type* lookup_var(const std::string& n) {
        if (const Var* v = lookup_local_var(n)) return v->ty;
        auto it = globals.find(n);
        if (it != globals.end()) return it->second.ty;
        return nullptr;
    }

    // true iff `t` names a script-declared `struct Name {...}` (as opposed to
    // a built-in handle type like vec3/string, which is also prim=I64 with a
    // struct_name but isn't in `structs` - only script structs go through the
    // by-value calling convention below).
    bool is_registered_struct(const Type* t) const {
        return structs && t && !t->struct_name.empty() && structs->count(t->struct_name) != 0;
    }

    // Opaque named handles and function handles use an integer machine word,
    // but are nominal types.  Only actual, untagged integer types participate
    // in v1's integer compatibility rule.
    static bool is_plain_integer(const Type* t) {
        return t && t->is_int() && !t->is_fn_handle && t->struct_name.empty() && t->enum_name.empty();
    }
    bool is_by_value_aggregate(const Type* t) const {
        return t && (t->array_len > 0 || is_registered_struct(t));
    }
    static int int_width(const Type* t) {
        if (!t) return 0;
        switch (t->prim) {
        case Prim::I8: case Prim::U8: return 8;
        case Prim::I16: case Prim::U16: return 16;
        case Prim::I32: case Prim::U32: return 32;
        case Prim::I64: case Prim::U64: return 64;
        default: return 0;
        }
    }
    static bool can_implicitly_convert(const Type* want, const Type* got) {
        if (!want || !got) return false;
        if (want->same(*got)) return true;
        // Tier 1 typed enums (§6.3): enum->backing-int implicit widening. A
        // typed-enum value (enum_name set) flows to a plain integer target
        // when the backing prim widens to (or exactly matches) the target —
        // same signedness, backing width <= target width. The reverse
        // (int->enum) is NOT implicit: is_plain_integer(want) is false for a
        // typed-enum target, so the plain-int matrix below does not accept it.
        if (!got->enum_name.empty() && is_plain_integer(want)) {
            return got->is_uint() == want->is_uint() && int_width(got) <= int_width(want);
        }
        // Float widening (docs/spec/TYPE_SYSTEM.md Section 6): f32->f64 is the
        // one lossless implicit float conversion (matches C/Rust). The reverse
        // (f64->f32) is narrowing and stays explicit (`as`); int<->float is
        // never implicit either. Codegen already emits cvtss2sd for the
        // CastExpr this synthesizes (the explicit `f32 as f64` path), so this
        // is purely the sema gate.
        if (got->is_float() && want->is_float() &&
            got->prim == Prim::F32 && want->prim == Prim::F64)
            return true;
        return is_plain_integer(want) && is_plain_integer(got) &&
               want->is_uint() == got->is_uint() && int_width(got) < int_width(want);
    }
    static bool types_compatible(const Type* want, const Type* got) {
        if (!want || !got) return false;
        if (want->same(*got)) return true;
        // #20 lambda compatibility: a lambda type (is_lambda) is compatible
        // with a fn-handle type (is_fn_handle) if both carry matching recorded
        // sigs. This bridges the two representations without making them
        // globally `same` (which would break fn-handle equality elsewhere).
        if ((want->is_lambda || want->is_fn_handle) && (got->is_lambda || got->is_fn_handle)) {
            // Cross-module handles are a distinct space from intra-module —
            // don't bridge across that boundary (the cross_module_handles
            // test expects cross != intra even with matching sigs).
            if (want->is_cross_module_handle != got->is_cross_module_handle)
                return false;
            if (want->has_recorded_sig && got->has_recorded_sig) {
                if (want->recorded_params.size() != got->recorded_params.size()) return false;
                for (size_t i = 0; i < want->recorded_params.size(); ++i)
                    if (!want->recorded_params[i]->same(*got->recorded_params[i])) return false;
                if (want->recorded_ret && got->recorded_ret)
                    return want->recorded_ret->same(*got->recorded_ret);
                return !want->recorded_ret && !got->recorded_ret;
            }
            // one or both bare (no recorded sig): compatible (bare fn accepts any).
            return true;
        }
        return false;
    }

    // The single implicit-conversion gate for value-flow contexts.  Legal
    // same-signedness widening is represented by the ordinary typed CastExpr
    // so codegen sees the conversion explicitly; narrowing/signedness changes
    // remain available only through source-level `as`.
    const Type* check_value(ExprPtr& slot, const Type* want, bool allow_struct_ret_call = false) {
        const Type* got = check_expr(*slot, want, allow_struct_ret_call);
        // Integer literals retain contextual adaptation (including a fitting
        // literal's signedness); non-literal values obey widening-only.
        if (dynamic_cast<IntLit*>(slot.get()) && want && want->same(*got)) return got;
        if (!can_implicitly_convert(want, got)) return got;
        if (!want->same(*got)) {
            auto cast = std::make_unique<CastExpr>();
            cast->loc = slot->loc; cast->operand = std::move(slot);
            cast->to = std::make_shared<Type>(*want); cast->ty = intern(*want);
            slot = std::move(cast);
            return slot->ty;
        }
        return got;
    }

    // A struct-by-value call argument may be (a) a bare local variable, (b)
    // a struct literal of the matching struct type, or (c) a call whose own
    // return type is the matching struct (a struct-returning call used
    // directly as an argument). Cases (b)/(c) have no single source frame
    // address, so codegen (CG::eval's CallExpr struct-arg stash) materializes
    // them into a compiler-hidden temp frame slot first, then copies that
    // temp's bytes into the arg stash - reusing the existing hidden-pointer /
    // arg-copy machinery. A genuinely-mismatched shape (wrong struct, scalar
    // where a struct is expected) is still rejected - by types_compatible for
    // the wrong struct / scalar case above, and here for any other aggregate
    // expression (e.g. a field access) that isn't one of the three allowed
    // forms. arg.ty is already populated by the check_value(arg, want) call
    // that runs immediately before this.
    void check_struct_arg_shape(const Expr& arg, const Type* want) {
        if (!is_registered_struct(want)) return;
        if (dynamic_cast<const Ident*>(&arg)) return;
        if (auto* sl = dynamic_cast<const StructLit*>(&arg)) {
            if (sl->type_name == want->struct_name) return;
        }
        if (auto* call = dynamic_cast<const CallExpr*>(&arg)) {
            if (call->ty && is_registered_struct(call->ty) && call->ty->struct_name == want->struct_name)
                return;
        }
        err("a struct-by-value argument (type '" + want->struct_name + "') must be a plain "
            "local variable, a struct literal of '" + want->struct_name + "', or a call "
            "returning '" + want->struct_name + "'", arg.loc.line, arg.loc.col);
    }

    void reject_large_native_aggregate(const Type* want, const Loc& loc) {
        if (!is_registered_struct(want)) return;
        auto it = structs->find(want->struct_name);
        // The Win64 ABI passes structs >8 bytes by hidden pointer (caller
        // allocates, passes ptr in first arg slot, callee fills). Ember's
        // codegen already implements this for script structs. Host-registered
        // structs (registered via register_struct) also use this path.
        // Limit: 128 bytes (enough for Mat4 = 64 bytes, with room for larger).
        if (it != structs->end() && it->second.size > 128)
            err("native by-value argument '" + want->struct_name + "' is " +
                std::to_string(it->second.size) + " bytes; maximum is 128 bytes",
                loc.line, loc.col);
    }

    // --- Tier 1 enums (docs/planning/plan_ENUMS.md) ---
    //
    // Pass 1.4 (typed-enum registration, §6): BEFORE resolve_type runs, scan
    // every enum and register the typed ones (those with a `: type` backing
    // declaration). For each typed enum we validate the backing type is an
    // integer primitive, then cache (name -> backing Prim) and the interned
    // typed-enum Type (prim=backing, struct_name="", enum_name=name). This
    // MUST precede resolve_type so a `let c: Color` annotation resolves to
    // the typed enum type instead of the opaque-handle I64 the untyped path
    // would produce (a typed enum name is not in the StructLayoutTable).
    void register_typed_enums() {
        for (auto& e : prog->enums) {
            if (!e.is_typed()) continue;
            const Type* bt = e.backing_type.get();
            if (!bt || !bt->is_int() || !bt->struct_name.empty()) {
                err("enum '" + e.name + "' backing type must be an integer (i8/i16/i32/i64/u8/u16/u32/u64)",
                    e.loc.line, e.loc.col);
                continue;
            }
            typed_enum_backing[e.name] = bt->prim;
            Type t = make_prim(bt->prim);
            t.enum_name = e.name;
            typed_enum_types[e.name] = intern(std::move(t));
        }
    }
    // Range (min,max) for a signed/unsigned integer Prim, used to range-check
    // enum variant values against the enum's backing type (i32 for untyped,
    // the declared backing for typed). Returns true iff `v` fits.
    static bool int_fits(Prim p, int64_t v) {
        switch (p) {
        case Prim::I8:  return v >= -128 && v <= 127;
        case Prim::U8:  return v >= 0 && v <= 0xFF;
        case Prim::I16: return v >= -32768 && v <= 32767;
        case Prim::U16: return v >= 0 && v <= 0xFFFF;
        case Prim::I32: return v >= -2147483648LL && v <= 2147483647LL;
        case Prim::U32: return v >= 0 && v <= 0xFFFFFFFFLL;
        case Prim::I64: return true;  // full int64_t range
        case Prim::U64: return true;  // the lexer bit-casts full u64 into int64_t
        default: return false;
        }
    }
    static const char* prim_name(Prim p) {
        switch (p) {
        case Prim::I8: return "i8";  case Prim::U8: return "u8";
        case Prim::I16: return "i16"; case Prim::U16: return "u16";
        case Prim::I32: return "i32"; case Prim::U32: return "u32";
        case Prim::I64: return "i64"; case Prim::U64: return "u64";
        default: return "?";
        }
    }
    //
    // Pass 1.5 (Section 4.1): resolve every enum variant's value. First
    // variant defaults to 0; each variant without an explicit `= value` is
    // prev+1; an explicit `= constexpr_int` sets the next base. Explicit
    // values are restricted to what try_eval_const_i64 already folds
    // (IntLit / -IntLit / BinExpr-of-literals) - no new evaluator - EXCEPT a
    // constexpr fn call is ALSO accepted: lower_constexpr_calls_expr folds it
    // to an IntLit first (the enum-from-constexpr-expr feature, §6.2), so a
    // variant value like `X = base()` where base is a `constexpr fn` works.
    // Values are range-checked against the backing type (i32 for untyped, the
    // declared backing for typed) and duplicate values / duplicate variant
    // names within one enum are errors (stricter than C). Builds the
    // (enum_name -> (variant -> value)) table for EnumAccessExpr resolution
    // and the enum_names set for the type-position rejection (Section 4.5,
    // untyped-only now that typed enums are real types).
    void resolve_enums() {
        std::unordered_set<std::string> seen_enum_names;
        for (auto& e : prog->enums) {
            if (!seen_enum_names.insert(e.name).second)
                err("duplicate enum declaration '" + e.name + "'", e.loc.line, e.loc.col);
            enum_names.insert(e.name);
            Prim backing = is_typed_enum(e.name) ? typed_enum_backing[e.name] : Prim::I32;
            int64_t next = 0;
            std::unordered_set<int64_t> seen_values;
            std::unordered_set<std::string> seen_variant_names;
            for (auto& v : e.variants) {
                if (!seen_variant_names.insert(v.name).second)
                    err("enum '" + e.name + "' has duplicate variant '" + v.name + "'",
                        v.loc.line, v.loc.col);
                if (v.explicit_value) {
                    // enum-from-constexpr-expr (§6.2): fold any constexpr fn
                    // call in the variant value to an IntLit BEFORE the const
                    // check, so `X = base()` (base a constexpr fn) resolves.
                    lower_constexpr_calls_expr(v.explicit_value);
                    int64_t ev = 0;
                    if (!try_eval_const_i64(*v.explicit_value, ev)) {
                        err("enum variant '" + e.name + "::" + v.name +
                            "' explicit value must be a compile-time integer constant",
                            v.loc.line, v.loc.col);
                    } else if (!int_fits(backing, ev)) {
                        err("enum variant '" + e.name + "::" + v.name +
                            "' value " + std::to_string(ev) + " out of " +
                            prim_name(backing) + " range",
                            v.loc.line, v.loc.col);
                    } else {
                        next = ev;
                    }
                }
                v.resolved = next;
                if (!seen_values.insert(next).second)
                    err("enum '" + e.name + "' has duplicate value " + std::to_string(next) +
                        " (variant '" + v.name + "')", v.loc.line, v.loc.col);
                enum_values[e.name][v.name] = next;
                ++next;
            }
        }
    }

    // Pass 1.6 (Section 4.5 follow-through): reject an UNTYPED enum name used
    // in a type position (`let x: Color = ...`, `fn f(p: Color)`, a struct
    // field, a global, a return type). A TYPED enum name (`enum Color : i32`)
    // IS a real type and is accepted (resolve_type already rewrote it to the
    // backing-prim + enum_name form by this point, so its struct_name is
    // empty and this check is a no-op for typed enums — the explicit
    // is_typed_enum guard is defensive).
    void check_type_not_enum(const Type* t, const Loc& loc) {
        if (t && !t->struct_name.empty() && is_enum_name(t->struct_name) &&
            !is_typed_enum(t->struct_name))
            err("enum '" + t->struct_name + "' is untyped; declare the binding with an integer type, or declare the enum as `enum " + t->struct_name + " : <int>`",
                loc.line, loc.col);
    }
    // Walk every declared type in the program (struct fields, func params,
    // func returns, globals) and reject enum-named ones. `let` types live in
    // function bodies and are checked lazily in check_stmt's LetStmt.
    void check_declared_types_not_enum() {
        for (auto& sd : prog->structs)
            for (auto& fd : sd.fields) check_type_not_enum(fd.ty.get(), sd.loc);  // FieldDecl carries no Loc; report at the struct
        for (auto& f : prog->funcs) {
            for (auto& p : f.params) check_type_not_enum(p.ty.get(), p.loc);
            check_type_not_enum(f.ret.get(), f.loc);
        }
        for (auto& g : prog->globals) check_type_not_enum(g.ty.get(), g.loc);
    }

    static bool contains_void(const Type* t) {
        if (!t) return true;
        if (t->is_slice || t->array_len) return !t->elem || contains_void(t->elem.get());
        return t->is_void();
    }

    void validate_declarations() {
        std::unordered_set<std::string> struct_names;
        for (auto& sd : prog->structs) {
            if (!struct_names.insert(sd.name).second)
                err("duplicate struct declaration '" + sd.name + "'", sd.loc.line, sd.loc.col);
            std::unordered_set<std::string> fields;
            for (auto& fd : sd.fields) {
                if (!fields.insert(fd.name).second)
                    err("duplicate field '" + fd.name + "' in struct '" + sd.name + "'", sd.loc.line, sd.loc.col);
                if (contains_void(fd.ty.get()))
                    err("struct field '" + sd.name + "." + fd.name + "' cannot have void type", sd.loc.line, sd.loc.col);
            }
        }
        for (auto& sd : prog->structs) {
            if (!structs || !structs->count(sd.name))
                err("struct '" + sd.name + "' has a recursive, unknown, or invalid by-value layout", sd.loc.line, sd.loc.col);
        }
        std::unordered_set<std::string> fn_names;
        for (auto& f : prog->funcs) {
            std::string decl_key = f.ns.empty() ? f.name : (f.ns + "::" + f.name);
            if (!fn_names.insert(decl_key).second)
                err("duplicate function declaration '" + decl_key + "'", f.loc.line, f.loc.col);
            std::unordered_set<std::string> params;
            for (auto& p : f.params) {
                if (!params.insert(p.name).second)
                    err("duplicate parameter '" + p.name + "'", p.loc.line, p.loc.col);
                if (contains_void(p.ty.get()))
                    err("parameter '" + p.name + "' cannot have void type", p.loc.line, p.loc.col);
            }
        }
        std::unordered_set<std::string> global_names;
        for (auto& g : prog->globals) {
            std::string gkey = g.ns.empty() ? g.name : (g.ns + "::" + g.name);
            if (!global_names.insert(gkey).second)
                err("duplicate global declaration '" + gkey + "'", g.loc.line, g.loc.col);
            if (contains_void(g.ty.get()))
                err("global '" + g.name + "' cannot have void type", g.loc.line, g.loc.col);
            // Aggregate globals (struct / fixed-array / slice) are accepted in
            // v1 (chunk c3): a typed globals-block layout + load-time const
            // folding of aggregate initializers backs them. A global with NO
            // initializer stays zero (as for scalars). When an initializer IS
            // present it is validated below (after signatures are registered)
            // by the same check_value -> check_expr path fn bodies use, so
            // the StructLit/ArrayLit field/element shape checks fire there.
        }
    }

    // Pass 1.7 (Section 5, the central design move): rewrite every
    // EnumAccessExpr node IN PLACE to an IntLit carrying the variant's i32
    // value, so that by the time check_expr runs there are NO EnumAccessExpr
    // nodes left anywhere in the program. This is what lets codegen, the
    // const-folder (try_eval_const_i64), the switch case-value literal-check
    // (sema.cpp's SwitchStmt `dynamic_cast<IntLit*>`), and globals.hpp's
    // initializer evaluator all treat an enum variant as an ordinary i32
    // literal - they never see an EnumAccessExpr. Running this as a sema-
    // internal pass (NOT deferred to codegen) is what makes the rewrite visible
    // to sema's own switch check (docs/planning/plan_ENUMS.md Section 5 point 1), and is
    // why codegen stays untouched (codegen's eval() has no EnumAccessExpr
    // case and silently emits nothing for one - Section 5 point 3).
    void lower_enum_access_expr(ExprPtr& slot) {
        if (!slot) return;
        if (auto* ea = dynamic_cast<EnumAccessExpr*>(slot.get())) {
            // Tier 1 namespaces: Foo::x where Foo is a namespace (not an enum)
            // is a namespace-global access. Rewrite to an Ident with the
            // qualified name so check_expr resolves it as a global.
            if (namespace_names.count(ea->enum_name)) {
                std::string qualified = ea->enum_name + "::" + ea->variant;
                // check if the qualified global exists
                auto git = std::find_if(prog->globals.begin(), prog->globals.end(),
                    [&](const GlobalDecl& g) { return g.ns == ea->enum_name && g.name == ea->variant; });
                if (git != prog->globals.end()) {
                    auto id = std::make_unique<Ident>();
                    id->loc = ea->loc;
                    id->name = qualified;
                    slot = std::move(id);
                    return;
                }
                err("namespace '" + ea->enum_name + "' has no global '" + ea->variant + "'",
                    ea->loc.line, ea->loc.col);
                auto lit = std::make_unique<IntLit>();
                lit->loc = ea->loc; lit->v = 0;
                lit->ty = intern(type_i64());
                slot = std::move(lit);
                return;
            }
            auto eit = enum_values.find(ea->enum_name);
            if (eit == enum_values.end()) {
                err("unknown enum '" + ea->enum_name + "'", ea->loc.line, ea->loc.col);
                // rewrite to a placeholder IntLit anyway so check_expr doesn't
                // double-report via its catch-all (the error is already recorded).
                auto lit = std::make_unique<IntLit>();
                lit->loc = ea->loc; lit->v = 0;
                lit->ty = intern(make_prim(Prim::I32));
                slot = std::move(lit);
                return;
            }
            auto vit = eit->second.find(ea->variant);
            if (vit == eit->second.end()) {
                err("enum '" + ea->enum_name + "' has no variant '" + ea->variant + "'",
                    ea->loc.line, ea->loc.col);
                auto lit = std::make_unique<IntLit>();
                lit->loc = ea->loc; lit->v = 0;
                lit->ty = intern(make_prim(Prim::I32));
                slot = std::move(lit);
                return;
            }
            auto lit = std::make_unique<IntLit>();
            lit->loc = ea->loc;
            lit->v = vit->second;       // the resolved value, sign-extended into i64
            // Tier 1 typed enums (§6): a typed enum's variant literal carries
            // the typed-enum type (prim=backing, enum_name=enum) so a
            // `let c: Color = Color::Red` binding type-checks (Color==Color)
            // and a raw `let c: Color = 5` is rejected (5 stays a plain int,
            // see adapt_int_lit's typed-enum guard). An untyped enum's variant
            // stays a plain i32 (backward compat: `let x: i64 = Color::Blue`
            // + arithmetic on variants keep working exactly as before). ty is
            // still refinable by check_expr's IntLit case for a plain-int
            // expected context (adapt_int_lit re-types to the target int).
            auto tit = typed_enum_types.find(ea->enum_name);
            lit->ty = (tit != typed_enum_types.end()) ? tit->second
                                                      : intern(make_prim(Prim::I32));
            slot = std::move(lit);
            return;
        }
        // recurse into every child ExprPtr slot of compound nodes
        if (auto* b = dynamic_cast<BinExpr*>(slot.get())) {
            lower_enum_access_expr(b->lhs); lower_enum_access_expr(b->rhs);
        } else if (auto* u = dynamic_cast<UnaryExpr*>(slot.get())) {
            lower_enum_access_expr(u->operand);
        } else if (auto* c = dynamic_cast<CastExpr*>(slot.get())) {
            lower_enum_access_expr(c->operand);
        } else if (auto* c = dynamic_cast<CallExpr*>(slot.get())) {
            if (c->receiver) lower_enum_access_expr(c->receiver);
            for (auto& a : c->args) lower_enum_access_expr(a);
        } else if (auto* ix = dynamic_cast<IndexExpr*>(slot.get())) {
            lower_enum_access_expr(ix->base); lower_enum_access_expr(ix->index);
        } else if (auto* fl = dynamic_cast<FieldExpr*>(slot.get())) {
            lower_enum_access_expr(fl->base);
        } else if (auto* v = dynamic_cast<ViewExpr*>(slot.get())) {
            lower_enum_access_expr(v->base);
        } else if (auto* a = dynamic_cast<AssignExpr*>(slot.get())) {
            lower_enum_access_expr(a->target); lower_enum_access_expr(a->value);
        } else if (auto* t = dynamic_cast<TernaryExpr*>(slot.get())) {
            lower_enum_access_expr(t->cond);
            lower_enum_access_expr(t->then_e);
            lower_enum_access_expr(t->else_e);
        } else if (auto* sl = dynamic_cast<StructLit*>(slot.get())) {
            for (auto& kv : sl->fields) lower_enum_access_expr(kv.second);
        } else if (auto* al = dynamic_cast<ArrayLit*>(slot.get())) {
            for (auto& el : al->elements) lower_enum_access_expr(el);
        }
        // leaves (IntLit/FloatLit/BoolLit/StringLit/Ident/SizeofExpr): no children
    }
    void lower_enum_access_block(Block& b);
    void lower_enum_access_stmt(Stmt& s);

    void push_scope() { scopes.emplace_back(); }
    void pop_scope()  { scopes.pop_back(); }
    void declare(const std::string& n, const Type* t, bool is_const,
                 bool local_array_view = false, Loc loc = {0, 0},
                 const Type* array_elem_ty = nullptr) {
        if (scopes.empty()) return;
        for (const auto& v : scopes.back()) {
            if (v.name == n) {
                err("redeclaration of '" + n + "' in the same scope", loc.line, loc.col);
                return;
            }
        }
        scopes.back().push_back({n, t, is_const, local_array_view, array_elem_ty});
    }

    // iterable() hook (Tier 1, array case): infer the element type of an
    // array<T> handle from its creation expression. `array_new(elem_size, n)`
    // is the only way a handle is minted in script, and elem_size statically
    // determines the typed get/set family (1 -> u8, 4 -> f32, 8 -> i64). Used
    // both at the `let a = array_new(...)` declaration (to tag the Var) and at
    // the ForEachStmt iterable (for an inline `for (x in array_new(...))`).
    // Returns nullptr when the expression is not a (possibly cast-wrapped)
    // array_new call with a compile-time-constant elem_size.
    const Type* infer_array_elem_ty_from_call(const CallExpr* c) {
        if (!c || !c->is_native || c->native_binding_name != "array_new") return nullptr;
        if (c->args.size() < 1) return nullptr;
        int64_t esz = 0;
        if (!try_eval_const_i64(*c->args[0], esz)) return nullptr;
        if (esz == 1) return intern(make_prim(Prim::U8));
        if (esz == 4) return &type_f32();
        if (esz == 8) return &type_i64();
        return nullptr;  // unsupported elem size for typed for-each
    }
    // Unwrap a (possibly CastExpr-wrapped) initializer to find an array_new
    // call; returns the inferred element type or nullptr.
    const Type* infer_array_elem_ty_from_init(const Expr* init) {
        if (!init) return nullptr;
        if (auto* c = dynamic_cast<const CallExpr*>(init)) return infer_array_elem_ty_from_call(c);
        if (auto* cast = dynamic_cast<const CastExpr*>(init)) return infer_array_elem_ty_from_init(cast->operand.get());
        return nullptr;
    }
    // Infer the element type for a for-each iterable expression: an inline
    // array_new call, or a variable that was declared from one (tracked via
    // the Var::array_elem_ty tag, including aliases through a bare Ident
    // assignment). Returns nullptr if the iterable is not a provable array
    // handle (the caller then rejects it as a non-iterable).
    const Type* infer_iterable_array_elem_ty(const Expr& iter) {
        if (auto* c = dynamic_cast<const CallExpr*>(&iter)) {
            if (const Type* t = infer_array_elem_ty_from_call(c)) return t;
        }
        if (auto* id = dynamic_cast<const Ident*>(&iter)) {
            if (const Var* v = lookup_local_var(id->name)) {
                if (v->array_elem_ty) return v->array_elem_ty;
            }
        }
        return nullptr;
    }

    // NOTE (slice-escape safety, stage 1 vs stage 2): this function tracks
    // stack-backed slices (ViewExpr over a fixed array, a StringLit that
    // resolved to slice<u8>, and aliases through Ident/Ternary). The escape
    // GUARDS that consume it are: return (ReturnStmt), global-Ident-store
    // (AssignExpr Ident target), and global-rooted FieldExpr/IndexExpr-store
    // (AssignExpr else-if branch) — these close C1/C2a/C2b for both the
    // local_array_view class and the StringLit class.
    //
    // STAGE 2 (this pass): C3 + C5 are closed with a borrow/escape analysis
    // instead of a blanket call-site reject:
    //   - C3 (native retain): NativeSig gains a `retains` bool. A stack-backed
    //     slice passed to a `retains=true` native is rejected at the call site
    //     (the native stores the ptr past the call -> it dangles). A copying
    //     native (retains=false, the default; string_from_slice copies) is
    //     allowed. No shipped native retains, so C3 is "accidentally safe";
    //     the field + guard are the annotation surface for a future retaining
    //     native.
    //   - C5 (script fn / fn-handle / cross-module): for a direct named script
    //     call, a pre-pass computes per-fn `borrowed_params` (slice params that
    //     flow into the return) and `retained_params` (slice params stored to a
    //     global inside the fn). A RETAINED param rejects at the call site (the
    //     escape is inside the fn, invisible to the caller's guards). A BORROWED
    //     param does NOT reject at the call site — instead is_local_array_view()
    //     (below) recognizes the CallExpr as itself a stack-backed slice, so the
    //     call RESULT is tagged localview and the caller's OWN C1/C2a/C2b
    //     guards catch the escape at the actual escape point (return / global-
    //     store). This is what keeps return_slice_defer working: the result is
    //     tagged localview, but synchronous reads (r[0], r[1]) are not escape
    //     sites. Indirect (fn-handle) and cross-module calls can't see the
    //     callee body, so a stack-backed slice arg is conservatively rejected
    //     at the call site (sound; no existing test passes a slice to either).
    // See demo/SLICE_ESCAPE_SAFETY_INVESTIGATION.md §6.3/§8 and the ROADMAP's
    // slice-escape Stage 2 entry.
    bool is_local_array_view(const Expr& e) const {
        if (dynamic_cast<const ViewExpr*>(&e)) return true;
        // A StringLit that resolved to slice<u8> (NOT promoted to a `string`
        // handle via implicit_to_string) backs into a compiler-hidden stack
        // temp frame slot (codegen's alloc_str_temp) — the same lifetime shape
        // as a ViewExpr over a fixed array. Track it so the existing escape
        // guards (return, global-store) and the FieldExpr-global-store guard
        // below cover StringLit-derived slices too. A StringLit promoted to a
        // `string` handle has ty->is_slice == false (its ty is the `string`
        // struct type), so this returns false for it — correct (the handle is
        // owned and persistent). sl->ty is set in the StringLit check_expr
        // case before any guard can run.
        if (auto* sl = dynamic_cast<const StringLit*>(&e))
            return sl->ty && sl->ty->is_slice;
        if (auto* id = dynamic_cast<const Ident*>(&e)) {
            for (int i = int(scopes.size()) - 1; i >= 0; --i)
                for (const auto& v : scopes[size_t(i)])
                    if (v.name == id->name) return v.local_array_view;
        }
        if (auto* t = dynamic_cast<const TernaryExpr*>(&e))
            return is_local_array_view(*t->then_e) || is_local_array_view(*t->else_e);
        // C5 propagation (Stage 2): a direct named script call whose callee
        // BORROWS one of its slice params into the return value, called with a
        // stack-backed slice in that arg position, is ITSELF a stack-backed
        // slice — the returned slice aliases the caller's frame-local backing
        // store. Tag it so the caller's own C1 (return) / C2a (global-store) /
        // C2b (field-store) guards fire at the actual escape point, NOT at this
        // call. This is the key to keeping the synchronous pattern
        // `let r = return_slice_defer(s); r[0];` valid: r is tagged localview,
        // but r[0] is a read (not an escape), so no guard fires. A NATIVE call
        // is never localview here — a native's return value is host-owned (a
        // handle or a host slice), never an alias of the caller's frame. Only
        // direct named script calls (not indirect / cross-module — those can't
        // be proven to borrow) propagate.
        if (auto* c = dynamic_cast<const CallExpr*>(&e)) {
            if (!c->is_native && !c->is_indirect && c->module_alias.empty() && !c->name.empty()) {
                auto ssi = script_sigs.find(c->name);
                if (ssi != script_sigs.end()) {
                    for (size_t i = 0; i < c->args.size() && i < ssi->second.params.size(); ++i) {
                        if (ssi->second.borrowed_params.count(i) &&
                            is_local_array_view(*c->args[i]))
                            return true;
                    }
                }
            }
        }
        return false;
    }

    // --- slice-escape Stage 2 pre-pass: per-fn borrow/retain analysis ---
    // Does expression `e` reference the slice param named `paramName` (directly
    // as an Ident, via a ViewExpr over the param, via a CastExpr, via a
    // Ternary branch, or transitively via a call to a script fn that BORROWS
    // the arg position `e` is in)? The transitive case is what makes
    // `return relay(s)` (where relay returns its arg) count as borrowing s.
    // Only SLICE params are of interest (a non-slice param is copied by value;
    // only a slice ptr+len can dangle), but this predicate is purely syntactic
    // — the caller filters by slice-ness when populating the param sets.
    // `const` so it is usable from is_local_array_view's recursion path too.
    bool expr_refs_slice_param(const Expr& e, const std::string& paramName) const {
        if (auto* id = dynamic_cast<const Ident*>(&e))
            return id->name == paramName;
        if (auto* v = dynamic_cast<const ViewExpr*>(&e))
            return expr_refs_slice_param(*v->base, paramName);
        if (auto* t = dynamic_cast<const TernaryExpr*>(&e))
            return expr_refs_slice_param(*t->then_e, paramName) ||
                   expr_refs_slice_param(*t->else_e, paramName);
        if (auto* cast = dynamic_cast<const CastExpr*>(&e))
            return expr_refs_slice_param(*cast->operand, paramName);
        if (auto* c = dynamic_cast<const CallExpr*>(&e)) {
            // Transitive borrow: a direct named script call to a fn that
            // borrows arg j, where arg j refs paramName. Native / indirect /
            // cross-module calls don't carry borrowed_params, so they never
            // propagate (conservative — they don't help prove a borrow). The
            // fixed-point in compute_borrow_retain() converges this.
            if (!c->is_native && !c->is_indirect && c->module_alias.empty() && !c->name.empty()) {
                auto ssi = script_sigs.find(c->name);
                if (ssi != script_sigs.end()) {
                    for (size_t j = 0; j < c->args.size() && j < ssi->second.params.size(); ++j) {
                        if (ssi->second.borrowed_params.count(j) &&
                            expr_refs_slice_param(*c->args[j], paramName))
                            return true;
                    }
                }
            }
            return false;
        }
        return false;
    }

    // Is assignment target `target` rooted at a GLOBAL (a bare global Ident,
    // or a FieldExpr / IndexExpr chain whose root base is a global)? Mirrors
    // the C2b root-chase in check_expr's AssignExpr guard. A local-rooted
    // target is only unsafe if the local itself escapes (the harder struct-
    // escape analysis, a follow-on); the v1 cut tracks only global-rooted
    // stores as retained, matching Stage 1's C2b scope.
    bool assign_target_is_global_rooted(const Expr& target) const {
        const Expr* root = &target;
        while (true) {
            if (auto* fe = dynamic_cast<const FieldExpr*>(root)) { root = fe->base.get(); continue; }
            if (auto* ix = dynamic_cast<const IndexExpr*>(root)) { root = ix->base.get(); continue; }
            break;
        }
        if (auto* rid = dynamic_cast<const Ident*>(root)) {
            if (globals.count(rid->name)) return true;
        }
        return false;
    }

    // Scan a fn body for the C5 escape shapes the pre-pass needs:
    //   - return_values: every `return <expr>` value (a borrow source — does
    //     the value flow a slice param into the return?).
    //   - global_stores: every (target,value) where target is global-rooted
    //     (a retain source — does the value flow a slice param into a global?).
    //   - script_calls: every direct-named script CallExpr in the body (a
    //     transitive borrow/retain source — does a call to a borrowing/retaining
    //     fn with an arg that refs a slice param propagate the borrow/retain?).
    // Recurses through every block-bearing statement (if/while/for/do-while/
    // for-each/switch/match/block/defer). Used by compute_borrow_retain() per fn.
    struct BorrowScan {
        std::vector<const Expr*> return_values;
        std::vector<std::pair<const Expr*, const Expr*>> global_stores;
        std::vector<const CallExpr*> script_calls;
    };
    void scan_body_for_escapes(const Block& b, BorrowScan& out) const {
        for (const auto& s : b.stmts) {
            if (auto* rs = dynamic_cast<const ReturnStmt*>(s.get())) {
                if (rs->value) out.return_values.push_back(rs->value.get());
                continue;
            }
            if (auto* es = dynamic_cast<const ExprStmt*>(s.get())) {
                scan_expr_for_escapes(*es->expr, out);
                continue;
            }
            if (auto* ls = dynamic_cast<const LetStmt*>(s.get())) {
                if (ls->init) scan_expr_for_escapes(*ls->init, out);
                continue;
            }
            if (auto* ds = dynamic_cast<const DeferStmt*>(s.get())) {
                // A defer may carry an AssignExpr (defer g = s;) that stores to
                // a global at block exit — that is a retain too. Or a CallExpr
                // whose callee retains. Scan it like any other expr.
                if (ds->expr) scan_expr_for_escapes(*ds->expr, out);
                continue;
            }
            if (auto* is = dynamic_cast<const IfStmt*>(s.get())) {
                if (is->cond) scan_expr_for_escapes(*is->cond, out);
                scan_body_for_escapes(is->then_b, out);
                if (is->has_else) scan_body_for_escapes(is->else_b, out);
                continue;
            }
            if (auto* ws = dynamic_cast<const WhileStmt*>(s.get())) {
                if (ws->cond) scan_expr_for_escapes(*ws->cond, out);
                scan_body_for_escapes(ws->body, out);
                continue;
            }
            if (auto* fs = dynamic_cast<const ForStmt*>(s.get())) {
                if (fs->init && fs->init->init) scan_expr_for_escapes(*fs->init->init, out);
                if (fs->cond) scan_expr_for_escapes(*fs->cond, out);
                if (fs->step) scan_expr_for_escapes(*fs->step, out);
                scan_body_for_escapes(fs->body, out);
                continue;
            }
            if (auto* dw = dynamic_cast<const DoWhileStmt*>(s.get())) {
                scan_body_for_escapes(dw->body, out);
                if (dw->cond) scan_expr_for_escapes(*dw->cond, out);
                continue;
            }
            if (auto* fe = dynamic_cast<const ForEachStmt*>(s.get())) {
                if (fe->iter) scan_expr_for_escapes(*fe->iter, out);
                scan_body_for_escapes(fe->body, out);
                continue;
            }
            if (auto* bs = dynamic_cast<const BlockStmt*>(s.get())) {
                scan_body_for_escapes(bs->block, out);
                continue;
            }
            if (auto* sw = dynamic_cast<const SwitchStmt*>(s.get())) {
                if (sw->subject) scan_expr_for_escapes(*sw->subject, out);
                for (const auto& c : sw->cases) scan_body_for_escapes(c.body, out);
                continue;
            }
            if (auto* ms = dynamic_cast<const MatchStmt*>(s.get())) {
                if (ms->subject) scan_expr_for_escapes(*ms->subject, out);
                for (const auto& arm : ms->arms) scan_body_for_escapes(arm.body, out);
                continue;
            }
            // BreakStmt / ContinueStmt / StaticAssertStmt: no escape shapes.
        }
    }
    // Scan a single expression tree for nested AssignExprs (global-rooted ->
    // retain), direct-named script CallExprs (transitive borrow/retain
    // sources), and recurse through compound expressions. AssignExprs are
    // statement-level in practice but the recursion is defensive.
    void scan_expr_for_escapes(const Expr& e, BorrowScan& out) const {
        if (auto* a = dynamic_cast<const AssignExpr*>(&e)) {
            if (assign_target_is_global_rooted(*a->target))
                out.global_stores.push_back({a->target.get(), a->value.get()});
            // still recurse into value for nested assigns (rare)
            scan_expr_for_escapes(*a->value, out);
            return;
        }
        if (auto* b = dynamic_cast<const BinExpr*>(&e)) {
            scan_expr_for_escapes(*b->lhs, out);
            scan_expr_for_escapes(*b->rhs, out);
            return;
        }
        if (auto* u = dynamic_cast<const UnaryExpr*>(&e)) {
            scan_expr_for_escapes(*u->operand, out);
            return;
        }
        if (auto* c = dynamic_cast<const CallExpr*>(&e)) {
            // A direct named script call is a transitive borrow/retain source:
            // if the callee borrows/retains arg j and arg j refs one of THIS
            // fn's slice params, this fn borrows/retains that param. Collect
            // the call; compute_borrow_retain() checks it against the callee's
            // sets (which the fixed-point converges). Native / indirect /
            // cross-module calls carry no borrowed/retained sets, so they are
            // not collected (they don't help prove a borrow/retain here).
            if (!c->is_native && !c->is_indirect && c->module_alias.empty() && !c->name.empty())
                out.script_calls.push_back(c);
            if (c->receiver) scan_expr_for_escapes(*c->receiver, out);
            for (const auto& a : c->args) scan_expr_for_escapes(*a, out);
            if (c->indirect_target) scan_expr_for_escapes(*c->indirect_target, out);
            return;
        }
        if (auto* ix = dynamic_cast<const IndexExpr*>(&e)) {
            scan_expr_for_escapes(*ix->base, out);
            scan_expr_for_escapes(*ix->index, out);
            return;
        }
        if (auto* fe = dynamic_cast<const FieldExpr*>(&e)) {
            scan_expr_for_escapes(*fe->base, out);
            return;
        }
        if (auto* v = dynamic_cast<const ViewExpr*>(&e)) {
            scan_expr_for_escapes(*v->base, out);
            return;
        }
        if (auto* t = dynamic_cast<const TernaryExpr*>(&e)) {
            if (t->cond) scan_expr_for_escapes(*t->cond, out);
            scan_expr_for_escapes(*t->then_e, out);
            scan_expr_for_escapes(*t->else_e, out);
            return;
        }
        if (auto* cast = dynamic_cast<const CastExpr*>(&e)) {
            scan_expr_for_escapes(*cast->operand, out);
            return;
        }
        // IntLit/FloatLit/BoolLit/StringLit/Ident/SizeofExpr/OffsetofExpr/
        // StructLit/ArrayLit/EnumAccessExpr/FnHandleExpr: no nested escape
        // shapes to scan.
    }

    // Compute per-fn borrowed_params / retained_params for every script fn, via
    // a fixed-point. Each pass, per fn, scans the body for the three escape
    // sources and grows the sets:
    //   - BORROWED (return flows a slice param into the return value): a return
    //     value refs a slice param — directly (`return s`), via a view
    //     (`return s[..]`), or transitively (`return relay(s)` where relay
    //     borrows; expr_refs_slice_param handles the transitive CallExpr case).
    //   - RETAINED (a slice param stored to a global): a global-rooted
    //     assignment value refs a slice param (directly or via a retaining call
    //     in the value position).
    //   - TRANSITIVE (a call to a borrowing/retaining fn): for each direct
    //     named script call in the body, if the callee borrows/retains arg j
    //     and arg j refs one of THIS fn's slice params, this fn borrows/retains
    //     that param. This is what makes `wrapper(s) { store_to_global(s); }`
    //     retain param 0 (store_to_global retains; wrapper calls it with s).
    // The fixed-point repeats until no set grows (bounded by num_fns * num_slice
    // _params; converges in 1-2 passes in practice). Runs BEFORE any function
    // body is checked (in the script_sigs registration loop) so a forward call
    // from G to F (G checked first) already sees F's sets.
    void compute_borrow_retain() {
        bool changed = true;
        int guard = 0;
        while (changed && guard < 1024) {
            changed = false;
            ++guard;
            for (auto& f : prog->funcs) {
                auto ssi = script_sigs.find(f.name);
                if (ssi == script_sigs.end()) continue;
                // collect this fn's slice-param names (index -> name; empty for
                // non-slice params, which can't dangle and aren't tracked).
                std::vector<std::string> slice_param_names;
                for (size_t i = 0; i < f.params.size(); ++i) {
                    if (f.params[i].ty && f.params[i].ty->is_slice)
                        slice_param_names.push_back(f.params[i].name);
                    else
                        slice_param_names.emplace_back();
                }
                BorrowScan scan;
                scan_body_for_escapes(f.body, scan);
                // BORROWED: a slice param whose value flows into a return value.
                for (const Expr* rv : scan.return_values) {
                    for (size_t i = 0; i < slice_param_names.size(); ++i) {
                        if (slice_param_names[i].empty()) continue;
                        if (expr_refs_slice_param(*rv, slice_param_names[i])) {
                            if (ssi->second.borrowed_params.insert(i).second)
                                changed = true;
                        }
                    }
                }
                // RETAINED: a slice param stored to a global (directly, or via a
                // retaining call in the value position) inside the fn.
                for (auto& gs : scan.global_stores) {
                    for (size_t i = 0; i < slice_param_names.size(); ++i) {
                        if (slice_param_names[i].empty()) continue;
                        if (expr_refs_slice_param(*gs.second, slice_param_names[i])) {
                            if (ssi->second.retained_params.insert(i).second)
                                changed = true;
                        }
                    }
                }
                // TRANSITIVE borrow + retain: a call to a borrowing/retaining fn
                // with an arg that refs one of this fn's slice params.
                for (const CallExpr* c : scan.script_calls) {
                    auto callee_it = script_sigs.find(c->name);
                    if (callee_it == script_sigs.end()) continue;
                    const auto& callee = callee_it->second;
                    for (size_t j = 0; j < c->args.size() && j < callee.params.size(); ++j) {
                        const Expr& arg = *c->args[j];
                        for (size_t i = 0; i < slice_param_names.size(); ++i) {
                            if (slice_param_names[i].empty()) continue;
                            if (!expr_refs_slice_param(arg, slice_param_names[i])) continue;
                            if (callee.borrowed_params.count(j)) {
                                if (ssi->second.borrowed_params.insert(i).second)
                                    changed = true;
                            }
                            if (callee.retained_params.count(j)) {
                                if (ssi->second.retained_params.insert(i).second)
                                    changed = true;
                            }
                        }
                    }
                }
            }
        }
    }

    // C5 conservative reject for a call path whose callee body is NOT visible
    // to sema (an indirect / fn-handle call, or a cross-module call). For a
    // direct named script call we can compute borrowed_params/retained_params
    // and reject precisely (retain) or propagate (borrow). For these two paths
    // we can't see the callee, so we can't prove it doesn't retain the ptr —
    // conservatively reject any stack-backed slice arg. Sound: the worst case
    // is a false positive on a synchronous-reading callee, but no existing
    // test passes a stack-backed slice to an indirect or cross-module call
    // (fn-handle tests use i64; import tests pass no slices), so this closes
    // C5's last two paths without breaking the suite. The synchronous
    // return_slice_defer pattern is a DIRECT named call, so it is unaffected.
    void reject_local_view_slice_arg_opaque_callee(const CallExpr& c, const Expr& arg,
                                                    const std::string& callee_label,
                                                    size_t arg_one_based) {
        if (arg.ty && arg.ty->is_slice && is_local_array_view(arg))
            err(std::string("cannot pass a slice derived from a stack local to ") +
                callee_label + " (arg " + std::to_string(arg_one_based) +
                "); the callee's body is not visible at this call site, so it "
                "may retain the pointer past the frame. Materialize it to a "
                "`string` handle or a rodata/global-backed slice first.",
                arg.loc.line, arg.loc.col);
    }

    static bool is_lvalue(const Expr& e) {
        return dynamic_cast<const Ident*>(&e) || dynamic_cast<const IndexExpr*>(&e) ||
               dynamic_cast<const FieldExpr*>(&e);
    }

    // --- expression type-check. returns resolved type (or nullptr on error).
    // literal_int_as: when checking an IntLit in a context expecting a
    // specific integer type, adopt it (literal adaptation). nullptr = default i64.
    // allow_struct_ret_call: true only for a LetStmt initializer or a
    // ReturnStmt value - the only two positions where a struct-returning
    // CallExpr is allowed (codegen needs a concrete destination address for
    // the callee's hidden return pointer, not a value to materialize in a
    // register - see the CallExpr check below).
    const Type* check_expr(Expr& e, const Type* expected = nullptr, bool allow_struct_ret_call = false);

    // try to adapt an IntLit's type to `target` (a numeric int type) if value fits
    void adapt_int_lit(IntLit& lit, const Type* target) {
        if (!target || !target->is_int()) return;
        // Tier 1 typed enums (§6.3): never adapt a raw integer literal to a
        // typed-enum type. This is what rejects `let c: Color = 5` — the
        // literal 5 is NOT re-typed to Color, so it stays a plain int and the
        // let's type compatibility check fails (int->enum is not implicit). A
        // Color::Red variant literal, by contrast, already carries ty=Color
        // (set by lower_enum_access_expr) and is never passed through here
        // with a typed-enum target it would need to adopt.
        if (!target->enum_name.empty()) return;
        // Symmetric guard: a literal that ALREADY carries a typed-enum type
        // (a variant literal, e.g. Color::Red which is an IntLit with
        // ty=Color backing i32) must NOT be re-typed to a plain integer here.
        // Without this, `let x: i16 = Color::Red` and `let x: u8 = Color::Red`
        // would silently narrow / change signedness of the i32-backed enum
        // value (adapt_int_lit re-types by raw value fit alone, discarding
        // the enum's backing width + signedness constraint) — a downgrade
        // that the spec (Section 6) makes explicit-only and that an enum
        // *variable* is already correctly rejected for. Keeping the enum
        // type routes the value through check_value's enum->int widening
        // gate (can_implicitly_convert: backing width <= target, same
        // signedness), so a widening target still accepts and a narrowing /
        // signedness-change target correctly errors. Codegen handles the
        // synthesized enum->int CastExpr via the same normalize_rax path a
        // plain int cast uses (a typed enum's backing value is in rax).
        if (lit.ty && !lit.ty->enum_name.empty()) return;
        // only adopt if the literal currently is default i64 and fits target
        int64_t v = lit.v;
        switch (target->prim) {
        case Prim::U8:  if (v >= 0 && v <= 0xFF)        { lit.ty = target; return; } break;
        case Prim::U16: if (v >= 0 && v <= 0xFFFF)     { lit.ty = target; return; } break;
        case Prim::U32: if (v >= 0 && v <= 0xFFFFFFFFLL){ lit.ty = target; return; } break;
        // U64: the lexer bit-casts the full u64 range into a signed int64_t, so
        // values >= 2^63 appear negative as int64_t. Any 64-bit bit pattern is a
        // valid u64, so accept unconditionally (no `>= 0` guard).
        case Prim::U64: lit.ty = target; return;
        case Prim::I8:  if (v >= -128 && v <= 127)     { lit.ty = target; return; } break;
        case Prim::I16: if (v >= -32768 && v <= 32767) { lit.ty = target; return; } break;
        case Prim::I32: if (v >= -2147483648LL && v <= 2147483647LL){ lit.ty = target; return; } break;
        case Prim::I64: lit.ty = target; return;
        default: break;
        }
    }

    // --- statements ---
    void check_block(Block& b, const Type* ret_ty, bool& returns);
    void check_stmt(Stmt& s, const Type* ret_ty, bool& returns);
    void check_func(FuncDecl& f);

    // @realtime contract validation runs after ordinary type checking has
    // resolved every CallExpr. It is intentionally conservative for native
    // calls: only the explicitly-audited pure/math/buffer surface is allowed.
    void validate_realtime(FuncDecl& f);
    void validate_realtime_block(const Block& b);
    void validate_realtime_stmt(const Stmt& s);
    void validate_realtime_expr(const Expr& e);
    // Tier 1 static_assert: fold cond + resolve (true -> elided, false ->
    // compile error with msg, non-const -> compile error). Shared by the
    // in-body check_stmt path and the top-level prog.static_asserts pass so
    // both positions apply the identical compile-time verdict.
    void check_static_assert(StaticAssertStmt& sa);

    // --- constexpr fn evaluation (Tier 1) ---
    // A tree-walking interpreter that evaluates a constexpr fn call at sema
    // time to produce a compile-time i64 result. Bounded: max 256 recursion
    // depth (nested constexpr calls) and a TOTAL iteration budget of
    // CE_MAX_TOTAL_ITERS across ALL loops in one eval (see ce_eval_stmt's loop
    // cases). The total budget is what makes the bound real: a per-loop counter
    // alone is bypassed by NESTED loops (N nested 100k loops = 100k^N iters,
    // a compile-time DoS). The shared counter credits every loop body iteration
    // across every nesting level against one budget, so nesting cannot multiply
    // past it. Only i64 integer fns in this increment; float/bool/struct fns
    // skip constexpr eval and fall back to runtime calls.
    static constexpr int64_t CE_MAX_TOTAL_ITERS = 100000;
    struct ConstEvalCtx {
        std::vector<std::unordered_map<std::string, int64_t>> scopes;
        int64_t result = 0;
        bool returned = false;
        bool broke = false;
        bool continued = false;
        int depth = 0;
        int64_t total_iters = 0;  // shared across ALL loops in this eval (DoS bound)
    };
    bool eval_constexpr_fn(const FuncDecl& fn, const std::vector<int64_t>& arg_values,
                           int64_t& result_out, std::string& err, int depth = 0);
    bool ce_eval_expr(const Expr& e, int64_t& out, ConstEvalCtx& ctx, std::string& err);
    bool ce_eval_block(const Block& b, ConstEvalCtx& ctx, std::string& err);
    bool ce_eval_stmt(const Stmt& s, ConstEvalCtx& ctx, std::string& err);

    // Pre-pass: fold constexpr calls into IntLits BEFORE check_expr runs
    // (mirrors lower_enum_access_expr's pattern - walks ExprPtr& slots so
    // nodes can be replaced in place). Runs bottom-up so nested constexpr
    // calls fold inner-first.
    void lower_constexpr_calls_expr(ExprPtr& slot);
    void lower_constexpr_calls_block(Block& b);
    void lower_constexpr_calls_stmt(Stmt& s);
    void try_fold_constexpr_call(ExprPtr& slot);

    // --- #20 lambdas with by-value capture ---
    // Walk a lambda body to find Idents that resolve to a local in an
    // ENCLOSING scope (scope index < lambda_scope_start). Those are the
    // by-value captures. Globals, fn names, and the lambda's own params/
    // locals are NOT captures. Dedups by name; preserves first-encounter
    // order (which becomes the env field order).
    void collect_captures_expr(const Expr& e, size_t lambda_scope_start,
                               std::vector<std::string>& out);
    void collect_captures_block(const Block& b, size_t lambda_scope_start,
                               std::vector<std::string>& out);
    void collect_captures_stmt(const Stmt& s, size_t lambda_scope_start,
                               std::vector<std::string>& out);
    // Type-check a LambdaExpr: determine captures, build the env, prepend the
    // hidden __env param to the synthetic fn, check the body, set the type.
    const Type* check_lambda(LambdaExpr& le);
    // Check a synthetic lambda fn's body (called from check_func when the fn
    // is a lambda). Binds __env + the captures (as env-capture vars) + the
    // declared params, then checks the body.
    void check_lambda_func(FuncDecl& f);
    // #20 pre-pass: upgrade `fn(Args)->Ret` param types to is_lambda where a
    // call site passes a lambda to that param. Walks all fn bodies for
    // CallExpr args that are LambdaExprs.
    void upgrade_lambda_param_types();
    void upgrade_lambda_params_block(Block& b, std::unordered_set<std::string>& lam_locals);
    void upgrade_lambda_params_stmt(Stmt& s, std::unordered_set<std::string>& lam_locals);
    void upgrade_lambda_params_expr(Expr& e, const std::unordered_set<std::string>& lam_locals);
};

const Type* Checker::check_expr(Expr& e, const Type* expected, bool allow_struct_ret_call) {
    if (++check_depth > MAX_CHECK_DEPTH) {
        --check_depth;
        throw SemaError{"recursion depth exceeded (expression too deeply nested)", e.loc.line, e.loc.col};
    }
    struct DepthRestore { int& d; ~DepthRestore() { --d; } } dr{check_depth};
    // default: literals get expected type via adaptation where applicable
    if (auto* lit = dynamic_cast<IntLit*>(&e)) {
        if (expected) adapt_int_lit(*lit, expected);
        if (!lit->ty) lit->ty = intern(make_prim(Prim::I64));
        e.ty = lit->ty;
        return e.ty;
    }
    if (auto* lit = dynamic_cast<FloatLit*>(&e)) {
        // `f` suffix is authoritative (f32); else adapt to expected or default f64
        if (lit->is_f32) lit->ty = intern(make_prim(Prim::F32));
        else if (expected && expected->prim == Prim::F32) lit->ty = intern(make_prim(Prim::F32));
        else lit->ty = intern(make_prim(Prim::F64));
        e.ty = lit->ty; return e.ty;
    }
    if (auto* lit = dynamic_cast<BoolLit*>(&e)) { e.ty = lit->ty = &type_bool(); return e.ty; }
    if (auto* lit = dynamic_cast<StringLit*>(&e)) {
        // string literal -> slice<u8> (rodata, docs/spec/MEMORY_AND_GC.md Section 6). Bake a
        // stable-address copy into the Program's rodata store so codegen can
        // emit an absolute pointer immediate (same lifetime contract as the
        // type_store above - must outlive codegen/JIT execution).
        // String encryption (default): XOR the bytes with a
        // per-compile key before storing. The raw string never appears in
        // the JIT'd exec memory - only the encrypted bytes are in rodata,
        // and codegen decrypts inline into a temp frame slot at each use
        // site (the plaintext is transient - stack-scoped, reclaimed at frame
        // teardown; no host native, no heap, no leak).
        e.ty = lit->ty = intern(make_slice(std::make_shared<Type>(make_prim(Prim::U8))));
        uint8_t key = prog->string_xor_key;
        auto enc = std::make_shared<std::string>(lit->s);
        if (key != 0) {
            for (auto& c : *enc) c ^= key;
            lit->encrypted = true;
        }
        lit->baked_key = key;
        lit->baked_ptr = reinterpret_cast<const uint8_t*>(enc->data());
        lit->baked_len = int64_t(enc->size());
        prog->rodata_store.push_back(std::move(enc));
        // Implicit conversion: when a `string` (not slice<u8>, and not
        // anything else - e.g. today's print_str(uint8_t*,int64_t) natives
        // stay exactly as they are, since their param type is slice<u8>, not
        // string) is expected here, the literal still bakes as slice<u8>
        // rodata above (the conversion call needs a slice to convert FROM),
        // but its resolved type becomes `string` and codegen (see StringLit's
        // codegen case) emits a real string_from_slice(ptr,len) call right
        // after materializing that slice, using its handle result instead.
        // This is the ONLY implicit conversion in the language other than
        // numeric literal adaptation just above - gated on an exact match of
        // `expected` so no existing bare-literal-as-slice call site changes.
        //
        // `expected == nullptr` also takes this branch: by construction, the
        // only caller that checks an expression with no expected type at all
        // is a type-less `let x = ...;`/`auto x = ...;` (every other call
        // site - a typed let, a return, a call argument, a binary operand -
        // always has SOME expected type to pass down). Defaulting THAT case
        // to `u8[]` was a silent trap: `let s = "literal";` then couldn't be
        // passed to any native expecting `string` (e.g. print_string) without
        // an explicit `: string` annotation, since sema has no later pass
        // that revisits an already-declared variable's type based on how
        // it's subsequently used. `string` is the far more useful default
        // for "a variable holding a string literal, no annotation given."
        if (!expected || (expected->prim == Prim::I64 && expected->struct_name == "string")) {
            auto nit = natives->find("string_from_slice");
            if (nit != natives->end()) {
                lit->implicit_to_string = true;
                lit->to_string_native_fn = nit->second.fn_ptr;
                lit->to_string_native_name = nit->first;
                // `expected` is null in the "no context at all" case this
                // branch now also covers - build the `string` Type directly
                // rather than reusing `expected` itself (which would just
                // set the resolved type to null).
                const Type* string_ty = expected;
                if (!string_ty) { Type t; t.prim = Prim::I64; t.struct_name = "string"; string_ty = intern(t); }
                e.ty = lit->ty = string_ty;
            }
        }
        return e.ty;
    }
    if (auto* id = dynamic_cast<Ident*>(&e)) {
        const Type* t = lookup_var(id->name);
        if (!t) { err("undefined name '" + id->name + "'", id->loc.line, id->loc.col); e.ty = intern(type_void()); return e.ty; }
        e.ty = t; return t;
    }
    // v1.0 Tier 2 (docs/planning/plan_FUNCTION_REFS.md §4.2): `&fn_name` takes a function handle.
    // The operand must be an Ident naming a script function of this module.
    // The handle IS the slot index, baked as an i64 literal — a COMPILE-TIME
    // reification, not a runtime value. We stash the slot on the AST node for
    // codegen; the first-class-ness lives at the CALL site (handle(args)), not here.
    if (auto* le = dynamic_cast<LambdaExpr*>(&e)) {
        return check_lambda(*le);
    }
    if (auto* h = dynamic_cast<FnHandleExpr*>(&e)) {
        // v1.0 Tier 2 cross-module handle `&mod::fn` (docs/MODULES.md + plan_FUNCTION_REFS.md):
        // resolve against the host-provided ModuleExportTable, stamp the
        // target module's registry id + the fn's slot, and build a
        // cross-module-tagged fn-handle type carrying the export's recorded
        // signature. Unlike a cross-module CALL (which defers an unresolved
        // module to a runtime trap — the module may register later), a HANDLE
        // bakes its (module_id, slot) into the call site at the `&mod::fn`
        // point; if the module/fn is not resolvable NOW there is no valid value
        // to bake, so it is a hard sema error (not a deferred trap). The
        // runtime guard still validates a (forged) cross-module handle against
        // the target module's allowlist before dispatch (REDSHELL #6 lifted
        // cross-module), so forged out-of-range / unregistered handles trap.
        if (h->is_cross_module) {
            if (!module_exports) {
                err("'&" + h->module_alias + "::" + h->fn_name +
                    "' cannot resolve: no modules are linked in this module",
                    h->loc.line, h->loc.col);
                e.ty = intern(type_void()); return e.ty;
            }
            auto mit = module_exports->find(h->module_alias);
            if (mit == module_exports->end()) {
                err("'&" + h->module_alias + "::" + h->fn_name +
                    "' cannot resolve module '" + h->module_alias +
                    "' (it is not linked; a cross-module handle requires the "
                    "target module to be registered at compile time)",
                    h->loc.line, h->loc.col);
                e.ty = intern(type_void()); return e.ty;
            }
            for (const auto& exp : mit->second) {
                if (exp.fn_name == h->fn_name) {
                    h->cross_module_unresolved = false;
                    h->cross_module_id = exp.module_id;
                    h->cross_module_slot = exp.slot;
                    Type t = type_i64();
                    t.is_fn_handle = true;
                    t.is_cross_module_handle = true;
                    if (!exp.unknown_sig) {
                        t.has_recorded_sig = true;
                        for (const auto& p : exp.params)
                            t.recorded_params.push_back(std::make_shared<Type>(p));
                        t.recorded_ret = std::make_shared<Type>(exp.ret);
                    }
                    e.ty = intern(t); return e.ty;
                }
            }
            // F1 visibility (mirrors the cross-module CALL path): the module IS
            // registered but no exported fn matches — the target is private or
            // undefined, so it is not part of the module's public surface.
            err("'&" + h->module_alias + "::" + h->fn_name +
                "' targets a function that is not exported by module '" +
                h->module_alias + "' (it is private or undefined)",
                h->loc.line, h->loc.col);
            e.ty = intern(type_void()); return e.ty;
        }
        auto* id = dynamic_cast<Ident*>(h->operand.get());
        if (!id) {
            err("'&' may only be applied to a function name, not an expression", h->loc.line, h->loc.col);
            e.ty = intern(type_void()); return e.ty;
        }
        auto sit = script_slots->find(id->name);
        if (sit == script_slots->end()) {
            err("'" + id->name + "' is not a script function in this module "
                "(cannot take a handle of a native or an unknown name)",
                h->loc.line, h->loc.col);
            e.ty = intern(type_void()); return e.ty;
        }
        h->slot = sit->second;
        // Build a fn-handle type carrying the source fn's recorded signature,
        // so a `let h = &fib; h(5);` call checks 5 against fib's (i64) param (§4.4).
        Type t = type_i64();
        t.is_fn_handle = true;
        for (auto& f : prog->funcs) {
            if (f.name == id->name) {
                t.has_recorded_sig = true;
                for (auto& p : f.params) t.recorded_params.push_back(p.ty);
                t.recorded_ret = std::make_shared<Type>(*f.ret);
                break;
            }
        }
        e.ty = intern(t); return e.ty;
    }
    if (auto* c = dynamic_cast<CallExpr*>(&e)) {
        // v1.0 Tier 2 first-class call (docs/planning/plan_FUNCTION_REFS.md §4.3): handle(args).
        // The target is a RUNTIME i64 fn handle, not a compile-time-known name.
        // Type the target; it must be a fn handle. If it carries a recorded sig,
        // check args against it; if bare `fn` (no recorded sig, e.g. a param),
        // args are unchecked at the type level — the runtime guard (§5) still
        // validates the handle is a registered slot before dispatch.
        if (c->indirect_target) {
            const Type* tt = check_expr(*c->indirect_target);
            if (!tt || !tt->is_fn_handle) {
                err("call target must be a function handle (fn), got '" +
                    (tt ? tt->to_string() : std::string("?")) + "'",
                    c->loc.line, c->loc.col);
                e.ty = intern(type_void()); return e.ty;
            }
            c->is_indirect = true;
            if (tt->has_recorded_sig) {
                size_t nargs = c->args.size() + (c->receiver ? 1 : 0);
                if (nargs != tt->recorded_params.size()) {
                    err("function handle call has " + std::to_string(nargs) +
                        " arg(s) but the function takes " +
                        std::to_string(tt->recorded_params.size()),
                        c->loc.line, c->loc.col);
                    e.ty = intern(type_void()); return e.ty;
                }
                size_t i = 0;
                if (c->receiver) {
                    const Type* want = tt->recorded_params[0].get();
                    const Type* got = check_value(c->receiver, want);
                    if (!types_compatible(want, got))
                        err("function handle receiver type mismatch (expected " + want->to_string() +
                            ", got " + got->to_string() + ")", c->receiver->loc.line, c->receiver->loc.col);
                    // C5 (indirect / fn-handle call): callee body not visible.
                    reject_local_view_slice_arg_opaque_callee(*c, *c->receiver,
                        "a function handle call", 0);
                    ++i;
                }
                for (auto& a : c->args) {
                    const Type* want = tt->recorded_params[i++].get();
                    const Type* got = check_value(a, want);
                    if (!types_compatible(want, got))
                        err("function handle argument type mismatch (expected " + want->to_string() +
                            ", got " + got->to_string() + ")", a->loc.line, a->loc.col);
                    // C5 (indirect / fn-handle call): callee body not visible.
                    reject_local_view_slice_arg_opaque_callee(*c, *a,
                        "a function handle call", i);
                }
                e.ty = intern(*tt->recorded_ret);
            } else {
                for (auto& a : c->args) check_expr(*a);
                e.ty = intern(type_i64());   // default ret for a bare-`fn` call
            }
            return e.ty;
        }
        // f-string interpolation sentinel (Item D - synthesized by the
        // parser's build_fstring_expr around every {expr} segment, never
        // user-callable). Resolve BEFORE the normal natives lookup below:
        // check the sole inner sub-expression with no type hint, then
        // retarget this call to whichever real converter fits its resolved
        // type, reusing CallExpr's existing is_native/native_fn stamp
        // fields - no new AST fields needed, and codegen (which only ever
        // sees a normal resolved-native CallExpr by the time it runs) needs
        // no changes at all.
        if (c->name == "__fstring_to_string") {
            const Type* it = check_expr(*c->args[0]);
            std::string conv;
            if (it && it->prim==Prim::I64 && it->struct_name=="string") conv = "string_identity";
            else if (it && it->is_bool())                               conv = "string_from_bool";
            else if (it && it->is_int())                                conv = "string_from_i64"; // any int width funnels here - codegen treats every width as a uniform 8-byte value
            else if (it && it->prim==Prim::F32)                         conv = "string_from_f32";
            else if (it && it->prim==Prim::F64)                         conv = "string_from_f64";
            else if (it && it->is_slice && it->elem && it->elem->prim==Prim::U8) conv = "string_from_slice";
            else {
                err("cannot interpolate a value of type '" + (it ? it->to_string() : std::string("?")) +
                    "' into an f-string", c->loc.line, c->loc.col);
                e.ty = intern(type_void()); return e.ty;
            }
            auto cit = natives->find(conv);
            if (cit == natives->end()) {
                err("internal: missing native '" + conv + "'", c->loc.line, c->loc.col);
                e.ty = intern(type_void()); return e.ty;
            }
            c->name = conv;
            c->is_native = true;
            c->native_fn = cit->second.fn_ptr;
            c->native_binding_name = cit->first;
            e.ty = intern(cit->second.ret);
            return e.ty;
        }
        // v0.5 cross-module call `mod::fn(args)` (docs/MODULES.md §6). Resolve against
        // the host-provided module export table. Resolved -> stamp the call's
        // cross_module_id/slot + ret type (so the arg/return type-check below
        // still works). Unresolved -> deferred trap (NOT a hard error — the
        // module may register later, §5 step 1/3); ret type is i64 (the handle/
        // int default — arg checking is skipped, codegen emits a trap stub).
        if (!c->module_alias.empty()) {
            // Tier 1 namespaces: if the alias is a namespace (not a module),
            // resolve Foo::bar as a same-module call to the qualified name.
            if (namespace_names.count(c->module_alias)) {
                std::string qualified = c->module_alias + "::" + c->name;
                auto ssi = script_sigs.find(qualified);
                if (ssi != script_sigs.end()) {
                    // Found a namespaced fn. Rewrite as a same-module call:
                    // clear module_alias, set name to qualified, fall through
                    // to the normal same-module resolution + arg type-check.
                    c->module_alias.clear();
                    c->cross_module_unresolved = false;
                    c->name = qualified;
                    goto namespace_resolved;
                } else {
                    err("namespace '" + c->module_alias + "' has no function '" + c->name + "'",
                        c->loc.line, c->loc.col);
                    for (auto& a : c->args) check_expr(*a);
                    e.ty = intern(type_i64());
                    return e.ty;
                }
            }
            c->cross_module_unresolved = true;  // assume unresolved until found
            if (module_exports) {
                auto mit = module_exports->find(c->module_alias);
                if (mit != module_exports->end()) {
                    for (const auto& exp : mit->second) {
                        if (exp.fn_name == c->name) {
                            c->cross_module_unresolved = false;
                            c->cross_module_id = int(exp.module_id);
                            c->cross_module_slot = exp.slot;
                            if (exp.unknown_sig) {
                                // .em export: signature not in the name table (the .em was
                                // type-checked at compile time). Skip arity/return checks;
                                // still type-check each arg standalone (no expected hint) so
                                // malformed args surface. Default ret to i64.
                                for (auto& a : c->args) {
                                    check_expr(*a);
                                    // C5 (cross-module, unknown sig): callee body not
                                    // visible — conservatively reject a stack-backed slice.
                                    reject_local_view_slice_arg_opaque_callee(*c, *a,
                                        "cross-module call '" + c->module_alias + "::" + c->name + "'",
                                        size_t(&a - &c->args[0]) + 1);
                                }
                                e.ty = intern(type_i64());
                                return e.ty;
                            }
                            // JIT export: type the call from the export's ret; check arity.
                            size_t nargs = c->args.size() + (c->receiver ? 1 : 0);
                            if (nargs != exp.params.size()) {
                                err("cross-module call '" + c->module_alias + "::" + c->name +
                                    "' expects " + std::to_string(exp.params.size()) +
                                    " args, got " + std::to_string(nargs),
                                    c->loc.line, c->loc.col);
                            }
                            // check each arg's type against the export's param types
                            for (size_t i = 0; i < c->args.size() && i < exp.params.size(); ++i) {
                                const Type* got = check_value(c->args[i], &exp.params[i]);
                                if (!types_compatible(&exp.params[i], got))
                                    err("cross-module arg " + std::to_string(i) + " type mismatch (expected " +
                                        exp.params[i].to_string() + ", got " + got->to_string() + ")",
                                        c->loc.line, c->loc.col);
                                // C5 (cross-module, JIT export): callee body not visible
                                // — conservatively reject a stack-backed slice.
                                reject_local_view_slice_arg_opaque_callee(*c, *c->args[i],
                                    "cross-module call '" + c->module_alias + "::" + c->name + "'",
                                    i + 1);
                            }
                            e.ty = intern(exp.ret);
                            return e.ty;
                        }
                    }
                    // F1 visibility (docs/spec/SPEC_AUDIT_2026-07-10.md F1): the
                    // module IS registered (its alias is in the export table) but
                    // no exported fn matches `c->name`. The callee is either a
                    // module-private helper (`priv fn`) or does not exist; either
                    // way it is NOT part of the module's public surface, so a cross-
                    // module call to it is a compile error, not a deferred trap. The
                    // deferred-trap path below is reserved for a module that has not
                    // registered YET (may register later, §5 step 1/3).
                    err("cross-module call '" + c->module_alias + "::" + c->name +
                        "' targets a function that is not exported by module '" +
                        c->module_alias + "' (it is private or undefined)",
                        c->loc.line, c->loc.col);
                    // Type the call so downstream expr use doesn't cascade; args are
                    // still checked standalone to surface any other errors.
                    for (auto& a : c->args) check_expr(*a);
                    e.ty = intern(type_i64());
                    return e.ty;
                }
            }
            // unresolved: deferred trap. Default ret type to i64 so the call
            // is usable in i64 contexts (the most common cross-module return).
            e.ty = intern(type_i64());
            return e.ty;
        }
        namespace_resolved:
        // resolve: native first, then script function
        auto nit = natives->find(c->name);
        auto sit = script_slots->find(c->name);
        if (nit == natives->end() && sit == script_slots->end()) {
            // Tier 1 namespaces: if we're inside a namespace, try the qualified
            // name (CurrentNs::fn) before giving up.
            if (!current_ns.empty()) {
                std::string qualified = current_ns + "::" + c->name;
                auto qsit = script_slots->find(qualified);
                auto qnit = natives->find(qualified);
                if (qsit != script_slots->end() || qnit != natives->end()) {
                    c->name = qualified;
                    sit = qsit;
                    nit = qnit;
                    // fall through to the normal resolution below with the qualified name
                    goto resolved_qualified;
                }
            }
            // v1.0 Tier 2 (docs/planning/plan_FUNCTION_REFS.md §4.3): `name(args)` where `name`
            // is neither a native nor a script fn — but it might be a LOCAL
            // VARIABLE of fn-handle type (the `let h = &fn; h(args);` case, which
            // parses identically to a named call). Promote it to an indirect call:
            // move an Ident referring to the var into indirect_target + recurse.
            // This closes the parse-time ambiguity (the parser can't know if `h`
            // is a fn name or a fn-typed var; sema resolves it here).
            const Var* v = lookup_local_var(c->name);
            // #20 lambda call: `f(args)` where f is a lambda-typed local. A
            // lambda value is {slot, env_ptr}; the call prepends env_ptr as the
            // hidden first arg + dispatches via the slot. Checked here (before
            // the fn-handle path) because a lambda type is is_lambda, NOT
            // is_fn_handle — the two are distinct value shapes.
            if (v && v->ty && v->ty->is_lambda) {
                auto hid = std::make_unique<Ident>();
                hid->loc = c->loc; hid->name = c->name; hid->ty = v->ty;
                c->lambda_target = std::move(hid);
                c->is_lambda_call = true;
                // arity + arg type check against the lambda's recorded sig
                // (the declared params, NOT the hidden env param).
                size_t nargs = c->args.size() + (c->receiver ? 1 : 0);
                if (nargs != v->ty->recorded_params.size()) {
                    err("lambda call has " + std::to_string(nargs) +
                        " arg(s) but the lambda takes " +
                        std::to_string(v->ty->recorded_params.size()),
                        c->loc.line, c->loc.col);
                    e.ty = intern(type_void()); return e.ty;
                }
                size_t i = 0;
                if (c->receiver) {
                    const Type* want = v->ty->recorded_params[0].get();
                    const Type* got = check_value(c->receiver, want);
                    if (!types_compatible(want, got))
                        err("lambda receiver type mismatch (expected " + want->to_string() +
                            ", got " + got->to_string() + ")", c->receiver->loc.line, c->receiver->loc.col);
                    ++i;
                }
                for (auto& a : c->args) {
                    const Type* want = v->ty->recorded_params[i++].get();
                    const Type* got = check_value(a, want);
                    if (!types_compatible(want, got))
                        err("lambda argument type mismatch (expected " + want->to_string() +
                            ", got " + got->to_string() + ")", a->loc.line, a->loc.col);
                }
                e.ty = intern(*v->ty->recorded_ret);
                c->name.clear();
                return e.ty;
            }
            if (v && v->ty && v->ty->is_fn_handle) {
                auto hid = std::make_unique<Ident>();
                hid->loc = c->loc; hid->name = c->name; hid->ty = v->ty;
                c->indirect_target = std::move(hid);
                c->is_indirect = true;
                c->name.clear();
                // re-run as an indirect call (the indirect path at the top of
                // the CallExpr case). Re-check this same CallExpr by recursing.
                const Type* tt = v->ty;
                if (tt->has_recorded_sig) {
                    size_t nargs = c->args.size() + (c->receiver ? 1 : 0);
                    if (nargs != tt->recorded_params.size()) {
                        err("function handle call has " + std::to_string(nargs) +
                            " arg(s) but the function takes " +
                            std::to_string(tt->recorded_params.size()),
                            c->loc.line, c->loc.col);
                        e.ty = intern(type_void()); return e.ty;
                    }
                    size_t i = 0;
                    if (c->receiver) {
                        const Type* want = tt->recorded_params[0].get();
                        const Type* got = check_value(c->receiver, want);
                        if (!types_compatible(want, got))
                            err("function handle receiver type mismatch (expected " + want->to_string() +
                                ", got " + got->to_string() + ")", c->receiver->loc.line, c->receiver->loc.col);
                        // C5 (indirect / fn-handle call): callee body not visible.
                        reject_local_view_slice_arg_opaque_callee(*c, *c->receiver,
                            "a function handle call", 0);
                        ++i;
                    }
                    for (auto& a : c->args) {
                        const Type* want = tt->recorded_params[i++].get();
                        const Type* got = check_value(a, want);
                        if (!types_compatible(want, got))
                            err("function handle argument type mismatch (expected " + want->to_string() +
                                ", got " + got->to_string() + ")", a->loc.line, a->loc.col);
                        // C5 (indirect / fn-handle call): callee body not visible.
                        reject_local_view_slice_arg_opaque_callee(*c, *a,
                            "a function handle call", i);
                    }
                    e.ty = intern(*tt->recorded_ret);
                } else {
                    for (auto& a : c->args) check_expr(*a);
                    e.ty = intern(type_i64());
                }
                return e.ty;
            }
            err("unknown function '" + c->name + "'", c->loc.line, c->loc.col);
            e.ty = intern(type_void()); return e.ty;
        }
        resolved_qualified:
        const Type* ret_ty;
        const std::vector<Type>* params;
        if (nit != natives->end()) {
            // permission gate (docs/spec/SAFETY_AND_SANDBOX.md Section 6): a native
            // flagged PERM_FFI is only callable from a module compiled with the
            // FFI permission bit. Compile-time check (zero runtime cost).
            if ((nit->second.permission & PERM_FFI) && !(perms & PERM_FFI)) {
                err("function '" + c->name + "' requires PERM_FFI permission",
                    c->loc.line, c->loc.col);
            }
            c->is_native = true; c->native_fn = nit->second.fn_ptr;
            c->native_binding_name = nit->first;
            ret_ty = &nit->second.ret; params = &nit->second.params;
            // arity + arg type check (method call: receiver is arg[0])
            size_t nargs = c->args.size() + (c->receiver ? 1 : 0);
            if (nargs != params->size()) {
                err("function '" + c->name + "' expects " + std::to_string(params->size()) +
                    " args, got " + std::to_string(nargs), c->loc.line, c->loc.col);
            } else {
                size_t off = 0;
                if (c->receiver) {
                    const Type* want = &(*params)[0];
                    const Type* got = check_value(c->receiver, want);
                    if (!types_compatible(want, got))
                        err("receiver of '" + c->name + "': expected " +
                            want->to_string() + ", got " + got->to_string(),
                            c->receiver->loc.line, c->receiver->loc.col);
                    check_struct_arg_shape(*c->receiver, want);
                    reject_large_native_aggregate(want, c->receiver->loc);
                    // C3 (slice-escape safety Stage 2): a `retains=true` native
                    // may store a slice ptr past the call. A stack-backed slice
                    // (ViewExpr over a fixed array, or an encrypted StringLit
                    // temp) receiver would dangle once the backing frame dies.
                    if (nit->second.retains && got && got->is_slice &&
                        is_local_array_view(*c->receiver))
                        err("cannot pass a slice derived from a stack local to native '" +
                            c->name + "' (receiver); the native retains the pointer "
                            "past the call, so it would outlive the frame. "
                            "Materialize it to a `string` handle or a rodata/global-"
                            "backed slice first.",
                            c->receiver->loc.line, c->receiver->loc.col);
                    off = 1;
                }
                for (size_t i = 0; i < c->args.size(); ++i) {
                    const Type* want = &(*params)[off + i];
                    const Type* got = check_value(c->args[i], want);
                    if (!types_compatible(want, got))
                        err("arg " + std::to_string(i+1) + " of '" + c->name + "': expected " +
                            want->to_string() + ", got " + got->to_string(),
                            c->args[i]->loc.line, c->args[i]->loc.col);
                    check_struct_arg_shape(*c->args[i], want);
                    reject_large_native_aggregate(want, c->args[i]->loc);
                    // C3 (slice-escape safety Stage 2): a `retains=true` native
                    // may store a slice ptr past the call (the annotation surface
                    // for a future retaining native). A stack-backed slice arg
                    // would dangle once the backing frame dies — reject at the
                    // call site. A copying native (retains=false, the default —
                    // string_from_slice copies the bytes into a host-owned
                    // std::string during the call) is allowed: the bytes are
                    // copied out before the frame dies. No shipped native
                    // retains, so this guard fires only for an explicitly
                    // retains-tagged native.
                    if (nit->second.retains && got && got->is_slice &&
                        is_local_array_view(*c->args[i]))
                        err("cannot pass a slice derived from a stack local to native '" +
                            c->name + "' (arg " + std::to_string(i+1) + "); the native "
                            "retains the pointer past the call, so it would outlive "
                            "the frame. Materialize it to a `string` handle or a "
                            "rodata/global-backed slice first.",
                            c->args[i]->loc.line, c->args[i]->loc.col);
                }
                // Compile-time assertion folding (efficiency ask, part 2):
                // when BOTH arguments to an assert_eq_* call are compile-time
                // constants, resolve the comparison NOW instead of leaving a
                // real runtime call+trap for something that can never
                // meaningfully fail differently at runtime. A passing
                // constant assertion costs nothing at all once elided
                // (codegen's CallExpr case skips emission entirely when
                // c->elided - see codegen.cpp); a failing one is caught here,
                // immediately, as a normal compile error instead of only
                // surfacing whenever that code path happens to execute.
                //
                // Left completely untouched when either argument is NOT a
                // constant (the overwhelmingly common case - asserting on a
                // computed/runtime value): a real native call is emitted,
                // exactly as today, still gracefully caught by the crash
                // guard if it ever traps.
                if (!c->receiver && c->args.size() == 2) {
                    if (c->name == "assert_eq_i64") {
                        int64_t a, b;
                        if (try_eval_const_i64(*c->args[0], a) && try_eval_const_i64(*c->args[1], b)) {
                            if (a == b) c->elided = true;
                            else err("assert_eq_i64(" + std::to_string(a) + ", " + std::to_string(b) +
                                     ") fails at compile time (constant operands do not compare equal)",
                                     c->loc.line, c->loc.col);
                        }
                    } else if (c->name == "assert_eq_f32") {
                        float a, b;
                        if (try_eval_const_f32(*c->args[0], a) && try_eval_const_f32(*c->args[1], b)) {
                            if (a == b) c->elided = true;
                            else err("assert_eq_f32(" + std::to_string(a) + ", " + std::to_string(b) +
                                     ") fails at compile time (constant operands do not compare equal)",
                                     c->loc.line, c->loc.col);
                        }
                    } else if (c->name == "assert_eq_bool") {
                        bool a, b;
                        if (try_eval_const_bool(*c->args[0], a) && try_eval_const_bool(*c->args[1], b)) {
                            if (a == b) c->elided = true;
                            else err(std::string("assert_eq_bool(") + (a?"true":"false") + ", " + (b?"true":"false") +
                                     ") fails at compile time (constant operands do not compare equal)",
                                     c->loc.line, c->loc.col);
                        }
                    } else if (c->name == "assert_eq_str") {
                        auto* sa = dynamic_cast<StringLit*>(c->args[0].get());
                        auto* sb = dynamic_cast<StringLit*>(c->args[1].get());
                        if (sa && sb) {
                            if (sa->s == sb->s) c->elided = true;
                            else err("assert_eq_str(\"" + sa->s + "\", \"" + sb->s +
                                     "\") fails at compile time (constant operands do not compare equal)",
                                     c->loc.line, c->loc.col);
                        }
                    }
                }
            }
        } else {
            c->is_native = false; c->script_slot = sit->second;
            auto ssi = script_sigs.find(c->name);
            if (ssi != script_sigs.end()) {
                ret_ty = ssi->second.ret;
                size_t total = ssi->second.params.size();
                size_t required = ssi->second.required_count;
                // arity (now a range, to allow trailing defaulted args) + arg type check
                if (c->args.size() < required || c->args.size() > total) {
                    err("function '" + c->name + "' expects " +
                        (required == total ? std::to_string(total)
                                            : std::to_string(required) + "-" + std::to_string(total)) +
                        " args, got " + std::to_string(c->args.size()), c->loc.line, c->loc.col);
                } else {
                    // splice synthesized literals for missing trailing args
                    // directly onto c->args (the real, owned vector on the
                    // CallExpr node) BEFORE the check loop below, so codegen
                    // - which walks c->args directly, after all of sema has
                    // run - sees the filled-in arguments too, indistinguishable
                    // from explicit ones.
                    for (size_t i = c->args.size(); i < total; ++i) {
                        const DefaultValue& dv = *ssi->second.defaults[i];
                        ExprPtr synth;
                        switch (dv.kind) {
                        case DefaultValue::Kind::Int: {
                            auto lit = std::make_unique<IntLit>(); lit->loc = c->loc; lit->v = dv.i;
                            synth = std::move(lit); break;
                        }
                        case DefaultValue::Kind::Float: {
                            auto lit = std::make_unique<FloatLit>(); lit->loc = c->loc; lit->v = dv.f; lit->is_f32 = dv.f_is_f32;
                            synth = std::move(lit); break;
                        }
                        case DefaultValue::Kind::Bool: {
                            auto lit = std::make_unique<BoolLit>(); lit->loc = c->loc; lit->v = dv.b;
                            synth = std::move(lit); break;
                        }
                        case DefaultValue::Kind::String: {
                            auto lit = std::make_unique<StringLit>(); lit->loc = c->loc; lit->s = dv.s;
                            synth = std::move(lit); break;
                        }
                        default: break; // Kind::None here would be a signature-validation bug
                        }
                        c->args.push_back(std::move(synth));
                    }
                    for (size_t i = 0; i < c->args.size(); ++i) {
                        const Type* want = ssi->second.params[i];
                        const Type* got = check_value(c->args[i], want);
                        if (!types_compatible(want, got))
                            err("arg " + std::to_string(i+1) + " of '" + c->name +
                                "': expected " + want->to_string() + ", got " + got->to_string(),
                                c->args[i]->loc.line, c->args[i]->loc.col);
                        check_struct_arg_shape(*c->args[i], want);
                        // C5 retain guard (slice-escape safety Stage 2): this
                        // param is in the callee's retained_params set — the
                        // callee stores it (or a value derived from it) to a
                        // GLOBAL inside its body, so the slice ptr escapes the
                        // frame via the callee's global store (invisible to the
                        // caller's own C1/C2 guards, since the param is not
                        // localview from the callee's perspective). Reject a
                        // stack-backed slice arg at the call site. A BORROWED
                        // param (returned, not stored) is NOT rejected here —
                        // instead is_local_array_view() tags the call result
                        // localview and the caller's own escape guards catch
                        // the escape at the actual escape point (this is what
                        // keeps return_slice_defer working).
                        if (ssi->second.retained_params.count(i) && got && got->is_slice &&
                            is_local_array_view(*c->args[i]))
                            err("cannot pass a slice derived from a stack local to '" +
                                c->name + "' (arg " + std::to_string(i+1) + "); the callee "
                                "retains the pointer (stores it to a global), so it would "
                                "outlive the frame. Materialize it to a `string` handle or "
                                "a rodata/global-backed slice first.",
                                c->args[i]->loc.line, c->args[i]->loc.col);
                    }
                }
                params = nullptr;
            } else {
                ret_ty = intern(type_void()); params = nullptr;
            }
        }
        // A struct-returning call is allowed as a `let x: T = f(...);`
        // initializer, a `return f(...);` value (see check_expr's
        // allow_struct_ret_call doc comment), OR - when `expected` is a
        // registered struct of the SAME type - as a struct-by-value call
        // argument (e.g. `v3_dot(v3_up(), v3_up())`). The arg-slot case is
        // gated here by the matching `expected` type so a struct-returning
        // call used as a bare discarded statement (expected == nullptr) or as
        // the base of a field access (expected == nullptr) is still rejected;
        // a struct-returning call of a MISMATCHED struct used as an arg is
        // rejected by types_compatible above (it never reaches here with a
        // matching expected). Codegen materializes the arg case into a
        // compiler-hidden temp frame slot (CG::eval's CallExpr struct-arg
        // stash), reusing the existing hidden-pointer / arg-copy machinery.
        if (is_registered_struct(ret_ty) && !allow_struct_ret_call &&
            !(expected && is_registered_struct(expected) && ret_ty->same(*expected))) {
            err("a call returning struct '" + ret_ty->struct_name + "' is only allowed as a "
                "`let x: " + ret_ty->struct_name + " = " + c->name + "(...);` initializer, a "
                "`return " + c->name + "(...);` value, or a struct-by-value argument of "
                "type '" + ret_ty->struct_name + "'", c->loc.line, c->loc.col);
        }
        e.ty = ret_ty; return ret_ty;
    }
    if (auto* b = dynamic_cast<BinExpr*>(&e)) {
        const Type* lt = check_expr(*b->lhs);
        const Type* rt = check_expr(*b->rhs, lt); // give rhs a chance to adapt to lhs
        // re-check a literal lhs with rhs's type so `200.0f * s` (s:f32) adapts lhs to f32,
        // and `500 + u64var` adapts lhs int literal to u64 (docs/spec/TYPE_SYSTEM.md Section 6 literal adaptation).
        // StringLit is included here too so `"prefix: " + report` (literal on
        // the left) adapts the same way `report + "\n"` already does via the
        // rhs-gets-lhs's-type path above - without this, string
        // concatenation would silently only work in one operand order, which
        // is surprising since `+` reads as commutative for strings.
        if (dynamic_cast<IntLit*>(b->lhs.get()) || dynamic_cast<FloatLit*>(b->lhs.get()) || dynamic_cast<StringLit*>(b->lhs.get())) {
            lt = check_expr(*b->lhs, rt);
        }
        // comparison/logical produce bool; arithmetic require same numeric type
        bool is_cmp = (b->op==BinExpr::Op::Eq||b->op==BinExpr::Op::Neq||b->op==BinExpr::Op::Lt||
                       b->op==BinExpr::Op::Le||b->op==BinExpr::Op::Gt||b->op==BinExpr::Op::Ge);
        bool is_logical = (b->op==BinExpr::Op::LAnd||b->op==BinExpr::Op::LOr);
        if (is_logical) {
            if (!lt->is_bool() || !rt->is_bool())
                err("logical operator requires bool operands", b->loc.line, b->loc.col);
            e.ty = &type_bool(); return e.ty;
        }
        // operator-overload dispatch (docs/spec/TYPE_SYSTEM.md section 7): if both operands
        // are a registered type with an overload for this op, stamp the BinExpr
        // as a native call (codegen emits call instead of inline arithmetic).
        // Checked BEFORE the is_cmp default-handling below - a handle type
        // (e.g. vec3, string) that registers an Eq/Neq overload must dispatch
        // to it instead of falling through to a raw handle-value compare
        // (two structurally-equal-but-distinct handles would wrongly compare
        // unequal otherwise).
        if (overloads && lt && rt && lt->same(*rt)) {
            // try lhs's type name (both same per the check above)
            // the type is either a struct name or a registered "handle" type
            std::string tname = lt->struct_name;
            if (!tname.empty()) {
                const OpOverload* oo = overloads->find(tname, int(b->op));
                if (oo) {
                    b->is_overload = true;
                    b->overload_fn = oo->fn_ptr;
                    b->overload_name = oo->fn_name;
                    b->overload_ret = oo->ret;
                    b->overload_params = oo->params;
                    e.ty = intern(oo->ret);
                    return e.ty;
                }
            }
        }
        if (is_cmp) {
            // Tier 1 typed enums (§6.3): a typed-enum value may be compared
            // to a plain integer (the enum widens to its backing int for the
            // comparison) — `if (c != 2)` where c: Color, or `Color::Red == 0`.
            // Codegen normalizes both sides to 64-bit in rax/rcx, so a
            // mixed-width compare is value-correct; the condition code's
            // signedness follows the lhs (an unsigned-backed enum selects the
            // unsigned setcc). Two DIFFERENT typed enums (Color vs Hue) stay
            // rejected (neither is a plain int), matching the rule that an
            // enum's variants only compare within their own enum.
            bool cmp_ok = types_compatible(lt, rt);
            if (!cmp_ok && lt && rt) {
                if ((!lt->enum_name.empty() && is_plain_integer(rt)) ||
                    (!rt->enum_name.empty() && is_plain_integer(lt)))
                    cmp_ok = true;
            }
            if (!cmp_ok)
                err("comparison requires same-type operands (got " + lt->to_string() + " and " + rt->to_string() + ")",
                    b->loc.line, b->loc.col);
            e.ty = &type_bool(); return e.ty;
        }
        // arithmetic/bitwise: require same numeric type (literal adaptation already applied)
        // EXCEPTION (docs/spec/TYPE_SYSTEM.md Section 7): shift rhs may be any unsigned integer type
        bool is_shift = (b->op==BinExpr::Op::Shl || b->op==BinExpr::Op::Shr);
        if (is_shift) {
            if (!lt->is_int() || !rt->is_int())
                err("shift requires integer operands (got " + lt->to_string() + " and " + rt->to_string() + ")",
                    b->loc.line, b->loc.col);
            // A compile-time-negative shift count is undefined behavior (in C
            // it's UB; here codegen masks with `& 63` so `x << -1` currently
            // silently yields a surprising value instead of erroring). A
            // negative amount is almost always a typo or logic bug, so hard-
            // fail at sema when the rhs folds to a negative constant. A
            // runtime (non-constant) negative count still reaches codegen's
            // mask and is left as-is (no cheap compile-time proof of sign).
            int64_t shift_amt;
            if (try_eval_const_i64(*b->rhs, shift_amt) && shift_amt < 0) {
                err("shift amount cannot be negative (got " + std::to_string(shift_amt) +
                    "); shifting by a negative count is undefined behavior",
                    b->rhs->loc.line, b->rhs->loc.col);
            }
            e.ty = lt; return e.ty;
        }
        if (!lt->same(*rt)) {
            err("operator requires same-type operands (got " + lt->to_string() + " and " + rt->to_string() + ")",
                b->loc.line, b->loc.col);
            e.ty = lt; return e.ty;
        }
        // Bitwise binary ops (& | ^) are integer-only (docs/spec/TYPE_SYSTEM.md
        // Section 7: "Bitwise only defined on integer types"). The unary form
        // (~) already rejects floats (sema_invalid_bitnot_non_int); the binary
        // forms must too — without this, `1.0f & 2.0f` would silently compile
        // and codegen would emit an integer `and` over the float bit patterns
        // in rax, a value-wrong no-trap miscompile. Arithmetic (+ - * / %)
        // remains valid on floats (the is_float() branch below).
        bool is_bitwise = (b->op==BinExpr::Op::And || b->op==BinExpr::Op::Or || b->op==BinExpr::Op::Xor);
        if (is_bitwise && !lt->is_int()) {
            err("bitwise operator requires integer operands (got " + lt->to_string() + " and " + rt->to_string() + ")",
                b->loc.line, b->loc.col);
        }
        if (!lt->is_int() && !lt->is_float()) {
            err("operator requires numeric operands", b->loc.line, b->loc.col);
        }
        e.ty = lt; return e.ty;
    }
    if (auto* u = dynamic_cast<UnaryExpr*>(&e)) {
        const Type* ot = check_expr(*u->operand);
        if (u->op == UnaryExpr::Op::Not) {
            if (!ot->is_bool()) err("'!' requires bool", u->loc.line, u->loc.col);
            e.ty = &type_bool();
        } else if (u->op == UnaryExpr::Op::Neg) {
            if (!ot->is_int() && !ot->is_float()) err("unary '-' requires numeric", u->loc.line, u->loc.col);
            e.ty = ot;
        } else { // BitNot
            if (!ot->is_int()) err("'~' requires integer", u->loc.line, u->loc.col);
            e.ty = ot;
        }
        return e.ty;
    }
    if (auto* c = dynamic_cast<CastExpr*>(&e)) {
        const Type* from = check_expr(*c->operand);
        const Type* to = intern(*c->to);
        // Explicit v1 cast matrix.  No representation reinterpretation is
        // permitted: slices, aggregates, opaque handles, bool, and function
        // handles can only take a same-type no-op cast.
        const bool same = from && from->same(*to);
        const bool int_to_int = is_plain_integer(from) && is_plain_integer(to);
        // Tier 1 typed enums (§6.3): explicit enum->int cast (the explicit
        // spelling of the implicit enum->backing-int widening). The operand
        // is a typed-enum value (enum_name set); the target is a plain int.
        // int->enum is NOT in the explicit matrix (a raw int has no enum_name,
        // so enum_from_int is false) — reclassify via `as` is deliberately out.
        const bool enum_to_int = from && !from->enum_name.empty() && is_plain_integer(to);
        const bool float_to_float = from && to && from->is_float() && to->is_float();
        // The x64 backend currently implements signed integer/float conversion.
        // Unsigned conversion (especially u64 above INT64_MAX) needs a distinct
        // lowering, so it is deliberately outside the v1 cast matrix.
        const bool int_float = (is_plain_integer(from) && !from->is_uint() && to && to->is_float()) ||
                               (from && from->is_float() && is_plain_integer(to) && !to->is_uint());
        if (!same && !int_to_int && !enum_to_int && !float_to_float && !int_float) {
            err("invalid cast from '" + (from ? from->to_string() : std::string("?")) +
                "' to '" + to->to_string() + "'", c->loc.line, c->loc.col);
        } else if (same && is_by_value_aggregate(from) &&
                   (aggregate_cast_init != &e || !dynamic_cast<const Ident*>(c->operand.get()))) {
            err("same-type aggregate casts are only supported as direct local initializers from a local variable",
                c->loc.line, c->loc.col);
        }
        e.ty = to; return e.ty;
    }
    if (auto* t = dynamic_cast<TernaryExpr*>(&e)) {
        const Type* ct = check_expr(*t->cond);
        if (!ct->is_bool()) err("ternary condition must be bool", t->loc.line, t->loc.col);
        const Type* tt = check_expr(*t->then_e);
        const Type* et = check_expr(*t->else_e, tt);
        if (!tt->same(*et))
            err("ternary branches must have same type (got " + tt->to_string() + " and " + et->to_string() + ")",
                t->loc.line, t->loc.col);
        e.ty = tt; return e.ty;
    }
    if (auto* a = dynamic_cast<AssignExpr*>(&e)) {
        const Type* lt = check_expr(*a->target);
        if (!is_lvalue(*a->target))
            err("assignment target is not an lvalue", a->loc.line, a->loc.col);
        // Constness is shallow binding constness for both locals and globals.
        if (auto* tid = dynamic_cast<Ident*>(a->target.get())) {
            if (const Var* v = lookup_local_var(tid->name)) {
                if (v->is_const)
                    err("cannot assign to const variable '" + tid->name + "'",
                        a->loc.line, a->loc.col);
            } else {
                auto gi = globals.find(tid->name);
                if (gi != globals.end() && gi->second.is_const)
                    err("cannot assign to const global '" + tid->name + "'",
                        a->loc.line, a->loc.col);
            }
        }
        const Type* rt = a->compound ? check_expr(*a->value, lt) : check_value(a->value, lt);
        if (lt && rt && !types_compatible(lt, rt))
            err("assignment type mismatch (" + lt->to_string() + " = " + rt->to_string() + ")",
                a->loc.line, a->loc.col);
        if (auto* tid = dynamic_cast<Ident*>(a->target.get())) {
            const bool local_view = rt && rt->is_slice && is_local_array_view(*a->value);
            if (Var* v = lookup_local_var(tid->name)) {
                // Until full provenance escape analysis exists, slice aliases
                // carry the conservative local-array-view bit through every
                // simple assignment. This closes `s = a[..]; return s;` as
                // well as longer local alias chains.
                if (!a->compound && v->ty && v->ty->is_slice)
                    v->local_array_view = v->local_array_view || local_view;
            } else {
                auto gi = globals.find(tid->name);
                if (gi != globals.end() && local_view)
                    err("cannot store a slice/view derived from a stack local in a global",
                        a->loc.line, a->loc.col);
            }
        } else if (rt && rt->is_slice && is_local_array_view(*a->value)) {
            // C2b: the target is a FieldExpr (gs.data = s;) or IndexExpr
            // (garr[0] = s;). Chase to the root base; if it is a GLOBAL, the
            // store escapes the frame (the slice ptr would dangle into the
            // dead frame once the backing local's frame is torn down). A
            // LOCAL-struct/local-array target is only unsafe if the local
            // itself escapes — that is the harder escape analysis and is a
            // follow-on; for v1 the conservative-and-correct cut rejects only
            // the global-rooted case, matching the existing global-store guard
            // above. Use a SINGLE loop peeling both FieldExpr and IndexExpr so
            // interleaved access (gs.arr[0].data) is chased all the way to the
            // root (two sequential loops would stop at the first IndexExpr and
            // miss an inner FieldExpr). If the root is not an Ident (a call
            // result, a literal, ...) we cannot prove it is a global, so we
            // conservatively ALLOW — you may only reject when you can PROVE
            // the target is a global.
            const Expr* root = a->target.get();
            while (true) {
                if (auto* fe = dynamic_cast<const FieldExpr*>(root)) { root = fe->base.get(); continue; }
                if (auto* ix = dynamic_cast<const IndexExpr*>(root)) { root = ix->base.get(); continue; }
                break;
            }
            if (auto* rid = dynamic_cast<const Ident*>(root)) {
                if (!lookup_local_var(rid->name)) {
                    auto gi = globals.find(rid->name);
                    if (gi != globals.end())
                        err("cannot store a slice/view derived from a stack local into a "
                            "field/element of a global '" + rid->name + "'",
                            a->loc.line, a->loc.col);
                }
            }
        }
        if (a->compound && lt && lt->is_float()) {
            switch (*a->compound) {
            case BinExpr::Op::Mod: case BinExpr::Op::Shl: case BinExpr::Op::Shr:
            case BinExpr::Op::And: case BinExpr::Op::Or:  case BinExpr::Op::Xor:
                err("compound-assign operator requires an integer target (got " + lt->to_string() + ")",
                    a->loc.line, a->loc.col);
                break;
            default: break;
            }
        }
        e.ty = lt; return e.ty;
    }
    if (auto* s = dynamic_cast<SizeofExpr*>(&e)) {
        int64_t size = frame_byte_width(*s->ty, structs);
        if (!s->ty->struct_name.empty() && (!structs || !structs->count(s->ty->struct_name)))
            err("unknown type '" + s->ty->struct_name + "' in sizeof", s->loc.line, s->loc.col);
        else if (size < 0 || size == INT64_MAX)
            err("invalid type in sizeof", s->loc.line, s->loc.col);
        else s->resolved = uint64_t(size);
        e.ty = intern(make_prim(Prim::U64)); return e.ty;
    }
    if (auto* o = dynamic_cast<OffsetofExpr*>(&e)) {
        if (!o->ty->is_struct() || !structs || !structs->count(o->ty->struct_name)) {
            err("unknown struct type '" + o->ty->to_string() + "' in offsetof", o->loc.line, o->loc.col);
        } else {
            auto& layout = structs->at(o->ty->struct_name);
            auto fit = layout.fields.find(o->field);
            if (fit == layout.fields.end())
                err("struct '" + o->ty->struct_name + "' has no field '" + o->field + "'", o->loc.line, o->loc.col);
            else o->resolved = uint64_t(fit->second.offset);
        }
        e.ty = intern(make_prim(Prim::U64)); return e.ty;
    }
    if (auto* ix = dynamic_cast<IndexExpr*>(&e)) {
        // base[index]: base must be a fixed array T[N] or a slice T[]
        // (codegen Section index-expr - CG::eval's IndexExpr case).
        const Type* bt = check_expr(*ix->base);
        const Type* it = check_expr(*ix->index);
        if (!it->is_int())
            err("array index must be an integer (got " + it->to_string() + ")", ix->index->loc.line, ix->index->loc.col);
        if (bt && (bt->is_slice || bt->array_len > 0) && bt->elem) {
            e.ty = intern(*bt->elem);
            // Bounds checking (efficiency: fold away the check at compile
            // time whenever provably safe, otherwise leave a real runtime
            // check for codegen to emit - see codegen.cpp's IndexExpr/
            // AssignExpr-IndexExpr-target cases).
            //
            // A constant index is only fully verifiable here against a FIXED
            // array T[N] - `array_len` is known right now, at compile time.
            // A slice T[] carries {ptr,len} where len is a runtime value
            // (loaded into rdx at eval() time, per the slice-ABI comments
            // throughout codegen.cpp) even when the index itself is a
            // constant - there is no compile-time length to check a constant
            // index against, so a slice ALWAYS needs codegen's runtime check
            // regardless of index_is_const.
            int64_t idx_val;
            if (try_eval_const_i64(*ix->index, idx_val)) {
                ix->index_is_const = true;
                ix->index_const_value = idx_val;
                if (!bt->is_slice && bt->array_len > 0) {
                    // fixed array + constant index: fully verifiable now.
                    // In-range -> proven safe, codegen emits zero-cost access
                    // (no runtime check at all). Out-of-range -> a normal
                    // sema compile error, exactly like every other sema
                    // error - this code never reaches codegen at all.
                    if (idx_val < 0 || idx_val >= int64_t(bt->array_len)) {
                        err("array index " + std::to_string(idx_val) + " out of bounds for array of size " +
                            std::to_string(bt->array_len), ix->index->loc.line, ix->index->loc.col);
                    }
                }
            }
        } else {
            err("indexing requires an array or slice type (got " + (bt ? bt->to_string() : "?") + ")",
                ix->loc.line, ix->loc.col);
            e.ty = intern(type_void());
        }
        return e.ty;
    }
    if (auto* v = dynamic_cast<ViewExpr*>(&e)) {
        // arr[..]: fixed array T[N] -> slice T[] (codegen's ViewExpr case
        // takes the frame address of `arr` directly, so v1 requires the base
        // to resolve to a named local - checked structurally here, enforced
        // by codegen which only handles a bare Ident base).
        const Type* bt = check_expr(*v->base);
        if (bt && bt->array_len > 0 && bt->elem) {
            e.ty = intern(make_slice(bt->elem));
        } else {
            err("'[..]' view requires a fixed-size array type (got " + (bt ? bt->to_string() : "?") + ")",
                v->loc.line, v->loc.col);
            e.ty = intern(type_void());
        }
        return e.ty;
    }
    if (auto* fl = dynamic_cast<FieldExpr*>(&e)) {
        // struct field read (base.field). Note: obj.method(args) never
        // reaches here - the parser desugars that to a CallExpr with
        // `receiver` set (docs/spec/BINDING_API.md sec 3) before sema ever sees a bare
        // FieldExpr for a method call; only a genuine field access lands here.
        const Type* bt = check_expr(*fl->base);
        const StructLayout* layout = (structs && bt && !bt->struct_name.empty())
            ? (structs->count(bt->struct_name) ? &structs->at(bt->struct_name) : nullptr) : nullptr;
        if (!layout) {
            err("field access requires a struct type (got " + (bt ? bt->to_string() : "?") + ")",
                fl->loc.line, fl->loc.col);
            e.ty = intern(type_void());
            return e.ty;
        }
        auto fit = layout->fields.find(fl->field);
        if (fit == layout->fields.end()) {
            err("struct '" + bt->struct_name + "' has no field '" + fl->field + "'", fl->loc.line, fl->loc.col);
            e.ty = intern(type_void());
            return e.ty;
        }
        e.ty = fit->second.ty; // stable: owned by Program::structs' FieldDecl, alive as long as prog
        return e.ty;
    }
    if (auto* sl = dynamic_cast<StructLit*>(&e)) {
        const StructLayout* layout = (structs && structs->count(sl->type_name)) ? &structs->at(sl->type_name) : nullptr;
        if (!layout) {
            err("unknown struct type '" + sl->type_name + "'", sl->loc.line, sl->loc.col);
            e.ty = intern(type_void());
            return e.ty;
        }
        for (auto& kv : sl->fields) {
            auto fit = layout->fields.find(kv.first);
            if (fit == layout->fields.end()) {
                err("struct '" + sl->type_name + "' has no field '" + kv.first + "'", kv.second->loc.line, kv.second->loc.col);
                continue;
            }
            const Type* want = fit->second.ty;
            const Type* got = check_value(kv.second, want);
            if (!types_compatible(want, got))
                err("field '" + kv.first + "' type mismatch (expected " + want->to_string() + ", got " + got->to_string() + ")",
                    kv.second->loc.line, kv.second->loc.col);
            if (is_registered_struct(want) &&
                !dynamic_cast<const Ident*>(kv.second.get()) &&
                !dynamic_cast<const StructLit*>(kv.second.get()))
                err("nested struct field '" + kv.first + "' must be initialized from a local or struct literal",
                    kv.second->loc.line, kv.second->loc.col);
        }
        // v1: every field must be explicitly initialized (keeps codegen
        // simple - one store per literal field, no separate zero-fill pass).
        for (auto& fname : layout->field_names) {
            bool found = false;
            for (auto& kv : sl->fields) if (kv.first == fname) { found = true; break; }
            if (!found) err("struct literal for '" + sl->type_name + "' is missing field '" + fname + "'",
                            sl->loc.line, sl->loc.col);
        }
        // The struct type: prim=Void for registered (by-value) structs
        // (in the StructLayoutTable), prim=I64 for opaque handles (not in
        // the table). This matches bind_struct (prim=Void) for host-registered
        // structs and bind_handle (prim=I64) for opaque handles.
        Type st;
        st.prim = (structs && structs->count(sl->type_name)) ? Prim::Void : Prim::I64;
        st.struct_name = sl->type_name;
        e.ty = intern(st);
        return e.ty;
    }
    if (auto* al = dynamic_cast<ArrayLit*>(&e)) {
        // Array literal `[ expr, expr, ... ]` (chunk c2). v1 does NOT infer array
        // types from elements - an ArrayLit requires a declared target type
        // (`expected`) at the let-init / arg site, which is the ONLY place this
        // node is legal. Sema's check_value/check_expr callers always thread a
        // declared `expected` for a typed let / a call argument / a return;
        // a bare `let x = [1,2,3];` (no annotation) reaches here with
        // expected==nullptr and is rejected. The declared type must be a fixed
        // array T[N] or a slice T[]; anything else (a scalar, a struct, void)
        // is a shape mismatch. Each element is checked against the declared
        // element type via check_value so the existing int-literal coercion
        // (adapt_int_lit) applies - `let arr: i32[3] = [1, 2, 3]` adopts i32.
        if (!expected || !(expected->is_slice || expected->array_len > 0) || !expected->elem) {
            if (!expected) {
                err("array literal needs an explicit type annotation (v1 does not infer array types from elements)",
                    al->loc.line, al->loc.col);
            } else {
                err("array literal requires a fixed-array or slice target type (got " +
                    expected->to_string() + ")", al->loc.line, al->loc.col);
            }
            e.ty = intern(type_void());
            return e.ty;
        }
        if (al->elements.empty()) {
            // v1 rejects an empty array literal outright (no type-inference
            // subsystem to recover the element type from a surrounding
            // context, and a zero-length fixed array T[0] is not a valid v1
            // shape). The declared type is present here, but the literal still
            // carries no elements to validate against, so reject for v1.
            err("empty array literal needs a declared type and at least one element (v1 does not infer array types)",
                al->loc.line, al->loc.col);
            e.ty = intern(type_void());
            return e.ty;
        }
        // v1 scope restriction (chunk c2): a FIXED-array ArrayLit may only
        // appear as the direct initializer of a `let`/`const`/`auto` binding -
        // the LetStmt init path sets aggregate_cast_init to point at the
        // init expr while it is being checked, so == &e identifies exactly
        // that position. A fixed array is a multi-word aggregate that v1's
        // call/return convention does NOT carry by value (words_for_type
        // returns 1 for a fixed array, truncating it; no hidden-pointer path
        // like a registered struct has), so a fixed-array ArrayLit used as a
        // call argument or a return value would silently miscompile. A
        // SLICE ArrayLit is fine in any position with a declared slice type
        // (codegen materializes its backing into a temp and yields
        // {rax=ptr, rdx=len}, which the existing 2-word slice arg / slice
        // return paths already handle). An ArrayLit as the base of an index /
        // a binary operand reaches here with expected==nullptr and is rejected
        // by the annotation check above.
        if (!expected->is_slice && aggregate_cast_init != &e) {
            err("a fixed-array array literal may only appear as a `let`/`const` "
                "initializer in v1 (fixed-array call args / returns are not "
                "supported); use a slice `" + expected->elem->to_string() +
                "[]` for arg-passing, or assign the literal to a local first",
                al->loc.line, al->loc.col);
        }
        const Type* elem_ty = expected->elem.get();
        if (!expected->is_slice) {
            // fixed array T[N]: requires EXACTLY N elements.
            if (al->elements.size() != expected->array_len) {
                err("array literal for " + expected->to_string() + " needs " +
                    std::to_string(expected->array_len) + " elements, got " +
                    std::to_string(al->elements.size()),
                    al->loc.line, al->loc.col);
            }
        }
        for (auto& el : al->elements) {
            const Type* got = check_value(el, elem_ty);
            if (!types_compatible(elem_ty, got))
                err("array element type mismatch (expected " + elem_ty->to_string() +
                    ", got " + got->to_string() + ")", el->loc.line, el->loc.col);
        }
        // Bake the declared array/slice Type onto ->ty so codegen knows the
        // shape (fixed-array extent vs. slice {ptr,len}) and the element type.
        e.ty = expected;
        return e.ty;
    }
    err("codegen: expression node not yet supported by sema", e.loc.line, e.loc.col);
    e.ty = intern(type_void()); return e.ty;
}

// Tier 1 static_assert(cond, "msg") — compile-time assertion (shared by the
// in-body check_stmt path and the top-level prog.static_asserts pass).
//
// By the time this runs, the constexpr-call pre-pass has already rewritten any
// constexpr fn calls in `cond` to IntLits and the enum-access pre-pass has
// already rewritten any EnumAccessExpr to IntLits, so `cond` is a tree of
// literals / BinExpr / UnaryExpr that try_eval_const_bool can fold (the
// comparison-operator folding added to try_eval_const_bool handles
// `1 + 1 == 2` and `square(7) == 49` shapes). The verdict:
//   - folds to true  -> elided (codegen emits nothing; a passing compile-time
//                       check costs zero, mirroring assert_eq_*'s elided path)
//   - folds to false -> sema compile error carrying `msg`
//   - doesn't fold   -> sema compile error ("condition must be a compile-time
//                       constant") — a runtime value is not a static assertion
// The cond is still type-checked first (it must be bool) so a type error
// surfaces with its own diagnostic instead of being masked as "non-const".
void Checker::check_static_assert(StaticAssertStmt& sa) {
    const Type* ct = check_expr(*sa.cond);
    if (!ct->is_bool()) {
        err("static_assert condition must be bool (got " + ct->to_string() + ")",
            sa.cond->loc.line, sa.cond->loc.col);
        return;
    }
    bool result = false;
    if (try_eval_const_bool(*sa.cond, result)) {
        if (!result) {
            err("static_assert failed: " + sa.msg, sa.loc.line, sa.loc.col);
        }
        // true -> elided: nothing to record, codegen skips StaticAssertStmt
        // entirely (it produces no code in either statement walker).
    } else {
        err("static_assert condition must be a compile-time constant",
            sa.cond->loc.line, sa.cond->loc.col);
    }
}

void Checker::check_stmt(Stmt& s, const Type* ret_ty, bool& returns) {
    if (++check_depth > MAX_CHECK_DEPTH) {
        --check_depth;
        throw SemaError{"recursion depth exceeded (statement too deeply nested)", s.loc.line, s.loc.col};
    }
    struct DepthRestore { int& d; ~DepthRestore() { --d; } } dr{check_depth};
    if (auto* ls = dynamic_cast<LetStmt*>(&s)) {
        // `auto` is deprecated: it's a redundant spelling of `let x = expr;`
        // inference (both share the is_auto path; `let x = expr;` is the
        // canonical form). Keep working — emit a non-fatal warning, don't error.
        if (ls->used_auto_kw) {
            warn("'auto' is deprecated; use 'let x = expr;' for inference or 'let x: T = expr;' for an explicit type",
                 ls->loc.line, ls->loc.col);
        }
        // V6-DoS mitigation: reject any local whose frame width exceeds the
        // per-frame byte budget BEFORE codegen emits a `sub rsp, <huge>`.
        // The confirmed exploit was `let a: u8[65536];` -> SIGSEGV (no cap).
        // Also catches V6-overflow (array_len so large byte_size wraps).
        if (ls->ty) {
            if (contains_void(ls->ty.get()))
                err("local '" + ls->name + "' cannot have void type", ls->loc.line, ls->loc.col);
            int64_t w = frame_byte_width(*ls->ty, structs);
            if (w > MAX_FRAME_BYTES) {
                err("local '" + ls->name + "' frame size (" + std::to_string(w) +
                    " bytes) exceeds the per-frame budget (" + std::to_string(MAX_FRAME_BYTES) +
                    " bytes); reduce the array/struct size",
                    ls->loc.line, ls->loc.col);
            }
            // enum name used as a let type is a v1 error (docs/planning/plan_ENUMS.md Section 4.5):
            // enums are untyped (their values ARE i32). The hook the typed-enum
            // upgrade (Tier 2) flips from reject to accept.
            check_type_not_enum(ls->ty.get(), ls->loc);
        }
        if (!ls->init) {
            // no initializer (parser only allows this for explicitly-typed
            // let/const, never auto): declare at the stated type, default
            // zero-filled by codegen.
            declare(ls->name, intern(*ls->ty), ls->is_const, false, ls->loc);
            return;
        }
        // Pass the declared type as `expected` (when there is one) so the
        // initializer can adapt to it - the same "expected flows into
        // check_expr" idiom BinExpr/CallExpr args already use for literal
        // adaptation (numeric literals) and, with this change, string
        // literals too (see StringLit's check_expr case below). Previously
        // this always passed nullptr, which is why `let x: string = "lit";`
        // couldn't work even in principle - the literal never saw what type
        // was wanted.
        // #20: `let g: fn(Args)->Ret = <lambda>` — upgrade the declared fn-
        // handle type to is_lambda (16 bytes) so the 16-byte lambda value
        // {slot, env_ptr} fits + g(...) dispatches as a lambda call. The
        // upgrade is on ls->ty (the shared_ptr), permanent for this binding.
        if (!ls->is_auto && ls->ty && ls->ty->is_fn_handle && ls->ty->has_recorded_sig &&
            ls->init && dynamic_cast<LambdaExpr*>(ls->init.get())) {
            auto* le = dynamic_cast<LambdaExpr*>(ls->init.get());
            if (le->ret && ls->ty->recorded_ret && ls->ty->recorded_ret->same(*le->ret) &&
                ls->ty->recorded_params.size() == le->params.size()) {
                bool sig_match = true;
                for (size_t j = 0; j < le->params.size(); ++j)
                    if (!ls->ty->recorded_params[j]->same(*le->params[j].ty)) { sig_match = false; break; }
                if (sig_match) {
                    ls->ty->is_fn_handle = false;
                    ls->ty->is_lambda = true;
                }
            }
        }
        const Type* expected_ty = ls->is_auto ? nullptr : intern(*ls->ty);
        const Expr* saved_aggregate_cast_init = aggregate_cast_init;
        aggregate_cast_init = ls->init.get();
        const Type* init_ty = expected_ty ? check_value(ls->init, expected_ty, true)
                                          : check_expr(*ls->init, nullptr, true);
        aggregate_cast_init = saved_aggregate_cast_init;
        const Type* decl_ty;
        if (ls->is_auto) decl_ty = init_ty;
        else {
            decl_ty = expected_ty;
            if (!types_compatible(decl_ty, init_ty))
                err("let type mismatch (" + decl_ty->to_string() + " = " + init_ty->to_string() + ")",
                    ls->loc.line, ls->loc.col);
        }
        if (contains_void(decl_ty))
            err("local '" + ls->name + "' cannot have void type", ls->loc.line, ls->loc.col);
        // iterable() hook (Tier 1, array case): if this local is initialized
        // from array_new(esz, ...) — or aliased from a var that was — tag it
        // with the inferred element type so a later `for (x in this_var)` can
        // type its loop variable and pick the array_get_* variant. A bare
        // `let b = a;` where `a` is an array handle propagates the tag.
        const Type* arr_elem = infer_array_elem_ty_from_init(ls->init.get());
        if (!arr_elem) {
            if (auto* id = dynamic_cast<const Ident*>(ls->init.get())) {
                if (const Var* v = lookup_local_var(id->name)) arr_elem = v->array_elem_ty;
            }
        }
        declare(ls->name, decl_ty, ls->is_const, is_local_array_view(*ls->init), ls->loc, arr_elem);
        return;
    }
    if (auto* sa = dynamic_cast<StaticAssertStmt*>(&s)) {
        check_static_assert(*sa);
        return;
    }
    if (auto* es = dynamic_cast<ExprStmt*>(&s)) { check_expr(*es->expr); return; }
    if (auto* rs = dynamic_cast<ReturnStmt*>(&s)) {
        returns = true;
        if (rs->value) {
            const Type* vt = check_value(rs->value, ret_ty, true);
            if (ret_ty && !types_compatible(ret_ty, vt))
                err("return type mismatch (got " + vt->to_string() + ", fn returns " + ret_ty->to_string() + ")",
                    rs->loc.line, rs->loc.col);
            if (vt && vt->is_slice && is_local_array_view(*rs->value))
                err("cannot return a slice/view derived from a stack local",
                    rs->loc.line, rs->loc.col);
            // A struct-returning function's `return` value may be (a) a bare
            // local (codegen copies its bytes through the hidden return
            // pointer), (b) a call to a function with the SAME struct return
            // type (forwarding this function's own incoming hidden pointer -
            // supports multi-hop chains, e.g. return relay(...);), or (c) a
            // struct literal of the return type (codegen materializes it into
            // a compiler-hidden temp frame slot, then copies that temp's
            // bytes through the hidden return pointer). A struct literal of a
            // MISMATCHED struct is rejected by types_compatible above and never
            // reaches here; any other aggregate expression (e.g. a field
            // access) has no destination address codegen can use and stays
            // rejected.
            if (ret_ty && is_registered_struct(ret_ty)) {
                bool is_ident = dynamic_cast<const Ident*>(rs->value.get()) != nullptr;
                auto* call = dynamic_cast<const CallExpr*>(rs->value.get());
                bool is_forwarding_call = call && vt->same(*ret_ty);
                auto* sl = dynamic_cast<const StructLit*>(rs->value.get());
                bool is_struct_lit = sl && sl->type_name == ret_ty->struct_name;
                if (!is_ident && !is_forwarding_call && !is_struct_lit) {
                    err("a `return` of struct '" + ret_ty->struct_name + "' must be a plain "
                        "local variable, a call to a function with the same struct return "
                        "type, or a struct literal of '" + ret_ty->struct_name + "'",
                        rs->loc.line, rs->loc.col);
                }
            }
        } else if (ret_ty && !ret_ty->is_void()) {
            err("non-void function must return a value", rs->loc.line, rs->loc.col);
        }
        return;
    }
    if (auto* is = dynamic_cast<IfStmt*>(&s)) {
        const Type* ct = check_expr(*is->cond);
        if (!ct->is_bool()) err("if condition must be bool", is->loc.line, is->loc.col);
        push_scope();
        bool t_ret=false, e_ret=false;
        check_block(is->then_b, ret_ty, t_ret);
        pop_scope();
        if (is->has_else) {
            push_scope(); check_block(is->else_b, ret_ty, e_ret); pop_scope();
            returns = t_ret && e_ret;
        } else {
            returns = false;
        }
        return;
    }
    if (auto* ws = dynamic_cast<WhileStmt*>(&s)) {
        const Type* ct = check_expr(*ws->cond);
        if (!ct->is_bool()) err("while condition must be bool", ws->loc.line, ws->loc.col);
        ++loop_depth; push_scope(); bool r=false; check_block(ws->body, ret_ty, r); pop_scope(); --loop_depth;
        return; // while doesn't guarantee return (condition may be false first)
    }
    if (dynamic_cast<BreakStmt*>(&s)) {
        if (loop_depth == 0 && switch_depth == 0)
            err("break is only valid inside a loop or switch", s.loc.line, s.loc.col);
        return;
    }
    if (dynamic_cast<ContinueStmt*>(&s)) {
        if (loop_depth == 0)
            err("continue is only valid inside a loop", s.loc.line, s.loc.col);
        return;
    }
    if (auto* bs = dynamic_cast<BlockStmt*>(&s)) {
        push_scope(); check_block(bs->block, ret_ty, returns); pop_scope(); return;
    }
    if (auto* ds = dynamic_cast<DeferStmt*>(&s)) {
        // Cleanup runs while this lexical Block's locals remain live, so the
        // ordinary left-to-right declaration-before-use and scope lookup are
        // sufficient. The expression result is discarded.
        check_expr(*ds->expr);
        return;
    }
    if (auto* fs = dynamic_cast<ForStmt*>(&s)) {
        ++loop_depth;
        push_scope();
        if (fs->init) check_stmt(*fs->init, ret_ty, returns);
        if (fs->cond) { const Type* ct = check_expr(*fs->cond); if(!ct->is_bool()) err("for cond must be bool", fs->loc.line, fs->loc.col); }
        if (fs->step) check_expr(*fs->step);
        bool r=false; check_block(fs->body, ret_ty, r);
        pop_scope();
        --loop_depth;
        return;
    }
    if (auto* ds = dynamic_cast<DoWhileStmt*>(&s)) {
        ++loop_depth;
        push_scope(); bool r=false; check_block(ds->body, ret_ty, r); pop_scope();
        --loop_depth;
        if (ds->cond) { const Type* ct = check_expr(*ds->cond); if(!ct->is_bool()) err("do-while cond must be bool", ds->loc.line, ds->loc.col); }
        return;
    }
    if (auto* fe = dynamic_cast<ForEachStmt*>(&s)) {
        // for (x in iter): the iterable() hook (Tier 1). Two iterable kinds:
        //   1. a slice T[]              -> x gets the slice's element type
        //   2. an array<T> handle (i64) -> x gets the inferred element type
        //      (u8/f32/i64 from the array_new elem_size), and codegen lowers
        //      this to array_length(h) + array_get_*(h, i). The handle must be
        //      PROVABLY from the array extension (an inline array_new call or a
        //      var tagged at its `let`); a bare i64 that isn't a known array
        //      handle is rejected so a typo like `for (x in 42)` stays an error.
        const Type* iter_ty = check_expr(*fe->iter);
        const Type* elem_ty = nullptr;
        if (iter_ty && iter_ty->is_slice) {
            elem_ty = iter_ty->elem.get();
            if (!elem_ty) elem_ty = &type_i64();  // fallback
        } else if (iter_ty && iter_ty->prim == Prim::I64 && !iter_ty->is_fn_handle && iter_ty->struct_name.empty()) {
            // array<T> handle path: infer the element type, else reject.
            elem_ty = infer_iterable_array_elem_ty(*fe->iter);
            if (!elem_ty) {
                err("for-each iterable must be a slice or array handle (got "
                    + iter_ty->to_string() + "); if this is an array handle, declare it from array_new so its element type is known",
                    fe->iter->loc.line, fe->iter->loc.col);
                elem_ty = &type_i64();  // continue checking the body with a fallback
            } else {
                fe->array_elem_ty = elem_ty;  // codegen reads this for the array branch
            }
        } else {
            err("for-each iterable must be a slice or array handle (got "
                + (iter_ty ? iter_ty->to_string() : std::string("?")) + ")",
                fe->iter->loc.line, fe->iter->loc.col);
            elem_ty = &type_i64();  // fallback so the body still type-checks
        }
        ++loop_depth;
        push_scope();
        declare(fe->var, elem_ty, false, false, fe->loc);
        bool r = false;
        check_block(fe->body, ret_ty, r);
        pop_scope();
        --loop_depth;
        return;
    }
    if (auto* sw = dynamic_cast<SwitchStmt*>(&s)) {
        const Type* subj_ty = check_expr(*sw->subject);
        if (!subj_ty->is_int() && !subj_ty->is_bool())
            err("switch subject must be an integer or bool (got " + subj_ty->to_string() + ")",
                sw->loc.line, sw->loc.col);
        bool seen_default = false;
        ++switch_depth;
        for (auto& c : sw->cases) {
            if (c.is_default) {
                if (seen_default) err("switch has more than one 'default' case", sw->loc.line, sw->loc.col);
                seen_default = true;
            } else {
                // case values must be compile-time constants (codegen compares
                // against them directly, no general constant-folding pass).
                if (!dynamic_cast<IntLit*>(c.value.get()) && !dynamic_cast<BoolLit*>(c.value.get()))
                    err("case value must be a literal constant", c.value->loc.line, c.value->loc.col);
                const Type* vt = check_expr(*c.value, subj_ty);
                if (!vt->same(*subj_ty) && !(vt->is_int() && subj_ty->is_int()))
                    err("case value type mismatch (switch on " + subj_ty->to_string() + ", case is " + vt->to_string() + ")",
                        c.value->loc.line, c.value->loc.col);
            }
            push_scope();
            bool r = false;
            check_block(c.body, ret_ty, r);
            pop_scope();
            if (!c.body.stmts.empty()) {
                Stmt* last = c.body.stmts.back().get();
                if (!dynamic_cast<BreakStmt*>(last) && !dynamic_cast<ReturnStmt*>(last) &&
                    !dynamic_cast<ContinueStmt*>(last))
                    err("nonempty switch case must end with break, continue, or return; implicit fallthrough is not allowed",
                        last->loc.line, last->loc.col);
            }
        }
        --switch_depth;
        // Conservative: switch does not prove a return unless a future CFG
        // analysis establishes exhaustive all-return cases.
        returns = false;
        return;
    }
    if (auto* ms = dynamic_cast<MatchStmt*>(&s)) {
        const Type* subj_ty = check_expr(*ms->subject);
        // Tier 1 struct destructure: the subject can be a struct (for struct
        // patterns) OR an int/bool (for literal patterns). A match with struct
        // patterns requires a struct subject; a match with literal patterns
        // requires int/bool.
        bool has_struct_pat = false;
        for (auto& arm : ms->arms) if (arm.has_struct_pat) { has_struct_pat = true; break; }
        if (has_struct_pat) {
            // struct destructure match: subject must be a struct
            if (subj_ty->struct_name.empty())
                err("match with struct patterns requires a struct subject (got " + subj_ty->to_string() + ")",
                    ms->loc.line, ms->loc.col);
        } else {
            if (!subj_ty->is_int() && !subj_ty->is_bool())
                err("match subject must be an integer or bool (got " + subj_ty->to_string() + ")",
                    ms->loc.line, ms->loc.col);
        }
        bool seen_wildcard = false;
        for (auto& arm : ms->arms) {
            if (arm.is_wildcard) {
                if (seen_wildcard) err("match has more than one '_' wildcard arm", ms->loc.line, ms->loc.col);
                seen_wildcard = true;
            } else if (arm.has_struct_pat) {
                // Tier 1 struct destructure: type-check the pattern against the subject struct.
                // Look up the struct layout.
                if (structs && structs->count(arm.struct_pat.struct_name)) {
                    const auto& layout = structs->at(arm.struct_pat.struct_name);
                    // Verify the pattern's struct name matches the subject's struct type.
                    if (subj_ty->struct_name != arm.struct_pat.struct_name) {
                        err("match struct pattern '" + arm.struct_pat.struct_name +
                            "' does not match subject type '" + subj_ty->to_string() + "'",
                            ms->loc.line, ms->loc.col);
                    }
                    for (auto& spf : arm.struct_pat.fields) {
                        auto fit = layout.fields.find(spf.name);
                        if (fit == layout.fields.end()) {
                            err("struct '" + arm.struct_pat.struct_name + "' has no field '" + spf.name + "'",
                                ms->loc.line, ms->loc.col);
                        } else if (spf.literal) {
                            // field: literal — type-check the literal against the field type
                            const Type* pt = check_expr(*spf.literal, fit->second.ty);
                            if (!pt->same(*fit->second.ty) && !(pt->is_int() && fit->second.ty->is_int()))
                                err("struct pattern field '" + spf.name + "' type mismatch (field is " +
                                    fit->second.ty->to_string() + ", pattern is " + pt->to_string() + ")",
                                    spf.literal->loc.line, spf.literal->loc.col);
                        }
                        // else: capture-only — the field is bound as a local in the arm body scope.
                    }
                } else {
                    err("match struct pattern references unknown struct '" + arm.struct_pat.struct_name + "'",
                        ms->loc.line, ms->loc.col);
                }
            } else {
                if (!dynamic_cast<IntLit*>(arm.pattern.get()) && !dynamic_cast<BoolLit*>(arm.pattern.get()))
                    err("match pattern must be a literal constant",
                        arm.pattern->loc.line, arm.pattern->loc.col);
                const Type* pt = check_expr(*arm.pattern, subj_ty);
                if (!pt->same(*subj_ty) && !(pt->is_int() && subj_ty->is_int()))
                    err("match pattern type mismatch (match on " + subj_ty->to_string() + ", pattern is " + pt->to_string() + ")",
                        arm.pattern->loc.line, arm.pattern->loc.col);
            }
            push_scope();
            // Tier 1 struct destructure: bind captured fields as locals.
            if (arm.has_struct_pat && structs && structs->count(arm.struct_pat.struct_name)) {
                const auto& layout = structs->at(arm.struct_pat.struct_name);
                for (auto& spf : arm.struct_pat.fields) {
                    if (!spf.literal) {  // capture-only
                        auto fit = layout.fields.find(spf.name);
                        if (fit != layout.fields.end()) {
                            declare(spf.name, fit->second.ty, false, false, ms->loc);
                        }
                    }
                }
            }
            // Tier 1 match guards: type-check the guard as bool.
            if (arm.guard) {
                const Type* gt = check_expr(*arm.guard);
                if (!gt->is_bool())
                    err("match guard must be a bool expression (got " + gt->to_string() + ")",
                        arm.guard->loc.line, arm.guard->loc.col);
            }
            bool r = false;
            check_block(arm.body, ret_ty, r);
            pop_scope();
        }
        returns = false;
        return;
    }
    // Tier 4: try { ... } catch (name) { ... } — in-language exceptions.
    // The try body + catch body are each checked in their own scope. The
    // catch_name is bound as an i64 local in the catch block's scope (the
    // thrown value is always i64 in v1). A throw is a terminator (sets
    // `returns`), so `returns` for the whole try/catch is true only if BOTH
    // the try body and the catch body always terminate — conservative but
    // correct (if the try can complete normally, the try/catch can fall
    // through; if the try throws and the catch completes normally, the
    // try/catch also falls through). A throw outside any try/catch is NOT a
    // sema error — it's a valid runtime behavior (unwinds to the host as a
    // TrapReason::UnhandledThrow, mirroring runtime_error), so no enclosing-
    // try check is performed here.
    if (auto* tc = dynamic_cast<TryCatchStmt*>(&s)) {
        push_scope();
        bool try_ret = false;
        check_block(tc->try_body, ret_ty, try_ret);
        pop_scope();
        push_scope();
        declare(tc->catch_name, &type_i64(), false, false, tc->loc);
        bool catch_ret = false;
        check_block(tc->catch_body, ret_ty, catch_ret);
        pop_scope();
        returns = try_ret && catch_ret;
        return;
    }
    // Tier 4: throw expr; — raises an exception (i64 value for v1) that
    // unwinds to the nearest enclosing catch, or to the host if none. The
    // throw expr must be i64. throw is a terminator (like return), so it
    // sets `returns=true` — check_block's unreachable-code guard flags any
    // statement after a throw in the same block.
    if (auto* th = dynamic_cast<ThrowStmt*>(&s)) {
        const Type* vt = check_expr(*th->value);
        if (!vt->is_int() || !is_plain_integer(vt))
            err("throw value must be an i64 integer (got " + vt->to_string() + ")",
                th->loc.line, th->loc.col);
        returns = true;
        return;
    }
    // #21 coroutine yield: `yield expr;` marks the enclosing fn as a
    // coroutine. The yield value's type must match the coroutine's yield
    // type (the first yield establishes it; subsequent yields must match).
    // A void yield (`yield;`) is allowed only if the coroutine's yield type
    // is void. yield does NOT terminate the fn (unlike return) — execution
    // suspends + resumes on next().
    if (auto* ys = dynamic_cast<YieldStmt*>(&s)) {
        if (!current_func) {
            err("yield outside a function", ys->loc.line, ys->loc.col);
            return;
        }
        current_func->is_coroutine = true;
        const Type* yt = ys->value ? check_expr(*ys->value) : &type_void();
        if (!current_func->coroutine_yield_type) {
            current_func->coroutine_yield_type = std::make_shared<Type>(*yt);
        } else {
            const Type* want = current_func->coroutine_yield_type.get();
            if (!want->same(*yt))
                err("yield type mismatch (got " + yt->to_string() +
                    ", coroutine yields " + want->to_string() + ")",
                    ys->loc.line, ys->loc.col);
        }
        return;
    }
}

void Checker::check_block(Block& b, const Type* ret_ty, bool& returns) {
    if (++check_depth > MAX_CHECK_DEPTH) {
        --check_depth;
        uint32_t line = b.stmts.empty() ? 0 : b.stmts[0]->loc.line;
        throw SemaError{"recursion depth exceeded (block too deeply nested)", line, 0};
    }
    struct DepthRestore { int& d; ~DepthRestore() { --d; } } dr{check_depth};
    returns = false;
    push_scope();
    bool reported_unreachable = false;
    for (auto& s : b.stmts) {
        // Unreachable-code guard: once a DIRECT child of this block is an
        // unconditional terminator (return / break / continue, or an if/else
        // where BOTH branches terminate), every subsequent direct child is
        // dead code. Previously this was silently accepted (sema kept
        // type-checking the dead stmts and codegen kept emitting them, never
        // executed). A dead statement after a return is almost always a bug
        // (forgotten early return, leftover debug stmt), so hard-fail on the
        // FIRST unreachable stmt rather than silently accept it. Keep type-
        // checking the rest (so any other real errors in the dead stmts still
        // surface) but report the unreachability only once per block.
        if (returns) {
            if (!reported_unreachable) {
                err("unreachable code after a return/break/continue in this block",
                    s->loc.line, s->loc.col);
                reported_unreachable = true;
            }
            bool r = false;
            check_stmt(*s, ret_ty, r);
            continue;
        }
        bool r = false;
        check_stmt(*s, ret_ty, r);
        if (r) returns = true;
    }
    pop_scope();
}

void Checker::check_func(FuncDecl& f) {
    current_ns = f.ns;  // Tier 1: track the current namespace for bare-call resolution
    current_func = &f;  // #21: track for yield->coroutine marking
    scopes.clear();
    push_scope();
    // #20: a synthetic lambda fn has a hidden __env param (params[0]) + the
    // captures are env-capture vars (not frame locals). check_lambda_func sets
    // those up; the declared params (params[1..]) are bound there.
    if (f.is_lambda) {
        check_lambda_func(f);
        pop_scope();
        return;
    }
    const bool is_realtime = std::any_of(
        f.annotations.begin(), f.annotations.end(),
        [](const Annotation& a) { return a.name == "realtime"; });
    const size_t realtime_error_start = errs.size();
    if (is_realtime) {
        // Walk the source body before ordinary type checking as well, so a
        // forbidden but unavailable native (for example a host that did not
        // register gc_alloc) still gets the required realtime diagnostic.
        validate_realtime(f);
    }

    // Struct-by-value params/returns are supported (shipped; see
    // docs/spec/TYPE_SYSTEM.md §2 struct layout + docs/spec/CODEGEN_SPEC.md
    // for the Win64 word-based param convention and hidden-pointer return
    // convention) -
    // codegen has a real word-based param convention and a hidden-pointer
    // return convention. The only restrictions live at call sites, not here:
    // check_struct_arg_shape (a by-value argument must be a bare local) and
    // the CallExpr/ReturnStmt position checks for struct-returning calls.
    for (auto& p : f.params) declare(p.name, intern(*p.ty), false, false, p.loc);
    const Type* ret_ty = intern(*f.ret);
    bool returns = false;
    check_block(f.body, ret_ty, returns);
    if (is_realtime) {
        // Re-run after type checking to classify resolved natives and indirect
        // calls. Remove the pre-walk's realtime-only diagnostics first to
        // avoid duplicates while preserving ordinary type errors it emitted.
        errs.erase(std::remove_if(errs.begin() + realtime_error_start, errs.end(),
                                  [](const SemaError& e) {
                                      return e.msg.compare(0, 20, "@realtime violation:") == 0;
                                  }),
                   errs.end());
        validate_realtime(f);
    }
    if (!ret_ty->is_void() && !returns) {
        // find end of function for the error loc
        err("function '" + f.name + "' not all paths return a value", f.loc.line, f.loc.col);
    }
    pop_scope();
}

// ============================================================================
// @realtime validation
// ============================================================================

namespace {

bool realtime_name_has_prefix(const std::string& name, const char* prefix) {
    return name.compare(0, std::strlen(prefix), prefix) == 0;
}

// Return a user-facing reason for a categorically forbidden call, or nullptr
// when the name is not on a realtime blocklist. This checks the source name as
// well as sema's exact native binding name, so an unavailable/unknown native
// still receives the useful @realtime diagnostic in addition to the ordinary
// "unknown function" error.
const char* realtime_forbidden_call_reason(const std::string& name) {
    if (name == "new" || name == "delete" || name == "gc_alloc" ||
        name == "gc_collect" || name == "gc_set_threshold" ||
        realtime_name_has_prefix(name, "gc_") ||
        realtime_name_has_prefix(name, "__ember_gc_"))
        return "GC/heap allocation";

    if (name == "print" || name == "println" || name == "print_string" ||
        name == "print_f32" || name == "print_f64" ||
        realtime_name_has_prefix(name, "print_") ||
        realtime_name_has_prefix(name, "file_") ||
        realtime_name_has_prefix(name, "path_") ||
        realtime_name_has_prefix(name, "console_") || name == "read_line")
        return "I/O operation";

    if (name == "thread_create" || name == "thread_join" ||
        realtime_name_has_prefix(name, "thread_") ||
        realtime_name_has_prefix(name, "mutex_") ||
        realtime_name_has_prefix(name, "channel_"))
        return "thread or synchronization operation";

    if (name == "call_raw" || name == "make_executable" ||
        name == "free_executable" || name == "free_executable_ptr")
        return "FFI/executable-memory operation";

    return nullptr;
}

bool realtime_safe_native(const std::string& name) {
    static const std::unordered_set<std::string> safe = {
        // Audio-plane raw buffer access: FFI-gated for capability security,
        // but allocation-free and explicitly realtime-safe once granted.
        "load_f32", "store_f32", "load_f64", "store_f64",
        "load_i32", "store_i32",
        // Host-provided preallocated DSP state accessors used by the headless
        // delay reference (same allocation-free contract as audio buffers).
        "delay_buffer", "delay_size",

        // Audited string observations/conversion named by the realtime
        // profile. (Other string natives may allocate and are rejected.)
        "string_from_slice", "string_length",

        // Scalar math extension.
        "sqrt", "sin", "cos", "tan", "atan", "atan2", "exp", "log",
        "floor", "ceil", "abs", "round", "sqrt_f64", "sin_f64",
        "cos_f64", "tan_f64", "floor_f64", "ceil_f64", "abs_f64",
        "pow_f64", "abs_i64", "atan_f64", "atan2_f64", "exp_f64",
        "log_f64", "log2_f64", "log10_f64", "fmod_f64", "round_f64",
        "trunc_f64", "min_f64", "max_f64", "clamp_f64", "min_i64",
        "max_i64", "clamp_i64"
    };
    if (safe.count(name) != 0) return true;

    // The language's vector/matrix/quaternion operations are part of the
    // explicitly allowed realtime math surface.
    return realtime_name_has_prefix(name, "vec2_") ||
           realtime_name_has_prefix(name, "vec3_") ||
           realtime_name_has_prefix(name, "vec4_") ||
           realtime_name_has_prefix(name, "quat_") ||
           realtime_name_has_prefix(name, "mat4_");
}

} // namespace

void Checker::validate_realtime_expr(const Expr& e) {
    if (auto* call = dynamic_cast<const CallExpr*>(&e)) {
        const std::string& binding = call->native_binding_name.empty()
                                       ? call->name : call->native_binding_name;
        if (const char* reason = realtime_forbidden_call_reason(binding)) {
            err(std::string("@realtime violation: ") + reason + " '" + binding +
                "' (forbidden in realtime functions)", call->loc.line, call->loc.col);
        } else if (call->is_native && !realtime_safe_native(binding)) {
            err("@realtime violation: native call '" + binding +
                "' is not known realtime-safe (forbidden in realtime functions)",
                call->loc.line, call->loc.col);
        } else if (call->is_indirect || call->is_lambda_call ||
                   !call->module_alias.empty()) {
            err("@realtime violation: indirect or cross-module call cannot be proven realtime-safe "
                "(forbidden in realtime functions)", call->loc.line, call->loc.col);
        } else if (!call->is_native && call->script_slot >= 0) {
            err("@realtime violation: script call '" + call->name +
                "' cannot be proven realtime-safe (forbidden in realtime functions)",
                call->loc.line, call->loc.col);
        }
        if (call->receiver) validate_realtime_expr(*call->receiver);
        if (call->indirect_target) validate_realtime_expr(*call->indirect_target);
        if (call->lambda_target) validate_realtime_expr(*call->lambda_target);
        for (const auto& arg : call->args) validate_realtime_expr(*arg);
        return;
    }
    if (auto* lambda = dynamic_cast<const LambdaExpr*>(&e)) {
        const bool by_ref = !lambda->ref_capture_names.empty() ||
                            std::any_of(lambda->capture_by_ref.begin(),
                                        lambda->capture_by_ref.end(),
                                        [](bool v) { return v; });
        if (by_ref) {
            err("@realtime violation: lambda with by-reference capture "
                "(forbidden in realtime functions)", lambda->loc.line, lambda->loc.col);
        }
        return; // the synthetic lambda body is checked as its own function
    }
    if (auto* b = dynamic_cast<const BinExpr*>(&e)) {
        validate_realtime_expr(*b->lhs); validate_realtime_expr(*b->rhs); return;
    }
    if (auto* u = dynamic_cast<const UnaryExpr*>(&e)) {
        validate_realtime_expr(*u->operand); return;
    }
    if (auto* c = dynamic_cast<const CastExpr*>(&e)) {
        validate_realtime_expr(*c->operand); return;
    }
    if (auto* h = dynamic_cast<const FnHandleExpr*>(&e)) {
        if (h->operand) validate_realtime_expr(*h->operand);
        return;
    }
    if (auto* ix = dynamic_cast<const IndexExpr*>(&e)) {
        validate_realtime_expr(*ix->base); validate_realtime_expr(*ix->index); return;
    }
    if (auto* fl = dynamic_cast<const FieldExpr*>(&e)) {
        validate_realtime_expr(*fl->base); return;
    }
    if (auto* v = dynamic_cast<const ViewExpr*>(&e)) {
        validate_realtime_expr(*v->base); return;
    }
    if (auto* a = dynamic_cast<const AssignExpr*>(&e)) {
        validate_realtime_expr(*a->target); validate_realtime_expr(*a->value); return;
    }
    if (auto* t = dynamic_cast<const TernaryExpr*>(&e)) {
        validate_realtime_expr(*t->cond); validate_realtime_expr(*t->then_e);
        validate_realtime_expr(*t->else_e); return;
    }
    if (auto* sl = dynamic_cast<const StructLit*>(&e)) {
        for (const auto& field : sl->fields) validate_realtime_expr(*field.second);
        return;
    }
    if (auto* al = dynamic_cast<const ArrayLit*>(&e)) {
        for (const auto& element : al->elements) validate_realtime_expr(*element);
    }
}

void Checker::validate_realtime_stmt(const Stmt& s) {
    if (auto* ls = dynamic_cast<const LetStmt*>(&s)) {
        if (ls->init) validate_realtime_expr(*ls->init);
        return;
    }
    if (auto* es = dynamic_cast<const ExprStmt*>(&s)) {
        validate_realtime_expr(*es->expr); return;
    }
    if (auto* rs = dynamic_cast<const ReturnStmt*>(&s)) {
        if (rs->value) validate_realtime_expr(*rs->value);
        return;
    }
    if (auto* sa = dynamic_cast<const StaticAssertStmt*>(&s)) {
        validate_realtime_expr(*sa->cond); return;
    }
    if (auto* is = dynamic_cast<const IfStmt*>(&s)) {
        validate_realtime_expr(*is->cond); validate_realtime_block(is->then_b);
        if (is->has_else) validate_realtime_block(is->else_b);
        return;
    }
    if (auto* ws = dynamic_cast<const WhileStmt*>(&s)) {
        validate_realtime_expr(*ws->cond); validate_realtime_block(ws->body); return;
    }
    if (auto* fs = dynamic_cast<const ForStmt*>(&s)) {
        if (fs->init) validate_realtime_stmt(*fs->init);
        if (fs->cond) validate_realtime_expr(*fs->cond);
        if (fs->step) validate_realtime_expr(*fs->step);
        validate_realtime_block(fs->body); return;
    }
    if (auto* ds = dynamic_cast<const DoWhileStmt*>(&s)) {
        validate_realtime_block(ds->body); validate_realtime_expr(*ds->cond); return;
    }
    if (auto* fe = dynamic_cast<const ForEachStmt*>(&s)) {
        validate_realtime_expr(*fe->iter); validate_realtime_block(fe->body); return;
    }
    if (auto* ds = dynamic_cast<const DeferStmt*>(&s)) {
        validate_realtime_expr(*ds->expr); return;
    }
    if (auto* bs = dynamic_cast<const BlockStmt*>(&s)) {
        validate_realtime_block(bs->block); return;
    }
    if (auto* sw = dynamic_cast<const SwitchStmt*>(&s)) {
        validate_realtime_expr(*sw->subject);
        for (const auto& c : sw->cases) {
            if (c.value) validate_realtime_expr(*c.value);
            validate_realtime_block(c.body);
        }
        return;
    }
    if (auto* ms = dynamic_cast<const MatchStmt*>(&s)) {
        validate_realtime_expr(*ms->subject);
        for (const auto& arm : ms->arms) {
            if (arm.pattern) validate_realtime_expr(*arm.pattern);
            for (const auto& field : arm.struct_pat.fields)
                if (field.literal) validate_realtime_expr(*field.literal);
            if (arm.guard) validate_realtime_expr(*arm.guard);
            validate_realtime_block(arm.body);
        }
        return;
    }
    if (auto* tc = dynamic_cast<const TryCatchStmt*>(&s)) {
        err("@realtime violation: try/catch statement (forbidden in realtime functions)",
            tc->loc.line, tc->loc.col);
        validate_realtime_block(tc->try_body);
        validate_realtime_block(tc->catch_body);
        return;
    }
    if (auto* th = dynamic_cast<const ThrowStmt*>(&s)) {
        err("@realtime violation: throw statement (forbidden in realtime functions)",
            th->loc.line, th->loc.col);
        if (th->value) validate_realtime_expr(*th->value);
        return;
    }
    if (auto* ys = dynamic_cast<const YieldStmt*>(&s)) {
        if (ys->value) validate_realtime_expr(*ys->value);
    }
}

void Checker::validate_realtime_block(const Block& b) {
    for (const auto& stmt : b.stmts) validate_realtime_stmt(*stmt);
}

void Checker::validate_realtime(FuncDecl& f) {
    validate_realtime_block(f.body);
}

// ============================================================================
// #20 lambdas with by-value capture
// ============================================================================

// Walk an expression tree to find Idents that resolve to a local in an
// ENCLOSING scope (scope index < lambda_scope_start). Those are the captures.
// Globals, fn names, + the lambda's own params/locals (scope index >= start)
// are NOT captures. Dedups by name; preserves first-encounter order (the env
// field order).
void Checker::collect_captures_expr(const Expr& e, size_t lambda_scope_start,
                                    std::vector<std::string>& out) {
    if (auto* id = dynamic_cast<const Ident*>(&e)) {
        // Is this name a local in an ENCLOSING scope (below the lambda's own)?
        for (size_t i = 0; i < lambda_scope_start && i < scopes.size(); ++i) {
            for (const auto& v : scopes[i]) {
                if (v.name == id->name) {
                    // dedup
                    if (std::find(out.begin(), out.end(), id->name) == out.end())
                        out.push_back(id->name);
                    return;
                }
            }
        }
        return;
    }
    if (auto* b = dynamic_cast<const BinExpr*>(&e)) {
        collect_captures_expr(*b->lhs, lambda_scope_start, out);
        collect_captures_expr(*b->rhs, lambda_scope_start, out);
        return;
    }
    if (auto* u = dynamic_cast<const UnaryExpr*>(&e)) {
        collect_captures_expr(*u->operand, lambda_scope_start, out);
        return;
    }
    if (auto* c = dynamic_cast<const CastExpr*>(&e)) {
        collect_captures_expr(*c->operand, lambda_scope_start, out);
        return;
    }
    if (auto* t = dynamic_cast<const TernaryExpr*>(&e)) {
        collect_captures_expr(*t->cond, lambda_scope_start, out);
        collect_captures_expr(*t->then_e, lambda_scope_start, out);
        collect_captures_expr(*t->else_e, lambda_scope_start, out);
        return;
    }
    if (auto* a = dynamic_cast<const AssignExpr*>(&e)) {
        if (a->target) collect_captures_expr(*a->target, lambda_scope_start, out);
        if (a->value) collect_captures_expr(*a->value, lambda_scope_start, out);
        return;
    }
    if (auto* c = dynamic_cast<const CallExpr*>(&e)) {
        if (c->receiver) collect_captures_expr(*c->receiver, lambda_scope_start, out);
        if (c->indirect_target) collect_captures_expr(*c->indirect_target, lambda_scope_start, out);
        if (c->lambda_target) collect_captures_expr(*c->lambda_target, lambda_scope_start, out);
        for (auto& arg : c->args) collect_captures_expr(*arg, lambda_scope_start, out);
        return;
    }
    if (auto* ix = dynamic_cast<const IndexExpr*>(&e)) {
        collect_captures_expr(*ix->base, lambda_scope_start, out);
        collect_captures_expr(*ix->index, lambda_scope_start, out);
        return;
    }
    if (auto* fl = dynamic_cast<const FieldExpr*>(&e)) {
        collect_captures_expr(*fl->base, lambda_scope_start, out);
        return;
    }
    if (auto* v = dynamic_cast<const ViewExpr*>(&e)) {
        collect_captures_expr(*v->base, lambda_scope_start, out);
        return;
    }
    // IntLit/FloatLit/BoolLit/StringLit/StructLit/ArrayLit/EnumAccessExpr/
    // FnHandleExpr/LambdaExpr: no unbound Idents to capture (a nested lambda
    // collects its OWN captures against its own scope start; a FnHandleExpr's
    // operand is a fn name, not a local). StructLit/ArrayLit recurse via their
    // field/element exprs:
    if (auto* sl = dynamic_cast<const StructLit*>(&e)) {
        for (auto& kv : sl->fields) collect_captures_expr(*kv.second, lambda_scope_start, out);
        return;
    }
    if (auto* al = dynamic_cast<const ArrayLit*>(&e)) {
        for (auto& el : al->elements) collect_captures_expr(*el, lambda_scope_start, out);
        return;
    }
    // A nested LambdaExpr: its own body is its own synthetic fn (checked
    // separately, with its own captures). But for TRANSITIVE capture, a var
    // that the nested lambda references from an ENCLOSING scope of THIS lambda
    // (e.g. main's `base`, which is above this lambda) must ALSO be captured
    // by THIS lambda — otherwise the nested lambda's env (built in this
    // lambda's frame) could not reach it (this lambda's frame cannot access a
    // grandparent frame; the only bridge is this lambda's own env). So we DO
    // recurse into the nested lambda's synthetic-fn body here, collecting any
    // Idents it references that resolve to a scope below THIS lambda's start.
    // Idents resolving to the nested lambda's OWN params/locals (or to THIS
    // lambda's params — which are in a scope that doesn't exist yet at collect
    // time) are simply not found below `lambda_scope_start`, so they are not
    // wrongly added to THIS lambda's captures; they become the nested lambda's
    // own captures when IT is checked.
    if (auto* nle = dynamic_cast<const LambdaExpr*>(&e)) {
        FuncDecl* nlf = nullptr;
        for (auto& fn : prog->funcs) {
            if (fn.name == nle->synthetic_fn_name) { nlf = &fn; break; }
        }
        if (nlf) collect_captures_block(nlf->body, lambda_scope_start, out);
        return;
    }
}

void Checker::collect_captures_stmt(const Stmt& s, size_t lambda_scope_start,
                                    std::vector<std::string>& out) {
    if (dynamic_cast<const StaticAssertStmt*>(&s)) return;
    if (auto* ls = dynamic_cast<const LetStmt*>(&s)) {
        if (ls->init) collect_captures_expr(*ls->init, lambda_scope_start, out);
        return;
    }
    if (auto* es = dynamic_cast<const ExprStmt*>(&s)) {
        collect_captures_expr(*es->expr, lambda_scope_start, out);
        return;
    }
    if (auto* rs = dynamic_cast<const ReturnStmt*>(&s)) {
        if (rs->value) collect_captures_expr(*rs->value, lambda_scope_start, out);
        return;
    }
    if (auto* ds = dynamic_cast<const DeferStmt*>(&s)) {
        collect_captures_expr(*ds->expr, lambda_scope_start, out);
        return;
    }
    if (auto* is = dynamic_cast<const IfStmt*>(&s)) {
        collect_captures_expr(*is->cond, lambda_scope_start, out);
        collect_captures_block(is->then_b, lambda_scope_start, out);
        if (is->has_else) collect_captures_block(is->else_b, lambda_scope_start, out);
        return;
    }
    if (auto* ws = dynamic_cast<const WhileStmt*>(&s)) {
        collect_captures_expr(*ws->cond, lambda_scope_start, out);
        collect_captures_block(ws->body, lambda_scope_start, out);
        return;
    }
    if (auto* ds = dynamic_cast<const DoWhileStmt*>(&s)) {
        collect_captures_block(ds->body, lambda_scope_start, out);
        collect_captures_expr(*ds->cond, lambda_scope_start, out);
        return;
    }
    if (auto* fe = dynamic_cast<const ForEachStmt*>(&s)) {
        collect_captures_expr(*fe->iter, lambda_scope_start, out);
        collect_captures_block(fe->body, lambda_scope_start, out);
        return;
    }
    if (auto* fs = dynamic_cast<const ForStmt*>(&s)) {
        if (fs->init) collect_captures_stmt(*fs->init, lambda_scope_start, out);
        if (fs->cond) collect_captures_expr(*fs->cond, lambda_scope_start, out);
        if (fs->step) collect_captures_expr(*fs->step, lambda_scope_start, out);
        collect_captures_block(fs->body, lambda_scope_start, out);
        return;
    }
    if (auto* bs = dynamic_cast<const BlockStmt*>(&s)) {
        collect_captures_block(bs->block, lambda_scope_start, out);
        return;
    }
    if (auto* sw = dynamic_cast<const SwitchStmt*>(&s)) {
        collect_captures_expr(*sw->subject, lambda_scope_start, out);
        for (auto& c : sw->cases) collect_captures_block(c.body, lambda_scope_start, out);
        return;
    }
    if (auto* ms = dynamic_cast<const MatchStmt*>(&s)) {
        collect_captures_expr(*ms->subject, lambda_scope_start, out);
        for (auto& arm : ms->arms) {
            if (arm.guard) collect_captures_expr(*arm.guard, lambda_scope_start, out);
            collect_captures_block(arm.body, lambda_scope_start, out);
        }
        return;
    }
    if (auto* tc = dynamic_cast<const TryCatchStmt*>(&s)) {
        collect_captures_block(tc->try_body, lambda_scope_start, out);
        collect_captures_block(tc->catch_body, lambda_scope_start, out);
        return;
    }
    if (auto* th = dynamic_cast<const ThrowStmt*>(&s)) {
        if (th->value) collect_captures_expr(*th->value, lambda_scope_start, out);
        return;
    }
    // YieldStmt (#21): recurse into the yielded value (may capture).
    if (auto* ys = dynamic_cast<const YieldStmt*>(&s)) {
        if (ys->value) collect_captures_expr(*ys->value, lambda_scope_start, out);
        return;
    }
}

void Checker::collect_captures_block(const Block& b, size_t lambda_scope_start,
                                     std::vector<std::string>& out) {
    for (auto& s : b.stmts) collect_captures_stmt(*s, lambda_scope_start, out);
}

// Type-check a LambdaExpr at its creation site. Determines captures, builds
// the env, prepends the hidden __env param to the synthetic fn, registers
// the fn's signature, + sets the LambdaExpr's type to a 16-byte lambda type.
// The synthetic fn's BODY is checked lazily in check_func (when the fn loop
// reaches it) — NOT here (the body may reference the captures, which are
// bound in check_lambda_func).
const Type* Checker::check_lambda(LambdaExpr& le) {
    // find the synthetic FuncDecl
    FuncDecl* lf = nullptr;
    for (auto& fn : prog->funcs) {
        if (fn.name == le.synthetic_fn_name) { lf = &fn; break; }
    }
    if (!lf) {
        err("internal: lambda synthetic fn '" + le.synthetic_fn_name + "' not found",
            le.loc.line, le.loc.col);
        le.ty = intern(type_void()); return le.ty;
    }
    // stamp the slot (for codegen's dispatch + the lambda value's slot field)
    auto sit = script_slots->find(lf->name);
    if (sit != script_slots->end()) le.slot = sit->second;
    lf->slot = le.slot;
    // Determine captures: walk the synthetic fn's body for Idents resolving to
    // an ENCLOSING scope (below the lambda's own scope, which doesn't exist yet
    // — we use the current scope stack depth as the boundary). The lambda's own
    // params/locals will live in a scope pushed during check_lambda_func; any
    // local at scope index < scopes.size() now is an enclosing-scope capture.
    size_t lambda_scope_start = scopes.size();
    // #20 nested-lambda capture: snapshot the enclosing scope chain so
    // check_lambda_func can restore it before checking this lambda's body.
    // A nested lambda's collect_captures runs during the enclosing lambda's
    // body check; without restoring this chain, check_func's scopes.clear()
    // would have wiped the grandparent scopes (e.g. main's locals), so a
    // capture resolving to a grandparent var would be missed. Save the
    // enclosing scopes [0..lambda_scope_start) (the lambda's own scope is
    // pushed fresh in check_lambda_func on top of this restored chain).
    std::vector<std::vector<Var>> snap;
    for (size_t i = 0; i < lambda_scope_start && i < scopes.size(); ++i)
        snap.push_back(scopes[i]);
    lambda_scope_snapshots[lf->name] = std::move(snap);
    std::vector<std::string> caps;
    collect_captures_block(lf->body, lambda_scope_start, caps);
    // Build the env layout: each capture is 8 bytes (v1: scalars only — i64/
    // f64/etc.). Capture types come from the enclosing scope's var types.
    // (A non-scalar capture — slice/struct/array — is rejected for v1; by-ref
    // capture via GC is the follow-up.)
    std::vector<std::shared_ptr<Type>> cap_types;
    std::vector<int32_t> cap_offsets;
    std::vector<bool> cap_by_ref;  // #20: per-capture by-ref flag (parallel to caps)
    int32_t env_off = 0;
    for (const auto& name : caps) {
        const Type* vt = lookup_var(name);
        if (!vt) {
            // shouldn't happen (collect only picks up resolved enclosing locals)
            err("lambda capture '" + name + "' has no type", le.loc.line, le.loc.col);
            continue;
        }
        // v1: captures must be scalars (fit in 8 bytes, no GC needed). Reject
        // slices/structs/arrays (by-value copy of those is a follow-up). A
        // by-ref capture stores a POINTER (8 bytes) to the scalar's storage,
        // so the scalar-only restriction still holds (the pointer is 8 bytes).
        if (vt->is_slice || vt->array_len > 0 ||
            (!vt->struct_name.empty() && is_registered_struct(vt))) {
            err("lambda capture '" + name + "' has type " + vt->to_string() +
                "; v1 lambdas capture only scalar values (slice/struct/array " +
                "capture is a follow-up)", le.loc.line, le.loc.col);
            // still record it so the body resolves (use the type as-is)
        }
        // #20 by-ref: a name explicitly marked `&` in the capture list is a
        // by-ref capture (the env slot holds a pointer to the variable's
        // storage). Default (no capture list, or bare name in the list) is
        // by-value (the env slot holds a copy). by-ref captures are MUTABLE
        // inside the body (a write mutates the original); by-value captures
        // stay immutable (bound is_const below).
        bool by_ref = (std::find(le.ref_capture_names.begin(),
                                le.ref_capture_names.end(), name)
                       != le.ref_capture_names.end());
        cap_types.push_back(std::make_shared<Type>(*vt));
        cap_offsets.push_back(env_off);
        cap_by_ref.push_back(by_ref);
        env_off += 8;  // every capture occupies one 8-byte slot (value OR ptr)
    }
    int32_t env_size = env_off;
    // Record capture metadata on BOTH the LambdaExpr (creation-site codegen)
    // and the synthetic FuncDecl (body codegen loads captures from env).
    le.captures = caps;
    le.capture_types = cap_types;
    le.capture_offsets = cap_offsets;
    le.capture_by_ref = cap_by_ref;
    le.env_size = env_size;
    lf->lambda_captures = caps;
    lf->lambda_capture_types = cap_types;
    lf->lambda_capture_offsets = cap_offsets;
    lf->lambda_capture_by_ref = cap_by_ref;
    lf->env_size = env_size;
    // Prepend the hidden __env i64 param to the synthetic fn (params[0]).
    // The body's captures are NOT params — they're env-capture vars bound in
    // check_lambda_func. The declared params shift to params[1..].
    // Guard: if __env is already params[0] (check_lambda re-ran for this fn —
    // e.g. a LambdaExpr re-evaluated during a re-check), don't prepend again
    // (a double __env would corrupt the param layout + sig word count).
    const bool already_has_env =
        !lf->params.empty() && lf->params[0].name == "__env";
    if (!already_has_env) {
        Param env_param;
        env_param.name = "__env";
        env_param.ty = std::make_shared<Type>(type_i64());
        env_param.loc = lf->loc;
        std::vector<Param> new_params;
        new_params.push_back(std::move(env_param));
        for (auto& p : lf->params) new_params.push_back(std::move(p));
        lf->params = std::move(new_params);
    }
    // Register the synthetic fn's signature (so calls to it via the slot +
    // script_sigs resolve). The recorded sig uses the DECLARED params (sans
    // __env) — but script_sigs indexes ALL params (including __env) for the
    // call-site word-count. We register the full param list (with __env); the
    // lambda-call path accounts for the hidden env arg.
    ScriptSig ss;
    ss.ret = intern(*lf->ret);
    for (auto& p : lf->params) ss.params.push_back(intern(*p.ty));
    script_sigs[lf->name] = ss;
    // Build the lambda value type: 16 bytes {slot, env_ptr}, recorded sig =
    // the declared params (sans env) + ret.
    Type lt;
    lt.is_lambda = true;
    lt.has_recorded_sig = true;
    lt.prim = Prim::I64;  // placeholder prim (byte_size uses is_lambda=16)
    for (auto& p : le.params) lt.recorded_params.push_back(p.ty);
    lt.recorded_ret = le.ret;
    const Type* ty = intern(lt);
    le.ty = ty;
    return ty;
}

// Check a synthetic lambda fn's body. Binds the hidden __env param + the
// captures (as env-capture vars) + the declared params, then checks the body.
void Checker::check_lambda_func(FuncDecl& f) {
    current_func = &f;  // #21: track for yield->coroutine marking
    // #20 nested-lambda capture: restore the enclosing scope chain saved in
    // check_lambda, then push THIS lambda's own scope on top. check_func did
    // scopes.clear()+push_scope() before calling us, leaving a single empty
    // scope; we replace it with the restored chain + a fresh top scope so that
    // a NESTED lambda (checked during this body's check) can collect captures
    // from the grandparent scopes too (e.g. main's locals), and so THIS body's
    // own Idents resolve against the enclosing chain. If no snapshot exists
    // (a lambda fn reached without its LambdaExpr being checked — shouldn't
    // happen for a referenced lambda), fall back to the single empty scope
    // check_func already pushed.
    auto snit = lambda_scope_snapshots.find(f.name);
    if (snit != lambda_scope_snapshots.end()) {
        scopes = snit->second;  // restore enclosing chain
        push_scope();           // fresh top scope for this lambda's params/captures/locals
    }
    // params[0] = __env (i64 pointer to the env struct). Bind it as a normal
    // local so codegen spills it to a frame slot (the env-capture loads read
    // __env's frame slot to get env_ptr).
    declare(f.params[0].name, intern(*f.params[0].ty), false, false, f.params[0].loc);
    // Bind the captures as env-capture vars (is_env_capture=true + offset).
    // These resolve capture Idents in the body to env loads (not frame slots).
    // #20 by-ref: a by-ref capture is MUTABLE (is_const=false) so a write in
    // the body mutates the original through the pointer; a by-value capture
    // stays immutable (is_const=true). env_capture_by_ref tells codegen to
    // double-dereference on read + store-through on write.
    for (size_t i = 0; i < f.lambda_captures.size(); ++i) {
        bool by_ref = i < f.lambda_capture_by_ref.size() && f.lambda_capture_by_ref[i];
        Var v;
        v.name = f.lambda_captures[i];
        v.ty = intern(*f.lambda_capture_types[i]);
        v.is_const = !by_ref;          // by-ref captures are mutable; by-value immutable
        v.local_array_view = false;
        v.is_env_capture = true;
        v.env_offset = f.lambda_capture_offsets[i];
        v.env_capture_by_ref = by_ref;
        scopes.back().push_back(std::move(v));
    }
    // Bind the declared params (params[1..]) normally.
    for (size_t i = 1; i < f.params.size(); ++i) {
        declare(f.params[i].name, intern(*f.params[i].ty), false, false, f.params[i].loc);
    }
    const Type* ret_ty = intern(*f.ret);
    bool returns = false;
    check_block(f.body, ret_ty, returns);
    if (!ret_ty->is_void() && !returns) {
        err("lambda not all paths return a value", f.loc.line, f.loc.col);
    }
}

// #20 pre-pass: upgrade `fn(Args)->Ret` param types to is_lambda where a call
// site passes a lambda to that param. For each CallExpr to a same-module
// script fn, for each arg that is a LambdaExpr, if the callee's param at that
// position (offset by the receiver) is a fn-handle type WITH a recorded sig
// matching the lambda's sig, mutate that param's `ty` in place to is_lambda.
// This makes the param 16 bytes + dispatch as a lambda call in the callee's
// body. (The mutation is on the FuncDecl's shared_ptr<Type>, safe + permanent.)
void Checker::upgrade_lambda_param_types() {
    for (auto& fn : prog->funcs) {
        // #20 nested lambdas: a lambda fn's body CAN pass a lambda-typed value
        // to another fn (e.g. `apply(inner, 30)` inside an outer lambda's body),
        // so we MUST walk lambda fn bodies here too — otherwise the callee's
        // fn-handle param is never upgraded to is_lambda, the lambda value
        // {slot, env_ptr} is passed as a single word, and the callee reads a
        // garbage env_ptr. (Previously skipped with "synthetic lambda fns have
        // no lambda-arg calls to upgrade", which only held for non-nested
        // lambdas.)
        std::unordered_set<std::string> lam_locals;  // this fn's `let X = <lambda>` names
        upgrade_lambda_params_block(fn.body, lam_locals);
    }
}
void Checker::upgrade_lambda_params_block(Block& b, std::unordered_set<std::string>& lam_locals) {
    for (auto& s : b.stmts) upgrade_lambda_params_stmt(*s, lam_locals);
}
void Checker::upgrade_lambda_params_stmt(Stmt& s, std::unordered_set<std::string>& lam_locals) {
    if (auto* ls = dynamic_cast<LetStmt*>(&s)) {
        if (ls->init) upgrade_lambda_params_expr(*ls->init, lam_locals);
        // track `let X = <LambdaExpr>` so a later `callee(..., X, ...)` upgrades
        // the callee's param (the lambda value flows through X).
        if (ls->init && dynamic_cast<LambdaExpr*>(ls->init.get()))
            lam_locals.insert(ls->name);
        return;
    }
    if (auto* es = dynamic_cast<ExprStmt*>(&s)) { upgrade_lambda_params_expr(*es->expr, lam_locals); return; }
    if (auto* rs = dynamic_cast<ReturnStmt*>(&s)) { if (rs->value) upgrade_lambda_params_expr(*rs->value, lam_locals); return; }
    if (auto* ds = dynamic_cast<DeferStmt*>(&s)) { upgrade_lambda_params_expr(*ds->expr, lam_locals); return; }
    if (auto* is = dynamic_cast<IfStmt*>(&s)) {
        upgrade_lambda_params_expr(*is->cond, lam_locals);
        upgrade_lambda_params_block(is->then_b, lam_locals);
        if (is->has_else) upgrade_lambda_params_block(is->else_b, lam_locals);
        return;
    }
    if (auto* ws = dynamic_cast<WhileStmt*>(&s)) { upgrade_lambda_params_expr(*ws->cond, lam_locals); upgrade_lambda_params_block(ws->body, lam_locals); return; }
    if (auto* ds = dynamic_cast<DoWhileStmt*>(&s)) { upgrade_lambda_params_block(ds->body, lam_locals); upgrade_lambda_params_expr(*ds->cond, lam_locals); return; }
    if (auto* fe = dynamic_cast<ForEachStmt*>(&s)) { upgrade_lambda_params_expr(*fe->iter, lam_locals); upgrade_lambda_params_block(fe->body, lam_locals); return; }
    if (auto* fs = dynamic_cast<ForStmt*>(&s)) {
        if (fs->init) upgrade_lambda_params_stmt(*fs->init, lam_locals);
        if (fs->cond) upgrade_lambda_params_expr(*fs->cond, lam_locals);
        if (fs->step) upgrade_lambda_params_expr(*fs->step, lam_locals);
        upgrade_lambda_params_block(fs->body, lam_locals);
        return;
    }
    if (auto* bs = dynamic_cast<BlockStmt*>(&s)) { upgrade_lambda_params_block(bs->block, lam_locals); return; }
    if (auto* sw = dynamic_cast<SwitchStmt*>(&s)) {
        upgrade_lambda_params_expr(*sw->subject, lam_locals);
        for (auto& c : sw->cases) upgrade_lambda_params_block(c.body, lam_locals);
        return;
    }
    if (auto* ms = dynamic_cast<MatchStmt*>(&s)) {
        upgrade_lambda_params_expr(*ms->subject, lam_locals);
        for (auto& arm : ms->arms) {
            if (arm.guard) upgrade_lambda_params_expr(*arm.guard, lam_locals);
            upgrade_lambda_params_block(arm.body, lam_locals);
        }
        return;
    }
    if (auto* tc = dynamic_cast<TryCatchStmt*>(&s)) { upgrade_lambda_params_block(tc->try_body, lam_locals); upgrade_lambda_params_block(tc->catch_body, lam_locals); return; }
    if (auto* th = dynamic_cast<ThrowStmt*>(&s)) { if (th->value) upgrade_lambda_params_expr(*th->value, lam_locals); return; }
    if (auto* ys = dynamic_cast<YieldStmt*>(&s)) { if (ys->value) upgrade_lambda_params_expr(*ys->value, lam_locals); return; }
}
void Checker::upgrade_lambda_params_expr(Expr& e, const std::unordered_set<std::string>& lam_locals) {
    if (auto* c = dynamic_cast<CallExpr*>(&e)) {
        if (c->receiver) upgrade_lambda_params_expr(*c->receiver, lam_locals);
        if (c->indirect_target) upgrade_lambda_params_expr(*c->indirect_target, lam_locals);
        if (c->lambda_target) upgrade_lambda_params_expr(*c->lambda_target, lam_locals);
        for (auto& a : c->args) upgrade_lambda_params_expr(*a, lam_locals);
        // Only same-module script calls (a named callee in script_slots, no
        // module_alias, not native/indirect/lambda-call) can have their param
        // types upgraded. Skip native/cross-module/indirect calls.
        if (!c->name.empty() && c->module_alias.empty() && !c->is_native &&
            !c->is_indirect && !c->is_lambda_call) {
            auto sit = script_slots->find(c->name);
            if (sit != script_slots->end()) {
                // find the callee FuncDecl
                FuncDecl* callee = nullptr;
                for (auto& fn : prog->funcs) if (fn.name == c->name) { callee = &fn; break; }
                if (callee) {
                    size_t off = c->receiver ? 1 : 0;
                    for (size_t i = 0; i < c->args.size(); ++i) {
                        // An arg is a lambda flow if it is directly a LambdaExpr
                        // OR an Ident naming a `let X = <lambda>` local.
                        LambdaExpr* le = dynamic_cast<LambdaExpr*>(c->args[i].get());
                        if (!le) {
                            if (auto* id = dynamic_cast<Ident*>(c->args[i].get())) {
                                if (lam_locals.count(id->name)) {
                                    // find the let's lambda to get its sig
                                    // (walk this fn's body for the let — but we
                                    // don't have it here; instead, look up the
                                    // lambda's synthetic fn by reconstructing
                                    // the name is not feasible. So: skip sig
                                    // check for an Ident arg; upgrade iff the
                                    // param is ANY `fn(Args)->Ret` with the
                                    // right ARITY — the actual sig match is
                                    // enforced at the call site by check_expr.)
                                    size_t pidx = off + i;
                                    if (pidx < callee->params.size()) {
                                        Type& pt = *callee->params[pidx].ty;
                                        if (pt.is_fn_handle && pt.has_recorded_sig) {
                                            pt.is_fn_handle = false;
                                            pt.is_lambda = true;
                                        }
                                    }
                                }
                            }
                        }
                        if (le) {
                            size_t pidx = off + i;
                            if (pidx < callee->params.size()) {
                                Type& pt = *callee->params[pidx].ty;
                                // upgrade a `fn(Args)->Ret` fn-handle param to
                                // is_lambda iff the recorded sig matches the
                                // lambda's declared sig.
                                if (pt.is_fn_handle && pt.has_recorded_sig &&
                                    pt.recorded_params.size() == le->params.size()) {
                                    bool sig_match = true;
                                    for (size_t j = 0; j < le->params.size(); ++j) {
                                        if (!pt.recorded_params[j]->same(*le->params[j].ty)) { sig_match = false; break; }
                                    }
                                    if (sig_match && le->ret && pt.recorded_ret &&
                                        pt.recorded_ret->same(*le->ret)) {
                                        pt.is_fn_handle = false;
                                        pt.is_lambda = true;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        return;
    }
    if (auto* b = dynamic_cast<BinExpr*>(&e)) { upgrade_lambda_params_expr(*b->lhs, lam_locals); upgrade_lambda_params_expr(*b->rhs, lam_locals); return; }
    if (auto* u = dynamic_cast<UnaryExpr*>(&e)) { upgrade_lambda_params_expr(*u->operand, lam_locals); return; }
    if (auto* c = dynamic_cast<CastExpr*>(&e)) { upgrade_lambda_params_expr(*c->operand, lam_locals); return; }
    if (auto* t = dynamic_cast<TernaryExpr*>(&e)) { upgrade_lambda_params_expr(*t->cond, lam_locals); upgrade_lambda_params_expr(*t->then_e, lam_locals); upgrade_lambda_params_expr(*t->else_e, lam_locals); return; }
    if (auto* a = dynamic_cast<AssignExpr*>(&e)) { if (a->target) upgrade_lambda_params_expr(*a->target, lam_locals); if (a->value) upgrade_lambda_params_expr(*a->value, lam_locals); return; }
    if (auto* ix = dynamic_cast<IndexExpr*>(&e)) { upgrade_lambda_params_expr(*ix->base, lam_locals); upgrade_lambda_params_expr(*ix->index, lam_locals); return; }
    if (auto* fl = dynamic_cast<FieldExpr*>(&e)) { upgrade_lambda_params_expr(*fl->base, lam_locals); return; }
    if (auto* sl = dynamic_cast<StructLit*>(&e)) { for (auto& kv : sl->fields) upgrade_lambda_params_expr(*kv.second, lam_locals); return; }
    if (auto* al = dynamic_cast<ArrayLit*>(&e)) { for (auto& el : al->elements) upgrade_lambda_params_expr(*el, lam_locals); return; }
    // a nested LambdaExpr is checked on its own; its body's lambda-arg calls
    // are upgraded when THIS walk reaches the nested lambda's synthetic fn's
    // body (the outer loop visits all prog->funcs including nested synthetic
    // fns). So do NOT recurse into the LambdaExpr here.
}

// ============================================================================
// constexpr fn evaluation (Tier 1) — compile-time interpreter
// ============================================================================
// A bounded tree-walking interpreter that evaluates a constexpr fn call at
// sema time. Produces a compile-time i64 result that replaces the call site
// (an IntLit), so downstream consumers (case labels, global inits, codegen
// const-fold) see the literal directly. Only i64 integer fns are supported
// in this increment; float/bool/struct fns skip constexpr eval.
//
// Bounds: max 100000 loop iterations per loop, max 256 recursion depth.
// Exceeding either returns false (the call falls back to a runtime call —
// no error, the fn is still callable at runtime). A zero divisor in Div/Mod
// also returns false (the runtime trap must handle it, same as
// try_eval_const_i64's rationale).
// ============================================================================

bool Checker::ce_eval_expr(const Expr& e, int64_t& out, ConstEvalCtx& ctx, std::string& err) {
    if (auto* lit = dynamic_cast<const IntLit*>(&e)) {
        out = lit->v; return true;
    }
    if (auto* lit = dynamic_cast<const BoolLit*>(&e)) {
        out = lit->v ? 1 : 0; return true;
    }
    if (auto* u = dynamic_cast<const UnaryExpr*>(&e)) {
        int64_t v;
        if (!ce_eval_expr(*u->operand, v, ctx, err)) return false;
        switch (u->op) {
        case UnaryExpr::Op::Neg:    out = bit_cast_i64(uint64_t(0) - uint64_t(v)); return true;
        case UnaryExpr::Op::BitNot: out = ~v; return true;
        case UnaryExpr::Op::Not:    out = v ? 0 : 1; return true;
        }
        return false;
    }
    if (auto* b = dynamic_cast<const BinExpr*>(&e)) {
        // short-circuit logical ops
        if (b->op == BinExpr::Op::LAnd) {
            int64_t l;
            if (!ce_eval_expr(*b->lhs, l, ctx, err)) return false;
            if (!l) { out = 0; return true; }
            int64_t r;
            if (!ce_eval_expr(*b->rhs, r, ctx, err)) return false;
            out = r ? 1 : 0; return true;
        }
        if (b->op == BinExpr::Op::LOr) {
            int64_t l;
            if (!ce_eval_expr(*b->lhs, l, ctx, err)) return false;
            if (l) { out = 1; return true; }
            int64_t r;
            if (!ce_eval_expr(*b->rhs, r, ctx, err)) return false;
            out = r ? 1 : 0; return true;
        }
        int64_t l, r;
        if (!ce_eval_expr(*b->lhs, l, ctx, err)) return false;
        if (!ce_eval_expr(*b->rhs, r, ctx, err)) return false;
        switch (b->op) {
        case BinExpr::Op::Add: out = bit_cast_i64(uint64_t(l) + uint64_t(r)); return true;
        case BinExpr::Op::Sub: out = bit_cast_i64(uint64_t(l) - uint64_t(r)); return true;
        case BinExpr::Op::Mul: out = bit_cast_i64(uint64_t(l) * uint64_t(r)); return true;
        case BinExpr::Op::And: out = l & r; return true;
        case BinExpr::Op::Or:  out = l | r; return true;
        case BinExpr::Op::Xor: out = l ^ r; return true;
        case BinExpr::Op::Shl: out = bit_cast_i64(uint64_t(l) << (r & 63)); return true;
        case BinExpr::Op::Shr: {
            int sh = int(r & 63);
            uint64_t ur = uint64_t(l) >> sh;
            if (sh != 0 && l < 0) ur |= ~((1ULL << (64 - sh)) - 1);
            out = bit_cast_i64(ur); return true;
        }
        case BinExpr::Op::Div:
            if (r == 0) { err = "constexpr division by zero"; return false; }
            out = l / r; return true;
        case BinExpr::Op::Mod:
            if (r == 0) { err = "constexpr modulo by zero"; return false; }
            out = l % r; return true;
        case BinExpr::Op::Eq: out = (l == r) ? 1 : 0; return true;
        case BinExpr::Op::Neq: out = (l != r) ? 1 : 0; return true;
        case BinExpr::Op::Lt: out = (l < r) ? 1 : 0; return true;
        case BinExpr::Op::Le: out = (l <= r) ? 1 : 0; return true;
        case BinExpr::Op::Gt: out = (l > r) ? 1 : 0; return true;
        case BinExpr::Op::Ge: out = (l >= r) ? 1 : 0; return true;
        default: return false;
        }
    }
    if (auto* id = dynamic_cast<const Ident*>(&e)) {
        for (int i = int(ctx.scopes.size()) - 1; i >= 0; --i) {
            auto it = ctx.scopes[size_t(i)].find(id->name);
            if (it != ctx.scopes[size_t(i)].end()) { out = it->second; return true; }
        }
        err = "constexpr: unknown variable '" + id->name + "'";
        return false;
    }
    if (auto* c = dynamic_cast<const CallExpr*>(&e)) {
        // Only direct calls to constexpr script fns with all-constant args.
        // Indirect / native / cross-module / method calls are not foldable.
        if (c->is_indirect || c->receiver || !c->module_alias.empty()) return false;
        const FuncDecl* fn = nullptr;
        for (auto& f : prog->funcs) {
            if (f.name == c->name) { fn = &f; break; }
        }
        if (!fn || !fn->is_constexpr) return false;
        // i64 integer fns only in this increment
        if (!fn->ret || !fn->ret->is_int()) return false;
        for (auto& p : fn->params)
            if (!p.ty || !p.ty->is_int()) return false;
        if (c->args.size() != fn->params.size()) return false;
        std::vector<int64_t> arg_vals;
        arg_vals.reserve(c->args.size());
        for (auto& a : c->args) {
            int64_t v;
            if (!ce_eval_expr(*a, v, ctx, err)) return false;
            arg_vals.push_back(v);
        }
        return eval_constexpr_fn(*fn, arg_vals, out, err, ctx.depth + 1);
    }
    if (auto* t = dynamic_cast<const TernaryExpr*>(&e)) {
        int64_t c;
        if (!ce_eval_expr(*t->cond, c, ctx, err)) return false;
        return ce_eval_expr(c ? *t->then_e : *t->else_e, out, ctx, err);
    }
    if (auto* c = dynamic_cast<const CastExpr*>(&e)) {
        // int-to-int casts: evaluate the operand (i64 holds all integer widths)
        if (!c->to || !c->to->is_int()) return false;
        return ce_eval_expr(*c->operand, out, ctx, err);
    }
    if (auto* a = dynamic_cast<const AssignExpr*>(&e)) {
        // Only Ident targets are supported in the i64 interpreter.
        auto* id = dynamic_cast<Ident*>(a->target.get());
        if (!id) return false;
        int64_t rhs;
        if (!ce_eval_expr(*a->value, rhs, ctx, err)) return false;
        if (a->compound) {
            // A compound-assign (x += rhs) requires x to already exist in an
            // enclosing scope. If it isn't found, cur would be read
            // uninitialized below — detect that and fail the eval instead.
            bool found = false;
            int64_t cur = 0;
            for (int i = int(ctx.scopes.size()) - 1; i >= 0; --i) {
                auto it = ctx.scopes[size_t(i)].find(id->name);
                if (it != ctx.scopes[size_t(i)].end()) { cur = it->second; found = true; break; }
            }
            if (!found) {
                err = "constexpr: compound-assign to unknown variable '" + id->name + "'";
                return false;
            }
            // replicate BinExpr arithmetic for the compound op
            int64_t tmp = 0;
            switch (*a->compound) {
            case BinExpr::Op::Add: tmp = bit_cast_i64(uint64_t(cur) + uint64_t(rhs)); break;
            case BinExpr::Op::Sub: tmp = bit_cast_i64(uint64_t(cur) - uint64_t(rhs)); break;
            case BinExpr::Op::Mul: tmp = bit_cast_i64(uint64_t(cur) * uint64_t(rhs)); break;
            case BinExpr::Op::Div:
                if (rhs == 0) { err = "constexpr division by zero"; return false; }
                tmp = cur / rhs; break;
            case BinExpr::Op::Mod:
                if (rhs == 0) { err = "constexpr modulo by zero"; return false; }
                tmp = cur % rhs; break;
            case BinExpr::Op::And: tmp = cur & rhs; break;
            case BinExpr::Op::Or:  tmp = cur | rhs; break;
            case BinExpr::Op::Xor: tmp = cur ^ rhs; break;
            case BinExpr::Op::Shl: tmp = bit_cast_i64(uint64_t(cur) << (rhs & 63)); break;
            case BinExpr::Op::Shr: {
                int sh = int(rhs & 63);
                uint64_t ur = uint64_t(cur) >> sh;
                if (sh != 0 && cur < 0) ur |= ~((1ULL << (64 - sh)) - 1);
                tmp = bit_cast_i64(ur); break;
            }
            default: return false;
            }
            rhs = tmp;
        }
        for (int i = int(ctx.scopes.size()) - 1; i >= 0; --i) {
            auto it = ctx.scopes[size_t(i)].find(id->name);
            if (it != ctx.scopes[size_t(i)].end()) { it->second = rhs; out = rhs; return true; }
        }
        err = "constexpr: assignment to unknown variable '" + id->name + "'";
        return false;
    }
    // FloatLit, StringLit, FnHandleExpr, SizeofExpr, OffsetofExpr,
    // StructLit, ArrayLit, EnumAccessExpr (already lowered), FieldExpr,
    // IndexExpr, ViewExpr: not supported in the i64 constexpr interpreter.
    return false;
}

bool Checker::ce_eval_block(const Block& b, ConstEvalCtx& ctx, std::string& err) {
    ctx.scopes.emplace_back();
    for (auto& s : b.stmts) {
        if (!ce_eval_stmt(*s, ctx, err)) return false;
        if (ctx.returned || ctx.broke || ctx.continued) break;
    }
    ctx.scopes.pop_back();
    return true;
}

bool Checker::ce_eval_stmt(const Stmt& s, ConstEvalCtx& ctx, std::string& err) {
    if (auto* es = dynamic_cast<const ExprStmt*>(&s)) {
        int64_t v;
        return ce_eval_expr(*es->expr, v, ctx, err);
    }
    if (auto* ls = dynamic_cast<const LetStmt*>(&s)) {
        int64_t v = 0;
        if (ls->init) {
            if (!ce_eval_expr(*ls->init, v, ctx, err)) return false;
        }
        if (ctx.scopes.empty()) ctx.scopes.emplace_back();
        ctx.scopes.back()[ls->name] = v;
        return true;
    }
    if (auto* is = dynamic_cast<const IfStmt*>(&s)) {
        int64_t c;
        if (!ce_eval_expr(*is->cond, c, ctx, err)) return false;
        if (c) return ce_eval_block(is->then_b, ctx, err);
        if (is->has_else) return ce_eval_block(is->else_b, ctx, err);
        return true;
    }
    if (auto* ws = dynamic_cast<const WhileStmt*>(&s)) {
        while (true) {
            int64_t c;
            if (!ce_eval_expr(*ws->cond, c, ctx, err)) return false;
            if (!c) break;
            // Charge the shared TOTAL iteration budget (not a per-loop
            // counter) so nested loops cannot multiply past it (a per-loop
            // 100k cap is bypassed by N nested loops = 100k^N iters). The
            // budget is shared across every loop at every nesting level in
            // this eval, making the bound a real total-work bound.
            if (++ctx.total_iters > CE_MAX_TOTAL_ITERS) {
                err = "constexpr loop exceeded iteration limit"; return false;
            }
            ctx.broke = false; ctx.continued = false;
            if (!ce_eval_block(ws->body, ctx, err)) return false;
            if (ctx.returned) break;
            if (ctx.broke) { ctx.broke = false; break; }
            // continued or fell through: keep looping
        }
        return true;
    }
    if (auto* fs = dynamic_cast<const ForStmt*>(&s)) {
        // for-init runs in a scope that wraps the whole loop (the loop var
        // persists across iterations, same as runtime semantics)
        ctx.scopes.emplace_back();
        if (fs->init) {
            if (!ce_eval_stmt(*fs->init, ctx, err)) { ctx.scopes.pop_back(); return false; }
        }
        while (true) {
            if (fs->cond) {
                int64_t c;
                if (!ce_eval_expr(*fs->cond, c, ctx, err)) { ctx.scopes.pop_back(); return false; }
                if (!c) break;
            }
            // Shared TOTAL iteration budget (see WhileStmt above): nested
            // loops cannot multiply past this single per-eval budget.
            if (++ctx.total_iters > CE_MAX_TOTAL_ITERS) {
                err = "constexpr loop exceeded iteration limit"; ctx.scopes.pop_back(); return false;
            }
            ctx.broke = false; ctx.continued = false;
            if (!ce_eval_block(fs->body, ctx, err)) { ctx.scopes.pop_back(); return false; }
            if (ctx.returned) break;
            if (ctx.broke) { ctx.broke = false; break; }
            if (fs->step) {
                int64_t v;
                if (!ce_eval_expr(*fs->step, v, ctx, err)) { ctx.scopes.pop_back(); return false; }
            }
        }
        ctx.scopes.pop_back();
        return true;
    }
    if (auto* ds = dynamic_cast<const DoWhileStmt*>(&s)) {
        while (true) {
            ctx.broke = false; ctx.continued = false;
            if (!ce_eval_block(ds->body, ctx, err)) return false;
            if (ctx.returned) break;
            if (ctx.broke) { ctx.broke = false; break; }
            int64_t c;
            if (!ce_eval_expr(*ds->cond, c, ctx, err)) return false;
            if (!c) break;
            // Shared TOTAL iteration budget (see WhileStmt above).
            if (++ctx.total_iters > CE_MAX_TOTAL_ITERS) {
                err = "constexpr loop exceeded iteration limit"; return false;
            }
        }
        return true;
    }
    if (auto* rs = dynamic_cast<const ReturnStmt*>(&s)) {
        if (rs->value) {
            if (!ce_eval_expr(*rs->value, ctx.result, ctx, err)) return false;
        } else {
            ctx.result = 0;
        }
        ctx.returned = true;
        return true;
    }
    if (dynamic_cast<const BreakStmt*>(&s)) {
        ctx.broke = true;
        return true;
    }
    if (dynamic_cast<const ContinueStmt*>(&s)) {
        ctx.continued = true;
        return true;
    }
    if (auto* bs = dynamic_cast<const BlockStmt*>(&s)) {
        return ce_eval_block(bs->block, ctx, err);
    }
    // SwitchStmt, MatchStmt, ForEachStmt, DeferStmt: not supported in the
    // i64 constexpr interpreter — return false so the call falls back to
    // runtime (sema still type-checks the body normally).
    return false;
}

bool Checker::eval_constexpr_fn(const FuncDecl& fn, const std::vector<int64_t>& arg_values,
                                int64_t& result_out, std::string& err, int depth) {
    if (depth > 256) { err = "constexpr recursion depth exceeded"; return false; }
    ConstEvalCtx ctx;
    ctx.depth = depth;
    ctx.scopes.emplace_back();
    // bind params
    for (size_t i = 0; i < fn.params.size() && i < arg_values.size(); ++i)
        ctx.scopes.back()[fn.params[i].name] = arg_values[i];
    // walk body
    for (auto& s : fn.body.stmts) {
        if (!ce_eval_stmt(*s, ctx, err)) return false;
        if (ctx.returned) break;
    }
    result_out = ctx.result;
    return true;
}

// ============================================================================
// constexpr call folding pre-pass (mirrors lower_enum_access_expr)
// ============================================================================
// Walks every ExprPtr slot bottom-up; when a CallExpr to a constexpr fn with
// all-constant i64 args is found, evaluates it and replaces the CallExpr with
// an IntLit carrying the result. Runs AFTER enum lowering (so EnumAccessExpr
// nodes are already IntLits) and BEFORE check_expr (so check_expr / case
// labels / global inits / codegen all see the folded literal). If the eval
// fails (non-const arg, too-deep recursion, unsupported node), the CallExpr
// is left as-is — the fn is still callable at runtime.
// ============================================================================

void Checker::try_fold_constexpr_call(ExprPtr& slot) {
    auto* c = dynamic_cast<CallExpr*>(slot.get());
    if (!c) return;
    // skip indirect / cross-module / method calls
    if (c->is_indirect || c->receiver || !c->module_alias.empty()) return;
    const FuncDecl* fn = nullptr;
    for (auto& f : prog->funcs) {
        if (f.name == c->name) { fn = &f; break; }
    }
    if (!fn || !fn->is_constexpr) return;
    // i64 integer fns only in this increment
    if (!fn->ret || !fn->ret->is_int()) return;
    for (auto& p : fn->params)
        if (!p.ty || !p.ty->is_int()) return;
    if (c->args.size() != fn->params.size()) return;
    // all args must fold to i64 constants
    std::vector<int64_t> arg_vals;
    arg_vals.reserve(c->args.size());
    for (auto& a : c->args) {
        int64_t v;
        if (!try_eval_const_i64(*a, v)) return; // not foldable — leave as runtime call
        arg_vals.push_back(v);
    }
    // evaluate
    int64_t result;
    std::string eval_err;
    if (!eval_constexpr_fn(*fn, arg_vals, result, eval_err, 0)) return; // eval failed — runtime call
    // replace the CallExpr with an IntLit carrying the folded result
    auto lit = std::make_unique<IntLit>();
    lit->loc = c->loc;
    lit->v = result;
    lit->ty = intern(*fn->ret); // adopt the fn's return type
    slot = std::move(lit);
}

void Checker::lower_constexpr_calls_expr(ExprPtr& slot) {
    if (!slot) return;
    // recurse into children first (bottom-up: inner constexpr calls fold
    // before outer ones, so square(square(7)) folds inner→49 then outer→2401)
    if (auto* b = dynamic_cast<BinExpr*>(slot.get())) {
        lower_constexpr_calls_expr(b->lhs);
        lower_constexpr_calls_expr(b->rhs);
    } else if (auto* u = dynamic_cast<UnaryExpr*>(slot.get())) {
        lower_constexpr_calls_expr(u->operand);
    } else if (auto* c = dynamic_cast<CastExpr*>(slot.get())) {
        lower_constexpr_calls_expr(c->operand);
    } else if (auto* c = dynamic_cast<CallExpr*>(slot.get())) {
        if (c->receiver) lower_constexpr_calls_expr(c->receiver);
        for (auto& a : c->args) lower_constexpr_calls_expr(a);
    } else if (auto* ix = dynamic_cast<IndexExpr*>(slot.get())) {
        lower_constexpr_calls_expr(ix->base);
        lower_constexpr_calls_expr(ix->index);
    } else if (auto* fl = dynamic_cast<FieldExpr*>(slot.get())) {
        lower_constexpr_calls_expr(fl->base);
    } else if (auto* v = dynamic_cast<ViewExpr*>(slot.get())) {
        lower_constexpr_calls_expr(v->base);
    } else if (auto* a = dynamic_cast<AssignExpr*>(slot.get())) {
        lower_constexpr_calls_expr(a->target);
        lower_constexpr_calls_expr(a->value);
    } else if (auto* t = dynamic_cast<TernaryExpr*>(slot.get())) {
        lower_constexpr_calls_expr(t->cond);
        lower_constexpr_calls_expr(t->then_e);
        lower_constexpr_calls_expr(t->else_e);
    } else if (auto* sl = dynamic_cast<StructLit*>(slot.get())) {
        for (auto& kv : sl->fields) lower_constexpr_calls_expr(kv.second);
    } else if (auto* al = dynamic_cast<ArrayLit*>(slot.get())) {
        for (auto& el : al->elements) lower_constexpr_calls_expr(el);
    } else if (auto* fn = dynamic_cast<FnHandleExpr*>(slot.get())) {
        lower_constexpr_calls_expr(fn->operand);
    }
    // AFTER children are folded, check if THIS node is a foldable constexpr call
    try_fold_constexpr_call(slot);
}

void Checker::lower_constexpr_calls_stmt(Stmt& s) {
    if (auto* ls = dynamic_cast<LetStmt*>(&s)) {
        if (ls->init) lower_constexpr_calls_expr(ls->init);
        return;
    }
    if (auto* sa = dynamic_cast<StaticAssertStmt*>(&s)) {
        if (sa->cond) lower_constexpr_calls_expr(sa->cond);
        return;
    }
    if (auto* es = dynamic_cast<ExprStmt*>(&s)) { lower_constexpr_calls_expr(es->expr); return; }
    if (auto* rs = dynamic_cast<ReturnStmt*>(&s)) {
        if (rs->value) lower_constexpr_calls_expr(rs->value);
        return;
    }
    if (auto* is = dynamic_cast<IfStmt*>(&s)) {
        lower_constexpr_calls_expr(is->cond);
        lower_constexpr_calls_block(is->then_b);
        if (is->has_else) lower_constexpr_calls_block(is->else_b);
        return;
    }
    if (auto* ws = dynamic_cast<WhileStmt*>(&s)) {
        lower_constexpr_calls_expr(ws->cond);
        lower_constexpr_calls_block(ws->body);
        return;
    }
    if (dynamic_cast<BreakStmt*>(&s)) return;
    if (dynamic_cast<ContinueStmt*>(&s)) return;
    if (auto* bs = dynamic_cast<BlockStmt*>(&s)) { lower_constexpr_calls_block(bs->block); return; }
    if (auto* ds = dynamic_cast<DeferStmt*>(&s)) { lower_constexpr_calls_expr(ds->expr); return; }
    if (auto* fs = dynamic_cast<ForStmt*>(&s)) {
        if (fs->init) lower_constexpr_calls_stmt(*fs->init);
        if (fs->cond) lower_constexpr_calls_expr(fs->cond);
        if (fs->step) lower_constexpr_calls_expr(fs->step);
        lower_constexpr_calls_block(fs->body);
        return;
    }
    if (auto* ds = dynamic_cast<DoWhileStmt*>(&s)) {
        lower_constexpr_calls_block(ds->body);
        if (ds->cond) lower_constexpr_calls_expr(ds->cond);
        return;
    }
    if (auto* fe = dynamic_cast<ForEachStmt*>(&s)) {
        lower_constexpr_calls_expr(fe->iter);
        lower_constexpr_calls_block(fe->body);
        return;
    }
    if (auto* sw = dynamic_cast<SwitchStmt*>(&s)) {
        lower_constexpr_calls_expr(sw->subject);
        for (auto& c : sw->cases) {
            if (!c.is_default) lower_constexpr_calls_expr(c.value);
            lower_constexpr_calls_block(c.body);
        }
        return;
    }
    if (auto* ms = dynamic_cast<MatchStmt*>(&s)) {
        lower_constexpr_calls_expr(ms->subject);
        for (auto& arm : ms->arms) {
            if (!arm.is_wildcard) lower_constexpr_calls_expr(arm.pattern);
            lower_constexpr_calls_block(arm.body);
        }
        return;
    }
}

void Checker::lower_constexpr_calls_block(Block& b) {
    for (auto& s : b.stmts) lower_constexpr_calls_stmt(*s);
}

// Pass 1.7 (docs/planning/plan_ENUMS.md Section 5): rewrite every EnumAccessExpr to an
// IntLit before check_expr runs, walking the statement tree in parallel
// with check_stmt/check_block's own traversal. After this pass there are no
// EnumAccessExpr nodes anywhere - codegen, the const-folder, the switch
// case-value literal-check, and globals.hpp's initializer evaluator all see
// ordinary IntLits. A mirror of check_stmt/check_block's structure, kept
// separate so check_expr's 600-line body stays byte-for-byte unchanged
// (the plan's headline "codegen untouched" + "no edit to the switch block"
// claims hold because by the time check_stmt runs, the rewrite is done).
void Checker::lower_enum_access_stmt(Stmt& s) {
    if (auto* ls = dynamic_cast<LetStmt*>(&s)) {
        if (ls->init) lower_enum_access_expr(ls->init);
        return;
    }
    if (auto* sa = dynamic_cast<StaticAssertStmt*>(&s)) {
        if (sa->cond) lower_enum_access_expr(sa->cond);
        return;
    }
    if (auto* es = dynamic_cast<ExprStmt*>(&s)) { lower_enum_access_expr(es->expr); return; }
    if (auto* rs = dynamic_cast<ReturnStmt*>(&s)) {
        if (rs->value) lower_enum_access_expr(rs->value);
        return;
    }
    if (auto* is = dynamic_cast<IfStmt*>(&s)) {
        lower_enum_access_expr(is->cond);
        lower_enum_access_block(is->then_b);
        if (is->has_else) lower_enum_access_block(is->else_b);
        return;
    }
    if (auto* ws = dynamic_cast<WhileStmt*>(&s)) {
        lower_enum_access_expr(ws->cond);
        lower_enum_access_block(ws->body);
        return;
    }
    if (dynamic_cast<BreakStmt*>(&s)) return;
    if (dynamic_cast<ContinueStmt*>(&s)) return;
    if (auto* bs = dynamic_cast<BlockStmt*>(&s)) { lower_enum_access_block(bs->block); return; }
    if (auto* ds = dynamic_cast<DeferStmt*>(&s)) { lower_enum_access_expr(ds->expr); return; }
    if (auto* fs = dynamic_cast<ForStmt*>(&s)) {
        if (fs->init) lower_enum_access_stmt(*fs->init);
        if (fs->cond) lower_enum_access_expr(fs->cond);
        if (fs->step) lower_enum_access_expr(fs->step);
        lower_enum_access_block(fs->body);
        return;
    }
    if (auto* ds = dynamic_cast<DoWhileStmt*>(&s)) {
        lower_enum_access_block(ds->body);
        if (ds->cond) lower_enum_access_expr(ds->cond);
        return;
    }
    if (auto* fe = dynamic_cast<ForEachStmt*>(&s)) {
        lower_enum_access_expr(fe->iter);
        lower_enum_access_block(fe->body);
        return;
    }
    if (auto* sw = dynamic_cast<SwitchStmt*>(&s)) {
        lower_enum_access_expr(sw->subject);
        for (auto& c : sw->cases) {
            if (!c.is_default) lower_enum_access_expr(c.value);
            lower_enum_access_block(c.body);
        }
        return;
    }
    if (auto* ms = dynamic_cast<MatchStmt*>(&s)) {
        lower_enum_access_expr(ms->subject);
        for (auto& arm : ms->arms) {
            if (!arm.is_wildcard) lower_enum_access_expr(arm.pattern);
            lower_enum_access_block(arm.body);
        }
        return;
    }
}

void Checker::lower_enum_access_block(Block& b) {
    for (auto& s : b.stmts) lower_enum_access_stmt(*s);
}

} // namespace

SemaResult sema(Program& prog,
                const std::unordered_map<std::string, NativeSig>& natives,
                const std::unordered_map<std::string, int>& script_slots,
                uint32_t module_permissions,
                const OpOverloadTable* overloads,
                const StructLayoutTable* structs,
                const ModuleExportTable* module_exports) {
    Checker c;
    c.natives = &natives;
    c.script_slots = &script_slots;
    c.perms = module_permissions;
    c.overloads = overloads;
    c.structs = structs;
    c.module_exports = module_exports;
    c.prog = &prog;
    // Tier 1 namespaces: build the namespace-name set for Foo::bar resolution.
    for (const auto& ns : prog.namespaces) c.namespace_names.insert(ns.name);

    // Tier 1 typed enums (docs/planning/plan_ENUMS.md §6): register typed
    // enum names + their backing Prim BEFORE resolve_type runs, so a typed
    // enum name used in a type position (`let c: Color`, `fn f(p: Color)`, a
    // global/field/return) resolves to the typed-enum type (prim=backing,
    // struct_name="", enum_name=name) instead of the opaque-handle I64 the
    // untyped path below would produce (a typed enum name is not in the
    // StructLayoutTable). Validates each typed enum's backing type is an
    // integer; bad backing types are recorded as sema errors here.
    c.register_typed_enums();

    // Resolve named types: the parser sets prim=Void for all named types.
    // A named type that IS in the StructLayoutTable is a by-value struct
    // (prim stays Void). A named type that is NOT in the table is an opaque
    // handle (prim → I64). A named type that is a TYPED enum resolves to its
    // backing integer prim (struct_name cleared, enum_name set). This must
    // happen before any type checking.
    if (structs) {
        auto resolve_type = [&](Type& t) {
            if (t.prim == Prim::Void && !t.struct_name.empty()) {
                auto teit = c.typed_enum_backing.find(t.struct_name);
                if (teit != c.typed_enum_backing.end()) {
                    t.prim = teit->second;   // backing integer prim
                    t.enum_name = t.struct_name;
                    t.struct_name.clear();
                    return;
                }
                if (structs->find(t.struct_name) == structs->end())
                    t.prim = Prim::I64;
            }
        };
        // Resolve types in struct fields, function signatures, local let types, globals.
        // The parser sets prim=Void for all named types; sema resolves to
        // I64 for opaque handles (not in the StructLayoutTable).
        for (auto& sd : prog.structs)
            for (auto& fd : sd.fields) if (fd.ty) resolve_type(*fd.ty);
        for (auto& fn : prog.funcs) {
            if (fn.ret) resolve_type(*fn.ret);
            for (auto& p : fn.params) if (p.ty) resolve_type(*p.ty);
            // Walk the function body for LetStmt types.
            std::function<void(Block&)> resolve_block = [&](Block& b) {
                for (auto& s : b.stmts) {
                    if (auto* ls = dynamic_cast<LetStmt*>(s.get())) {
                        if (ls->ty) resolve_type(*ls->ty);
                    }
                    if (auto* is = dynamic_cast<IfStmt*>(s.get())) {
                        resolve_block(is->then_b);
                        if (is->has_else) resolve_block(is->else_b);
                    }
                    if (auto* ws = dynamic_cast<WhileStmt*>(s.get())) resolve_block(ws->body);
                    if (auto* fs = dynamic_cast<ForStmt*>(s.get())) resolve_block(fs->body);
                    if (auto* ds = dynamic_cast<DoWhileStmt*>(s.get())) resolve_block(ds->body);
                    if (auto* fe = dynamic_cast<ForEachStmt*>(s.get())) resolve_block(fe->body);
                    if (auto* sw = dynamic_cast<SwitchStmt*>(s.get()))
                        for (auto& c : sw->cases) resolve_block(c.body);
                    if (auto* ms = dynamic_cast<MatchStmt*>(s.get()))
                        for (auto& arm : ms->arms) resolve_block(arm.body);
                    if (auto* bs = dynamic_cast<BlockStmt*>(s.get())) resolve_block(bs->block);
                }
            };
            resolve_block(fn.body);
        }
        for (auto& g : prog.globals) if (g.ty) resolve_type(*g.ty);
    }

    // register globals (stable type pointers via intern)
    for (auto& g : prog.globals) {
        if (!c.globals.count(g.name)) c.globals[g.name] = {c.intern(*g.ty), g.is_const};
        // Tier 1 namespaces: also register the qualified name (Ns::global).
        if (!g.ns.empty()) c.globals[g.ns + "::" + g.name] = {c.intern(*g.ty), g.is_const};
    }
    // Tier 1 enums (docs/planning/plan_ENUMS.md Section 4): pass 1.5 resolve variant values
    // + build the (enum,variant)->i32 table and the enum_names set; pass 1.6
    // reject enum names used in type positions (the untyped v1 hook); pass 1.7
    // rewrite every EnumAccessExpr to an IntLit BEFORE any function body is
    // checked, so check_expr / the const-folder / the switch case-value
    // literal-check / globals.hpp's initializer evaluator / codegen never see
    // an EnumAccessExpr at all (Section 5: codegen stays untouched).
    c.resolve_enums();
    c.check_declared_types_not_enum();
    c.validate_declarations();
    for (auto& g : prog.globals) {
        if (g.init) c.lower_enum_access_expr(g.init);
    }
    for (auto& sa : prog.static_asserts) {
        if (sa.cond) c.lower_enum_access_expr(sa.cond);
    }
    for (auto& f : prog.funcs) {
        c.lower_enum_access_block(f.body);
    }
    // constexpr call folding pre-pass (Tier 1): after enum lowering (so enum
    // variants are already IntLits) and BEFORE any check_expr runs (so global
    // initializers, case labels, and codegen all see the folded literal).
    // Walks every ExprPtr slot bottom-up; a CallExpr to a constexpr fn with
    // all-constant i64 args is evaluated and replaced with an IntLit. If the
    // eval fails (non-const arg, too-deep recursion, unsupported node), the
    // CallExpr is left as-is — the fn is still callable at runtime.
    for (auto& g : prog.globals) {
        if (g.init) c.lower_constexpr_calls_expr(g.init);
    }
    for (auto& sa : prog.static_asserts) {
        if (sa.cond) c.lower_constexpr_calls_expr(sa.cond);
    }
    for (auto& f : prog.funcs) {
        c.lower_constexpr_calls_block(f.body);
    }
    // register script function signatures (pass 2 of docs/spec/COMPILER_PIPELINE.md Section 4:
    // all signatures resolved before any body is checked, so forward calls work)
    for (auto& f : prog.funcs) {
        Checker::ScriptSig ss;
        ss.ret = c.intern(*f.ret);
        for (auto& p : f.params) {
            const Type* pty = c.intern(*p.ty);
            ss.params.push_back(pty);
            ss.defaults.push_back(&p.default_val);
            if (p.default_val.kind == DefaultValue::Kind::None) {
                ss.required_count++;
            } else {
                // coarse kind-vs-declared-type check, up front at the
                // declaration - the synthesized literal still goes through
                // full check_expr()/adaptation at each call site regardless,
                // this just catches an obviously wrong default immediately
                // rather than only ever erroring lazily at the first
                // missing-arg call site.
                bool ok = (p.default_val.kind==DefaultValue::Kind::Int    && pty->is_int()) ||
                          (p.default_val.kind==DefaultValue::Kind::Float  && pty->is_float()) ||
                          (p.default_val.kind==DefaultValue::Kind::Bool   && pty->is_bool()) ||
                          (p.default_val.kind==DefaultValue::Kind::String &&
                           (pty->is_slice || (pty->prim==Prim::I64 && pty->struct_name=="string")));
                if (!ok) c.err("default value for parameter '" + p.name + "' of '" + f.name +
                                "' does not match its declared type (" + pty->to_string() + ")",
                                p.loc.line, p.loc.col);
            }
        }
        // Tier 1 namespaces: namespaced fns are registered under the
        // qualified name only (Ns::fn), not the bare name. They are callable
        // via Foo::bar() from anywhere (inside or outside the namespace).
        if (!f.ns.empty()) {
            c.script_sigs[f.ns + "::" + f.name] = ss;
        } else {
            c.script_sigs[f.name] = ss;
        }
    }

    // Slice-escape safety Stage 2 (C5): compute per-fn borrowed_params /
    // retained_params BEFORE any function body is checked. A forward call
    // from G to F (G checked first in the check_func loop below) must already
    // see F's sets, so this runs here, after script_sigs are registered and
    // before the check_func loop. The fixed-point converges transitivity
    // (return relay(s) where relay borrows its arg; g = relay(s) where relay
    // retains). See compute_borrow_retain() + is_local_array_view()'s CallExpr
    // case for how the sets are consumed.
    // #20 lambda param-type upgrade pre-pass: walk all fn bodies for CallExpr
    // args that are LambdaExprs. If the callee is a script fn whose param at
    // that position is a `fn(Args)->Ret` fn-handle type with a matching
    // recorded sig, upgrade that param type to is_lambda (16 bytes) so the
    // 16-byte lambda value {slot, env_ptr} fits + the body's call through that
    // param dispatches as a lambda call. Runs BEFORE body checking so the
    // upgraded param type is visible when the callee's body is checked. (v1
    // limitation: if a param receives a lambda in one call + a bare &fn handle
    // in another, the upgrade makes the &fn call a type error — acceptable for
    // v1; mixing the two callable shapes at one param is a follow-up.)
    c.upgrade_lambda_param_types();

    c.compute_borrow_retain();

    // Global initializers obey the same nominal assignment barrier as locals.
    // Check them after signatures are registered so any initializer call has
    // the normal native/script result type available. An aggregate (struct /
    // fixed-array / slice) global's initializer is a StructLit/ArrayLit; the
    // ArrayLit path uses the aggregate_cast_init == &e discriminator to allow
    // a fixed-array ArrayLit ONLY as a direct init, so set it to the init expr
    // for the duration of this check (mirrors LetStmt's own setting, c2/c3).
    for (auto& g : prog.globals) {
        if (!g.init) continue;
        const Type* want = c.globals[g.name].ty;
        const Expr* saved_aggregate_cast_init = c.aggregate_cast_init;
        c.aggregate_cast_init = g.init.get();
        const Type* got = c.check_value(g.init, want);
        c.aggregate_cast_init = saved_aggregate_cast_init;
        if (!Checker::types_compatible(want, got))
            c.err("global initializer type mismatch (" + want->to_string() + " = " +
                  got->to_string() + ")", g.loc.line, g.loc.col);
    }

    // check each function. #20: synthetic lambda fns (__lambda_N) are
    // checked AFTER all non-lambda fns — their captures + env layout are
    // determined by check_lambda, which runs during the ENCLOSING fn's check
    // (the fn whose body holds the LambdaExpr). Checking lambda fns after all
    // their enclosing fns ensures lf->lambda_captures is populated before
    // check_lambda_func binds them.
    try {
    for (auto& f : prog.funcs) {
        if (!f.is_lambda) c.check_func(f);
    }
    // Nested lambdas: a lambda fn's captures are set by check_lambda, which
    // runs during the body-check of its ENCLOSING fn. For a top-level lambda
    // the enclosing fn is a non-lambda (checked above). For a NESTED lambda
    // the enclosing fn is itself a lambda fn — so the enclosing lambda must
    // be checked first (its check_lambda_func walks its body, which calls
    // check_lambda on the nested lambda, populating the nested lambda's
    // captures + scope snapshot). Process lambda fns in dependency order with
    // a fixpoint: a lambda is ready to check once check_lambda has prepended
    // its hidden __env param (params[0].name == "__env"); skip any not yet
    // ready and repeat until no progress. Any lambda still unready at the end
    // (e.g. an unreferenced lambda literal whose enclosing fn was never
    // checked) is checked anyway to surface body errors.
    std::vector<bool> done(prog.funcs.size(), false);
    bool progress = true;
    while (progress) {
        progress = false;
        for (size_t i = 0; i < prog.funcs.size(); ++i) {
            if (done[i] || !prog.funcs[i].is_lambda) continue;
            const bool ready = !prog.funcs[i].params.empty() &&
                               prog.funcs[i].params[0].name == "__env";
            if (!ready) continue;
            c.check_func(prog.funcs[i]);
            done[i] = true;
            progress = true;
        }
    }
    for (size_t i = 0; i < prog.funcs.size(); ++i) {
        if (prog.funcs[i].is_lambda && !done[i]) c.check_func(prog.funcs[i]);
    }
    } catch (const SemaError& se) {
        c.err(se.msg, se.line, se.col);
    }

    // Tier 1 static_assert: check top-level assertions (in-body ones are
    // checked inside check_stmt). Runs after function bodies so all script
    // signatures + globals are registered (a top-level static_assert cond may
    // reference a global or call a fn — the constexpr-call pre-pass already
    // folded constexpr calls to IntLits, but a non-constexpr fn call or a
    // global reference still needs check_expr's name resolution to succeed).
    for (auto& sa : prog.static_asserts) {
        c.check_static_assert(sa);
    }

    SemaResult r;
    r.errors = std::move(c.errs);
    r.warnings = std::move(c.warns);
    r.ok = r.errors.empty();
    return r;
}

} // namespace ember
