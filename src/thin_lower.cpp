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
#include "safety.hpp"

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

// Maximum supported JIT stack frame size (64 MiB). Frame-extent arithmetic
// is done in int64_t/size_t + checked against this ceiling before the
// checked-cast back to int32_t, so a pathological/malformed IR that would
// overflow int32 frame offsets (wrapping to a tiny frame with huge negative
// offsets) is caught + rejected as a hard lowering error instead of
// miscompiling. 64 MiB is far beyond any real ember frame (largest test
// frames are a few KB) but small enough that int32 offsets stay valid.
static constexpr int64_t MAX_FRAME_SIZE = (int64_t(1) << 26);

// Checked cast: int64 -> int32, clamped to MAX_FRAME_SIZE. Returns false
// (leaving *out = 0) if `v` exceeds the supported frame-size ceiling or the
// int32 range, so the caller can flag a hard lowering error.
inline bool checked_frame_cast(int64_t v, int32_t& out) {
    if (v < 0 || v > MAX_FRAME_SIZE) return false;
    out = static_cast<int32_t>(v);
    return true;
}

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
    int32_t try_depth = 0;  // active_try_depth at loop entry (break/continue unwind)
};

struct PinState { std::string name; int32_t offset = 0; };

// The lowerer. One instance per function.
struct ThinLowerer {
    const CodeGenCtx& ctx;
    const FuncDecl& f;
    ThinFunction out;
    int lower_depth = 0;
    static constexpr int MAX_COMPILE_DEPTH = 4000;

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
    // for-each / match internal-slot unique-suffix counters (mirrors CG's
    // fe_counter; match needs one too for the subject slot + struct-destructure
    // capture locals). A nested for-each/match would otherwise overwrite the
    // outer's locals-map entry.
    int32_t fe_counter = 0;
    int32_t match_counter = 0;
    std::vector<std::shared_ptr<Type>> arr_temp_types;
    std::vector<std::shared_ptr<Type>> str_temp_types;

    // --- rodata (StringLit bytes) ---
    // (carried into out.rodata)

    // --- defer / cleanup ---
    std::vector<DeferSite> defer_sites;
    std::unordered_map<const DeferStmt*, size_t> defer_site_indices;
    std::vector<CleanupScope> cleanup_scopes;

    // Tier 4 try/catch: the number of active try handlers on the catch stack
    // at the current point (mirrors CG::active_try_depth). A return/break/
    // continue that exits one or more enclosing try bodies must pop
    // catch_depth by the delta before transferring control (mirrors CG's
    // emit_catch_unwind_to), or the catch stack leaks across calls (catch_depth
    // lives in the persistent context_t). The TryCatchStmt lowering pushes a
    // TryCatch (catch_depth++) + increments this; the normal-completion pop is
    // a CatchCleanup(imm=1) + the delta is decremented back.
    int32_t active_try_depth = 0;

    // --- loop / pin ---
    std::vector<LoopCtx> loops;
    std::optional<PinState> active_pin;

    // --- obf ---
    ObfOptions obf;
    bool non_serializable = false;
    std::string non_serializable_reason;

    // #20 lambda capture map (set when compiling a synthetic lambda fn body):
    // capture name -> (byte offset within env, type, by_ref). The env_ptr is
    // the __env param (params[0]), whose frame slot offset is lambda_env_off.
    // The Ident lowering loads a capture as: load env_ptr from
    // [frame + lambda_env_off], then load the value at [env_ptr + offset].
    // A by_ref capture's env slot holds a POINTER to the captured variable's
    // storage (not a copy), so the read is a DOUBLE dereference (load ptr from
    // [env_ptr+offset], then load value from [ptr]) and a write stores THROUGH
    // the pointer. Mirrors CG::compiling_lambda / lambda_captures /
    // lambda_env_off (codegen.cpp:177-180).
    bool compiling_lambda = false;
    struct CaptureInfo { int32_t offset; const Type* ty; bool by_ref; };
    std::unordered_map<std::string, CaptureInfo> lambda_captures;
    int32_t lambda_env_off = 0;  // frame slot offset of the __env param

    ThinLowerer(const CodeGenCtx& c, const FuncDecl& fn) : ctx(c), f(fn) {
        obf = c.obf;
    }

    // ─────────────── type helpers (mechanical copies of CG) ───────────────

