// thin_lower.cpp — Stage A c2: the AST -> ThinFunction lowering implementation.
//
// Mirrors src/codegen.cpp's CG::eval / CG::exec_stmt / CG::exec_block /
// compile_func mechanically, producing a ThinFunction (src/thin_ir.hpp) whose
// instructions are a value-equivalent IR for the same source. See thin_lower.hpp
// for the representation conventions and the correctness contract.
//
// LAZY MODE: mechanical lowering, no optimization. The prescan / frame-layout
// helpers (value_bytes, local_width_bytes, words_for_type, prescan_block,
// sum_bytes, count_*_temps_block, collect_defers, find_pin_candidate) are
// reimplemented here because CG (in codegen.cpp) lives in an anonymous
// namespace and is not linkable. They are byte-for-byte mechanical copies of
// the CG helpers so the frame layout matches compile_func EXACTLY.
//
// DEPENDENCY NOTE: this TU links into ember_frontend (alongside codegen.cpp)
// because it calls Type methods (is_int/is_float/byte_size/...) defined in
// types.cpp and uses sema.hpp's StructLayoutTable / NativeSig. It includes
// context.hpp for TrapReason ordinals and codegen.hpp for CodeGenCtx.

#include "thin_lower.hpp"
#include "stmt_walk.hpp"  // walk_if: shared IfStmt traversal for prescan/count passes
#include "context.hpp"   // TrapReason
#include "sema.hpp"      // StructLayoutTable, StructLayout, NativeSig

#include <cassert>
#include <cstring>
#include <algorithm>
#include <functional>
#include <limits>
#include <optional>
#include <unordered_map>
#include <vector>

namespace ember {

namespace {

// Bit-preserving uint64_t -> int64_t (matches codegen.cpp's bit_cast_i64).
static int64_t bit_cast_i64(uint64_t u) {
    int64_t i;
    std::memcpy(&i, &u, sizeof(i));
    return i;
}

inline int32_t round16(int32_t n) { return (n + 15) & ~15; }

// The value descriptor returned by lower_expr. See thin_lower.hpp.
struct LoweredValue {
    enum Kind { Scalar, Slice, Aggregate } kind = Scalar;
    VReg vreg = 0;          // Scalar/float: the vreg. Slice: ptr vreg (len=vreg+1). Aggregate: 0.
    int32_t frame_off = 0;  // Aggregate: rbp-negative ABSOLUTE offset of the slot.
    const Type* ty = nullptr;
};

// One recorded defer site (mirrors CG::DeferSite).
struct DeferSite {
    const DeferStmt* stmt = nullptr;
    int32_t flag_offset = 0;
    std::unordered_map<std::string, int32_t> locals_at_decl;
    std::unordered_map<std::string, const Type*> types_at_decl;
};
struct CleanupScope { std::vector<size_t> reached_sites; };

// Loop context for break/continue (mirrors CG::LoopCtx). is_switch = break-only.
struct LoopCtx {
    uint32_t cond_bb = 0;   // continue target (cond or step block)
    uint32_t exit_bb = 0;   // break target
    bool is_switch = false;
    size_t cleanup_depth = 0;
};

struct PinState { std::string name; int32_t offset = 0; };

// The lowerer. One instance per function.
struct ThinLowerer {
    const CodeGenCtx& ctx;
    const FuncDecl& f;
    ThinFunction out;

    // --- vreg + block management ---
    VReg next_vreg = 1;     // 0 = invalid/none; allocate from 1
    std::unordered_map<VReg, const Type*> vreg_types;
    uint32_t cur_bb = 0;    // index of the block currently being filled

    // --- frame layout (mirrors CG) ---
    int32_t rbx_save_offset = -8;
    int32_t next_local_off = 0;          // grows downward
    std::unordered_map<std::string, int32_t> locals;
    std::unordered_map<std::string, const Type*> local_types;
    int32_t struct_ret_ptr_offset = 0;
    int32_t arg_temps_base = 0;
    int32_t frame_size = 0;
    bool makes_calls = false;
    int max_args = 0;

    // --- synthesized temp slot counters (mirrors CG) ---
    int32_t temp_counter = 0;
    int32_t arr_temp_counter = 0;
    int32_t str_temp_counter = 0;
    std::vector<std::shared_ptr<Type>> arr_temp_types;
    std::vector<std::shared_ptr<Type>> str_temp_types;

    // --- rodata (StringLit bytes) ---
    // (carried into out.rodata)

    // --- defer / cleanup ---
    std::vector<DeferSite> defer_sites;
    std::unordered_map<const DeferStmt*, size_t> defer_site_indices;
    std::vector<CleanupScope> cleanup_scopes;

    // --- loop / pin ---
    std::vector<LoopCtx> loops;
    std::optional<PinState> active_pin;

    // --- obf ---
    ObfOptions obf;
    bool non_serializable = false;
    std::string non_serializable_reason;

    ThinLowerer(const CodeGenCtx& c, const FuncDecl& fn) : ctx(c), f(fn) {
        obf = c.obf;
    }

    // ─────────────── type helpers (mechanical copies of CG) ───────────────

    static int32_t value_bytes(const Type* t, const StructLayoutTable* structs) {
        if (!t) return 8;
        if (t->is_slice) return 16;
        if (t->array_len > 0)
            return int32_t(t->array_len) * value_bytes(t->elem.get(), structs);
        if (!t->struct_name.empty() && structs) {
            auto it = structs->find(t->struct_name);
            if (it != structs->end()) return it->second.size;
        }
        switch (t->prim) {
        case Prim::Bool: case Prim::I8: case Prim::U8: return 1;
        case Prim::I16: case Prim::U16: return 2;
        case Prim::I32: case Prim::U32: case Prim::F32: return 4;
        default: return 8;
        }
    }
    static int32_t local_width_bytes(const Type* t, const StructLayoutTable* structs) {
        if (t && (t->is_slice || t->array_len > 0 ||
                  (!t->struct_name.empty() && structs && structs->count(t->struct_name))))
            return value_bytes(t, structs);
        return 8;
    }
    static int32_t words_for_type(const Type* t, const StructLayoutTable* structs) {
        if (t && t->is_slice) return 2;
        if (t && !t->struct_name.empty() && structs) {
            auto it = structs->find(t->struct_name);
            if (it != structs->end()) return (it->second.size + 7) / 8;
        }
        return 1;
    }
    static int int_bits(const Type* t) {
        if (!t) return 64;
        switch (t->prim) {
        case Prim::I8: case Prim::U8: return 8;
        case Prim::I16: case Prim::U16: return 16;
        case Prim::I32: case Prim::U32: return 32;
        default: return 64;
        }
    }
    bool is_registered_struct_ty(const Type* t) const {
        return t && !t->struct_name.empty() && ctx.structs && ctx.structs->count(t->struct_name) != 0;
    }
    bool returns_struct_by_ptr() const {
        return f.ret && !f.ret->struct_name.empty() && ctx.structs && ctx.structs->count(f.ret->struct_name) != 0;
    }
    int32_t struct_size(const Type* t) const {
        if (!t || t->struct_name.empty() || !ctx.structs) return 0;
        auto it = ctx.structs->find(t->struct_name);
        return it == ctx.structs->end() ? 0 : it->second.size;
    }

    // ─────────────── locals + temp allocation (mirrors CG) ───────────────

    int32_t alloc_local(const std::string& n, const Type* t) {
        int32_t width = local_width_bytes(t, ctx.structs);
        next_local_off += width;
        int32_t off = -next_local_off;
        locals[n] = off;
        local_types[n] = t;
        return off;
    }
    int32_t alloc_struct_temp(const Type* t) {
        std::string name = "__tmp$" + std::to_string(temp_counter++);
        return alloc_local(name, t);
    }
    int32_t alloc_arr_temp(const Type* elem, uint32_t count) {
        std::string name = "__arrtmp$" + std::to_string(arr_temp_counter++);
        auto bt = std::make_shared<Type>(*elem);
        Type t; t.prim = elem->prim; t.array_len = count; t.elem = bt;
        arr_temp_types.push_back(std::make_shared<Type>(std::move(t)));
        return alloc_local(name, arr_temp_types.back().get());
    }
    int32_t alloc_str_temp(int64_t baked_len) {
        std::string name = "__strtmp$" + std::to_string(str_temp_counter++);
        auto bt = std::make_shared<Type>(make_prim(Prim::U8));
        Type t; t.prim = Prim::U8; t.array_len = uint32_t(baked_len); t.elem = bt;
        str_temp_types.push_back(std::make_shared<Type>(std::move(t)));
        return alloc_local(name, str_temp_types.back().get());
    }

    uint32_t append_rodata(const uint8_t* data, size_t size) {
        uint32_t off = uint32_t(out.rodata.size());
        out.rodata.insert(out.rodata.end(), data, data + size);
        return off;
    }

    // ─────────────── vreg + block helpers ───────────────

    VReg new_vreg(const Type* ty = nullptr) {
        VReg v = next_vreg++;
        if (ty) vreg_types[v] = ty;
        return v;
    }
    // Allocate a slice's two consecutive vregs; return the ptr vreg (len = v+1).
    VReg new_slice_vregs(const Type* ty) {
        VReg ptr = next_vreg++;
        VReg len = next_vreg++;  // consecutive
        (void)len;
        if (ty) { vreg_types[ptr] = ty; vreg_types[len] = &type_i64(); }
        return ptr;
    }

    ThinBlock& cur_block() { return out.blocks[cur_bb]; }
    // Allocate a new (empty, unsealed) block and return its id. Does NOT switch the
    // current block — call enter_block() to start filling it. This lets a caller
    // allocate branch targets, seal the current block with a term that names them,
    // then enter each target in turn.
    uint32_t new_block() {
        ThinBlock b;
        b.id = uint32_t(out.blocks.size());
        out.blocks.push_back(std::move(b));
        return b.id;
    }
    // Start filling an already-allocated block. The block must be unsealed.
    void enter_block(uint32_t id) { cur_bb = id; }
    // Allocate a new block AND enter it (the common "continue lowering into a fresh
    // block" case). Returns the new block's id.
    uint32_t new_and_enter() { uint32_t id = new_block(); cur_bb = id; return id; }
    // Push an instr into the current block and return a reference. NOTE: the
    // reference is only valid until the next push into this block's instrs vector
    // (a push may reallocate). Populate the instr fully BEFORE any other emit.
    ThinInstr& emit(ThinOp op, VReg dst = 0, VReg src1 = 0, VReg src2 = 0, Loc loc = {}) {
        ThinInstr in;
        in.op = op; in.dst = dst; in.src1 = src1; in.src2 = src2; in.loc = loc;
        cur_block().instrs.push_back(std::move(in));
        return cur_block().instrs.back();
    }
    void set_term_jmp(uint32_t target) {
        ThinTerm t; t.kind = TermKind::Jmp; t.target = target;
        cur_block().term = t;
    }
    void set_term_branch(VReg cond, uint32_t tt, uint32_t ft) {
        ThinTerm t; t.kind = TermKind::Branch; t.cond = cond; t.target = tt; t.false_target = ft;
        cur_block().term = t;
    }
    void set_term_return(VReg ret) {
        ThinTerm t; t.kind = TermKind::Return; t.ret = ret;
        cur_block().term = t;
    }
    void set_term_trap(uint8_t reason) {
        ThinTerm t; t.kind = TermKind::Trap; t.trap_reason = reason;
        cur_block().term = t;
    }

    // ─────────────── prescans (mechanical copies of CG) ───────────────

    void prescan_block(const Block& b) { for (auto& s : b.stmts) prescan_stmt(*s); }
    void prescan_stmt(const Stmt& s) {
        // static_assert is fully resolved at sema and produces NO runtime code;
        // every statement walker skips it (mirrors the elided assert_eq_* path).
        if (dynamic_cast<const StaticAssertStmt*>(&s)) return;
        if (auto* ls = dynamic_cast<const LetStmt*>(&s)) { if (ls->init) prescan_expr(*ls->init); return; }
        if (auto* es = dynamic_cast<const ExprStmt*>(&s)) { prescan_expr(*es->expr); return; }
        if (auto* rs = dynamic_cast<const ReturnStmt*>(&s)) { if (rs->value) prescan_expr(*rs->value); return; }
        if (auto* ds = dynamic_cast<const DeferStmt*>(&s)) { prescan_expr(*ds->expr); return; }
        if (auto* is = dynamic_cast<const IfStmt*>(&s)) {
            walk_if(*is, [&](const Expr& e){ prescan_expr(e); }, [&](const Block& b){ prescan_block(b); });
            return;
        }
        if (auto* ws = dynamic_cast<const WhileStmt*>(&s)) { prescan_expr(*ws->cond); prescan_block(ws->body); return; }
        if (auto* ds = dynamic_cast<const DoWhileStmt*>(&s)) { prescan_block(ds->body); prescan_expr(*ds->cond); return; }
        if (auto* fe = dynamic_cast<const ForEachStmt*>(&s)) { prescan_expr(*fe->iter); prescan_block(fe->body); return; }
        if (auto* fs = dynamic_cast<const ForStmt*>(&s)) {
            if (fs->init) prescan_stmt(*fs->init);
            if (fs->cond) prescan_expr(*fs->cond);
            if (fs->step) prescan_expr(*fs->step);
            prescan_block(fs->body); return;
        }
        if (auto* bs = dynamic_cast<const BlockStmt*>(&s)) { prescan_block(bs->block); return; }
        if (auto* sw = dynamic_cast<const SwitchStmt*>(&s)) {
            prescan_expr(*sw->subject);
            for (auto& c : sw->cases) prescan_block(c.body);
            return;
        }
    }
    void prescan_expr(const Expr& ex) {
        if (auto* c = dynamic_cast<const CallExpr*>(&ex)) {
            makes_calls = true;
            max_args = std::max(max_args, int(c->args.size()));
            for (auto& a : c->args) prescan_expr(*a);
            return;
        }
        if (auto* b = dynamic_cast<const BinExpr*>(&ex)) { prescan_expr(*b->lhs); prescan_expr(*b->rhs); return; }
        if (auto* u = dynamic_cast<const UnaryExpr*>(&ex)) { prescan_expr(*u->operand); return; }
        if (auto* c = dynamic_cast<const CastExpr*>(&ex)) { prescan_expr(*c->operand); return; }
        if (auto* t = dynamic_cast<const TernaryExpr*>(&ex)) { prescan_expr(*t->cond); prescan_expr(*t->then_e); prescan_expr(*t->else_e); return; }
        if (auto* a = dynamic_cast<const AssignExpr*>(&ex)) { prescan_expr(*a->value); if (a->target) prescan_expr(*a->target); return; }
    }

    void count_struct_temps_block(const Block& b, int32_t& total) { for (auto& s : b.stmts) count_struct_temps_stmt(*s, total); }
    void count_struct_temps_stmt(const Stmt& s, int32_t& total) {
        if (dynamic_cast<const StaticAssertStmt*>(&s)) return;
        if (auto* ls = dynamic_cast<const LetStmt*>(&s)) { if (ls->init) count_struct_temps_expr(*ls->init, total); return; }
        if (auto* es = dynamic_cast<const ExprStmt*>(&s)) { count_struct_temps_expr(*es->expr, total); return; }
        if (auto* rs = dynamic_cast<const ReturnStmt*>(&s)) {
            if (rs->value && returns_struct_by_ptr()) {
                if (dynamic_cast<const StructLit*>(rs->value.get()) &&
                    rs->value->ty && is_registered_struct_ty(rs->value->ty))
                    total += value_bytes(rs->value->ty, ctx.structs);
            }
            if (rs->value) count_struct_temps_expr(*rs->value, total);
            return;
        }
        if (auto* ds = dynamic_cast<const DeferStmt*>(&s)) { count_struct_temps_expr(*ds->expr, total); return; }
        if (auto* is = dynamic_cast<const IfStmt*>(&s)) {
            walk_if(*is, [&](const Expr& e){ count_struct_temps_expr(e, total); }, [&](const Block& b){ count_struct_temps_block(b, total); });
            return;
        }
        if (auto* ws = dynamic_cast<const WhileStmt*>(&s)) { count_struct_temps_expr(*ws->cond, total); count_struct_temps_block(ws->body, total); return; }
        if (auto* ds = dynamic_cast<const DoWhileStmt*>(&s)) { count_struct_temps_block(ds->body, total); count_struct_temps_expr(*ds->cond, total); return; }
        if (auto* fs = dynamic_cast<const ForStmt*>(&s)) {
            if (fs->init) count_struct_temps_stmt(*fs->init, total);
            if (fs->cond) count_struct_temps_expr(*fs->cond, total);
            if (fs->step) count_struct_temps_expr(*fs->step, total);
            count_struct_temps_block(fs->body, total); return;
        }
        if (auto* bs = dynamic_cast<const BlockStmt*>(&s)) { count_struct_temps_block(bs->block, total); return; }
        if (auto* sw = dynamic_cast<const SwitchStmt*>(&s)) {
            count_struct_temps_expr(*sw->subject, total);
            for (auto& c : sw->cases) count_struct_temps_block(c.body, total);
            return;
        }
    }
    void count_struct_temps_expr(const Expr& ex, int32_t& total) {
        if (auto* c = dynamic_cast<const CallExpr*>(&ex)) {
            if (c->receiver) count_struct_temps_expr(*c->receiver, total);
            for (auto& a : c->args) {
                if (a->ty && is_registered_struct_ty(a->ty) &&
                    !dynamic_cast<const Ident*>(a.get()))
                    total += value_bytes(a->ty, ctx.structs);
                count_struct_temps_expr(*a, total);
            }
            return;
        }
        if (auto* b = dynamic_cast<const BinExpr*>(&ex)) { count_struct_temps_expr(*b->lhs, total); count_struct_temps_expr(*b->rhs, total); return; }
        if (auto* u = dynamic_cast<const UnaryExpr*>(&ex)) { count_struct_temps_expr(*u->operand, total); return; }
        if (auto* c = dynamic_cast<const CastExpr*>(&ex)) { count_struct_temps_expr(*c->operand, total); return; }
        if (auto* t = dynamic_cast<const TernaryExpr*>(&ex)) { count_struct_temps_expr(*t->cond, total); count_struct_temps_expr(*t->then_e, total); count_struct_temps_expr(*t->else_e, total); return; }
        if (auto* a = dynamic_cast<const AssignExpr*>(&ex)) { if (a->target) count_struct_temps_expr(*a->target, total); count_struct_temps_expr(*a->value, total); return; }
        if (auto* ix = dynamic_cast<const IndexExpr*>(&ex)) { count_struct_temps_expr(*ix->base, total); count_struct_temps_expr(*ix->index, total); return; }
        if (auto* fx = dynamic_cast<const FieldExpr*>(&ex)) { count_struct_temps_expr(*fx->base, total); return; }
        if (auto* v = dynamic_cast<const ViewExpr*>(&ex)) { count_struct_temps_expr(*v->base, total); return; }
        if (auto* sl = dynamic_cast<const StructLit*>(&ex)) { for (auto& kv : sl->fields) count_struct_temps_expr(*kv.second, total); return; }
    }

