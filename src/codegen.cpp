#include "codegen.hpp"
#include "engine.hpp"
#include "context.hpp"   // TrapReason (unified trap surface, v0.4)
#include <cassert>
#include <cstring>
#include <algorithm>
#include <array>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__GNUC__)
#include <cpuid.h>
#endif

namespace ember {

// Read CPUID.1:EAX (the CPU signature the obfuscation pass keys on).
int64_t current_cpuid_signature() {
#if defined(_MSC_VER)
    int regs[4] = {0}; __cpuid(regs, 1); return regs[0];
#elif defined(__GNUC__)
    unsigned a=0,b=0,c=0,d=0;
    if (__get_cpuid(1, &a,&b,&c,&d)) return a;
#endif
    return 0;
}

namespace {

// round up to multiple of 16
inline int32_t round16(int32_t n) { return (n + 15) & ~15; }

struct CG {
    X64Emitter e;
    const CodeGenCtx& ctx;
    const FuncDecl& f;

    // frame layout: locals + arg temps, all 8-byte slots, rbp-relative
    // rbx_save_offset (Item E): the FIRST reservation made in every
    // function's frame (see compile_func), always -8 - a fixed slot for the
    // unconditional callee-saved rbx save/restore, reserved via the same
    // next_local_off bump every other frame slot uses (NOT a raw push/pop,
    // which would touch rsp and break the 16-byte-aligned frame_size the
    // rest of this file already assumes).
    int32_t rbx_save_offset = 0;
    int32_t next_local_off = 0;          // grows downward (negative offsets)
    std::unordered_map<std::string, int32_t> locals;  // name -> offset from rbp
    std::unordered_map<std::string, const Type*> local_types;
    int32_t arg_temps_base = 0;          // offset of arg-temp area start
    int32_t frame_size = 0;
    bool makes_calls = false;
    int max_args = 0;

    CG(const CodeGenCtx& c, const FuncDecl& fn) : ctx(c), f(fn) {}

    // byte width of a local's frame slot. Slices are {ptr,len} = 16 bytes
    // (slice ABI: see eval()'s StringLit/Ident cases). Fixed-size arrays T[N]
    // are N tightly-packed elem_bytes(T) slots (see elem_bytes below) so a
    // fixed array can be viewed as a real contiguous slice via `arr[..]`.
    // A registered struct type (looked up in `structs`) gets its full layout
    // size. Everything else (scalars, i64 handles) is a flat 8-byte slot.
    static int32_t local_width_bytes(const Type* t, const StructLayoutTable* structs) {
        if (t && t->is_slice) return 16;
        if (t && t->array_len > 0) return int32_t(t->array_len) * elem_bytes(t->elem.get());
        if (t && !t->struct_name.empty() && structs) {
            auto it = structs->find(t->struct_name);
            if (it != structs->end()) return it->second.size;
        }
        return 8;
    }

    // byte width of one array/slice element for JIT frame storage - distinct
    // from Type::byte_size() (which isn't used for frame layout here).
    static int32_t elem_bytes(const Type* t) {
        if (!t) return 8;
        switch (t->prim) {
        case Prim::Bool: case Prim::I8: case Prim::U8: return 1;
        case Prim::I16: case Prim::U16: return 2;
        case Prim::I32: case Prim::U32: case Prim::F32: return 4;
        default: return 8; // I64/U64/F64 and handle types (i64 under the hood)
        }
    }

    // number of 8-byte "words" a call-site operand (or param) occupies in
    // ember's private calling convention: 1 for a scalar/float/handle, 2 for
    // a slice ({ptr,len}), ceil(size/8) for a registered struct passed by
    // value (sema restricts such an argument to a bare local Ident - see
    // check_struct_arg_shape - so codegen always has a source address to
    // copy the words from, never an arbitrary expression to evaluate).
    static int32_t words_for_type(const Type* t, const StructLayoutTable* structs) {
        if (t && t->is_slice) return 2;
        if (t && !t->struct_name.empty() && structs) {
            auto it = structs->find(t->struct_name);
            if (it != structs->end()) return (it->second.size + 7) / 8;
        }
        return 1;
    }

    // Copy exactly `total_bytes` from [src_base+src_off] to [dst_base+dst_off]
    // via rax, in 8/4/2/1-byte chunks (never a blind "N full 8-byte words"
    // copy). Used for every struct-by-value memory-to-memory copy (call-site
    // argument stashing, struct-returning-call forwarding, return-value
    // writes). A struct's byte size is frequently NOT a multiple of 8 (e.g.
    // Mixed: i64+f32+u8+bool = 14 bytes; words_for_type rounds that up to 2
    // words = 16 bytes for register/stack-slot PLACEMENT purposes only). A
    // naive word-granularity copy reads/writes up to 7 bytes past the
    // struct's true extent on both ends. Confirmed harmful in practice: when
    // such a struct is a function's first local or param, that overrun lands
    // on the function's own saved rbp, corrupting it - the callee's epilogue
    // then pops a bad frame pointer, so every rbp-relative access after the
    // corrupted call returns (in the caller) goes to the wrong address, only
    // crashing well after the actual corruption happened.
    void copy_bytes(Reg dst_base, int32_t dst_off, Reg src_base, int32_t src_off, int32_t total_bytes) {
        int32_t done = 0;
        while (done < total_bytes) {
            int32_t remaining = total_bytes - done;
            int chunk = remaining >= 8 ? 8 : remaining >= 4 ? 4 : remaining >= 2 ? 2 : 1;
            e.load_elem_to_rax(src_base, src_off + done, chunk, false);
            e.store_rax_elem(dst_base, dst_off + done, chunk);
            done += chunk;
        }
    }
    // Write rax's low `nbytes` to [base+off], chunked - used when a struct
    // word's value is already in rax (e.g. moved out of an arg register)
    // rather than sourced from memory, so copy_bytes' load side doesn't fit.
    // store_rax_elem always writes rax's LOW `chunk` bytes, so after each
    // chunk (except the last) rax must be shifted right to bring the next
    // unwritten bytes down to the low end - clobbers rax (fine: every call
    // site reloads it fresh before use).
    void store_rax_bytes(Reg base, int32_t off, int32_t nbytes) {
        int32_t done = 0;
        while (done < nbytes) {
            int chunk = nbytes - done >= 4 ? 4 : nbytes - done >= 2 ? 2 : 1;
            e.store_rax_elem(base, off + done, chunk);
            done += chunk;
            if (done < nbytes) e.shr_reg_imm8(Reg::rax, uint8_t(chunk * 8));
        }
    }

    // Runtime array/slice bounds check (Part 1 of the bounds-checking work -
    // see IndexExpr's load path and AssignExpr's IndexExpr-target store path
    // below, both of which call this). `idx_reg` holds the already-evaluated
    // index (a full 64-bit value, sign-extended per this codegen's normal
    // int convention). `len_reg` overload compares against a runtime length
    // already sitting in a register (a slice's rdx, per the ABI comments
    // throughout this file); the imm overload compares against a compile-
    // time-known length (a fixed array's N).
    //
    // Comparison strategy: a single UNSIGNED compare (`jb`, "jump if
    // idx < len as unsigned" to the in-bounds continuation) catches BOTH a
    // too-large index AND a negative one in one shot - a negative int64
    // index reinterpreted as unsigned is enormous (e.g. -1 ->
    // 0xFFFFFFFFFFFFFFFF), so it's always >= any real length without
    // needing a separate `idx < 0` check.
    //
    // On failure: trap via `ud2` (raises #UD -> EXCEPTION_ILLEGAL_
    // INSTRUCTION), NOT the semantically-closer EXCEPTION_ARRAY_BOUNDS_
    // EXCEEDED. This needs explaining:
    //
    // x86's `BOUND` instruction (opcode 0x62) is the one hardware
    // instruction that raises #BR/EXCEPTION_ARRAY_BOUNDS_EXCEEDED directly -
    // but it is NOT encodable at all in 64-bit long mode (that opcode byte
    // was repurposed as a VEX-prefix escape for AVX), so there is no
    // hardware instruction this JIT can emit that raises that code.
    //
    // The alternative - a host-provided native (mirroring __str_decrypt's
    // "codegen calls a host function by absolute address" pattern) that
    // calls Win32 RaiseException(EXCEPTION_ARRAY_BOUNDS_EXCEEDED, ...) - was
    // tried first and empirically confirmed UNRELIABLE from this JIT:
    // RaiseException's dispatch goes through the real Windows SEH machinery
    // (RtlDispatchException / stack unwinding), which expects every frame on
    // the call stack to have registered .pdata/.xdata unwind metadata; this
    // JIT's code has none (it's raw bytes in an exec-memory page, never
    // registered with RtlAddFunctionTable). In testing, calling that native
    // from JIT'd code intermittently hard-crashed the WHOLE PROCESS instead
    // of being caught by ScriptCrashVeh's VEH - reproducibly flipping from
    // "works" to "process-terminating crash that bypasses the guard
    // entirely" based on incidental, unrelated frame-size/stack-layout
    // changes elsewhere in the same function (verified directly: adding an
    // unrelated unused local elsewhere in the same script was enough to flip
    // the outcome). That's strictly worse than the thing this whole task is
    // trying to fix (an uncaught crash), so it was abandoned.
    //
    // `ud2` has none of these problems: it's a pure CPU decode-time trap
    // (#UD) with no memory access, no register requirements, no stack
    // alignment requirements, and no dependency on unwind metadata for
    // dispatch - the OS's hardware-exception delivery path (the SAME path
    // EXCEPTION_ACCESS_VIOLATION/EXCEPTION_INT_DIVIDE_BY_ZERO already use
    // successfully from this exact JIT, per the pre-existing div-by-zero
    // trap) doesn't need to unwind anything to deliver a first-chance
    // exception to the VEH. It's also already a codebase-proven pattern
    // (emit_cpuid_gate's trap, above) and already a handled/caught code in
    // ScriptCrashVeh (EXCEPTION_ILLEGAL_INSTRUCTION). The user-visible
    // result is a graceful "RUNTIME: illegal instruction" trap rather than
    // "RUNTIME: array bounds exceeded" - a less specific description, but a
    // script-crashing bounds violation either way, with the process staying
    // alive exactly as required.
    void emit_bounds_check_reg(Reg idx_reg, Reg len_reg) {
        e.cmp_reg_reg(idx_reg, len_reg); // sets flags from idx-len
        Label ok = e.alloc_label();
        e.jcc(Cond::b, ok); // idx < len (unsigned) -> in bounds, continue
        emit_trap(int(TrapReason::BoundsCheck), "bounds check: index out of range");
        e.bind(ok);
    }
    // Same idea, but against a compile-time-known length (a fixed array's N).
    void emit_bounds_check_imm(Reg idx_reg, int64_t len) {
        // cmp_reg_imm32 sign-extends its imm32, but the branch below reads
        // the unsigned Cond::b relation over the raw bit patterns - same
        // reasoning as the register overload above: a fixed array's length
        // is always a small non-negative value that fits in imm32 (array
        // sizes are bounded by frame layout arithmetic elsewhere in this
        // file), so idx (unsigned) >= len is exactly "out of range,
        // including negative idx".
        e.cmp_reg_imm32(idx_reg, int32_t(len));
        Label ok = e.alloc_label();
        e.jcc(Cond::b, ok);
        emit_trap(int(TrapReason::BoundsCheck), "bounds check: index out of range");
        e.bind(ok);
    }

    // --- v0.4 unified trap surface (SAFETY_AND_SANDBOX.md §2-§4, REDSHELL V7) ---
    // Emit a trap call. If ctx.trap_stub is set, calls it via Win64 ABI:
    //   rcx = ctx.trap_ctx (context_t*), edx = reason (TrapReason), r8 = detail (const char*)
    // The stub longjmps to the checkpoint (never returns). If trap_stub is null,
    // falls back to ud2 (pre-v0.4 behavior — backward compatible, process death
    // unless the host installed a VEH). `detail` is baked as a rodata imm64
    // (a string literal's address) or null.
    //
    // Caller must preserve rax/any live value-register across this: traps never
    // return when a stub is set (longjmp), so live values are irrelevant; when
    // the stub is null the ud2 kills the process, also irrelevant. So we can
    // freely clobber rax/rcx/rdx/r8 here.
    void emit_trap(int reason_ord, const char* detail) {
        if (ctx.trap_stub) {
            // Win64: the stub is a normal C function — needs 16-byte-aligned
            // rsp at the call + 32-byte shadow space. At a trap site rsp is at
            // the frame base (16-aligned by the prologue), so a raw `call`
            // would misalign to 8 and fault. Reserve 0x28 (40 = 32 shadow + 8
            // align) before the call; the stub longjmps (never returns) so the
            // reserved space is never restored by us — longjmp handles rsp.
            e.byte(0x48); e.byte(0x83); e.byte(0xEC); e.byte(0x28); // sub rsp, 0x28
            e.mov_reg_imm64(Reg::rcx, int64_t(ctx.trap_ctx));
            e.mov_reg_imm64(Reg::rdx, int64_t(reason_ord));
            e.mov_reg_imm64(Reg::r8,  int64_t(reinterpret_cast<uintptr_t>(detail)));
            e.mov_reg_imm64(Reg::rax, int64_t(ctx.trap_stub));
            e.call_reg(Reg::rax);
            // stub does not return (longjmps); if it ever did, fall through.
            e.byte(0x48); e.byte(0x83); e.byte(0xC4); e.byte(0x28); // add rsp, 0x28
        } else {
            e.byte(0x0F); e.byte(0x0B); // ud2 (raises #UD, pre-v0.4 trap)
        }
    }

    // v0.4 instruction budget (SAFETY_AND_SANDBOX.md §3). Emitted at LOOP
    // BACK-EDGES ONLY (the actual runaway-loop catch point), gated by
    // ctx.emit_budget_checks + ctx.budget_ptr. Zero emitted when off.
    //   mov rax, budget_ptr
    //   sub qword [rax], body_cost   ; coarse per-iteration instruction count
    //   jg  .continue                ; budget_remaining > 0, keep looping
    //   emit_trap(BudgetExceeded)   ; else trap (longjmp to checkpoint)
    //   .continue:
    // body_cost is a coarse statement count of the loop body (the spec's
    // "how much work happened" proxy — not cycle-accurate, just enough that
    // a true infinite loop drives it to zero in finite iterations). A bare
    // `while(true){}` floors at 1 so even an empty infinite loop is caught.
    void emit_budget_check(int64_t body_cost) {
        if (!ctx.emit_budget_checks || !ctx.budget_ptr || body_cost <= 0) return;
        e.mov_reg_imm64(Reg::rax, int64_t(ctx.budget_ptr));
        // sub qword ptr [rax], imm32 : REX.W (48) + opcode 81 /5 + imm32
        e.byte(0x48); e.byte(0x81); e.byte(0x28); e.imm32(int32_t(body_cost));
        Label cont = e.alloc_label();
        e.jcc(Cond::g, cont);              // budget_remaining > 0 -> continue
        emit_trap(int(TrapReason::BudgetExceeded), "budget exceeded at loop back-edge");
        e.bind(cont);
    }

    // Coarse per-iteration cost of a loop body: count statements (each ~a few
    // emitted instructions). Floors at 1 so an empty infinite loop is caught.
    int64_t block_cost(const Block& b);
    int64_t stmt_cost(const Stmt& s);

