#include "sema.hpp"
#include "binding_builder.hpp"  // PERM_FFI constant (v0.4 sema gating)
#include <cassert>
#include <climits>
#include <cstring>
#include <unordered_set>
#include <string>

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

bool try_eval_const_bool(const Expr& e, bool& out) {
    if (auto* lit = dynamic_cast<const BoolLit*>(&e)) { out = lit->v; return true; }
    if (auto* u = dynamic_cast<const UnaryExpr*>(&e)) {
        if (u->op != UnaryExpr::Op::Not) return false;
        bool v;
        if (!try_eval_const_bool(*u->operand, v)) return false;
        out = !v; return true;
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
    struct Var { std::string name; const Type* ty; bool is_const; bool local_array_view; };
    std::vector<std::vector<Var>> scopes;

    struct GlobalVar { const Type* ty; bool is_const; };
    std::unordered_map<std::string, GlobalVar> globals;
    int loop_depth = 0;
    int switch_depth = 0;
    const Expr* aggregate_cast_init = nullptr;

    // Tier 1 enums (docs/planning/plan_ENUMS.md Section 4): (enum_name -> (variant -> i32 value)),
    // built by the enum-resolution pass (pass 1.5, before any function body is
    // checked). `enum_names` is the set of registered enum names, used by the
    // type-position enum-name rejection (Section 4.5: `let x: Color = ...` is a
    // clean error in v1, the hook the typed-enum upgrade later flips).
    std::unordered_map<std::string, std::unordered_map<std::string, int64_t>> enum_values;
    std::unordered_set<std::string> enum_names;

    bool is_enum_name(const std::string& n) const { return enum_names.count(n) != 0; }

    // script function signatures: name -> (ret type, param types)
    struct ScriptSig {
        const Type* ret;
        std::vector<const Type*> params;
        std::vector<const DefaultValue*> defaults; // parallel to params; Kind::None = required
        size_t required_count = 0;                 // count of leading non-defaulted params
    };
    std::unordered_map<std::string, ScriptSig> script_sigs;

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
        return t && t->is_int() && !t->is_fn_handle && t->struct_name.empty();
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
        return is_plain_integer(want) && is_plain_integer(got) &&
               want->is_uint() == got->is_uint() && int_width(got) < int_width(want);
    }
    static bool types_compatible(const Type* want, const Type* got) {
        return want && got && want->same(*got);
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
        if (it != structs->end() && it->second.size > 8)
            err("native by-value argument '" + want->struct_name + "' is " +
                std::to_string(it->second.size) + " bytes; Ember v1 supports native "
                "aggregate arguments only through 8 bytes", loc.line, loc.col);
    }

    // --- Tier 1 enums (docs/planning/plan_ENUMS.md) ---
    //
    // Pass 1.5 (Section 4.1): resolve every enum variant's i32 value. First
    // variant defaults to 0; each variant without an explicit `= value` is
    // prev+1; an explicit `= constexpr_int` sets the next base. Explicit
    // values are restricted to what try_eval_const_i64 already folds
    // (IntLit / -IntLit / BinExpr-of-literals) - no new evaluator. Values are
    // i32-range-checked (reusing the bound from adapt_int_lit) and duplicate
    // values / duplicate variant names within one enum are errors (stricter
    // than C - the one footgun C allows that v1 rejects). Builds the
    // (enum_name -> (variant -> i32)) table for EnumAccessExpr resolution and
    // the enum_names set for the type-position rejection (Section 4.5).
    void resolve_enums() {
        std::unordered_set<std::string> seen_enum_names;
        for (auto& e : prog->enums) {
            if (!seen_enum_names.insert(e.name).second)
                err("duplicate enum declaration '" + e.name + "'", e.loc.line, e.loc.col);
            enum_names.insert(e.name);
            int64_t next = 0;
            std::unordered_set<int32_t> seen_values;
            std::unordered_set<std::string> seen_variant_names;
            for (auto& v : e.variants) {
                if (!seen_variant_names.insert(v.name).second)
                    err("enum '" + e.name + "' has duplicate variant '" + v.name + "'",
                        v.loc.line, v.loc.col);
                if (v.explicit_value) {
                    int64_t ev = 0;
                    if (!try_eval_const_i64(*v.explicit_value, ev)) {
                        err("enum variant '" + e.name + "::" + v.name +
                            "' explicit value must be a compile-time integer constant",
                            v.loc.line, v.loc.col);
                    } else if (ev < -2147483648LL || ev > 2147483647LL) {
                        err("enum variant '" + e.name + "::" + v.name +
                            "' value " + std::to_string(ev) + " out of i32 range",
                            v.loc.line, v.loc.col);
                    } else {
                        next = ev;
                    }
                }
                v.resolved = next;
                if (!seen_values.insert(int32_t(next)).second)
                    err("enum '" + e.name + "' has duplicate value " + std::to_string(next) +
                        " (variant '" + v.name + "')", v.loc.line, v.loc.col);
                enum_values[e.name][v.name] = next;
                ++next;
            }
        }
    }

    // Pass 1.6 (Section 4.5 follow-through): reject an enum name used in a
    // type position (`let x: Color = ...`, `fn f(p: Color)`, a struct field,
    // a global, a return type). In v1 an enum is untyped (its values ARE
    // i32); the hook flips from reject to accept when typed enums land (Tier 2).
    void check_type_not_enum(const Type* t, const Loc& loc) {
        if (t && !t->struct_name.empty() && is_enum_name(t->struct_name))
            err("enum '" + t->struct_name + "' is not a type in v1; declare the binding as `i32`",
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
            if (!fn_names.insert(f.name).second)
                err("duplicate function declaration '" + f.name + "'", f.loc.line, f.loc.col);
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
            if (!global_names.insert(g.name).second)
                err("duplicate global declaration '" + g.name + "'", g.loc.line, g.loc.col);
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
            lit->v = vit->second;       // the resolved i32 value, sign-extended into i64
            // ty is finalized by check_expr's IntLit case (adapt_int_lit to the
            // expected context, defaulting to i32 here per Section 1.4).
            lit->ty = intern(make_prim(Prim::I32));
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
                 bool local_array_view = false, Loc loc = {0, 0}) {
        if (scopes.empty()) return;
        for (const auto& v : scopes.back()) {
            if (v.name == n) {
                err("redeclaration of '" + n + "' in the same scope", loc.line, loc.col);
                return;
            }
        }
        scopes.back().push_back({n, t, is_const, local_array_view});
    }

    // NOTE (slice-escape safety, stage 1 vs stage 2): this function tracks
    // stack-backed slices (ViewExpr over a fixed array, a StringLit that
    // resolved to slice<u8>, and aliases through Ident/Ternary). The escape
    // GUARDS that consume it are: return (ReturnStmt), global-Ident-store
    // (AssignExpr Ident target), and global-rooted FieldExpr/IndexExpr-store
    // (AssignExpr else-if branch) — these close C1/C2a/C2b for both the
    // local_array_view class and the StringLit class.
    //
    // STAGE 2 (deferred): C3 (a stack-backed slice passed to a NATIVE that may
    // retain the ptr) and C5 (a stack-backed slice passed to a script fn /
    // fn-handle / cross-module call that may retain it) are NOT guarded at the
    // call-arg sites. A blanket reject there was rejected because it breaks
    // the legitimate synchronous pattern `return_slice_defer(return_values[..])`
    // (a fn that takes a slice and returns it for the caller to read within the
    // caller's own frame — see tests/lang/runtime_language_features.ember).
    // Closing C3/C5 needs a real borrow/escape analysis: propagate the
    // localview bit through a call's RETURN value (so the caller's binding of
    // the result is itself a stack-backed slice), then reject only at the
    // ACTUAL escape point (return/store of that propagated result), and add a
    // `borrows`/`retains` annotation to NativeSig so C3 can distinguish
    // copying natives (string_from_slice) from retaining ones. See
    // demo/SLICE_ESCAPE_SAFETY_INVESTIGATION.md §6.3/§8 and the stage-2 roadmap
    // entry. Until then C3/C5 are open at the call site (no shipped native
    // retains, and a retaining script fn is the residual live hole).
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
        return false;
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
        // only adopt if the literal currently is default i64 and fits target
        int64_t v = lit.v;
        switch (target->prim) {
        case Prim::U8:  if (v >= 0 && v <= 0xFF)        { lit.ty = target; return; } break;
        case Prim::U16: if (v >= 0 && v <= 0xFFFF)     { lit.ty = target; return; } break;
        case Prim::U32: if (v >= 0 && v <= 0xFFFFFFFFLL){ lit.ty = target; return; } break;
        case Prim::U64: if (v >= 0)                    { lit.ty = target; return; } break;
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
};

const Type* Checker::check_expr(Expr& e, const Type* expected, bool allow_struct_ret_call) {
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
    if (auto* h = dynamic_cast<FnHandleExpr*>(&e)) {
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
                    ++i;
                }
                for (auto& a : c->args) {
                    const Type* want = tt->recorded_params[i++].get();
                    const Type* got = check_value(a, want);
                    if (!types_compatible(want, got))
                        err("function handle argument type mismatch (expected " + want->to_string() +
                            ", got " + got->to_string() + ")", a->loc.line, a->loc.col);
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
                                for (auto& a : c->args) check_expr(*a);
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
                            }
                            e.ty = intern(exp.ret);
                            return e.ty;
                        }
                    }
                }
            }
            // unresolved: deferred trap. Default ret type to i64 so the call
            // is usable in i64 contexts (the most common cross-module return).
            e.ty = intern(type_i64());
            return e.ty;
        }
        // resolve: native first, then script function
        auto nit = natives->find(c->name);
        auto sit = script_slots->find(c->name);
        if (nit == natives->end() && sit == script_slots->end()) {
            // v1.0 Tier 2 (docs/planning/plan_FUNCTION_REFS.md §4.3): `name(args)` where `name`
            // is neither a native nor a script fn — but it might be a LOCAL
            // VARIABLE of fn-handle type (the `let h = &fn; h(args);` case, which
            // parses identically to a named call). Promote it to an indirect call:
            // move an Ident referring to the var into indirect_target + recurse.
            // This closes the parse-time ambiguity (the parser can't know if `h`
            // is a fn name or a fn-typed var; sema resolves it here).
            const Var* v = lookup_local_var(c->name);
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
                        ++i;
                    }
                    for (auto& a : c->args) {
                        const Type* want = tt->recorded_params[i++].get();
                        const Type* got = check_value(a, want);
                        if (!types_compatible(want, got))
                            err("function handle argument type mismatch (expected " + want->to_string() +
                                ", got " + got->to_string() + ")", a->loc.line, a->loc.col);
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
            if (!types_compatible(lt, rt))
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
            e.ty = lt; return e.ty;
        }
        if (!lt->same(*rt)) {
            err("operator requires same-type operands (got " + lt->to_string() + " and " + rt->to_string() + ")",
                b->loc.line, b->loc.col);
            e.ty = lt; return e.ty;
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
        const bool float_to_float = from && to && from->is_float() && to->is_float();
        // The x64 backend currently implements signed integer/float conversion.
        // Unsigned conversion (especially u64 above INT64_MAX) needs a distinct
        // lowering, so it is deliberately outside the v1 cast matrix.
        const bool int_float = (is_plain_integer(from) && !from->is_uint() && to && to->is_float()) ||
                               (from && from->is_float() && is_plain_integer(to) && !to->is_uint());
        if (!same && !int_to_int && !float_to_float && !int_float) {
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
        // matches the parser's own convention for struct_name-bearing types
        // (handle types like vec3 are also prim=I64 - see parse_type()'s
        // Ident case) so Type::same() compares equal against a `let p: Point`
        // declared-type slot.
        Type st; st.prim = Prim::I64; st.struct_name = sl->type_name;
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

void Checker::check_stmt(Stmt& s, const Type* ret_ty, bool& returns) {
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
        declare(ls->name, decl_ty, ls->is_const, is_local_array_view(*ls->init), ls->loc);
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
}

void Checker::check_block(Block& b, const Type* ret_ty, bool& returns) {
    returns = false;
    push_scope();
    for (auto& s : b.stmts) {
        bool r = false;
        check_stmt(*s, ret_ty, r);
        if (r) returns = true;
    }
    pop_scope();
}

void Checker::check_func(FuncDecl& f) {
    scopes.clear();
    push_scope();
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
    if (!ret_ty->is_void() && !returns) {
        // find end of function for the error loc
        err("function '" + f.name + "' not all paths return a value", f.loc.line, f.loc.col);
    }
    pop_scope();
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
    if (auto* sw = dynamic_cast<SwitchStmt*>(&s)) {
        lower_enum_access_expr(sw->subject);
        for (auto& c : sw->cases) {
            if (!c.is_default) lower_enum_access_expr(c.value);
            lower_enum_access_block(c.body);
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

    // register globals (stable type pointers via intern)
    for (auto& g : prog.globals) {
        if (!c.globals.count(g.name)) c.globals[g.name] = {c.intern(*g.ty), g.is_const};
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
    for (auto& f : prog.funcs) {
        c.lower_enum_access_block(f.body);
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
        c.script_sigs[f.name] = ss;
    }

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

    // check each function
    for (auto& f : prog.funcs) {
        c.check_func(f);
    }

    SemaResult r;
    r.errors = std::move(c.errs);
    r.warnings = std::move(c.warns);
    r.ok = r.errors.empty();
    return r;
}

} // namespace ember