    void count_arr_temps_block(const Block& b, int32_t& total) { for (auto& s : b.stmts) count_arr_temps_stmt(*s, total); }
    void count_arr_temps_stmt(const Stmt& s, int32_t& total) {
        if (dynamic_cast<const StaticAssertStmt*>(&s)) return;
        if (auto* ls = dynamic_cast<const LetStmt*>(&s)) { if (ls->init) count_arr_temps_expr(*ls->init, total); return; }
        if (auto* es = dynamic_cast<const ExprStmt*>(&s)) { count_arr_temps_expr(*es->expr, total); return; }
        if (auto* rs = dynamic_cast<const ReturnStmt*>(&s)) { if (rs->value) count_arr_temps_expr(*rs->value, total); return; }
        if (auto* ds = dynamic_cast<const DeferStmt*>(&s)) { count_arr_temps_expr(*ds->expr, total); return; }
        if (auto* is = dynamic_cast<const IfStmt*>(&s)) {
            walk_if(*is, [&](const Expr& e){ count_arr_temps_expr(e, total); }, [&](const Block& b){ count_arr_temps_block(b, total); });
            return;
        }
        if (auto* ws = dynamic_cast<const WhileStmt*>(&s)) { count_arr_temps_expr(*ws->cond, total); count_arr_temps_block(ws->body, total); return; }
        if (auto* ds = dynamic_cast<const DoWhileStmt*>(&s)) { count_arr_temps_block(ds->body, total); count_arr_temps_expr(*ds->cond, total); return; }
        if (auto* fs = dynamic_cast<const ForStmt*>(&s)) {
            if (fs->init) count_arr_temps_stmt(*fs->init, total);
            if (fs->cond) count_arr_temps_expr(*fs->cond, total);
            if (fs->step) count_arr_temps_expr(*fs->step, total);
            count_arr_temps_block(fs->body, total); return;
        }
        if (auto* bs = dynamic_cast<const BlockStmt*>(&s)) { count_arr_temps_block(bs->block, total); return; }
        if (auto* sw = dynamic_cast<const SwitchStmt*>(&s)) {
            count_arr_temps_expr(*sw->subject, total);
            for (auto& c : sw->cases) count_arr_temps_block(c.body, total);
            return;
        }
    }
    void count_arr_temps_expr(const Expr& ex, int32_t& total) {
        if (auto* al = dynamic_cast<const ArrayLit*>(&ex)) {
            if (al->ty && al->ty->is_slice && al->ty->elem) {
                total += int32_t(al->elements.size()) * value_bytes(al->ty->elem.get(), ctx.structs);
            }
            for (auto& el : al->elements) count_arr_temps_expr(*el, total);
            return;
        }
        if (auto* c = dynamic_cast<const CallExpr*>(&ex)) {
            if (c->receiver) count_arr_temps_expr(*c->receiver, total);
            for (auto& a : c->args) count_arr_temps_expr(*a, total);
            return;
        }
        if (auto* b = dynamic_cast<const BinExpr*>(&ex)) { count_arr_temps_expr(*b->lhs, total); count_arr_temps_expr(*b->rhs, total); return; }
        if (auto* u = dynamic_cast<const UnaryExpr*>(&ex)) { count_arr_temps_expr(*u->operand, total); return; }
        if (auto* c = dynamic_cast<const CastExpr*>(&ex)) { count_arr_temps_expr(*c->operand, total); return; }
        if (auto* t = dynamic_cast<const TernaryExpr*>(&ex)) { count_arr_temps_expr(*t->cond, total); count_arr_temps_expr(*t->then_e, total); count_arr_temps_expr(*t->else_e, total); return; }
        if (auto* a = dynamic_cast<const AssignExpr*>(&ex)) { if (a->target) count_arr_temps_expr(*a->target, total); count_arr_temps_expr(*a->value, total); return; }
        if (auto* ix = dynamic_cast<const IndexExpr*>(&ex)) { count_arr_temps_expr(*ix->base, total); count_arr_temps_expr(*ix->index, total); return; }
        if (auto* fx = dynamic_cast<const FieldExpr*>(&ex)) { count_arr_temps_expr(*fx->base, total); return; }
        if (auto* v = dynamic_cast<const ViewExpr*>(&ex)) { count_arr_temps_expr(*v->base, total); return; }
        if (auto* sl = dynamic_cast<const StructLit*>(&ex)) { for (auto& kv : sl->fields) count_arr_temps_expr(*kv.second, total); return; }
    }

    void count_str_temps_block(const Block& b, int32_t& total) { for (auto& s : b.stmts) count_str_temps_stmt(*s, total); }
    void count_str_temps_stmt(const Stmt& s, int32_t& total) {
        if (dynamic_cast<const StaticAssertStmt*>(&s)) return;
        if (auto* ls = dynamic_cast<const LetStmt*>(&s)) { if (ls->init) count_str_temps_expr(*ls->init, total); return; }
        if (auto* es = dynamic_cast<const ExprStmt*>(&s)) { count_str_temps_expr(*es->expr, total); return; }
        if (auto* rs = dynamic_cast<const ReturnStmt*>(&s)) { if (rs->value) count_str_temps_expr(*rs->value, total); return; }
        if (auto* ds = dynamic_cast<const DeferStmt*>(&s)) { count_str_temps_expr(*ds->expr, total); return; }
        if (auto* is = dynamic_cast<const IfStmt*>(&s)) {
            walk_if(*is, [&](const Expr& e){ count_str_temps_expr(e, total); }, [&](const Block& b){ count_str_temps_block(b, total); });
            return;
        }
        if (auto* ws = dynamic_cast<const WhileStmt*>(&s)) { count_str_temps_expr(*ws->cond, total); count_str_temps_block(ws->body, total); return; }
        if (auto* ds = dynamic_cast<const DoWhileStmt*>(&s)) { count_str_temps_block(ds->body, total); count_str_temps_expr(*ds->cond, total); return; }
        if (auto* fs = dynamic_cast<const ForStmt*>(&s)) {
            if (fs->init) count_str_temps_stmt(*fs->init, total);
            if (fs->cond) count_str_temps_expr(*fs->cond, total);
            if (fs->step) count_str_temps_expr(*fs->step, total);
            count_str_temps_block(fs->body, total); return;
        }
        if (auto* bs = dynamic_cast<const BlockStmt*>(&s)) { count_str_temps_block(bs->block, total); return; }
        if (auto* sw = dynamic_cast<const SwitchStmt*>(&s)) {
            count_str_temps_expr(*sw->subject, total);
            for (auto& c : sw->cases) count_str_temps_block(c.body, total);
            return;
        }
    }
    void count_str_temps_expr(const Expr& ex, int32_t& total) {
        if (auto* lit = dynamic_cast<const StringLit*>(&ex)) {
            if (lit->encrypted && lit->baked_key != 0 && lit->baked_len > 0)
                total += int32_t(lit->baked_len);
            return;
        }
        if (auto* c = dynamic_cast<const CallExpr*>(&ex)) {
            if (c->receiver) count_str_temps_expr(*c->receiver, total);
            for (auto& a : c->args) count_str_temps_expr(*a, total);
            return;
        }
        if (auto* b = dynamic_cast<const BinExpr*>(&ex)) { count_str_temps_expr(*b->lhs, total); count_str_temps_expr(*b->rhs, total); return; }
        if (auto* u = dynamic_cast<const UnaryExpr*>(&ex)) { count_str_temps_expr(*u->operand, total); return; }
        if (auto* c = dynamic_cast<const CastExpr*>(&ex)) { count_str_temps_expr(*c->operand, total); return; }
        if (auto* t = dynamic_cast<const TernaryExpr*>(&ex)) { count_str_temps_expr(*t->cond, total); count_str_temps_expr(*t->then_e, total); count_str_temps_expr(*t->else_e, total); return; }
        if (auto* a = dynamic_cast<const AssignExpr*>(&ex)) { if (a->target) count_str_temps_expr(*a->target, total); count_str_temps_expr(*a->value, total); return; }
        if (auto* ix = dynamic_cast<const IndexExpr*>(&ex)) { count_str_temps_expr(*ix->base, total); count_str_temps_expr(*ix->index, total); return; }
        if (auto* fx = dynamic_cast<const FieldExpr*>(&ex)) { count_str_temps_expr(*fx->base, total); return; }
        if (auto* v = dynamic_cast<const ViewExpr*>(&ex)) { count_str_temps_expr(*v->base, total); return; }
        if (auto* sl = dynamic_cast<const StructLit*>(&ex)) { for (auto& kv : sl->fields) count_str_temps_expr(*kv.second, total); return; }
        if (auto* al = dynamic_cast<const ArrayLit*>(&ex)) { for (auto& el : al->elements) count_str_temps_expr(*el, total); return; }
    }

    // Count short-circuit (&&/||) result temps: one 8-byte bool frame slot per
    // LAnd/LOr BinExpr (the IR lowering's join-block result vreg must be frame-
    // backed; see the logical-temp note in the frame-plan computation). Recurses
    // into every expr/stmt shape that can contain a BinExpr (the same shapes the
    // other count_*_temps walkers cover).
    void count_logical_temps_block(const Block& b, int32_t& total) { for (auto& s : b.stmts) count_logical_temps_stmt(*s, total); }
    void count_logical_temps_stmt(const Stmt& s, int32_t& total) {
        if (dynamic_cast<const StaticAssertStmt*>(&s)) return;
        if (auto* ls = dynamic_cast<const LetStmt*>(&s)) { if (ls->init) count_logical_temps_expr(*ls->init, total); return; }
        if (auto* es = dynamic_cast<const ExprStmt*>(&s)) { count_logical_temps_expr(*es->expr, total); return; }
        if (auto* rs = dynamic_cast<const ReturnStmt*>(&s)) { if (rs->value) count_logical_temps_expr(*rs->value, total); return; }
        if (auto* ds = dynamic_cast<const DeferStmt*>(&s)) { count_logical_temps_expr(*ds->expr, total); return; }
        if (auto* is = dynamic_cast<const IfStmt*>(&s)) {
            walk_if(*is, [&](const Expr& e){ count_logical_temps_expr(e, total); }, [&](const Block& b){ count_logical_temps_block(b, total); });
            return;
        }
        if (auto* ws = dynamic_cast<const WhileStmt*>(&s)) { count_logical_temps_expr(*ws->cond, total); count_logical_temps_block(ws->body, total); return; }
        if (auto* ds = dynamic_cast<const DoWhileStmt*>(&s)) { count_logical_temps_block(ds->body, total); count_logical_temps_expr(*ds->cond, total); return; }
        if (auto* fs = dynamic_cast<const ForStmt*>(&s)) {
            if (fs->init) count_logical_temps_stmt(*fs->init, total);
            if (fs->cond) count_logical_temps_expr(*fs->cond, total);
            if (fs->step) count_logical_temps_expr(*fs->step, total);
            count_logical_temps_block(fs->body, total); return;
        }
        if (auto* bs = dynamic_cast<const BlockStmt*>(&s)) { count_logical_temps_block(bs->block, total); return; }
        if (auto* sw = dynamic_cast<const SwitchStmt*>(&s)) {
            count_logical_temps_expr(*sw->subject, total);
            for (auto& c : sw->cases) count_logical_temps_block(c.body, total);
            return;
        }
    }
    void count_logical_temps_expr(const Expr& ex, int32_t& total) {
        if (auto* b = dynamic_cast<const BinExpr*>(&ex)) {
            if (b->op == BinExpr::Op::LAnd || b->op == BinExpr::Op::LOr) total += 8;
            count_logical_temps_expr(*b->lhs, total); count_logical_temps_expr(*b->rhs, total); return;
        }
        if (auto* c = dynamic_cast<const CallExpr*>(&ex)) {
            if (c->receiver) count_logical_temps_expr(*c->receiver, total);
            for (auto& a : c->args) count_logical_temps_expr(*a, total);
            return;
        }
        if (auto* u = dynamic_cast<const UnaryExpr*>(&ex)) { count_logical_temps_expr(*u->operand, total); return; }
        if (auto* c = dynamic_cast<const CastExpr*>(&ex)) { count_logical_temps_expr(*c->operand, total); return; }
        if (auto* t = dynamic_cast<const TernaryExpr*>(&ex)) { count_logical_temps_expr(*t->cond, total); count_logical_temps_expr(*t->then_e, total); count_logical_temps_expr(*t->else_e, total); return; }
        if (auto* a = dynamic_cast<const AssignExpr*>(&ex)) { if (a->target) count_logical_temps_expr(*a->target, total); count_logical_temps_expr(*a->value, total); return; }
        if (auto* ix = dynamic_cast<const IndexExpr*>(&ex)) { count_logical_temps_expr(*ix->base, total); count_logical_temps_expr(*ix->index, total); return; }
        if (auto* fx = dynamic_cast<const FieldExpr*>(&ex)) { count_logical_temps_expr(*fx->base, total); return; }
        if (auto* v = dynamic_cast<const ViewExpr*>(&ex)) { count_logical_temps_expr(*v->base, total); return; }
        if (auto* sl = dynamic_cast<const StructLit*>(&ex)) { for (auto& kv : sl->fields) count_logical_temps_expr(*kv.second, total); return; }
        if (auto* al = dynamic_cast<const ArrayLit*>(&ex)) { for (auto& el : al->elements) count_logical_temps_expr(*el, total); return; }
    }

    void collect_defers(const Block& b) {
        for (auto& s : b.stmts) {
            if (auto* ds = dynamic_cast<const DeferStmt*>(s.get())) {
                size_t index = defer_sites.size();
                defer_sites.push_back(DeferSite{ds});
                defer_site_indices[ds] = index;
            }
            if (auto* is = dynamic_cast<const IfStmt*>(s.get())) { collect_defers(is->then_b); if (is->has_else) collect_defers(is->else_b); }
            if (auto* ws = dynamic_cast<const WhileStmt*>(s.get())) collect_defers(ws->body);
            if (auto* ds = dynamic_cast<const DoWhileStmt*>(s.get())) collect_defers(ds->body);
            if (auto* bs = dynamic_cast<const BlockStmt*>(s.get())) collect_defers(bs->block);
            if (auto* fs = dynamic_cast<const ForStmt*>(s.get())) collect_defers(fs->body);
            if (auto* sw = dynamic_cast<const SwitchStmt*>(s.get())) for (auto& c : sw->cases) collect_defers(c.body);
        }
    }

    // Item E pin-candidate selection (mechanical copy of CG::find_pin_candidate).
    void count_pin_refs_block(const Block& b, std::unordered_map<std::string,int>& counts) {
        for (auto& s : b.stmts) count_pin_refs_stmt(*s, counts);
    }
    void count_pin_refs_stmt(const Stmt& s, std::unordered_map<std::string,int>& counts) {
        if (dynamic_cast<const StaticAssertStmt*>(&s)) return;
        if (auto* ls = dynamic_cast<const LetStmt*>(&s)) { if (ls->init) count_pin_refs_expr(*ls->init, counts); return; }
        if (auto* es = dynamic_cast<const ExprStmt*>(&s)) { count_pin_refs_expr(*es->expr, counts); return; }
        if (auto* rs = dynamic_cast<const ReturnStmt*>(&s)) { if (rs->value) count_pin_refs_expr(*rs->value, counts); return; }
        if (auto* ds = dynamic_cast<const DeferStmt*>(&s)) { count_pin_refs_expr(*ds->expr, counts); return; }
        if (auto* is = dynamic_cast<const IfStmt*>(&s)) {
            walk_if(*is, [&](const Expr& e){ count_pin_refs_expr(e, counts); }, [&](const Block& b){ count_pin_refs_block(b, counts); });
            return;
        }
        if (auto* ws = dynamic_cast<const WhileStmt*>(&s)) { count_pin_refs_expr(*ws->cond, counts); count_pin_refs_block(ws->body, counts); return; }
        if (auto* ds = dynamic_cast<const DoWhileStmt*>(&s)) { count_pin_refs_block(ds->body, counts); count_pin_refs_expr(*ds->cond, counts); return; }
        if (auto* fs = dynamic_cast<const ForStmt*>(&s)) {
            if (fs->init) count_pin_refs_stmt(*fs->init, counts);
            if (fs->cond) count_pin_refs_expr(*fs->cond, counts);
            if (fs->step) count_pin_refs_expr(*fs->step, counts);
            count_pin_refs_block(fs->body, counts); return;
        }
        if (auto* bs = dynamic_cast<const BlockStmt*>(&s)) { count_pin_refs_block(bs->block, counts); return; }
        if (auto* sw = dynamic_cast<const SwitchStmt*>(&s)) {
            count_pin_refs_expr(*sw->subject, counts);
            for (auto& c : sw->cases) count_pin_refs_block(c.body, counts);
            return;
        }
    }
    void count_pin_refs_expr(const Expr& ex, std::unordered_map<std::string,int>& counts) {
        if (auto* id = dynamic_cast<const Ident*>(&ex)) { counts[id->name]++; return; }
        if (auto* c = dynamic_cast<const CallExpr*>(&ex)) {
            if (c->receiver) count_pin_refs_expr(*c->receiver, counts);
            for (auto& a : c->args) count_pin_refs_expr(*a, counts);
            return;
        }
        if (auto* b = dynamic_cast<const BinExpr*>(&ex)) { count_pin_refs_expr(*b->lhs, counts); count_pin_refs_expr(*b->rhs, counts); return; }
        if (auto* u = dynamic_cast<const UnaryExpr*>(&ex)) { count_pin_refs_expr(*u->operand, counts); return; }
        if (auto* c = dynamic_cast<const CastExpr*>(&ex)) { count_pin_refs_expr(*c->operand, counts); return; }
        if (auto* t = dynamic_cast<const TernaryExpr*>(&ex)) {
            count_pin_refs_expr(*t->cond, counts); count_pin_refs_expr(*t->then_e, counts); count_pin_refs_expr(*t->else_e, counts);
            return;
        }
        if (auto* a = dynamic_cast<const AssignExpr*>(&ex)) {
            if (a->target) count_pin_refs_expr(*a->target, counts);
            count_pin_refs_expr(*a->value, counts);
            return;
        }
        if (auto* ix = dynamic_cast<const IndexExpr*>(&ex)) { count_pin_refs_expr(*ix->base, counts); count_pin_refs_expr(*ix->index, counts); return; }
        if (auto* fx = dynamic_cast<const FieldExpr*>(&ex)) { count_pin_refs_expr(*fx->base, counts); return; }
        if (auto* v = dynamic_cast<const ViewExpr*>(&ex)) { count_pin_refs_expr(*v->base, counts); return; }
    }
    std::optional<std::string> find_pin_candidate(const Block& loop_body) {
        std::unordered_map<std::string,int> counts;
        count_pin_refs_block(loop_body, counts);
        std::string best; int best_count = 0;
        for (auto& kv : counts) {
            if (kv.second < 2) continue;
            auto lit = locals.find(kv.first);
            if (lit == locals.end()) continue;
            auto tit = local_types.find(kv.first);
            if (tit == local_types.end() || !tit->second) continue;
            const Type* t = tit->second;
            if (t->is_slice || t->array_len > 0 || !t->struct_name.empty() || t->is_float()) continue;
            if (kv.second > best_count) { best = kv.first; best_count = kv.second; }
        }
        if (best_count == 0) return std::nullopt;
        return best;
    }