    // v0.4 stack-depth guard (SAFETY_AND_SANDBOX.md §4). Emitted at
    // SCRIPT-TO-SCRIPT call entry only (not native calls — natives have
    // their own stacks), gated by ctx.emit_depth_checks + ctx.depth_ptr.
    // Zero emitted when off. Pairs with emit_depth_leave() after the call.
    //   mov rax, depth_ptr
    //   inc dword [rax]            ; ++call_depth
    //   cmp dword [rax], max       ; depth >= max?
    //   jge .trap                  ; yes -> overflow
    //   .ok:
    //   <the call>
    //   ... emit_depth_leave() after the call returns: dec dword [rax]
    void emit_depth_check() {
        if (!ctx.emit_depth_checks || !ctx.depth_ptr) return;
        e.mov_reg_imm64(Reg::rax, int64_t(ctx.depth_ptr));
        e.byte(0xFF); e.byte(0x00);                  // inc dword ptr [rax]
        e.byte(0x81); e.byte(0x38); e.imm32(int32_t(ctx.max_call_depth)); // cmp dword [rax], imm32
        Label ok = e.alloc_label();
        e.jcc(Cond::l, ok);                          // depth < max -> ok
        emit_trap(int(TrapReason::StackOverflow), "stack overflow: call depth exceeded");
        e.bind(ok);
    }
    void emit_depth_leave() {
        if (!ctx.emit_depth_checks || !ctx.depth_ptr) return;
        // After the call returns, rax HOLDS THE RESULT — must not clobber it.
        // Use r10 (caller-saved scratch, not a return/arg reg) for the ptr load.
        e.mov_reg_imm64(Reg::r10, int64_t(ctx.depth_ptr));
        e.byte(0x49); e.byte(0xFF); e.byte(0x0A);  // dec dword ptr [r10]  (REX.B=1, /1, rm=r10)
    }

    int32_t alloc_local(const std::string& n, const Type* t) {
        int32_t width = local_width_bytes(t, ctx.structs);
        next_local_off += width;
        int32_t off = -next_local_off;
        locals[n] = off;
        local_types[n] = t;
        return off;
    }

