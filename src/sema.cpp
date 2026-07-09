#include "sema.hpp"
#include "binding_builder.hpp"  // PERM_FFI constant (v0.4 sema gating)
#include <cassert>
#include <climits>

namespace ember {

// Per-frame byte budget (red-team V6-DoS mitigation): a single local or the
// running frame total must not exceed this, or a script can declare a huge
// fixed array and exhaust the host stack (the confirmed u8[65536] SIGSEGV).
// Host-configurable later (DESIGN.md v0.4 budgets); a fixed default now.
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
        if (int64_t(t.array_len) > MAX_ARRAY_LEN) return INT64_MAX; // overflow -> reject
        const Type* e = t.elem.get();
        int64_t eb = e ? int64_t(e->byte_size()) : 1;
        if (eb < 1) eb = 8;
        return int64_t(t.array_len) * eb;
    }
    if (!t.struct_name.empty() && structs) {
        auto it = structs->find(t.struct_name);
        if (it != structs->end()) return int64_t(it->second.size);
    }
    return 8;
}

StructLayoutTable build_struct_layouts(const Program& prog) {
    StructLayoutTable out;
    for (auto& sd : prog.structs) {
        StructLayout layout;
        int64_t off = 0;
        for (auto& fd : sd.fields) {
            int64_t sz = int64_t(fd.ty->byte_size());
            if (sz < 1) sz = 8; // defensive fallback (v1 fields are primitives, always sized)
            // V6-overflow: a field whose byte_size overflows int32 (e.g.
            // i64[1073741824] = 8.6GB) must NOT silently become a 0-byte frame
            // slot (the confirmed latent arbitrary-write path). Force the field
            // to a size that exceeds the per-frame budget so any `let` of this
            // struct is rejected by the LetStmt budget check. The struct is
            // also unreachable for legitimate use at this size anyway.
            if (sz > INT32_MAX) {
                sz = MAX_FRAME_BYTES + 1; // guarantees rejection at any `let`
            }
            layout.fields[fd.name] = StructFieldLayout{fd.ty.get(), int32_t(off)};
            layout.field_names.push_back(fd.name);
            off += sz;
        }
        // cap the struct's total size at int32 (a struct bigger than the frame
        // budget is rejected by the per-local check at the LetStmt).
        layout.size = off > INT32_MAX ? INT32_MAX : int32_t(off);
        out[sd.name] = std::move(layout);
    }
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
// same-type rule (TYPE_SYSTEM.md Section 6: literals are context-influenced).
// Variables never implicitly promote (Section 7).

// Collect every Ident referenced inside an expression (for the defer
// scope-safety check below). Deliberately conservative about what it
// recurses into - StructLit field values and SizeofExpr's type aren't
// expression-shaped in the same way and don't need it for this check.
void collect_idents(const Expr& e, std::vector<const Ident*>& out) {
    if (auto* id = dynamic_cast<const Ident*>(&e)) { out.push_back(id); return; }
    if (auto* b = dynamic_cast<const BinExpr*>(&e)) { collect_idents(*b->lhs, out); collect_idents(*b->rhs, out); return; }
    if (auto* u = dynamic_cast<const UnaryExpr*>(&e)) { collect_idents(*u->operand, out); return; }
    if (auto* c = dynamic_cast<const CastExpr*>(&e)) { collect_idents(*c->operand, out); return; }
    if (auto* t = dynamic_cast<const TernaryExpr*>(&e)) {
        collect_idents(*t->cond, out); collect_idents(*t->then_e, out); collect_idents(*t->else_e, out); return;
    }
    if (auto* c = dynamic_cast<const CallExpr*>(&e)) {
        if (c->receiver) collect_idents(*c->receiver, out);
        for (auto& a : c->args) collect_idents(*a, out);
        return;
    }
    if (auto* a = dynamic_cast<const AssignExpr*>(&e)) { collect_idents(*a->target, out); collect_idents(*a->value, out); return; }
    if (auto* ix = dynamic_cast<const IndexExpr*>(&e)) { collect_idents(*ix->base, out); collect_idents(*ix->index, out); return; }
    if (auto* fl = dynamic_cast<const FieldExpr*>(&e)) { collect_idents(*fl->base, out); return; }
    if (auto* v = dynamic_cast<const ViewExpr*>(&e)) { collect_idents(*v->base, out); return; }
}

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
        case UnaryExpr::Op::Neg:    out = int64_t(uint64_t(0) - uint64_t(v)); return true;
        case UnaryExpr::Op::BitNot: out = ~v; return true;
        default: return false; // Not (!) is bool-only, never reaches an int context
        }
    }
    if (auto* b = dynamic_cast<const BinExpr*>(&e)) {
        int64_t l, r;
        if (!try_eval_const_i64(*b->lhs, l)) return false;
        if (!try_eval_const_i64(*b->rhs, r)) return false;
        switch (b->op) {
        case BinExpr::Op::Add: out = int64_t(uint64_t(l) + uint64_t(r)); return true;
        case BinExpr::Op::Sub: out = int64_t(uint64_t(l) - uint64_t(r)); return true;
        case BinExpr::Op::Mul: out = int64_t(uint64_t(l) * uint64_t(r)); return true;
        case BinExpr::Op::And: out = l & r; return true;
        case BinExpr::Op::Or:  out = l | r; return true;
        case BinExpr::Op::Xor: out = l ^ r; return true;
        case BinExpr::Op::Shl: out = l << (r & 63); return true;
        case BinExpr::Op::Shr: out = l >> (r & 63); return true;
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
    Program* prog = nullptr;
    const OpOverloadTable* overloads = nullptr;
    const StructLayoutTable* structs = nullptr;
    const ModuleExportTable* module_exports = nullptr;  // v0.5 cross-module exports (mod::fn resolution)

    // scope stack for locals/params
    struct Var { std::string name; const Type* ty; bool is_const; };
    std::vector<std::vector<Var>> scopes;

    // global table: name -> type (pointer into a stable type store)
    std::unordered_map<std::string, const Type*> globals;

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

    // Returns the scope-stack record itself (type + const-ness), or nullptr
    // if `n` isn't a local/param (does NOT search globals - globals have no
    // const tracking today; GlobalDecl carries no is_const field).
    const Var* lookup_local_var(const std::string& n) {
        for (int i = int(scopes.size())-1; i >= 0; --i)
            for (auto& v : scopes[i]) if (v.name == n) return &v;
        return nullptr;
    }

    const Type* lookup_var(const std::string& n) {
        if (const Var* v = lookup_local_var(n)) return v->ty;
        auto it = globals.find(n);
        if (it != globals.end()) return it->second;
        return nullptr;
    }

    // true iff `n` is a function parameter or a global - the only bindings
    // guaranteed to still be valid wherever `emit_defers()` ends up running
    // (see the DeferStmt check below). scopes[0] is always exactly the
    // parameter scope (check_func pushes it before check_block pushes the
    // function body's own scope), so this is a cheap, precise check.
    bool is_param_or_global(const std::string& n) const {
        if (!scopes.empty()) for (auto& v : scopes[0]) if (v.name == n) return true;
        return globals.count(n) != 0;
    }

    // true iff `t` names a script-declared `struct Name {...}` (as opposed to
    // a built-in handle type like vec3/string, which is also prim=I64 with a
    // struct_name but isn't in `structs` - only script structs go through the
    // by-value calling convention below).
    bool is_registered_struct(const Type* t) const {
        return structs && t && !t->struct_name.empty() && structs->count(t->struct_name) != 0;
    }

    // A struct-by-value call argument must be a bare local variable: codegen
    // (CG::eval's CallExpr struct-arg stash) copies the argument's bytes
    // directly from a known frame address, never evaluating a general
    // expression into a multi-word aggregate (there's no such single-value
    // form for a struct). Reject anything else here instead of letting
    // codegen either crash or silently copy garbage.
    void check_struct_arg_shape(const Expr& arg, const Type* want) {
        if (!is_registered_struct(want)) return;
        if (dynamic_cast<const Ident*>(&arg)) return;
        err("a struct-by-value argument (type '" + want->struct_name + "') must be a plain "
            "local variable, not a general expression", arg.loc.line, arg.loc.col);
    }

    void push_scope() { scopes.emplace_back(); }
    void pop_scope()  { scopes.pop_back(); }
    void declare(const std::string& n, const Type* t, bool is_const) {
        if (!scopes.empty()) scopes.back().push_back({n, t, is_const});
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
        // string literal -> slice<u8> (rodata, MEMORY_AND_GC.md Section 6). Bake a
        // stable-address copy into the Program's rodata store so codegen can
        // emit an absolute pointer immediate (same lifetime contract as the
        // type_store above - must outlive codegen/JIT execution).
        // String encryption (default): XOR the bytes with a
        // per-compile key before storing. The raw string never appears in
        // the JIT'd exec memory - only the encrypted bytes are in rodata,
        // and codegen emits a __str_decrypt call at each use site.
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
    if (auto* c = dynamic_cast<CallExpr*>(&e)) {
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
            e.ty = intern(cit->second.ret);
            return e.ty;
        }
        // v0.5 cross-module call `mod::fn(args)` (MODULES.md §6). Resolve against
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
                                const Type* got = check_expr(*c->args[i], &exp.params[i]);
                                if (!got->same(exp.params[i]) && !(got->is_int() && exp.params[i].is_int()))
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
            err("unknown function '" + c->name + "'", c->loc.line, c->loc.col);
            e.ty = intern(type_void()); return e.ty;
        }
        const Type* ret_ty;
        const std::vector<Type>* params;
        if (nit != natives->end()) {
            // permission gate (SAFETY_AND_SANDBOX.md Section 6): a native
            // flagged PERM_FFI is only callable from a module compiled with the
            // FFI permission bit. Compile-time check (zero runtime cost).
            if ((nit->second.permission & PERM_FFI) && !(perms & PERM_FFI)) {
                err("function '" + c->name + "' requires PERM_FFI permission",
                    c->loc.line, c->loc.col);
            }
            c->is_native = true; c->native_fn = nit->second.fn_ptr;
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
                    const Type* got = check_expr(*c->receiver, want);
                    if (!got->same(*want) && !(got->is_int() && want->is_int()) && !(got->prim==Prim::I64 && want->prim==Prim::I64))
                        err("receiver of '" + c->name + "': expected " +
                            want->to_string() + ", got " + got->to_string(),
                            c->receiver->loc.line, c->receiver->loc.col);
                    check_struct_arg_shape(*c->receiver, want);
                    off = 1;
                }
                for (size_t i = 0; i < c->args.size(); ++i) {
                    const Type* want = &(*params)[off + i];
                    const Type* got = check_expr(*c->args[i], want);
                    if (!got->same(*want) && !(got->is_int() && want->is_int()) && !(got->prim==Prim::I64 && want->prim==Prim::I64))
                        err("arg " + std::to_string(i+1) + " of '" + c->name + "': expected " +
                            want->to_string() + ", got " + got->to_string(),
                            c->args[i]->loc.line, c->args[i]->loc.col);
                    check_struct_arg_shape(*c->args[i], want);
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
                        const Type* got = check_expr(*c->args[i], want);
                        if (!got->same(*want) && !(got->is_int() && want->is_int()) && !(got->prim==Prim::I64 && want->prim==Prim::I64))
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
        // A struct-returning call is only allowed as a `let x: T = f(...);`
        // initializer or a `return f(...);` value (see check_expr's
        // allow_struct_ret_call doc comment) - codegen needs a concrete
        // destination address (the hidden return pointer) for those two
        // shapes, not a value to materialize in a register.
        if (is_registered_struct(ret_ty) && !allow_struct_ret_call) {
            err("a call returning struct '" + ret_ty->struct_name + "' is only allowed as a "
                "`let x: " + ret_ty->struct_name + " = " + c->name + "(...);` initializer or a "
                "`return " + c->name + "(...);` value", c->loc.line, c->loc.col);
        }
        e.ty = ret_ty; return ret_ty;
    }
    if (auto* b = dynamic_cast<BinExpr*>(&e)) {
        const Type* lt = check_expr(*b->lhs);
        const Type* rt = check_expr(*b->rhs, lt); // give rhs a chance to adapt to lhs
        // re-check a literal lhs with rhs's type so `200.0f * s` (s:f32) adapts lhs to f32,
        // and `500 + u64var` adapts lhs int literal to u64 (TYPE_SYSTEM.md Section 6 literal adaptation).
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
        // operator-overload dispatch (TYPE_SYSTEM.md section 7): if both operands
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
                    e.ty = intern(oo->ret);
                    return e.ty;
                }
            }
        }
        if (is_cmp) {
            if (!lt->same(*rt) && !(lt->is_int() && rt->is_int()))
                err("comparison requires same-type operands (got " + lt->to_string() + " and " + rt->to_string() + ")",
                    b->loc.line, b->loc.col);
            e.ty = &type_bool(); return e.ty;
        }
        // arithmetic/bitwise: require same numeric type (literal adaptation already applied)
        // EXCEPTION (TYPE_SYSTEM.md Section 7): shift rhs may be any unsigned integer type
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
        check_expr(*c->operand);
        e.ty = intern(*c->to); return e.ty;
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
        // const-correctness: reassigning a const-declared local is an error.
        // Only a bare-identifier target is checked - arr[0]=... or p.f=...
        // through a const binding is intentionally out of scope (is_const
        // models "this binding can't be reassigned", not deep immutability).
        if (auto* tid = dynamic_cast<Ident*>(a->target.get())) {
            if (const Var* v = lookup_local_var(tid->name)) {
                if (v->is_const)
                    err("cannot assign to const variable '" + tid->name + "'",
                        a->loc.line, a->loc.col);
            }
        }
        const Type* rt = check_expr(*a->value, lt);
        if (lt && rt && !lt->same(*rt) && !(lt->is_int() && rt->is_int()))
            err("assignment type mismatch (" + lt->to_string() + " = " + rt->to_string() + ")",
                a->loc.line, a->loc.col);
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
        // `receiver` set (BINDING_API.md sec 3) before sema ever sees a bare
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
            const Type* got = check_expr(*kv.second, want);
            if (!got->same(*want) && !(got->is_int() && want->is_int()))
                err("field '" + kv.first + "' type mismatch (expected " + want->to_string() + ", got " + got->to_string() + ")",
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
    err("codegen: expression node not yet supported by sema", e.loc.line, e.loc.col);
    e.ty = intern(type_void()); return e.ty;
}