    // ─────────────── budget cost (mechanical copy of CG::block_cost) ───────────────
    static int64_t cost_add(int64_t a, int64_t b) {
        const int64_t cap = std::numeric_limits<int32_t>::max();
        return a >= cap - b ? cap : a + b;
    }
    static int64_t aggregate_copy_cost(const Type* t, const StructLayoutTable* structs) {
        if (!t) return 1;
        if (t->is_slice) return 2;
        int32_t bytes = value_bytes(t, structs);
        int64_t n = (int64_t(bytes) + 7) / 8;
        return n < 1 ? 1 : n;
    }
    int64_t block_cost(const Block& b) {
        int64_t n = 0;
        for (auto& s : b.stmts) n = cost_add(n, cost_add(1, stmt_cost(*s)));
        return n < 1 ? 1 : n;
    }
    int64_t expr_cost(const Expr& ex) {
        if (dynamic_cast<const IntLit*>(&ex))     return 1;
        if (dynamic_cast<const FloatLit*>(&ex))   return 1;
        if (dynamic_cast<const BoolLit*>(&ex))    return 1;
        if (dynamic_cast<const StringLit*>(&ex))  return 1;
        if (dynamic_cast<const Ident*>(&ex))      return 1;
        if (dynamic_cast<const FnHandleExpr*>(&ex)) return 1;
        if (dynamic_cast<const SizeofExpr*>(&ex)) return 1;
        if (dynamic_cast<const OffsetofExpr*>(&ex)) return 1;
        if (dynamic_cast<const EnumAccessExpr*>(&ex)) return 1;
        if (auto* b = dynamic_cast<const BinExpr*>(&ex)) {
            int64_t n = 1;
            n = cost_add(n, expr_cost(*b->lhs));
            n = cost_add(n, expr_cost(*b->rhs));
            if (b->is_overload) n = cost_add(n, 2);
            return n;
        }
        if (auto* u = dynamic_cast<const UnaryExpr*>(&ex)) return cost_add(1, expr_cost(*u->operand));
        if (auto* c = dynamic_cast<const CastExpr*>(&ex))  return cost_add(1, expr_cost(*c->operand));
        if (auto* t = dynamic_cast<const TernaryExpr*>(&ex)) return cost_add(cost_add(cost_add(1, expr_cost(*t->cond)), expr_cost(*t->then_e)), expr_cost(*t->else_e));
        if (auto* a = dynamic_cast<const AssignExpr*>(&ex)) {
            int64_t n = cost_add(1, expr_cost(*a->value));
            if (a->target) n = cost_add(n, expr_cost(*a->target));
            return n;
        }
        if (auto* ix = dynamic_cast<const IndexExpr*>(&ex)) return cost_add(cost_add(1, expr_cost(*ix->base)), expr_cost(*ix->index));
        if (auto* fx = dynamic_cast<const FieldExpr*>(&ex)) return cost_add(1, expr_cost(*fx->base));
        if (auto* v = dynamic_cast<const ViewExpr*>(&ex))   return cost_add(1, expr_cost(*v->base));
        if (auto* sl = dynamic_cast<const StructLit*>(&ex)) {
            int64_t n = 1;
            for (auto& kv : sl->fields) n = cost_add(n, expr_cost(*kv.second));
            return n;
        }
        if (auto* al = dynamic_cast<const ArrayLit*>(&ex)) {
            int64_t n = 1;
            for (auto& el : al->elements) n = cost_add(n, expr_cost(*el));
            return n;
        }
        if (auto* c = dynamic_cast<const CallExpr*>(&ex)) {
            int64_t n = 2;
            if (c->receiver) n = cost_add(n, expr_cost(*c->receiver));
            for (auto& a : c->args) {
                n = cost_add(n, expr_cost(*a));
                n = cost_add(n, 1);
                if (a) n = cost_add(n, aggregate_copy_cost(a->ty, ctx.structs));
            }
            if (c->indirect_target) n = cost_add(n, expr_cost(*c->indirect_target));
            return n;
        }
        return 1;
    }
    int64_t stmt_cost(const Stmt& s) {
        // static_assert produces no code, so it costs zero.
        if (dynamic_cast<const StaticAssertStmt*>(&s)) return 0;
        if (auto* bs = dynamic_cast<const BlockStmt*>(&s))  return block_cost(bs->block);
        if (auto* is = dynamic_cast<const IfStmt*>(&s))
            return cost_add(cost_add(1, is->cond ? expr_cost(*is->cond) : 0),
                            cost_add(block_cost(is->then_b), is->has_else ? block_cost(is->else_b) : 0));
        if (auto* ws = dynamic_cast<const WhileStmt*>(&s))
            return cost_add(cost_add(1, ws->cond ? expr_cost(*ws->cond) : 0), block_cost(ws->body));
        if (auto* fs = dynamic_cast<const ForStmt*>(&s)) {
            int64_t n = cost_add(1, block_cost(fs->body));
            if (fs->cond) n = cost_add(n, expr_cost(*fs->cond));
            if (fs->step)  n = cost_add(n, expr_cost(*fs->step));
            if (fs->init)  n = cost_add(n, stmt_cost(*fs->init));
            return n;
        }
        if (auto* ds = dynamic_cast<const DoWhileStmt*>(&s))
            return cost_add(cost_add(1, ds->cond ? expr_cost(*ds->cond) : 0), block_cost(ds->body));
        if (auto* sw = dynamic_cast<const SwitchStmt*>(&s)) {
            int64_t n = sw->subject ? expr_cost(*sw->subject) : 0;
            n = cost_add(n, int64_t(sw->cases.size()));
            for (auto& c : sw->cases) n = cost_add(n, block_cost(c.body));
            return n;
        }
        if (auto* ls = dynamic_cast<const LetStmt*>(&s)) return ls->init ? cost_add(1, expr_cost(*ls->init)) : 1;
        if (auto* rs = dynamic_cast<const ReturnStmt*>(&s)) return rs->value ? cost_add(1, expr_cost(*rs->value)) : 1;
        if (auto* es = dynamic_cast<const ExprStmt*>(&s))   return cost_add(1, expr_cost(*es->expr));
        if (auto* ds = dynamic_cast<const DeferStmt*>(&s))  return ds->expr ? cost_add(1, expr_cost(*ds->expr)) : 1;
        return 1;
    }

    // ─────────────── globals resolution (mirrors CG) ───────────────
    const std::unordered_map<std::string, uint32_t>* gidx() const {
        return ctx.globals_index ? ctx.globals_index : (g_globals_for_codegen ? &g_globals_for_codegen->index : nullptr);
    }
    const std::unordered_map<std::string, uint32_t>* goffsets() const {
        return ctx.globals_offsets ? ctx.globals_offsets : (g_globals_for_codegen ? &g_globals_for_codegen->offsets : nullptr);
    }
    const std::unordered_map<std::string, const Type*>* gtypes() const {
        return ctx.globals_types ? ctx.globals_types : (g_globals_for_codegen ? &g_globals_for_codegen->types : nullptr);
    }
    bool resolve_global(const std::string& name, int32_t& off, const Type*& ty) const {
        const auto* idx = gidx();
        const auto* offs = goffsets();
        const auto* tys = gtypes();
        if (!idx || !tys) return false;
        off = 0; bool found = false;
        if (offs) { auto oit = offs->find(name); if (oit != offs->end()) { off = int32_t(oit->second); found = true; } }
        if (!found) { auto gi = idx->find(name); if (gi != idx->end()) { off = int32_t(gi->second) * 8; found = true; } }
        if (!found) return false;
        auto tit = tys->find(name);
        ty = (tit != tys->end()) ? tit->second : nullptr;
        return true;
    }

    // ─────────────── safety guards (mirror CG's emit_*_check, gated on flags) ───────────────
    void emit_depth_check(Loc loc) {
        if (!ctx.emit_depth_checks) return;
        ThinInstr& in = emit(ThinOp::DepthCheck, 0, 0, 0, loc);
        in.meta.trap_reason = uint8_t(TrapReason::StackOverflow);
    }
    void emit_budget_check(int64_t body_cost, Loc loc) {
        if (!ctx.emit_budget_checks || body_cost <= 0) return;
        ThinInstr& in = emit(ThinOp::BudgetCheck, 0, 0, 0, loc);
        in.imm.i = body_cost;
        in.meta.trap_reason = uint8_t(TrapReason::BudgetExceeded);
    }
    void emit_call_target_guard(Loc loc) {
        if (ctx.fn_slot_count <= 0 || ctx.fn_allowlist_base == 0) return;
        ThinInstr& in = emit(ThinOp::CallTargetGuard, 0, 0, 0, loc);
        in.meta.trap_reason = uint8_t(TrapReason::BadCallTarget);
    }
    // BoundsCheck: src1 = idx vreg, src2 = len vreg (0 if imm len), imm.i = imm len (when src2==0).
    void emit_bounds_check(VReg idx, VReg len_vreg, int64_t imm_len, Loc loc) {
        ThinInstr& in = emit(ThinOp::BoundsCheck, 0, idx, len_vreg, loc);
        if (len_vreg == 0) in.imm.i = imm_len;
        in.meta.trap_reason = uint8_t(TrapReason::BoundsCheck);
    }
    // DivOverflowCheck before signed Div/Mod: src1 = dividend, src2 = divisor.
    void emit_div_overflow_check(VReg dividend, VReg divisor, Loc loc) {
        ThinInstr& in = emit(ThinOp::DivOverflowCheck, 0, dividend, divisor, loc);
        in.meta.trap_reason = uint8_t(TrapReason::DivByZero);
    }

    // ─────────────── value materialization helpers ───────────────

    // Load a scalar/float local slot into a fresh vreg. For a slice, use load_slice_local.
    LoweredValue load_scalar_local(int32_t off, const Type* t, Loc loc) {
        if (t && t->is_slice) {
            VReg ptr = new_slice_vregs(t);
            ThinInstr& p = emit(ThinOp::LoadFrame, ptr, 0, 0, loc);
            p.meta.frame_off = off; p.meta.type = t; p.meta.width = 8;
            ThinInstr& l = emit(ThinOp::LoadFrame, ptr + 1, 0, 0, loc);
            l.meta.frame_off = off + 8; l.meta.type = &type_i64(); l.meta.width = 8;
            return { LoweredValue::Slice, ptr, 0, t };
        }
        VReg v = new_vreg(t);
        ThinInstr& in = emit(ThinOp::LoadFrame, v, 0, 0, loc);
        in.meta.frame_off = off; in.meta.type = t;
        in.meta.width = value_bytes(t, ctx.structs);
        if (t && t->is_float()) in.meta.is_f32 = (t->prim == Prim::F32) ? 1 : 0;
        return { LoweredValue::Scalar, v, 0, t };
    }

    // Store a scalar/float/slice value (described by lv) into a local slot at off.
    void store_scalar_local(const LoweredValue& lv, int32_t off, Loc loc) {
        const Type* t = lv.ty;
        if (lv.kind == LoweredValue::Slice) {
            ThinInstr& p = emit(ThinOp::StoreFrame, 0, lv.vreg, 0, loc);
            p.meta.frame_off = off; p.meta.type = t; p.meta.width = 8;
            ThinInstr& l = emit(ThinOp::StoreFrame, 0, lv.vreg + 1, 0, loc);
            l.meta.frame_off = off + 8; l.meta.type = &type_i64(); l.meta.width = 8;
            return;
        }
        ThinInstr& in = emit(ThinOp::StoreFrame, 0, lv.vreg, 0, loc);
        in.meta.frame_off = off; in.meta.type = t;
        in.meta.width = value_bytes(t, ctx.structs);
        if (t && t->is_float()) in.meta.is_f32 = (t->prim == Prim::F32) ? 1 : 0;
    }
    // Store a scalar/float value to a global slot at byte offset goff.
    void store_scalar_global(const LoweredValue& lv, int32_t goff, Loc loc) {
        const Type* t = lv.ty;
        ThinInstr& in = emit(ThinOp::StoreGlobal, 0, lv.vreg, 0, loc);
        in.meta.base_kind = AbsFixup::GlobalsBase;
        in.meta.addend = uint32_t(goff);
        in.meta.type = t;
        in.meta.width = value_bytes(t, ctx.structs);
        if (t && t->is_float()) in.meta.is_f32 = (t->prim == Prim::F32) ? 1 : 0;
    }

    // Materialize an aggregate value (StructLit/ArrayLit/aggregate Ident/call) into
    // a frame slot at `dst_off` (rbp-relative). Mirrors CG::store_value_to_memory.
    void store_value_to_frame(const Expr& value, const Type* t, int32_t dst_off, Loc loc);

    // CopyBytes from [rbp+src_off] to [rbp+dst_off], len bytes (both rbp-relative).
    void copy_frame_frame(int32_t dst_off, int32_t src_off, int32_t len, Loc loc) {
        ThinInstr& in = emit(ThinOp::CopyBytes, 0, 0, 0, loc);
        in.meta.frame_off = dst_off; in.meta.field_off = src_off; in.meta.len = len;
    }
    // CopyBytes from [globals_base+goff] to [rbp+dst_off], len bytes.
    void copy_global_frame(int32_t dst_off, int32_t goff, int32_t len, Loc loc) {
        ThinInstr& in = emit(ThinOp::CopyBytes, 0, 0, 0, loc);
        in.meta.base_kind = AbsFixup::GlobalsBase;  // src is global (dst vreg==0, src vreg==0 -> dst rbp)
        in.meta.frame_off = dst_off; in.meta.field_off = goff; in.meta.len = len;
    }
    // CopyBytes from [rbp+src_off] to [globals_base+goff], len bytes (temp -> global).
    // src1 is a sentinel (NOT a vreg): the emit decodes base_kind==GlobalsBase +
    // dst vreg==0 + src1!=0 as "the DEST is the global side" (copy_frame_global),
    // vs src1==0 "the SOURCE is the global side" (copy_global_frame). CopyBytes
    // never uses src1 as a real operand, so this is an unambiguous signal.
    void copy_frame_global(int32_t goff, int32_t src_off, int32_t len, Loc loc) {
        ThinInstr& in = emit(ThinOp::CopyBytes, 0, 1, 0, loc);
        in.meta.base_kind = AbsFixup::GlobalsBase;  // dst is global (src1=1 sentinel)
        in.meta.frame_off = goff; in.meta.field_off = src_off; in.meta.len = len;
    }
    // CopyBytes from [rbp+src_off] to [vreg-ptr+0], len bytes (return local struct through hidden ptr).
    void copy_frame_vptr(VReg dst_ptr, int32_t src_off, int32_t len, Loc loc) {
        ThinInstr& in = emit(ThinOp::CopyBytes, dst_ptr, 0, 0, loc);
        in.meta.frame_off = 0; in.meta.field_off = src_off; in.meta.len = len;
    }
    // CopyBytes from [globals_base+goff] to [vreg-ptr+0], len bytes.
    void copy_global_vptr(VReg dst_ptr, int32_t goff, int32_t len, Loc loc) {
        ThinInstr& in = emit(ThinOp::CopyBytes, dst_ptr, 0, 0, loc);
        in.meta.base_kind = AbsFixup::GlobalsBase;  // dst vreg != 0 -> src is global
        in.meta.frame_off = 0; in.meta.field_off = goff; in.meta.len = len;
    }

    // resolve a local aggregate base (Ident or FieldExpr-of-Ident) to a frame offset + type
    bool local_value_offset(const Expr& ex, int32_t& off, const Type*& ty) const {
        if (auto* id = dynamic_cast<const Ident*>(&ex)) {
            auto it = locals.find(id->name);
            if (it == locals.end()) return false;
            off = it->second;
            auto tt = local_types.find(id->name);
            ty = tt == local_types.end() ? ex.ty : tt->second;
            return true;
        }
        if (auto* fl = dynamic_cast<const FieldExpr*>(&ex)) {
            const Type* base_ty = nullptr;
            if (!local_value_offset(*fl->base, off, base_ty) || !ctx.structs ||
                !base_ty || base_ty->struct_name.empty()) return false;
            auto sit = ctx.structs->find(base_ty->struct_name);
            if (sit == ctx.structs->end()) return false;
            auto fit = sit->second.fields.find(fl->field);
            if (fit == sit->second.fields.end()) return false;
            off += fit->second.offset;
            ty = fit->second.ty;
            return true;
        }
        return false;
    }

    // ─────────────── the lowering: expressions ───────────────
    LoweredValue lower_expr(const Expr& ex);
    // lower a call expr, optionally with a hidden-dest frame offset (struct return into a
    // known slot) or a hidden-dest vreg (struct return through a runtime ptr). For non-
    // struct returns these are unused. Returns the call's LoweredValue.
    LoweredValue lower_call(const CallExpr& c, int32_t hidden_dest_off, VReg hidden_dest_vreg, Loc loc);
    // store an rvalue (lv) into an assignment target (Ident / IndexExpr / FieldExpr).
    void store_to_target(const Expr& target, const LoweredValue& lv, Loc loc);

    // ─────────────── the lowering: statements / blocks ───────────────
    void lower_block(const Block& b);
    void lower_stmt(const Stmt& s);

    // ─────────────── defer cleanup (mirrors CG) ───────────────
    bool has_active_cleanups() const {
        for (const auto& scope : cleanup_scopes)
            if (!scope.reached_sites.empty()) return true;
        return false;
    }
    void emit_defer_site(size_t index, Loc loc) {
        DeferSite& site = defer_sites[index];
        // load flag; if !=0, clear it, eval the defer expr in the saved binding env.
        VReg flag = new_vreg(&type_i64());
        ThinInstr& ld = emit(ThinOp::LoadFrame, flag, 0, 0, loc);
        ld.meta.frame_off = site.flag_offset; ld.meta.type = &type_i64(); ld.meta.width = 8;
        VReg zero = new_vreg(&type_i64());
        ThinInstr& z = emit(ThinOp::ConstInt, zero, 0, 0, loc); z.imm.i = 0; z.meta.type = &type_i64();
        VReg cond = new_vreg(&type_bool());
        ThinInstr& c = emit(ThinOp::Cmp, cond, flag, zero, loc);
        c.meta.cmp = 0; c.meta.type = &type_i64(); c.meta.width = 8;  // flag == 0 ?
        uint32_t run_bb = new_block();
        uint32_t after_bb = new_block();
        set_term_branch(cond, after_bb, run_bb);  // flag==0 -> skip(after), else run
        // run_bb: clear flag, eval defer expr in saved env
        enter_block(run_bb);
        {
            ThinInstr& clr = emit(ThinOp::StoreFrame, 0, zero, 0, loc);
            clr.meta.frame_off = site.flag_offset; clr.meta.type = &type_i64(); clr.meta.width = 8;
            auto saved_locals = locals;
            auto saved_types = local_types;
            auto saved_pin = active_pin;
            active_pin.reset();
            locals = site.locals_at_decl;
            local_types = site.types_at_decl;
            lower_expr(*site.stmt->expr);
            locals = std::move(saved_locals);
            local_types = std::move(saved_types);
            active_pin = std::move(saved_pin);
        }
        if (cur_block().term.kind == TermKind::None) set_term_jmp(after_bb);
        // skip path falls into after_bb; continue lowering there
        enter_block(after_bb);
    }
    void emit_cleanup_scope(size_t index, Loc loc) {
        const auto& reached = cleanup_scopes[index].reached_sites;
        for (auto it = reached.rbegin(); it != reached.rend(); ++it)
            emit_defer_site(*it, loc);
    }
    void emit_cleanups_to(size_t retained_depth, Loc loc) {
        for (size_t n = cleanup_scopes.size(); n > retained_depth; --n)
            emit_cleanup_scope(n - 1, loc);
    }