    // find max arg count + whether any calls (pre-pass over the function body)
    void prescan_block(const Block& b) {
        for (auto& s : b.stmts) prescan_stmt(*s);
    }
    void prescan_stmt(const Stmt& s) {
        if (auto* ls = dynamic_cast<const LetStmt*>(&s)) { if (ls->init) prescan_expr(*ls->init); return; }
        if (auto* es = dynamic_cast<const ExprStmt*>(&s)) { prescan_expr(*es->expr); return; }
        if (auto* rs = dynamic_cast<const ReturnStmt*>(&s)) { if (rs->value) prescan_expr(*rs->value); return; }
        if (auto* ds = dynamic_cast<const DeferStmt*>(&s)) { prescan_expr(*ds->expr); return; }
        if (auto* is = dynamic_cast<const IfStmt*>(&s)) {
            prescan_expr(*is->cond); prescan_block(is->then_b);
            if (is->has_else) prescan_block(is->else_b); return;
        }
        if (auto* ws = dynamic_cast<const WhileStmt*>(&s)) { prescan_expr(*ws->cond); prescan_block(ws->body); return; }
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

    // Item E ("hot local pinning") candidate selection: a purely syntactic,
    // static pre-pass over ONE loop's body (not the whole function - loops
    // are pinned independently, one at a time), counting textual references
    // (reads AND writes) to each name. Same recursive dynamic_cast walking
    // idiom as prescan_block/prescan_stmt/prescan_expr above, just counting
    // instead of tallying call-argument counts.
    void count_pin_refs_block(const Block& b, std::unordered_map<std::string,int>& counts) {
        for (auto& s : b.stmts) count_pin_refs_stmt(*s, counts);
    }
    void count_pin_refs_stmt(const Stmt& s, std::unordered_map<std::string,int>& counts) {
        if (auto* ls = dynamic_cast<const LetStmt*>(&s)) { if (ls->init) count_pin_refs_expr(*ls->init, counts); return; }
        if (auto* es = dynamic_cast<const ExprStmt*>(&s)) { count_pin_refs_expr(*es->expr, counts); return; }
        if (auto* rs = dynamic_cast<const ReturnStmt*>(&s)) { if (rs->value) count_pin_refs_expr(*rs->value, counts); return; }
        if (auto* ds = dynamic_cast<const DeferStmt*>(&s)) { count_pin_refs_expr(*ds->expr, counts); return; }
        if (auto* is = dynamic_cast<const IfStmt*>(&s)) {
            count_pin_refs_expr(*is->cond, counts); count_pin_refs_block(is->then_b, counts);
            if (is->has_else) count_pin_refs_block(is->else_b, counts); return;
        }
        if (auto* ws = dynamic_cast<const WhileStmt*>(&s)) { count_pin_refs_expr(*ws->cond, counts); count_pin_refs_block(ws->body, counts); return; }
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
    // Eligible = a plain scalar local (not slice/array/struct - provably
    // never address-taken under ember's semantics, no escape analysis
    // needed), declared in an OUTER scope (already present in `locals` at
    // the point the loop is reached - a function param, an enclosing `let`,
    // or the ForStmt's own init clause; never a local declared fresh INSIDE
    // the loop body, which is re-initialized every iteration and gains
    // nothing from pinning), referenced at least twice textually within the
    // loop body (N=2 - the smallest threshold where pinning can't lose:
    // below that, the pin-entry load itself costs more than it saves).
    // Returns the single highest-count candidate, or nullopt if none
    // qualify (a tie is broken arbitrarily but deterministically for a
    // given input - which exact candidate wins a tie has no correctness
    // impact, since every candidate considered here is independently safe
    // to pin). v1 pins exactly one register per loop - the simplest correct
    // increment given zero prior register allocation exists.
    std::optional<std::string> find_pin_candidate(const Block& loop_body) {
        std::unordered_map<std::string,int> counts;
        count_pin_refs_block(loop_body, counts);
        std::string best; int best_count = 0;
        for (auto& kv : counts) {
            if (kv.second < 2) continue; // Task 2's N=2 floor
            auto lit = locals.find(kv.first);
            if (lit == locals.end()) continue; // not an outer-scope local at all (e.g. a global, or unresolved)
            auto tit = local_types.find(kv.first);
            if (tit == local_types.end() || !tit->second) continue;
            const Type* t = tit->second;
            // scalar only, AND general-purpose-register-compatible: a
            // float local lives in xmm registers (movss convention), not
            // rax/rbx - the pin fast paths in eval()'s Ident case and
            // AssignExpr's Ident-target case only implement the plain
            // integer/bool (GP register) convention, so a float candidate
            // here would silently corrupt the value through a bare
            // mov_reg_reg into rax/rbx instead of an xmm move.
            if (t->is_slice || t->array_len > 0 || !t->struct_name.empty() || t->is_float()) continue;
            if (kv.second > best_count) { best = kv.first; best_count = kv.second; }
        }
        if (best_count == 0) return std::nullopt;
        return best;
    }

    // --- expression evaluation: int/bool result in rax, f32 in xmm0 ---
    void eval(const Expr& ex);

    // load a value (int or float) from a stack slot [rbp + off]
    void load_slot(int32_t off, const Type* t) {
        if (t->is_float()) e.movss_xmm_mem(Xmm::xmm0, Reg::rbp, off);
        else e.load_reg_mem(Reg::rax, Reg::rbp, off);
    }
    // store rax (int) or xmm0 (float) to a stack slot [rbp + off]
    void store_slot(int32_t off, const Type* t) {
        if (t->is_float()) e.movss_mem_xmm(Reg::rbp, off, Xmm::xmm0);
        else { // mov [rbp+off], rax  -> REX.W 89 /r mod=10 reg=rax rm=rbp
            e.byte(0x48); e.byte(0x89);
            e.byte(0x85 | 0); // mod=10, reg=0(rax), rm=5(rbp) -> 10 000 101 = 0x85
            // actually modrm(0b10, rax=0, rbp=5) = (2<<6)|(0<<3)|5 = 0x85. correct.
            // but we already emitted 0x85 above; need disp32
            e.imm32(off);
        }
    }
    // Actually the above hand-encoded store is fragile; use a helper via emitter.
    // (Replaced below by a proper emitter method call.)

    void emit_epilogue() {
        // Unconditional rbx restore (Item E, register-pinning prerequisite):
        // every function saves rbx to a reserved frame slot in its prologue
        // regardless of whether it personally uses the hot-local pin, so rbx
        // preservation is an ember-internal ABI invariant independent of any
        // callee's own implementation - exactly like a real Win64
        // callee-saved register. Without this, a script-to-script call has
        // no guarantee a caller's pinned rbx value survives the callee's own
        // (possibly conflicting) use of the same register. Restored via a
        // plain load from the reserved slot (NOT pop) while the frame is
        // still live, before rsp/rbp are torn down - this is the single
        // shared exit path (every ReturnStmt and the implicit fallthrough
        // all call this), so one edit here covers every exit uniformly.
        e.load_reg_mem(Reg::rbx, Reg::rbp, rbx_save_offset);
        e.mov_reg_reg(Reg::rsp, Reg::rbp);
        e.pop(Reg::rbp);
        e.ret();
    }

    // loop context for break/continue (CODEGEN_SPEC.md). v0.x codegen had these
    // as silent no-ops (fall-through bug surfaced by the spectator_list port).
    // is_switch: a switch pushes a frame too (so `break` can exit it via the
    // same mechanism) but `continue` must skip past it to the nearest real
    // loop - switch is a break-only construct, not a loop.
    struct LoopCtx { Label cont; Label brk; bool is_switch = false; };
    std::vector<LoopCtx> loops;

    // Item E ("hot local pinning"): at most one pin active at a time,
    // scoped to a single loop's duration (entered/cleared around a
    // WhileStmt/ForStmt's body exactly like `loops.push_back`/`pop_back`
    // above). The register is fixed fleet-wide (rbx - see Step 0's
    // unconditional prologue/epilogue save/restore, the ABI-style contract
    // that makes pinning safe across script-to-script calls). While active,
    // eval()'s Ident case and AssignExpr's Ident-target case fast-path
    // reads/writes of the matching name through the register instead of
    // reloading from [rbp+off] - but the stack slot stays the always-synced
    // backing store (write-through on every write), so nothing outside
    // those two fast paths needs to know pinning exists at all.
    struct PinState { std::string name; Reg reg; };
    std::optional<PinState> active_pin;

    // --- defer (function-scoped, LIFO at every return/fall-off-end) ---
    // v1 semantic: each textual `defer EXPR;` site gets one runtime flag,
    // set when control reaches it. At every function exit, flags are checked
    // in reverse (LIFO) order and each set flag's EXPR runs once. Known
    // simplification vs. Go: a defer inside a loop body fires at most once
    // per call regardless of iteration count (a static flag, not a dynamic
    // per-iteration stack) - correct for the common "conditional cleanup"
    // pattern (`if (opened) { defer close(f); }`), not for loop-accumulated
    // defers. Populated by compile_func's collect_defers pass before the
    // body is walked (DeferStmt codegen needs its flag's offset up front).
    std::vector<const DeferStmt*> defer_sites;
    std::vector<int32_t> defer_flag_offsets;
    void emit_defers() {
        for (int i = int(defer_sites.size()) - 1; i >= 0; --i) {
            e.load_reg_mem(Reg::rax, Reg::rbp, defer_flag_offsets[size_t(i)]);
            e.cmp_reg_imm32(Reg::rax, 0);
            Label skip = e.alloc_label();
            e.jcc(Cond::e, skip);
            eval(*defer_sites[size_t(i)]->expr); // side effect only; result discarded
            e.bind(skip);
        }
    }

    // --- struct-by-value return (hidden pointer) ---
    // A function whose declared return type is a registered struct gets an
    // implicit extra parameter at word 0: the address the caller wants the
    // struct written to. compile_func's prologue spills it here (before the
    // real params, which shift to start at word 1) instead of into the
    // named-local `locals` map (it isn't a script-visible variable).
    int32_t struct_ret_ptr_offset = 0;
    bool returns_struct_by_ptr() const {
        return f.ret && !f.ret->struct_name.empty() && ctx.structs && ctx.structs->count(f.ret->struct_name) != 0;
    }
    // Evaluate a struct-returning CallExpr, forwarding `dest` (already loaded
    // into rax by the caller - either a fresh local's address via LEA, for a
    // `let x: T = f(...);` init, or this function's own incoming hidden
    // pointer, for a `return f(...);` forward) as the hidden word-0 argument.
    // Mirrors the generic CallExpr word-placement logic in eval() exactly,
    // just with every real operand's word index shifted up by 1.
    void eval_struct_returning_call(const CallExpr& c) {
        struct Operand { const Expr* e; const Type* ty; int32_t slot0; int words; bool is_struct; };
        std::vector<Operand> ops;
        int32_t next_slot = 1; // word 0 reserved for the hidden dest pointer
        auto add_operand = [&](const Expr* oe) {
            const Type* t = oe->ty;
            int w = words_for_type(t, ctx.structs);
            bool is_struct = t && !t->struct_name.empty() && ctx.structs && ctx.structs->count(t->struct_name) != 0;
            ops.push_back({oe, t, next_slot, w, is_struct});
            next_slot += w;
        };
        if (c.receiver) add_operand(c.receiver.get());
        for (auto& a : c.args) add_operand(a.get());
        int n = next_slot;
        int32_t stash_size = n * 8;
        int32_t outgoing = std::max(0, n - 4) * 8;
        int32_t total = round16(stash_size + 32 + outgoing);
        e.byte(0x48); e.byte(0x81); e.byte(0xEC); e.imm32(total); // sub rsp,total
        e.store_reg_mem(Reg::rsp, 0, Reg::rax); // word 0: the hidden dest pointer (already in rax)
        for (auto& op : ops) {
            int32_t off = op.slot0 * 8;
            if (op.is_struct) {
                auto* id = dynamic_cast<const Ident*>(op.e);
                auto it = id ? locals.find(id->name) : locals.end();
                auto sit = (op.ty && !op.ty->struct_name.empty() && ctx.structs) ? ctx.structs->find(op.ty->struct_name) : ctx.structs->end();
                if (it != locals.end() && sit != ctx.structs->end()) {
                    copy_bytes(Reg::rsp, off, Reg::rbp, it->second, sit->second.size);
                }
            } else {
                eval(*op.e);
                if (op.words == 2) {
                    e.store_reg_mem(Reg::rsp, off, Reg::rax);
                    e.store_reg_mem(Reg::rsp, off + 8, Reg::rdx);
                } else if (op.ty && op.ty->is_float()) {
                    e.byte(0xF3); e.byte(0x0F); e.byte(0x11);
                    e.byte(0x84); e.byte(0x24); e.imm32(off);
                } else {
                    e.byte(0x48); e.byte(0x89); e.byte(0x84); e.byte(0x24); e.imm32(off);
                }
            }
        }
        std::vector<bool> word_is_float(size_t(n), false);
        for (auto& op : ops) {
            if (op.words == 1 && !op.is_struct && op.ty && op.ty->is_float()) word_is_float[size_t(op.slot0)] = true;
        }
        static const Reg int_regs[4] = {Reg::rcx, Reg::rdx, Reg::r8, Reg::r9};
        static const Xmm flt_regs[4] = {Xmm::xmm0, Xmm::xmm1, Xmm::xmm2, Xmm::xmm3};
        for (int w = 0; w < n && w < 4; ++w) {
            int32_t off = w * 8;
            if (word_is_float[size_t(w)]) e.movss_xmm_mem(flt_regs[w], Reg::rsp, off);
            else e.load_reg_mem(int_regs[w], Reg::rsp, off);
        }
        for (int w = 4; w < n; ++w) {
            int32_t src = w * 8;
            int32_t dst = stash_size + 32 + (w - 4) * 8;
            e.load_reg_mem(Reg::rax, Reg::rsp, src);
            e.store_reg_mem(Reg::rsp, dst, Reg::rax);
        }
        if (c.is_native) {
            e.mov_reg_imm64(Reg::rax, int64_t(c.native_fn));
            e.call_reg(Reg::rax);
        } else {
            // dispatch-table base is a relocatable imm64 (BUNDLING_AND_EM_MODULES
            // .md Section 2.4): emit 8 zero placeholders + a DispatchTableBase AbsFixup;
            // compile_func fills the placeholder with ctx.dispatch_base after
            // emit (byte-identical to the old raw mov_reg_imm64).
            emit_depth_check();                        // v0.4 stack-depth guard (SAFETY §4)
            e.mov_reg_imm64_external(Reg::r11, AbsFixup::DispatchTableBase);
            e.call_mem(Reg::r11, int32_t(c.script_slot) * 8);
            emit_depth_leave();
        }
        e.byte(0x48); e.byte(0x81); e.byte(0xC4); e.imm32(total); // add rsp,total
        // no result in rax/xmm0 to propagate - the callee wrote the struct
        // directly through the hidden pointer.
    }
    // Copy a registered struct's bytes from a local (by name) to the address
    // in `dest_reg` (matching how struct-by-value call arguments are copied
    // above - exact byte count, not a rounded-up word count).
    void copy_struct_to_ptr(const std::string& local_name, const StructLayout& layout, Reg dest_reg) {
        auto it = locals.find(local_name);
        if (it == locals.end()) return;
        copy_bytes(dest_reg, 0, Reg::rbp, it->second, layout.size);
    }

    // --- obfuscation helpers (CODEGEN_SPEC.md Section obf) ---
    ObfOptions obf;

    // CPUID-keyed entry gate:
    // push rbx; mov rax,1; cpuid; cmp eax,<key>; jne trap; pop rbx.
    // trap = ud2 (0F 0B). Runs after the prologue (rsp already frame-set).
    void emit_cpuid_gate() {
        if (!obf.keyed) return;
        e.push(Reg::rbx);                       // rbx clobbered by cpuid
        e.mov_reg_imm64(Reg::rax, 1);           // cpuid leaf 1
        // cpuid: 0F A2
        e.byte(0x0F); e.byte(0xA2);
        // cmp eax, imm32: F9 81 F8 <key32>  (no REX - 32-bit cmp)
        // Actually: cmp rax, imm32 sign-extends; use cmp eax, imm32 (no REX, 81 /7 id)
        e.byte(0x81); e.byte(0xF8); e.imm32(int32_t(obf.cpuid_key));
        Label trap = e.alloc_label(), ok = e.alloc_label();
        e.jcc(Cond::ne, trap);                  // mismatch -> trap
        e.pop(Reg::rbx);                        // restore rbx, continue
        e.jmp(ok);
        e.bind(trap);
        // v0.4: route through the trap stub (longjmp to checkpoint) instead of
        // ud2 -> recoverable, not process death (REDSHELL V7). On the trap path
        // rbx is still pushed, but the stub longjmps (never returns, rsp
        // restored by longjmp) so the unbalanced push is harmless; with a null
        // stub the ud2 kills the process regardless.
        emit_trap(int(TrapReason::IllegalInstruction), "@obf_keyed: CPUID gate mismatch");
        e.bind(ok);
    }

    // --- MBA (Mixed Boolean-Arithmetic) substitution helpers ---
    // Each replaces the obvious instruction with an algebraically-equivalent
    // sequence (no occurrence of the original opcode). Operands are held in
    // dst=lhs (rax) and src=rhs (rcx); rdx is a scratch that the BinExpr path
    // never keeps live across the eval, so it's free to clobber.
    //
    // Identities used (see CODEGEN_SPEC.md Section obf MBA pass):
    //   add: x + y  =  x - (-y)         -> mov rdx,y; neg rdx; sub x,rdx
    //   sub: x - y  =  x + (-y)         -> mov rdx,y; neg rdx; add x,rdx
    //   xor: x ^ y  =  (x|y) & ~(x&y)  -> or rax,rcx; andn-style via not+and
    //   and: x & y  =  ~(~x | ~y)     -> not x; not y; or x,y; not x
    //   or:  x | y  =  ~(~x & ~y)    -> not x; not y; and x,y; not x
    //
    // All five are bit-exact on i64 two's-complement (proven by obf_test).
    void emit_add_mba(Reg dst, Reg src) {
        e.mov_reg_reg(Reg::rdx, src);
        // neg rdx: REX.W F7 /3 (mod=11, reg=3, rm=rdx=2) -> 48 F7 DA
        e.byte(0x48); e.byte(0xF7); e.byte(0xDA);
        e.sub_reg_reg(dst, Reg::rdx);           // x - (-y) = x + y
    }
    void emit_sub_mba(Reg dst, Reg src) {
        e.mov_reg_reg(Reg::rdx, src);
        e.byte(0x48); e.byte(0xF7); e.byte(0xDA); // neg rdx
        e.add_reg_reg(dst, Reg::rdx);           // x + (-y) = x - y
    }
    // x ^ y = (x | y) & ~(x & y). Uses rdx to hold (x & y), then ~(rdx), then
    // (x | y) & ~rdx via andn (BMI1 0xF2 /r) if available; to stay baseline
    // x86-64 (no BMI1 requirement) we expand andn as: not rdx; and dst, rdx.
    // Sequence (no `xor` opcode):
    //   mov rdx, rax      ; rdx = x
    //   and rdx, rcx      ; rdx = x & y
    //   not rdx           ; rdx = ~(x & y)
    //   or  rax, rcx      ; rax = x | y
    //   and rax, rdx      ; rax = (x|y) & ~(x&y) = x ^ y
    void emit_xor_mba(Reg dst, Reg src) {
        e.mov_reg_reg(Reg::rdx, dst);
        // and rdx, src: REX.W 21 /r (mod=11, reg=src, rm=rdx)
        e.byte(rex_byte(Reg::rdx, src)); e.byte(0x21); e.byte(modrm3(src, Reg::rdx));
        // not rdx: 48 F7 D2 (rm=rdx=2)
        e.byte(0x48); e.byte(0xF7); e.byte(0xD2);
        // or dst, src
        emit_or_raw(dst, src);
        // and dst, rdx
        e.byte(rex_byte(dst, Reg::rdx)); e.byte(0x21); e.byte(modrm3(Reg::rdx, dst));
    }
    // x & y = ~(~x | ~y): not x; not y; or x,y; not x  (no `and` opcode)
    void emit_and_mba(Reg dst, Reg src) {
        emit_not_reg(dst);                       // ~x
        emit_not_reg(src);                        // ~y
        emit_or_raw(dst, src);                    // ~x | ~y
        emit_not_reg(dst);                        // ~(~x | ~y) = x & y
    }
    // x | y = ~(~x & ~y): not x; not y; and x,y; not x  (no `or` opcode)
    void emit_or_mba(Reg dst, Reg src) {
        emit_not_reg(dst);                        // ~x
        emit_not_reg(src);                        // ~y
        emit_and_raw(dst, src);                   // ~x & ~y
        emit_not_reg(dst);                        // ~(~x & ~y) = x | y
    }
    // --- low-level byte helpers for the MBA paths (no dependency on a
    // particular emitter method existing for and/or/not on every reg) ---
    // REX.W byte for a reg/reg instruction (W=1, R=reg-ext-of-reg2, B=reg-ext-of-rm).
    static uint8_t rex_byte(Reg reg, Reg rm) {
        return X64Emitter::rex(true,
            X64Emitter::is_extended(reg), false, X64Emitter::is_extended(rm));
    }
    static uint8_t modrm3(Reg reg, Reg rm) { return X64Emitter::modrm(0b11, reg, rm); }
    // and dst, src: REX.W 21 /r (mod=11, reg=src, rm=dst)
    void emit_and_raw(Reg dst, Reg src) {
        e.byte(rex_byte(src, dst)); e.byte(0x21); e.byte(modrm3(src, dst));
    }
    // or dst, src: REX.W 09 /r (mod=11, reg=src, rm=dst)
    void emit_or_raw(Reg dst, Reg src) {
        e.byte(rex_byte(src, dst)); e.byte(0x09); e.byte(modrm3(src, dst));
    }
    // not r: REX.W F7 /2 (mod=11, reg=2, rm=r)
    void emit_not_reg(Reg r) {
        e.byte(X64Emitter::rex(true, false, false, X64Emitter::is_extended(r)));
        e.byte(0xF7); e.byte(X64Emitter::modrm(0b11, Reg(2), r));
    }

    // Dispatch an MBA transform for an integer binary op. Returns true if
    // the op was handled by MBA (caller must NOT then emit the plain form).
    bool emit_mba_binop(BinExpr::Op op, Reg dst, Reg src) {
        switch (op) {
        case BinExpr::Op::Add: emit_add_mba(dst, src); return true;
        case BinExpr::Op::Sub: emit_sub_mba(dst, src); return true;
        case BinExpr::Op::Xor: emit_xor_mba(dst, src); return true;
        case BinExpr::Op::And: emit_and_mba(dst, src); return true;
        case BinExpr::Op::Or:  emit_or_mba(dst, src);  return true;
        default: return false;  // Mul/Div/Mod/Shift/Cmp: no MBA variant (yet)
        }
    }

    // Opaque predicate + junk. Four always-true patterns, picked round-robin
    // by the current code offset so a function with several opaque sites
    // doesn't emit identical bytes each time (static-analysis noise is the
    // point). Each branches over a small junk sled so the junk is statically
    // reachable but dynamically never executed.
    void emit_opaque_junk() {
        if (!obf.opaque) return;
        unsigned variant = (e.code.size() / 8) & 3;   // varies across sites
        Label over = e.alloc_label();
        switch (variant) {
        case 0: {
            // mov r10, 1; test r10, r10; jnz over  (r10 != 0 always)
            e.mov_reg_imm64(Reg::r10, 1);
            // test r10, r10: REX.WR 85 /r -> 4C 85 D2
            e.byte(0x4C); e.byte(0x85); e.byte(0xD2);
            e.jcc(Cond::ne, over);
            break;
        }
        case 1: {
            // mov r10, 7; imul r10, r10; cmp r10, 49; jne over  (7*7==49)
            e.mov_reg_imm64(Reg::r10, 7);
            e.imul_reg_reg(Reg::r10, Reg::r10);
            e.cmp_reg_imm32(Reg::r10, 49);
            e.jcc(Cond::ne, over);
            break;
        }
        case 2: {
            // mov r10, 0x5555555555555555; not r10; test r10,r10; jnz over
            // (~0x555..) is all the flipped bits -> nonzero, always taken.
            e.mov_reg_imm64(Reg::r10, int64_t(0x5555555555555555ULL));
            // not r10: REX.WR F7 /2 (rm=r10=10 -> needs REX.B) -> 49 F7 D2
            e.byte(0x49); e.byte(0xF7); e.byte(0xD2);
            // test r10, r10: 4C 85 D2
            e.byte(0x4C); e.byte(0x85); e.byte(0xD2);
            e.jcc(Cond::ne, over);
            break;
        }
        default: {
            // case 3: read the return address off the stack (a value that is
            // always nonzero on any real call) and compare to 0.
            // mov r10, [rsp]; cmp r10, 0; jne over
            // mov r10, [rsp]: REX.WR 8B /r mod=00 reg=r10 rm=rsp(4) -> SIB
            //   REX.WR (4C), 8B, modrm(00, r10, rsp=4)=0x14, SIB(0,none=4,rsp=4)=0x24
            e.byte(0x4C); e.byte(0x8B); e.byte(0x14); e.byte(0x24);
            e.cmp_reg_imm32(Reg::r10, 0);
            e.jcc(Cond::ne, over);
            break;
        }
        }
        // junk sled (statically reachable, dynamically never executed).
        // Mix nops + a dead xchg so the bytes aren't a clean nop pattern.
        e.nop(); e.nop();
        // xchg rax, rax (66 48 90 = the 3-byte canonical nop, harmless)
        e.byte(0x66); e.byte(0x48); e.byte(0x90);
        e.nop();
        // (unreachable at runtime, present for static-analysis noise)
        e.bind(over);
    }

    void exec_block(const Block& b);
    void exec_stmt(const Stmt& s);
};

// proper store rax -> [rbp + disp32]
void store_rax_to_rbp(CG& cg, int32_t off) {
    // REX.W 89 /r  mod=10 reg=rax(0) rm=rbp(5) -> 48 89 85 <disp32>
    cg.e.byte(0x48); cg.e.byte(0x89); cg.e.byte(0x85); cg.e.imm32(off);
}
// store xmm0 -> [rbp + disp32]
void store_xmm0_to_rbp(CG& cg, int32_t off) {
    // F3 0F 11 /r mod=10 reg=xmm0(0) rm=rbp(5) -> F3 0F 11 85 <disp32>
    cg.e.byte(0xF3); cg.e.byte(0x0F); cg.e.byte(0x11); cg.e.byte(0x85); cg.e.imm32(off);
}
// load [rbp + disp32] -> rax
void load_rbp_to_rax(CG& cg, int32_t off) {
    // REX.W 8B /r mod=10 reg=rax(0) rm=rbp(5) -> 48 8B 85 <disp32>
    cg.e.byte(0x48); cg.e.byte(0x8B); cg.e.byte(0x85); cg.e.imm32(off);
}
// load [imm64 + disp32] -> rax  (for globals: mov rax, [globals_base + off])
// The globals base is a relocatable imm64 (BUNDLING_AND_EM_MODULES.md Section 2.4):
// emit 8 zero placeholders + a GlobalsBase AbsFixup; compile_func fills the
// placeholder with ctx.globals_base after emit (byte-identical to the old raw
// mov_reg_imm64(Reg::r11, base), since ctx.globals_base == the base that was
// passed here = g_globals_for_codegen->base). `base` is kept in the signature
// for API stability but is now unused - the value comes from ctx at fill time.
void load_global_to_rax(CG& cg, int64_t /*base*/, int32_t off) {
    // mov r11, <globals_base reloc>; mov rax, [r11 + off]
    cg.e.mov_reg_imm64_external(Reg::r11, AbsFixup::GlobalsBase);
    cg.e.load_reg_mem(Reg::rax, Reg::r11, off);
}
void store_rax_to_global(CG& cg, int64_t /*base*/, int32_t off) {
    // mov r11, <globals_base reloc>; mov [r11 + off], rax
    cg.e.mov_reg_imm64_external(Reg::r11, AbsFixup::GlobalsBase);
    // 49 89 03 ... no, [r11+disp32]: REX.WB (49), 89, modrm(10, rax=0, r11=3) = 0x83, disp32
    cg.e.byte(0x49); cg.e.byte(0x89); cg.e.byte(0x83); cg.e.imm32(off);
}

void CG::eval(const Expr& ex) {
    if (auto* lit = dynamic_cast<const IntLit*>(&ex)) {
        e.mov_reg_imm64(Reg::rax, lit->v);
        return;
    }
    if (auto* lit = dynamic_cast<const FloatLit*>(&ex)) {
        // load the f32 bit pattern into eax, then movd xmm0, eax
        // movd xmm0, eax = 66 0F 6E C0
        float fv = (float)lit->v;
        uint32_t bits;
        std::memcpy(&bits, &fv, 4);
        e.mov_reg_imm64(Reg::rax, int64_t(int32_t(bits)));  // mov rax, imm (sign-extended)
        // movd xmm0, eax: 66 0F 6E C0
        e.byte(0x66); e.byte(0x0F); e.byte(0x6E); e.byte(0xC0);
        return;
    }
    if (auto* lit = dynamic_cast<const BoolLit*>(&ex)) {
        e.mov_reg_imm64(Reg::rax, lit->v ? 1 : 0);
        return;
    }
    if (auto* id = dynamic_cast<const Ident*>(&ex)) {
        // Item E ("hot local pinning") fast path: if this name is the
        // currently-active pin, read straight from its register instead of
        // reloading from [rbp+off] - strictly cheaper, and always safe
        // regardless of the pinned local's actual current value, since the
        // stack slot is kept write-through synced (Task 4) so the register
        // and memory never disagree. Only ever set for eligible scalars
        // (find_pin_candidate's filter), so no is_slice/is_float branching
        // is needed here - a plain reg-reg move into rax matches the
        // convention every other scalar read already uses below.
        if (active_pin && active_pin->name == id->name) {
            e.mov_reg_reg(Reg::rax, active_pin->reg);
            return;
        }
        // local?
        auto it = locals.find(id->name);
        if (it != locals.end()) {
            const Type* t = local_types[id->name];
            if (t->is_slice) {
                // slice ABI: rax=ptr ([off]), rdx=len ([off+8]) - the two-
                // register convention CallExpr/LetStmt/AssignExpr all share.
                e.load_reg_mem(Reg::rax, Reg::rbp, it->second);
                e.load_reg_mem(Reg::rdx, Reg::rbp, it->second + 8);
            } else if (t->is_float()) { e.movss_xmm_mem(Xmm::xmm0, Reg::rbp, it->second); }
            else load_rbp_to_rax(*this, it->second);
            return;
        }
        // global?
        if (g_globals_for_codegen) {
            auto gi = g_globals_for_codegen->index.find(id->name);
            if (gi != g_globals_for_codegen->index.end()) {
                int32_t off = int32_t(gi->second) * 8;
                const Type* t = g_globals_for_codegen->types[id->name];
                load_global_to_rax(*this, g_globals_for_codegen->base, off);
                if (t->is_float()) {
                    // movd xmm0, eax: 66 0F 6E C0
                    e.byte(0x66); e.byte(0x0F); e.byte(0x6E); e.byte(0xC0);
                }
                return;
            }
        }
        return;
    }
    if (auto* b = dynamic_cast<const BinExpr*>(&ex)) {
        // operator-overload dispatch: sema stamped this BinExpr as a native call
        // (e.g. vec3 + vec3 -> vec3_add). Emit a call with lhs as arg[0], rhs as arg[1].
        if (b->is_overload && b->overload_fn) {
            // sub rsp, 48 (2 args*8 + 32 shadow)
            e.byte(0x48); e.byte(0x81); e.byte(0xEC); e.imm32(48);
            // eval lhs -> rax; mov [rsp+0], rax
            eval(*b->lhs);
            e.byte(0x48); e.byte(0x89); e.byte(0x84); e.byte(0x24); e.imm32(0);
            // eval rhs -> rax; mov [rsp+8], rax
            eval(*b->rhs);
            e.byte(0x48); e.byte(0x89); e.byte(0x84); e.byte(0x24); e.imm32(8);
            // mov rcx, [rsp+0]; mov rdx, [rsp+8]
            e.byte(0x48); e.byte(0x8B); e.byte(0x0C); e.byte(0x24);
            e.byte(0x48); e.byte(0x8B); e.byte(0x54); e.byte(0x24); e.byte(0x08);
            // call overload_fn
            e.mov_reg_imm64(Reg::rax, int64_t(b->overload_fn));
            e.call_reg(Reg::rax);
            // add rsp, 48
            e.byte(0x48); e.byte(0x81); e.byte(0xC4); e.imm32(48);
            return;
        }
        const Type* lt = b->lhs->ty;
        // constant folding (7.1): both operands are literals - compute at
        // compile time instead of emitting a runtime sequence. Deliberately
        // narrow: Div/Mod on integer literals are excluded (a literal-zero
        // divisor would need to either fold to a bogus value or replicate
        // the runtime path's hardware trap - not worth the complexity for
        // an optimization that must be semantically invisible either way).
        // Comparisons/logical ops are excluded too (more surface area for a
        // stretch goal that only needs to prove the concept).
        if (auto* li = dynamic_cast<const IntLit*>(b->lhs.get())) {
            if (auto* ri = dynamic_cast<const IntLit*>(b->rhs.get())) {
                bool folded = true;
                int64_t result = 0;
                // Add/Sub/Mul go through uint64_t first: script-level integer
                // overflow is expected to wrap (edge_cases2.ember already
                // exercises this at the runtime-add level, where it's just a
                // hardware `add` - no UB), but `int64_t + int64_t` overflow
                // is undefined in the C++ that's doing the folding here.
                // Unsigned overflow is well-defined (wraps mod 2^64), and
                // produces the identical bit pattern - so casting through
                // uint64_t keeps the fold's result identical to the runtime
                // path's while removing the UB from the *host* computation.
                switch (b->op) {
                case BinExpr::Op::Add: result = int64_t(uint64_t(li->v) + uint64_t(ri->v)); break;
                case BinExpr::Op::Sub: result = int64_t(uint64_t(li->v) - uint64_t(ri->v)); break;
                case BinExpr::Op::Mul: result = int64_t(uint64_t(li->v) * uint64_t(ri->v)); break;
                case BinExpr::Op::And: result = li->v & ri->v; break;
                case BinExpr::Op::Or:  result = li->v | ri->v; break;
                case BinExpr::Op::Xor: result = li->v ^ ri->v; break;
                // x86 shl/sar mask the shift count to 0-63 for a 64-bit
                // operand; replicate that so folded and unfolded code agree.
                case BinExpr::Op::Shl: result = li->v << (ri->v & 63); break;
                case BinExpr::Op::Shr: result = li->v >> (ri->v & 63); break;
                default: folded = false; break;
                }
                if (folded) { e.mov_reg_imm64(Reg::rax, result); return; }
            }
        }
        if (auto* lf = dynamic_cast<const FloatLit*>(b->lhs.get())) {
            if (auto* rf = dynamic_cast<const FloatLit*>(b->rhs.get())) {
                bool folded = true;
                double result = 0;
                switch (b->op) {
                case BinExpr::Op::Add: result = lf->v + rf->v; break;
                case BinExpr::Op::Sub: result = lf->v - rf->v; break;
                case BinExpr::Op::Mul: result = lf->v * rf->v; break;
                // float div-by-zero is well-defined (IEEE754 inf/nan, no
                // trap) - safe to fold, unlike the integer case above.
                case BinExpr::Op::Div: result = lf->v / rf->v; break;
                default: folded = false; break;
                }
                if (folded) {
                    // mirrors the FloatLit codegen above exactly (always
                    // narrows to f32) so folded and unfolded results agree
                    // even for a nominally-f64 expression.
                    float fv = float(result);
                    uint32_t bits; std::memcpy(&bits, &fv, 4);
                    e.mov_reg_imm64(Reg::rax, int64_t(int32_t(bits)));
                    e.byte(0x66); e.byte(0x0F); e.byte(0x6E); e.byte(0xC0); // movd xmm0, eax
                    return;
                }
            }
        }
        bool is_logical = (b->op==BinExpr::Op::LAnd||b->op==BinExpr::Op::LOr);
        bool is_cmp = (b->op>=BinExpr::Op::Eq && b->op<=BinExpr::Op::Ge);
        bool is_float = lt && lt->is_float();

        if (is_logical) {
            // short-circuit: eval lhs; test; jz (for &&) -> false; eval rhs; test
            Label false_l = e.alloc_label(), end_l = e.alloc_label();
            eval(*b->lhs); e.cmp_reg_imm32(Reg::rax, 0);
            if (b->op == BinExpr::Op::LAnd) e.jcc(Cond::e, false_l);
            else e.jcc(Cond::ne, end_l); // LOr: if lhs true, result true
            eval(*b->rhs); e.cmp_reg_imm32(Reg::rax, 0);
            e.jcc(Cond::ne, end_l); // rhs true -> 1
            e.bind(false_l);
            e.mov_reg_imm64(Reg::rax, 0);
            e.bind(end_l);
            // normalize: rax = (rax != 0) ? 1 : 0
            e.cmp_reg_imm32(Reg::rax, 0);
            Label z = e.alloc_label(), done = e.alloc_label();
            e.mov_reg_imm64(Reg::rax, 0); e.jcc(Cond::e, done);
            e.mov_reg_imm64(Reg::rax, 1); e.bind(done);
            return;
        }

        if (is_float) {
            // float path (clean): eval lhs -> xmm0, stash to stack; eval rhs -> xmm0; lhs in xmm1
            eval(*b->lhs);
            // sub rsp, 8; movss [rsp], xmm0
            e.byte(0x48); e.byte(0x83); e.byte(0xEC); e.byte(0x08);
            e.byte(0xF3); e.byte(0x0F); e.byte(0x11); e.byte(0x04); e.byte(0x24);
            eval(*b->rhs);
            // xmm0 = rhs; load lhs into xmm1: movss xmm1, [rsp]; add rsp, 8
            e.byte(0xF3); e.byte(0x0F); e.byte(0x10); e.byte(0x0C); e.byte(0x24);
            e.byte(0x48); e.byte(0x83); e.byte(0xC4); e.byte(0x08);
            switch (b->op) {
            case BinExpr::Op::Add: e.addss_xmm(Xmm::xmm0, Xmm::xmm1); break;
            case BinExpr::Op::Mul: e.mulss_xmm(Xmm::xmm0, Xmm::xmm1); break;
            case BinExpr::Op::Sub: e.subss_xmm(Xmm::xmm1, Xmm::xmm0);
                e.movss_xmm_xmm(Xmm::xmm0, Xmm::xmm1); break;
            case BinExpr::Op::Div: e.divss_xmm(Xmm::xmm1, Xmm::xmm0);
                e.movss_xmm_xmm(Xmm::xmm0, Xmm::xmm1); break;
            case BinExpr::Op::Eq: case BinExpr::Op::Neq:
            case BinExpr::Op::Lt: case BinExpr::Op::Le:
            case BinExpr::Op::Gt: case BinExpr::Op::Ge: {
                e.ucomiss_xmm(Xmm::xmm1, Xmm::xmm0);
                uint8_t cc = 0;
                switch (b->op) {
                case BinExpr::Op::Eq: cc = 0x4; break;
                case BinExpr::Op::Neq: cc = 0x5; break;
                case BinExpr::Op::Lt: cc = 0x2; break;
                case BinExpr::Op::Le: cc = 0x6; break;
                case BinExpr::Op::Gt: cc = 0x7; break;
                case BinExpr::Op::Ge: cc = 0x3; break;
                default: break;
                }
                e.byte(0x0F); e.byte(0x90 | cc); e.byte(0xC0);
                e.byte(0x48); e.byte(0x0F); e.byte(0xB6); e.byte(0xC0);
                break;
            }
            default: break;
            }
            return;
        }
        // Integer path:
        if (!is_float) {
            eval(*b->lhs);
            e.push(Reg::rax);
            eval(*b->rhs);
            e.mov_reg_reg(Reg::rcx, Reg::rax); // rcx = rhs
            e.pop(Reg::rax);                    // rax = lhs
            switch (b->op) {
            case BinExpr::Op::Add:
            case BinExpr::Op::Sub:
            case BinExpr::Op::Xor:
            case BinExpr::Op::And:
            case BinExpr::Op::Or:
                if (obf.mba && emit_mba_binop(b->op, Reg::rax, Reg::rcx)) {
                    // MBA transform emitted; no plain form.
                } else {
                    switch (b->op) {
                    case BinExpr::Op::Add: e.add_reg_reg(Reg::rax, Reg::rcx); break;
                    case BinExpr::Op::Sub: e.sub_reg_reg(Reg::rax, Reg::rcx); break;
                    case BinExpr::Op::Xor: e.byte(0x48); e.byte(0x31); e.byte(0xC8); break; // xor rax,rcx
                    case BinExpr::Op::And: e.byte(0x48); e.byte(0x21); e.byte(0xC8); break; // and rax,rcx
                    case BinExpr::Op::Or:  e.byte(0x48); e.byte(0x09); e.byte(0xC8); break; // or rax,rcx
                    default: break;
                    }
                }
                break;
            case BinExpr::Op::Mul: e.imul_reg_reg(Reg::rax, Reg::rcx); break;
            case BinExpr::Op::Div:
                // rax / rcx: cqo; idiv rcx
                e.byte(0x48); e.byte(0x99); // cqo
                // idiv rcx: REX.W F7 /7 (mod=11, reg=7, rm=rcx=1) -> 48 F7 F9
                e.byte(0x48); e.byte(0xF7); e.byte(0xF9);
                break;
            case BinExpr::Op::Mod:
                e.byte(0x48); e.byte(0x99); // cqo
                e.byte(0x48); e.byte(0xF7); e.byte(0xF9); // idiv rcx -> rdx=remainder
                e.mov_reg_reg(Reg::rax, Reg::rdx);
                break;
            case BinExpr::Op::Shl: e.byte(0x48); e.byte(0xD3); e.byte(0xE0); break; // shl rax,cl
            case BinExpr::Op::Shr: e.byte(0x48); e.byte(0xD3); e.byte(0xF8); break; // sar rax,cl
            case BinExpr::Op::Eq: case BinExpr::Op::Neq:
            case BinExpr::Op::Lt: case BinExpr::Op::Le:
            case BinExpr::Op::Gt: case BinExpr::Op::Ge: {
                e.cmp_reg_reg(Reg::rax, Reg::rcx); // cmp lhs(rax), rhs(rcx)
                uint8_t cc = 0;
                switch (b->op) {
                case BinExpr::Op::Eq: cc = 0x4; break;  // sete
                case BinExpr::Op::Neq: cc = 0x5; break; // setne
                case BinExpr::Op::Lt: cc = 0xC; break;  // setl
                case BinExpr::Op::Le: cc = 0xE; break;  // setle
                case BinExpr::Op::Gt: cc = 0xF; break;  // setg
                case BinExpr::Op::Ge: cc = 0xD; break;  // setge
                default: break;
                }
                // setcc al: 0F 9x /0 (mod=11, reg=0, rm=al=0) -> 0F 9x C0
                e.byte(0x0F); e.byte(0x90 | cc); e.byte(0xC0);
                // movzx rax, al: 48 0F B6 C0
                e.byte(0x48); e.byte(0x0F); e.byte(0xB6); e.byte(0xC0);
                break;
            }
            default: break;
            }
            return;
        }
        // float path (clean):
        eval(*b->lhs);
        // sub rsp, 8; movss [rsp], xmm0
        e.byte(0x48); e.byte(0x83); e.byte(0xEC); e.byte(0x08);
        e.byte(0xF3); e.byte(0x0F); e.byte(0x11); e.byte(0x04); e.byte(0x24);
        eval(*b->rhs);
        // xmm0 = rhs; load lhs into xmm1: movss xmm1, [rsp]; add rsp, 8
        e.byte(0xF3); e.byte(0x0F); e.byte(0x10); e.byte(0x0C); e.byte(0x24);
        e.byte(0x48); e.byte(0x83); e.byte(0xC4); e.byte(0x08);
        switch (b->op) {
        case BinExpr::Op::Add: e.addss_xmm(Xmm::xmm0, Xmm::xmm1); break; // xmm0 = rhs+lhs
        case BinExpr::Op::Mul: e.mulss_xmm(Xmm::xmm0, Xmm::xmm1); break;
        case BinExpr::Op::Sub: e.subss_xmm(Xmm::xmm1, Xmm::xmm0); // xmm1 = lhs-rhs
            e.movss_xmm_xmm(Xmm::xmm0, Xmm::xmm1); break;
        case BinExpr::Op::Div: e.divss_xmm(Xmm::xmm1, Xmm::xmm0);
            e.movss_xmm_xmm(Xmm::xmm0, Xmm::xmm1); break;
        case BinExpr::Op::Eq: case BinExpr::Op::Neq:
        case BinExpr::Op::Lt: case BinExpr::Op::Le:
        case BinExpr::Op::Gt: case BinExpr::Op::Ge: {
            // ucomiss xmm1(lhs), xmm0(rhs); setcc
            e.ucomiss_xmm(Xmm::xmm1, Xmm::xmm0);
            uint8_t cc = 0;
            switch (b->op) {
            case BinExpr::Op::Eq: cc = 0x4; break;  // sete (ZF=1)
            case BinExpr::Op::Neq: cc = 0x5; break; // setne
            case BinExpr::Op::Lt: cc = 0x2; break;  // setb (CF=1, lhs<rhs)
            case BinExpr::Op::Le: cc = 0x6; break;  // setbe
            case BinExpr::Op::Gt: cc = 0x7; break;  // seta (CF=0 ZF=0)
            case BinExpr::Op::Ge: cc = 0x3; break;  // setae
            default: break;
            }
            e.byte(0x0F); e.byte(0x90 | cc); e.byte(0xC0);
            e.byte(0x48); e.byte(0x0F); e.byte(0xB6); e.byte(0xC0); // movzx rax,al
            break;
        }
        default: break;
        }
        return;
    }
    if (auto* u = dynamic_cast<const UnaryExpr*>(&ex)) {
        eval(*u->operand);
        if (u->op == UnaryExpr::Op::Not) {
            // rax = (rax == 0) ? 1 : 0
            e.cmp_reg_imm32(Reg::rax, 0);
            e.byte(0x0F); e.byte(0x94); e.byte(0xC0); // sete al
            e.byte(0x48); e.byte(0x0F); e.byte(0xB6); e.byte(0xC0); // movzx rax,al
        } else if (u->op == UnaryExpr::Op::Neg) {
            if (u->operand->ty && u->operand->ty->is_float()) {
                // negate f32 in xmm0: flip sign bit via xorps with 0x80000000
                // mov eax, 0x80000000 ; movd xmm1, eax ; xorps xmm0, xmm1
                e.byte(0xB8); e.imm32(int32_t(0x80000000));
                e.byte(0x66); e.byte(0x0F); e.byte(0x6E); e.byte(0xC8);  // movd xmm1, eax
                e.byte(0x0F); e.byte(0x57); e.byte(0xC1);                // xorps xmm0, xmm1
            } else {
                // neg rax: REX.W F7 /3 -> 48 F7 D8
                e.byte(0x48); e.byte(0xF7); e.byte(0xD8);
            }
        } else { // BitNot
            // not rax: REX.W F7 /2 -> 48 F7 D0
            e.byte(0x48); e.byte(0xF7); e.byte(0xD0);
        }
        return;
    }
    if (auto* c = dynamic_cast<const CastExpr*>(&ex)) {
        eval(*c->operand);
        // int<->int: no-op (same width in rax). f32<->f32: no-op.
        // int->float: cvtsi2ss. float->int: cvttss2si.
        const Type* from = c->operand->ty;
        const Type* to = c->to.get();
        if (from->is_int() && to->is_float()) {
            // cvtsi2ss xmm0, rax: F3 REX.W 0F 2A C0
            e.byte(0xF3); e.byte(0x48); e.byte(0x0F); e.byte(0x2A); e.byte(0xC0);
        } else if (from->is_float() && to->is_int()) {
            // cvttss2si rax, xmm0: F3 REX.W 0F 2C C0
            e.byte(0xF3); e.byte(0x48); e.byte(0x0F); e.byte(0x2C); e.byte(0xC0);
        }
        return;
    }
    if (auto* t = dynamic_cast<const TernaryExpr*>(&ex)) {
        eval(*t->cond);
        e.cmp_reg_imm32(Reg::rax, 0);
        Label else_l = e.alloc_label(), end_l = e.alloc_label();
        e.jcc(Cond::e, else_l);
        eval(*t->then_e);
        e.jmp(end_l);
        e.bind(else_l);
        eval(*t->else_e);
        e.bind(end_l);
        return;
    }
    if (auto* a = dynamic_cast<const AssignExpr*>(&ex)) {
        // simple (non-compound) assignment: eval value, store to target
        if (a->compound) {
            const Type* tt = a->target->ty;
            if (tt && tt->is_float()) {
                // float compound-assign: target op= value, via SSE (mirrors the
                // BinExpr float path). xmm0=target(lhs) stashed to [rsp],
                // then xmm0=value(rhs), xmm1=restored lhs.
                eval(*a->target);                                             // xmm0 = lhs
                e.byte(0x48); e.byte(0x83); e.byte(0xEC); e.byte(0x08);        // sub rsp,8
                e.byte(0xF3); e.byte(0x0F); e.byte(0x11); e.byte(0x04); e.byte(0x24); // movss [rsp], xmm0
                eval(*a->value);                                              // xmm0 = rhs
                e.byte(0xF3); e.byte(0x0F); e.byte(0x10); e.byte(0x0C); e.byte(0x24); // movss xmm1, [rsp]
                e.byte(0x48); e.byte(0x83); e.byte(0xC4); e.byte(0x08);        // add rsp,8
                switch (*a->compound) {
                case BinExpr::Op::Add: e.addss_xmm(Xmm::xmm0, Xmm::xmm1); break;
                case BinExpr::Op::Mul: e.mulss_xmm(Xmm::xmm0, Xmm::xmm1); break;
                case BinExpr::Op::Sub: e.subss_xmm(Xmm::xmm1, Xmm::xmm0);
                    e.movss_xmm_xmm(Xmm::xmm0, Xmm::xmm1); break;
                case BinExpr::Op::Div: e.divss_xmm(Xmm::xmm1, Xmm::xmm0);
                    e.movss_xmm_xmm(Xmm::xmm0, Xmm::xmm1); break;
                default: break; // Mod/Shl/Shr/And/Or/Xor: sema rejects these for float targets
                }
            } else {
                // load target, op with value, store back (integer path)
                eval(*a->target);              // rax = current target value
                e.push(Reg::rax);
                eval(*a->value);               // rax = rhs
                e.mov_reg_reg(Reg::rcx, Reg::rax);
                e.pop(Reg::rax);               // rax = lhs (current)
                switch (*a->compound) {
                case BinExpr::Op::Add:
                case BinExpr::Op::Sub:
                case BinExpr::Op::Xor:
                case BinExpr::Op::And:
                case BinExpr::Op::Or:
                    if (obf.mba && emit_mba_binop(*a->compound, Reg::rax, Reg::rcx)) {
                        // MBA transform emitted; no plain form.
                    } else {
                        switch (*a->compound) {
                        case BinExpr::Op::Add: e.add_reg_reg(Reg::rax, Reg::rcx); break;
                        case BinExpr::Op::Sub: e.sub_reg_reg(Reg::rax, Reg::rcx); break;
                        case BinExpr::Op::Xor: e.byte(0x48); e.byte(0x31); e.byte(0xC8); break;
                        case BinExpr::Op::And: e.byte(0x48); e.byte(0x21); e.byte(0xC8); break;
                        case BinExpr::Op::Or:  e.byte(0x48); e.byte(0x09); e.byte(0xC8); break;
                        default: break;
                        }
                    }
                    break;
                case BinExpr::Op::Mul: e.imul_reg_reg(Reg::rax, Reg::rcx); break;
                case BinExpr::Op::Div:
                    e.byte(0x48); e.byte(0x99);              // cqo
                    e.byte(0x48); e.byte(0xF7); e.byte(0xF9); // idiv rcx -> rax=quotient
                    break;
                case BinExpr::Op::Mod:
                    e.byte(0x48); e.byte(0x99);              // cqo
                    e.byte(0x48); e.byte(0xF7); e.byte(0xF9); // idiv rcx -> rdx=remainder
                    e.mov_reg_reg(Reg::rax, Reg::rdx);
                    break;
                case BinExpr::Op::Shl: e.byte(0x48); e.byte(0xD3); e.byte(0xE0); break; // shl rax,cl
                case BinExpr::Op::Shr: e.byte(0x48); e.byte(0xD3); e.byte(0xF8); break; // sar rax,cl
                default: break;
                }
            }
        } else {
            eval(*a->value);
        }
        // store rax (or xmm0, or rax/rdx for a slice) to target
        if (auto* id = dynamic_cast<Ident*>(a->target.get())) {
            auto it = locals.find(id->name);
            if (it != locals.end()) {
                const Type* t = local_types[id->name];
                if (t->is_slice) {
                    e.store_reg_mem(Reg::rbp, it->second, Reg::rax);
                    e.store_reg_mem(Reg::rbp, it->second + 8, Reg::rdx);
                } else if (t->is_float()) store_xmm0_to_rbp(*this, it->second);
                else {
                    // Item E ("hot local pinning") write-through: keep the
                    // pinned register in sync too, IN ADDITION to the
                    // always-synced stack slot below (Task 4) - never
                    // instead of it, so every other codegen path that reads
                    // this local by its stack offset (defers, a later
                    // outer-scope reference once this loop's pin has been
                    // cleared, etc.) still sees the correct, current value
                    // with zero changes needed on the read side. Only ever
                    // set for eligible scalars (find_pin_candidate's
                    // filter), so this can't fire for the is_slice/is_float
                    // branches above.
                    if (active_pin && active_pin->name == id->name)
                        e.mov_reg_reg(active_pin->reg, Reg::rax);
                    store_rax_to_rbp(*this, it->second);
                }
            } else {
                if (g_globals_for_codegen) {
                    auto gi = g_globals_for_codegen->index.find(id->name);
                    if (gi != g_globals_for_codegen->index.end()) {
                        store_rax_to_global(*this, g_globals_for_codegen->base, int32_t(gi->second)*8);
                    }
                }
            }
        } else if (auto* ixt = dynamic_cast<IndexExpr*>(a->target.get())) {
            // buf[i] = value: value was already eval'd into rax/xmm0 above;
            // stash it, compute the element address, then store. Mirrors the
            // IndexExpr *load* path in eval() (element addressing).
            const Type* bt = ixt->base->ty;
            const Type* elem = bt && bt->elem ? bt->elem.get() : nullptr;
            int32_t width = elem_bytes(elem);
            bool is_f32 = elem && elem->prim == Prim::F32 && !elem->is_slice;
            if (is_f32) { e.byte(0x48); e.byte(0x83); e.byte(0xEC); e.byte(0x08); // sub rsp,8
                e.byte(0xF3); e.byte(0x0F); e.byte(0x11); e.byte(0x04); e.byte(0x24); } // movss [rsp],xmm0
            else e.push(Reg::rax);
            if (auto* bid = dynamic_cast<const Ident*>(ixt->base.get())) {
                auto bit = locals.find(bid->name);
                if (bit != locals.end()) {
                    const Type* lt = local_types[bid->name];
                    Reg base_reg = Reg::rbp; int32_t base_off = 0; bool ready = false;
                    bool is_slice_base = false;
                    if (lt->is_slice) {
                        eval(*ixt->base);                      // rax=ptr, rdx=len
                        e.mov_reg_reg(Reg::r10, Reg::rax);
                        e.mov_reg_reg(Reg::r9, Reg::rdx);      // r9 = len (survives index eval)
                        base_reg = Reg::r10; base_off = 0; ready = true; is_slice_base = true;
                    } else if (lt->array_len > 0) {
                        base_reg = Reg::rbp; base_off = bit->second; ready = true;
                    }
                    if (ready) {
                        eval(*ixt->index);                     // rax = index
                        // Bounds check (Part 1) - a bad WRITE is exactly as
                        // dangerous as a bad read, so this mirrors the
                        // IndexExpr load path above exactly: a slice always
                        // gets a genuine runtime check (its length is
                        // runtime-only even for a constant index); a fixed
                        // array with a compile-time-constant index was
                        // already range-checked by sema (out-of-range is a
                        // compile error there, never reaching this code) and
                        // needs no check at all; everything else (a
                        // non-constant index against a fixed array) gets a
                        // genuine check. The value-to-store stashed on the
                        // stack just above (push rax / [rsp]=xmm0) survives
                        // the trap call's own balanced sub/add rsp untouched.
                        if (is_slice_base) {
                            e.mov_reg_reg(Reg::rcx, Reg::rax);
                            emit_bounds_check_reg(Reg::rcx, Reg::r9);
                        } else if (!ixt->index_is_const) {
                            e.mov_reg_reg(Reg::rcx, Reg::rax);
                            emit_bounds_check_imm(Reg::rcx, int64_t(bt->array_len));
                        }
                        if (width > 1) {
                            e.mov_reg_imm64(Reg::rcx, int64_t(width));
                            e.imul_reg_reg(Reg::rax, Reg::rcx);
                        }
                        e.mov_reg_reg(Reg::r11, base_reg);
                        e.add_reg_reg(Reg::r11, Reg::rax);     // r11 = element address
                        if (is_f32) {
                            e.byte(0xF3); e.byte(0x0F); e.byte(0x10); e.byte(0x0C); e.byte(0x24); // movss xmm1,[rsp]
                            e.byte(0x48); e.byte(0x83); e.byte(0xC4); e.byte(0x08); // add rsp,8
                            e.movss_mem_xmm(Reg::r11, base_off, Xmm::xmm1);
                        } else {
                            e.pop(Reg::rax);
                            e.store_rax_elem(Reg::r11, base_off, width);
                        }
                        return;
                    }
                }
            }
            // base wasn't a recognized local: drop the stashed value cleanly
            if (is_f32) { e.byte(0x48); e.byte(0x83); e.byte(0xC4); e.byte(0x08); }
            else e.pop(Reg::rax);
        } else if (auto* flt = dynamic_cast<FieldExpr*>(a->target.get())) {
            // p.x = value: value already eval'd into rax/xmm0 above. The
            // field's address is fixed at compile time (local's frame offset
            // + field's struct offset), so no index computation is needed.
            if (auto* bid = dynamic_cast<const Ident*>(flt->base.get())) {
                auto bit = locals.find(bid->name);
                if (bit != locals.end() && ctx.structs) {
                    const Type* bt = local_types[bid->name];
                    auto sit = bt && !bt->struct_name.empty() ? ctx.structs->find(bt->struct_name) : ctx.structs->end();
                    if (sit != ctx.structs->end()) {
                        auto fit = sit->second.fields.find(flt->field);
                        if (fit != sit->second.fields.end()) {
                            int32_t addr_off = bit->second + fit->second.offset;
                            const Type* ft = fit->second.ty;
                            if (ft->prim == Prim::F32 && !ft->is_slice) e.movss_mem_xmm(Reg::rbp, addr_off, Xmm::xmm0);
                            else e.store_rax_elem(Reg::rbp, addr_off, elem_bytes(ft));
                        }
                    }
                }
            }
        }
        return;
    }
    if (auto* c = dynamic_cast<const CallExpr*>(&ex)) {
        // Compile-time-folded assert_eq_* (sema.cpp's CallExpr check): both
        // arguments were compile-time constants that compared equal, so
        // sema already proved this call can never trap. Emit NOTHING at all
        // - zero runtime cost, matching the efficiency ask - not even the
        // argument-evaluation code, since every arg here is by construction
        // a compile-time constant with no side effects to preserve. (A
        // mismatched constant pair never reaches codegen in the first place:
        // sema turns that into a compile error instead (see CallExpr::elided
        // in ast.hpp and the assert_eq_* folding in sema.cpp's CallExpr case).
        if (c->elided) return;
        // Stash args on the STACK (rsp-relative, sub rsp per call) so nested calls
        // (e.g. ackermann(m-1, ackermann(m, n-1))) get a fresh region each - a
        // fixed rbp-relative arg_temps region would let an inner call clobber the
        // outer's stashed args. Layout: [rsp+0..n*8] = arg stash,
        // [rsp+n*8..+32] = shadow space, [rsp+n*8+32+(i-4)*8] = outgoing args 5+.
        // Method-call sugar: receiver is arg[0] (stashed first), c->args follow.
        //
        // Each logical operand occupies 1 "word" (8 bytes) EXCEPT a slice-typed
        // one (2 words: ptr, len - matching how a native declares a slice<u8>
        // ember param as two separate C++ params, e.g. print_str(uint8_t* p,
        // int64_t len)) or a registered-struct-typed one (ceil(size/8) words,
        // copied directly from its source local - see words_for_type). Word
        // index determines the physical register (int_regs[w] or flt_regs[w]
        // depending on that word's own type), exactly like the real Win64 ABI
        // resolves param slots.
        struct Operand { const Expr* e; const Type* ty; int32_t slot0; int words; bool is_struct; };
        std::vector<Operand> ops;
        int32_t next_slot = 0;
        auto add_operand = [&](const Expr* oe) {
            const Type* t = oe->ty;
            int w = words_for_type(t, ctx.structs);
            bool is_struct = t && !t->struct_name.empty() && ctx.structs && ctx.structs->count(t->struct_name) != 0;
            ops.push_back({oe, t, next_slot, w, is_struct});
            next_slot += w;
        };
        if (c->receiver) add_operand(c->receiver.get());
        for (auto& a : c->args) add_operand(a.get());
        int n = next_slot; // total word count
        int32_t stash_size = n * 8;
        int32_t outgoing = std::max(0, n - 4) * 8;
        int32_t total = round16(stash_size + 32 + outgoing);
        // sub rsp, total
        e.byte(0x48); e.byte(0x81); e.byte(0xEC); e.imm32(total);
        // eval + stash each operand to its word slot(s)
        for (auto& op : ops) {
            int32_t off = op.slot0 * 8;
            if (op.is_struct) {
                // struct-by-value: sema (check_struct_arg_shape) guarantees
                // op.e is a bare local Ident - copy its EXACT byte extent
                // (not a rounded-up word count - see copy_bytes) from the
                // source local's frame address into the stash region,
                // rather than eval()'ing a single register value (a
                // multi-word struct has no such single-register form).
                auto* id = dynamic_cast<const Ident*>(op.e);
                auto it = id ? locals.find(id->name) : locals.end();
                auto sit = (op.ty && !op.ty->struct_name.empty() && ctx.structs) ? ctx.structs->find(op.ty->struct_name) : ctx.structs->end();
                if (it != locals.end() && sit != ctx.structs->end()) {
                    copy_bytes(Reg::rsp, off, Reg::rbp, it->second, sit->second.size);
                }
            } else {
                eval(*op.e);
                if (op.words == 2) {
                    // slice: rax=ptr, rdx=len (see eval()'s Ident/StringLit cases)
                    e.store_reg_mem(Reg::rsp, off, Reg::rax);
                    e.store_reg_mem(Reg::rsp, off + 8, Reg::rdx);
                } else if (op.ty && op.ty->is_float()) {
                    e.byte(0xF3); e.byte(0x0F); e.byte(0x11);
                    e.byte(0x84); e.byte(0x24); e.imm32(off);
                } else {
                    e.byte(0x48); e.byte(0x89); e.byte(0x84); e.byte(0x24); e.imm32(off);
                }
            }
        }
        // per-word float flag (a slice's/struct's words are always integer)
        std::vector<bool> word_is_float(size_t(n), false);
        for (auto& op : ops) {
            if (op.words == 1 && !op.is_struct && op.ty && op.ty->is_float()) word_is_float[size_t(op.slot0)] = true;
        }
        // place words 0..3 into regs
        static const Reg int_regs[4] = {Reg::rcx, Reg::rdx, Reg::r8, Reg::r9};
        static const Xmm flt_regs[4] = {Xmm::xmm0, Xmm::xmm1, Xmm::xmm2, Xmm::xmm3};
        for (int w = 0; w < n && w < 4; ++w) {
            int32_t off = w * 8;
            if (word_is_float[size_t(w)]) {
                e.movss_xmm_mem(flt_regs[w], Reg::rsp, off);
            } else {
                e.load_reg_mem(int_regs[w], Reg::rsp, off);
            }
        }
        // words 4+ -> [rsp + stash_size + 32 + (w-4)*8] (caller's outgoing stack args)
        for (int w = 4; w < n; ++w) {
            int32_t src = w * 8;
            int32_t dst = stash_size + 32 + (w - 4) * 8;
            e.load_reg_mem(Reg::rax, Reg::rsp, src);
            e.store_reg_mem(Reg::rsp, dst, Reg::rax);
        }
        if (c->is_native) {
            // mov rax, imm64(native_fn); call rax
            e.mov_reg_imm64(Reg::rax, int64_t(c->native_fn));
            e.call_reg(Reg::rax);
        } else {
            // call [dispatch_base + slot*8] - dispatch-table base is a
            // relocatable imm64 (BUNDLING_AND_EM_MODULES.md Section 2.4): emit 8 zero
            // placeholders + a DispatchTableBase AbsFixup; compile_func fills
            // the placeholder with ctx.dispatch_base after emit (byte-identical
            // to the old raw mov_reg_imm64).
            emit_depth_check();                        // v0.4 stack-depth guard (SAFETY §4)
            e.mov_reg_imm64_external(Reg::r11, AbsFixup::DispatchTableBase);
            e.call_mem(Reg::r11, int32_t(c->script_slot) * 8);
            emit_depth_leave();
        }
        // add rsp, total
        e.byte(0x48); e.byte(0x81); e.byte(0xC4); e.imm32(total);
        // result in rax (int) or xmm0 (float) - already there per convention
        return;
    }
    if (auto* lit = dynamic_cast<const StringLit*>(&ex)) {
        // slice ABI: rax=ptr, rdx=len.
        // String encryption (default): if the string is XOR-encrypted in
        // rodata, emit a __str_decrypt(enc_ptr, len, key) call instead of
        // a raw pointer. The host native allocates a decrypted buffer on
        // the heap (not in JIT exec memory) and returns its address; rax=
        // decrypted ptr, rdx=len (stays the same). Raw strings never appear
        // in the executable memory.
        if (lit->encrypted && lit->baked_key != 0) {
            // call __str_decrypt(enc_ptr, len, key) -> rax = decrypted_ptr
            // Win64: rcx=enc_ptr, rdx=len, r8=key; shadow space reserved.
            int32_t call_sz = 32; // shadow space for 3-arg call
            e.byte(0x48); e.byte(0x81); e.byte(0xEC); e.imm32(call_sz);
            // mov rcx, enc_ptr
            e.mov_reg_imm64(Reg::rcx, int64_t(lit->baked_ptr));
            // mov rdx, len
            e.mov_reg_imm64(Reg::rdx, lit->baked_len);
            // mov r8, key
            e.mov_reg_imm64(Reg::r8, int64_t(lit->baked_key));
            // call __str_decrypt (host-provided native)
            e.mov_reg_imm64(Reg::rax, int64_t(ctx.str_decrypt_fn));
            e.call_reg(Reg::rax);
            // add rsp, shadow
            e.byte(0x48); e.byte(0x81); e.byte(0xC4); e.imm32(call_sz);
            // rax = decrypted ptr (from native), rdx = len (preserved by callee? no -
            // Win64 callee clobbers rdx. Re-set rdx = len after the call.)
            e.mov_reg_imm64(Reg::rdx, lit->baked_len);
        } else {
            // unencrypted: raw pointer (backward compat / key=0)
            e.mov_reg_imm64(Reg::rax, int64_t(lit->baked_ptr));
            e.mov_reg_imm64(Reg::rdx, lit->baked_len);
        }
        // Implicit conversion to a `string` handle (see sema.cpp's StringLit
        // check_expr case, which only sets implicit_to_string when a `string`
        // was specifically expected here - never for slice<u8> or untyped
        // contexts, so this never fires for an unrelated existing call like
        // print_str("literal")). rax=ptr, rdx=len from whichever branch just
        // ran above; call string_from_slice(ptr,len) and let ITS result (an
        // i64 handle, already the Win64 return register) stand in for this
        // expression's value instead of the raw slice.
        if (lit->implicit_to_string && lit->to_string_native_fn) {
            // mov rcx,rax FIRST, before sub rsp touches anything else - keeps
            // this obviously correct rather than "correct because nothing
            // happens to intervene" (string encryption is on by default for
            // every compile, so the encrypted branch above - itself a native
            // call that clobbers rdx and re-derives it - is the common case
            // this runs after, not a rare corner case).
            e.mov_reg_reg(Reg::rcx, Reg::rax);
            int32_t call_sz = 32; // shadow space for a 2-arg call
            e.byte(0x48); e.byte(0x81); e.byte(0xEC); e.imm32(call_sz);
            e.mov_reg_imm64(Reg::rax, int64_t(lit->to_string_native_fn));
            e.call_reg(Reg::rax);
            e.byte(0x48); e.byte(0x81); e.byte(0xC4); e.imm32(call_sz);
            // rax = string handle (i64) - already the correct convention
        }
        return;
    }
    if (auto* ix = dynamic_cast<const IndexExpr*>(&ex)) {
        // base[index]: base is either a fixed array T[N] (address = its own
        // frame slot) or a slice T[] (address = the ptr it evaluates to).
        // v1 scope: base must be a bare local Ident (matches current usage -
        // FieldExpr/nested-index bases are a follow-on extension).
        const Type* bt = ix->base->ty;
        const Type* elem = bt && bt->elem ? bt->elem.get() : nullptr;
        int32_t width = elem_bytes(elem);
        Reg base_reg = Reg::rbp;
        int32_t base_off = 0;
        bool base_ready = false;
        bool is_slice_base = false;
        if (auto* bid = dynamic_cast<const Ident*>(ix->base.get())) {
            auto it = locals.find(bid->name);
            if (it != locals.end()) {
                const Type* lt = local_types[bid->name];
                if (lt->is_slice) {
                    eval(*ix->base);           // rax=ptr, rdx=len
                    e.mov_reg_reg(Reg::r10, Reg::rax); // r10 = base ptr (survives index eval)
                    e.mov_reg_reg(Reg::r9, Reg::rdx);  // r9 = len (survives index eval, same convention as r10 above)
                    base_reg = Reg::r10; base_off = 0; base_ready = true; is_slice_base = true;
                } else if (lt->array_len > 0) {
                    base_reg = Reg::rbp; base_off = it->second; base_ready = true;
                }
            }
        }
        if (base_ready) {
            eval(*ix->index);                  // rax = index
            // Bounds check (Part 1): a slice's length is runtime-only even
            // for a constant index (its {ptr,len} is a two-word runtime
            // value - see the slice-ABI comments throughout this file), so
            // it always gets the real runtime check. A fixed array with a
            // compile-time-constant index was already range-checked by sema
            // (out-of-range there is a compile error, never reaching this
            // code at all) - that ONE combination is the sole case that
            // skips the check entirely, matching the efficiency ask
            // ("provably-safe accesses cost nothing"). Every other
            // combination (non-constant index against a fixed array; ANY
            // index, constant or not, against a slice) gets a genuine check.
            if (is_slice_base) {
                e.mov_reg_reg(Reg::rcx, Reg::rax);       // stash index (rcx is free here)
                emit_bounds_check_reg(Reg::rcx, Reg::r9); // idx (unsigned) < len
            } else if (!ix->index_is_const) {
                e.mov_reg_reg(Reg::rcx, Reg::rax);
                emit_bounds_check_imm(Reg::rcx, int64_t(bt->array_len));
            }
            if (width > 1) {
                e.mov_reg_imm64(Reg::rcx, int64_t(width));
                e.imul_reg_reg(Reg::rax, Reg::rcx);   // rax = index * width
            }
            e.mov_reg_reg(Reg::r11, base_reg);
            e.add_reg_reg(Reg::r11, Reg::rax);        // r11 = element address
            bool is_f32 = elem && elem->prim == Prim::F32 && !elem->is_slice;
            bool is_signed = elem && (elem->prim==Prim::I8||elem->prim==Prim::I16||
                                       elem->prim==Prim::I32||elem->prim==Prim::I64);
            if (is_f32) e.movss_xmm_mem(Xmm::xmm0, Reg::r11, base_off);
            else e.load_elem_to_rax(Reg::r11, base_off, width, is_signed);
        }
        return;
    }
    if (auto* v = dynamic_cast<const ViewExpr*>(&ex)) {
        // arr[..]: fixed array T[N] -> slice T[] {ptr=&arr, len=N}. v1 scope:
        // base must be a bare local Ident (a fixed-array frame slot has a
        // real address to take; a general expression would need a temp).
        if (auto* bid = dynamic_cast<const Ident*>(v->base.get())) {
            auto it = locals.find(bid->name);
            if (it != locals.end() && local_types[bid->name]->array_len > 0) {
                const Type* lt = local_types[bid->name];
                // rax = &arr (lea rax, [rbp+off]); rdx = N
                e.byte(0x48); e.byte(0x8D); e.byte(0x85); e.imm32(it->second); // lea rax, [rbp+off]
                e.mov_reg_imm64(Reg::rdx, int64_t(lt->array_len));
            }
        }
        return;
    }
    if (auto* fl = dynamic_cast<const FieldExpr*>(&ex)) {
        // struct field read (p.x). v1 scope: base must be a bare local Ident
        // of a registered struct type (matches the IndexExpr/ViewExpr
        // decision above - no nested-expression bases yet).
        if (auto* bid = dynamic_cast<const Ident*>(fl->base.get())) {
            auto it = locals.find(bid->name);
            if (it != locals.end() && ctx.structs) {
                const Type* bt = local_types[bid->name];
                auto sit = bt && !bt->struct_name.empty() ? ctx.structs->find(bt->struct_name) : ctx.structs->end();
                if (sit != ctx.structs->end()) {
                    auto fit = sit->second.fields.find(fl->field);
                    if (fit != sit->second.fields.end()) {
                        int32_t addr_off = it->second + fit->second.offset;
                        const Type* ft = fit->second.ty;
                        if (ft->prim == Prim::F32 && !ft->is_slice) {
                            e.movss_xmm_mem(Xmm::xmm0, Reg::rbp, addr_off);
                        } else {
                            bool is_signed = ft->prim==Prim::I8||ft->prim==Prim::I16||
                                             ft->prim==Prim::I32||ft->prim==Prim::I64;
                            e.load_elem_to_rax(Reg::rbp, addr_off, elem_bytes(ft), is_signed);
                        }
                    }
                }
            }
        }
        return;
    }
    // StructLit: only handled at its LetStmt init site (exec_stmt) - a
    // struct literal is a multi-word aggregate, it doesn't fit eval()'s
    // single-register-result convention. SizeofExpr: not yet supported.
}

void CG::exec_block(const Block& b) {
    // snapshot locals/local_types so inner-scope `let` shadowing restores on exit
    // (a flat map would let an inner `let x` clobber the outer x permanently).
    auto saved_locals = locals;
    auto saved_types = local_types;
    for (auto& s : b.stmts) exec_stmt(*s);
    locals = std::move(saved_locals);
    local_types = std::move(saved_types);
}

void CG::exec_stmt(const Stmt& s) {
    if (auto* ls = dynamic_cast<const LetStmt*>(&s)) {
        if (!ls->init) {
            // no initializer (parser only allows this for explicitly-typed
            // let/const): alloc the frame slot(s) and zero-fill them - e.g. a
            // fixed array later populated via indexed assignment.
            const Type* t = ls->ty.get();
            int32_t off = alloc_local(ls->name, t);
            int32_t remaining = local_width_bytes(t, ctx.structs);
            int32_t cur = off;
            e.mov_reg_imm64(Reg::rax, 0);
            while (remaining > 0) {
                int32_t chunk = remaining >= 8 ? 8 : remaining >= 4 ? 4 : remaining >= 2 ? 2 : 1;
                e.store_rax_elem(Reg::rbp, cur, chunk);
                cur += chunk; remaining -= chunk;
            }
            return;
        }
        if (auto* slit = dynamic_cast<const StructLit*>(ls->init.get())) {
            // struct-literal init: a struct is a multi-word aggregate, not a
            // single eval() result - allocate its whole frame region, then
            // evaluate and store each field expression directly at its own
            // offset. sema guarantees every field is present (build_struct_
            // layouts + the "missing field" check in check_expr(StructLit)).
            const Type* st = ls->init->ty;
            int32_t base_off = alloc_local(ls->name, st);
            const StructLayout* layout = (ctx.structs && st && !st->struct_name.empty())
                ? (ctx.structs->count(st->struct_name) ? &ctx.structs->at(st->struct_name) : nullptr) : nullptr;
            if (layout) {
                for (auto& kv : slit->fields) {
                    auto fit = layout->fields.find(kv.first);
                    if (fit == layout->fields.end()) continue;
                    eval(*kv.second);
                    int32_t addr_off = base_off + fit->second.offset;
                    const Type* ft = fit->second.ty;
                    if (ft->prim == Prim::F32 && !ft->is_slice) e.movss_mem_xmm(Reg::rbp, addr_off, Xmm::xmm0);
                    else e.store_rax_elem(Reg::rbp, addr_off, elem_bytes(ft));
                }
            }
            return;
        }
        if (auto* call = dynamic_cast<const CallExpr*>(ls->init.get())) {
            const Type* ct = call->ty;
            if (ct && !ct->struct_name.empty() && ctx.structs && ctx.structs->count(ct->struct_name)) {
                // struct-returning call: allocate the destination local first,
                // then LEA its address into rax as the hidden word-0 argument
                // (see eval_struct_returning_call) - the callee writes the
                // struct directly through that pointer, no register result.
                int32_t off = alloc_local(ls->name, ct);
                e.byte(0x48); e.byte(0x8D); e.byte(0x85); e.imm32(off); // lea rax, [rbp+off]
                eval_struct_returning_call(*call);
                return;
            }
        }
        int32_t off = alloc_local(ls->name, ls->init->ty);
        eval(*ls->init);
        const Type* t = ls->init->ty;
        if (t->is_slice) {
            // slice ABI: rax=ptr -> [off], rdx=len -> [off+8]
            e.store_reg_mem(Reg::rbp, off, Reg::rax);
            e.store_reg_mem(Reg::rbp, off + 8, Reg::rdx);
        } else if (t->is_float()) store_xmm0_to_rbp(*this, off);
        else store_rax_to_rbp(*this, off);
        return;
    }
    if (auto* es = dynamic_cast<const ExprStmt*>(&s)) { eval(*es->expr); return; }
    if (auto* ds = dynamic_cast<const DeferStmt*>(&s)) {
        // mark this defer site as "reached" (runtime flag; see emit_defers).
        // Found by pointer identity - defer_sites was populated from the same
        // AST by compile_func's collect_defers pass before codegen began.
        for (size_t i = 0; i < defer_sites.size(); ++i) {
            if (defer_sites[i] == ds) {
                e.mov_reg_imm64(Reg::rax, 1);
                e.store_reg_mem(Reg::rbp, defer_flag_offsets[i], Reg::rax);
                break;
            }
        }
        return;
    }
    if (auto* rs = dynamic_cast<const ReturnStmt*>(&s)) {
        bool has_defers = !defer_sites.empty();
        if (returns_struct_by_ptr()) {
            // struct-by-value return: no register-based result to stash
            // across defers (the callee writes through the hidden pointer
            // directly, at whatever point in this sequence that happens) -
            // sema guarantees rs->value is a bare Ident or a same-return-
            // type forwarding call (see check_stmt's ReturnStmt handling).
            if (rs->value) {
                if (auto* id = dynamic_cast<const Ident*>(rs->value.get())) {
                    auto sit = ctx.structs->find(f.ret->struct_name);
                    if (sit != ctx.structs->end()) {
                        e.load_reg_mem(Reg::r11, Reg::rbp, struct_ret_ptr_offset);
                        copy_struct_to_ptr(id->name, sit->second, Reg::r11);
                    }
                } else if (auto* call = dynamic_cast<const CallExpr*>(rs->value.get())) {
                    e.load_reg_mem(Reg::rax, Reg::rbp, struct_ret_ptr_offset);
                    eval_struct_returning_call(*call);
                }
            }
            if (has_defers) emit_defers();
            emit_epilogue();
            return;
        }
        bool is_float_ret = f.ret && f.ret->is_float();
        bool is_slice_ret = f.ret && f.ret->is_slice;
        if (rs->value) {
            eval(*rs->value);
            // float return -> xmm0 (already there if float eval); int -> rax (already)
            if (has_defers) {
                // stash the return value(s) across defer evaluation - a
                // defer's own expression may clobber rax/xmm0/rdx.
                if (is_float_ret) {
                    e.byte(0x48); e.byte(0x83); e.byte(0xEC); e.byte(0x08);        // sub rsp,8
                    e.byte(0xF3); e.byte(0x0F); e.byte(0x11); e.byte(0x04); e.byte(0x24); // movss [rsp],xmm0
                } else if (is_slice_ret) {
                    e.byte(0x48); e.byte(0x83); e.byte(0xEC); e.byte(0x10);        // sub rsp,16
                    e.store_reg_mem(Reg::rsp, 0, Reg::rax);
                    e.store_reg_mem(Reg::rsp, 8, Reg::rdx);
                } else {
                    e.push(Reg::rax);
                }
            }
        }
        if (has_defers) {
            emit_defers();
            if (rs->value) {
                if (is_float_ret) {
                    e.byte(0xF3); e.byte(0x0F); e.byte(0x10); e.byte(0x04); e.byte(0x24); // movss xmm0,[rsp]
                    e.byte(0x48); e.byte(0x83); e.byte(0xC4); e.byte(0x08);        // add rsp,8
                } else if (is_slice_ret) {
                    e.load_reg_mem(Reg::rax, Reg::rsp, 0);
                    e.load_reg_mem(Reg::rdx, Reg::rsp, 8);
                    e.byte(0x48); e.byte(0x83); e.byte(0xC4); e.byte(0x10);        // add rsp,16
                } else {
                    e.pop(Reg::rax);
                }
            }
        }
        emit_epilogue();
        return;
    }
    if (auto* is = dynamic_cast<const IfStmt*>(&s)) {
        eval(*is->cond);
        e.cmp_reg_imm32(Reg::rax, 0);
        Label else_l = e.alloc_label(), end_l = e.alloc_label();
        e.jcc(Cond::e, else_l);
        exec_block(is->then_b);
        if (is->has_else) e.jmp(end_l);
        e.bind(else_l);
        if (is->has_else) { exec_block(is->else_b); e.bind(end_l); }
        return;
    }
    if (auto* ws = dynamic_cast<const WhileStmt*>(&s)) {
        Label top = e.alloc_label(), end = e.alloc_label();
        e.bind(top);
        eval(*ws->cond);
        e.cmp_reg_imm32(Reg::rax, 0);
        e.jcc(Cond::e, end);
        // Item E ("hot local pinning"): entered after the condition check
        // (so it never activates on the "never entered the loop" path),
        // cleared before the backward jump - matches this loop's own
        // lexical extent (loop-scoped, not whole-function). v1 supports at
        // most one active pin: if an OUTER loop already holds it, this loop
        // simply doesn't get one (falls back to ordinary [rbp+off] codegen
        // for its body) rather than clobbering the outer loop's pin - only
        // the loop that actually set it clears it on exit.
        bool set_pin_here = false;
        if (!active_pin) {
            auto pin_name = find_pin_candidate(ws->body);
            if (pin_name) {
                // pin-entry: reload once per iteration (this whole region
                // reruns every time `top` is reached, including the
                // backward jump below) - still a net win per Task 2's N=2
                // floor: N memory loads become 1 load + N register reads.
                e.load_reg_mem(Reg::rbx, Reg::rbp, locals[*pin_name]);
                active_pin = PinState{*pin_name, Reg::rbx};
                set_pin_here = true;
            }
        }
        loops.push_back({top, end});
        exec_block(ws->body);
        loops.pop_back();
        if (set_pin_here) active_pin.reset();
        emit_budget_check(block_cost(ws->body));   // v0.4 budget (SAFETY §3): back-edge only
        e.jmp(top);
        e.bind(end);
        return;
    }
    if (auto* fs = dynamic_cast<const ForStmt*>(&s)) {
        // for (init; cond; step) { body } desugars to:
        //   init; L_cond: if(!cond) goto L_end; body; L_step: step; goto L_cond; L_end:
        // continue -> L_step (runs step, THEN rechecks cond) - not L_cond directly,
        // or `continue` would skip the step (a real, easy-to-get-wrong bug).
        auto saved_locals = locals;
        auto saved_types = local_types;
        if (fs->init) exec_stmt(*fs->init);
        Label cond_top = e.alloc_label();
        Label step_l = e.alloc_label();
        Label end_l = e.alloc_label();
        e.bind(cond_top);
        if (fs->cond) {
            eval(*fs->cond);
            e.cmp_reg_imm32(Reg::rax, 0);
            e.jcc(Cond::e, end_l);
        }
        // Item E ("hot local pinning"): candidate search over the loop body,
        // but kept active THROUGH the step clause too (a for-loop's own
        // counter update is almost always there, e.g. `i = i + 1`) - only
        // cleared right before the exit label. Same "outer loop already
        // holds the single pin slot" nesting rule as WhileStmt above.
        bool set_pin_here = false;
        if (!active_pin) {
            auto pin_name = find_pin_candidate(fs->body);
            if (pin_name) {
                // pin-entry: reload once per iteration, right after the
                // per-iteration cond check - same reasoning as WhileStmt
                // above. Stays active through the step clause below too.
                e.load_reg_mem(Reg::rbx, Reg::rbp, locals[*pin_name]);
                active_pin = PinState{*pin_name, Reg::rbx};
                set_pin_here = true;
            }
        }
        loops.push_back({step_l, end_l});
        exec_block(fs->body);
        loops.pop_back();
        e.bind(step_l);
        if (fs->step) eval(*fs->step);
        emit_budget_check(block_cost(fs->body));   // v0.4 budget (SAFETY §3): back-edge only
        e.jmp(cond_top);
        e.bind(end_l);
        if (set_pin_here) active_pin.reset();
        locals = std::move(saved_locals);
        local_types = std::move(saved_types);
        return;
    }
    if (auto* sw = dynamic_cast<const SwitchStmt*>(&s)) {
        // Dispatch prologue: compare the subject against each case constant
        // in order, jump to the first match (or default, or past everything).
        // Case BODIES are then emitted back to back with NO jump inserted
        // between them, so execution falls through from one case into the
        // next exactly like C's switch unless a `break` (or `return`) exits
        // early - correctness-first, matching this tree-walker's style
        // (no jump-table optimization).
        eval(*sw->subject);
        e.mov_reg_reg(Reg::r13, Reg::rax); // stash subject - survives the case-value evals below
        std::vector<Label> case_labels;
        for (size_t i = 0; i < sw->cases.size(); ++i) case_labels.push_back(e.alloc_label());
        Label end_label = e.alloc_label();
        int default_idx = -1;
        for (size_t i = 0; i < sw->cases.size(); ++i) {
            if (sw->cases[i].is_default) { default_idx = int(i); continue; }
            eval(*sw->cases[i].value);
            e.cmp_reg_reg(Reg::r13, Reg::rax);
            e.jcc(Cond::e, case_labels[i]);
        }
        e.jmp(default_idx >= 0 ? case_labels[size_t(default_idx)] : end_label);
        for (size_t i = 0; i < sw->cases.size(); ++i) {
            e.bind(case_labels[i]);
            loops.push_back({Label{0}, end_label, true}); // is_switch=true: continue skips this frame
            exec_block(sw->cases[i].body);
            loops.pop_back();
        }
        e.bind(end_label);
        return;
    }
    if (auto* bs = dynamic_cast<const BlockStmt*>(&s)) { exec_block(bs->block); return; }
    if (dynamic_cast<const BreakStmt*>(&s)) {
        if (!loops.empty()) e.jmp(loops.back().brk);
        return;
    }
    if (dynamic_cast<const ContinueStmt*>(&s)) {
        // skip past any enclosing switch frame(s) - continue always targets
        // the nearest real loop, never a switch (which is break-only).
        for (int i = int(loops.size()) - 1; i >= 0; --i) {
            if (!loops[size_t(i)].is_switch) { e.jmp(loops[size_t(i)].cont); break; }
        }
        return;
    }
}

// v0.4 instruction-budget cost estimator (SAFETY_AND_SANDBOX.md §3).
// Coarse statement count of a block/stmt — the spec's "how much work happened"
// proxy. Not cycle-accurate; just enough that a true infinite loop drives
// budget_remaining to zero in finite iterations. Recurses into nested blocks.
int64_t CG::block_cost(const Block& b) {
    int64_t n = 0;
    for (auto& s : b.stmts) n += 1 + stmt_cost(*s);
    return n < 1 ? 1 : n;  // floor at 1: empty infinite loop is still caught
}
int64_t CG::stmt_cost(const Stmt& s) {
    if (auto* bs = dynamic_cast<const BlockStmt*>(&s))  return block_cost(bs->block);
    if (auto* is = dynamic_cast<const IfStmt*>(&s))   return block_cost(is->then_b) + (is->has_else ? block_cost(is->else_b) : 0);
    if (auto* ws = dynamic_cast<const WhileStmt*>(&s)) return block_cost(ws->body);
    if (auto* fs = dynamic_cast<const ForStmt*>(&s))  return block_cost(fs->body);
    if (auto* ds = dynamic_cast<const DoWhileStmt*>(&s)) return block_cost(ds->body);
    if (auto* sw = dynamic_cast<const SwitchStmt*>(&s)) {
        int64_t n = 0; for (auto& c : sw->cases) n += block_cost(c.body); return n;
    }
    return 1;
}

} // namespace

GlobalsBlock* g_globals_for_codegen = nullptr;

CompiledFn compile_func(const FuncDecl& f, const CodeGenCtx& ctx) {
    CG cg(ctx, f);
    // prescan: find max_args, makes_calls
    cg.prescan_block(f.body);

    // prologue
    cg.e.push(Reg::rbp);
    cg.e.mov_reg_reg(Reg::rbp, Reg::rsp);
    // Reserve the rbx-save slot FIRST, before any other frame slot, so it's
    // always at a fixed, predictable offset (-8) regardless of what else
    // the function declares (Item E). Actual save happens via a plain store
    // after `sub rsp, frame_size` below, once the frame is live - not via
    // push, which would touch rsp and break the 16-byte-aligned frame_size
    // computation the rest of this function already depends on.
    cg.next_local_off += 8;
    cg.rbx_save_offset = -cg.next_local_off;
    // locals + arg temps (arg temps = max_args * 8, after locals)
    int32_t locals_bytes = cg.next_local_off; // already counted during... no, prescan doesn't alloc
    // We alloc locals during exec. But frame size must be known up front.
    // Simplification: give a generous fixed frame based on a quick count of let-stmts.
    // Count locals: walk and count LetStmt occurrences.
    // sum actual byte widths (slices=16, fixed arrays=N*elem_bytes, else 8)
    // rather than a flat per-local count*8 - needed now that locals can be
    // wider than 8 bytes (slice ABI / fixed arrays).
    int32_t locals_area = 8; // rbx_save_offset's reservation above (Item E) - must track next_local_off's starting bump
    for (size_t i = 0; i < f.params.size(); ++i) locals_area += CG::local_width_bytes(f.params[i].ty.get(), ctx.structs);
    if (cg.returns_struct_by_ptr()) locals_area += 8; // hidden return-pointer slot (struct_ret_ptr_offset)
    std::function<void(const Block&)> sum_bytes = [&](const Block& b) {
        for (auto& s : b.stmts) {
            if (auto* ls = dynamic_cast<const LetStmt*>(s.get()))
                locals_area += CG::local_width_bytes(ls->init ? ls->init->ty : ls->ty.get(), ctx.structs);
            if (auto* is = dynamic_cast<const IfStmt*>(s.get())) { sum_bytes(is->then_b); if(is->has_else) sum_bytes(is->else_b); }
            if (auto* ws = dynamic_cast<const WhileStmt*>(s.get())) sum_bytes(ws->body);
            if (auto* bs = dynamic_cast<const BlockStmt*>(s.get())) sum_bytes(bs->block);
            if (auto* fs = dynamic_cast<const ForStmt*>(s.get())) {
                if (fs->init) locals_area += CG::local_width_bytes(fs->init->init ? fs->init->init->ty : fs->init->ty.get(), ctx.structs);
                sum_bytes(fs->body);
            }
            if (auto* sw = dynamic_cast<const SwitchStmt*>(s.get()))
                for (auto& c : sw->cases) sum_bytes(c.body);
        }
    };
    sum_bytes(f.body);
    // collect_defers: gather all `defer EXPR;` sites in program order (see
    // CG::emit_defers for the runtime-flag LIFO semantics) and reserve one
    // 8-byte flag slot per site in the frame-size budget.
    std::function<void(const Block&)> collect_defers = [&](const Block& b) {
        for (auto& s : b.stmts) {
            if (auto* ds = dynamic_cast<const DeferStmt*>(s.get())) cg.defer_sites.push_back(ds);
            if (auto* is = dynamic_cast<const IfStmt*>(s.get())) { collect_defers(is->then_b); if (is->has_else) collect_defers(is->else_b); }
            if (auto* ws = dynamic_cast<const WhileStmt*>(s.get())) collect_defers(ws->body);
            if (auto* bs = dynamic_cast<const BlockStmt*>(s.get())) collect_defers(bs->block);
            if (auto* fs = dynamic_cast<const ForStmt*>(s.get())) collect_defers(fs->body);
            if (auto* sw = dynamic_cast<const SwitchStmt*>(s.get())) for (auto& c : sw->cases) collect_defers(c.body);
        }
    };
    collect_defers(f.body);
    locals_area += int32_t(cg.defer_sites.size()) * 8;
    int32_t arg_temps_area = cg.max_args * 8;
    cg.arg_temps_base = -locals_area; // arg temps come after locals (more negative)
    // Wait: locals are at [-1..-locals_area], arg_temps at [-locals_area-1 .. -locals_area-arg_temps_area].
    // Let's set arg_temps_base so arg_temps[i] = arg_temps_base - i*8 lives below locals.
    // Simpler: arg_temps at [-locals_area - 8 - i*8]. Set arg_temps_base = -(locals_area + 8).
    cg.arg_temps_base = -(locals_area + 8);
    int32_t total = locals_area + arg_temps_area + 16; // +16 slack
    cg.frame_size = round16(total);
    // sub rsp, frame_size
    cg.e.byte(0x48); cg.e.byte(0x81); cg.e.byte(0xEC); cg.e.imm32(cg.frame_size);
    // Save rbx into its reserved frame slot now that the (still 16-byte
    // aligned) frame is live (Item E) - paired with emit_epilogue()'s
    // load_reg_mem restore. Unconditional regardless of whether this
    // function's own pinning heuristic ends up using rbx.
    cg.e.store_reg_mem(Reg::rbp, cg.rbx_save_offset, Reg::rbx);

    // spill incoming params to frame slots (Win64: rcx/rdx/r8/r9 int, xmm0-3 float).
    // Done BEFORE the CPUID gate (cpuid clobbers rcx/rdx) and before the body.
    // Word-indexed (not param-indexed): a slice param consumes 2 consecutive
    // int-register words (ptr, len), a struct-by-value param consumes
    // ceil(size/8) words (raw bytes, always via int registers) - matching
    // the CallExpr caller-side convention above.
    static const Reg int_arg_regs[4] = {Reg::rcx, Reg::rdx, Reg::r8, Reg::r9};
    static const Xmm flt_arg_regs[4] = {Xmm::xmm0, Xmm::xmm1, Xmm::xmm2, Xmm::xmm3};
    // Word w<4 comes from a register (int_arg_regs[w]/flt_arg_regs[w]); word
    // w>=4 comes from the CALLER's outgoing stack-argument area. That area is
    // NOT at the textbook Win64 offset [rbp+16+(w-4)*8] - this codebase's
    // caller-side (CallExpr / eval_struct_returning_call) redundantly
    // stashes ALL words (including 0-3, which also go into registers) on the
    // stack first, THEN reserves shadow space, THEN the outgoing words 4+:
    // [caller_rsp + 0..n*8) = full word stash, [+n*8..+32) = shadow space,
    // [+n*8+32+(w-4)*8] = outgoing word w. From the callee's rbp (after
    // `call` pushes a return address and this prologue's `push rbp`, so
    // callee_rbp = caller_rsp_at_call - 16), that's
    // [rbp + 16 + n*8 + 32 + (w-4)*8] = [rbp + 48 + n*8 + (w-4)*8], where n
    // is THIS function's own total word count (hidden ptr + all params) -
    // it must equal what the caller used, since sema's arity check already
    // guarantees the call site matches this signature.
    // Resolving per-WORD (not per-param) means a param whose word range
    // straddles the register/stack boundary is handled correctly too.
    // This used to bail out entirely past word 4 (silently dropping any
    // later parameter with no error - found via a struct-by-value return,
    // whose hidden pointer eats word 0 and pushes ordinary 4-parameter
    // functions past the old limit), which is now fixed for every caller.
    int32_t total_words = cg.returns_struct_by_ptr() ? 1 : 0;
    for (auto& p : f.params) total_words += CG::words_for_type(p.ty.get(), ctx.structs);
    auto spill_word = [&](int32_t w, int32_t dst_off, bool is_float_word) {
        if (w < 4) {
            if (is_float_word) cg.e.movss_mem_xmm(Reg::rbp, dst_off, flt_arg_regs[w]);
            else cg.e.store_reg_mem(Reg::rbp, dst_off, int_arg_regs[w]);
        } else {
            int32_t src_off = 48 + total_words * 8 + (w - 4) * 8;
            if (is_float_word) {
                cg.e.movss_xmm_mem(Xmm::xmm4, Reg::rbp, src_off); // xmm4: scratch, unused elsewhere here
                cg.e.movss_mem_xmm(Reg::rbp, dst_off, Xmm::xmm4);
            } else {
                cg.e.load_reg_mem(Reg::rax, Reg::rbp, src_off);
                cg.e.store_reg_mem(Reg::rbp, dst_off, Reg::rax);
            }
        }
    };
    int32_t param_word = 0;
    if (cg.returns_struct_by_ptr()) {
        // word 0 is the hidden return-buffer pointer (see CG::
        // returns_struct_by_ptr / eval_struct_returning_call). Spilled to a
        // dedicated frame slot directly (bumping next_local_off by hand,
        // bypassing alloc_local's name->offset map) since it's not a
        // script-visible variable - ReturnStmt's codegen retrieves it via
        // struct_ret_ptr_offset, not by name.
        cg.next_local_off += 8;
        cg.struct_ret_ptr_offset = -cg.next_local_off;
        spill_word(0, cg.struct_ret_ptr_offset, false);
        param_word = 1;
    }
    for (size_t i = 0; i < f.params.size(); ++i) {
        const Type* pt = f.params[i].ty.get();
        int wcount = CG::words_for_type(pt, ctx.structs);
        bool is_struct = pt && !pt->struct_name.empty() && ctx.structs && ctx.structs->count(pt->struct_name) != 0;
        int32_t off = cg.alloc_local(f.params[i].name, pt);
        if (is_struct) {
            // A struct param's byte size is frequently NOT a multiple of 8
            // (e.g. Mixed: i64+f32+u8+bool = 14 bytes, wcount=2 words). The
            // LAST word must only write its real remaining bytes: writing a
            // full 8 bytes there overruns past this param's frame slot into
            // whatever sits right after it - when the struct is this
            // function's first local, that's its OWN saved rbp, so a naive
            // full-word spill corrupts the frame pointer (confirmed via a
            // crash whose symptom was garbled control flow well after the
            // corrupting call returned, not an immediate fault).
            auto sit = ctx.structs->find(pt->struct_name);
            int32_t struct_bytes = (sit != ctx.structs->end()) ? sit->second.size : wcount * 8;
            int32_t byte_pos = 0;
            for (int w = 0; w < wcount; ++w) {
                int32_t word_bytes = std::min<int32_t>(8, struct_bytes - byte_pos);
                int32_t w_global = param_word + w;
                if (word_bytes >= 8) {
                    spill_word(w_global, off + byte_pos, false);
                } else {
                    if (w_global < 4) cg.e.mov_reg_reg(Reg::rax, int_arg_regs[w_global]);
                    else {
                        int32_t src_off = 48 + total_words * 8 + (w_global - 4) * 8;
                        cg.e.load_reg_mem(Reg::rax, Reg::rbp, src_off);
                    }
                    cg.store_rax_bytes(Reg::rbp, off + byte_pos, word_bytes);
                }
                byte_pos += word_bytes;
            }
        } else if (pt->is_slice) {
            spill_word(param_word, off, false);
            spill_word(param_word + 1, off + 8, false);
        } else if (pt->is_float()) {
            spill_word(param_word, off, true);
        } else {
            spill_word(param_word, off, false);
        }
        param_word += wcount;
    }

    // reserve + zero-init the defer flags (see collect_defers above and
    // CG::emit_defers) - after param spilling so they get distinct offsets.
    for (size_t i = 0; i < cg.defer_sites.size(); ++i) {
        cg.next_local_off += 8;
        cg.defer_flag_offsets.push_back(-cg.next_local_off);
    }
    if (!cg.defer_sites.empty()) {
        cg.e.mov_reg_imm64(Reg::rax, 0);
        for (int32_t off : cg.defer_flag_offsets) cg.e.store_reg_mem(Reg::rbp, off, Reg::rax);
    }

    // @obf: layer on top of ctx.obf defaults, then emit the CPUID-keyed gate
    // (AFTER param spilling so cpuid's rcx/rdx clobber is safe) + opaque junk.
    cg.obf = ctx.obf;
    for (auto& ann : f.annotations) {
        if (ann.name == "obf") {
            for (auto& a : ann.args) {
                if (a == "\"mba\"")    cg.obf.mba = true;
                if (a == "\"opaque\"") cg.obf.opaque = true;
            }
        }
        if (ann.name == "obf_keyed") { cg.obf.keyed = true; }
    }
    if (cg.obf.keyed) cg.emit_cpuid_gate();
    cg.emit_opaque_junk();

    // body
    cg.exec_block(f.body);

    // implicit void return (in case function falls off end)
    if (!cg.defer_sites.empty()) cg.emit_defers();
    cg.emit_epilogue();

    cg.e.resolve_fixups();

    // Fill the absolute-imm64 placeholders recorded by mov_reg_imm64_external
    // (BUNDLING_AND_EM_MODULES.md Section 2.4). resolve_fixups does not touch these  - 
    // absolute relocation is deferred to the `.em` loader, but at JIT time we
    // still need the real addresses baked so the JIT'd code runs identically to
    // before. This writes the 8-byte imm64 at each AbsFixup's code_offset, the
    // exact bytes the old raw `mov_reg_imm64(Reg::r11, ctx.dispatch_base)` /
    // `mov_reg_imm64(Reg::r11, g_globals_for_codegen->base)` would have emitted,
    // so the byte output is unchanged. The `.em` serializer reads the SAME
    // AbsFixup list (via X64Emitter::abs_fixups()) to record the relocs, and
    // the loader repoints them at load time - the placeholders are what the
    // serializer sees pre-fill (and the post-fill bytes are what the JIT runs).
    for (const auto& af : cg.e.abs_fixups()) {
        if (af.code_offset + 8 > cg.e.code.size()) continue; // sanity
        uint8_t* p = cg.e.code.data() + af.code_offset;
        uint64_t v = 0;
        switch (af.kind) {
            case AbsFixup::DispatchTableBase:  v = uint64_t(ctx.dispatch_base); break;
            case AbsFixup::GlobalsBase:       v = uint64_t(ctx.globals_base); break;
            case AbsFixup::ModuleRegistryBase: v = 0; break; // reserved, unused in v1
        }
        for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i));
    }

    CompiledFn out;
    out.name = f.name;
    out.abs_fixups = cg.e.abs_fixups(); // capture for .em serialization (Section 2.4)
    out.bytes = std::move(cg.e.code);
    return out;
}

} // namespace ember