    static int32_t value_bytes(const Type* t, const StructLayoutTable* structs) {
        if (!t) return 8;
        if (t->is_slice || t->is_lambda) return 16;
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
        if (t && (t->is_slice || t->is_lambda || t->array_len > 0 ||
                  (!t->struct_name.empty() && structs && structs->count(t->struct_name))))
            return value_bytes(t, structs);
        return 8;
    }
    static int32_t words_for_type(const Type* t, const StructLayoutTable* structs) {
        if (t && (t->is_slice || t->is_lambda)) return 2;
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
    // Look up a native binding by name from ctx.natives (mirrors CG::native_named).
    const NativeSig* native_named(const std::string& name) const {
        if (!ctx.natives || name.empty()) return nullptr;
        auto it = ctx.natives->find(name);
        return it == ctx.natives->end() ? nullptr : &it->second;
    }

    // ─────────────── locals + temp allocation (mirrors CG) ───────────────

    int32_t alloc_local(const std::string& n, const Type* t) {
        int32_t width = local_width_bytes(t, ctx.structs);
        // Accumulate in int64 + check against MAX_FRAME_SIZE before assigning
        // back to the int32 next_local_off, so a pathological count of locals
        // (or a malformed huge array type) overflows loudly instead of wrapping
        // next_local_off to a small positive value (which would make `off`
        // point into the caller's frame). On overflow, set non_serializable so
        // the function bails to the tree-walker (x86) / hard error (arm64).
        int64_t acc = int64_t(next_local_off) + int64_t(width);
        if (acc > MAX_FRAME_SIZE) {
            non_serializable = true;
            if (non_serializable_reason.empty())
                non_serializable_reason = "stack frame exceeds supported maximum (local/temp allocation overflow)";
            next_local_off = int32_t(MAX_FRAME_SIZE);  // poison: any further alloc also fails
            return -int32_t(MAX_FRAME_SIZE);
        }
        next_local_off = int32_t(acc);
        int32_t off = -next_local_off;
        locals[n] = off;
        local_types[n] = t;
        // Precise GC: a lambda-typed local/param is 16 bytes {slot, env_ptr};
        // the env_ptr (second word at off+8) is a GC object pointer when the
        // env is on the GC heap (use_gc_env). A no-capture lambda's env_ptr is
        // null (safely ignored), so recording every lambda slot's env_ptr word
        // is conservative + correct. Matches the tree-walker's alloc_local hook.
        if (ctx.use_gc_env && t && t->is_lambda) add_gc_ptr_slot(off + 8);
        return off;
    }
    // Record a frame slot (rbp-relative ABSOLUTE negative offset) holding a GC
    // object pointer into the frame plan's gc_ptr_frame_offs (dedup'd). No-op
    // when precise GC is off (use_gc_env false). Consumed by emit (c3) to build
    // the CompiledFn's GcFrameMap.
    void add_gc_ptr_slot(int32_t off) {
        if (!ctx.use_gc_env) return;
        for (int32_t o : out.frame.gc_ptr_frame_offs) if (o == off) return;  // dedup
        out.frame.gc_ptr_frame_offs.push_back(off);
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
    // #20: alloc a frame temp of `env_size` (rounded to 8) bytes for a lambda's
    // stack env (a fixed-array-of-u8 backing, mirroring CG's __envtmp$N).
    int32_t alloc_env_temp(int32_t env_size) {
        std::string name = "__envtmp$" + std::to_string(temp_counter++);
        int32_t rounded = (env_size + 7) & ~7;
        auto bt = std::make_shared<Type>(make_prim(Prim::U8));
        Type t; t.prim = Prim::U8; t.array_len = uint32_t(rounded); t.elem = bt;
        arr_temp_types.push_back(std::make_shared<Type>(std::move(t)));
        return alloc_local(name, arr_temp_types.back().get());
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

    void prescan_block(const Block& b) {
        safety::DepthGuard guard(lower_depth, MAX_COMPILE_DEPTH, "thin_lower::prescan_block");
        for (auto& s : b.stmts) prescan_stmt(*s);
    }
    void prescan_stmt(const Stmt& s) {
        safety::DepthGuard guard(lower_depth, MAX_COMPILE_DEPTH, "thin_lower::prescan_stmt");
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
        if (auto* ms = dynamic_cast<const MatchStmt*>(&s)) {
            prescan_expr(*ms->subject);
            for (auto& arm : ms->arms) {
                if (arm.guard) prescan_expr(*arm.guard);
                prescan_block(arm.body);
            }
            return;
        }
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
        // Tier 4: try/catch recurses both bodies; throw prescans its expr.
        if (auto* tc = dynamic_cast<const TryCatchStmt*>(&s)) {
            prescan_block(tc->try_body);
            prescan_block(tc->catch_body);
            return;
        }
        if (auto* th = dynamic_cast<const ThrowStmt*>(&s)) {
            if (th->value) prescan_expr(*th->value);
            return;
        }
        // #21 coroutines (Phase 8): yield lowers to a 1-arg __ember_coro_yield
        // native call, so it makes calls + needs a 1-word arg-spill area.
        if (auto* ys = dynamic_cast<const YieldStmt*>(&s)) {
            makes_calls = true;
            max_args = std::max(max_args, 1);
            if (ys->value) prescan_expr(*ys->value);
            return;
        }
    }
    void prescan_expr(const Expr& ex) {
        safety::DepthGuard guard(lower_depth, MAX_COMPILE_DEPTH, "thin_lower::prescan_expr");
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

    void count_struct_temps_block(const Block& b, int32_t& total) {
        safety::DepthGuard guard(lower_depth, MAX_COMPILE_DEPTH, "thin_lower::count_struct_temps_block");
        for (auto& s : b.stmts) count_struct_temps_stmt(*s, total);
    }
    void count_struct_temps_stmt(const Stmt& s, int32_t& total) {
        safety::DepthGuard guard(lower_depth, MAX_COMPILE_DEPTH, "thin_lower::count_struct_temps_stmt");
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
        if (auto* fe = dynamic_cast<const ForEachStmt*>(&s)) { count_struct_temps_expr(*fe->iter, total); count_struct_temps_block(fe->body, total); return; }
        if (auto* ms = dynamic_cast<const MatchStmt*>(&s)) {
            count_struct_temps_expr(*ms->subject, total);
            for (auto& arm : ms->arms) {
                if (arm.guard) count_struct_temps_expr(*arm.guard, total);
                if (arm.pattern) count_struct_temps_expr(*arm.pattern, total);
                count_struct_temps_block(arm.body, total);
            }
            return;
        }
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
        if (auto* tc = dynamic_cast<const TryCatchStmt*>(&s)) {
            count_struct_temps_block(tc->try_body, total);
            count_struct_temps_block(tc->catch_body, total);
            return;
        }
        if (auto* th = dynamic_cast<const ThrowStmt*>(&s)) { if (th->value) count_struct_temps_expr(*th->value, total); return; }
        if (auto* ys = dynamic_cast<const YieldStmt*>(&s)) { if (ys->value) count_struct_temps_expr(*ys->value, total); return; }
    }
    void count_struct_temps_expr(const Expr& ex, int32_t& total) {
        safety::DepthGuard guard(lower_depth, MAX_COMPILE_DEPTH, "thin_lower::count_struct_temps_expr");
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
        // #20: a LambdaExpr allocs a __envtmp$N frame temp of env_size bytes
        // (rounded up to 8). Count it so the frame is sized to hold the env.
        // GC path (use_gc_env): the env itself lives on the GC heap; the frame
        // only needs an 8-byte slot to hold the env_ptr returned by
        // __ember_gc_alloc_env, so count 8 (not env_size). Mirrors CG
        // (codegen.cpp:1110-1117).
        if (auto* le = dynamic_cast<const LambdaExpr*>(&ex)) {
            if (le->env_size > 0)
                total += ctx.use_gc_env ? 8 : int32_t((le->env_size + 7) & ~7);
            return;
        }
    }

    void count_arr_temps_block(const Block& b, int32_t& total) {
        safety::DepthGuard guard(lower_depth, MAX_COMPILE_DEPTH, "thin_lower::count_arr_temps_block");
        for (auto& s : b.stmts) count_arr_temps_stmt(*s, total);
    }
    void count_arr_temps_stmt(const Stmt& s, int32_t& total) {
        safety::DepthGuard guard(lower_depth, MAX_COMPILE_DEPTH, "thin_lower::count_arr_temps_stmt");
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
        if (auto* fe = dynamic_cast<const ForEachStmt*>(&s)) { count_arr_temps_expr(*fe->iter, total); count_arr_temps_block(fe->body, total); return; }
        if (auto* ms = dynamic_cast<const MatchStmt*>(&s)) {
            count_arr_temps_expr(*ms->subject, total);
            for (auto& arm : ms->arms) {
                if (arm.guard) count_arr_temps_expr(*arm.guard, total);
                if (arm.pattern) count_arr_temps_expr(*arm.pattern, total);
                count_arr_temps_block(arm.body, total);
            }
            return;
        }
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
        if (auto* tc = dynamic_cast<const TryCatchStmt*>(&s)) {
            count_arr_temps_block(tc->try_body, total);
            count_arr_temps_block(tc->catch_body, total);
            return;
        }
        if (auto* th = dynamic_cast<const ThrowStmt*>(&s)) { if (th->value) count_arr_temps_expr(*th->value, total); return; }
        if (auto* ys = dynamic_cast<const YieldStmt*>(&s)) { if (ys->value) count_arr_temps_expr(*ys->value, total); return; }
    }
    void count_arr_temps_expr(const Expr& ex, int32_t& total) {
        safety::DepthGuard guard(lower_depth, MAX_COMPILE_DEPTH, "thin_lower::count_arr_temps_expr");
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

    void count_str_temps_block(const Block& b, int32_t& total) {
        safety::DepthGuard guard(lower_depth, MAX_COMPILE_DEPTH, "thin_lower::count_str_temps_block");
        for (auto& s : b.stmts) count_str_temps_stmt(*s, total);
    }
    void count_str_temps_stmt(const Stmt& s, int32_t& total) {
        safety::DepthGuard guard(lower_depth, MAX_COMPILE_DEPTH, "thin_lower::count_str_temps_stmt");
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
        if (auto* fe = dynamic_cast<const ForEachStmt*>(&s)) { count_str_temps_expr(*fe->iter, total); count_str_temps_block(fe->body, total); return; }
        if (auto* ms = dynamic_cast<const MatchStmt*>(&s)) {
            count_str_temps_expr(*ms->subject, total);
            for (auto& arm : ms->arms) {
                if (arm.guard) count_str_temps_expr(*arm.guard, total);
                if (arm.pattern) count_str_temps_expr(*arm.pattern, total);
                count_str_temps_block(arm.body, total);
            }
            return;
        }
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
        if (auto* tc = dynamic_cast<const TryCatchStmt*>(&s)) {
            count_str_temps_block(tc->try_body, total);
            count_str_temps_block(tc->catch_body, total);
            return;
        }
        if (auto* th = dynamic_cast<const ThrowStmt*>(&s)) { if (th->value) count_str_temps_expr(*th->value, total); return; }
        if (auto* ys = dynamic_cast<const YieldStmt*>(&s)) { if (ys->value) count_str_temps_expr(*ys->value, total); return; }
    }
    void count_str_temps_expr(const Expr& ex, int32_t& total) {
        safety::DepthGuard guard(lower_depth, MAX_COMPILE_DEPTH, "thin_lower::count_str_temps_expr");
        if (auto* lit = dynamic_cast<const StringLit*>(&ex)) {
            // The StringLit lowering (lower_expr's StringLit case) ALWAYS
            // allocates a 16-byte {ptr,len} slice frame slot (encrypted or
            // not — the slice must be frame-backed because the ConstInt(len)
            // that follows clobbers rax where ptr lives). An encrypted literal
            // additionally allocates baked_len bytes for the decrypted-data
            // buffer (alloc_str_temp). An implicit-to-string literal
            // additionally allocates an 8-byte handle slot for the CallNative
            // result (frame-backed across the next DepthCheck). The frame-size
            // pre-count MUST account for all three or the lowerer writes past
            // frame_size (the validator's frame_off-span check then rejects
            // the lowered IR — the marker-baseline failure).
            total += 16;  // slice {ptr,len} slot (always)
            if (lit->encrypted && lit->baked_key != 0 && lit->baked_len > 0)
                total += int32_t(lit->baked_len);  // decrypted-data buffer
            if (lit->implicit_to_string && lit->to_string_native_fn)
                total += 8;  // string-handle CallNative result slot
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
    void count_logical_temps_block(const Block& b, int32_t& total) {
        safety::DepthGuard guard(lower_depth, MAX_COMPILE_DEPTH, "thin_lower::count_logical_temps_block");
        for (auto& s : b.stmts) count_logical_temps_stmt(*s, total);
    }
    void count_logical_temps_stmt(const Stmt& s, int32_t& total) {
        safety::DepthGuard guard(lower_depth, MAX_COMPILE_DEPTH, "thin_lower::count_logical_temps_stmt");
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
        if (auto* fe = dynamic_cast<const ForEachStmt*>(&s)) { count_logical_temps_expr(*fe->iter, total); count_logical_temps_block(fe->body, total); return; }
        if (auto* ms = dynamic_cast<const MatchStmt*>(&s)) {
            count_logical_temps_expr(*ms->subject, total);
            for (auto& arm : ms->arms) {
                if (arm.guard) count_logical_temps_expr(*arm.guard, total);
                if (arm.pattern) count_logical_temps_expr(*arm.pattern, total);
                count_logical_temps_block(arm.body, total);
            }
            return;
        }
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
        if (auto* tc = dynamic_cast<const TryCatchStmt*>(&s)) {
            count_logical_temps_block(tc->try_body, total);
            count_logical_temps_block(tc->catch_body, total);
            return;
        }
        if (auto* th = dynamic_cast<const ThrowStmt*>(&s)) { if (th->value) count_logical_temps_expr(*th->value, total); return; }
        if (auto* ys = dynamic_cast<const YieldStmt*>(&s)) { if (ys->value) count_logical_temps_expr(*ys->value, total); return; }
    }
    void count_logical_temps_expr(const Expr& ex, int32_t& total) {
        safety::DepthGuard guard(lower_depth, MAX_COMPILE_DEPTH, "thin_lower::count_logical_temps_expr");
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
            if (auto* fe = dynamic_cast<const ForEachStmt*>(s.get())) collect_defers(fe->body);
            if (auto* bs = dynamic_cast<const BlockStmt*>(s.get())) collect_defers(bs->block);
            if (auto* fs = dynamic_cast<const ForStmt*>(s.get())) collect_defers(fs->body);
            if (auto* sw = dynamic_cast<const SwitchStmt*>(s.get())) for (auto& c : sw->cases) collect_defers(c.body);
            if (auto* ms = dynamic_cast<const MatchStmt*>(s.get())) for (auto& arm : ms->arms) collect_defers(arm.body);
            if (auto* tc = dynamic_cast<const TryCatchStmt*>(s.get())) { collect_defers(tc->try_body); collect_defers(tc->catch_body); }
        }
    }

    // Item E pin-candidate selection (mechanical copy of CG::find_pin_candidate).
    void count_pin_refs_block(const Block& b, std::unordered_map<std::string,int>& counts) {
        safety::DepthGuard guard(lower_depth, MAX_COMPILE_DEPTH, "thin_lower::count_pin_refs_block");
        for (auto& s : b.stmts) count_pin_refs_stmt(*s, counts);
    }
    void count_pin_refs_stmt(const Stmt& s, std::unordered_map<std::string,int>& counts) {
        safety::DepthGuard guard(lower_depth, MAX_COMPILE_DEPTH, "thin_lower::count_pin_refs_stmt");
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
        if (auto* fe = dynamic_cast<const ForEachStmt*>(&s)) { count_pin_refs_expr(*fe->iter, counts); count_pin_refs_block(fe->body, counts); return; }
        if (auto* ms = dynamic_cast<const MatchStmt*>(&s)) {
            count_pin_refs_expr(*ms->subject, counts);
            for (auto& arm : ms->arms) {
                if (arm.guard) count_pin_refs_expr(*arm.guard, counts);
                if (arm.pattern) count_pin_refs_expr(*arm.pattern, counts);
                count_pin_refs_block(arm.body, counts);
            }
            return;
        }
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
        if (auto* tc = dynamic_cast<const TryCatchStmt*>(&s)) {
            count_pin_refs_block(tc->try_body, counts);
            count_pin_refs_block(tc->catch_body, counts);
            return;
        }
        if (auto* th = dynamic_cast<const ThrowStmt*>(&s)) { if (th->value) count_pin_refs_expr(*th->value, counts); return; }
        if (auto* ys = dynamic_cast<const YieldStmt*>(&s)) { if (ys->value) count_pin_refs_expr(*ys->value, counts); return; }
    }
    void count_pin_refs_expr(const Expr& ex, std::unordered_map<std::string,int>& counts) {
        safety::DepthGuard guard(lower_depth, MAX_COMPILE_DEPTH, "thin_lower::count_pin_refs_expr");
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
        safety::DepthGuard guard(lower_depth, MAX_COMPILE_DEPTH, "thin_lower::block_cost");
        int64_t n = 0;
        for (auto& s : b.stmts) n = cost_add(n, cost_add(1, stmt_cost(*s)));
        return n < 1 ? 1 : n;
    }
    int64_t expr_cost(const Expr& ex) {
        safety::DepthGuard guard(lower_depth, MAX_COMPILE_DEPTH, "thin_lower::expr_cost");
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
        safety::DepthGuard guard(lower_depth, MAX_COMPILE_DEPTH, "thin_lower::stmt_cost");
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
        if (auto* fe = dynamic_cast<const ForEachStmt*>(&s))
            return cost_add(cost_add(cost_add(1, fe->iter ? expr_cost(*fe->iter) : 0), 1), block_cost(fe->body));
        if (auto* sw = dynamic_cast<const SwitchStmt*>(&s)) {
            int64_t n = sw->subject ? expr_cost(*sw->subject) : 0;
            n = cost_add(n, int64_t(sw->cases.size()));
            for (auto& c : sw->cases) n = cost_add(n, block_cost(c.body));
            return n;
        }
        if (auto* ms = dynamic_cast<const MatchStmt*>(&s)) {
            int64_t n = ms->subject ? expr_cost(*ms->subject) : 0;
            n = cost_add(n, int64_t(ms->arms.size()));  // compare chain
            for (auto& arm : ms->arms) {
                if (arm.guard) n = cost_add(n, expr_cost(*arm.guard));
                if (arm.pattern) n = cost_add(n, expr_cost(*arm.pattern));
                n = cost_add(n, block_cost(arm.body));
            }
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
        if (t && (t->is_slice || t->is_lambda)) {
            // A lambda value is {slot, env_ptr} — same 2-vreg shape as a slice.
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
    void store_scalar_local(const LoweredValue& lv, int32_t off, Loc loc,
                            bool exact_width = false) {
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
        // Scalar locals use eight-byte frame slots, but aggregate fields are
        // packed and must be written at their exact type width. field_off is
        // otherwise unused by StoreFrame and records that distinction.
        if (exact_width) in.meta.field_off = 1;
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
    // Tier 4: pop catch_depth by (active_try_depth - retained) before a
    // return/break/continue that exits one or more enclosing try handlers.
    // Emits a CatchCleanup with imm = the pop count (0 = no try handlers to
    // unwind -> emits nothing). Mirrors CG::emit_catch_unwind_to.
    void emit_catch_unwind(int32_t retained, Loc loc) {
        int32_t pops = active_try_depth - retained;
        if (pops <= 0) return;
        ThinInstr& in = emit(ThinOp::CatchCleanup, 0, 0, 0, loc);
        in.imm.i = int64_t(pops);
        in.meta.type = &type_i64(); in.meta.width = 8;
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

        // ForEachStmt + MatchStmt are now lowered to ThinFunction IR (Phase
        // 6d). No non_serializable gate here — the prescan/count/lowering passes
        // all handle them. struct-destructure match arms whose lowering is not
        // yet implemented set non_serializable per-arm (see lower_stmt).
        //
        // #21 coroutines (plan_MACOS_ARM64.md Phase 8): `yield` IS lowered to
        // ThinFunction IR here — as a 1-arg CallNative to __ember_coro_yield
        // (see lower_stmt's YieldStmt case). On ARM64 (ThinIR-only, no tree-
        // walker) this is the ONLY path, so the old "fall back to tree-walker"
        // gate is removed. The native performs the cooperative context switch
        // (ember_ctx_switch on Darwin / SwitchToFiber on Windows); the IR just
        // emits the call + continues after the resume. prescan_stmt sets
        // makes_calls/max_args for the yield native call so the frame's arg-
        // spill area is sized.

        // #20: if this is a synthetic lambda fn, set up the capture map so the
        // Ident lowering loads captures from [env_ptr + offset]. The __env
        // param is params[0]; its frame slot is assigned during the param-spill
        // plan below (recorded into lambda_env_off). Mirrors compile_tree_walker_
        // (codegen.cpp:5474-5481).
        if (f.is_lambda) {
            compiling_lambda = true;
            for (size_t i = 0; i < f.lambda_captures.size(); ++i) {
                lambda_captures[f.lambda_captures[i]] = {
                    f.lambda_capture_offsets[i],
                    f.lambda_capture_types[i].get(),
                    i < f.lambda_capture_by_ref.size() && f.lambda_capture_by_ref[i]
                };
            }
        }

        // --- frame plan (mirror compile_func) ---
        prescan_block(f.body);

        next_local_off = 0;
        next_local_off += 8;            // rbx save slot
        rbx_save_offset = -next_local_off;  // -8
        // Precise GC: reserve a 24-byte GcFrameRecord region (prev/frame_base/
        // map) right after rbx_save so its offsets are deterministic. Only when
        // use_gc_env (the GC heap env backend); otherwise no record + no
        // maintenance (byte-identical to the pre-GC IR path).
        int32_t gc_rec_off = 0, gc_rec_base_off = 0, gc_rec_map_off = 0;
        if (ctx.use_gc_env) {
            next_local_off += 24;
            gc_rec_off = -next_local_off;
            gc_rec_base_off = gc_rec_off + 8;
            gc_rec_map_off = gc_rec_off + 16;
        }

        int32_t locals_area = 8;        // rbx save
        if (ctx.use_gc_env) locals_area += 24;  // GC frame record region
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
                if (auto* fe = dynamic_cast<const ForEachStmt*>(s.get())) {
                    // for-each allocates: (handle|ptr)(8) + len(8) + idx(8) +
                    // var(elem_width). 24 covers the three i64 internal slots;
                    // the var slot width is local_width_bytes(elem_ty) (8 for a
                    // scalar). Mirrors CG's sum_bytes ForEachStmt (array-handle +
                    // slice both allocate 24 + var).
                    const Type* et = fe->array_elem_ty
                        ? fe->array_elem_ty
                        : (fe->iter && fe->iter->ty && fe->iter->ty->elem ? fe->iter->ty->elem.get() : nullptr);
                    locals_area += 24;  // (handle|ptr) + len + idx
                    locals_area += local_width_bytes(et, ctx.structs);  // var
                    sum_bytes(fe->body);
                }
                if (auto* bs = dynamic_cast<const BlockStmt*>(s.get())) sum_bytes(bs->block);
                if (auto* fs = dynamic_cast<const ForStmt*>(s.get())) {
                    if (fs->init) locals_area += local_width_bytes(fs->init->init ? fs->init->init->ty : fs->init->ty.get(), ctx.structs);
                    sum_bytes(fs->body);
                }
                if (auto* sw = dynamic_cast<const SwitchStmt*>(s.get()))
                    for (auto& c : sw->cases) sum_bytes(c.body);
                if (auto* ms = dynamic_cast<const MatchStmt*>(s.get())) {
                    // match allocates a subject frame slot for the literal/enum
                    // path (the IR holds the subject across the compare chain in
                    // a frame slot, unlike the tree-walker which holds it in r10).
                    // struct-destructure match uses the subject's existing local
                    // slot, so the 8 bytes is a conservative over-reservation.
                    // Recurse into each arm body for nested locals (a general
                    // arm can contain LetStmts).
                    locals_area += 8;  // subject slot
                    for (auto& arm : ms->arms) sum_bytes(arm.body);
                }
                // Tier 4: each try/catch allocates one i64 catch-variable slot
                // (alloc_local in lower_stmt's TryCatchStmt case). Recurse into
                // both try + catch bodies for nested locals (mirrors CG's
                // sum_bytes).
                if (auto* tc = dynamic_cast<const TryCatchStmt*>(s.get())) {
                    locals_area += 8;  // catch_name (i64)
                    sum_bytes(tc->try_body);
                    sum_bytes(tc->catch_body);
                }
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
        // Frame-extent arithmetic in int64 + checked against MAX_FRAME_SIZE
        // before the round16 + int32 assignment, so a pathological locals_area
        // / arg_temps_area overflow wraps loudly instead of producing a tiny
        // frame_size with huge negative offsets.
        int64_t total64 = int64_t(locals_area) + int64_t(arg_temps_area) + 16;
        int32_t total32 = 0;
        if (!checked_frame_cast(total64, total32)) {
            non_serializable = true;
            non_serializable_reason = "stack frame exceeds supported maximum (locals + arg temps overflow)";
        }
        frame_size = round16(total32);

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
            if (f.is_lambda && i == 0) {
                lambda_env_off = off;  // #20: record __env's frame slot
                // Precise GC: the __env param holds the heap env pointer (a GC
                // object pointer) for the lambda fn's whole frame. Record it
                // (mirrors CG codegen.cpp:5710-5712).
                if (ctx.use_gc_env) add_gc_ptr_slot(off);
            }
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
        out.frame.gc_rec_off = gc_rec_off;
        out.frame.gc_rec_base_off = gc_rec_base_off;
        out.frame.gc_rec_map_off = gc_rec_map_off;
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
            // run remaining cleanups through depth 0, unwind any active try
            // handlers, then return void
            emit_cleanups_to(0, f.loc);
            emit_catch_unwind(0, f.loc);
            set_term_return(0);
        }

        // Defensive fallback: if any expression lowering hit the unhandled-node
        // default in lower_expr (a future Expr type, or an EnumAccessExpr that
        // escaped the sema pre-pass), it set non_serializable mid-lowering with
        // a poison return value. Bail to the tree-walker rather than emit IR
        // containing a silent zero/scratch value. The tree-walker's eval() has
        // its own defensive trap for the same case, so a miscompile stays loud.
        if (non_serializable) {
            out.non_serializable = true;
            out.non_serializable_reason = non_serializable_reason.empty()
                ? std::string("unhandled expression node in IR lowering; falling back to tree-walker")
                : non_serializable_reason;
            out.blocks.clear();
            return out;
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
        // local, a temp, or an arg temp. Base = next_local_off + arg_temps_area;
        // each spill slot grows downward from there.
        //
        // gap 2j (aggregate_global): use the ACTUAL post-lowering next_local_off
        // (not the pre-computed locals_area) as the base. locals_area is sized
        // upfront from count_*_temps_block, but a few ad-hoc frame temps are
        // allocated DURING lowering via alloc_local and are NOT counted there —
        // notably the __gld$N 16-byte slot for a global slice/lambda Ident load
        // (lower_expr's Ident case). With locals_area, the first spill slot
        // could land on an ad-hoc temp's word (e.g. the __gld$ len half at
        // tmp+8), so a later scalar spill overwrote the len and a slice bounds
        // check read idx==len and trapped (SIGILL). Basing on next_local_off
        // (which every alloc_local grows) places spill slots BELOW every actually-
        // allocated local/temp, eliminating the collision. When no ad-hoc temps
        // exist next_local_off == locals_area, so this is byte-identical to the
        // prior formula for every function that already worked.
        // Frame-extent arithmetic in int64 + checked against MAX_FRAME_SIZE
        // (then checked-cast to int32 for the spill offsets / frame_size) so a
        // pathological spill-slot count overflows loudly instead of wrapping
        // spill_top to a small value (which would make `slot` point into the
        // locals/arg-temps region, corrupting them). For normal functions the
        // values are identical to the old int32 arithmetic.
        int64_t spill_base = int64_t(next_local_off) + int64_t(arg_temps_area);
        int64_t spill_top = spill_base;
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
            case ThinOp::LoadFrame:
                // computed-address loads (src1 != 0, e.g. array element reads)
                // need a spill slot for the result so regalloc doesn't lose it.
                // Ordinary frame loads (src1 == 0) already have meta.frame_off.
                if (in.src1 != 0 && !(in.meta.type && in.meta.type->is_slice)) return true;
                return false;
            case ThinOp::LoadGlobal:
                // A global load leaves its result in rax/x9 (NOT frame-backed by
                // default). If a subsequent instr clobbers rax/x9 before the
                // global value is consumed (e.g. `trace = trace * 10 + v` loads
                // the global trace, then loads the local v — the second load
                // clobbers rax/x9 and the global is lost), the consumer reads a
                // stale register. Frame-back the result so the emit spills it
                // (both emit_x64's record_dst and emit_arm64's LoadGlobal honor
                // meta.frame_off). Exclude slice/lambda (16-byte {ptr,len}, use
                // dst+1 + their own pin_slice_dst path, NOT an 8-byte scalar slot).
                if (in.meta.type && (in.meta.type->is_slice || in.meta.type->is_lambda)) return false;
                return true;
            case ThinOp::CallNative: case ThinOp::CallScript:
            case ThinOp::CallIndirect: case ThinOp::CallCrossModule:
                // scalar/float returns + OPAQUE HANDLE returns (e.g. `string` =
                // Prim::I64 with struct_name="string", an opaque host handle NOT
                // a registered struct-by-value). Exclude slice (16-byte {ptr,len}),
                // REGISTERED struct-by-value (in ctx.structs — those use the
                // hidden-ptr / multi-word path, not an 8-byte scalar slot), and
                // void. plan_MACOS_ARM64.md Phase 6e: this fixed chained string
                // concat (a + b + c) — the intermediate `+` overload returns a
                // string handle that wasn't frame-backed, so the 2nd `+` read a
                // stale register.
                if (in.ret_type && !in.ret_type->is_slice && !in.ret_type->is_void()) {
                    if (in.ret_type->struct_name.empty()) return true;  // scalar/float
                    // opaque handle (string, etc.): NOT a registered struct
                    if (!ctx.structs || ctx.structs->count(in.ret_type->struct_name) == 0)
                        return true;
                }
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
                        int32_t slot32 = 0;
                        if (!checked_frame_cast(spill_top, slot32)) {
                            non_serializable = true;
                            if (non_serializable_reason.empty())
                                non_serializable_reason = "stack frame exceeds supported maximum (spill slot overflow)";
                            in.meta.frame_off = 0;  // poison; non_serializable bails below
                            continue;
                        }
                        slot = -slot32;
                        vreg_spill_slot[in.dst] = slot;
                    } else {
                        slot = it->second;  // reuse the same slot for this vreg
                    }
                    in.meta.frame_off = slot;
                }
            }
        }
        // Always reconcile the frame size with the ACTUAL post-lowering extent
        // (next_local_off + arg_temps_area + spill slots). The pre-computed
        // frame_size (round16(locals_area + arg_temps_area + 16)) does NOT
        // account for ad-hoc alloc_local temps grown during body lowering (e.g.
        // the __gld$N global-slice/lambda load slot), so without this a function
        // that allocates such a temp but needs NO scalar spill slots would get a
        // frame too small to hold it. Taking the max keeps the prior size when
        // it is already large enough (no behavior change for functions that
        // already worked) and grows it only when an ad-hoc temp + the spill
        // region exceed the pre-computed estimate.
        int64_t needed64 = spill_top + 16;  // spill_top = next_local_off + arg_temps_area + spill_slots
        int32_t needed = 0;
        if (checked_frame_cast(needed64, needed) && needed > frame_size) {
            frame_size = round16(needed);
            out.frame.frame_size = frame_size;
        }
        if (spill_top > spill_base) {
            int32_t spill_top32 = 0;
            if (checked_frame_cast(spill_top, spill_top32))
                out.frame.next_local_off = spill_top32;
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
        store_scalar_local(lv, dst_off, loc, true);
    }
}

// ─────────────── lower_expr (mirrors CG::eval) ───────────────
LoweredValue ThinLowerer::lower_expr(const Expr& ex) {
    safety::DepthGuard guard(lower_depth, MAX_COMPILE_DEPTH, "thin_lower::lower_expr");
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
        // The slice {ptr,len} from ConstStringRef/StringDecrypt must always be
        // frame-backed: the ConstInt(len) that follows clobbers rax (where ptr
        // lives), so any subsequent load_int_vreg(ptr) or load_slice_vreg(ptr)
        // needs a frame slot to reload from. Without this, string literals
        // through the IR backend produce garbage ptr values.
        int32_t slice_slot;
        {
            // Allocate 16 contiguous bytes for {ptr, len}.
            next_local_off += 16;
            slice_slot = -next_local_off;
            locals["__strslice$" + std::to_string(str_temp_counter)] = slice_slot;
            local_types["__strslice$" + std::to_string(str_temp_counter)] = &type_i64();
        }
        if (lit->encrypted && lit->baked_key != 0) {
            const int32_t data_off = alloc_str_temp(lit->baked_len);
            ptr = new_slice_vregs(ex.ty); len = ptr + 1;
            ThinInstr& dec = emit(ThinOp::StringDecrypt, ptr, 0, 0, loc);
            dec.meta.data_temp_off = data_off;  // decrypted-data buffer
            dec.meta.frame_off = slice_slot;     // slice result slot
            dec.meta.addend = string_off;
            dec.meta.base_kind = AbsFixup::FunctionRodataBase;
            dec.meta.len = int32_t(lit->baked_len);
            dec.imm.i = int64_t(lit->baked_key);  // the XOR key
            dec.meta.type = ex.ty;
            ThinInstr& l = emit(ThinOp::ConstInt, len, 0, 0, loc);
            l.imm.i = lit->baked_len; l.meta.type = &type_i64(); l.meta.width = 8;
            l.meta.frame_off = slice_slot + 8;  // frame-back len
        } else {
            ptr = new_slice_vregs(ex.ty); len = ptr + 1;
            ThinInstr& p = emit(ThinOp::ConstStringRef, ptr, 0, 0, loc);
            p.meta.addend = string_off; p.meta.base_kind = AbsFixup::FunctionRodataBase;
            p.meta.len = int32_t(lit->baked_len); p.meta.type = ex.ty;
            p.meta.frame_off = slice_slot;  // frame-back the slice result
            ThinInstr& l = emit(ThinOp::ConstInt, len, 0, 0, loc);
            l.imm.i = lit->baked_len; l.meta.type = &type_i64(); l.meta.width = 8;
            l.meta.frame_off = slice_slot + 8;  // frame-back len
        }
        // implicit conversion to a `string` handle: chained CallNative(ptr, len) -> i64.
        if (lit->implicit_to_string && lit->to_string_native_fn) {
            const Type* ret_ty = ex.ty ? ex.ty : &type_i64();
            VReg res = new_vreg(ret_ty);
            // Frame-back the string handle result: a DepthCheck is emitted before
            // the next call (string_length etc.), which clobbers rax. Without a
            // frame slot, the handle is lost (load_int_vreg takes stale-rax).
            int32_t handle_slot = alloc_local("__strhdl$" + std::to_string(str_temp_counter++), &type_i64());
            ThinInstr in;
            in.op = ThinOp::CallNative;
            in.loc = loc;
            in.dst = res;
            in.args.push_back(ptr);
            in.args.push_back(len);
            in.arg_frame_offs.push_back(-1);
            in.arg_frame_offs.push_back(-1);
            // Binding signature (arg_types) MUST carry the CANONICAL NativeSig
            // param types — one slice<u8> param — NOT the flattened {ptr,len}=
            // {i64,i64} arg-placement words. The .em native-binding signature
            // is checked against the live NativeSig at load time
            // (em_loader.cpp: signature mismatch for native), so a flattened
            // [i64,i64] (2 params) is rejected vs the canonical [slice<u8>]
            // (1 param). The two args[] vregs (ptr,len) are the arg-PLACEMENT
            // words; emit_native_call's placement loop consumes both via the
            // is_slice ++i path when arg_types[0] is the slice type. Mirrors
            // the tree-walker (codegen.cpp emit_counted_named_native), which
            // stamps the binding from &sig->params directly.
            if (const NativeSig* ssig = native_named(lit->to_string_native_name))
                for (const Type& p : ssig->params) in.arg_types.push_back(&p);
            else { in.arg_types.push_back(&type_i64()); in.arg_types.push_back(&type_i64()); }
            in.meta.native_name = lit->to_string_native_name;
            in.native_fn = lit->to_string_native_fn;
            in.ret_type = ret_ty;
            in.meta.type = ret_ty; in.meta.width = value_bytes(ret_ty, ctx.structs);
            in.meta.frame_off = handle_slot;  // pin_int_dst stores rax here
            emit_depth_check(loc);
            cur_block().instrs.push_back(std::move(in));
            return { LoweredValue::Scalar, res, 0, ret_ty };
        }
        return { LoweredValue::Slice, ptr, 0, ex.ty };
    }
    if (auto* id = dynamic_cast<const Ident*>(&ex)) {
        // #20 lambda capture read: if compiling a lambda fn + this name is a
        // capture, load env_ptr from [frame + lambda_env_off], then load the
        // value at [env_ptr + offset]. A by_ref capture's env slot holds a
        // POINTER to the captured variable's storage (double dereference);
        // by-value holds the value directly (single dereference). Mirrors CG
        // (codegen.cpp:2553-2580). v1: captures are scalars (int or float).
        if (compiling_lambda) {
            auto cit = lambda_captures.find(id->name);
            if (cit != lambda_captures.end()) {
                int32_t env_off = cit->second.offset;
                const Type* ct = cit->second.ty;
                bool by_ref = cit->second.by_ref;
                // load env_ptr from [frame + lambda_env_off] into a vreg
                VReg env_ptr = new_vreg(&type_i64());
                ThinInstr& ep = emit(ThinOp::LoadFrame, env_ptr, 0, 0, loc);
                ep.meta.frame_off = lambda_env_off; ep.meta.type = &type_i64();
                ep.meta.width = 8;
                if (ct && ct->is_float()) {
                    // float capture: emit a computed-address LoadFrame (src1 =
                    // env_ptr) into a float vreg. by-ref: double deref (load
                    // the ptr, then the float at [ptr]). by-value: single deref.
                    if (by_ref) {
                        VReg ptr = new_vreg(&type_i64());
                        ThinInstr& pp = emit(ThinOp::LoadFrame, ptr, env_ptr, 0, loc);
                        pp.meta.field_off = env_off; pp.meta.type = &type_i64();
                        pp.meta.width = 8;
                        VReg fv = new_vreg(ct);
                        ThinInstr& fv_in = emit(ThinOp::LoadFrame, fv, ptr, 0, loc);
                        fv_in.meta.field_off = 0; fv_in.meta.type = ct;
                        fv_in.meta.width = value_bytes(ct, ctx.structs);
                        fv_in.meta.is_f32 = (ct->prim == Prim::F32) ? 1 : 0;
                        return { LoweredValue::Scalar, fv, 0, ct };
                    }
                    VReg fv = new_vreg(ct);
                    ThinInstr& fv_in = emit(ThinOp::LoadFrame, fv, env_ptr, 0, loc);
                    fv_in.meta.field_off = env_off; fv_in.meta.type = ct;
                    fv_in.meta.width = value_bytes(ct, ctx.structs);
                    fv_in.meta.is_f32 = (ct->prim == Prim::F32) ? 1 : 0;
                    return { LoweredValue::Scalar, fv, 0, ct };
                }
                // int capture
                if (by_ref) {
                    VReg ptr = new_vreg(&type_i64());
                    ThinInstr& pp = emit(ThinOp::LoadFrame, ptr, env_ptr, 0, loc);
                    pp.meta.field_off = env_off; pp.meta.type = &type_i64();
                    pp.meta.width = 8;
                    VReg v = new_vreg(ct ? ct : &type_i64());
                    ThinInstr& vin = emit(ThinOp::LoadFrame, v, ptr, 0, loc);
                    vin.meta.field_off = 0; vin.meta.type = ct ? ct : &type_i64();
                    vin.meta.width = value_bytes(ct, ctx.structs);
                    return { LoweredValue::Scalar, v, 0, ct ? ct : &type_i64() };
                }
                VReg v = new_vreg(ct ? ct : &type_i64());
                ThinInstr& vin = emit(ThinOp::LoadFrame, v, env_ptr, 0, loc);
                vin.meta.field_off = env_off; vin.meta.type = ct ? ct : &type_i64();
                vin.meta.width = value_bytes(ct, ctx.structs);
                return { LoweredValue::Scalar, v, 0, ct ? ct : &type_i64() };
            }
        }
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
                // slice global: 2 words {ptr,len}. The ptr is stored as a
                // RELATIVE offset within the globals block (baked at load so the
                // bytes round-trip through .em without loader fixup); the slice
                // LoadGlobal emit (both emit_x64 + emit_arm64) absolute-izes it
                // by adding globals_base, yielding {abs_ptr, len} in the slice
                // ABI regs. Spill both words into a 16-byte temp via one slice
                // StoreFrame (frame_backing both the ptr + companion-len vreg),
                // then reload the slice from the temp so any consumer (IndexAddr
                // slice base, slice arg marshal, slice return) reads durably
                // frame-backed vregs. Mirrors CG::eval's global-slice Ident
                // case (load relative ptr, add globals_base, yield {ptr,len}).
                //
                // gap 2j (aggregate_global): the prior form loaded the ptr word
                // via a SCALAR LoadGlobal (type=i64), which loads the raw
                // RELATIVE offset verbatim (NO globals_base add) — so an
                // IndexAddr slice base computed ptr+idx*width from a junk
                // near-zero address and the element load segfaulted. The slice
                // LoadGlobal path absolute-izes, fixing `s[i]` on a global slice.
                int32_t tmp = alloc_local("__gld$" + std::to_string(temp_counter++), gt);
                VReg sv = new_slice_vregs(gt);  // sv = ptr vreg, sv+1 = len vreg
                ThinInstr& lg = emit(ThinOp::LoadGlobal, sv, 0, 0, loc);
                lg.meta.base_kind = AbsFixup::GlobalsBase; lg.meta.addend = uint32_t(goff);
                lg.meta.type = gt; lg.meta.width = 8;
                ThinInstr& ss = emit(ThinOp::StoreFrame, 0, sv, 0, loc);
                ss.meta.frame_off = tmp; ss.meta.type = gt; ss.meta.width = 8;
                return load_scalar_local(tmp, gt, loc);
            }
            if (gt && gt->is_lambda) {
                // lambda global: 2 words {slot, env_ptr}. The env_ptr (second
                // word) is an ABSOLUTE GC heap pointer (not a relative block
                // offset like a slice ptr), rooted via the global-root
                // descriptor. Load both words into a 16-byte temp via two scalar
                // LoadGlobals (verbatim — NO globals_base add on either word),
                // then load the lambda from the temp (load_scalar_local produces
                // 2 frame-backed vregs). This avoids the slice-LoadGlobal emit
                // (which absolute-izes word0) — wrong for a lambda whose slot is
                // a raw value, not a relative ptr.
                int32_t tmp = alloc_local("__gld$" + std::to_string(temp_counter++), gt);
                VReg w0 = new_vreg(&type_i64());
                ThinInstr& p = emit(ThinOp::LoadGlobal, w0, 0, 0, loc);
                p.meta.base_kind = AbsFixup::GlobalsBase; p.meta.addend = uint32_t(goff);
                p.meta.type = &type_i64(); p.meta.width = 8;
                ThinInstr& ps = emit(ThinOp::StoreFrame, 0, w0, 0, loc);
                ps.meta.frame_off = tmp; ps.meta.type = &type_i64(); ps.meta.width = 8;
                VReg w1 = new_vreg(&type_i64());
                ThinInstr& l = emit(ThinOp::LoadGlobal, w1, 0, 0, loc);
                l.meta.base_kind = AbsFixup::GlobalsBase; l.meta.addend = uint32_t(goff + 8);
                l.meta.type = &type_i64(); l.meta.width = 8;
                ThinInstr& ls = emit(ThinOp::StoreFrame, 0, w1, 0, loc);
                ls.meta.frame_off = tmp + 8; ls.meta.type = &type_i64(); ls.meta.width = 8;
                return load_scalar_local(tmp, gt, loc);
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
            // Stamp the BINDING signature from the canonical NativeSig (looked
            // up by the overload name), NOT the AST operand types. The .em
            // native-binding signature is checked against the live NativeSig at
            // load time (em_loader.cpp signature mismatch), and the NativeSig
            // params can differ from the AST operand types — e.g. a `string`
            // handle operand is `i64`+struct_name "string" in the AST but plain
            // `i64` (no struct_name) in the overload's NativeSig params
            // (BindingBuilder::add_overload uses bind_prim(I64)). Mirrors the
            // tree-walker (codegen.cpp emit_counted_named_native ->
            // &sig->params). The args[]/arg_frame_offs[] (placement words) are
            // untouched; arg_types now carries one canonical entry per logical
            // param, which emit's placement loop consumes via the is_slice ++i /
            // struct-sentinel paths. Skipped for struct-by-ptr returns (the
            // hidden dest occupies args[0]/arg_types[0]; that ABI-experimental
            // overload shape is not shipped through .em IR).
            if (!is_registered_struct_ty(ret_ty)) {
                if (const NativeSig* osig = native_named(b->overload_name)) {
                    in.arg_types.clear();
                    for (const Type& p : osig->params) in.arg_types.push_back(&p);
                }
            }
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
    // #20 lambda expression: materialize the env (a frame temp / GC heap
    // region holding the captured values copied from the enclosing scope) +
    // emit the 16-byte lambda value {slot, env_ptr} as 2 consecutive vregs
    // (the slice ABI: vreg=slot, vreg+1=env_ptr). Two env backends, mirroring
    // the tree-walker (codegen.cpp:2252-2470):
    //   * stack (default): a compiler-hidden frame temp of env_size bytes at
    //     [frame+env_off]; env_ptr = the address of that region. v1 limitation:
    //     the lambda must not outlive this frame (the env_ptr is a stack addr).
    //   * GC heap (ctx.use_gc_env): __ember_gc_alloc_env(env_size) returns a
    //     heap env ptr pinned by ext_gc; captures copy into [ptr+offset];
    //     env_ptr = the heap ptr (so the lambda CAN outlive this frame). The
    //     env_ptr frame slot is registered as a GC root (add_gc_ptr_slot).
    // By-ref capture (`fn[&x]`): the env slot holds the ADDRESS of the captured
    // variable's storage (so the lambda sees post-capture mutations + writes
    // mutate the original). By-value: the env slot holds a copy.
    if (auto* le = dynamic_cast<const LambdaExpr*>(&ex)) {
        const bool gc_env = ctx.use_gc_env && le->env_size > 0;
        const Type* lty = ex.ty ? ex.ty : &type_i64();
        int32_t env_off = 0;      // stack path: env bytes at [frame+env_off]
        int32_t envptr_off = 0;   // gc path: 8-byte slot holding the heap env ptr

        // ---- materialize the env (only when there are captures) ----
        if (gc_env) {
            // Reserve an 8-byte frame slot to hold the heap env ptr returned by
            // __ember_gc_alloc_env (the env itself lives on the GC heap).
            std::string name = "__envptr$" + std::to_string(temp_counter++);
            envptr_off = alloc_local(name, &type_i64());
            // Precise GC: envptr_off holds the heap env pointer (a GC object
            // pointer) for this lambda's whole frame lifetime. Record it so the
            // collector marks it as a root while the frame is live.
            add_gc_ptr_slot(envptr_off);
            // Call __ember_gc_alloc_env(env_size) -> env_ptr vreg.
            const NativeSig* gsig = native_named("__ember_gc_alloc_env");
            VReg env_ptr = new_vreg(&type_i64());
            int32_t env_ptr_slot = alloc_local("__envptrt$" + std::to_string(temp_counter++), &type_i64());
            ThinInstr in;
            in.op = ThinOp::CallNative;
            in.loc = loc;
            in.dst = env_ptr;
            VReg sz = new_vreg(&type_i64());
            ThinInstr& sz_in = emit(ThinOp::ConstInt, sz, 0, 0, loc);
            sz_in.imm.i = int64_t(le->env_size); sz_in.meta.type = &type_i64(); sz_in.meta.width = 8;
            in.args.push_back(sz);
            in.arg_frame_offs.push_back(-1);
            in.arg_types.push_back(&type_i64());   // arg 0 = env size
            in.meta.native_name = "__ember_gc_alloc_env";
            in.native_fn = gsig ? gsig->fn_ptr : nullptr;
            in.ret_type = &type_i64();
            in.meta.type = &type_i64(); in.meta.width = 8;
            in.meta.frame_off = env_ptr_slot;  // pin_int_dst stores the result here
            emit_depth_check(loc);
            cur_block().instrs.push_back(std::move(in));
            // spill env_ptr to its frame slot immediately (further capture
            // loads clobber volatile regs).
            ThinInstr& sp = emit(ThinOp::StoreFrame, 0, env_ptr, 0, loc);
            sp.meta.frame_off = envptr_off; sp.meta.type = &type_i64(); sp.meta.width = 8;
            // copy each capture from its enclosing scope into [envptr + offset]
            for (size_t i = 0; i < le->captures.size(); ++i) {
                int32_t coff = le->capture_offsets[i];
                const std::string& cname = le->captures[i];
                const Type* ct = le->capture_types[i].get();
                const bool inner_by_ref = i < le->capture_by_ref.size() && le->capture_by_ref[i];
                auto cit = locals.find(cname);
                if (cit != locals.end()) {
                    int32_t loff = cit->second;
                    if (inner_by_ref) {
                        // store the ADDRESS of the frame slot into [envptr+coff].
                        // FieldAddr(frame_off=0, field_off=loff) computes
                        // x29 + loff WITHOUT spilling back to loff (the emit's
                        // spill-back is gated on frame_off != 0, so the local's
                        // storage is NOT corrupted). The result is left in x9
                        // (not frame-backed); StoreAddr loads src2 (env_ptr)
                        // first, which would clobber the address in x9, so spill
                        // the address to a temp slot first (frame-backs it), then
                        // StoreAddr recovers it from the slot.
                        int32_t addr_spill = alloc_local("__capaddr$" + std::to_string(temp_counter++), &type_i64());
                        VReg addr = new_vreg(&type_i64());
                        ThinInstr& a = emit(ThinOp::FieldAddr, addr, 0, 0, loc);
                        a.meta.frame_off = 0; a.meta.field_off = loff;
                        a.meta.type = &type_i64(); a.meta.width = 8;
                        ThinInstr& as = emit(ThinOp::StoreFrame, 0, addr, 0, loc);
                        as.meta.frame_off = addr_spill; as.meta.type = &type_i64(); as.meta.width = 8;
                        VReg ep2 = new_vreg(&type_i64());
                        ThinInstr& epl = emit(ThinOp::LoadFrame, ep2, 0, 0, loc);
                        epl.meta.frame_off = envptr_off; epl.meta.type = &type_i64(); epl.meta.width = 8;
                        ThinInstr& s = emit(ThinOp::StoreAddr, 0, addr, ep2, loc);
                        s.meta.frame_off = coff; s.meta.type = &type_i64(); s.meta.width = 8;
                    } else if (ct && ct->is_float()) {
                        VReg fv = new_vreg(ct);
                        ThinInstr& fl = emit(ThinOp::LoadFrame, fv, 0, 0, loc);
                        fl.meta.frame_off = loff; fl.meta.type = ct; fl.meta.width = value_bytes(ct, ctx.structs);
                        fl.meta.is_f32 = (ct->prim == Prim::F32) ? 1 : 0;
                        VReg ep2 = new_vreg(&type_i64());
                        ThinInstr& epl = emit(ThinOp::LoadFrame, ep2, 0, 0, loc);
                        epl.meta.frame_off = envptr_off; epl.meta.type = &type_i64(); epl.meta.width = 8;
                        ThinInstr& s = emit(ThinOp::StoreAddr, 0, fv, ep2, loc);
                        s.meta.frame_off = coff; s.meta.type = ct; s.meta.width = value_bytes(ct, ctx.structs);
                        s.meta.is_f32 = (ct->prim == Prim::F32) ? 1 : 0;
                    } else {
                        VReg vv = new_vreg(ct ? ct : &type_i64());
                        ThinInstr& vl = emit(ThinOp::LoadFrame, vv, 0, 0, loc);
                        vl.meta.frame_off = loff; vl.meta.type = ct ? ct : &type_i64();
                        vl.meta.width = value_bytes(ct, ctx.structs);
                        VReg ep2 = new_vreg(&type_i64());
                        ThinInstr& epl = emit(ThinOp::LoadFrame, ep2, 0, 0, loc);
                        epl.meta.frame_off = envptr_off; epl.meta.type = &type_i64(); epl.meta.width = 8;
                        ThinInstr& s = emit(ThinOp::StoreAddr, 0, vv, ep2, loc);
                        s.meta.frame_off = coff; s.meta.type = ct ? ct : &type_i64(); s.meta.width = 8;
                    }
                    continue;
                }
                // #20 nested-lambda transitive capture: the value lives in THIS
                // lambda's env (one of its own captures). Load it from
                // [enclosing_env_ptr + cap_env_off] (the enclosing env_ptr is in
                // [frame + lambda_env_off]) and store it into the new heap env.
                auto lcit = compiling_lambda ? lambda_captures.find(cname)
                                             : lambda_captures.end();
                if (lcit != lambda_captures.end()) {
                    int32_t cap_env_off = lcit->second.offset;
                    bool outer_by_ref = lcit->second.by_ref;
                    bool store_ptr = inner_by_ref && outer_by_ref && !(ct && ct->is_float());
                    // enclosing env_ptr -> vreg
                    VReg oenv = new_vreg(&type_i64());
                    ThinInstr& oel = emit(ThinOp::LoadFrame, oenv, 0, 0, loc);
                    oel.meta.frame_off = lambda_env_off; oel.meta.type = &type_i64(); oel.meta.width = 8;
                    // new env ptr -> vreg (store base)
                    VReg nep = new_vreg(&type_i64());
                    ThinInstr& nepl = emit(ThinOp::LoadFrame, nep, 0, 0, loc);
                    nepl.meta.frame_off = envptr_off; nepl.meta.type = &type_i64(); nepl.meta.width = 8;
                    if (store_ptr) {
                        // copy the POINTER: load ptr from [oenv + cap_env_off]
                        VReg ptr = new_vreg(&type_i64());
                        ThinInstr& pl = emit(ThinOp::LoadFrame, ptr, oenv, 0, loc);
                        pl.meta.field_off = cap_env_off; pl.meta.type = &type_i64(); pl.meta.width = 8;
                        ThinInstr& s = emit(ThinOp::StoreAddr, 0, ptr, nep, loc);
                        s.meta.frame_off = coff; s.meta.type = &type_i64(); s.meta.width = 8;
                    } else if (outer_by_ref) {
                        // load ptr from [oenv + cap_env_off], then value at [ptr]
                        VReg ptr = new_vreg(&type_i64());
                        ThinInstr& pl = emit(ThinOp::LoadFrame, ptr, oenv, 0, loc);
                        pl.meta.field_off = cap_env_off; pl.meta.type = &type_i64(); pl.meta.width = 8;
                        if (ct && ct->is_float()) {
                            VReg fv = new_vreg(ct);
                            ThinInstr& fl = emit(ThinOp::LoadFrame, fv, ptr, 0, loc);
                            fl.meta.field_off = 0; fl.meta.type = ct; fl.meta.width = value_bytes(ct, ctx.structs);
                            fl.meta.is_f32 = (ct->prim == Prim::F32) ? 1 : 0;
                            ThinInstr& s = emit(ThinOp::StoreAddr, 0, fv, nep, loc);
                            s.meta.frame_off = coff; s.meta.type = ct; s.meta.width = value_bytes(ct, ctx.structs);
                            s.meta.is_f32 = (ct->prim == Prim::F32) ? 1 : 0;
                        } else {
                            VReg vv = new_vreg(ct ? ct : &type_i64());
                            ThinInstr& vl = emit(ThinOp::LoadFrame, vv, ptr, 0, loc);
                            vl.meta.field_off = 0; vl.meta.type = ct ? ct : &type_i64();
                            vl.meta.width = value_bytes(ct, ctx.structs);
                            ThinInstr& s = emit(ThinOp::StoreAddr, 0, vv, nep, loc);
                            s.meta.frame_off = coff; s.meta.type = ct ? ct : &type_i64(); s.meta.width = 8;
                        }
                    } else {
                        // outer by-value: load the value from [oenv + cap_env_off]
                        if (ct && ct->is_float()) {
                            VReg fv = new_vreg(ct);
                            ThinInstr& fl = emit(ThinOp::LoadFrame, fv, oenv, 0, loc);
                            fl.meta.field_off = cap_env_off; fl.meta.type = ct; fl.meta.width = value_bytes(ct, ctx.structs);
                            fl.meta.is_f32 = (ct->prim == Prim::F32) ? 1 : 0;
                            ThinInstr& s = emit(ThinOp::StoreAddr, 0, fv, nep, loc);
                            s.meta.frame_off = coff; s.meta.type = ct; s.meta.width = value_bytes(ct, ctx.structs);
                            s.meta.is_f32 = (ct->prim == Prim::F32) ? 1 : 0;
                        } else {
                            VReg vv = new_vreg(ct ? ct : &type_i64());
                            ThinInstr& vl = emit(ThinOp::LoadFrame, vv, oenv, 0, loc);
                            vl.meta.field_off = cap_env_off; vl.meta.type = ct ? ct : &type_i64();
                            vl.meta.width = value_bytes(ct, ctx.structs);
                            ThinInstr& s = emit(ThinOp::StoreAddr, 0, vv, nep, loc);
                            s.meta.frame_off = coff; s.meta.type = ct ? ct : &type_i64(); s.meta.width = 8;
                        }
                    }
                    continue;
                }
                // unresolvable capture: zero-fill the heap env slot
                VReg zero = new_vreg(&type_i64());
                ThinInstr& zi = emit(ThinOp::ConstInt, zero, 0, 0, loc);
                zi.imm.i = 0; zi.meta.type = &type_i64(); zi.meta.width = 8;
                VReg nep = new_vreg(&type_i64());
                ThinInstr& nepl = emit(ThinOp::LoadFrame, nep, 0, 0, loc);
                nepl.meta.frame_off = envptr_off; nepl.meta.type = &type_i64(); nepl.meta.width = 8;
                ThinInstr& s = emit(ThinOp::StoreAddr, 0, zero, nep, loc);
                s.meta.frame_off = coff; s.meta.type = &type_i64(); s.meta.width = 8;
            }
        } else if (le->env_size > 0) {
            // ---- stack-env backend (default) ----
            // alloc a frame temp sized to env_size (rounded up to 8)
            env_off = alloc_env_temp(le->env_size);
            for (size_t i = 0; i < le->captures.size(); ++i) {
                int32_t dst = env_off + le->capture_offsets[i];
                const std::string& cname = le->captures[i];
                const Type* ct = le->capture_types[i].get();
                const bool inner_by_ref = i < le->capture_by_ref.size() && le->capture_by_ref[i];
                auto cit = locals.find(cname);
                if (cit != locals.end()) {
                    int32_t loff = cit->second;
                    if (inner_by_ref) {
                        // store the ADDRESS of the frame slot into [frame+dst].
                        // FieldAddr(frame_off=0, field_off=loff) computes
                        // x29 + loff WITHOUT spilling back to loff (preserves
                        // the captured local's storage). Result in x9; the
                        // following StoreFrame consumes it.
                        VReg addr = new_vreg(&type_i64());
                        ThinInstr& a = emit(ThinOp::FieldAddr, addr, 0, 0, loc);
                        a.meta.frame_off = 0; a.meta.field_off = loff;
                        a.meta.type = &type_i64(); a.meta.width = 8;
                        ThinInstr& s = emit(ThinOp::StoreFrame, 0, addr, 0, loc);
                        s.meta.frame_off = dst; s.meta.type = &type_i64(); s.meta.width = 8;
                    } else if (ct && ct->is_float()) {
                        VReg fv = new_vreg(ct);
                        ThinInstr& fl = emit(ThinOp::LoadFrame, fv, 0, 0, loc);
                        fl.meta.frame_off = loff; fl.meta.type = ct; fl.meta.width = value_bytes(ct, ctx.structs);
                        fl.meta.is_f32 = (ct->prim == Prim::F32) ? 1 : 0;
                        ThinInstr& s = emit(ThinOp::StoreFrame, 0, fv, 0, loc);
                        s.meta.frame_off = dst; s.meta.type = ct; s.meta.width = value_bytes(ct, ctx.structs);
                        s.meta.is_f32 = (ct->prim == Prim::F32) ? 1 : 0;
                    } else {
                        VReg vv = new_vreg(ct ? ct : &type_i64());
                        ThinInstr& vl = emit(ThinOp::LoadFrame, vv, 0, 0, loc);
                        vl.meta.frame_off = loff; vl.meta.type = ct ? ct : &type_i64();
                        vl.meta.width = value_bytes(ct, ctx.structs);
                        ThinInstr& s = emit(ThinOp::StoreFrame, 0, vv, 0, loc);
                        s.meta.frame_off = dst; s.meta.type = ct ? ct : &type_i64(); s.meta.width = 8;
                    }
                    continue;
                }
                // #20 nested-lambda transitive capture (stack env)
                auto lcit = compiling_lambda ? lambda_captures.find(cname)
                                             : lambda_captures.end();
                if (lcit != lambda_captures.end()) {
                    int32_t cap_env_off = lcit->second.offset;
                    bool outer_by_ref = lcit->second.by_ref;
                    bool store_ptr = inner_by_ref && outer_by_ref && !(ct && ct->is_float());
                    VReg oenv = new_vreg(&type_i64());
                    ThinInstr& oel = emit(ThinOp::LoadFrame, oenv, 0, 0, loc);
                    oel.meta.frame_off = lambda_env_off; oel.meta.type = &type_i64(); oel.meta.width = 8;
                    if (store_ptr) {
                        VReg ptr = new_vreg(&type_i64());
                        ThinInstr& pl = emit(ThinOp::LoadFrame, ptr, oenv, 0, loc);
                        pl.meta.field_off = cap_env_off; pl.meta.type = &type_i64(); pl.meta.width = 8;
                        ThinInstr& s = emit(ThinOp::StoreFrame, 0, ptr, 0, loc);
                        s.meta.frame_off = dst; s.meta.type = &type_i64(); s.meta.width = 8;
                    } else if (outer_by_ref) {
                        VReg ptr = new_vreg(&type_i64());
                        ThinInstr& pl = emit(ThinOp::LoadFrame, ptr, oenv, 0, loc);
                        pl.meta.field_off = cap_env_off; pl.meta.type = &type_i64(); pl.meta.width = 8;
                        if (ct && ct->is_float()) {
                            VReg fv = new_vreg(ct);
                            ThinInstr& fl = emit(ThinOp::LoadFrame, fv, ptr, 0, loc);
                            fl.meta.field_off = 0; fl.meta.type = ct; fl.meta.width = value_bytes(ct, ctx.structs);
                            fl.meta.is_f32 = (ct->prim == Prim::F32) ? 1 : 0;
                            ThinInstr& s = emit(ThinOp::StoreFrame, 0, fv, 0, loc);
                            s.meta.frame_off = dst; s.meta.type = ct; s.meta.width = value_bytes(ct, ctx.structs);
                            s.meta.is_f32 = (ct->prim == Prim::F32) ? 1 : 0;
                        } else {
                            VReg vv = new_vreg(ct ? ct : &type_i64());
                            ThinInstr& vl = emit(ThinOp::LoadFrame, vv, ptr, 0, loc);
                            vl.meta.field_off = 0; vl.meta.type = ct ? ct : &type_i64();
                            vl.meta.width = value_bytes(ct, ctx.structs);
                            ThinInstr& s = emit(ThinOp::StoreFrame, 0, vv, 0, loc);
                            s.meta.frame_off = dst; s.meta.type = ct ? ct : &type_i64(); s.meta.width = 8;
                        }
                    } else {
                        if (ct && ct->is_float()) {
                            VReg fv = new_vreg(ct);
                            ThinInstr& fl = emit(ThinOp::LoadFrame, fv, oenv, 0, loc);
                            fl.meta.field_off = cap_env_off; fl.meta.type = ct; fl.meta.width = value_bytes(ct, ctx.structs);
                            fl.meta.is_f32 = (ct->prim == Prim::F32) ? 1 : 0;
                            ThinInstr& s = emit(ThinOp::StoreFrame, 0, fv, 0, loc);
                            s.meta.frame_off = dst; s.meta.type = ct; s.meta.width = value_bytes(ct, ctx.structs);
                            s.meta.is_f32 = (ct->prim == Prim::F32) ? 1 : 0;
                        } else {
                            VReg vv = new_vreg(ct ? ct : &type_i64());
                            ThinInstr& vl = emit(ThinOp::LoadFrame, vv, oenv, 0, loc);
                            vl.meta.field_off = cap_env_off; vl.meta.type = ct ? ct : &type_i64();
                            vl.meta.width = value_bytes(ct, ctx.structs);
                            ThinInstr& s = emit(ThinOp::StoreFrame, 0, vv, 0, loc);
                            s.meta.frame_off = dst; s.meta.type = ct ? ct : &type_i64(); s.meta.width = 8;
                        }
                    }
                    continue;
                }
                // unresolvable capture: zero-fill the env slot
                VReg zero = new_vreg(&type_i64());
                ThinInstr& zi = emit(ThinOp::ConstInt, zero, 0, 0, loc);
                zi.imm.i = 0; zi.meta.type = &type_i64(); zi.meta.width = 8;
                ThinInstr& s = emit(ThinOp::StoreFrame, 0, zero, 0, loc);
                s.meta.frame_off = dst; s.meta.type = &type_i64(); s.meta.width = 8;
            }
        }
        // Build the {slot, env_ptr} value as 2 consecutive vregs (slice ABI).
        // Store both words into a 16-byte temp slot, then load the slice from
        // it (load_scalar_local produces 2 frame-backed vregs). This avoids
        // the FieldAddr op's result-spill-to-frame_off semantics (which would
        // corrupt the env region) + ensures both words are durably frame-backed
        // for any consumer (StoreFrame to a local, a lambda call, etc.).
        int32_t lam_slot = alloc_local("__lamval$" + std::to_string(temp_counter++), lty);
        // slot word. The slot is an i64 fn-handle value (the dispatch slot),
        // NOT a 2-word lambda value — the env_ptr word is stored separately
        // below. Store it as a SCALAR i64 (type=&type_i64()), NOT lty (the
        // lambda type): a StoreFrame with meta.type = lty triggers the emit's
        // slice/lambda 2-word store path, which loads src1 AND src1+1 (the
        // not-yet-materialized env_ptr word) and writes garbage into the
        // env_ptr half of the lambda value. That left the callee receiving
        // env_ptr = the captured VALUE (e.g. 40) instead of the env region
        // address, so the body's [env_ptr+offset] load dereferenced 40
        // (EXC_BAD_ACCESS at 0x28). The 16-byte lam_slot is still allocated
        // with lty (so the trailing load_scalar_local loads both words as a
        // slice); only THIS word's store is scalar.
        {
            VReg sv = new_vreg(&type_i64());
            ThinInstr& si = emit(ThinOp::ConstInt, sv, 0, 0, loc);
            si.imm.i = int64_t(le->slot); si.meta.type = &type_i64(); si.meta.width = 8;
            ThinInstr& ss = emit(ThinOp::StoreFrame, 0, sv, 0, loc);
            ss.meta.frame_off = lam_slot; ss.meta.type = &type_i64(); ss.meta.width = 8;
        }
        // env_ptr word
        if (gc_env) {
            // env_ptr = load the heap ptr from its frame slot, store to lam_slot+8
            VReg ep = new_vreg(&type_i64());
            ThinInstr& el = emit(ThinOp::LoadFrame, ep, 0, 0, loc);
            el.meta.frame_off = envptr_off; el.meta.type = &type_i64(); el.meta.width = 8;
            ThinInstr& es = emit(ThinOp::StoreFrame, 0, ep, 0, loc);
            es.meta.frame_off = lam_slot + 8; es.meta.type = &type_i64(); es.meta.width = 8;
        } else if (le->env_size > 0) {
            // env_ptr = address of the stack env region [frame + env_off].
            // FieldAddr with frame_off=0 + field_off=env_off computes
            // x29 + env_off WITHOUT spilling the result back to env_off (the
            // emit's spill-back is gated on frame_off != 0). The result stays
            // in x9; the immediately-following StoreFrame consumes it.
            VReg addr = new_vreg(&type_i64());
            ThinInstr& a = emit(ThinOp::FieldAddr, addr, 0, 0, loc);
            a.meta.frame_off = 0; a.meta.field_off = env_off;
            a.meta.type = &type_i64(); a.meta.width = 8;
            ThinInstr& as = emit(ThinOp::StoreFrame, 0, addr, 0, loc);
            as.meta.frame_off = lam_slot + 8; as.meta.type = &type_i64(); as.meta.width = 8;
        } else {
            VReg z = new_vreg(&type_i64());
            ThinInstr& zi = emit(ThinOp::ConstInt, z, 0, 0, loc);
            zi.imm.i = 0; zi.meta.type = &type_i64(); zi.meta.width = 8;
            ThinInstr& zs = emit(ThinOp::StoreFrame, 0, z, 0, loc);
            zs.meta.frame_off = lam_slot + 8; zs.meta.type = &type_i64(); zs.meta.width = 8;
        }
        if (non_serializable_reason.empty())
            non_serializable_reason = gc_env
                ? "lambda env is a GC heap allocation (process-local pin)"
                : "lambda env is a stack-frame-local allocation";
        // load the {slot, env_ptr} slice from the temp slot (2 frame-backed vregs)
        return load_scalar_local(lam_slot, lty, loc);
    }
    if (auto* h = dynamic_cast<const FnHandleExpr*>(&ex)) {
        if (h->is_cross_module) {
            // v1.0 Tier 2 cross-module handles (plan_MACOS_ARM64.md Phase 8):
            // sema stamped cross_module_id + cross_module_slot from the linked-
            // module export table. Bake the packed handle as a ConstInt, exactly
            // as the tree-walker does (codegen.cpp FnHandleExpr eval):
            //   handle = (1<<63) | (module_id << 32) | slot
            // Bit 63 is the cross-module flag (an intra-module handle is a bare
            // slot, never bit 63 set, so the spaces never collide). The handle
            // is an i64 (is_fn_handle type). The CALL through it lowers to a
            // CallIndirect with this vreg as src1; emit_arm64's CallTargetGuard
            // tests bit 63 to skip the intra-module allowlist, and
            // emit_indirect_call tests bit 63 again to dispatch via the module
            // registry (ModuleRegistryBase -> [mod_id*8] -> [slot*8] -> blr) +
            // validate via the handle-records table. The process-local
            // records/registry bases make this non-serializable to .em (the
            // same constraint as the intra-module allowlist).
            uint64_t handle = (uint64_t(1) << 63)
                            | (uint64_t(h->cross_module_id) << 32)
                            | uint64_t(uint32_t(h->cross_module_slot));
            VReg v = new_vreg(ex.ty ? ex.ty : &type_i64());
            ThinInstr& in = emit(ThinOp::ConstInt, v, 0, 0, loc);
            in.imm.i = int64_t(handle);
            in.meta.type = ex.ty ? ex.ty : &type_i64();
            in.meta.width = 8;
            if (non_serializable_reason.empty())
                non_serializable_reason =
                    "cross-module function handle requires process-local module-records storage";
            return { LoweredValue::Scalar, v, 0, ex.ty ? ex.ty : &type_i64() };
        }
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
            // LoadFrame from [element_addr + field_off]. The computed-address
            // LoadFrame convention (shared by emit_x64 + emit_arm64, and by the
            // lambda-capture LoadFrame sites above) is: src1 = the address vreg,
            // meta.field_off = the within-base displacement (the field offset),
            // meta.frame_off = a SEPARATE spill slot for the loaded RESULT (0 =
            // none yet; the post-lowering spill pass assigns one below).
            //
            // gap 2j root cause: this previously set meta.frame_off = field_off,
            // which (a) made emit read the wrong displacement (it uses field_off,
            // which was left at 0, so `arr[1].b` read `arr[1].a`) and (b) made
            // pin_int_dst spill the RESULT to [x29 + field_off] — overwriting
            // the saved return address (x30 at [x29+8]) for field_off==8 and
            // crashing the epilogue's `ret`. Setting frame_off=0 lets the spill
            // pass assign a real result slot, and field_off carries the field
            // displacement so the load reads [addr + field_off].
            VReg res = new_vreg(ft);
            ThinInstr& ld = emit(ThinOp::LoadFrame, res, 0, 0, loc);
            ld.src1 = addr; ld.meta.field_off = field_off; ld.meta.frame_off = 0;
            ld.meta.type = ft;
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
    // Defensive: any expression node sema did not classify (a future node
    // type, or an EnumAccessExpr that escaped the enum-access pre-pass) must
    // NOT silently lower to a zero/scratch vreg. Previously lower_expr fell
    // off the end returning a poison {Scalar, vreg=0} with no diagnostic, and
    // the IR emit would produce wrong code. Flag non_serializable so the
    // whole function falls back to the tree-walker (whose eval() has its own
    // defensive trap) instead of emitting a silent miscompile.
    non_serializable = true;
    non_serializable_reason =
        "unhandled expression node reached IR lowering; falling back to tree-walker";
    return { LoweredValue::Scalar, 0, 0, ex.ty };
}

// store an rvalue (lv) into an assignment target (Ident / IndexExpr / FieldExpr)
void ThinLowerer::store_to_target(const Expr& target, const LoweredValue& lv, Loc loc) {
    if (auto* id = dynamic_cast<const Ident*>(&target)) {
        // #20 lambda capture write: if compiling a lambda fn + this name is a
        // capture, store through the env slot. by_ref: the env slot holds a
        // POINTER to the captured storage -> load the ptr, store the value
        // through [ptr] (mutates the original). by-value: store into the env
        // slot directly (defensive; sema marks by-value captures const).
        // Mirrors CG (codegen.cpp:3170-3240). v1: captures are scalars.
        if (compiling_lambda) {
            auto cit = lambda_captures.find(id->name);
            if (cit != lambda_captures.end()) {
                int32_t env_off = cit->second.offset;
                const Type* ct = cit->second.ty;
                bool by_ref = cit->second.by_ref;
                // load env_ptr from [frame + lambda_env_off]
                VReg env_ptr = new_vreg(&type_i64());
                ThinInstr& ep = emit(ThinOp::LoadFrame, env_ptr, 0, 0, loc);
                ep.meta.frame_off = lambda_env_off; ep.meta.type = &type_i64();
                ep.meta.width = 8;
                if (by_ref) {
                    // load the pointer from [env_ptr + env_off]
                    VReg ptr = new_vreg(&type_i64());
                    ThinInstr& pp = emit(ThinOp::LoadFrame, ptr, env_ptr, 0, loc);
                    pp.meta.field_off = env_off; pp.meta.type = &type_i64();
                    pp.meta.width = 8;
                    // store the value through [ptr + 0]
                    ThinInstr& s = emit(ThinOp::StoreAddr, 0, lv.vreg, ptr, loc);
                    s.meta.frame_off = 0; s.meta.type = ct;
                    s.meta.width = value_bytes(ct, ctx.structs);
                    if (ct && ct->is_float()) s.meta.is_f32 = (ct->prim == Prim::F32) ? 1 : 0;
                    return;
                }
                // by-value: store into [env_ptr + env_off]
                ThinInstr& s = emit(ThinOp::StoreAddr, 0, lv.vreg, env_ptr, loc);
                s.meta.frame_off = env_off; s.meta.type = ct;
                s.meta.width = value_bytes(ct, ctx.structs);
                if (ct && ct->is_float()) s.meta.is_f32 = (ct->prim == Prim::F32) ? 1 : 0;
                return;
            }
        }
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
        ThinInstr& ia = emit(ThinOp::IndexAddr, addr, 0, idx.vreg, loc);
        ia.meta.width = width; ia.meta.type = elem;
        if (is_slice_base) { ia.src1 = base_ptr; ia.meta.frame_off = 0; }
        else if (is_global_base) { ia.meta.base_kind = AbsFixup::GlobalsBase; ia.meta.addend = uint32_t(base_off); }
        else { ia.meta.frame_off = base_off; }
        // store element to [addr+0]
        ThinInstr& st = emit(ThinOp::StoreAddr, 0, lv.vreg, addr, loc);
        st.meta.frame_off = 0; st.meta.type = lv.ty;
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
    safety::DepthGuard guard(lower_depth, MAX_COMPILE_DEPTH, "thin_lower::lower_call");
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

    // Red 6: lambda call (is_lambda_call). A lambda value is {slot, env_ptr};
    // the call prepends env_ptr as the hidden first arg (word 0) + dispatches
    // via the slot. Lower the lambda target → {slot_vreg, env_ptr_vreg}, then
    // prepend env_ptr as args[0] (or args[1] if ret_struct) and set src1 =
    // slot_vreg. The op is refined to CallIndirect below (the slot is a runtime
    // value, like a function handle); emit_call's CallIndirect path loads src1,
    // runs the guard, stashes the handle, + dispatches (via the keyed resolver
    // in keyed mode or the dispatch table in legacy mode). This reuses the
    // existing CallIndirect infrastructure — the only difference is the
    // prepended env_ptr arg.
    if (c.is_lambda_call) {
        LoweredValue lv = lower_expr(*c.lambda_target);
        VReg env_ptr_vreg = lv.vreg + 1;  // the second vreg (env_ptr)
        VReg slot_vreg = lv.vreg;          // the first vreg (slot)
        // Prepend env_ptr as the hidden first user arg. For ret_struct,
        // args[0] is already the hidden dest ptr; env_ptr goes at args[1].
        if (ret_struct) {
            in.args.insert(in.args.begin() + 1, env_ptr_vreg);
            in.arg_frame_offs.insert(in.arg_frame_offs.begin() + 1, -1);
            in.arg_types.insert(in.arg_types.begin() + 1, &type_i64());
        } else {
            in.args.insert(in.args.begin(), env_ptr_vreg);
            in.arg_frame_offs.insert(in.arg_frame_offs.begin(), -1);
            in.arg_types.insert(in.arg_types.begin(), &type_i64());
        }
        // Shift the ops' slot0 values up by 1 to account for env_ptr at word 0.
        for (auto& op : ops) op.slot0 += 1;
        // Set src1 = slot_vreg (the logical slot; the guard validates it in
        // emit_call's CallIndirect path).
        in.src1 = slot_vreg;
    }

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
        // Stamp the BINDING signature from the canonical NativeSig (looked up
        // by the binding name), NOT the AST operand types built above. The .em
        // native-binding signature is checked against the live NativeSig at
        // load time (em_loader.cpp signature mismatch), and the two can differ:
        // a slice param is ONE NativeSig param (slice<u8>) but TWO placement
        // words ({ptr,len}={i64,i64}) in args[], and a `string` handle operand
        // is i64+struct_name in the AST but plain i64 in the NativeSig. So the
        // binding must carry the canonical NativeSig params (one per logical
        // param), while args[]/arg_frame_offs[] keep the flattened placement
        // words. emit's placement loop consumes the slice's second vreg via the
        // is_slice ++i path and the struct-by-value sentinel via the v==0 &&
        // afo!=-1 path. Mirrors the tree-walker (codegen.cpp
        // emit_counted_named_native -> &sig->params). Skipped for struct-by-
        // ptr returns (the hidden dest occupies args[0]/arg_types[0]; that
        // ABI-experimental native shape is not shipped through .em IR).
        if (!ret_struct) {
            if (const NativeSig* nsig = native_named(c.native_binding_name)) {
                in.arg_types.clear();
                for (const Type& p : nsig->params) in.arg_types.push_back(&p);
            }
        }
        emit_depth_check(loc);
    } else if (!c.module_alias.empty()) {
        in.op = ThinOp::CallCrossModule;
        in.meta.mod_id = int32_t(c.cross_module_id);
        in.meta.slot = int32_t(c.cross_module_slot);
        in.meta.cross_module_target_mode = c.cross_module_target_mode;  // Red 7
        if (c.cross_module_unresolved) {
            // deferred trap (module/fn not registered) — mirrors emit_cross_module_call.
            set_term_trap(uint8_t(TrapReason::None));
            return { LoweredValue::Scalar, 0, 0, ret_ty };
        }
        in.meta.base_kind = AbsFixup::ModuleRegistryBase;
        emit_depth_check(loc);
    } else if (c.is_indirect || c.is_lambda_call) {
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
    safety::DepthGuard guard(lower_depth, MAX_COMPILE_DEPTH, "thin_lower::lower_block");
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
    // The trailing cleanup runs on normal block fallthrough ONLY. If the block
    // already terminated (Return/Break/Continue/Trap), the terminator path ran
    // the cleanups via emit_cleanups_to — emitting again would overwrite the
    // block's terminator (emit_defer_site sets a Branch term) and re-run the
    // defer (the defer double-fire bug). The tree-walker emits this trailing
    // cleanup as dead code after a `ret` (unreachable); the IR cannot, because
    // a Block term is a single structured terminator, not a linear `ret`.
    // Skip when the current block already has a terminator.
    if (cur_block().term.kind == TermKind::None)
        emit_cleanup_scope(cleanup_scopes.size() - 1, b.stmts.empty() ? Loc{} : b.stmts.back()->loc);
    cleanup_scopes.pop_back();
    locals = std::move(saved_locals);
    local_types = std::move(saved_types);
}

void ThinLowerer::lower_stmt(const Stmt& s) {
    safety::DepthGuard guard(lower_depth, MAX_COMPILE_DEPTH, "thin_lower::lower_stmt");
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
    // #21 coroutines (plan_MACOS_ARM64.md Phase 8): yield lowers to a 1-arg
    // CallNative to __ember_coro_yield(i64) -> i64. The native performs the
    // cooperative context switch (ember_ctx_switch on Darwin / SwitchToFiber
    // on Windows); on resume it returns and the fn continues after this stmt.
    // Sema guarantees the enclosing fn is a coroutine + the yield value type
    // matches coroutine_yield_type (i64 for v1). A void yield (`yield;`)
    // passes 0. The native's i64 return is discarded (the value is stashed on
    // the coroutine, not returned through x0) — mirrors the tree-walker.
    if (auto* ys = dynamic_cast<const YieldStmt*>(&s)) {
        VReg arg;
        if (ys->value) {
            LoweredValue vv = lower_expr(*ys->value);
            arg = vv.vreg;
        } else {
            arg = new_vreg(&type_i64());
            ThinInstr& z = emit(ThinOp::ConstInt, arg, 0, 0, loc);
            z.imm.i = 0; z.meta.type = &type_i64(); z.meta.width = 8;
        }
        // Resolve the __ember_coro_yield native (sema registered it via
        // ext_coroutine::register_natives). emit resolves by name too, so a
        // null fn_ptr is tolerable; stamping it avoids the emit-time lookup.
        void* yield_fn = nullptr;
        if (ctx.natives) {
            auto it = ctx.natives->find("__ember_coro_yield");
            if (it != ctx.natives->end()) yield_fn = it->second.fn_ptr;
        }
        VReg res = new_vreg(&type_i64());
        ThinInstr in;
        in.op = ThinOp::CallNative;
        in.loc = loc;
        in.dst = res;
        in.args.push_back(arg);
        in.arg_frame_offs.push_back(-1);
        in.arg_types.push_back(&type_i64());
        in.meta.native_name = "__ember_coro_yield";
        in.native_fn = yield_fn;
        in.ret_type = &type_i64();
        in.meta.type = &type_i64(); in.meta.width = 8;
        emit_depth_check(loc);
        cur_block().instrs.push_back(std::move(in));
        // res (the native's discarded i64 return) is left unused — the value
        // was stashed on the coroutine by the native. Continue to the next stmt.
        return;
    }
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
            // If the return-value expression already terminated this block
            // (e.g. an unresolved cross-module call baked a Trap term), the
            // block is unreachable — the trap longjmps past defer cleanups,
            // matching the tree-walker's inline-trap semantics. Do not emit
            // cleanups or a Return term over the Trap.
            if (cur_block().term.kind != TermKind::None) return;
            if (has_defers) emit_cleanups_to(0, loc);
            emit_catch_unwind(0, loc);
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
        // If the return-value expression already terminated this block (e.g. an
        // unresolved cross-module call baked a Trap term), the block is
        // unreachable — skip the defer cleanups + Return term (the trap
        // longjmps past the cleanups, matching the tree-walker inline-trap).
        if (cur_block().term.kind != TermKind::None) return;
        if (has_defers && rs->value) {
            // The tree-walker stashes the return value(s) across defer cleanup (a defer's
            // expression may clobber rax/xmm0/rdx). In the IR we Move the return vregs into
            // fresh stable vregs BEFORE cleanups, run cleanups, then Return the saved ones.
            // CRITICAL: the save vregs MUST be frame-backed (a Move dst with no frame_off
            // is lost — emit_arm64's Move stores to meta.frame_off). Allocate a frame
            // slot for the saved value + set the Move's frame_off so it persists across
            // the cleanup calls (which clobber the return registers). plan_MACOS_ARM64.md
            // Phase 6e: this fixed return_slice_defer (slice return + defer SIGSEGV).
            VReg save_v = 0;
            if (is_slice_ret) {
                int32_t slot = alloc_local("__retsave$slice", rv.ty ? rv.ty : &type_i64());
                save_v = new_vreg(rv.ty);
                VReg save_len = new_vreg(&type_i64());
                ThinInstr& m1 = emit(ThinOp::Move, save_v, rv.vreg, 0, loc); m1.meta.type = rv.ty; m1.meta.width = 8; m1.meta.frame_off = slot;
                ThinInstr& m2 = emit(ThinOp::Move, save_len, rv.vreg + 1, 0, loc); m2.meta.type = &type_i64(); m2.meta.width = 8; m2.meta.frame_off = slot + 8;
                (void)save_len;  // slice len = save_v+1 (consecutive vregs, slice convention)
            } else {
                int32_t slot = alloc_local("__retsave$scalar", rv.ty ? rv.ty : &type_i64());
                save_v = new_vreg(rv.ty);
                ThinInstr& m = emit(ThinOp::Move, save_v, rv.vreg, 0, loc);
                m.meta.type = rv.ty; m.meta.width = value_bytes(rv.ty, ctx.structs);
                m.meta.frame_off = slot;
                if (is_float_ret) m.meta.is_f32 = (rv.ty && rv.ty->prim == Prim::F32) ? 1 : 0;
            }
            emit_cleanups_to(0, loc);
            emit_catch_unwind(0, loc);
            set_term_return(save_v);
            return;
        }
        if (has_defers) emit_cleanups_to(0, loc);
        emit_catch_unwind(0, loc);
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
            loops.push_back({latch, exit_bb, false, cleanup_scopes.size(), active_try_depth});
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
            loops.push_back({cond_bb, exit_bb, false, cleanup_scopes.size(), active_try_depth});
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
            loops.push_back({step_bb, end_bb, false, cleanup_scopes.size(), active_try_depth});
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
    if (auto* fe = dynamic_cast<const ForEachStmt*>(&s)) {
        // for (x in iter) { body } — desugars to a while loop (mirrors CG::exec_stmt
        // ForEachStmt at line ~4668). Two iterable kinds:
        //   - array<T> handle (fe->array_elem_ty set): array_length(h) +
        //     array_get_*(h, i) natives.
        //   - slice T[] (no array_elem_ty): ptr+len indexing via IndexAddr.
        // Both share the loop shape: alloc (handle|ptr)/len/idx/var frame slots,
        // eval the iterable, i=0; while (i < len) { x = elem[i]; body; i++; }.
        if (fe->array_elem_ty) {
            // ---- array-handle for-each ----
            const Type* elem_ty = fe->array_elem_ty;
            const char* get_name =
                (elem_ty->prim == Prim::U8)  ? "array_get_u8"  :
                (elem_ty->prim == Prim::F32) ? "array_get_f32" :
                /* I64 default */              "array_get_i64";
            // Look up the native fn ptrs from ctx.natives (the array extension
            // registers array_length + array_get_* together). The emit resolves
            // by name too (resolve_native_ptr), so a null fn_ptr is tolerable;
            // stamping it avoids a name lookup at emit time.
            void* len_fn = nullptr; void* get_fn = nullptr;
            if (ctx.natives) {
                auto lk = ctx.natives->find("array_length");
                if (lk != ctx.natives->end()) len_fn = lk->second.fn_ptr;
                auto gk = ctx.natives->find(get_name);
                if (gk != ctx.natives->end()) get_fn = gk->second.fn_ptr;
            }
            int fe_id = fe_counter++;
            int32_t h_off   = alloc_local("__fe_h$"   + std::to_string(fe_id), &type_i64());
            int32_t len_off = alloc_local("__fe_len$" + std::to_string(fe_id), &type_i64());
            int32_t idx_off = alloc_local("__fe_idx$" + std::to_string(fe_id), &type_i64());
            int32_t var_off = alloc_local(fe->var, elem_ty);
            // Evaluate the iterable -> the i64 array handle; store to h_off.
            LoweredValue iter_v = lower_expr(*fe->iter);
            store_scalar_local(iter_v, h_off, loc);
            // len = array_length(h). CallNative(h) -> i64.
            {
                VReg h = new_vreg(&type_i64());
                ThinInstr& ld = emit(ThinOp::LoadFrame, h, 0, 0, loc);
                ld.meta.frame_off = h_off; ld.meta.type = &type_i64(); ld.meta.width = 8;
                VReg len_res = new_vreg(&type_i64());
                ThinInstr in;
                in.op = ThinOp::CallNative;
                in.loc = loc;
                in.dst = len_res;
                in.args.push_back(h);
                in.arg_frame_offs.push_back(-1);
                in.arg_types.push_back(&type_i64());
                in.meta.native_name = "array_length";
                in.native_fn = len_fn;
                in.ret_type = &type_i64();
                in.meta.type = &type_i64(); in.meta.width = 8;
                emit_depth_check(loc);
                cur_block().instrs.push_back(std::move(in));
                ThinInstr& st = emit(ThinOp::StoreFrame, 0, len_res, 0, loc);
                st.meta.frame_off = len_off; st.meta.type = &type_i64(); st.meta.width = 8;
            }
            // i = 0.
            {
                VReg zero = new_vreg(&type_i64());
                ThinInstr& z = emit(ThinOp::ConstInt, zero, 0, 0, loc); z.imm.i = 0; z.meta.type = &type_i64(); z.meta.width = 8;
                ThinInstr& st = emit(ThinOp::StoreFrame, 0, zero, 0, loc);
                st.meta.frame_off = idx_off; st.meta.type = &type_i64(); st.meta.width = 8;
            }
            uint32_t top = new_block(), body_bb = new_block(), latch = new_block(), exit_bb = new_block();
            if (cur_block().term.kind == TermKind::None) set_term_jmp(top);
            // top: while (i < len)
            enter_block(top);
            {
                VReg idx = new_vreg(&type_i64());
                ThinInstr& li = emit(ThinOp::LoadFrame, idx, 0, 0, loc);
                li.meta.frame_off = idx_off; li.meta.type = &type_i64(); li.meta.width = 8;
                VReg len = new_vreg(&type_i64());
                ThinInstr& ll = emit(ThinOp::LoadFrame, len, 0, 0, loc);
                ll.meta.frame_off = len_off; ll.meta.type = &type_i64(); ll.meta.width = 8;
                VReg cond = new_vreg(&type_bool());
                ThinInstr& c = emit(ThinOp::Cmp, cond, idx, len, loc);
                c.meta.cmp = 2;  // Lt
                c.meta.type = &type_i64(); c.meta.width = 8;
                c.meta.is_unsigned = 1;  // idx/len are non-negative; unsigned lt
                set_term_branch(cond, body_bb, exit_bb);  // i<len -> body, else exit
            }
            // body_bb: x = array_get_*(h, i); lower body
            enter_block(body_bb);
            {
                VReg h = new_vreg(&type_i64());
                ThinInstr& ld = emit(ThinOp::LoadFrame, h, 0, 0, loc);
                ld.meta.frame_off = h_off; ld.meta.type = &type_i64(); ld.meta.width = 8;
                VReg idx = new_vreg(&type_i64());
                ThinInstr& li = emit(ThinOp::LoadFrame, idx, 0, 0, loc);
                li.meta.frame_off = idx_off; li.meta.type = &type_i64(); li.meta.width = 8;
                const Type* ret_ty = elem_ty;
                VReg res = new_vreg(ret_ty);
                ThinInstr in;
                in.op = ThinOp::CallNative;
                in.loc = loc;
                in.dst = res;
                in.args.push_back(h);
                in.args.push_back(idx);
                in.arg_frame_offs.push_back(-1);
                in.arg_frame_offs.push_back(-1);
                in.arg_types.push_back(&type_i64());
                in.arg_types.push_back(&type_i64());
                in.meta.native_name = get_name;
                in.native_fn = get_fn;
                in.ret_type = ret_ty;
                in.meta.type = ret_ty; in.meta.width = value_bytes(ret_ty, ctx.structs);
                if (ret_ty->is_float()) in.meta.is_f32 = (ret_ty->prim == Prim::F32) ? 1 : 0;
                emit_depth_check(loc);
                cur_block().instrs.push_back(std::move(in));
                LoweredValue elem_lv{ LoweredValue::Scalar, res, 0, ret_ty };
                store_scalar_local(elem_lv, var_off, loc);
                loops.push_back({latch, exit_bb, false, cleanup_scopes.size(), active_try_depth});
                lower_block(fe->body);
                loops.pop_back();
                if (cur_block().term.kind == TermKind::None) set_term_jmp(latch);
            }
            // latch: i = i + 1; budget back-edge; jmp top
            enter_block(latch);
            {
                VReg idx = new_vreg(&type_i64());
                ThinInstr& li = emit(ThinOp::LoadFrame, idx, 0, 0, loc);
                li.meta.frame_off = idx_off; li.meta.type = &type_i64(); li.meta.width = 8;
                VReg one = new_vreg(&type_i64());
                ThinInstr& o = emit(ThinOp::ConstInt, one, 0, 0, loc); o.imm.i = 1; o.meta.type = &type_i64(); o.meta.width = 8;
                VReg inc = new_vreg(&type_i64());
                ThinInstr& a = emit(ThinOp::Add, inc, idx, one, loc);
                a.meta.type = &type_i64(); a.meta.width = 8;
                ThinInstr& st = emit(ThinOp::StoreFrame, 0, inc, 0, loc);
                st.meta.frame_off = idx_off; st.meta.type = &type_i64(); st.meta.width = 8;
                emit_budget_check(block_cost(fe->body), loc);
                set_term_jmp(top);
            }
            enter_block(exit_bb);
            return;
        }
        // ---- slice for-each ----
        // The iter is a slice {ptr, len}; x gets the element at [ptr + idx*esz].
        // The slice is stored as a contiguous 16-byte {ptr,len} frame slot so the
        // StoreFrame/LoadFrame slice path (meta.type = slice) is used — the
        // MakeSlice/ViewExpr result lives in {x0,x1} and is NOT frame-backed, so
        // storing it requires the slice-aware StoreFrame (a no-op load_slice_vreg
        // that trusts {x0,x1}), not a plain i64 StoreFrame (which would try to
        // reload ptr from x9 = garbage).
        const Type* iter_ty = fe->iter->ty;
        const Type* elem_ty = iter_ty && iter_ty->elem ? iter_ty->elem.get() : nullptr;
        int32_t esz = value_bytes(elem_ty, ctx.structs);
        if (esz <= 0) esz = 8;
        int fe_id = fe_counter++;
        int32_t slice_off = alloc_local("__fe_slice$" + std::to_string(fe_id),
                                        iter_ty ? iter_ty : &type_i64());  // 16-byte {ptr,len}
        int32_t idx_off = alloc_local("__fe_idx$" + std::to_string(fe_id), &type_i64());
        int32_t var_off = alloc_local(fe->var, elem_ty ? elem_ty : &type_i64());
        // Evaluate the iterable -> {ptr, len}; store the slice to slice_off.
        LoweredValue iter_v = lower_expr(*fe->iter);
        if (iter_v.kind == LoweredValue::Slice) {
            store_scalar_local(iter_v, slice_off, loc);  // stores {ptr@off, len@off+8}
        } else {
            // non-slice iterable (defensive): store the scalar as ptr, 0 len.
            store_scalar_local(iter_v, slice_off, loc);
            VReg zero = new_vreg(&type_i64());
            ThinInstr& z = emit(ThinOp::ConstInt, zero, 0, 0, loc); z.imm.i = 0; z.meta.type = &type_i64(); z.meta.width = 8;
            ThinInstr& sl = emit(ThinOp::StoreFrame, 0, zero, 0, loc);
            sl.meta.frame_off = slice_off + 8; sl.meta.type = &type_i64(); sl.meta.width = 8;
        }
        // i = 0.
        {
            VReg zero = new_vreg(&type_i64());
            ThinInstr& z = emit(ThinOp::ConstInt, zero, 0, 0, loc); z.imm.i = 0; z.meta.type = &type_i64(); z.meta.width = 8;
            ThinInstr& st = emit(ThinOp::StoreFrame, 0, zero, 0, loc);
            st.meta.frame_off = idx_off; st.meta.type = &type_i64(); st.meta.width = 8;
        }
        uint32_t top = new_block(), body_bb = new_block(), latch = new_block(), exit_bb = new_block();
        if (cur_block().term.kind == TermKind::None) set_term_jmp(top);
        // top: while (i < len)
        enter_block(top);
        {
            VReg idx = new_vreg(&type_i64());
            ThinInstr& li = emit(ThinOp::LoadFrame, idx, 0, 0, loc);
            li.meta.frame_off = idx_off; li.meta.type = &type_i64(); li.meta.width = 8;
            VReg len = new_vreg(&type_i64());
            ThinInstr& ll = emit(ThinOp::LoadFrame, len, 0, 0, loc);
            ll.meta.frame_off = slice_off + 8; ll.meta.type = &type_i64(); ll.meta.width = 8;
            VReg cond = new_vreg(&type_bool());
            ThinInstr& c = emit(ThinOp::Cmp, cond, idx, len, loc);
            c.meta.cmp = 2;  // Lt
            c.meta.type = &type_i64(); c.meta.width = 8;
            c.meta.is_unsigned = 1;
            set_term_branch(cond, body_bb, exit_bb);
        }
        // body_bb: x = [ptr + idx*esz]; lower body
        enter_block(body_bb);
        {
            VReg ptr = new_vreg(&type_i64());
            ThinInstr& lp = emit(ThinOp::LoadFrame, ptr, 0, 0, loc);
            lp.meta.frame_off = slice_off; lp.meta.type = &type_i64(); lp.meta.width = 8;
            VReg idx = new_vreg(&type_i64());
            ThinInstr& li = emit(ThinOp::LoadFrame, idx, 0, 0, loc);
            li.meta.frame_off = idx_off; li.meta.type = &type_i64(); li.meta.width = 8;
            // element address = ptr + idx*esz
            VReg addr = new_vreg(&type_i64());
            ThinInstr& ia = emit(ThinOp::IndexAddr, addr, ptr, idx, loc);
            ia.meta.width = esz;
            ia.meta.type = elem_ty;
            ia.meta.frame_off = 0;  // base is the ptr vreg (src1)
            // load element from [addr + 0]
            VReg res = new_vreg(elem_ty);
            ThinInstr& ld = emit(ThinOp::LoadFrame, res, 0, 0, loc);
            ld.src1 = addr; ld.meta.frame_off = 0; ld.meta.type = elem_ty; ld.meta.width = esz;
            if (elem_ty && elem_ty->is_float()) ld.meta.is_f32 = (elem_ty->prim == Prim::F32) ? 1 : 0;
            LoweredValue elem_lv{ LoweredValue::Scalar, res, 0, elem_ty };
            store_scalar_local(elem_lv, var_off, loc);
            loops.push_back({latch, exit_bb, false, cleanup_scopes.size(), active_try_depth});
            lower_block(fe->body);
            loops.pop_back();
            if (cur_block().term.kind == TermKind::None) set_term_jmp(latch);
        }
        // latch: i = i + 1; budget back-edge; jmp top
        enter_block(latch);
        {
            VReg idx = new_vreg(&type_i64());
            ThinInstr& li = emit(ThinOp::LoadFrame, idx, 0, 0, loc);
            li.meta.frame_off = idx_off; li.meta.type = &type_i64(); li.meta.width = 8;
            VReg one = new_vreg(&type_i64());
            ThinInstr& o = emit(ThinOp::ConstInt, one, 0, 0, loc); o.imm.i = 1; o.meta.type = &type_i64(); o.meta.width = 8;
            VReg inc = new_vreg(&type_i64());
            ThinInstr& a = emit(ThinOp::Add, inc, idx, one, loc);
            a.meta.type = &type_i64(); a.meta.width = 8;
            ThinInstr& st = emit(ThinOp::StoreFrame, 0, inc, 0, loc);
            st.meta.frame_off = idx_off; st.meta.type = &type_i64(); st.meta.width = 8;
            emit_budget_check(block_cost(fe->body), loc);
            set_term_jmp(top);
        }
        enter_block(exit_bb);
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
            loops.push_back({0, end_label, true, cleanup_scopes.size(), active_try_depth});  // break-only
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
    if (auto* ms = dynamic_cast<const MatchStmt*>(&s)) {
        // match (subject) { pattern => body, ... _ => default } — mirrors
        // CG::exec_stmt MatchStmt (line ~4898). Two forms:
        //   (1) literal/enum patterns on an int/bool/enum subject (the common
        //       path; valid_typed_enum_match / valid_match / valid_type_stress).
        //   (2) struct-destructure patterns on a struct subject (Tier 1;
        //       valid_struct_destructure / valid_match_guards).
        // Each arm is a separate branch (no fallthrough): compare the subject
        // against the pattern, branch to the arm body or the next arm's check;
        // the arm body runs then jumps to the end. The last arm is typically
        // the wildcard/default.
        bool has_struct_pat = false;
        for (auto& arm : ms->arms) if (arm.has_struct_pat) { has_struct_pat = true; break; }
        if (has_struct_pat) {
            // ---- struct-destructure match ----
            // The subject must be a local struct variable (mirrors the
            // tree-walker's requirement). Get its frame offset + type.
            int32_t subj_off = 0; const Type* subj_ty = nullptr;
            if (!local_value_offset(*ms->subject, subj_off, subj_ty) ||
                !subj_ty || subj_ty->struct_name.empty() || !ctx.structs ||
                !ctx.structs->count(subj_ty->struct_name)) {
                // subject is not a local struct: defer this match (the
                // tree-walker emits a loud trap; fall back rather than
                // miscompile).
                non_serializable = true;
                non_serializable_reason =
                    "struct-destructure match subject must be a local struct "
                    "variable; falling back to tree-walker";
                return;
            }
            const StructLayout& layout = ctx.structs->at(subj_ty->struct_name);
            std::vector<uint32_t> arm_labels;
            for (size_t i = 0; i < ms->arms.size(); ++i) arm_labels.push_back(new_block());
            uint32_t end_label = new_block();
            int wildcard_idx = -1;
            // For each arm: compare literal-matched fields + eval guard.
            for (size_t i = 0; i < ms->arms.size(); ++i) {
                if (ms->arms[i].is_wildcard) { wildcard_idx = int(i); continue; }
                if (!ms->arms[i].has_struct_pat) continue;  // mixed not supported in v1
                uint32_t arm_bb = arm_labels[i];
                uint32_t next_arm = new_block();  // fail -> next arm's check
                bool matched = true;  // tracks whether any compare was emitted
                for (auto& spf : ms->arms[i].struct_pat.fields) {
                    if (!spf.literal) continue;  // capture-only, no comparison
                    auto fit = layout.fields.find(spf.name);
                    if (fit == layout.fields.end()) continue;
                    const Type* ft = fit->second.ty;
                    int32_t field_off = subj_off + fit->second.offset;
                    // load the subject's field value
                    LoweredValue fv = load_scalar_local(field_off, ft, loc);
                    // load the literal pattern value
                    LoweredValue pv = lower_expr(*spf.literal);
                    VReg cbool = new_vreg(&type_bool());
                    ThinInstr& cb = emit(ThinOp::Cmp, cbool, fv.vreg, pv.vreg, loc);
                    cb.meta.cmp = 0;  // Eq
                    cb.meta.type = ft; cb.meta.width = value_bytes(ft, ctx.structs);
                    cb.meta.is_unsigned = (ft && ft->is_uint()) ? 1 : 0;
                    // mismatch -> next arm; match -> continue to next field/guard
                    uint32_t cont_bb = new_block();
                    set_term_branch(cbool, cont_bb, next_arm);
                    enter_block(cont_bb);
                    matched = true;
                }
                // Guard check (if present): eval the guard, fail if false.
                if (ms->arms[i].guard) {
                    // Bind the arm's capture fields as locals BEFORE the guard
                    // (the guard references them). The captures are the
                    // non-literal fields.
                    auto saved_locals = locals;
                    auto saved_types = local_types;
                    for (auto& spf : ms->arms[i].struct_pat.fields) {
                        if (spf.literal) continue;  // capture-only
                        auto fit = layout.fields.find(spf.name);
                        if (fit == layout.fields.end()) continue;
                        const Type* ft = fit->second.ty;
                        int32_t cap_off = alloc_local(spf.name, ft);
                        copy_frame_frame(cap_off, subj_off + fit->second.offset,
                                         value_bytes(ft, ctx.structs), loc);
                    }
                    LoweredValue gv = lower_expr(*ms->arms[i].guard);
                    VReg zero = new_vreg(gv.ty ? gv.ty : &type_bool());
                    ThinInstr& z = emit(ThinOp::ConstInt, zero, 0, 0, loc);
                    z.imm.i = 0; z.meta.type = gv.ty ? gv.ty : &type_bool();
                    z.meta.width = value_bytes(gv.ty ? gv.ty : &type_bool(), ctx.structs);
                    VReg gbool = new_vreg(&type_bool());
                    ThinInstr& cb = emit(ThinOp::Cmp, gbool, gv.vreg, zero, loc);
                    cb.meta.cmp = 0; cb.meta.type = gv.ty ? gv.ty : &type_bool();
                    cb.meta.width = value_bytes(gv.ty ? gv.ty : &type_bool(), ctx.structs);
                    cb.meta.is_unsigned = (gv.ty && gv.ty->is_uint()) ? 1 : 0;
                    // guard==0 -> next arm; else arm body
                    set_term_branch(gbool, next_arm, arm_bb);
                    // restore scope (captures were only for the guard eval)
                    locals = std::move(saved_locals);
                    local_types = std::move(saved_types);
                    (void)matched;
                } else {
                    // no guard: all literal fields matched -> arm body
                    if (cur_block().term.kind == TermKind::None) set_term_jmp(arm_bb);
                }
                enter_block(next_arm);
            }
            // after the chain: wildcard or end
            set_term_jmp(wildcard_idx >= 0 ? arm_labels[size_t(wildcard_idx)] : end_label);
            // Arm bodies: bind captures (for arms WITHOUT a guard, the captures
            // are bound here; for arms WITH a guard, the guard already bound
            // them in its own scope, so rebind here for the body).
            for (size_t i = 0; i < ms->arms.size(); ++i) {
                enter_block(arm_labels[i]);
                auto saved_locals = locals;
                auto saved_types = local_types;
                if (ms->arms[i].has_struct_pat) {
                    for (auto& spf : ms->arms[i].struct_pat.fields) {
                        if (spf.literal) continue;  // capture-only
                        auto fit = layout.fields.find(spf.name);
                        if (fit == layout.fields.end()) continue;
                        const Type* ft = fit->second.ty;
                        int32_t cap_off = alloc_local(spf.name, ft);
                        copy_frame_frame(cap_off, subj_off + fit->second.offset,
                                         value_bytes(ft, ctx.structs), loc);
                    }
                }
                loops.push_back({0, end_label, true, cleanup_scopes.size(), active_try_depth});  // break-only
                lower_block(ms->arms[i].body);
                loops.pop_back();
                locals = std::move(saved_locals);
                local_types = std::move(saved_types);
                if (cur_block().term.kind == TermKind::None) set_term_jmp(end_label);
            }
            enter_block(end_label);
            return;
        }
        // ---- literal/enum pattern match ----
        // eval the subject -> a Scalar; store to a subject frame slot (the IR
        // holds the subject across the compare chain in a frame slot, unlike
        // the tree-walker which holds it in a volatile register).
        LoweredValue subj = lower_expr(*ms->subject);
        int32_t subj_off = alloc_local("__match_subj$" + std::to_string(match_counter++),
                                       ms->subject->ty ? ms->subject->ty : &type_i64());
        store_scalar_local(subj, subj_off, loc);
        std::vector<uint32_t> arm_labels;
        for (size_t i = 0; i < ms->arms.size(); ++i) arm_labels.push_back(new_block());
        uint32_t end_label = new_block();
        int wildcard_idx = -1;
        for (size_t i = 0; i < ms->arms.size(); ++i) {
            if (ms->arms[i].is_wildcard) { wildcard_idx = int(i); continue; }
            // reload subject + load pattern, compare Eq
            LoweredValue sv = load_scalar_local(subj_off, ms->subject->ty, loc);
            LoweredValue pv = lower_expr(*ms->arms[i].pattern);
            VReg cbool = new_vreg(&type_bool());
            ThinInstr& cb = emit(ThinOp::Cmp, cbool, sv.vreg, pv.vreg, loc);
            cb.meta.cmp = 0;  // Eq
            cb.meta.type = ms->subject->ty; cb.meta.width = value_bytes(ms->subject->ty, ctx.structs);
            cb.meta.is_unsigned = (ms->subject->ty && ms->subject->ty->is_uint()) ? 1 : 0;
            // equal -> arm body; else next arm's check in a new block
            uint32_t next_cmp = new_block();
            set_term_branch(cbool, arm_labels[i], next_cmp);
            enter_block(next_cmp);
        }
        // after the chain: wildcard arm or end
        set_term_jmp(wildcard_idx >= 0 ? arm_labels[size_t(wildcard_idx)] : end_label);
        for (size_t i = 0; i < ms->arms.size(); ++i) {
            enter_block(arm_labels[i]);
            loops.push_back({0, end_label, true, cleanup_scopes.size(), active_try_depth});  // break-only
            lower_block(ms->arms[i].body);
            loops.pop_back();
            if (cur_block().term.kind == TermKind::None) set_term_jmp(end_label);
        }
        enter_block(end_label);
        return;
    }
    // Tier 4: try { ... } catch (name) { ... } — lowered to the CFG as a
    // TryCatch (inline setjmp into context_t::catch_bufs, catch_target = the
    // catch block) in the try block, the try body in following blocks, a
    // CatchCleanup(imm=1) (normal-completion pop) + Jmp end at the try body's
    // tail, a catch block (CatchEntry loads thrown_value into the catch_name
    // slot + the catch body + Jmp end), and a continuation end block. A throw
    // is the Throw op (longjmp-or-trap). Mirrors CG::exec_stmt's TryCatchStmt /
    // ThrowStmt; the emit (thin_emit.cpp) reuses the same X64Emitter sequence +
    // context_offsets + emit_trap, so the IR-backend try/catch is byte-for-byte
    // the same setjmp/longjmp as the tree-walker's.
    if (auto* tc = dynamic_cast<const TryCatchStmt*>(&s)) {
        if (!ctx.use_context_reg) {
            // Without a context register the catch stack is unavailable. Fall
            // back to the tree-walker (which emits the loud trap), not a silent
            // miscompile. (The Red 9 gate compiles try/catch with
            // use_context_reg=true, so this path is the host-setup-error guard.)
            non_serializable = true;
            non_serializable_reason =
                "try/catch requires a context register (use_context_reg); "
                "falling back to tree-walker";
            return;
        }
        uint32_t catch_bb = new_block();   // pre-allocate the catch target
        uint32_t end_bb   = new_block();   // pre-allocate the continuation
        // Bind the catch_name i64 slot up front (the CatchEntry op loads
        // thrown_value here; the catch body references it as a local). Sema
        // declared catch_name as i64 in the catch block's scope.
        int32_t catch_off = alloc_local(tc->catch_name, &type_i64());
        // TryCatch: inline setjmp. meta.slot = catch block id (the catch-entry
        // rip saved into catch_bufs is block_labels[catch_bb]); meta.frame_off =
        // the catch_name slot (carried for the CatchEntry op's reference).
        ThinInstr& tcop = emit(ThinOp::TryCatch, 0, 0, 0, loc);
        tcop.meta.slot = int32_t(catch_bb);
        tcop.meta.frame_off = catch_off;
        tcop.meta.type = &type_i64(); tcop.meta.width = 8;
        ++active_try_depth;
        lower_block(tc->try_body);
        --active_try_depth;
        // Normal try completion: pop the catch handler + jump to end. If the
        // try body already terminated (a return/throw/break), the block is
        // sealed and this is unreachable — only emit when the block is open.
        if (cur_block().term.kind == TermKind::None) {
            ThinInstr& pop = emit(ThinOp::CatchCleanup, 0, 0, 0, loc);
            pop.imm.i = 1; pop.meta.type = &type_i64(); pop.meta.width = 8;
            set_term_jmp(end_bb);
        }
        // Catch block: a throw's longjmp restored registers + rsp + rip to
        // land at block_labels[catch_bb]. Load thrown_value into the catch_name
        // slot, then run the catch body with catch_name visible as a local.
        enter_block(catch_bb);
        ThinInstr& centry = emit(ThinOp::CatchEntry, 0, 0, 0, loc);
        centry.meta.frame_off = catch_off;
        centry.meta.type = &type_i64(); centry.meta.width = 8;
        {
            auto saved_locals = locals;
            auto saved_types = local_types;
            locals[tc->catch_name] = catch_off;
            local_types[tc->catch_name] = &type_i64();
            lower_block(tc->catch_body);
            locals = std::move(saved_locals);
            local_types = std::move(saved_types);
        }
        if (cur_block().term.kind == TermKind::None) set_term_jmp(end_bb);
        // Continuation end block.
        enter_block(end_bb);
        return;
    }
    // Tier 4: throw expr; — the Throw op. Eval the i64 value into a vreg, then
    // Throw(src1 = that vreg). Emit stores it into context_t::thrown_value +
    // longjmps to the nearest catch (or traps UnhandledThrow). Mirrors CG's
    // ThrowStmt.
    if (auto* th = dynamic_cast<const ThrowStmt*>(&s)) {
        if (!ctx.use_context_reg) {
            non_serializable = true;
            non_serializable_reason =
                "throw requires a context register (use_context_reg); "
                "falling back to tree-walker";
            return;
        }
        LoweredValue v = th->value ? lower_expr(*th->value) : LoweredValue{LoweredValue::Scalar, 0, 0, &type_i64()};
        VReg val = v.vreg;
        if (val == 0) {
            // void throw (`throw;`): throw 0.
            val = new_vreg(&type_i64());
            ThinInstr& z = emit(ThinOp::ConstInt, val, 0, 0, loc);
            z.imm.i = 0; z.meta.type = &type_i64(); z.meta.width = 8;
        }
        ThinInstr& top = emit(ThinOp::Throw, 0, val, 0, loc);
        top.meta.type = &type_i64(); top.meta.width = 8;
        // A throw never falls through. Seal this block with a Trap terminator
        // (unreachable: the emit either longjmps or traps) + start a fresh
        // unreachable block so subsequent lowering has somewhere to go.
        set_term_trap(uint8_t(TrapReason::None));
        new_and_enter();
        return;
    }
    if (auto* bs = dynamic_cast<const BlockStmt*>(&s)) { lower_block(bs->block); return; }
    if (dynamic_cast<const BreakStmt*>(&s)) {
        if (!loops.empty()) {
            emit_cleanups_to(loops.back().cleanup_depth, loc);
            emit_catch_unwind(loops.back().try_depth, loc);
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
                emit_catch_unwind(loops[size_t(i)].try_depth, loc);
                set_term_jmp(loops[size_t(i)].cond_bb);
                new_and_enter();
                break;
            }
        }
        return;
    }
    // Defensive: any statement type not handled above (a future node, or a
    // ForEachStmt/MatchStmt arm with an unsupported sub-feature) must NOT
    // silently lower to nothing. Flag non_serializable so the function falls
    // back to the tree-walker (the post-lowering check honors it).
    non_serializable = true;
    non_serializable_reason =
        "unhandled statement node reached IR lowering; falling back to tree-walker";
}

} // namespace

ThinFunction lower_function(const FuncDecl& f, const CodeGenCtx& ctx) {
    ThinLowerer lw(ctx, f);
    return lw.run();
}

} // namespace ember