    // ─────────────── the entry point ───────────────
    ThinFunction run() {
        out.name = f.name;
        out.slot = f.slot;
        out.ret_type = f.ret.get();

        // Merge @obf annotations on top of ctx.obf (mirrors compile_func).
        for (auto& ann : f.annotations) {
            if (ann.name == "obf") {
                for (auto& a : ann.args) {
                    if (a == "\"mba\"")    obf.mba = true;
                    if (a == "\"opaque\"") obf.opaque = true;
                }
            }
            if (ann.name == "obf_keyed") { obf.keyed = true; }
        }

        // Obf fallback (Stage A): the transforms have no ThinOp representation.
        if (obf.mba || obf.opaque || obf.keyed) {
            out.non_serializable = true;
            out.non_serializable_reason =
                "obfuscation transforms (MBA/opaque/keyed) have no ThinOp representation; "
                "falling back to tree-walker at Stage A";
            out.blocks.clear();
            return out;
        }

        // ForEachStmt fallback: for-each is not yet lowered to ThinFunction IR
        // (it's a tree-walker-only feature for now). If the function uses for-each,
        // fall back to the tree-walker.
        // (A simple recursive check — the body is small.)
        std::function<bool(const Block&)> has_for_each = [&](const Block& b) -> bool {
            for (const auto& st : b.stmts) {
                if (dynamic_cast<const ForEachStmt*>(st.get())) return true;
                if (dynamic_cast<const MatchStmt*>(st.get())) return true;
                if (auto* is = dynamic_cast<const IfStmt*>(st.get())) {
                    if (has_for_each(is->then_b)) return true;
                    if (is->has_else && has_for_each(is->else_b)) return true;
                }
                if (auto* ws = dynamic_cast<const WhileStmt*>(st.get())) { if (has_for_each(ws->body)) return true; }
                if (auto* fs = dynamic_cast<const ForStmt*>(st.get())) { if (has_for_each(fs->body)) return true; }
                if (auto* ds = dynamic_cast<const DoWhileStmt*>(st.get())) { if (has_for_each(ds->body)) return true; }
                if (auto* sw = dynamic_cast<const SwitchStmt*>(st.get())) { for (auto& c : sw->cases) if (has_for_each(c.body)) return true; }
            }
            return false;
        };
        if (has_for_each(f.body)) {
            out.non_serializable = true;
            out.non_serializable_reason =
                "for-each or match is not yet lowered to ThinFunction IR; "
                "falling back to tree-walker";
            out.blocks.clear();
            return out;
        }

        // --- frame plan (mirror compile_func) ---
        prescan_block(f.body);

        next_local_off = 0;
        next_local_off += 8;            // rbx save slot
        rbx_save_offset = -next_local_off;  // -8

        int32_t locals_area = 8;        // rbx save
        for (size_t i = 0; i < f.params.size(); ++i)
            locals_area += local_width_bytes(f.params[i].ty.get(), ctx.structs);
        if (returns_struct_by_ptr()) locals_area += 8;

        // sum_bytes over body (LetStmt widths)
        std::function<void(const Block&)> sum_bytes = [&](const Block& b) {
            for (auto& s : b.stmts) {
                if (auto* ls = dynamic_cast<const LetStmt*>(s.get()))
                    locals_area += local_width_bytes(ls->init ? ls->init->ty : ls->ty.get(), ctx.structs);
                if (auto* is = dynamic_cast<const IfStmt*>(s.get())) { sum_bytes(is->then_b); if(is->has_else) sum_bytes(is->else_b); }
                if (auto* ws = dynamic_cast<const WhileStmt*>(s.get())) sum_bytes(ws->body);
                if (auto* ds = dynamic_cast<const DoWhileStmt*>(s.get())) sum_bytes(ds->body);
                if (auto* bs = dynamic_cast<const BlockStmt*>(s.get())) sum_bytes(bs->block);
                if (auto* fs = dynamic_cast<const ForStmt*>(s.get())) {
                    if (fs->init) locals_area += local_width_bytes(fs->init->init ? fs->init->init->ty : fs->init->ty.get(), ctx.structs);
                    sum_bytes(fs->body);
                }
                if (auto* sw = dynamic_cast<const SwitchStmt*>(s.get()))
                    for (auto& c : sw->cases) sum_bytes(c.body);
            }
        };
        sum_bytes(f.body);

        int32_t struct_temp_bytes = 0; count_struct_temps_block(f.body, struct_temp_bytes); locals_area += struct_temp_bytes;
        int32_t arr_temp_bytes = 0;    count_arr_temps_block(f.body, arr_temp_bytes);       locals_area += arr_temp_bytes;
        int32_t str_temp_bytes = 0;    count_str_temps_block(f.body, str_temp_bytes);       locals_area += str_temp_bytes;
        // Short-circuit (&&/||) result temps: the IR lowering materializes the
        // 0/1 result into a FRAME-BACKED vreg (defined in the false/true blocks,
        // joined in the end block). A join-block vreg MUST be frame-backed (the
        // emit's in-rax model is unsound across a join when an intervening instr
        // clobbers rax), so the lowering allocates a bool temp per &&/||. The
        // tree-walker needs no such temp (it keeps the result in rax inline), so
        // this is an IR-path-only 8 bytes per &&/|| — value-equivalent, not
        // byte-identical (the Stage-A contract).
        int32_t logical_temp_bytes = 0; count_logical_temps_block(f.body, logical_temp_bytes); locals_area += logical_temp_bytes;

        collect_defers(f.body);
        locals_area += int32_t(defer_sites.size()) * 8;

        int32_t arg_temps_area = max_args * 8;
        arg_temps_base = -(locals_area + 8);
        int32_t total = locals_area + arg_temps_area + 16;
        frame_size = round16(total);

        // --- param-spill plan (mirror compile_func) ---
        int32_t total_words = returns_struct_by_ptr() ? 1 : 0;
        for (auto& p : f.params) total_words += words_for_type(p.ty.get(), ctx.structs);

        if (returns_struct_by_ptr()) {
            next_local_off += 8;
            struct_ret_ptr_offset = -next_local_off;
            ThinFramePlan::ParamSpill ps;
            ps.name = "__struct_ret_ptr";
            ps.ty = nullptr;
            ps.off = struct_ret_ptr_offset;
            ps.word0 = 0; ps.nwords = 1;
            out.frame.params.push_back(std::move(ps));
        }
        int32_t param_word = returns_struct_by_ptr() ? 1 : 0;
        for (size_t i = 0; i < f.params.size(); ++i) {
            const Type* pt = f.params[i].ty.get();
            int wcount = words_for_type(pt, ctx.structs);
            bool is_struct = is_registered_struct_ty(pt);
            int32_t off = alloc_local(f.params[i].name, pt);
            ThinFramePlan::ParamSpill ps;
            ps.name = f.params[i].name; ps.ty = pt; ps.off = off;
            ps.word0 = param_word; ps.nwords = wcount;
            if (is_struct) {
                int32_t struct_bytes = struct_size(pt);
                int32_t byte_pos = 0;
                for (int w = 0; w < wcount; ++w) {
                    int32_t word_bytes = std::min<int32_t>(8, struct_bytes - byte_pos);
                    (void)word_bytes;  // c3 reads nwords + the struct type to trim the last word
                    byte_pos += std::min<int32_t>(8, struct_bytes - byte_pos);
                }
            }
            out.frame.params.push_back(std::move(ps));
            param_word += wcount;
        }
        // defer flag slots
        for (auto& site : defer_sites) {
            next_local_off += 8;
            site.flag_offset = -next_local_off;
        }

        out.frame.frame_size = frame_size;
        out.frame.rbx_save_offset = rbx_save_offset;
        out.frame.struct_ret_ptr_offset = returns_struct_by_ptr() ? struct_ret_ptr_offset : 0;
        out.frame.arg_temps_base = arg_temps_base;
        out.frame.next_local_off = next_local_off;  // body lowering continues from here
        out.frame.returns_struct_by_ptr = returns_struct_by_ptr();

        // --- entry block: budget check at function entry (gated) ---
        new_block();  // blocks[0] = entry
        enter_block(0);
        emit_budget_check(block_cost(f.body), f.loc);

        // --- lower the body ---
        lower_block(f.body);

        // Implicit void return if the current block falls through.
        if (cur_block().term.kind == TermKind::None) {
            // run remaining cleanups through depth 0, then return void
            emit_cleanups_to(0, f.loc);
            set_term_return(0);
        }

        // ── Post-lowering vreg spill pass (the IR-emit correctness fix) ──
        // The emit's vreg-materialization model is "a VReg is either frame-backed
        // (reloadable from its frame slot) or the current rax_vreg". A producing
        // instr that leaves its dst in rax but does NOT spill to a frame slot is
        // only reloadable while rax_vreg still points at it — once any later instr
        // (e.g. the LHS load of an outer BinExpr, or the `0` literal of an
        // if-compare) clobbers rax, the dst is GONE and load_int_vreg's best-effort
        // path silently reuses the stale rax (a wrong value). This breaks nested
        // expressions (`a + b * c`), if/while conditions on computed bools, and
        // short-circuit joins.
        //
        // Fix: give every PLAIN scalar/float intermediate-result vreg a frame
        // slot (8 bytes) so it is always reloadable. We walk the lowered blocks
        // and, for each producing instr whose dst is a plain scalar/float (NOT a
        // slice — slice vregs use dst+1 and 16-byte slots handled at their own
        // emit sites; NOT a struct — structs are frame slots not vregs; NOT
        // already frame-backed — LoadFrame/ConstInt-with-frame_off/etc. keep
        // their existing slot), assign a fresh 8-byte frame slot in meta.frame_off.
        // Then recompute frame_size so the prologue reserves room for the spill
        // slots. This makes the emit spill every intermediate result and reload
        // it on use — value-equivalent (the tree-walker computes the same value
        // inline in rax; spilling+reloading just makes it durable across rax
        // clobbers). NOT byte-identical (the Stage-A contract is value-equiv).
        //
        // Scope is deliberately conservative: only plain scalar/float results
        // (ConstInt/ConstFloat/ConstBool, Move, int arith, float arith, Cmp,
        // Cast, scalar-returning calls). Slice/struct/address producers keep
        // their existing frame handling (slice dsts use dst+1; addresses are
        // consumed immediately by a following load/store). Spill slots go BELOW
        // the arg-temps area (which sits at -(locals_area+8) ..
        // -(locals_area+8+arg_temps_area)), so they never collide with a named
        // local, a temp, or an arg temp. Base = locals_area + arg_temps_area;
        // each spill slot grows downward from there.
        int32_t spill_base = locals_area + arg_temps_area;
        int32_t spill_top = spill_base;
        // Assign spill slots per-VREG (not per-instr): a vreg defined in multiple
        // blocks (a join — ternary/short-circuit result) MUST share one frame
        // slot across all its defs, or load_int_vreg would read the wrong slot at
        // the join. Map each dst vreg to its slot the first time we see it; reuse
        // the same slot for every subsequent def of that vreg.
        std::unordered_map<VReg, int32_t> vreg_spill_slot;
        auto is_plain_scalar_dst = [&](const ThinInstr& in) -> bool {
            if (in.dst == 0) return false;
            if (in.meta.frame_off != 0) return false;  // already frame-backed
            switch (in.op) {
            case ThinOp::ConstInt: case ThinOp::ConstFloat: case ThinOp::ConstBool:
            case ThinOp::Move:
            case ThinOp::Add: case ThinOp::Sub: case ThinOp::Mul: case ThinOp::Div:
            case ThinOp::Mod: case ThinOp::And: case ThinOp::Or: case ThinOp::Xor:
            case ThinOp::Shl: case ThinOp::Shr: case ThinOp::Neg: case ThinOp::Not:
            case ThinOp::BitNot:
            case ThinOp::FAdd: case ThinOp::FSub: case ThinOp::FMul: case ThinOp::FDiv:
            case ThinOp::FMod:
            case ThinOp::Cmp: case ThinOp::Cast:
                // exclude slice-typed results (rare for these ops, but guard):
                if (in.meta.type && in.meta.type->is_slice) return false;
                return true;
            case ThinOp::CallNative: case ThinOp::CallScript:
            case ThinOp::CallIndirect: case ThinOp::CallCrossModule:
                // only scalar/float returns (not slice/struct/void)
                if (in.ret_type && !in.ret_type->is_slice &&
                    in.ret_type->struct_name.empty() && !in.ret_type->is_void())
                    return true;
                return false;
            default:
                return false;
            }
        };
        for (auto& blk : out.blocks) {
            for (auto& in : blk.instrs) {
                if (is_plain_scalar_dst(in)) {
                    auto it = vreg_spill_slot.find(in.dst);
                    int32_t slot;
                    if (it == vreg_spill_slot.end()) {
                        spill_top += 8;
                        slot = -spill_top;
                        vreg_spill_slot[in.dst] = slot;
                    } else {
                        slot = it->second;  // reuse the same slot for this vreg
                    }
                    in.meta.frame_off = slot;
                }
            }
        }
        if (spill_top > spill_base) {
            int32_t total = spill_top + 16;
            frame_size = round16(total);
            out.frame.frame_size = frame_size;
            out.frame.next_local_off = spill_top;
        }

        out.non_serializable = non_serializable;
        out.non_serializable_reason = non_serializable_reason;
        return out;
    }
};

// ─────────────── store_value_to_frame (mirrors CG::store_value_to_memory) ───────────────
void ThinLowerer::store_value_to_frame(const Expr& value, const Type* t, int32_t dst_off, Loc loc) {
    if (t && !t->struct_name.empty() && ctx.structs && ctx.structs->count(t->struct_name)) {
        const StructLayout& layout = ctx.structs->at(t->struct_name);
        if (auto* lit = dynamic_cast<const StructLit*>(&value)) {
            for (const auto& kv : lit->fields) {
                auto fit = layout.fields.find(kv.first);
                if (fit != layout.fields.end())
                    store_value_to_frame(*kv.second, fit->second.ty, dst_off + fit->second.offset, kv.second->loc);
            }
            return;
        }
        int32_t src_off = 0; const Type* src_ty = nullptr;
        if (local_value_offset(value, src_off, src_ty))
            copy_frame_frame(dst_off, src_off, layout.size, loc);
        else {
            // global struct source: copy from globals_base + goff
            if (auto* id = dynamic_cast<const Ident*>(&value)) {
                int32_t goff = 0; const Type* gty = nullptr;
                if (resolve_global(id->name, goff, gty))
                    copy_global_frame(dst_off, goff, layout.size, loc);
            }
        }
        return;
    }
    if (t && t->array_len > 0) {
        int32_t src_off = 0; const Type* src_ty = nullptr;
        if (local_value_offset(value, src_off, src_ty))
            copy_frame_frame(dst_off, src_off, value_bytes(t, ctx.structs), loc);
        else if (auto* id = dynamic_cast<const Ident*>(&value)) {
            int32_t goff = 0; const Type* gty = nullptr;
            if (resolve_global(id->name, goff, gty))
                copy_global_frame(dst_off, goff, value_bytes(t, ctx.structs), loc);
        }
        return;
    }
    LoweredValue lv = lower_expr(value);
    if (t && t->is_slice) {
        // slice store: ptr at off, len at off+8
        ThinInstr& p = emit(ThinOp::StoreFrame, 0, lv.vreg, 0, loc);
        p.meta.frame_off = dst_off; p.meta.type = t; p.meta.width = 8;
        ThinInstr& l = emit(ThinOp::StoreFrame, 0, lv.vreg + 1, 0, loc);
        l.meta.frame_off = dst_off + 8; l.meta.type = &type_i64(); l.meta.width = 8;
    } else {
        store_scalar_local(lv, dst_off, loc);
    }
}

// ─────────────── lower_expr (mirrors CG::eval) ───────────────
LoweredValue ThinLowerer::lower_expr(const Expr& ex) {
    const Loc loc = ex.loc;

    if (auto* lit = dynamic_cast<const IntLit*>(&ex)) {
        VReg v = new_vreg(ex.ty);
        ThinInstr& in = emit(ThinOp::ConstInt, v, 0, 0, loc);
        in.imm.i = lit->v; in.meta.type = ex.ty;
        in.meta.width = value_bytes(ex.ty, ctx.structs);
        return { LoweredValue::Scalar, v, 0, ex.ty };
    }
    if (auto* lit = dynamic_cast<const FloatLit*>(&ex)) {
        VReg v = new_vreg(ex.ty);
        ThinInstr& in = emit(ThinOp::ConstFloat, v, 0, 0, loc);
        in.imm.f = lit->v; in.meta.type = ex.ty;
        in.meta.is_f32 = (ex.ty && ex.ty->prim == Prim::F32) ? 1 : (lit->is_f32 ? 1 : 0);
        in.meta.width = (in.meta.is_f32 ? 4 : 8);
        return { LoweredValue::Scalar, v, 0, ex.ty };
    }
    if (auto* lit = dynamic_cast<const BoolLit*>(&ex)) {
        VReg v = new_vreg(ex.ty ? ex.ty : &type_bool());
        ThinInstr& in = emit(ThinOp::ConstBool, v, 0, 0, loc);
        in.imm.i = lit->v ? 1 : 0; in.meta.type = ex.ty ? ex.ty : &type_bool();
        in.meta.width = 1;
        return { LoweredValue::Scalar, v, 0, ex.ty ? ex.ty : &type_bool() };
    }
    if (auto* lit = dynamic_cast<const StringLit*>(&ex)) {
        const uint32_t string_off = append_rodata(lit->baked_ptr, size_t(lit->baked_len));
        VReg ptr, len;
        if (lit->encrypted && lit->baked_key != 0) {
            const int32_t slot_off = alloc_str_temp(lit->baked_len);
            ptr = new_slice_vregs(ex.ty); len = ptr + 1;
            ThinInstr& dec = emit(ThinOp::StringDecrypt, ptr, 0, 0, loc);
            dec.meta.frame_off = slot_off;
            dec.meta.addend = string_off;
            dec.meta.base_kind = AbsFixup::FunctionRodataBase;
            dec.meta.len = int32_t(lit->baked_len);
            dec.imm.i = int64_t(lit->baked_key);  // the XOR key
            dec.meta.type = ex.ty;
            ThinInstr& l = emit(ThinOp::ConstInt, len, 0, 0, loc);
            l.imm.i = lit->baked_len; l.meta.type = &type_i64(); l.meta.width = 8;
        } else {
            ptr = new_slice_vregs(ex.ty); len = ptr + 1;
            ThinInstr& p = emit(ThinOp::ConstStringRef, ptr, 0, 0, loc);
            p.meta.addend = string_off; p.meta.base_kind = AbsFixup::FunctionRodataBase;
            p.meta.len = int32_t(lit->baked_len); p.meta.type = ex.ty;
            ThinInstr& l = emit(ThinOp::ConstInt, len, 0, 0, loc);
            l.imm.i = lit->baked_len; l.meta.type = &type_i64(); l.meta.width = 8;
        }
        // implicit conversion to a `string` handle: chained CallNative(ptr, len) -> i64.
        if (lit->implicit_to_string && lit->to_string_native_fn) {
            const Type* ret_ty = ex.ty ? ex.ty : &type_i64();
            VReg res = new_vreg(ret_ty);
            ThinInstr in;
            in.op = ThinOp::CallNative;
            in.loc = loc;
            in.dst = res;
            in.args.push_back(ptr);
            in.args.push_back(len);
            in.arg_frame_offs.push_back(-1);
            in.arg_frame_offs.push_back(-1);
            in.arg_types.push_back(&type_i64());   // ptr (carried as i64)
            in.arg_types.push_back(&type_i64());   // len
            in.meta.native_name = lit->to_string_native_name;
            in.native_fn = lit->to_string_native_fn;
            in.ret_type = ret_ty;
            in.meta.type = ret_ty; in.meta.width = value_bytes(ret_ty, ctx.structs);
            emit_depth_check(loc);
            cur_block().instrs.push_back(std::move(in));
            return { LoweredValue::Scalar, res, 0, ret_ty };
        }
        return { LoweredValue::Slice, ptr, 0, ex.ty };
    }
    if (auto* id = dynamic_cast<const Ident*>(&ex)) {
        // Item E pin fast path: read from the pin slot (value-equivalent: the slot
        // is always write-through synced, so a LoadFrame from the pin offset is
        // identical to the tree-walker's register read).
        if (active_pin && active_pin->name == id->name) {
            const Type* t = local_types.count(id->name) ? local_types.at(id->name) : ex.ty;
            return load_scalar_local(active_pin->offset, t, loc);
        }
        auto it = locals.find(id->name);
        if (it != locals.end()) {
            const Type* t = local_types.count(id->name) ? local_types.at(id->name) : ex.ty;
            return load_scalar_local(it->second, t, loc);
        }
        // global
        int32_t goff = 0; const Type* gt = nullptr;
        if (resolve_global(id->name, goff, gt)) {
            if (gt && gt->is_slice) {
                VReg ptr = new_slice_vregs(gt); VReg len = ptr + 1;
                ThinInstr& p = emit(ThinOp::LoadGlobal, ptr, 0, 0, loc);
                p.meta.base_kind = AbsFixup::GlobalsBase; p.meta.addend = uint32_t(goff);
                p.meta.type = gt; p.meta.width = 8;  // slice-global ptr: relative -> absolute (c3 adds base)
                ThinInstr& l = emit(ThinOp::LoadGlobal, len, 0, 0, loc);
                l.meta.base_kind = AbsFixup::GlobalsBase; l.meta.addend = uint32_t(goff + 8);
                l.meta.type = &type_i64(); l.meta.width = 8;
                return { LoweredValue::Slice, ptr, 0, gt };
            }
            VReg v = new_vreg(gt);
            ThinInstr& in = emit(ThinOp::LoadGlobal, v, 0, 0, loc);
            in.meta.base_kind = AbsFixup::GlobalsBase; in.meta.addend = uint32_t(goff);
            in.meta.type = gt; in.meta.width = value_bytes(gt, ctx.structs);
            if (gt && gt->is_float()) in.meta.is_f32 = (gt->prim == Prim::F32) ? 1 : 0;
            return { LoweredValue::Scalar, v, 0, gt };
        }
        // unresolved ident: produce nothing (mirrors tree-walker fall-through)
        return { LoweredValue::Scalar, 0, 0, ex.ty };
    }
    if (auto* b = dynamic_cast<const BinExpr*>(&ex)) {
        // operator-overload dispatch -> CallNative
        if (b->is_overload && b->overload_fn) {
            LoweredValue lhs = lower_expr(*b->lhs);
            LoweredValue rhs = lower_expr(*b->rhs);
            ThinInstr in;
            in.op = ThinOp::CallNative;
            in.loc = loc;
            const Type* ret_ty = &b->overload_ret;
            in.ret_type = ret_ty;
            // result vreg up front
            if (ret_ty && ret_ty->is_slice) {
                in.dst = new_slice_vregs(ret_ty); in.meta.type = ret_ty;
            } else if (ret_ty && is_registered_struct_ty(ret_ty)) {
                // struct return via hidden ptr: dest = temp (word-0 sentinel)
                int32_t temp_off = alloc_struct_temp(ret_ty);
                in.args.push_back(0);
                in.arg_frame_offs.push_back(temp_off);
                in.arg_types.push_back(ret_ty);
                in.dst = 0; in.meta.type = ret_ty;
            } else if (!ret_ty || ret_ty->is_void()) {
                in.dst = 0;
            } else {
                in.dst = new_vreg(ret_ty); in.meta.type = ret_ty; in.meta.width = value_bytes(ret_ty, ctx.structs);
                if (ret_ty->is_float()) in.meta.is_f32 = (ret_ty->prim == Prim::F32) ? 1 : 0;
            }
            // args: lhs, rhs (struct operands are bare Idents -> struct-by-value via frame sentinel)
            auto add_arg = [&](const LoweredValue& lv, const Type* aty) {
                if (lv.kind == LoweredValue::Aggregate) {
                    in.args.push_back(0);
                    in.arg_frame_offs.push_back(lv.frame_off);
                    in.arg_types.push_back(aty);
                } else if (lv.kind == LoweredValue::Slice) {
                    in.args.push_back(lv.vreg); in.arg_frame_offs.push_back(-1); in.arg_types.push_back(aty);
                    in.args.push_back(lv.vreg + 1); in.arg_frame_offs.push_back(-1); in.arg_types.push_back(&type_i64());
                } else {
                    in.args.push_back(lv.vreg); in.arg_frame_offs.push_back(-1); in.arg_types.push_back(aty);
                }
            };
            add_arg(lhs, b->lhs->ty);
            add_arg(rhs, b->rhs->ty);
            in.meta.native_name = b->overload_name;
            in.native_fn = b->overload_fn;
            emit_depth_check(loc);
            cur_block().instrs.push_back(std::move(in));
            if (ret_ty && ret_ty->is_slice)         return { LoweredValue::Slice, in.dst, 0, ret_ty };
            if (ret_ty && is_registered_struct_ty(ret_ty)) return { LoweredValue::Aggregate, 0, in.arg_frame_offs[0], ret_ty };
            if (!ret_ty || ret_ty->is_void())       return { LoweredValue::Scalar, 0, 0, ret_ty };
            return { LoweredValue::Scalar, in.dst, 0, ret_ty };
        }
        const Type* lt = b->lhs->ty;
        bool is_cmp = (b->op >= BinExpr::Op::Eq && b->op <= BinExpr::Op::Ge);
        // const fold: IntLit+IntLit for Add/Sub/Mul/And/Or/Xor/Shl/Shr
        if (auto* li = dynamic_cast<const IntLit*>(b->lhs.get())) {
            if (auto* ri = dynamic_cast<const IntLit*>(b->rhs.get())) {
                bool folded = true; int64_t result = 0;
                switch (b->op) {
                case BinExpr::Op::Add: result = bit_cast_i64(uint64_t(li->v) + uint64_t(ri->v)); break;
                case BinExpr::Op::Sub: result = bit_cast_i64(uint64_t(li->v) - uint64_t(ri->v)); break;
                case BinExpr::Op::Mul: result = bit_cast_i64(uint64_t(li->v) * uint64_t(ri->v)); break;
                case BinExpr::Op::And: result = li->v & ri->v; break;
                case BinExpr::Op::Or:  result = li->v | ri->v; break;
                case BinExpr::Op::Xor: result = li->v ^ ri->v; break;
                case BinExpr::Op::Shl: result = bit_cast_i64(uint64_t(li->v) << (ri->v & 63)); break;
                case BinExpr::Op::Shr: {
                    int sh = int(ri->v & 63);
                    uint64_t ur = uint64_t(li->v) >> sh;
                    if (sh != 0 && li->v < 0) ur |= ~((1ULL << (64 - sh)) - 1);
                    result = bit_cast_i64(ur);
                    break;
                }
                default: folded = false; break;
                }
                if (folded) {
                    VReg v = new_vreg(lt);
                    ThinInstr& in = emit(ThinOp::ConstInt, v, 0, 0, loc);
                    in.imm.i = result; in.meta.type = lt;
                    in.meta.width = value_bytes(lt, ctx.structs);
                    if (!is_cmp) {} // normalize width is encoded in meta.width
                    return { LoweredValue::Scalar, v, 0, lt };
                }
            }
        }
        bool is_logical = (b->op == BinExpr::Op::LAnd || b->op == BinExpr::Op::LOr);
        bool is_float = lt && lt->is_float();

        if (is_logical) {
            // short-circuit via Branch (mirrors the tree-walker), result 0/1 in a shared vreg.
            LoweredValue lhs = lower_expr(*b->lhs);
            VReg zero = new_vreg(lt);
            ThinInstr& z = emit(ThinOp::ConstInt, zero, 0, 0, loc); z.imm.i = 0; z.meta.type = lt; z.meta.width = value_bytes(lt, ctx.structs);
            VReg cond = new_vreg(&type_bool());
            ThinInstr& c = emit(ThinOp::Cmp, cond, lhs.vreg, zero, loc);
            c.meta.cmp = 0; c.meta.type = lt; c.meta.width = value_bytes(lt, ctx.structs);
            c.meta.is_unsigned = (lt && lt->is_uint()) ? 1 : 0;  // cond = (lhs == 0)
            uint32_t rhs_bb = new_block();
            uint32_t false_bb = new_block();
            uint32_t true_bb = new_block();
            uint32_t end_bb = new_block();
            // LAnd: lhs==0 -> false; else rhs. LOr: lhs==0 -> rhs; else true.
            if (b->op == BinExpr::Op::LAnd) set_term_branch(cond, false_bb, rhs_bb);
            else                             set_term_branch(cond, rhs_bb, true_bb);
            VReg res = new_vreg(&type_bool());
            // The result vreg `res` is defined in BOTH false_bb (=0) and true_bb
            // (=1) and consumed in the join block end_bb. A join-block vreg MUST
            // be frame-backed: the emit's in-rax model is unsound across a join
            // (an intervening instr in end_bb — e.g. the `0` literal for an
            // `if (res)` compare — clobbers rax before res is reloaded, and with
            // no frame slot load_int_vreg can only best-effort trust rax). So
            // spill res to a dedicated bool frame temp (counted in the frame plan
            // by count_logical_temps_block) and set meta.frame_off on both defs;
            // the emit stores each def to the slot and load_int_vreg reloads it.
            int32_t res_off = alloc_struct_temp(&type_bool());
            // rhs_bb: lower rhs; cond2 = (rhs==0). Both: eq0 -> false, else true.
            enter_block(rhs_bb);
            {
                LoweredValue rhs = lower_expr(*b->rhs);
                VReg zero2 = new_vreg(lt);
                ThinInstr& z2 = emit(ThinOp::ConstInt, zero2, 0, 0, loc); z2.imm.i = 0; z2.meta.type = lt; z2.meta.width = value_bytes(lt, ctx.structs);
                VReg cond2 = new_vreg(&type_bool());
                ThinInstr& c2 = emit(ThinOp::Cmp, cond2, rhs.vreg, zero2, loc);
                c2.meta.cmp = 0; c2.meta.type = lt; c2.meta.width = value_bytes(lt, ctx.structs);
                c2.meta.is_unsigned = (lt && lt->is_uint()) ? 1 : 0;
                set_term_branch(cond2, false_bb, true_bb);
            }
            // false_bb: res = 0 -> end  (frame-backed: store to res_off)
            enter_block(false_bb);
            { ThinInstr& ci = emit(ThinOp::ConstInt, res, 0, 0, loc); ci.imm.i = 0; ci.meta.type = &type_bool(); ci.meta.width = 1; ci.meta.frame_off = res_off; }
            set_term_jmp(end_bb);
            // true_bb: res = 1 -> end  (frame-backed: store to res_off)
            enter_block(true_bb);
            { ThinInstr& ci = emit(ThinOp::ConstInt, res, 0, 0, loc); ci.imm.i = 1; ci.meta.type = &type_bool(); ci.meta.width = 1; ci.meta.frame_off = res_off; }
            set_term_jmp(end_bb);
            // end_bb: continue (res is frame-backed; load_int_vreg reloads it)
            enter_block(end_bb);
            return { LoweredValue::Scalar, res, 0, &type_bool() };
        }

        if (is_float) {
            bool f64 = lt->prim == Prim::F64;
            LoweredValue lhs = lower_expr(*b->lhs);
            LoweredValue rhs = lower_expr(*b->rhs);
            if (is_cmp) {
                VReg res = new_vreg(&type_bool());
                ThinInstr& in = emit(ThinOp::Cmp, res, lhs.vreg, rhs.vreg, loc);
                in.meta.cmp = uint8_t(int(b->op) - int(BinExpr::Op::Eq));
                in.meta.type = lt; in.meta.is_f32 = f64 ? 0 : 1; in.meta.width = f64 ? 8 : 4;
                return { LoweredValue::Scalar, res, 0, &type_bool() };
            }
            ThinOp op = ThinOp::FAdd;
            switch (b->op) {
            case BinExpr::Op::Add: op = ThinOp::FAdd; break;
            case BinExpr::Op::Sub: op = ThinOp::FSub; break;
            case BinExpr::Op::Mul: op = ThinOp::FMul; break;
            case BinExpr::Op::Div: op = ThinOp::FDiv; break;
            case BinExpr::Op::Mod: op = ThinOp::FMod; break;
            default: break;
            }
            VReg res = new_vreg(lt);
            ThinInstr& in = emit(op, res, lhs.vreg, rhs.vreg, loc);
            in.meta.type = lt; in.meta.is_f32 = f64 ? 0 : 1; in.meta.width = f64 ? 8 : 4;
            return { LoweredValue::Scalar, res, 0, lt };
        }

        // Integer path
        // If the RHS is an IntLit, use the IMMEDIATE form (src2=0 + imm.i) —
        // the emit's int binop + cmp both have an imm-form path that bakes the
        // literal directly into the op (op rax, imm32 / cmp rax, imm32), avoiding
        // a separate ConstInt vreg for the RHS. This matters for the emit's
        // vreg-materialization model: a ConstInt RHS vreg is NOT frame-backed, so
        // reloading it after the LHS load clobbers rax would fail (load_int_vreg's
        // best-effort path would reuse the stale LHS in rax). The imm form sidesteps
        // the reload entirely. (The general intermediate-result spill is a
        // separate concern; this fix covers the common literal-operand case.)
        auto* rhs_lit = dynamic_cast<const IntLit*>(b->rhs.get());
        bool rhs_is_imm = rhs_lit && !is_float;
        LoweredValue lhs = lower_expr(*b->lhs);
        LoweredValue rhs = rhs_is_imm ? LoweredValue{} : lower_expr(*b->rhs);
        if (is_cmp) {
            VReg res = new_vreg(&type_bool());
            ThinInstr& in = emit(ThinOp::Cmp, res, lhs.vreg, rhs_is_imm ? 0 : rhs.vreg, loc);
            if (rhs_is_imm) in.imm.i = rhs_lit->v;
            in.meta.cmp = uint8_t(int(b->op) - int(BinExpr::Op::Eq));
            in.meta.type = lt; in.meta.width = value_bytes(lt, ctx.structs);
            in.meta.is_unsigned = (lt && lt->is_uint()) ? 1 : 0;
            return { LoweredValue::Scalar, res, 0, &type_bool() };
        }
        bool is_div = (b->op == BinExpr::Op::Div || b->op == BinExpr::Op::Mod);
        bool is_unsigned = lt && lt->is_uint();
        if (is_div && !is_unsigned) {
            // signed div/mod: overflow check first (mirrors emit_integer_divmod)
            emit_div_overflow_check(lhs.vreg, rhs_is_imm ? 0 : rhs.vreg, loc);
        }
        ThinOp op = ThinOp::Add;
        switch (b->op) {
        case BinExpr::Op::Add: op = ThinOp::Add; break;
        case BinExpr::Op::Sub: op = ThinOp::Sub; break;
        case BinExpr::Op::Mul: op = ThinOp::Mul; break;
        case BinExpr::Op::Div: op = ThinOp::Div; break;
        case BinExpr::Op::Mod: op = ThinOp::Mod; break;
        case BinExpr::Op::And: op = ThinOp::And; break;
        case BinExpr::Op::Or:  op = ThinOp::Or;  break;
        case BinExpr::Op::Xor: op = ThinOp::Xor; break;
        case BinExpr::Op::Shl: op = ThinOp::Shl; break;
        case BinExpr::Op::Shr: op = ThinOp::Shr; break;
        default: break;
        }
        VReg res = new_vreg(lt);
        ThinInstr& in = emit(op, res, lhs.vreg, rhs_is_imm ? 0 : rhs.vreg, loc);
        if (rhs_is_imm) in.imm.i = rhs_lit->v;
        in.meta.type = lt; in.meta.width = value_bytes(lt, ctx.structs);
        in.meta.is_unsigned = is_unsigned ? 1 : 0;
        return { LoweredValue::Scalar, res, 0, lt };
    }
    if (auto* h = dynamic_cast<const FnHandleExpr*>(&ex)) {
        VReg v = new_vreg(ex.ty ? ex.ty : &type_i64());
        ThinInstr& in = emit(ThinOp::ConstInt, v, 0, 0, loc);
        in.imm.i = int64_t(h->slot); in.meta.type = ex.ty ? ex.ty : &type_i64();
        in.meta.width = 8;
        return { LoweredValue::Scalar, v, 0, ex.ty ? ex.ty : &type_i64() };
    }
    if (auto* u = dynamic_cast<const UnaryExpr*>(&ex)) {
        LoweredValue operand = lower_expr(*u->operand);
        const Type* ot = u->operand->ty;
        if (u->op == UnaryExpr::Op::Not) {
            VReg res = new_vreg(&type_bool());
            ThinInstr& in = emit(ThinOp::Not, res, operand.vreg, 0, loc);
            in.meta.type = &type_bool(); in.meta.width = 1;
            return { LoweredValue::Scalar, res, 0, &type_bool() };
        }
        if (u->op == UnaryExpr::Op::Neg) {
            if (ot && ot->is_float()) {
                // 0.0 - operand (value-equivalent; see thin_lower.hpp note on -0.0).
                bool f64 = ot->prim == Prim::F64;
                VReg zero = new_vreg(ot);
                ThinInstr& z = emit(ThinOp::ConstFloat, zero, 0, 0, loc);
                z.imm.f = 0.0; z.meta.type = ot; z.meta.is_f32 = f64 ? 0 : 1; z.meta.width = f64 ? 8 : 4;
                VReg res = new_vreg(ot);
                ThinInstr& in = emit(ThinOp::FSub, res, zero, operand.vreg, loc);
                in.meta.type = ot; in.meta.is_f32 = f64 ? 0 : 1; in.meta.width = f64 ? 8 : 4;
                return { LoweredValue::Scalar, res, 0, ot };
            }
            VReg res = new_vreg(ot);
            ThinInstr& in = emit(ThinOp::Neg, res, operand.vreg, 0, loc);
            in.meta.type = ot; in.meta.width = value_bytes(ot, ctx.structs);
            return { LoweredValue::Scalar, res, 0, ot };
        }
        // BitNot
        VReg res = new_vreg(ot);
        ThinInstr& in = emit(ThinOp::BitNot, res, operand.vreg, 0, loc);
        in.meta.type = ot; in.meta.width = value_bytes(ot, ctx.structs);
        return { LoweredValue::Scalar, res, 0, ot };
    }
    if (auto* c = dynamic_cast<const CastExpr*>(&ex)) {
        const Type* from = c->operand->ty;
        const Type* to = c->to.get();
        const bool plain_from_int = from && from->is_int() && !from->is_fn_handle && from->struct_name.empty();
        const bool plain_to_int = to && to->is_int() && !to->is_fn_handle && to->struct_name.empty();
        const bool by_value_aggregate = from && (from->array_len > 0 ||
            (!from->struct_name.empty() && ctx.structs && ctx.structs->count(from->struct_name) != 0));
        LoweredValue operand = lower_expr(*c->operand);
        if (from && to && from->same(*to) && !by_value_aggregate) {
            return operand;  // same-type scalar/slice/handle no-op
        }
        if (plain_from_int && plain_to_int) {
            VReg res = new_vreg(to);
            ThinInstr& in = emit(ThinOp::Cast, res, operand.vreg, 0, loc);
            in.meta.type = to; in.meta.width = value_bytes(to, ctx.structs);
            return { LoweredValue::Scalar, res, 0, to };
        }
        if (from && to && from->is_float() && to->is_float()) {
            VReg res = new_vreg(to);
            ThinInstr& in = emit(ThinOp::Cast, res, operand.vreg, 0, loc);
            in.meta.type = to; in.meta.is_f32 = (to->prim == Prim::F32) ? 1 : 0; in.meta.width = (to->prim == Prim::F32) ? 4 : 8;
            return { LoweredValue::Scalar, res, 0, to };
        }
        if (plain_from_int && !from->is_uint() && to && to->is_float()) {
            VReg res = new_vreg(to);
            ThinInstr& in = emit(ThinOp::Cast, res, operand.vreg, 0, loc);
            in.meta.type = to; in.meta.is_f32 = (to->prim == Prim::F32) ? 1 : 0; in.meta.width = (to->prim == Prim::F32) ? 4 : 8;
            return { LoweredValue::Scalar, res, 0, to };
        }
        if (from && from->is_float() && plain_to_int && !to->is_uint()) {
            VReg res = new_vreg(to);
            ThinInstr& in = emit(ThinOp::Cast, res, operand.vreg, 0, loc);
            in.meta.type = to; in.meta.width = value_bytes(to, ctx.structs);
            return { LoweredValue::Scalar, res, 0, to };
        }
        // unreachable for sema-clean programs
        set_term_trap(uint8_t(TrapReason::IllegalInstruction));
        return { LoweredValue::Scalar, 0, 0, to };
    }
    if (auto* t = dynamic_cast<const TernaryExpr*>(&ex)) {
        LoweredValue cond = lower_expr(*t->cond);
        VReg zero = new_vreg(t->cond->ty);
        ThinInstr& z = emit(ThinOp::ConstInt, zero, 0, 0, loc); z.imm.i = 0; z.meta.type = t->cond->ty; z.meta.width = value_bytes(t->cond->ty, ctx.structs);
        VReg cbool = new_vreg(&type_bool());
        ThinInstr& cb = emit(ThinOp::Cmp, cbool, cond.vreg, zero, loc);
        cb.meta.cmp = 0; cb.meta.type = t->cond->ty; cb.meta.width = value_bytes(t->cond->ty, ctx.structs);
        cb.meta.is_unsigned = (t->cond->ty && t->cond->ty->is_uint()) ? 1 : 0;
        uint32_t then_bb = new_block(), else_bb = new_block(), end_bb = new_block();
        set_term_branch(cbool, else_bb, then_bb);  // cond==0 -> else, else then
        const Type* rt = t->then_e->ty;
        bool res_slice = rt && rt->is_slice;
        bool res_float = rt && rt->is_float();
        VReg res = 0, res_len = 0;
        if (res_slice) { res = new_slice_vregs(rt); res_len = res + 1; }
        else { res = new_vreg(rt); }
        // then_bb
        enter_block(then_bb);
        {
            LoweredValue tv = lower_expr(*t->then_e);
            if (res_slice) {
                ThinInstr& m1 = emit(ThinOp::Move, res, tv.vreg, 0, loc); m1.meta.type = rt; m1.meta.width = 8;
                ThinInstr& m2 = emit(ThinOp::Move, res_len, tv.vreg + 1, 0, loc); m2.meta.type = &type_i64(); m2.meta.width = 8;
            } else {
                ThinInstr& m = emit(ThinOp::Move, res, tv.vreg, 0, loc);
                m.meta.type = rt; m.meta.width = value_bytes(rt, ctx.structs);
                if (res_float) m.meta.is_f32 = (rt->prim == Prim::F32) ? 1 : 0;
            }
            if (cur_block().term.kind == TermKind::None) set_term_jmp(end_bb);
        }
        // else_bb
        enter_block(else_bb);
        {
            LoweredValue ev = lower_expr(*t->else_e);
            if (res_slice) {
                ThinInstr& m1 = emit(ThinOp::Move, res, ev.vreg, 0, loc); m1.meta.type = rt; m1.meta.width = 8;
                ThinInstr& m2 = emit(ThinOp::Move, res_len, ev.vreg + 1, 0, loc); m2.meta.type = &type_i64(); m2.meta.width = 8;
            } else {
                ThinInstr& m = emit(ThinOp::Move, res, ev.vreg, 0, loc);
                m.meta.type = rt; m.meta.width = value_bytes(rt, ctx.structs);
                if (res_float) m.meta.is_f32 = (rt->prim == Prim::F32) ? 1 : 0;
            }
            if (cur_block().term.kind == TermKind::None) set_term_jmp(end_bb);
        }
        // end_bb: continue
        enter_block(end_bb);
        if (res_slice) return { LoweredValue::Slice, res, 0, rt };
        return { LoweredValue::Scalar, res, 0, rt };
    }
    if (auto* a = dynamic_cast<const AssignExpr*>(&ex)) {
        // handled by a dedicated helper to keep this dispatch readable
        // (compound / postfix / struct-call-target / index/field targets)
        // fall through to the dedicated lowering below
        // ---- simple (non-compound) assignment ----
        if (!a->compound) {
            // struct-local/global REASSIGNMENT (`s = mk()` where mk returns a struct)
            if (auto* call = dynamic_cast<const CallExpr*>(a->value.get())) {
                const Type* ct = call->ty;
                if (ct && !ct->struct_name.empty() && ctx.structs && ctx.structs->count(ct->struct_name)) {
                    if (auto* id = dynamic_cast<Ident*>(a->target.get())) {
                        auto it = locals.find(id->name);
                        if (it != locals.end()) {
                            LoweredValue v = lower_call(*call, it->second, 0, loc);
                            return v;  // the call wrote through the hidden ptr; value is the struct (aggregate)
                        }
                        int32_t goff = 0; const Type* gty = nullptr;
                        if (resolve_global(id->name, goff, gty)) {
                            // materialize into a temp, then copy temp -> global
                            int32_t temp_off = alloc_struct_temp(ct);
                            lower_call(*call, temp_off, 0, loc);
                            copy_frame_global(goff, temp_off, struct_size(ct), loc);
                            return { LoweredValue::Aggregate, 0, goff, ct };
                        }
                    }
                }
            }
            LoweredValue val = lower_expr(*a->value);
            store_to_target(*a->target, val, loc);
            return val;
        }
        // ---- compound assignment ----
        const Type* tt = a->target->ty;
        LoweredValue cur = lower_expr(*a->target);  // read current
        LoweredValue val = lower_expr(*a->value);
        LoweredValue result;
        if (tt && tt->is_float()) {
            bool f64 = tt->prim == Prim::F64;
            ThinOp op = ThinOp::FAdd;
            switch (*a->compound) {
            case BinExpr::Op::Add: op = ThinOp::FAdd; break;
            case BinExpr::Op::Sub: op = ThinOp::FSub; break;
            case BinExpr::Op::Mul: op = ThinOp::FMul; break;
            case BinExpr::Op::Div: op = ThinOp::FDiv; break;
            default: break;
            }
            VReg r = new_vreg(tt);
            ThinInstr& in = emit(op, r, cur.vreg, val.vreg, loc);
            in.meta.type = tt; in.meta.is_f32 = f64 ? 0 : 1; in.meta.width = f64 ? 8 : 4;
            result = { LoweredValue::Scalar, r, 0, tt };
        } else {
            bool is_div = (*a->compound == BinExpr::Op::Div || *a->compound == BinExpr::Op::Mod);
            bool is_unsigned = tt && tt->is_uint();
            if (is_div && !is_unsigned) emit_div_overflow_check(cur.vreg, val.vreg, loc);
            ThinOp op = ThinOp::Add;
            switch (*a->compound) {
            case BinExpr::Op::Add: op = ThinOp::Add; break;
            case BinExpr::Op::Sub: op = ThinOp::Sub; break;
            case BinExpr::Op::Mul: op = ThinOp::Mul; break;
            case BinExpr::Op::Div: op = ThinOp::Div; break;
            case BinExpr::Op::Mod: op = ThinOp::Mod; break;
            case BinExpr::Op::And: op = ThinOp::And; break;
            case BinExpr::Op::Or:  op = ThinOp::Or;  break;
            case BinExpr::Op::Xor: op = ThinOp::Xor; break;
            case BinExpr::Op::Shl: op = ThinOp::Shl; break;
            case BinExpr::Op::Shr: op = ThinOp::Shr; break;
            default: break;
            }
            VReg r = new_vreg(tt);
            ThinInstr& in = emit(op, r, cur.vreg, val.vreg, loc);
            in.meta.type = tt; in.meta.width = value_bytes(tt, ctx.structs);
            in.meta.is_unsigned = is_unsigned ? 1 : 0;
            result = { LoweredValue::Scalar, r, 0, tt };
        }
        store_to_target(*a->target, result, loc);
        if (a->postfix) {
            // result of the expression is the OLD value: cur -/+ 1 (mirrors tree-walker)
            if (tt && tt->is_float()) {
                bool f64 = tt->prim == Prim::F64;
                VReg one = new_vreg(tt);
                ThinInstr& z = emit(ThinOp::ConstFloat, one, 0, 0, loc);
                z.imm.f = 1.0; z.meta.type = tt; z.meta.is_f32 = f64 ? 0 : 1; z.meta.width = f64 ? 8 : 4;
                VReg old = new_vreg(tt);
                ThinOp rev = (*a->compound == BinExpr::Op::Add) ? ThinOp::FSub : ThinOp::FAdd;
                ThinInstr& in = emit(rev, old, result.vreg, one, loc);
                in.meta.type = tt; in.meta.is_f32 = f64 ? 0 : 1; in.meta.width = f64 ? 8 : 4;
                return { LoweredValue::Scalar, old, 0, tt };
            }
            VReg old = new_vreg(tt);
            ThinInstr& in = emit((*a->compound == BinExpr::Op::Add) ? ThinOp::Sub : ThinOp::Add,
                                 old, result.vreg, 0, loc);
            in.imm.i = 1;  // immediate 1 (src2 as imm when used with imm)
            in.meta.type = tt; in.meta.width = value_bytes(tt, ctx.structs);
            return { LoweredValue::Scalar, old, 0, tt };
        }
        return result;
    }
    if (auto* c = dynamic_cast<const CallExpr*>(&ex)) {
        if (c->elided) return { LoweredValue::Scalar, 0, 0, ex.ty };  // compile-time-folded assert_eq_*: no instrs
        return lower_call(*c, 0, 0, loc);
    }
    if (auto* ix = dynamic_cast<const IndexExpr*>(&ex)) {
        const Type* bt = ix->base->ty;
        const Type* elem = bt && bt->elem ? bt->elem.get() : nullptr;
        int32_t width = value_bytes(elem, ctx.structs);
        // resolve base: local fixed-array (rbp), local slice (ptr+len vregs), global
        VReg base_ptr = 0, base_len = 0; int32_t base_off = 0; bool ready = false; bool is_slice_base = false;
        bool is_global_base = false;
        if (auto* bid = dynamic_cast<const Ident*>(ix->base.get())) {
            auto it = locals.find(bid->name);
            if (it != locals.end()) {
                const Type* lt = local_types.count(bid->name) ? local_types.at(bid->name) : ix->base->ty;
                if (lt && lt->is_slice) {
                    LoweredValue b = load_scalar_local(it->second, lt, loc);
                    base_ptr = b.vreg; base_len = b.vreg + 1; ready = true; is_slice_base = true;
                } else if (lt && lt->array_len > 0) {
                    base_off = it->second; ready = true;
                }
            } else {
                int32_t goff = 0; const Type* gt = nullptr;
                if (resolve_global(bid->name, goff, gt)) {
                    if (gt && gt->is_slice) {
                        LoweredValue b = lower_expr(*ix->base);  // LoadGlobal slice
                        base_ptr = b.vreg; base_len = b.vreg + 1; ready = true; is_slice_base = true;
                    } else if (gt && gt->array_len > 0) {
                        // global fixed array: IndexAddr with globals base
                        is_global_base = true; base_off = goff; ready = true;
                    }
                }
            }
        }
        if (!ready) return { LoweredValue::Scalar, 0, 0, ex.ty };
        LoweredValue idx = lower_expr(*ix->index);
        // bounds check
        if (is_slice_base) {
            emit_bounds_check(idx.vreg, base_len, 0, loc);
        } else if (!ix->index_is_const) {
            emit_bounds_check(idx.vreg, 0, int64_t(bt->array_len), loc);
        }
        // IndexAddr: dst = base + idx*width. The EMIT convention is
        //   src1 = base, src2 = index (vreg; or src2==0 + imm.i for an imm index)
        //   base resolution: src1==0 -> fixed-array base at meta.frame_off (local)
        //     or globals_base+addend (global, base_kind=GlobalsBase); src1 != 0 ->
        //     if src1 is a slice vreg, load_slice_vreg (ptr+len); else load_int_vreg.
        // So: slice base -> src1=base_ptr(slice vreg), src2=idx; local fixed array
        // -> src1=0 + meta.frame_off=base_off, src2=idx; global -> src1=0 +
        //   base_kind=GlobalsBase+addend, src2=idx.
        VReg addr = new_vreg(&type_i64());
        ThinInstr& ia = emit(ThinOp::IndexAddr, addr, 0, idx.vreg, loc);  // src1=base(set below), src2=idx
        ia.meta.width = width;
        ia.meta.type = elem;
        if (is_slice_base) {
            ia.src1 = base_ptr;            // base = slice ptr vreg (emit loads it as a slice)
            ia.meta.frame_off = 0;
        } else if (is_global_base) {
            ia.src1 = 0;                   // base = globals_base + addend
            ia.meta.base_kind = AbsFixup::GlobalsBase;
            ia.meta.addend = uint32_t(base_off);
        } else {
            ia.src1 = 0;                   // base = rbp + base_off (local fixed array)
            ia.meta.frame_off = base_off;
        }
        // load element
        if (elem && elem->is_float()) {
            VReg res = new_vreg(elem);
            ThinInstr& ld = emit(ThinOp::LoadFrame, res, 0, 0, loc);  // load from [addr+0]
            ld.src1 = addr; ld.meta.frame_off = 0; ld.meta.type = elem;
            ld.meta.width = width; ld.meta.is_f32 = (elem->prim == Prim::F32) ? 1 : 0;
            return { LoweredValue::Scalar, res, 0, elem };
        }
        VReg res = new_vreg(elem);
        ThinInstr& ld = emit(ThinOp::LoadFrame, res, 0, 0, loc);  // load from [addr+0]
        ld.src1 = addr; ld.meta.frame_off = 0; ld.meta.type = elem; ld.meta.width = width;
        return { LoweredValue::Scalar, res, 0, elem };
    }
    if (auto* v = dynamic_cast<const ViewExpr*>(&ex)) {
        // arr[..]: fixed array T[N] -> slice {ptr=&arr, len=N}
        if (auto* bid = dynamic_cast<const Ident*>(v->base.get())) {
            auto it = locals.find(bid->name);
            if (it != locals.end()) {
                const Type* lt = local_types.count(bid->name) ? local_types.at(bid->name) : v->base->ty;
                if (lt && lt->array_len > 0) {
                    VReg ptr = new_slice_vregs(ex.ty); VReg len = ptr + 1;
                    ThinInstr& mk = emit(ThinOp::MakeSlice, ptr, 0, 0, loc);
                    mk.meta.frame_off = it->second; mk.meta.len = int32_t(lt->array_len);
                    mk.meta.type = ex.ty; mk.meta.width = 8;
                    ThinInstr& l = emit(ThinOp::ConstInt, len, 0, 0, loc);
                    l.imm.i = int64_t(lt->array_len); l.meta.type = &type_i64(); l.meta.width = 8;
                    return { LoweredValue::Slice, ptr, 0, ex.ty };
                }
            } else {
                int32_t goff = 0; const Type* gt = nullptr;
                if (resolve_global(bid->name, goff, gt) && gt && gt->array_len > 0) {
                    VReg ptr = new_slice_vregs(ex.ty); VReg len = ptr + 1;
                    ThinInstr& mk = emit(ThinOp::MakeSlice, ptr, 0, 0, loc);
                    mk.meta.base_kind = AbsFixup::GlobalsBase; mk.meta.addend = uint32_t(goff);
                    mk.meta.len = int32_t(gt->array_len); mk.meta.type = ex.ty; mk.meta.width = 8;
                    ThinInstr& l = emit(ThinOp::ConstInt, len, 0, 0, loc);
                    l.imm.i = int64_t(gt->array_len); l.meta.type = &type_i64(); l.meta.width = 8;
                    return { LoweredValue::Slice, ptr, 0, ex.ty };
                }
            }
        }
        return { LoweredValue::Slice, 0, 0, ex.ty };
    }
    if (auto* fl = dynamic_cast<const FieldExpr*>(&ex)) {
        if (auto* bid = dynamic_cast<const Ident*>(fl->base.get())) {
            auto it = locals.find(bid->name);
            if (it != locals.end() && ctx.structs) {
                const Type* bt = local_types.count(bid->name) ? local_types.at(bid->name) : fl->base->ty;
                auto sit = bt && !bt->struct_name.empty() ? ctx.structs->find(bt->struct_name) : ctx.structs->end();
                if (sit != ctx.structs->end()) {
                    auto fit = sit->second.fields.find(fl->field);
                    if (fit != sit->second.fields.end()) {
                        int32_t addr_off = it->second + fit->second.offset;
                        const Type* ft = fit->second.ty;
                        return load_scalar_local(addr_off, ft, loc);
                    }
                }
            } else if (ctx.structs) {
                int32_t goff = 0; const Type* gt = nullptr;
                if (resolve_global(bid->name, goff, gt)) {
                    auto sit = gt && !gt->struct_name.empty() ? ctx.structs->find(gt->struct_name) : ctx.structs->end();
                    if (sit != ctx.structs->end()) {
                        auto fit = sit->second.fields.find(fl->field);
                        if (fit != sit->second.fields.end()) {
                            int32_t addr_off = goff + fit->second.offset;
                            const Type* ft = fit->second.ty;
                            // global field read via LoadGlobal
                            VReg v = new_vreg(ft);
                            ThinInstr& in = emit(ThinOp::LoadGlobal, v, 0, 0, loc);
                            in.meta.base_kind = AbsFixup::GlobalsBase; in.meta.addend = uint32_t(addr_off);
                            in.meta.type = ft; in.meta.width = value_bytes(ft, ctx.structs);
                            if (ft && ft->is_float()) in.meta.is_f32 = (ft->prim == Prim::F32) ? 1 : 0;
                            return { LoweredValue::Scalar, v, 0, ft };
                        }
                    }
                }
            }
        } else if (auto* ix = dynamic_cast<const IndexExpr*>(fl->base.get())) {
            // arr[i].field: base is an IndexExpr into an array/slice of structs.
            // The bare-Ident base above only handles a local/global struct
            // variable's field; an indexed struct element needs the element
            // ADDRESS computed (base + index*struct_size) before the field
            // offset is added. Mirrors the IndexExpr case's base resolution,
            // then emits IndexAddr (element address) + LoadFrame at the field
            // offset. v1 scope: ix->base must be a bare Ident (same
            // restriction IndexExpr itself enforces).
            const Type* bt = ix->base->ty;          // array/slice type
            const Type* elem = ix->ty;              // struct element type (sema sets ix->ty = base->elem)
            const Type* ft = nullptr; int32_t field_off = 0;
            if (elem && !elem->struct_name.empty() && ctx.structs) {
                auto sit = ctx.structs->find(elem->struct_name);
                if (sit != ctx.structs->end()) {
                    auto fit = sit->second.fields.find(fl->field);
                    if (fit != sit->second.fields.end()) {
                        ft = fit->second.ty; field_off = fit->second.offset;
                    }
                }
            }
            if (!ft) return { LoweredValue::Scalar, 0, 0, ex.ty };
            int32_t struct_width = value_bytes(elem, ctx.structs);
            // Resolve base (identical to the IndexExpr case): local fixed
            // array (base_off), local slice (base_ptr slice vreg), global
            // fixed array (GlobalsBase + addend), global slice (slice vreg).
            VReg base_ptr = 0, base_len = 0; int32_t base_off = 0;
            bool ready = false, is_slice_base = false, is_global_base = false;
            if (auto* ibid = dynamic_cast<const Ident*>(ix->base.get())) {
                auto lit = locals.find(ibid->name);
                if (lit != locals.end()) {
                    const Type* lt = local_types.count(ibid->name) ? local_types.at(ibid->name) : ix->base->ty;
                    if (lt && lt->is_slice) {
                        LoweredValue b = load_scalar_local(lit->second, lt, loc);
                        base_ptr = b.vreg; base_len = b.vreg + 1; ready = true; is_slice_base = true;
                    } else if (lt && lt->array_len > 0) {
                        base_off = lit->second; ready = true;
                    }
                } else {
                    int32_t goff = 0; const Type* gt = nullptr;
                    if (resolve_global(ibid->name, goff, gt)) {
                        if (gt && gt->is_slice) {
                            LoweredValue b = lower_expr(*ix->base);  // LoadGlobal slice
                            base_ptr = b.vreg; base_len = b.vreg + 1; ready = true; is_slice_base = true;
                        } else if (gt && gt->array_len > 0) {
                            is_global_base = true; base_off = goff; ready = true;
                        }
                    }
                }
            }
            if (!ready) return { LoweredValue::Scalar, 0, 0, ex.ty };
            LoweredValue idx = lower_expr(*ix->index);
            // bounds check (same policy as IndexExpr)
            if (is_slice_base) {
                emit_bounds_check(idx.vreg, base_len, 0, loc);
            } else if (!ix->index_is_const) {
                emit_bounds_check(idx.vreg, 0, int64_t(bt->array_len), loc);
            }
            // IndexAddr: dst = base + idx*struct_width (element address).
            VReg addr = new_vreg(&type_i64());
            ThinInstr& ia = emit(ThinOp::IndexAddr, addr, 0, idx.vreg, loc);
            ia.meta.width = struct_width;
            ia.meta.type = elem;
            if (is_slice_base) {
                ia.src1 = base_ptr;
                ia.meta.frame_off = 0;
            } else if (is_global_base) {
                ia.src1 = 0;
                ia.meta.base_kind = AbsFixup::GlobalsBase;
                ia.meta.addend = uint32_t(base_off);
            } else {
                ia.src1 = 0;
                ia.meta.frame_off = base_off;
            }
            // LoadFrame from [element_addr + field_off] (src1=addr -> [r11+off]).
            VReg res = new_vreg(ft);
            ThinInstr& ld = emit(ThinOp::LoadFrame, res, 0, 0, loc);
            ld.src1 = addr; ld.meta.frame_off = field_off; ld.meta.type = ft;
            ld.meta.width = value_bytes(ft, ctx.structs);
            if (ft && ft->is_float()) ld.meta.is_f32 = (ft->prim == Prim::F32) ? 1 : 0;
            return { LoweredValue::Scalar, res, 0, ft };
        }
        return { LoweredValue::Scalar, 0, 0, ex.ty };
    }
    if (auto* s = dynamic_cast<const SizeofExpr*>(&ex)) {
        VReg v = new_vreg(&type_i64());
        ThinInstr& in = emit(ThinOp::ConstInt, v, 0, 0, loc);
        in.imm.i = int64_t(s->resolved); in.meta.type = &type_i64(); in.meta.width = 8;
        return { LoweredValue::Scalar, v, 0, &type_i64() };
    }
    if (auto* o = dynamic_cast<const OffsetofExpr*>(&ex)) {
        VReg v = new_vreg(&type_i64());
        ThinInstr& in = emit(ThinOp::ConstInt, v, 0, 0, loc);
        in.imm.i = int64_t(o->resolved); in.meta.type = &type_i64(); in.meta.width = 8;
        return { LoweredValue::Scalar, v, 0, &type_i64() };
    }
    if (auto* al = dynamic_cast<const ArrayLit*>(&ex)) {
        // slice-typed array literal (the only kind reaching eval): backing temp + MakeSlice
        const Type* at = al->ty;
        if (!at || !at->is_slice || !at->elem) {
            set_term_trap(uint8_t(TrapReason::IllegalInstruction));
            return { LoweredValue::Slice, 0, 0, at };
        }
        const Type* elem_ty = at->elem.get();
        int32_t elem_sz = value_bytes(elem_ty, ctx.structs);
        uint32_t count = uint32_t(al->elements.size());
        int32_t back_off = alloc_arr_temp(elem_ty, count);
        for (size_t i = 0; i < al->elements.size(); ++i)
            store_value_to_frame(*al->elements[i], elem_ty, back_off + int32_t(i) * elem_sz, al->elements[i]->loc);
        VReg ptr = new_slice_vregs(at); VReg len = ptr + 1;
        ThinInstr& mk = emit(ThinOp::MakeSlice, ptr, 0, 0, loc);
        mk.meta.frame_off = back_off; mk.meta.len = int32_t(count); mk.meta.type = at; mk.meta.width = 8;
        ThinInstr& l = emit(ThinOp::ConstInt, len, 0, 0, loc);
        l.imm.i = int64_t(count); l.meta.type = &type_i64(); l.meta.width = 8;
        return { LoweredValue::Slice, ptr, 0, at };
    }
    if (dynamic_cast<const EnumAccessExpr*>(&ex)) {
        // sema rewrites EnumAccess to IntLit; handle defensively as a 0 constant
        VReg v = new_vreg(&type_i64());
        ThinInstr& in = emit(ThinOp::ConstInt, v, 0, 0, loc);
        in.imm.i = 0; in.meta.type = &type_i64(); in.meta.width = 8;
        return { LoweredValue::Scalar, v, 0, &type_i64() };
    }
    // StructLit: handled at its let-init site; if it reaches here as a value, materialize
    // into a temp and return an aggregate descriptor.
    if (auto* sl = dynamic_cast<const StructLit*>(&ex)) {
        const Type* st = ex.ty;
        if (st && is_registered_struct_ty(st)) {
            int32_t off = alloc_struct_temp(st);
            store_value_to_frame(*sl, st, off, loc);
            return { LoweredValue::Aggregate, 0, off, st };
        }
        return { LoweredValue::Aggregate, 0, 0, st };
    }
    return { LoweredValue::Scalar, 0, 0, ex.ty };
}

// store an rvalue (lv) into an assignment target (Ident / IndexExpr / FieldExpr)
void ThinLowerer::store_to_target(const Expr& target, const LoweredValue& lv, Loc loc) {
    if (auto* id = dynamic_cast<const Ident*>(&target)) {
        auto it = locals.find(id->name);
        if (it != locals.end()) {
            store_scalar_local(lv, it->second, loc);
            // Item E pin write-through: the slot is the backing store (always synced),
            // so the StoreFrame above already keeps the pin in sync — no extra instr.
            return;
        }
        int32_t goff = 0; const Type* gty = nullptr;
        if (resolve_global(id->name, goff, gty)) {
            store_scalar_global(lv, goff, loc);
            return;
        }
        return;
    }
    if (auto* ixt = dynamic_cast<const IndexExpr*>(&target)) {
        const Type* bt = ixt->base->ty;
        const Type* elem = bt && bt->elem ? bt->elem.get() : nullptr;
        int32_t width = value_bytes(elem, ctx.structs);
        // resolve base
        VReg base_ptr = 0, base_len = 0; int32_t base_off = 0; bool ready = false; bool is_slice_base = false; bool is_global_base = false;
        if (auto* bid = dynamic_cast<const Ident*>(ixt->base.get())) {
            auto it = locals.find(bid->name);
            if (it != locals.end()) {
                const Type* lt = local_types.count(bid->name) ? local_types.at(bid->name) : ixt->base->ty;
                if (lt && lt->is_slice) {
                    LoweredValue b = load_scalar_local(it->second, lt, loc);
                    base_ptr = b.vreg; base_len = b.vreg + 1; ready = true; is_slice_base = true;
                } else if (lt && lt->array_len > 0) {
                    base_off = it->second; ready = true;
                }
            } else {
                int32_t goff = 0; const Type* gt = nullptr;
                if (resolve_global(bid->name, goff, gt)) {
                    if (gt && gt->is_slice) {
                        LoweredValue b = lower_expr(*ixt->base);
                        base_ptr = b.vreg; base_len = b.vreg + 1; ready = true; is_slice_base = true;
                    } else if (gt && gt->array_len > 0) {
                        is_global_base = true; base_off = goff; ready = true;
                    }
                }
            }
        }
        if (!ready) return;
        LoweredValue idx = lower_expr(*ixt->index);
        if (is_slice_base) emit_bounds_check(idx.vreg, base_len, 0, loc);
        else if (!ixt->index_is_const) emit_bounds_check(idx.vreg, 0, int64_t(bt->array_len), loc);
        VReg addr = new_vreg(&type_i64());
        ThinInstr& ia = emit(ThinOp::IndexAddr, addr, idx.vreg, 0, loc);
        ia.meta.width = width; ia.meta.type = elem;
        if (is_slice_base) { ia.src2 = base_ptr; ia.meta.frame_off = 0; }
        else if (is_global_base) { ia.meta.base_kind = AbsFixup::GlobalsBase; ia.meta.addend = uint32_t(base_off); }
        else { ia.meta.frame_off = base_off; }
        // store element to [addr+0]
        ThinInstr& st = emit(ThinOp::StoreFrame, 0, lv.vreg, 0, loc);
        st.src2 = addr; st.meta.frame_off = 0; st.meta.type = lv.ty;
        st.meta.width = (lv.ty && lv.ty->is_float()) ? value_bytes(lv.ty, ctx.structs) : width;
        if (lv.ty && lv.ty->is_float()) st.meta.is_f32 = (lv.ty->prim == Prim::F32) ? 1 : 0;
        return;
    }
    if (auto* flt = dynamic_cast<const FieldExpr*>(&target)) {
        if (auto* bid = dynamic_cast<const Ident*>(flt->base.get())) {
            auto it = locals.find(bid->name);
            if (it != locals.end() && ctx.structs) {
                const Type* bt = local_types.count(bid->name) ? local_types.at(bid->name) : flt->base->ty;
                auto sit = bt && !bt->struct_name.empty() ? ctx.structs->find(bt->struct_name) : ctx.structs->end();
                if (sit != ctx.structs->end()) {
                    auto fit = sit->second.fields.find(flt->field);
                    if (fit != sit->second.fields.end()) {
                        int32_t addr_off = it->second + fit->second.offset;
                        const Type* ft = fit->second.ty;
                        if (ft->is_slice) {
                            ThinInstr& p = emit(ThinOp::StoreFrame, 0, lv.vreg, 0, loc);
                            p.meta.frame_off = addr_off; p.meta.type = ft; p.meta.width = 8;
                            ThinInstr& l = emit(ThinOp::StoreFrame, 0, lv.vreg + 1, 0, loc);
                            l.meta.frame_off = addr_off + 8; l.meta.type = &type_i64(); l.meta.width = 8;
                        } else {
                            ThinInstr& st = emit(ThinOp::StoreFrame, 0, lv.vreg, 0, loc);
                            st.meta.frame_off = addr_off; st.meta.type = ft;
                            st.meta.width = value_bytes(ft, ctx.structs);
                            if (ft->is_float()) st.meta.is_f32 = (ft->prim == Prim::F32) ? 1 : 0;
                        }
                        return;
                    }
                }
            } else if (ctx.structs) {
                int32_t goff = 0; const Type* gt = nullptr;
                if (resolve_global(bid->name, goff, gt)) {
                    auto sit = gt && !gt->struct_name.empty() ? ctx.structs->find(gt->struct_name) : ctx.structs->end();
                    if (sit != ctx.structs->end()) {
                        auto fit = sit->second.fields.find(flt->field);
                        if (fit != sit->second.fields.end()) {
                            int32_t addr_off = goff + fit->second.offset;
                            const Type* ft = fit->second.ty;
                            ThinInstr& st = emit(ThinOp::StoreGlobal, 0, lv.vreg, 0, loc);
                            st.meta.base_kind = AbsFixup::GlobalsBase; st.meta.addend = uint32_t(addr_off);
                            st.meta.type = ft; st.meta.width = value_bytes(ft, ctx.structs);
                            if (ft->is_float()) st.meta.is_f32 = (ft->prim == Prim::F32) ? 1 : 0;
                            return;
                        }
                    }
                }
            }
        }
        return;
    }
}

// ─────────────── lower_call (mirrors CG::eval CallExpr + eval_struct_returning_call) ───────────────
LoweredValue ThinLowerer::lower_call(const CallExpr& c, int32_t hidden_dest_off, VReg hidden_dest_vreg, Loc loc) {
    const Type* ret_ty = c.ty;
    bool ret_struct = ret_ty && is_registered_struct_ty(ret_ty);
    bool ret_slice = ret_ty && ret_ty->is_slice;

    // Build the operand plan (receiver first, then args), word-indexed.
    struct Operand { const Expr* e; const Type* ty; int32_t slot0; int words; bool is_struct; bool is_float; };
    std::vector<Operand> ops;
    int32_t next_slot = ret_struct ? 1 : 0;  // word 0 reserved for hidden ptr
    auto add_operand = [&](const Expr* oe) {
        const Type* t = oe->ty;
        int w = words_for_type(t, ctx.structs);
        bool is_struct = is_registered_struct_ty(t);
        bool is_float = t && t->is_float() && !is_struct;
        ops.push_back({oe, t, next_slot, w, is_struct, is_float});
        next_slot += w;
    };
    if (c.receiver) add_operand(c.receiver.get());
    for (auto& a : c.args) add_operand(a.get());
    (void)next_slot;

    // Build the call instr as a LOCAL (do not hold a reference into the block's
    // instrs vector across the arg-eval / guard / depth-check emits below — a
    // push may reallocate and dangle the reference). It is pushed ONCE at the end.
    ThinInstr in;
    in.op = ThinOp::CallScript;  // refined below
    in.loc = loc;

    // result vreg(s) allocated up front so arg-eval sub-calls can't reallocate past us.
    if (ret_struct) {
        // hidden dest resolution
        if (hidden_dest_vreg != 0) {
            in.args.push_back(hidden_dest_vreg);
            in.arg_frame_offs.push_back(-1);
            in.arg_types.push_back(ret_ty);
        } else if (hidden_dest_off != 0) {
            in.args.push_back(0);  // sentinel
            in.arg_frame_offs.push_back(hidden_dest_off);
            in.arg_types.push_back(ret_ty);
        } else {
            // no dest provided (call used as a scalar expr value but returns a struct):
            // materialize into a fresh temp.
            int32_t temp_off = alloc_struct_temp(ret_ty);
            in.args.push_back(0);
            in.arg_frame_offs.push_back(temp_off);
            in.arg_types.push_back(ret_ty);
            hidden_dest_off = temp_off;
        }
        in.dst = 0; in.meta.type = ret_ty;
    } else if (ret_slice) {
        VReg ptr = new_slice_vregs(ret_ty);
        in.dst = ptr; in.meta.type = ret_ty;
    } else if (!ret_ty || ret_ty->is_void()) {
        in.dst = 0; in.ret_type = ret_ty;
    } else {
        VReg res = new_vreg(ret_ty);
        in.dst = res; in.meta.type = ret_ty; in.meta.width = value_bytes(ret_ty, ctx.structs);
        if (ret_ty->is_float()) in.meta.is_f32 = (ret_ty->prim == Prim::F32) ? 1 : 0;
    }
    in.ret_type = ret_ty;

    // For an indirect call: lower the target + guard BEFORE args (mirrors tree-walker).
    if (c.is_indirect) {
        LoweredValue ht = lower_expr(*c.indirect_target);
        emit_call_target_guard(loc);  // validates the handle; c3 reads it from src1
        in.src1 = ht.vreg;
    }

    // args
    for (auto& op : ops) {
        if (op.is_struct) {
            // struct-by-value: bare Ident (local/global) / StructLit / struct-returning Call.
            if (auto* id = dynamic_cast<const Ident*>(op.e)) {
                auto it = locals.find(id->name);
                if (it != locals.end()) {
                    in.args.push_back(0);
                    in.arg_frame_offs.push_back(it->second);
                    in.arg_types.push_back(op.ty);
                    continue;
                }
                int32_t goff = 0; const Type* gty = nullptr;
                if (resolve_global(id->name, goff, gty)) {
                    // copy global -> temp, use temp as the source
                    int32_t temp_off = alloc_struct_temp(op.ty);
                    copy_global_frame(temp_off, goff, struct_size(op.ty), op.e->loc);
                    in.args.push_back(0);
                    in.arg_frame_offs.push_back(temp_off);
                    in.arg_types.push_back(op.ty);
                    continue;
                }
            }
            if (auto* sl = dynamic_cast<const StructLit*>(op.e)) {
                int32_t temp_off = alloc_struct_temp(op.ty);
                store_value_to_frame(*sl, op.ty, temp_off, op.e->loc);
                in.args.push_back(0);
                in.arg_frame_offs.push_back(temp_off);
                in.arg_types.push_back(op.ty);
                continue;
            }
            if (auto* call = dynamic_cast<const CallExpr*>(op.e)) {
                int32_t temp_off = alloc_struct_temp(op.ty);
                lower_call(*call, temp_off, 0, op.e->loc);
                in.args.push_back(0);
                in.arg_frame_offs.push_back(temp_off);
                in.arg_types.push_back(op.ty);
                continue;
            }
            // unreachable for sema-clean programs
            in.args.push_back(0);
            in.arg_frame_offs.push_back(-1);
            in.arg_types.push_back(op.ty);
        } else {
            LoweredValue av = lower_expr(*op.e);
            if (op.words == 2) {
                // slice: ptr, len
                in.args.push_back(av.vreg); in.arg_frame_offs.push_back(-1); in.arg_types.push_back(op.ty);
                in.args.push_back(av.vreg + 1); in.arg_frame_offs.push_back(-1); in.arg_types.push_back(&type_i64());
            } else {
                in.args.push_back(av.vreg); in.arg_frame_offs.push_back(-1); in.arg_types.push_back(op.ty);
            }
        }
    }

    // refine the op + stamp the target metadata, emit the depth check (gated),
    // then push the call instr (everything is populated, no further emits after).
    if (c.is_native) {
        in.op = ThinOp::CallNative;
        in.meta.native_name = c.native_binding_name;
        in.native_fn = c.native_fn;
        emit_depth_check(loc);
    } else if (!c.module_alias.empty()) {
        in.op = ThinOp::CallCrossModule;
        in.meta.mod_id = int32_t(c.cross_module_id);
        in.meta.slot = int32_t(c.cross_module_slot);
        if (c.cross_module_unresolved) {
            // deferred trap (module/fn not registered) — mirrors emit_cross_module_call.
            set_term_trap(uint8_t(TrapReason::None));
            return { LoweredValue::Scalar, 0, 0, ret_ty };
        }
        in.meta.base_kind = AbsFixup::ModuleRegistryBase;
        emit_depth_check(loc);
    } else if (c.is_indirect) {
        in.op = ThinOp::CallIndirect;
        in.meta.base_kind = AbsFixup::DispatchTableBase;
        emit_depth_check(loc);
    } else {
        in.op = ThinOp::CallScript;
        in.meta.slot = int32_t(c.script_slot);
        in.meta.base_kind = AbsFixup::DispatchTableBase;
        emit_depth_check(loc);
    }
    cur_block().instrs.push_back(std::move(in));

    if (ret_struct)   return { LoweredValue::Aggregate, 0, hidden_dest_off, ret_ty };
    if (ret_slice)    return { LoweredValue::Slice, in.dst, 0, ret_ty };
    if (!ret_ty || ret_ty->is_void()) return { LoweredValue::Scalar, 0, 0, ret_ty };
    return { LoweredValue::Scalar, in.dst, 0, ret_ty };
}

// ─────────────── lower_block / lower_stmt (mirrors CG::exec_block / exec_stmt) ───────────────
void ThinLowerer::lower_block(const Block& b) {
    auto saved_locals = locals;
    auto saved_types = local_types;
    CleanupScope scope;
    for (auto& s : b.stmts) {
        if (auto* ds = dynamic_cast<const DeferStmt*>(s.get())) {
            auto it = defer_site_indices.find(ds);
            if (it != defer_site_indices.end()) scope.reached_sites.push_back(it->second);
        }
    }
    cleanup_scopes.push_back(std::move(scope));
    // reset direct defer flags for this block (per-iteration for loops)
    if (!cleanup_scopes.back().reached_sites.empty()) {
        VReg zero = new_vreg(&type_i64());
        ThinInstr& z = emit(ThinOp::ConstInt, zero, 0, 0, b.stmts.empty() ? Loc{} : b.stmts.front()->loc);
        z.imm.i = 0; z.meta.type = &type_i64(); z.meta.width = 8;
        for (size_t site : cleanup_scopes.back().reached_sites) {
            ThinInstr& st = emit(ThinOp::StoreFrame, 0, zero, 0, Loc{});
            st.meta.frame_off = defer_sites[site].flag_offset; st.meta.type = &type_i64(); st.meta.width = 8;
        }
    }
    for (auto& s : b.stmts) lower_stmt(*s);
    emit_cleanup_scope(cleanup_scopes.size() - 1, b.stmts.empty() ? Loc{} : b.stmts.back()->loc);
    cleanup_scopes.pop_back();
    locals = std::move(saved_locals);
    local_types = std::move(saved_types);
}

void ThinLowerer::lower_stmt(const Stmt& s) {
    const Loc loc = s.loc;

    // static_assert produces NO codegen (sema resolved it fully: true ->
    // elided, false / non-const -> compile error that never reaches here).
    // Skip it before any dispatch so the IR lowering emits nothing for it.
    if (dynamic_cast<const StaticAssertStmt*>(&s)) return;

    if (auto* ls = dynamic_cast<const LetStmt*>(&s)) {
        if (!ls->init) {
            // no initializer: alloc + zero-fill the slot
            const Type* t = ls->ty.get();
            int32_t off = alloc_local(ls->name, t);
            int32_t remaining = local_width_bytes(t, ctx.structs);
            int32_t cur = off;
            VReg zero = new_vreg(&type_i64());
            ThinInstr& z = emit(ThinOp::ConstInt, zero, 0, 0, loc); z.imm.i = 0; z.meta.type = &type_i64(); z.meta.width = 8;
            while (remaining > 0) {
                int32_t chunk = remaining >= 8 ? 8 : remaining >= 4 ? 4 : remaining >= 2 ? 2 : 1;
                ThinInstr& st = emit(ThinOp::StoreFrame, 0, zero, 0, loc);
                st.meta.frame_off = cur; st.meta.width = chunk; st.meta.type = &type_i64();
                cur += chunk; remaining -= chunk;
            }
            return;
        }
        if (auto* slit = dynamic_cast<const StructLit*>(ls->init.get())) {
            const Type* st = ls->init->ty;
            int32_t base_off = alloc_local(ls->name, st);
            const StructLayout* layout = (ctx.structs && st && !st->struct_name.empty())
                ? (ctx.structs->count(st->struct_name) ? &ctx.structs->at(st->struct_name) : nullptr) : nullptr;
            if (layout) {
                for (auto& kv : slit->fields) {
                    auto fit = layout->fields.find(kv.first);
                    if (fit == layout->fields.end()) continue;
                    store_value_to_frame(*kv.second, fit->second.ty, base_off + fit->second.offset, kv.second->loc);
                }
            }
            return;
        }
        if (auto* alit = dynamic_cast<const ArrayLit*>(ls->init.get())) {
            const Type* at = ls->init->ty;
            const Type* elem_ty = at && at->elem ? at->elem.get() : nullptr;
            int32_t elem_sz = value_bytes(elem_ty, ctx.structs);
            if (at && at->array_len > 0) {
                int32_t base_off = alloc_local(ls->name, at);
                for (size_t i = 0; i < alit->elements.size(); ++i)
                    store_value_to_frame(*alit->elements[i], elem_ty, base_off + int32_t(i) * elem_sz, alit->elements[i]->loc);
            } else if (at && at->is_slice) {
                int32_t slot_off = alloc_local(ls->name, at);
                uint32_t count = uint32_t(alit->elements.size());
                int32_t back_off = alloc_arr_temp(elem_ty, count);
                for (size_t i = 0; i < alit->elements.size(); ++i)
                    store_value_to_frame(*alit->elements[i], elem_ty, back_off + int32_t(i) * elem_sz, alit->elements[i]->loc);
                // ptr = lea [rbp+back_off]; len = count -> store into slice slot
                VReg ptr = new_vreg(&type_i64());
                ThinInstr& mk = emit(ThinOp::MakeSlice, ptr, 0, 0, loc);
                mk.meta.frame_off = back_off; mk.meta.len = int32_t(count); mk.meta.type = at; mk.meta.width = 8;
                ThinInstr& sp = emit(ThinOp::StoreFrame, 0, ptr, 0, loc);
                sp.meta.frame_off = slot_off; sp.meta.type = at; sp.meta.width = 8;
                VReg len = new_vreg(&type_i64());
                ThinInstr& l = emit(ThinOp::ConstInt, len, 0, 0, loc); l.imm.i = int64_t(count); l.meta.type = &type_i64(); l.meta.width = 8;
                ThinInstr& sl = emit(ThinOp::StoreFrame, 0, len, 0, loc);
                sl.meta.frame_off = slot_off + 8; sl.meta.type = &type_i64(); sl.meta.width = 8;
            }
            return;
        }
        if (auto* call = dynamic_cast<const CallExpr*>(ls->init.get())) {
            const Type* ct = call->ty;
            if (ct && !ct->struct_name.empty() && ctx.structs && ctx.structs->count(ct->struct_name)) {
                int32_t off = alloc_local(ls->name, ct);
                lower_call(*call, off, 0, loc);
                return;
            }
        }
        if (auto* cast = dynamic_cast<const CastExpr*>(ls->init.get())) {
            const Type* ct = cast->ty;
            const bool aggregate = ct && (ct->array_len > 0 ||
                (!ct->struct_name.empty() && ctx.structs && ctx.structs->count(ct->struct_name) != 0));
            if (aggregate && cast->operand->ty && cast->operand->ty->same(*ct)) {
                int32_t dst = alloc_local(ls->name, ct);
                auto* id = dynamic_cast<const Ident*>(cast->operand.get());
                auto src = id ? locals.find(id->name) : locals.end();
                if (src != locals.end())
                    copy_frame_frame(dst, src->second, local_width_bytes(ct, ctx.structs), loc);
                return;
            }
        }
        int32_t off = alloc_local(ls->name, ls->init->ty);
        LoweredValue v = lower_expr(*ls->init);
        const Type* t = ls->init->ty;
        if (t && t->is_slice) {
            ThinInstr& p = emit(ThinOp::StoreFrame, 0, v.vreg, 0, loc);
            p.meta.frame_off = off; p.meta.type = t; p.meta.width = 8;
            ThinInstr& l = emit(ThinOp::StoreFrame, 0, v.vreg + 1, 0, loc);
            l.meta.frame_off = off + 8; l.meta.type = &type_i64(); l.meta.width = 8;
        } else if (t && (t->array_len > 0 || is_registered_struct_ty(t))) {
            // aggregate init from an aggregate value: copy bytes (v is Aggregate)
            if (v.kind == LoweredValue::Aggregate)
                copy_frame_frame(off, v.frame_off, local_width_bytes(t, ctx.structs), loc);
        } else {
            store_scalar_local(v, off, loc);
        }
        return;
    }
    if (auto* es = dynamic_cast<const ExprStmt*>(&s)) { lower_expr(*es->expr); return; }
    if (auto* ds = dynamic_cast<const DeferStmt*>(&s)) {
        auto it = defer_site_indices.find(ds);
        if (it != defer_site_indices.end()) {
            DeferSite& site = defer_sites[it->second];
            site.locals_at_decl = locals;
            site.types_at_decl = local_types;
            // set the activation flag = 1
            VReg one = new_vreg(&type_i64());
            ThinInstr& o = emit(ThinOp::ConstInt, one, 0, 0, loc); o.imm.i = 1; o.meta.type = &type_i64(); o.meta.width = 8;
            ThinInstr& st = emit(ThinOp::StoreFrame, 0, one, 0, loc);
            st.meta.frame_off = site.flag_offset; st.meta.type = &type_i64(); st.meta.width = 8;
        }
        return;
    }
    if (auto* rs = dynamic_cast<const ReturnStmt*>(&s)) {
        bool has_defers = has_active_cleanups();
        if (returns_struct_by_ptr()) {
            // struct-by-ptr return: write the struct through the incoming hidden ptr,
            // THEN run defers, THEN reload the hidden ptr (the hidden-ptr ABI returns it
            // in rax). The reload is AFTER cleanups so a defer can't clobber it (mirrors
            // the tree-walker's post-cleanup load_reg_mem(struct_ret_ptr_offset)).
            if (rs->value) {
                VReg hptr = new_vreg(&type_i64());
                ThinInstr& ld = emit(ThinOp::LoadFrame, hptr, 0, 0, loc);
                ld.meta.frame_off = struct_ret_ptr_offset; ld.meta.type = &type_i64(); ld.meta.width = 8;
                if (auto* id = dynamic_cast<const Ident*>(rs->value.get())) {
                    auto lit = locals.find(id->name);
                    if (lit != locals.end()) {
                        copy_frame_vptr(hptr, lit->second, struct_size(f.ret.get()), loc);
                    } else {
                        int32_t goff = 0; const Type* gty = nullptr;
                        if (resolve_global(id->name, goff, gty))
                            copy_global_vptr(hptr, goff, struct_size(f.ret.get()), loc);
                    }
                } else if (auto* call = dynamic_cast<const CallExpr*>(rs->value.get())) {
                    lower_call(*call, 0, hptr, loc);  // hidden dest = the loaded ptr
                } else if (auto* sl = dynamic_cast<const StructLit*>(rs->value.get())) {
                    int32_t temp_off = alloc_struct_temp(f.ret.get());
                    store_value_to_frame(*sl, f.ret.get(), temp_off, loc);
                    copy_frame_vptr(hptr, temp_off, struct_size(f.ret.get()), loc);
                }
            }
            if (has_defers) emit_cleanups_to(0, loc);
            VReg ret = new_vreg(&type_i64());
            ThinInstr& ld = emit(ThinOp::LoadFrame, ret, 0, 0, loc);
            ld.meta.frame_off = struct_ret_ptr_offset; ld.meta.type = &type_i64(); ld.meta.width = 8;
            set_term_return(ret);
            return;
        }
        bool is_float_ret = f.ret && f.ret->is_float();
        bool is_slice_ret = f.ret && f.ret->is_slice;
        LoweredValue rv;
        if (rs->value) rv = lower_expr(*rs->value);
        if (has_defers && rs->value) {
            // The tree-walker stashes the return value(s) across defer cleanup (a defer's
            // expression may clobber rax/xmm0/rdx). In the IR we Move the return vregs into
            // fresh stable vregs BEFORE cleanups, run cleanups, then Return the saved ones.
            VReg save_v = 0;
            if (is_slice_ret) {
                save_v = new_vreg(rv.ty);
                VReg save_len = new_vreg(&type_i64());
                ThinInstr& m1 = emit(ThinOp::Move, save_v, rv.vreg, 0, loc); m1.meta.type = rv.ty; m1.meta.width = 8;
                ThinInstr& m2 = emit(ThinOp::Move, save_len, rv.vreg + 1, 0, loc); m2.meta.type = &type_i64(); m2.meta.width = 8;
                (void)save_len;  // slice len = save_v+1 (consecutive vregs, slice convention)
            } else {
                save_v = new_vreg(rv.ty);
                ThinInstr& m = emit(ThinOp::Move, save_v, rv.vreg, 0, loc);
                m.meta.type = rv.ty; m.meta.width = value_bytes(rv.ty, ctx.structs);
                if (is_float_ret) m.meta.is_f32 = (rv.ty && rv.ty->prim == Prim::F32) ? 1 : 0;
            }
            emit_cleanups_to(0, loc);
            set_term_return(save_v);
            return;
        }
        if (has_defers) emit_cleanups_to(0, loc);
        set_term_return(rs->value ? rv.vreg : 0);
        return;
    }
    if (auto* is = dynamic_cast<const IfStmt*>(&s)) {
        LoweredValue cond = lower_expr(*is->cond);
        VReg zero = new_vreg(is->cond->ty);
        ThinInstr& z = emit(ThinOp::ConstInt, zero, 0, 0, loc); z.imm.i = 0; z.meta.type = is->cond->ty; z.meta.width = value_bytes(is->cond->ty, ctx.structs);
        VReg cbool = new_vreg(&type_bool());
        ThinInstr& cb = emit(ThinOp::Cmp, cbool, cond.vreg, zero, loc);
        cb.meta.cmp = 0; cb.meta.type = is->cond->ty; cb.meta.width = value_bytes(is->cond->ty, ctx.structs);
        cb.meta.is_unsigned = (is->cond->ty && is->cond->ty->is_uint()) ? 1 : 0;
        uint32_t then_bb = new_block(), else_bb = new_block();
        set_term_branch(cbool, else_bb, then_bb);  // cond==0 -> else, else then
        // then_bb
        enter_block(then_bb);
        lower_block(is->then_b);
        if (is->has_else) {
            uint32_t end_bb = new_block();
            if (cur_block().term.kind == TermKind::None) set_term_jmp(end_bb);
            // else_bb
            enter_block(else_bb);
            lower_block(is->else_b);
            if (cur_block().term.kind == TermKind::None) set_term_jmp(end_bb);
            enter_block(end_bb);
        } else {
            if (cur_block().term.kind == TermKind::None) set_term_jmp(else_bb);
            // else_bb falls through as the join
            enter_block(else_bb);
        }
        return;
    }
    if (auto* ws = dynamic_cast<const WhileStmt*>(&s)) {
        uint32_t top = new_block(), body_bb = new_block(), latch = new_block(), exit_bb = new_block();
        if (cur_block().term.kind == TermKind::None) set_term_jmp(top);
        // top: cond
        enter_block(top);
        {
            LoweredValue cond = lower_expr(*ws->cond);
            VReg zero = new_vreg(ws->cond->ty);
            ThinInstr& z = emit(ThinOp::ConstInt, zero, 0, 0, loc); z.imm.i = 0; z.meta.type = ws->cond->ty; z.meta.width = value_bytes(ws->cond->ty, ctx.structs);
            VReg cbool = new_vreg(&type_bool());
            ThinInstr& cb = emit(ThinOp::Cmp, cbool, cond.vreg, zero, loc);
            cb.meta.cmp = 0; cb.meta.type = ws->cond->ty; cb.meta.width = value_bytes(ws->cond->ty, ctx.structs);
            cb.meta.is_unsigned = (ws->cond->ty && ws->cond->ty->is_uint()) ? 1 : 0;
            set_term_branch(cbool, exit_bb, body_bb);  // cond==0 -> exit, else body
        }
        // body_bb: pin setup, body, then latch
        enter_block(body_bb);
        {
            bool set_pin_here = false;
            if (!active_pin) {
                auto pin_name = find_pin_candidate(ws->body);
                if (pin_name) {
                    // pin-entry: reload once per iteration (LoadFrame from the pin slot)
                    active_pin = PinState{*pin_name, locals[*pin_name]};
                    set_pin_here = true;
                }
            }
            loops.push_back({latch, exit_bb, false, cleanup_scopes.size()});
            lower_block(ws->body);
            loops.pop_back();
            if (set_pin_here) active_pin.reset();
            if (cur_block().term.kind == TermKind::None) set_term_jmp(latch);
        }
        // latch: budget back-edge, then jmp top
        enter_block(latch);
        emit_budget_check(block_cost(ws->body), loc);
        set_term_jmp(top);
        // exit_bb: continue
        enter_block(exit_bb);
        return;
    }
    if (auto* ds = dynamic_cast<const DoWhileStmt*>(&s)) {
        uint32_t body_bb = new_block(), cond_bb = new_block(), exit_bb = new_block();
        if (cur_block().term.kind == TermKind::None) set_term_jmp(body_bb);
        enter_block(body_bb);
        {
            loops.push_back({cond_bb, exit_bb, false, cleanup_scopes.size()});
            lower_block(ds->body);
            loops.pop_back();
            if (cur_block().term.kind == TermKind::None) set_term_jmp(cond_bb);
        }
        enter_block(cond_bb);
        {
            LoweredValue cond = lower_expr(*ds->cond);
            VReg zero = new_vreg(ds->cond->ty);
            ThinInstr& z = emit(ThinOp::ConstInt, zero, 0, 0, loc); z.imm.i = 0; z.meta.type = ds->cond->ty; z.meta.width = value_bytes(ds->cond->ty, ctx.structs);
            VReg cbool = new_vreg(&type_bool());
            ThinInstr& cb = emit(ThinOp::Cmp, cbool, cond.vreg, zero, loc);
            cb.meta.cmp = 0; cb.meta.type = ds->cond->ty; cb.meta.width = value_bytes(ds->cond->ty, ctx.structs);
            cb.meta.is_unsigned = (ds->cond->ty && ds->cond->ty->is_uint()) ? 1 : 0;
            // cond==0 -> exit; else body (back edge). Budget charged on the taken back edge,
            // so emit it in a tiny latch block BETWEEN cond-true and body (mirrors the
            // tree-walker: budget check, then jmp body).
            uint32_t latch = new_block();
            set_term_branch(cbool, exit_bb, latch);
            enter_block(latch);
            emit_budget_check(block_cost(ds->body), loc);
            set_term_jmp(body_bb);
        }
        enter_block(exit_bb);
        return;
    }
    if (auto* fs = dynamic_cast<const ForStmt*>(&s)) {
        auto saved_locals = locals;
        auto saved_types = local_types;
        if (fs->init) lower_stmt(*fs->init);
        uint32_t cond_top = new_block(), body_bb = new_block(), step_bb = new_block(), end_bb = new_block();
        if (cur_block().term.kind == TermKind::None) set_term_jmp(cond_top);
        enter_block(cond_top);
        {
            if (fs->cond) {
                LoweredValue cond = lower_expr(*fs->cond);
                VReg zero = new_vreg(fs->cond->ty);
                ThinInstr& z = emit(ThinOp::ConstInt, zero, 0, 0, loc); z.imm.i = 0; z.meta.type = fs->cond->ty; z.meta.width = value_bytes(fs->cond->ty, ctx.structs);
                VReg cbool = new_vreg(&type_bool());
                ThinInstr& cb = emit(ThinOp::Cmp, cbool, cond.vreg, zero, loc);
                cb.meta.cmp = 0; cb.meta.type = fs->cond->ty; cb.meta.width = value_bytes(fs->cond->ty, ctx.structs);
                cb.meta.is_unsigned = (fs->cond->ty && fs->cond->ty->is_uint()) ? 1 : 0;
                set_term_branch(cbool, end_bb, body_bb);
            } else {
                set_term_jmp(body_bb);
            }
        }
        enter_block(body_bb);
        {
            bool set_pin_here = false;
            if (!active_pin) {
                auto pin_name = find_pin_candidate(fs->body);
                if (pin_name) {
                    active_pin = PinState{*pin_name, locals[*pin_name]};
                    set_pin_here = true;
                }
            }
            loops.push_back({step_bb, end_bb, false, cleanup_scopes.size()});
            lower_block(fs->body);
            loops.pop_back();
            if (set_pin_here) active_pin.reset();
            if (cur_block().term.kind == TermKind::None) set_term_jmp(step_bb);
        }
        enter_block(step_bb);
        {
            if (fs->step) lower_expr(*fs->step);
            emit_budget_check(block_cost(fs->body), loc);
            set_term_jmp(cond_top);
        }
        enter_block(end_bb);
        locals = std::move(saved_locals);
        local_types = std::move(saved_types);
        return;
    }
    if (auto* sw = dynamic_cast<const SwitchStmt*>(&s)) {
        // compare chain + per-case bodies + join (mirrors tree-walker's switch lowering)
        LoweredValue subj = lower_expr(*sw->subject);
        std::vector<uint32_t> case_labels;
        for (size_t i = 0; i < sw->cases.size(); ++i) case_labels.push_back(new_block());
        uint32_t end_label = new_block();
        int default_idx = -1;
        for (size_t i = 0; i < sw->cases.size(); ++i) {
            if (sw->cases[i].is_default) { default_idx = int(i); continue; }
            LoweredValue cv = lower_expr(*sw->cases[i].value);
            VReg cbool = new_vreg(&type_bool());
            ThinInstr& cb = emit(ThinOp::Cmp, cbool, subj.vreg, cv.vreg, loc);
            cb.meta.cmp = 0; cb.meta.type = sw->subject->ty; cb.meta.width = value_bytes(sw->subject->ty, ctx.structs);
            cb.meta.is_unsigned = (sw->subject->ty && sw->subject->ty->is_uint()) ? 1 : 0;
            // branch to case_labels[i] if equal, else continue to next compare in a new block
            uint32_t next_cmp = new_block();
            set_term_branch(cbool, case_labels[i], next_cmp);
            enter_block(next_cmp);
        }
        // after the chain: default or end
        set_term_jmp(default_idx >= 0 ? case_labels[size_t(default_idx)] : end_label);
        for (size_t i = 0; i < sw->cases.size(); ++i) {
            enter_block(case_labels[i]);
            loops.push_back({0, end_label, true, cleanup_scopes.size()});  // break-only
            lower_block(sw->cases[i].body);
            loops.pop_back();
            if (cur_block().term.kind == TermKind::None) {
                // sema requires each nonempty body to end in break/return/continue, so a
                // fallthrough here is unreachable in a sema-clean program. Jmp to end as a
                // safe default (the tree-walker falls through; both are unreachable).
                set_term_jmp(end_label);
            }
        }
        enter_block(end_label);
        return;
    }
    if (auto* bs = dynamic_cast<const BlockStmt*>(&s)) { lower_block(bs->block); return; }
    if (dynamic_cast<const BreakStmt*>(&s)) {
        if (!loops.empty()) {
            emit_cleanups_to(loops.back().cleanup_depth, loc);
            set_term_jmp(loops.back().exit_bb);
            // start a fresh (unreachable) block so subsequent lowering has somewhere to go
            new_and_enter();
        }
        return;
    }
    if (dynamic_cast<const ContinueStmt*>(&s)) {
        // skip past enclosing switch frames; continue targets the nearest real loop
        for (int i = int(loops.size()) - 1; i >= 0; --i) {
            if (!loops[size_t(i)].is_switch) {
                emit_cleanups_to(loops[size_t(i)].cleanup_depth, loc);
                set_term_jmp(loops[size_t(i)].cond_bb);
                new_and_enter();
                break;
            }
        }
        return;
    }
}

} // namespace

ThinFunction lower_function(const FuncDecl& f, const CodeGenCtx& ctx) {
    ThinLowerer lw(ctx, f);
    return lw.run();
}

} // namespace ember