void Checker::check_stmt(Stmt& s, const Type* ret_ty, bool& returns) {
    if (auto* ls = dynamic_cast<LetStmt*>(&s)) {
        // V6-DoS mitigation: reject any local whose frame width exceeds the
        // per-frame byte budget BEFORE codegen emits a `sub rsp, <huge>`.
        // The confirmed exploit was `let a: u8[65536];` -> SIGSEGV (no cap).
        // Also catches V6-overflow (array_len so large byte_size wraps).
        if (ls->ty) {
            int64_t w = frame_byte_width(*ls->ty, structs);
            if (w > MAX_FRAME_BYTES) {
                err("local '" + ls->name + "' frame size (" + std::to_string(w) +
                    " bytes) exceeds the per-frame budget (" + std::to_string(MAX_FRAME_BYTES) +
                    " bytes); reduce the array/struct size",
                    ls->loc.line, ls->loc.col);
            }
        }
        if (!ls->init) {
            // no initializer (parser only allows this for explicitly-typed
            // let/const, never auto): declare at the stated type, default
            // zero-filled by codegen.
            declare(ls->name, intern(*ls->ty), ls->is_const);
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
        const Type* init_ty = check_expr(*ls->init, expected_ty, true);
        const Type* decl_ty;
        if (ls->is_auto) decl_ty = init_ty;
        else { decl_ty = expected_ty; if (!decl_ty->same(*init_ty) && !(decl_ty->is_int()&&init_ty->is_int())
            && !(decl_ty->prim==Prim::I64 && init_ty->prim==Prim::I64))
            err("let type mismatch (" + decl_ty->to_string() + " = " + init_ty->to_string() + ")",
                ls->loc.line, ls->loc.col); }
        declare(ls->name, decl_ty, ls->is_const);
        return;
    }
    if (auto* es = dynamic_cast<ExprStmt*>(&s)) { check_expr(*es->expr); return; }
    if (auto* rs = dynamic_cast<ReturnStmt*>(&s)) {
        returns = true;
        if (rs->value) {
            const Type* vt = check_expr(*rs->value, ret_ty, true);
            if (ret_ty && !vt->same(*ret_ty) && !(ret_ty->is_int()&&vt->is_int()))
                err("return type mismatch (got " + vt->to_string() + ", fn returns " + ret_ty->to_string() + ")",
                    rs->loc.line, rs->loc.col);
            // A struct-returning function's `return` value must be a bare
            // local (codegen copies its bytes through the hidden return
            // pointer) or a call to a function with the SAME struct return
            // type (forwarding this function's own incoming hidden pointer -
            // supports multi-hop chains, e.g. return relay(...);). Anything
            // else has no destination address codegen can use.
            if (ret_ty && is_registered_struct(ret_ty)) {
                bool is_ident = dynamic_cast<const Ident*>(rs->value.get()) != nullptr;
                auto* call = dynamic_cast<const CallExpr*>(rs->value.get());
                bool is_forwarding_call = call && vt->same(*ret_ty);
                if (!is_ident && !is_forwarding_call) {
                    err("a `return` of struct '" + ret_ty->struct_name + "' must be a plain "
                        "local variable or a call to a function with the same struct return "
                        "type", rs->loc.line, rs->loc.col);
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
        push_scope(); bool r=false; check_block(ws->body, ret_ty, r); pop_scope();
        return; // while doesn't guarantee return (condition may be false first)
    }
    if (dynamic_cast<BreakStmt*>(&s)) return;
    if (dynamic_cast<ContinueStmt*>(&s)) return;
    if (auto* bs = dynamic_cast<BlockStmt*>(&s)) {
        push_scope(); check_block(bs->block, ret_ty, returns); pop_scope(); return;
    }
    if (auto* ds = dynamic_cast<DeferStmt*>(&s)) {
        // type-check for side effects (native/script call resolution etc.) -
        // the result is discarded (codegen runs it at function exit, LIFO).
        check_expr(*ds->expr);
        // Scope-safety check: a defer's expression is re-evaluated at
        // function-exit time (CG::emit_defers), which may be well after a
        // block/loop-local variable's compile-time scope has closed. Codegen
        // has no error path for "identifier not found" at that point - it's
        // a silent no-op that leaves whatever was last in the register,
        // which is NOT the variable's real value (verified: `for (...) {
        // defer note(i); }` read back whatever eval() had just loaded to
        // check the defer flag, not i's actual last value). Only params and
        // globals are guaranteed to still resolve wherever the defer fires.
        std::vector<const Ident*> idents;
        collect_idents(*ds->expr, idents);
        for (auto* id : idents) {
            if (!is_param_or_global(id->name))
                err("defer expression references '" + id->name + "', a local variable - "
                    "only function parameters and globals may be referenced in a defer "
                    "expression (a block/loop-local may already be out of scope by the "
                    "time the defer actually runs, at function exit)", id->loc.line, id->loc.col);
        }
        return;
    }
    if (auto* fs = dynamic_cast<ForStmt*>(&s)) {
        push_scope();
        if (fs->init) check_stmt(*fs->init, ret_ty, returns);
        if (fs->cond) { const Type* ct = check_expr(*fs->cond); if(!ct->is_bool()) err("for cond must be bool", fs->loc.line, fs->loc.col); }
        if (fs->step) check_expr(*fs->step);
        bool r=false; check_block(fs->body, ret_ty, r);
        pop_scope();
        return;
    }
    if (auto* ds = dynamic_cast<DoWhileStmt*>(&s)) {
        push_scope(); bool r=false; check_block(ds->body, ret_ty, r); pop_scope();
        if (ds->cond) { const Type* ct = check_expr(*ds->cond); if(!ct->is_bool()) err("do-while cond must be bool", ds->loc.line, ds->loc.col); }
        return;
    }
    if (auto* sw = dynamic_cast<SwitchStmt*>(&s)) {
        const Type* subj_ty = check_expr(*sw->subject);
        if (!subj_ty->is_int() && !subj_ty->is_bool())
            err("switch subject must be an integer or bool (got " + subj_ty->to_string() + ")",
                sw->loc.line, sw->loc.col);
        bool seen_default = false;
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
        }
        // v1: conservative - a switch never proves the function returns on
        // every path (case fallthrough + optional default make that analysis
        // non-trivial, and no script currently relies on it).
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
    // Struct-by-value params/returns are supported (REMAINING_WORK.md 1.6) -
    // codegen has a real word-based param convention and a hidden-pointer
    // return convention. The only restrictions live at call sites, not here:
    // check_struct_arg_shape (a by-value argument must be a bare local) and
    // the CallExpr/ReturnStmt position checks for struct-returning calls.
    for (auto& p : f.params) declare(p.name, intern(*p.ty), false);
    const Type* ret_ty = intern(*f.ret);
    bool returns = false;
    check_block(f.body, ret_ty, returns);
    if (!ret_ty->is_void() && !returns) {
        // find end of function for the error loc
        err("function '" + f.name + "' not all paths return a value", f.loc.line, f.loc.col);
    }
    pop_scope();
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
        c.globals[g.name] = c.intern(*g.ty);
    }
    // register script function signatures (pass 2 of COMPILER_PIPELINE.md Section 4:
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

    // check each function
    for (auto& f : prog.funcs) {
        c.check_func(f);
    }

    SemaResult r;
    r.errors = std::move(c.errs);
    r.ok = r.errors.empty();
    return r;
}

} // namespace ember
