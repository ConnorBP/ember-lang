#include "codegen.hpp"
#include "stmt_walk.hpp"  // walk_if: shared IfStmt traversal for prescan/count passes
#include "engine.hpp"
#include "context.hpp"   // TrapReason (unified trap surface, v0.4)
#include "peephole.hpp"  // Stage 1: post-emit peephole pipeline (docs/spec/CODEGEN_OPTIMIZATION_DESIGN.md §4.5)
#include "thin_lower.hpp"  // Stage A c2: AST -> ThinFunction lowering (the IR-backend path)
#include "thin_emit.hpp"   // Stage A c3: ThinFunction -> x86-64 emit (the IR-backend path)
#include "ember_pass.hpp"  // Stage C: EmberPassManager (run IR optimization passes)
#include <cassert>
#include <cstring>
#include <algorithm>
#include <array>
#include <limits>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__GNUC__)
#include <cpuid.h>
#endif

namespace ember {

// Bit-preserving uint64_t -> int64_t conversion (L-§10-3 portability):
// `int64_t(uint64_t(x))` for an out-of-range x is implementation-defined per
// [conv.integral]. memcpy reinterprets the bit pattern with defined behavior
// and is identical on every two's-complement target (x64/MinGW included).
static int64_t bit_cast_i64(uint64_t u) {
    int64_t i;
    std::memcpy(&i, &u, sizeof(i));
    return i;
}

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
    int fe_counter = 0;                  // unique suffix for for-each internals
    std::unordered_map<std::string, int32_t> locals;  // name -> offset from rbp
    std::unordered_map<std::string, const Type*> local_types;
    // Compiler-hidden temp frame slots for struct-by-value general-expression
    // args (a struct literal or struct-returning call passed by value) and for
    // a struct-literal `return` value. These have no single source frame
    // address (they're not a bare local), so codegen materializes them into a
    // fresh temp slot (via alloc_local with a synthesized name containing '$',
    // which cannot collide with any user identifier - ember identifiers can't
    // contain '$'), then copies the temp's bytes through the existing
    // hidden-pointer / arg-copy machinery. temp_counter hands out distinct
    // names so two temps in the same fn get distinct slots (alloc_local already
    // mangles by name). compile_func's sum_bytes pass pre-counts the total
    // bytes these temps need so the frame is sized to hold them.
    int32_t temp_counter = 0;
    int32_t alloc_struct_temp(const Type* t) {
        std::string name = "__tmp$" + std::to_string(temp_counter++);
        return alloc_local(name, t);
    }
    // Chunk c2: compiler-hidden temp frame slots for array-literal backing
    // storage. A slice array literal `let s: i64[] = [1, 2, 3];` needs a
    // separate backing region of `count * elem_size` bytes to hold the
    // elements (the slice local itself is only the 16-byte {ptr,len} pair,
    // whose ptr field points at this backing region). Reuses c1's
    // alloc_local-with-a-synthesized-name temp discipline but with a DISTINCT
    // name prefix (`__arrtmp$`) and a distinct counter so the two temp families
    // never collide (c1's struct temps are `__tmp$N`; these are `__arrtmp$M`).
    // arr_temp_types owns the synthesized fixed-array backing Types so the
    // raw pointers stashed in local_types stay stable for the function's
    // codegen lifetime (mirrors sema's type_store owning synthesized Types).
    // compile_func's count_arr_temps_block pre-counts the total backing bytes
    // so the frame is sized to hold them (mirrors count_struct_temps_block).
    int32_t arr_temp_counter = 0;
    std::vector<std::shared_ptr<Type>> arr_temp_types;
    int32_t alloc_arr_temp(const Type* elem, uint32_t count) {
        std::string name = "__arrtmp$" + std::to_string(arr_temp_counter++);
        auto bt = std::make_shared<Type>(*elem);   // base element type, then wrap as fixed array
        Type t; t.prim = elem->prim; t.array_len = count; t.elem = bt;
        arr_temp_types.push_back(std::make_shared<Type>(std::move(t)));
        return alloc_local(name, arr_temp_types.back().get());
    }
    int32_t arg_temps_base = 0;          // offset of arg-temp area start
    // Chunk c3: compiler-hidden temp frame slots for string-literal inline
    // decryption. When string encryption is on (string_xor_key != 0), a
    // StringLit's plaintext is decrypted INLINE into a stack frame slot at
    // each use site (no __str_decrypt heap native - see the StringLit eval
    // case). The slot is `ceil(baked_len, 8)` bytes (8-aligned to match
    // next_local_off's 8-byte slot discipline) and is reclaimed when the
    // function returns (it's rbp-relative, part of the frame). Reuses c1/c2's
    // alloc_local-with-a-synthesized-name temp discipline with a DISTINCT
    // prefix (`__strtmp$`) and counter so the three temp families never
    // collide. compile_func's count_str_temps_block pre-counts the total
    // backing bytes so the frame is sized to hold them (mirrors the struct/
    // arr temp pre-counts). The Type passed to alloc_local is a fixed array of
    // u8[baked_len] so local_width_bytes reserves the right number of bytes.
    int32_t str_temp_counter = 0;
    std::vector<std::shared_ptr<Type>> str_temp_types;
    int32_t alloc_str_temp(int64_t baked_len) {
        std::string name = "__strtmp$" + std::to_string(str_temp_counter++);
        auto bt = std::make_shared<Type>(make_prim(Prim::U8));
        Type t; t.prim = Prim::U8; t.array_len = uint32_t(baked_len); t.elem = bt;
        str_temp_types.push_back(std::make_shared<Type>(std::move(t)));
        return alloc_local(name, str_temp_types.back().get());
    }
    int32_t frame_size = 0;
    bool makes_calls = false;
    int max_args = 0;
    std::vector<uint8_t> rodata;
    std::string non_serializable_reason;
    struct PendingNative {
        CompiledNativeBinding binding;
        void* target = nullptr; // JIT-only; never serialized
    };
    std::vector<PendingNative> pending_natives;

    CG(const CodeGenCtx& c, const FuncDecl& fn) : ctx(c), f(fn) {
        enable_local_regalloc = c.enable_local_regalloc;
    }

    void emit_native(Reg dst, void* ptr, const std::string& name,
                     const Type* ret, const std::vector<Type>* params,
                     const char* feature) {
        if (name.empty() || !ret || !params) {
            if (non_serializable_reason.empty()) non_serializable_reason = std::string(feature) + " has no complete symbolic NativeSig binding";
            e.mov_reg_imm64(dst, int64_t(ptr));
            return;
        }
        e.mov_reg_native(dst, name);
        PendingNative pending;
        pending.binding.code_offset = e.native_fixups().back().code_offset;
        pending.binding.name = name;
        pending.binding.ret = *ret;
        pending.binding.params = *params;
        pending.target = ptr;
        pending_natives.push_back(std::move(pending));
    }
    const NativeSig* native_named(const std::string& name) const {
        if (!ctx.natives || name.empty()) return nullptr;
        auto it = ctx.natives->find(name);
        return it == ctx.natives->end() ? nullptr : &it->second;
    }
    uint32_t append_rodata(const uint8_t* data, size_t size) {
        uint32_t off = uint32_t(rodata.size());
        rodata.insert(rodata.end(), data, data + size);
        return off;
    }

    // Exact byte width of an Ember-layout value. Struct fields are tightly
    // packed and fixed arrays recurse through their element type, so neither
    // may fall back to the scalar default merely because it is nested.
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
        default: return 8; // I64/U64/F64 and opaque handle types
        }
    }

    // Local scalar slots remain word-sized; aggregates use their exact,
    // recursively computed Ember extent.
    static int32_t local_width_bytes(const Type* t, const StructLayoutTable* structs) {
        if (t && (t->is_slice || t->array_len > 0 ||
                  (!t->struct_name.empty() && structs && structs->count(t->struct_name))))
            return value_bytes(t, structs);
        return 8;
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
    // The alternative - a host-provided native (codegen calls a host
    // function by absolute address) that calls Win32 RaiseException(EXCEPTION_ARRAY_BOUNDS_EXCEEDED, ...) - was
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

    // --- v0.4 unified trap surface (docs/spec/SAFETY_AND_SANDBOX.md §2-§4, REDSHELL V7) ---
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
            if (non_serializable_reason.empty())
                non_serializable_reason = "trap stub/context/detail pointers require a host runtime binding";
            // Emitter invariant: rsp must be 16-byte aligned immediately
            // before every Win64 call.  Reserve exactly the mandatory 32-byte
            // shadow space plus only the padding required by the tracked
            // parity (normally 0; 8 while a temporary value is pushed).
            const int32_t call_frame = e.win64_call_frame_size(32);
            e.sub_reg_imm32(Reg::rsp, call_frame);
            // rcx = context_t* (the trap stub's first arg). B1: from r14 (the per-call
            // context register) when use_context_reg; else the baked imm64 ptr.
            if (ctx.use_context_reg) e.mov_reg_reg(Reg::rcx, Reg::r14);
            else                     e.mov_reg_imm64(Reg::rcx, int64_t(ctx.trap_ctx));
            e.mov_reg_imm64(Reg::rdx, int64_t(reason_ord));
            e.mov_reg_imm64(Reg::r8,  int64_t(reinterpret_cast<uintptr_t>(detail)));
            e.mov_reg_imm64(Reg::rax, int64_t(ctx.trap_stub));
            e.require_win64_call_alignment();
            e.call_reg(Reg::rax);
            // Stub normally longjmps; keep a balanced fallback if a faulty
            // host stub returns.
            e.add_reg_imm32(Reg::rsp, call_frame);
        } else {
            e.byte(0x0F); e.byte(0x0B); // ud2 (raises #UD, pre-v0.4 trap)
        }
    }

    // v0.4/M4 instruction budget (docs/spec/SAFETY_AND_SANDBOX.md §3). Emitted at
    // function entry and preserved at loop back-edges, gated by
    // ctx.emit_budget_checks + ctx.budget_ptr. Zero emitted when off.
    //   mov rax, budget_ptr
    //   sub qword [rax], body_cost   ; coarse entry/per-iteration cost
    //   jg  .continue                ; budget_remaining > 0, keep executing
    //   emit_trap(BudgetExceeded)   ; else trap (longjmp to checkpoint)
    //   .continue:
    // body_cost is a coarse recursive statement count (the spec's "how much
    // work happened" proxy — not cycle-accurate). A bare block floors at 1.
    // x64 `sub r/m64, imm32` sign-extends its immediate, so never cast a large
    // legal estimator result directly: clamp to the largest positive imm32.
    void emit_budget_check(int64_t body_cost, const char* detail) {
        if (!ctx.emit_budget_checks || body_cost <= 0) return;
        const int32_t encoded_cost = body_cost > std::numeric_limits<int32_t>::max()
            ? std::numeric_limits<int32_t>::max() : int32_t(body_cost);
        if (!ctx.use_context_reg && !ctx.budget_ptr) return;  // baked mode needs a ptr
        if (!ctx.use_context_reg && non_serializable_reason.empty())
            non_serializable_reason = "instruction-budget storage is process-local";
        if (ctx.use_context_reg) {
            // B1: sub qword [r14 + off_budget], imm32. r14 = context_t*, budget_remaining is a field.
            // sub r/m,imm32 = opcode 81 /5 (modrm.reg=5). rm=r14 (low3=6, REX.B).
            // REX.WB (49: W=64, B=r14) + 81 + modrm(10,reg=101,rm=110)=0xAE + disp32 + imm32.
            e.byte(0x49); e.byte(0x81); e.byte(0xAE); e.imm32(context_offsets::budget()); e.imm32(encoded_cost);
        } else {
            e.mov_reg_imm64(Reg::rax, int64_t(ctx.budget_ptr));
            // sub qword ptr [rax], imm32 : REX.W (48) + opcode 81 /5 + imm32
            e.byte(0x48); e.byte(0x81); e.byte(0x28); e.imm32(encoded_cost);
        }
        Label cont = e.alloc_label();
        e.jcc(Cond::g, cont);              // budget_remaining > 0 -> continue
        emit_trap(int(TrapReason::BudgetExceeded), detail);
        e.bind(cont);
    }

    // Reach-aware recursive function/block cost: counts statements PLUS the
    // emitted work they reach (expression nodes, native-call setup + arg
    // marshalling, aggregate byte copies, switch compares, for init/step).
    // Trusted native body execution time is intentionally OUTSIDE the unit
    // (a single nap() under budget is intended, not a counter-example) - only
    // the JIT call-site setup is charged. Floors at 1 and saturates at the
    // positive imm32 consumed by emit_budget_check (never wrap negative).
    int64_t block_cost(const Block& b);
    int64_t stmt_cost(const Stmt& s);
    int64_t expr_cost(const Expr& e);

    // v0.4/M4 combined call-depth guard (docs/spec/SAFETY_AND_SANDBOX.md §4). Emitted
    // for every script-issued script or native call. Native re-entry therefore
    // remains nested while the earlier native invocation is active.
    // Zero emitted when off. Pairs with emit_depth_leave() after the call.
    //   mov rax, depth_ptr
    //   inc dword [rax]            ; ++call_depth
    //   cmp dword [rax], max       ; depth reached max?
    //   jge .trap                  ; yes -> overflow
    //   .ok:
    //   <the call>
    //   ... emit_depth_leave() after the call returns: dec dword [rax]
    void emit_depth_check() {
        if (!ctx.emit_depth_checks) return;
        if (!ctx.use_context_reg && !ctx.depth_ptr) return;
        if (!ctx.use_context_reg && non_serializable_reason.empty())
            non_serializable_reason = "call-depth storage is process-local";
        if (ctx.use_context_reg) {
            // B1: inc dword [r14+off_depth]; then compare call_depth to the
            // PER-CONTEXT max_call_depth loaded from [r14+off_max_depth] (NOT
            // the compile-time ctx.max_call_depth baked as imm32). This lets two
            // contexts share one compiled body and observe different runtime
            // depth limits. Compile-time ctx.max_call_depth is retained only for
            // the legacy baked branch below.
            // inc r/m = FF /0 (modrm.reg=0). rm=r14(low3=6, REX.B). modrm(10,000,110)=0x86.
            e.byte(0x41); e.byte(0xFF); e.byte(0x86); e.imm32(context_offsets::depth());
            // mov eax, dword [r14+off_max_depth]: 8B /r(eax=0), rm=r14(REX.B).
            // modrm(10,000,110)=0x86.
            e.byte(0x41); e.byte(0x8B); e.byte(0x86); e.imm32(context_offsets::max_depth());
            // cmp dword [r14+off_depth], eax: 39 /r(eax=0), rm=r14(REX.B).
            // modrm(10,000,110)=0x86.
            e.byte(0x41); e.byte(0x39); e.byte(0x86); e.imm32(context_offsets::depth());
        } else {
            e.mov_reg_imm64(Reg::rax, int64_t(ctx.depth_ptr));
            e.byte(0xFF); e.byte(0x00);                  // inc dword ptr [rax]
            e.byte(0x81); e.byte(0x38); e.imm32(int32_t(ctx.max_call_depth)); // cmp dword [rax], imm32
        }
        Label ok = e.alloc_label();
        e.jcc(Cond::l, ok);                          // depth < max -> ok
        emit_trap(int(TrapReason::StackOverflow), "stack overflow: call depth exceeded");
        e.bind(ok);
    }
    void emit_depth_leave() {
        if (!ctx.emit_depth_checks) return;
        if (!ctx.use_context_reg && !ctx.depth_ptr) return;
        if (ctx.use_context_reg) {
            // After the call, rax HOLDS THE RESULT — must not clobber it. r14 is
            // the callee-saved context reg (preserved across the call), use it.
            // dec r/m = FF /1 (modrm.reg=1). rm=r14(low3=6, REX.B). modrm(10,001,110)=0x8E.
            e.byte(0x41); e.byte(0xFF); e.byte(0x8E); e.imm32(context_offsets::depth());
        } else {
            // Use r10 (caller-saved scratch, not a return/arg reg) for the ptr load.
            e.mov_reg_imm64(Reg::r10, int64_t(ctx.depth_ptr));
            e.byte(0x49); e.byte(0xFF); e.byte(0x0A);  // dec dword ptr [r10]
        }
    }
    // All script-issued native paths use this wrapper, including ordinary
    // calls, hidden-pointer struct returns, overloads, and string helpers.
    // The check precedes target materialization because baked-pointer depth
    // checks use rax as scratch; leave preserves integer and SSE returns.
    void emit_counted_native_call(void* ptr, const std::string& name,
                                  const Type* ret, const std::vector<Type>* params,
                                  const char* feature) {
        emit_depth_check();
        emit_native(Reg::rax, ptr, name, ret, params, feature);
        e.call_reg(Reg::rax);
        emit_depth_leave();
    }
    void emit_counted_named_native(void* ptr, const std::string& name, const char* feature) {
        const NativeSig* sig = native_named(name);
        emit_counted_native_call(ptr, name, sig ? &sig->ret : nullptr,
                                 sig ? &sig->params : nullptr, feature);
    }

    // v1.0 Tier 2 REDSHELL guard #6 (docs/planning/plan_FUNCTION_REFS.md §5.2): validate that
    // the i64 in rax is a registered script-fn slot before it's used as a
    // dispatch-table index. Two checks, both before any arg-stash clobber of
    // rcx/rdx/r8/r9 (the guard runs BEFORE the stash; rcx + r11 are its scratch):
    //   1. Range: cmp rax, slot_count; jae .trap  (unsigned — a negative handle
    //      reinterprets as huge and fails range, same one-shot trick as bounds check).
    //   2. Bit test: bt [allowlist + (handle>>3)], (handle&7); jnc .trap  — only
    //      slots whose bit is set pass.
    // .trap is bound BEFORE emit_trap so the fall-through (valid handle) reaches
    // the dispatch, matching emit_cpuid_gate's pattern. rax SURVIVES (scratch is
    // rcx + r11 only). No-op (zero emitted) when no allowlist is configured (the
    // fn-slot-count-0 case — function refs unused, e.g. all existing modules).
    void emit_call_target_guard() {
        if (ctx.fn_slot_count <= 0 || ctx.fn_allowlist_base == 0) return;  // no allowlist = no indirect calls = no guard
        if (non_serializable_reason.empty())
            non_serializable_reason = "function-reference allowlist storage is process-local";
        Label trap = e.alloc_label();
        // 1. Range: cmp rax, slot_count; jae .trap (unsigned). cmp r64,imm32: REX.W 81 /7.
        e.byte(0x48); e.byte(0x81); e.byte(0xF8); e.imm32(int32_t(ctx.fn_slot_count));  // cmp rax, imm32
        e.jcc(Cond::ae, trap);                 // unsigned >= -> out of range -> trap
        // 2. Bit test. r11 = allowlist_base (raw imm64); rcx = handle>>3 (byte offset);
        //    r11 += rcx; rcx = handle & 7 (bit index); bt [r11], rcx; jnc .trap.
        e.mov_reg_imm64(Reg::r11, ctx.fn_allowlist_base);
        e.mov_reg_reg(Reg::rcx, Reg::rax);     // rcx = handle (preserve rax)
        // shr rcx, 3: REX.W C1 /5 rcx, 3
        e.byte(0x48); e.byte(0xC1); e.byte(0xE9); e.byte(0x03);  // shr rcx, 3
        // add r11, rcx: REX.W 01 /r (add r/m, r), mod=11 reg=rcx rm=r11 (REX.B)
        e.byte(0x49); e.byte(0x01); e.byte(0xCB);  // add r11, rcx  (rm=r11 low3=3 +REX.B; reg=rcx low3=1) -> modrm 11_001_011=0xCB
        e.mov_reg_reg(Reg::rcx, Reg::rax);     // rcx = handle again (bit index)
        e.byte(0x48); e.byte(0x83); e.byte(0xE1); e.byte(0x07);  // and rcx, 7 (REX.W 83 /4 imm8)
        // bt [r11], rcx: 0F AB /4 (bt r/m64, r64). reg=rcx(low3=1, the bit index in the
        // reg field), rm=r11(low3=3, +REX.B). modrm = (00<<6)|(1<<3)|3 = 0x0B.
        // REX = W(1) + B(r11 extended) = 0x49 (reg=rcx not extended -> R=0).
        e.byte(0x49); e.byte(0x0F); e.byte(0xAB); e.byte(0x0B);  // bt [r11], rcx; CF = tested bit
        e.jcc(Cond::ae, trap);                 // bit clear (CF=0) -> not a registered fn -> trap
        // fall through: rax still = handle (guard used rcx + r11 only)
        Label after = e.alloc_label();
        e.jmp(after);                            // skip the trap block on the valid path
        e.bind(trap);
        emit_trap(int(TrapReason::BadCallTarget),
                  "call-target provenance: handle is not a registered function");
        e.bind(after);
    }

    // v0.5 cross-module call (docs/MODULES.md §3). The kind-2 sequence: one registry
    // hop then the same indirect call as intra-module. `mod_id` and `slot` are
    // baked as displacements; only `registry_base` is a reloc (kind 2, filled
    // with ctx.registry_base at JIT time / patched at .em load). If the call
    // was unresolved at sema (the module/fn not yet registered), emit a trap
    // stub call instead — the call traps gracefully until a relink resolves it
    // (docs/MODULES.md §5 step 3). Args are already stashed by the caller; this only
    // replaces the final `call [r11+slot*8]` with the registry-hop + call.
    void emit_cross_module_call(const CallExpr& c) {
        if (c.cross_module_unresolved) {
            // deferred trap (module/fn not registered yet). Args are stashed;
            // the trap longjmps (never returns) so the stashed args + the sub
            // rsp,total are harmless. rax=trap_ctx, etc. handled by emit_trap.
            emit_trap(int(TrapReason::None), "cross-module call unresolved: module or function not registered");
            return;
        }
        // mov r11, [registry_base + mod_id*8]  ; r11 = target module's DispatchTable base
        e.mov_reg_imm64_external(Reg::r11, AbsFixup::ModuleRegistryBase);  // kind-2 reloc
        e.load_reg_mem(Reg::r11, Reg::r11, int32_t(c.cross_module_id) * 8);  // registry hop
        // mov r11, [r11 + slot*8]  ; r11 = callee's entry address
        e.load_reg_mem(Reg::r11, Reg::r11, int32_t(c.cross_module_slot) * 8);
        e.call_reg(Reg::r11);
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
        if (auto* ms = dynamic_cast<const MatchStmt*>(&s)) {
            prescan_expr(*ms->subject);
            for (auto& arm : ms->arms) prescan_block(arm.body);
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

    // Pre-count the total frame bytes needed for compiler-hidden struct temp
    // slots (see temp_counter / alloc_struct_temp). Two sources: (1) a
    // `return <StructLit>;` in a struct-by-pointer-returning function (the
    // ReturnStmt struct-by-pointer path materializes the literal into a temp
    // before copying through the hidden pointer), and (2) a struct-by-value
    // call argument that is itself a StructLit or a struct-returning CallExpr
    // (the CallExpr arg-stash materializes it into a temp before copying into
    // the arg slot). A bare-Ident struct arg needs no temp (it already has a
    // frame address). Temps are short-lived (materialize -> copy -> done) but
    // are reserved as full frame slots for simplicity, so the SUM of all temp
    // bytes across the function is a safe (over-)reservation. ex.ty is set by
    // sema before compile_func runs, so the struct size is resolvable here via
    // value_bytes(ex.ty, ctx.structs).
    void count_struct_temps_block(const Block& b, int32_t& total) {
        for (auto& s : b.stmts) count_struct_temps_stmt(*s, total);
    }
    void count_struct_temps_stmt(const Stmt& s, int32_t& total) {
        if (auto* ls = dynamic_cast<const LetStmt*>(&s)) { if (ls->init) count_struct_temps_expr(*ls->init, total); return; }
        if (auto* es = dynamic_cast<const ExprStmt*>(&s)) { count_struct_temps_expr(*es->expr, total); return; }
        if (auto* rs = dynamic_cast<const ReturnStmt*>(&s)) {
            if (rs->value && returns_struct_by_ptr()) {
                // Only a StructLit return value needs a temp; the Ident and
                // forwarding-call branches copy directly / forward the hidden
                // pointer with no temp.
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
    bool is_registered_struct_ty(const Type* t) const {
        return t && !t->struct_name.empty() && ctx.structs && ctx.structs->count(t->struct_name) != 0;
    }
    void count_struct_temps_expr(const Expr& ex, int32_t& total) {
        if (auto* c = dynamic_cast<const CallExpr*>(&ex)) {
            if (c->receiver) count_struct_temps_expr(*c->receiver, total);
            for (auto& a : c->args) {
                // A struct-by-value arg that is NOT a bare Ident needs a temp.
                // ex.ty (the arg's sema-resolved type) is a registered struct
                // iff this is a struct-by-value arg slot. StructLit and
                // struct-returning CallExpr both resolve to the struct type and
                // are not Idents -> temp.
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

    // Chunk c2: pre-count the total frame bytes needed for array-literal
    // backing storage (see arr_temp_counter / alloc_arr_temp). A SLICE array
    // literal `let s: i64[] = [1,2,3];` (and a slice literal used as a call
    // arg or a return value) needs a separate backing region of
    // count*elem_size bytes to hold the elements; the slice local/arg/return
    // itself is only the 16-byte {ptr,len} pair whose ptr points at the
    // backing. A FIXED-ARRAY array literal `let a: i64[3] = [1,2,3];` does NOT
    // need a separate backing temp (its elements go directly into the fixed-
    // array local's own frame slot, already counted by sum_bytes via
    // local_width_bytes(ls->init->ty)). So this pass adds backing bytes ONLY
    // for slice-typed ArrayLits. ex.ty is set by sema before compile_func runs,
    // so the slice-ness and element size are resolvable here. Counting the SUM
    // is a safe over-reservation (backings are short-lived but reserved as
    // full frame slots, mirroring struct temps).
    void count_arr_temps_block(const Block& b, int32_t& total) {
        for (auto& s : b.stmts) count_arr_temps_stmt(*s, total);
    }
    void count_arr_temps_stmt(const Stmt& s, int32_t& total) {
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
            // A slice-typed ArrayLit needs a backing temp (count*elem_size bytes);
            // a fixed-array ArrayLit does not (its elements go into its own
            // local slot). Recurse into elements too (a nested slice ArrayLit
            // inside an element expr would need its own backing).
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

    // Chunk c3: pre-count the total frame bytes needed for string-literal inline
    // decryption temps (see str_temp_counter / alloc_str_temp). Every encrypted
    // StringLit (encrypted && baked_key != 0) needs a backing region of
    // baked_len bytes (8-aligned via alloc_local's local_width_bytes slot
    // discipline) to hold its inline-decrypted plaintext for the expression's
    // lifetime. The slot is rbp-relative and reclaimed at frame teardown, so the
    // plaintext is transient - it lives only on the stack for the expression.
    // Mirrors count_struct_temps_block / count_arr_temps_block. Counting the SUM
    // is a safe over-reservation (temps are short-lived but reserved as full
    // frame slots). baked_len is int64_t but in-tree literals are <256 bytes;
    // cast to int32_t for the total as everywhere else.
    void count_str_temps_block(const Block& b, int32_t& total) {
        for (auto& s : b.stmts) count_str_temps_stmt(*s, total);
    }
    void count_str_temps_stmt(const Stmt& s, int32_t& total) {
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
            return;   // a StringLit has no sub-expressions
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
            walk_if(*is, [&](const Expr& e){ count_pin_refs_expr(e, counts); }, [&](const Block& b){ count_pin_refs_block(b, counts); });
            return;
        }
        if (auto* ws = dynamic_cast<const WhileStmt*>(&s)) { count_pin_refs_expr(*ws->cond, counts); count_pin_refs_block(ws->body, counts); return; }
        if (auto* ds = dynamic_cast<const DoWhileStmt*>(&s)) { count_pin_refs_block(ds->body, counts); count_pin_refs_expr(*ds->cond, counts); return; }
        if (auto* fe = dynamic_cast<const ForEachStmt*>(&s)) { count_pin_refs_expr(*fe->iter, counts); count_pin_refs_block(fe->body, counts); return; }
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

    static int int_bits(const Type* t) {
        if (!t) return 64;
        switch (t->prim) {
        case Prim::I8: case Prim::U8: return 8;
        case Prim::I16: case Prim::U16: return 16;
        case Prim::I32: case Prim::U32: return 32;
        default: return 64;
        }
    }
    void normalize_rax(const Type* t) {
        if (!t || !t->is_int() || t->is_fn_handle || !t->struct_name.empty()) return;
        int bits = int_bits(t);
        if (bits == 64) return;
        if (bits == 32) {
            if (t->is_uint()) { e.byte(0x89); e.byte(0xC0); } // mov eax,eax
            else { e.byte(0x48); e.byte(0x63); e.byte(0xC0); } // movsxd rax,eax
            return;
        }
        e.byte(0x48); e.byte(0xC1); e.byte(t->is_uint() ? 0xE0 : 0xE0); e.byte(uint8_t(64-bits));
        e.byte(0x48); e.byte(0xC1); e.byte(t->is_uint() ? 0xE8 : 0xF8); e.byte(uint8_t(64-bits));
    }
    void emit_integer_divmod(bool want_mod, bool is_unsigned) {
        Label nonzero = e.alloc_label();
        e.cmp_reg_imm32(Reg::rcx, 0); e.jcc(Cond::ne, nonzero);
        emit_trap(int(TrapReason::DivByZero), "integer division by zero"); e.bind(nonzero);
        if (!is_unsigned) {
            Label safe = e.alloc_label(), overflow = e.alloc_label();
            e.cmp_reg_imm32(Reg::rcx, -1); e.jcc(Cond::ne, safe);
            e.mov_reg_imm64(Reg::r10, INT64_MIN); e.cmp_reg_reg(Reg::rax, Reg::r10); e.jcc(Cond::e, overflow);
            e.jmp(safe); e.bind(overflow);
            emit_trap(int(TrapReason::DivByZero), "signed division overflow"); e.bind(safe);
            e.byte(0x48); e.byte(0x99);              // cqo
            e.byte(0x48); e.byte(0xF7); e.byte(0xF9); // idiv rcx
        } else {
            e.byte(0x48); e.byte(0x31); e.byte(0xD2); // xor rdx,rdx
            e.byte(0x48); e.byte(0xF7); e.byte(0xF1); // div rcx
        }
        if (want_mod) e.mov_reg_reg(Reg::rax, Reg::rdx);
    }

    // --- expression evaluation: int/bool result in rax, float in xmm0 ---
    void eval(const Expr& ex);

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

    // Materialize a value directly into Ember-layout memory. Aggregates never
    // acquire a fake scalar-register representation: nested literals recurse,
    // and existing aggregate locals are copied by their exact packed extent.
    void store_value_to_memory(const Expr& value, const Type* t, Reg dst, int32_t off) {
        if (t && !t->struct_name.empty() && ctx.structs && ctx.structs->count(t->struct_name)) {
            const StructLayout& layout = ctx.structs->at(t->struct_name);
            if (auto* lit = dynamic_cast<const StructLit*>(&value)) {
                for (const auto& kv : lit->fields) {
                    auto fit = layout.fields.find(kv.first);
                    if (fit != layout.fields.end())
                        store_value_to_memory(*kv.second, fit->second.ty, dst,
                                              off + fit->second.offset);
                }
                return;
            }
            int32_t src_off = 0; const Type* src_ty = nullptr;
            if (local_value_offset(value, src_off, src_ty))
                copy_bytes(dst, off, Reg::rbp, src_off, layout.size);
            return;
        }
        if (t && t->array_len > 0) {
            int32_t src_off = 0; const Type* src_ty = nullptr;
            if (local_value_offset(value, src_off, src_ty))
                copy_bytes(dst, off, Reg::rbp, src_off, value_bytes(t, ctx.structs));
            return;
        }
        eval(value);
        if (t && t->is_slice) {
            e.store_reg_mem(dst, off, Reg::rax);
            e.store_reg_mem(dst, off + 8, Reg::rdx);
        } else if (t && t->prim == Prim::F64) e.movsd_mem_xmm(dst, off, Xmm::xmm0);
        else if (t && t->prim == Prim::F32) e.movss_mem_xmm(dst, off, Xmm::xmm0);
        else e.store_rax_elem(dst, off, value_bytes(t, ctx.structs));
    }

    // load a value (int or float) from a stack slot [rbp + off]
    void load_slot(int32_t off, const Type* t) {
        if (t->prim == Prim::F64) e.movsd_xmm_mem(Xmm::xmm0, Reg::rbp, off);
        else if (t->is_float()) e.movss_xmm_mem(Xmm::xmm0, Reg::rbp, off);
        else { e.load_reg_mem(Reg::rax, Reg::rbp, off); normalize_rax(t); }
    }
    // store rax (int) or xmm0 (float) to a stack slot [rbp + off]
    void store_slot(int32_t off, const Type* t) {
        if (t->prim == Prim::F64) e.movsd_mem_xmm(Reg::rbp, off, Xmm::xmm0);
        else if (t->is_float()) e.movss_mem_xmm(Reg::rbp, off, Xmm::xmm0);
        else { normalize_rax(t); // mov [rbp+off], rax  -> REX.W 89 /r mod=10 reg=rax rm=rbp
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
        // (Stage 1 local regalloc uses volatile r10 — no callee-saved restore here.)
        e.mov_reg_reg(Reg::rsp, Reg::rbp);
        e.pop(Reg::rbp);
        e.ret();
    }

    // loop context for break/continue (docs/spec/CODEGEN_SPEC.md). v0.x codegen had these
    // as silent no-ops (fall-through bug surfaced by the spectator_list port).
    // is_switch: a switch pushes a frame too (so `break` can exit it via the
    // same mechanism) but `continue` must skip past it to the nearest real
    // loop - switch is a break-only construct, not a loop.
    struct LoopCtx {
        Label cont;
        Label brk;
        bool is_switch = false;
        size_t cleanup_depth = 0; // lexical cleanup scopes retained by this transfer
    };
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
    struct PinState { std::string name; int32_t offset; Reg reg; };
    std::optional<PinState> active_pin;

    // enable_local_regalloc mirrors ctx.enable_local_regalloc, captured once in
    // the CG ctor so the BinExpr eval path reads a cheap member instead of
    // threading ctx through every eval recursion.
    bool enable_local_regalloc = false;
    // r10_holding_lhs: true while the volatile scratch r10 holds a BinExpr LHS
    // across an RHS eval that contains NO r10-clobbering operation. r10 is volatile
    // (no prologue save/restore needed -> ZERO tax on any function, unlike a
    // callee-saved holding register which would need per-function save/restore
    // and can net WORSE on call-heavy code where the tax exceeds the per-BinExpr
    // win). A RHS that clobbers r10 falls back to the push/pop path (the stack).
    // A NESTED BinExpr whose r10 is already claimed also falls back (a single
    // holding register can't nest). Cleared after the op.
    bool r10_holding_lhs = false;
    // Does `ex` transitively contain anything that CLOBBERS r10? The regalloc's
    // r10 holding path is only safe when the RHS leaves r10 untouched. r10 is
    // clobbered by: (a) a CallExpr (r10 is volatile); (b) a SIGNED Div/Mod
    // (emit_integer_divmod's signed overflow check does `mov r10, INT64_MIN`);
    // (c) a global struct/array/slice access (the IndexExpr/FieldExpr global
    // base path does `mov r10, GlobalsBase` + copy_bytes through r10). A scalar
    // global read, a local-array/slice index, and arithmetic other than signed
    // div/mod are all r10-safe. Conservative: if in doubt, return true (fallback
    // to push/pop) — correctness over the micro-win.
    bool expr_clobbers_r10(const Expr& ex) const {
        if (dynamic_cast<const CallExpr*>(&ex)) return true;
        if (auto* b = dynamic_cast<const BinExpr*>(&ex)) {
            // A signed Div/Mod clobbers r10 (emit_integer_divmod's INT64_MIN check).
            // An unsigned Div/Mod does not (xor rdx; div). is_uint is on the LHS type.
            if ((b->op == BinExpr::Op::Div || b->op == BinExpr::Op::Mod) &&
                !(b->lhs->ty && b->lhs->ty->is_uint())) return true;
            return expr_clobbers_r10(*b->lhs) || expr_clobbers_r10(*b->rhs);
        }
        if (auto* u = dynamic_cast<const UnaryExpr*>(&ex)) return expr_clobbers_r10(*u->operand);
        if (auto* c = dynamic_cast<const CastExpr*>(&ex)) return expr_clobbers_r10(*c->operand);
        if (auto* t = dynamic_cast<const TernaryExpr*>(&ex)) return expr_clobbers_r10(*t->cond) || expr_clobbers_r10(*t->then_e) || expr_clobbers_r10(*t->else_e);
        if (auto* a = dynamic_cast<const AssignExpr*>(&ex)) { if (a->target && expr_clobbers_r10(*a->target)) return true; return expr_clobbers_r10(*a->value); }
        if (auto* ix = dynamic_cast<const IndexExpr*>(&ex)) {
            // A global aggregate base clobbers r10 (the global-base load path),
            // AND a local SLICE base clobbers r10 (the IndexExpr slice-base path
            // does `mov r10, rax` to hold the slice ptr across the index eval).
            // A local FIXED-ARRAY base does not (it uses rbp). A non-Ident base is
            // conservative-clobber (may eval through r10).
            if (auto* bid = dynamic_cast<const Ident*>(ix->base.get())) {
                auto it = locals.find(bid->name);
                if (it == locals.end()) return true;          // global base -> r10
                const Type* lt = local_types.count(bid->name) ? local_types.at(bid->name) : ix->base->ty;
                if (lt && lt->is_slice) return true;           // local slice base -> r10
            } else {
                return true; // non-Ident base -> conservative clobber
            }
            return expr_clobbers_r10(*ix->index);
        }
        if (auto* fx = dynamic_cast<const FieldExpr*>(&ex)) {
            // Same reasoning as IndexExpr: a global struct base clobbers r10.
            // A FieldExpr whose base is itself an IndexExpr (arr[i].field)
            // clobbers r10 exactly when that IndexExpr does - a local SLICE
            // or GLOBAL array/slice base loads through r10, and the index
            // expression may too (a call, a signed div/mod); a local
            // FIXED-ARRAY base with an r10-safe index does not (rbp-relative
            // addressing, r11/rcx/rax only - see the FieldExpr IndexExpr-base
            // eval case).
            if (auto* bid = dynamic_cast<const Ident*>(fx->base.get())) {
                if (locals.find(bid->name) == locals.end()) return true;
            } else if (auto* ix = dynamic_cast<const IndexExpr*>(fx->base.get())) {
                if (auto* ibid = dynamic_cast<const Ident*>(ix->base.get())) {
                    auto it = locals.find(ibid->name);
                    if (it == locals.end()) return true;          // global array/slice base -> r10
                    const Type* lt = local_types.count(ibid->name) ? local_types.at(ibid->name) : ix->base->ty;
                    if (lt && lt->is_slice) return true;           // local slice base -> r10
                } else {
                    return true; // non-Ident array base -> conservative clobber
                }
                return expr_clobbers_r10(*ix->index);
            } else {
                return true;
            }
            return false;
        }
        if (auto* sl = dynamic_cast<const StructLit*>(&ex)) { for (auto& kv : sl->fields) if (expr_clobbers_r10(*kv.second)) return true; return false; }
        // Ident (local scalar/global scalar), IntLit, BoolLit, FloatLit, StringLit,
        // FnHandleExpr, ViewExpr, ArrayLit: none clobber r10 in their eval.
        return false;
    }

    // --- lexical defer cleanup scopes ---
    // Every lexical Block pushes one cleanup scope while it is emitted. A
    // textual defer site owns a frame flag plus the local binding environment
    // visible at its declaration. The flag is reset whenever the owning block
    // is entered, set when the statement is reached, and cleared immediately
    // before its expression runs. This makes loop-body sites per-iteration and
    // prevents a conditional cleanup edge followed by fallthrough from firing
    // the same activation twice.
    struct DeferSite {
        const DeferStmt* stmt = nullptr;
        int32_t flag_offset = 0;
        std::unordered_map<std::string, int32_t> locals_at_decl;
        std::unordered_map<std::string, const Type*> types_at_decl;
    };
    struct CleanupScope { std::vector<size_t> reached_sites; };
    std::vector<DeferSite> defer_sites;
    std::unordered_map<const DeferStmt*, size_t> defer_site_indices;
    std::vector<CleanupScope> cleanup_scopes;

    void emit_defer_site(size_t index) {
        DeferSite& site = defer_sites[index];
        e.load_reg_mem(Reg::rax, Reg::rbp, site.flag_offset);
        e.cmp_reg_imm32(Reg::rax, 0);
        Label skip = e.alloc_label();
        e.jcc(Cond::e, skip);
        // Clear first: this activation executes at most once even when another
        // generated edge later reaches the same cleanup code.
        e.mov_reg_imm64(Reg::rax, 0);
        e.store_reg_mem(Reg::rbp, site.flag_offset, Reg::rax);
        auto saved_locals = locals;
        auto saved_types = local_types;
        auto saved_pin = active_pin;
        active_pin.reset();
        locals = site.locals_at_decl;
        local_types = site.types_at_decl;
        eval(*site.stmt->expr);
        locals = std::move(saved_locals);
        local_types = std::move(saved_types);
        if (saved_pin) e.load_reg_mem(saved_pin->reg, Reg::rbp, saved_pin->offset);
        active_pin = std::move(saved_pin);
        e.bind(skip);
    }
    void emit_cleanup_scope(size_t index) {
        const auto& reached = cleanup_scopes[index].reached_sites;
        for (auto it = reached.rbegin(); it != reached.rend(); ++it)
            emit_defer_site(*it);
    }
    void emit_cleanups_to(size_t retained_depth) {
        for (size_t n = cleanup_scopes.size(); n > retained_depth; --n)
            emit_cleanup_scope(n - 1);
    }
    bool has_active_cleanups() const {
        for (const auto& scope : cleanup_scopes)
            if (!scope.reached_sites.empty()) return true;
        return false;
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
        e.sub_reg_imm32(Reg::rsp, total);
        e.store_reg_mem(Reg::rsp, 0, Reg::rax); // word 0: the hidden dest pointer (already in rax)
        for (auto& op : ops) {
            int32_t off = op.slot0 * 8;
            if (op.is_struct) {
                auto sit = (op.ty && !op.ty->struct_name.empty() && ctx.structs) ? ctx.structs->find(op.ty->struct_name) : ctx.structs->end();
                if (sit != ctx.structs->end())
                    stash_struct_arg(op.e, op.ty, off, sit->second);
            } else {
                eval(*op.e);
                if (op.words == 2) {
                    e.store_reg_mem(Reg::rsp, off, Reg::rax);
                    e.store_reg_mem(Reg::rsp, off + 8, Reg::rdx);
                } else if (op.ty && op.ty->is_float()) {
                    if(op.ty->prim==Prim::F64)e.movsd_mem_xmm(Reg::rsp,off,Xmm::xmm0);else e.movss_mem_xmm(Reg::rsp,off,Xmm::xmm0);
                } else {
                    e.byte(0x48); e.byte(0x89); e.byte(0x84); e.byte(0x24); e.imm32(off);
                }
            }
        }
        std::vector<bool> word_is_float(size_t(n), false), word_is_f64(size_t(n), false);
        for (auto& op : ops) if (op.words==1 && !op.is_struct && op.ty && op.ty->is_float()) {
            word_is_float[size_t(op.slot0)]=true; word_is_f64[size_t(op.slot0)]=op.ty->prim==Prim::F64;
        }
        static const Reg int_regs[4] = {Reg::rcx, Reg::rdx, Reg::r8, Reg::r9};
        static const Xmm flt_regs[4] = {Xmm::xmm0, Xmm::xmm1, Xmm::xmm2, Xmm::xmm3};
        for (int w = 0; w < n && w < 4; ++w) {
            int32_t off = w * 8;
            if (word_is_float[size_t(w)]) { if(word_is_f64[size_t(w)])e.movsd_xmm_mem(flt_regs[w],Reg::rsp,off);else e.movss_xmm_mem(flt_regs[w],Reg::rsp,off); }
            else e.load_reg_mem(int_regs[w], Reg::rsp, off);
        }
        for (int w = 4; w < n; ++w) {
            int32_t src = w * 8;
            int32_t dst = stash_size + 32 + (w - 4) * 8;
            e.load_reg_mem(Reg::rax, Reg::rsp, src);
            e.store_reg_mem(Reg::rsp, dst, Reg::rax);
        }
        if (c.is_native) {
            emit_counted_named_native(c.native_fn, c.native_binding_name, "native call");
        } else if (!c.module_alias.empty()) {
            // v0.5 cross-module call (mod::fn): kind-2 registry-hop sequence.
            emit_depth_check();
            emit_cross_module_call(c);
            emit_depth_leave();
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
        e.add_reg_imm32(Reg::rsp, total);
        // no result in rax/xmm0 to propagate - the callee wrote the struct
        // directly through the hidden pointer.
    }
    // Copy a registered struct's bytes from a local (by name) to the address
    // in `dest_reg` (matching how struct-by-value call arguments are copied
    // above - exact byte count, not a rounded-up word count). c3: if the name
    // is a GLOBAL struct (not a local), copy from [globals_base + offset]
    // instead of [rbp + local_off] - a struct global used by-value (`return cfg;`
    // or `foo(cfg)`) has no frame slot, so the global block is the source.
    void copy_struct_to_ptr(const std::string& local_name, const StructLayout& layout, Reg dest_reg) {
        auto it = locals.find(local_name);
        if (it != locals.end()) {
            copy_bytes(dest_reg, 0, Reg::rbp, it->second, layout.size);
            return;
        }
        // c3: struct global by-value. Resolve the typed byte offset (globals_
        // offsets if wired, else index*8) and copy layout.size bytes from
        // [globals_base + offset] into dest_reg. mov r10, <globals_base reloc>;
        // copy_bytes(dest_reg, 0, r10, offset, size).
        const auto* gidx = ctx.globals_index ? ctx.globals_index : (g_globals_for_codegen ? &g_globals_for_codegen->index : nullptr);
        const auto* goffsets = ctx.globals_offsets ? ctx.globals_offsets : (g_globals_for_codegen ? &g_globals_for_codegen->offsets : nullptr);
        const auto* gtypes = ctx.globals_types ? ctx.globals_types : (g_globals_for_codegen ? &g_globals_for_codegen->types : nullptr);
        if (gidx && gtypes) {
            int32_t goff = 0; bool found = false;
            if (goffsets) { auto oit = goffsets->find(local_name); if (oit != goffsets->end()) { goff = int32_t(oit->second); found = true; } }
            if (!found) { auto gi = gidx->find(local_name); if (gi != gidx->end()) { goff = int32_t(gi->second) * 8; found = true; } }
            if (found) {
                e.mov_reg_imm64_external(Reg::r10, AbsFixup::GlobalsBase);
                copy_bytes(dest_reg, 0, Reg::r10, goff, layout.size);
            }
        }
    }

    // Stash a struct-by-value call argument into the call's arg-stash region
    // at [rsp+off]. Sema (check_struct_arg_shape) permits three shapes: a bare
    // Ident (copy from its frame address), a StructLit of the matching struct
    // (materialize into a fresh temp, then copy), or a struct-returning CallExpr
    // of the matching struct (lea the temp's address into rax as the hidden
    // word-0, eval_struct_returning_call writes through it into the temp, then
    // copy). Each non-Ident arg gets its OWN fresh temp (alloc_struct_temp
    // hands out distinct '$'-tagged names so two struct args in one call never
    // alias), sized into the frame by count_struct_temps_block. The temp lives
    // in the caller's frame (a plain stack slot) - W^X and register
    // preservation are unaffected.
    void stash_struct_arg(const Expr* arg_e, const Type* ty, int32_t off, const StructLayout& layout) {
        if (auto* id = dynamic_cast<const Ident*>(arg_e)) {
            auto it = locals.find(id->name);
            if (it != locals.end()) {
                copy_bytes(Reg::rsp, off, Reg::rbp, it->second, layout.size);
                return;
            }
            // c3: struct GLOBAL by-value arg. Copy from [globals_base + offset]
            // into the arg-stash region at [rsp+off] (mirrors copy_struct_to_ptr's
            // global fallback - a struct global has no frame slot).
            const auto* gidx = ctx.globals_index ? ctx.globals_index : (g_globals_for_codegen ? &g_globals_for_codegen->index : nullptr);
            const auto* goffsets = ctx.globals_offsets ? ctx.globals_offsets : (g_globals_for_codegen ? &g_globals_for_codegen->offsets : nullptr);
            const auto* gtypes = ctx.globals_types ? ctx.globals_types : (g_globals_for_codegen ? &g_globals_for_codegen->types : nullptr);
            if (gidx && gtypes) {
                int32_t goff = 0; bool found = false;
                if (goffsets) { auto oit = goffsets->find(id->name); if (oit != goffsets->end()) { goff = int32_t(oit->second); found = true; } }
                if (!found) { auto gi = gidx->find(id->name); if (gi != gidx->end()) { goff = int32_t(gi->second) * 8; found = true; } }
                if (found) {
                    e.mov_reg_imm64_external(Reg::r10, AbsFixup::GlobalsBase);
                    copy_bytes(Reg::rsp, off, Reg::r10, goff, layout.size);
                }
            }
            return;
        }
        if (auto* sl = dynamic_cast<const StructLit*>(arg_e)) {
            int32_t temp_off = alloc_struct_temp(ty);
            store_value_to_memory(*sl, ty, Reg::rbp, temp_off);
            copy_bytes(Reg::rsp, off, Reg::rbp, temp_off, layout.size);
            return;
        }
        if (auto* call = dynamic_cast<const CallExpr*>(arg_e)) {
            int32_t temp_off = alloc_struct_temp(ty);
            e.byte(0x48); e.byte(0x8D); e.byte(0x85); e.imm32(temp_off); // lea rax, [rbp+temp_off]
            eval_struct_returning_call(*call);
            copy_bytes(Reg::rsp, off, Reg::rbp, temp_off, layout.size);
            return;
        }
        // Sema's check_struct_arg_shape rejects any other shape; this is
        // unreachable for a sema-clean program. Trap rather than copy garbage.
        emit_trap(int(TrapReason::IllegalInstruction), "internal: struct-by-value arg is not a local/literal/call");
    }

    // --- obfuscation helpers (docs/spec/CODEGEN_SPEC.md Section obf) ---
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
        // Restore rbx before branching so both successors have the same stack
        // parity. pop does not alter CPUID's EAX result or flags (the compare
        // follows it), and a keyed-gate trap must not leave emitter tracking
        // dependent on the unrelated success edge.
        e.pop(Reg::rbx);
        // cmp eax, imm32: use the 32-bit CPUID signature (no REX, 81 /7 id).
        e.byte(0x81); e.byte(0xF8); e.imm32(int32_t(obf.cpuid_key));
        Label ok = e.alloc_label();
        e.jcc(Cond::e, ok);
        // v0.4: route through the trap stub (longjmp to checkpoint) instead of
        // ud2 -> recoverable, not process death (REDSHELL V7).
        emit_trap(int(TrapReason::IllegalInstruction), "@obf_keyed: CPUID gate mismatch");
        e.bind(ok);
    }

    // --- MBA (Mixed Boolean-Arithmetic) substitution helpers ---
    // Each replaces the obvious instruction with an algebraically-equivalent
    // sequence (no occurrence of the original opcode). Operands are held in
    // dst=lhs (rax) and src=rhs (rcx); rdx is a scratch that the BinExpr path
    // never keeps live across the eval, so it's free to clobber.
    //
    // Identities used (see docs/spec/CODEGEN_SPEC.md Section obf MBA pass):
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
void store_xmm0_to_rbp(CG& cg, int32_t off, const Type* t = nullptr) {
    if (t && t->prim == Prim::F64) cg.e.movsd_mem_xmm(Reg::rbp, off, Xmm::xmm0);
    else cg.e.movss_mem_xmm(Reg::rbp, off, Xmm::xmm0);
}
// load [rbp + disp32] -> rax
void load_rbp_to_rax(CG& cg, int32_t off) {
    // REX.W 8B /r mod=10 reg=rax(0) rm=rbp(5) -> 48 8B 85 <disp32>
    cg.e.byte(0x48); cg.e.byte(0x8B); cg.e.byte(0x85); cg.e.imm32(off);
}
// load [imm64 + disp32] -> rax  (for globals: mov rax, [globals_base + off])
// The globals base is a relocatable imm64 (docs/BUNDLING_AND_EM_MODULES.md Section 2.4):
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

// store xmm0 (a float) to a global slot [globals_base + off]. Mirrors
// store_xmm0_to_rbp but through the relocatable globals base. movss [r11+disp32],
// xmm0: r11 is reg 11, so the rm field needs REX.B (0x41) to extend rm=011 -> r11.
// Prefix order matters: REX must come LAST (immediately before the opcode 0F 11),
// AFTER the mandatory F3 prefix. Wrong order (41 F3 0F 11) drops REX -> stores to
// rbx instead of r11 (the bug that broke float-global writes in the heap case).
// Correct: F3 41 0F 11 + modrm(10, xmm0=0, r11=3)=0x83 + disp32.
void store_xmm0_to_global(CG& cg, int64_t /*base*/, int32_t off, const Type* t = nullptr) {
    cg.e.mov_reg_imm64_external(Reg::r11, AbsFixup::GlobalsBase);
    if (t && t->prim == Prim::F64) cg.e.movsd_mem_xmm(Reg::r11, off, Xmm::xmm0);
    else cg.e.movss_mem_xmm(Reg::r11, off, Xmm::xmm0);
}

void CG::eval(const Expr& ex) {
    if (auto* lit = dynamic_cast<const IntLit*>(&ex)) {
        e.mov_reg_imm64(Reg::rax, lit->v);
        return;
    }
    if (auto* lit = dynamic_cast<const FloatLit*>(&ex)) {
        if (ex.ty && ex.ty->prim == Prim::F64) {
            uint64_t bits; std::memcpy(&bits, &lit->v, 8);
            e.mov_reg_imm64(Reg::rax, int64_t(bits));
            e.byte(0x66); e.byte(0x48); e.byte(0x0F); e.byte(0x6E); e.byte(0xC0); // movq xmm0,rax
        } else {
            float fv = float(lit->v); uint32_t bits; std::memcpy(&bits, &fv, 4);
            e.mov_reg_imm64(Reg::rax, int64_t(int32_t(bits)));
            e.byte(0x66); e.byte(0x0F); e.byte(0x6E); e.byte(0xC0); // movd xmm0,eax
        }
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
            } else if (t->prim == Prim::F64) { e.movsd_xmm_mem(Xmm::xmm0, Reg::rbp, it->second); }
            else if (t->is_float()) { e.movss_xmm_mem(Xmm::xmm0, Reg::rbp, it->second); }
            else { load_rbp_to_rax(*this, it->second); normalize_rax(t); }
            return;
        }
        // global? (v1.0: prefer ctx.globals_index/types threaded through CodeGenCtx —
        // avoids the process-wide g_globals_for_codegen pointer which races under
        // parallel compile. Fall back to it for backward compat if ctx fields null.)
        // c3: prefer ctx.globals_offsets (the TYPED byte offset) over the legacy
        // flat index*8 — aggregate globals land at non-8-byte slots. Fall back to
        // index*8 when offsets is null (scalar-only hosts keep working).
        const auto* gidx = ctx.globals_index ? ctx.globals_index : (g_globals_for_codegen ? &g_globals_for_codegen->index : nullptr);
        const auto* goffsets = ctx.globals_offsets ? ctx.globals_offsets : (g_globals_for_codegen ? &g_globals_for_codegen->offsets : nullptr);
        const auto* gtypes = ctx.globals_types ? ctx.globals_types : (g_globals_for_codegen ? &g_globals_for_codegen->types : nullptr);
        const int64_t gbase = ctx.globals_base;
        if (gidx && gtypes) {
            // resolve the per-global byte offset: typed offset if available,
            // else the legacy flat index*8.
            int32_t off = 0;
            bool found = false;
            if (goffsets) {
                auto oit = goffsets->find(id->name);
                if (oit != goffsets->end()) { off = int32_t(oit->second); found = true; }
            }
            if (!found) {
                auto gi = gidx->find(id->name);
                if (gi != gidx->end()) { off = int32_t(gi->second) * 8; found = true; }
            }
            if (found) {
                auto tit = gtypes->find(id->name);
                const Type* t = (tit != gtypes->end()) ? tit->second : nullptr;
                if (t && t->is_slice) {
                    // slice global read: load {ptr,len} (16 bytes) at [base+off],
                    // then turn the RELATIVE ptr (a 0-based offset within the
                    // block, baked at load so the bytes round-trip through .em
                    // without loader fixup) into an ABSOLUTE address by adding
                    // globals_base. Mirrors the slice-local ABI {rax=ptr,rdx=len}.
                    // Sequence: rax=ptr; rdx=rax (save ptr); rax=len; rcx=rax
                    // (save len); rax=rdx (ptr); rax += base; rdx=rcx (len).
                    load_global_to_rax(*this, gbase, off);        // rax = relative ptr
                    e.mov_reg_reg(Reg::rdx, Reg::rax);             // rdx = ptr (saved)
                    load_global_to_rax(*this, gbase, off + 8);    // rax = len
                    e.mov_reg_reg(Reg::rcx, Reg::rax);             // rcx = len (saved)
                    e.mov_reg_reg(Reg::rax, Reg::rdx);             // rax = ptr
                    e.mov_reg_imm64_external(Reg::r11, AbsFixup::GlobalsBase);
                    e.add_reg_reg(Reg::rax, Reg::r11);             // rax = absolute ptr
                    e.mov_reg_reg(Reg::rdx, Reg::rcx);             // rdx = len
                } else {
                    load_global_to_rax(*this, gbase, off);
                    if (t && t->prim == Prim::F64) {
                        e.byte(0x66); e.byte(0x48); e.byte(0x0F); e.byte(0x6E); e.byte(0xC0); // movq xmm0,rax
                    } else if (t && t->is_float()) {
                        e.byte(0x66); e.byte(0x0F); e.byte(0x6E); e.byte(0xC0); // movd xmm0,eax
                    } else normalize_rax(t);
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
            // Two argument slots plus Win64 shadow space.
            e.sub_reg_imm32(Reg::rsp, 48);
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
            emit_counted_native_call(b->overload_fn, b->overload_name,
                                     &b->overload_ret, &b->overload_params,
                                     "operator overload");
            e.add_reg_imm32(Reg::rsp, 48);
            return;
        }
        const Type* lt = b->lhs->ty;
        // is_cmp is computed before the fold so the fold early return can use
        // it (H-§10-1): a folded integer result is normalized to the target
        // width exactly as the runtime path is at the end of this function,
        // so a folded i32 reaches the ABI boundary sign-normalized instead of
        // zero-extended. The fold only reads li->v/ri->v/b->op, never is_cmp.
        bool is_cmp = (b->op>=BinExpr::Op::Eq && b->op<=BinExpr::Op::Ge);
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
                // Add/Sub/Mul/Shl go through uint64_t first then bit_cast_i64
                // (L-§10-3): script-level integer overflow is expected to wrap
                // (edge_cases2.ember already exercises this at the runtime-add
                // level, where it's just a hardware `add` - no UB), but
                // `int64_t + int64_t` overflow is undefined in the C++ that's
                // doing the folding here. Unsigned overflow is well-defined
                // (wraps mod 2^64) and produces the identical bit pattern, so
                // computing in uint64_t and bit-casting (NOT an
                // implementation-defined `int64_t(uint64_t(...))` conversion)
                // keeps the fold's result identical to the runtime path's
                // while removing both the UB and the portability hazard from
                // the *host* computation.
                switch (b->op) {
                case BinExpr::Op::Add: result = bit_cast_i64(uint64_t(li->v) + uint64_t(ri->v)); break;
                case BinExpr::Op::Sub: result = bit_cast_i64(uint64_t(li->v) - uint64_t(ri->v)); break;
                case BinExpr::Op::Mul: result = bit_cast_i64(uint64_t(li->v) * uint64_t(ri->v)); break;
                case BinExpr::Op::And: result = li->v & ri->v; break;
                case BinExpr::Op::Or:  result = li->v | ri->v; break;
                case BinExpr::Op::Xor: result = li->v ^ ri->v; break;
                // x86 shl/sar mask the shift count to 0-63 for a 64-bit
                // operand; replicate that so folded and unfolded code agree.
                // Shl is the unsigned left shift (no sign behavior) bit_cast
                // back; Shr is an arithmetic right shift implemented as an
                // unsigned shift plus an explicit sign fill (L-§10-3): a
                // signed `int64_t >> count` of a negative value is
                // implementation-defined per [expr.shift], so we compute it
                // from the unsigned bit pattern and OR in the sign bits
                // ourselves, which is defined behavior and matches x64 sar.
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
                // H-§10-1: normalize the folded immediate to the target width
                // BEFORE returning, exactly as the runtime integer path does
                // at the end of this function. Without this a folded i32
                // (e.g. an enum i32 value shifted into the sign bit) reaches the
                // ABI boundary zero-extended instead of sign-normalized.
                if (folded) { e.mov_reg_imm64(Reg::rax, result); if (!is_cmp) normalize_rax(lt); return; }
            }
        }
        // Float expressions deliberately use the normal typed SSE path below;
        // avoiding a second constant implementation keeps f32/f64 width exact.
        bool is_logical = (b->op==BinExpr::Op::LAnd||b->op==BinExpr::Op::LOr);
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
            Label done = e.alloc_label();
            e.mov_reg_imm64(Reg::rax, 0); e.jcc(Cond::e, done);
            e.mov_reg_imm64(Reg::rax, 1); e.bind(done);
            return;
        }

        if (is_float) {
            bool f64 = lt->prim == Prim::F64;
            eval(*b->lhs); e.sub_reg_imm32(Reg::rsp, 8);
            if (f64) e.movsd_mem_xmm(Reg::rsp, 0, Xmm::xmm0); else e.movss_mem_xmm(Reg::rsp, 0, Xmm::xmm0);
            eval(*b->rhs);
            if (f64) e.movsd_xmm_mem(Xmm::xmm1, Reg::rsp, 0); else e.movss_xmm_mem(Xmm::xmm1, Reg::rsp, 0);
            e.add_reg_imm32(Reg::rsp, 8);
            switch (b->op) {
            case BinExpr::Op::Add: if(f64)e.addsd_xmm(Xmm::xmm0,Xmm::xmm1);else e.addss_xmm(Xmm::xmm0,Xmm::xmm1); break;
            case BinExpr::Op::Mul: if(f64)e.mulsd_xmm(Xmm::xmm0,Xmm::xmm1);else e.mulss_xmm(Xmm::xmm0,Xmm::xmm1); break;
            case BinExpr::Op::Sub: if(f64){e.subsd_xmm(Xmm::xmm1,Xmm::xmm0);e.movsd_xmm_xmm(Xmm::xmm0,Xmm::xmm1);}else{e.subss_xmm(Xmm::xmm1,Xmm::xmm0);e.movss_xmm_xmm(Xmm::xmm0,Xmm::xmm1);} break;
            case BinExpr::Op::Div: if(f64){e.divsd_xmm(Xmm::xmm1,Xmm::xmm0);e.movsd_xmm_xmm(Xmm::xmm0,Xmm::xmm1);}else{e.divss_xmm(Xmm::xmm1,Xmm::xmm0);e.movss_xmm_xmm(Xmm::xmm0,Xmm::xmm1);} break;
            case BinExpr::Op::Eq: case BinExpr::Op::Neq:
            case BinExpr::Op::Lt: case BinExpr::Op::Le:
            case BinExpr::Op::Gt: case BinExpr::Op::Ge: {
                if (f64) e.ucomisd_xmm(Xmm::xmm1, Xmm::xmm0); else e.ucomiss_xmm(Xmm::xmm1, Xmm::xmm0);
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
            // Stage 1 local regalloc (design W1): keep lhs in a register across
            // the RHS eval instead of `push rax; ...; pop rax`. Use the VOLATILE
            // scratch r10 (NOT a callee-saved register) so there is NO prologue
            // save/restore tax on any function — zero overhead when the RHS
            // happens to contain a call (that case falls back to push/pop below).
            //
            // The win applies to the no-call-in-RHS case (loop bodies, simple
            // arithmetic — the loop_overhead `s+i` and the arithmetic parts of
            // every path). r10 is free by default: MBA uses rdx, the call-target
            // guard + dispatch use r11, opaque junk (off by default) is the only
            // other r10 user and it runs in the OP (after the holding reg is
            // restored), not during the RHS eval.
            //
            // Two fallbacks to the pre-Stage-1 push/pop path (correctness):
            //   (1) the RHS clobbers r10 — a CallExpr (r10 volatile), a SIGNED
            //       Div/Mod (emit_integer_divmod's INT64_MIN check uses r10), or a
            //       global aggregate access (the global-base load uses r10). The
            //       stack must hold lhs across those;
            //   (2) r10 is already holding a nested BinExpr's lhs — a single
            //       holding register can't nest, so the inner one uses the stack.
            // r10_holding_lhs is cleared after the op, so it is free for the next
            // statement's outermost r10-safe BinExpr.
            bool used_r10 = false;
            bool rhs_clobbers = expr_clobbers_r10(*b->rhs);
            if (enable_local_regalloc && !r10_holding_lhs && !rhs_clobbers &&
                !(obf.mba || obf.opaque)) {
                e.mov_reg_reg(Reg::r10, Reg::rax);   // r10 = lhs (no stack spill)
                r10_holding_lhs = true;
                used_r10 = true;
            } else {
                e.push(Reg::rax);                    // lhs spills to stack (pre-Stage-1)
            }
            eval(*b->rhs);
            e.mov_reg_reg(Reg::rcx, Reg::rax);       // rcx = rhs
            if (used_r10) {
                e.mov_reg_reg(Reg::rax, Reg::r10);   // rax = lhs (no stack reload)
                r10_holding_lhs = false;
            } else {
                e.pop(Reg::rax);                     // rax = lhs (pre-Stage-1)
            }
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
            case BinExpr::Op::Div: emit_integer_divmod(false, lt && lt->is_uint()); break;
            case BinExpr::Op::Mod: emit_integer_divmod(true, lt && lt->is_uint()); break;
            case BinExpr::Op::Shl: e.byte(0x48); e.byte(0xD3); e.byte(0xE0); break; // shl rax,cl
            case BinExpr::Op::Shr: e.byte(0x48); e.byte(0xD3); e.byte((lt&&lt->is_uint())?0xE8:0xF8); break;
            case BinExpr::Op::Eq: case BinExpr::Op::Neq:
            case BinExpr::Op::Lt: case BinExpr::Op::Le:
            case BinExpr::Op::Gt: case BinExpr::Op::Ge: {
                e.cmp_reg_reg(Reg::rax, Reg::rcx); // cmp lhs(rax), rhs(rcx)
                uint8_t cc = 0;
                switch (b->op) {
                case BinExpr::Op::Eq: cc = 0x4; break;  // sete
                case BinExpr::Op::Neq: cc = 0x5; break; // setne
                case BinExpr::Op::Lt: cc = (lt&&lt->is_uint()) ? 0x2 : 0xC; break;
                case BinExpr::Op::Le: cc = (lt&&lt->is_uint()) ? 0x6 : 0xE; break;
                case BinExpr::Op::Gt: cc = (lt&&lt->is_uint()) ? 0x7 : 0xF; break;
                case BinExpr::Op::Ge: cc = (lt&&lt->is_uint()) ? 0x3 : 0xD; break;
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
            if (!is_cmp) normalize_rax(lt);
            return;
        }
        // float path (clean):
        eval(*b->lhs);
        // Preserve the temporary through any trapping RHS evaluation while
        // keeping the emitter's stack parity exact.
        e.sub_reg_imm32(Reg::rsp, 8);
        e.movss_mem_xmm(Reg::rsp, 0, Xmm::xmm0);
        eval(*b->rhs);
        e.movss_xmm_mem(Xmm::xmm1, Reg::rsp, 0);
        e.add_reg_imm32(Reg::rsp, 8);
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
    // v1.0 Tier 2 (docs/planning/plan_FUNCTION_REFS.md §4.2): `&fn_name` is a compile-time
    // reification — sema baked the slot as an i64 literal. Emit mov rax, imm64(slot).
    // The first-class-ness is at the CALL site (handle(args)), not here: this is
    // just a constant load. (The advisor flagged making sure this eval case exists
    // for the `&fib(42)` direct-call-without-let form.)
    if (auto* h = dynamic_cast<const FnHandleExpr*>(&ex)) {
        e.mov_reg_imm64(Reg::rax, int64_t(h->slot));
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
                if (u->operand->ty->prim == Prim::F64) {
                    e.mov_reg_imm64(Reg::rax, INT64_MIN);
                    e.byte(0x66); e.byte(0x48); e.byte(0x0F); e.byte(0x6E); e.byte(0xC8); // movq xmm1,rax
                    e.byte(0x66); e.byte(0x0F); e.byte(0x57); e.byte(0xC1); // xorpd
                } else {
                    e.byte(0xB8); e.imm32(int32_t(0x80000000));
                    e.byte(0x66); e.byte(0x0F); e.byte(0x6E); e.byte(0xC8);
                    e.byte(0x0F); e.byte(0x57); e.byte(0xC1);
                }
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
        const Type* from = c->operand->ty;
        const Type* to = c->to.get();
        const bool plain_from_int = from && from->is_int() && !from->is_fn_handle && from->struct_name.empty();
        const bool plain_to_int = to && to->is_int() && !to->is_fn_handle && to->struct_name.empty();
        const bool by_value_aggregate = from && (from->array_len > 0 ||
            (!from->struct_name.empty() && ctx.structs && ctx.structs->count(from->struct_name) != 0));

        if (from && to && from->same(*to) && !by_value_aggregate) {
            return; // same-type scalar, slice, or nominal handle
        }
        if (plain_from_int && plain_to_int) {
            normalize_rax(to); return;
        }
        if (from && to && from->is_float() && to->is_float()) {
            if (from->prim == Prim::F32 && to->prim == Prim::F64)
                { e.byte(0xF3); e.byte(0x0F); e.byte(0x5A); e.byte(0xC0); } // cvtss2sd
            else if (from->prim == Prim::F64 && to->prim == Prim::F32)
                { e.byte(0xF2); e.byte(0x0F); e.byte(0x5A); e.byte(0xC0); } // cvtsd2ss
            return;
        }
        if (plain_from_int && !from->is_uint() && to && to->is_float()) {
            normalize_rax(from);
            e.byte(to->prim == Prim::F64 ? 0xF2 : 0xF3); e.byte(0x48); e.byte(0x0F); e.byte(0x2A); e.byte(0xC0);
            return;
        }
        if (from && from->is_float() && plain_to_int && !to->is_uint()) {
            e.byte(from->prim == Prim::F64 ? 0xF2 : 0xF3); e.byte(0x48); e.byte(0x0F); e.byte(0x2C); e.byte(0xC0);
            normalize_rax(to); return;
        }

        // Sema's explicit cast matrix makes this unreachable.  Keep a hard
        // generated-code failure rather than ever treating unknown casts as
        // representation-preserving bitcasts.
        assert(false && "invalid cast reached codegen");
        emit_trap(int(TrapReason::IllegalInstruction), "internal: invalid cast reached codegen");
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
                bool f64 = tt->prim == Prim::F64;
                eval(*a->target); e.sub_reg_imm32(Reg::rsp, 8);
                if(f64)e.movsd_mem_xmm(Reg::rsp,0,Xmm::xmm0);else e.movss_mem_xmm(Reg::rsp,0,Xmm::xmm0);
                eval(*a->value);
                if(f64)e.movsd_xmm_mem(Xmm::xmm1,Reg::rsp,0);else e.movss_xmm_mem(Xmm::xmm1,Reg::rsp,0);
                e.add_reg_imm32(Reg::rsp,8);
                switch (*a->compound) {
                case BinExpr::Op::Add: if(f64)e.addsd_xmm(Xmm::xmm0,Xmm::xmm1);else e.addss_xmm(Xmm::xmm0,Xmm::xmm1); break;
                case BinExpr::Op::Mul: if(f64)e.mulsd_xmm(Xmm::xmm0,Xmm::xmm1);else e.mulss_xmm(Xmm::xmm0,Xmm::xmm1); break;
                case BinExpr::Op::Sub: if(f64){e.subsd_xmm(Xmm::xmm1,Xmm::xmm0);e.movsd_xmm_xmm(Xmm::xmm0,Xmm::xmm1);}else{e.subss_xmm(Xmm::xmm1,Xmm::xmm0);e.movss_xmm_xmm(Xmm::xmm0,Xmm::xmm1);} break;
                case BinExpr::Op::Div: if(f64){e.divsd_xmm(Xmm::xmm1,Xmm::xmm0);e.movsd_xmm_xmm(Xmm::xmm0,Xmm::xmm1);}else{e.divss_xmm(Xmm::xmm1,Xmm::xmm0);e.movss_xmm_xmm(Xmm::xmm0,Xmm::xmm1);} break;
                default: break;
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
                case BinExpr::Op::Div: emit_integer_divmod(false, tt && tt->is_uint()); break;
                case BinExpr::Op::Mod: emit_integer_divmod(true, tt && tt->is_uint()); break;
                case BinExpr::Op::Shl: e.byte(0x48); e.byte(0xD3); e.byte(0xE0); break;
                case BinExpr::Op::Shr: e.byte(0x48); e.byte(0xD3); e.byte((tt&&tt->is_uint())?0xE8:0xF8); break;
                default: break;
                }
                normalize_rax(tt);
            }
        } else {
            // struct-local/global REASSIGNMENT (`s = mk();` where mk() returns a
            // struct and s is a struct-typed local/global): the RHS is a
            // struct-returning CallExpr, which uses the Win64 hidden-pointer
            // ABI (rcx = &return_buffer; the callee writes the struct there).
            // The generic eval() CallExpr path has no such ABI - it would call
            // mk() with rcx unset (garbage), so mk() writes its result to an
            // unmapped address -> segfault, then stores a stray rax into s.
            // Mirror the LetStmt-init struct-return path (see compile_stmt):
            // load the TARGET's address into rax as the hidden word-0 pointer,
            // eval_struct_returning_call writes the struct directly into the
            // target slot - no register result, no rax store, no postfix.
            if (auto* call = dynamic_cast<const CallExpr*>(a->value.get())) {
                const Type* ct = call->ty;
                if (ct && !ct->struct_name.empty() && ctx.structs && ctx.structs->count(ct->struct_name)) {
                    if (auto* id = dynamic_cast<Ident*>(a->target.get())) {
                        auto it = locals.find(id->name);
                        if (it != locals.end()) {
                            // struct local target: hidden ptr = &local.
                            e.byte(0x48); e.byte(0x8D); e.byte(0x85); e.imm32(it->second); // lea rax, [rbp+off]
                            eval_struct_returning_call(*call);
                            return;
                        }
                        // struct GLOBAL target: hidden ptr = globals_base + goff.
                        const auto* gidx = ctx.globals_index ? ctx.globals_index : (g_globals_for_codegen ? &g_globals_for_codegen->index : nullptr);
                        const auto* goffsets = ctx.globals_offsets ? ctx.globals_offsets : (g_globals_for_codegen ? &g_globals_for_codegen->offsets : nullptr);
                        const auto* gtypes = ctx.globals_types ? ctx.globals_types : (g_globals_for_codegen ? &g_globals_for_codegen->types : nullptr);
                        if (gidx && gtypes) {
                            int32_t goff = 0; bool found = false;
                            if (goffsets) { auto oit = goffsets->find(id->name); if (oit != goffsets->end()) { goff = int32_t(oit->second); found = true; } }
                            if (!found) { auto gi = gidx->find(id->name); if (gi != gidx->end()) { goff = int32_t(gi->second) * 8; found = true; } }
                            if (found) {
                                e.mov_reg_imm64_external(Reg::r11, AbsFixup::GlobalsBase);
                                e.byte(0x49); e.byte(0x8D); e.byte(0x83); e.imm32(goff); // lea rax, [r11+goff]
                                eval_struct_returning_call(*call);
                                return;
                            }
                        }
                    }
                }
            }
            eval(*a->value);
        }
        // Postfix forms return the pre-update value.  The parser's minimal
        // flag is only produced for ++/--, whose target is evaluated once.
        if (a->postfix) {
            if (a->target->ty && a->target->ty->is_float()) {
                bool f64=a->target->ty->prim==Prim::F64;
                if(f64)e.movsd_xmm_xmm(Xmm::xmm2,Xmm::xmm0);else e.movss_xmm_xmm(Xmm::xmm2,Xmm::xmm0);
            } else {
                e.push(Reg::rax); // updated value survives address/store lowering
            }
        }
        // store rax (or xmm0, or rax/rdx for a slice) to target
        if (auto* id = dynamic_cast<Ident*>(a->target.get())) {
            auto it = locals.find(id->name);
            if (it != locals.end()) {
                const Type* t = local_types[id->name];
                if (t->is_slice) {
                    e.store_reg_mem(Reg::rbp, it->second, Reg::rax);
                    e.store_reg_mem(Reg::rbp, it->second + 8, Reg::rdx);
                } else if (t->is_float()) store_xmm0_to_rbp(*this, it->second, t);
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
                    normalize_rax(t);
                    if (active_pin && active_pin->name == id->name)
                        e.mov_reg_reg(active_pin->reg, Reg::rax);
                    store_rax_to_rbp(*this, it->second);
                }
            } else {
                // v1.0: prefer ctx.globals_index/types (parallel-compile-safe).
                // c3: prefer ctx.globals_offsets (typed byte offset) over index*8.
                const auto* gidx = ctx.globals_index ? ctx.globals_index : (g_globals_for_codegen ? &g_globals_for_codegen->index : nullptr);
                const auto* goffsets = ctx.globals_offsets ? ctx.globals_offsets : (g_globals_for_codegen ? &g_globals_for_codegen->offsets : nullptr);
                const auto* gtypes = ctx.globals_types ? ctx.globals_types : (g_globals_for_codegen ? &g_globals_for_codegen->types : nullptr);
                if (gidx && gtypes) {
                    int32_t off = 0; bool found = false;
                    if (goffsets) {
                        auto oit = goffsets->find(id->name);
                        if (oit != goffsets->end()) { off = int32_t(oit->second); found = true; }
                    }
                    if (!found) {
                        auto gi = gidx->find(id->name);
                        if (gi != gidx->end()) { off = int32_t(gi->second) * 8; found = true; }
                    }
                    if (found) {
                        auto tit = gtypes->find(id->name);
                        const Type* gt = (tit != gtypes->end()) ? tit->second : nullptr;
                        if (gt && gt->is_float()) store_xmm0_to_global(*this, ctx.globals_base, off, gt);
                        else { normalize_rax(gt); store_rax_to_global(*this, ctx.globals_base, off); }
                    }
                }
            }
        } else if (auto* ixt = dynamic_cast<IndexExpr*>(a->target.get())) {
            // buf[i] = value: value was already eval'd into rax/xmm0 above;
            // stash it, compute the element address, then store. Mirrors the
            // IndexExpr *load* path in eval() (element addressing).
            const Type* bt = ixt->base->ty;
            const Type* elem = bt && bt->elem ? bt->elem.get() : nullptr;
            int32_t width = value_bytes(elem, ctx.structs);
            bool is_f32 = elem && elem->prim == Prim::F32 && !elem->is_slice;
            bool is_f64 = elem && elem->prim == Prim::F64 && !elem->is_slice;
            if (is_f32 || is_f64) {
                // This temporary remains live across the bounds trap, so use
                // the tracked rsp helper: emit_trap must see the taken edge's
                // real parity and provide the required eight-byte call padding.
                e.sub_reg_imm32(Reg::rsp, 8);
                if (is_f64) e.movsd_mem_xmm(Reg::rsp, 0, Xmm::xmm0);
                else e.movss_mem_xmm(Reg::rsp, 0, Xmm::xmm0);
            } else e.push(Reg::rax);
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
                        if (is_f32 || is_f64) {
                            if (is_f64) e.movsd_xmm_mem(Xmm::xmm1, Reg::rsp, 0);
                            else e.movss_xmm_mem(Xmm::xmm1, Reg::rsp, 0);
                            e.add_reg_imm32(Reg::rsp, 8);
                            if (is_f64) e.movsd_mem_xmm(Reg::r11, base_off, Xmm::xmm1);
                            else e.movss_mem_xmm(Reg::r11, base_off, Xmm::xmm1);
                        } else {
                            e.pop(Reg::rax);
                            e.store_rax_elem(Reg::r11, base_off, width);
                        }
                        return;
                    }
                }
            }
            // base wasn't a recognized local: drop the stashed value cleanly
            if (is_f32 || is_f64) e.add_reg_imm32(Reg::rsp, 8);
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
                            if (ft->prim == Prim::F64 && !ft->is_slice) e.movsd_mem_xmm(Reg::rbp, addr_off, Xmm::xmm0);
                            else if (ft->prim == Prim::F32 && !ft->is_slice) e.movss_mem_xmm(Reg::rbp, addr_off, Xmm::xmm0);
                            else if (ft->is_slice) {
                                e.store_reg_mem(Reg::rbp, addr_off, Reg::rax);
                                e.store_reg_mem(Reg::rbp, addr_off + 8, Reg::rdx);
                            } else if (ft->struct_name.empty() || !ctx.structs || !ctx.structs->count(ft->struct_name)) {
                                e.store_rax_elem(Reg::rbp, addr_off, value_bytes(ft, ctx.structs));
                            }
                        }
                    }
                }
            }
        }
        if (a->postfix) {
            // The updated value is now committed; derive the old result from
            // the new value without evaluating target/value again.
            const Type* t=a->target->ty;
            if (t && t->is_float()) {
                bool f64=t->prim==Prim::F64;
                if(f64)e.movsd_xmm_xmm(Xmm::xmm0,Xmm::xmm2);else e.movss_xmm_xmm(Xmm::xmm0,Xmm::xmm2);
                double one=1.0; uint64_t db; float onef=1.0f; uint32_t fb;
                if(f64){std::memcpy(&db,&one,8);e.mov_reg_imm64(Reg::rax,int64_t(db));e.byte(0x66);e.byte(0x48);e.byte(0x0F);e.byte(0x6E);e.byte(0xC8);}
                else{std::memcpy(&fb,&onef,4);e.mov_reg_imm64(Reg::rax,int64_t(fb));e.byte(0x66);e.byte(0x0F);e.byte(0x6E);e.byte(0xC8);}
                if(*a->compound==BinExpr::Op::Add){if(f64)e.subsd_xmm(Xmm::xmm0,Xmm::xmm1);else e.subss_xmm(Xmm::xmm0,Xmm::xmm1);}
                else {if(f64)e.addsd_xmm(Xmm::xmm0,Xmm::xmm1);else e.addss_xmm(Xmm::xmm0,Xmm::xmm1);}
            } else {
                e.pop(Reg::rax);
                if(*a->compound==BinExpr::Op::Add)e.sub_reg_imm32(Reg::rax,1);else e.add_reg_imm32(Reg::rax,1);
                normalize_rax(t);
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
        // v1.0 Tier 2 indirect call (plan §5.1): reserve a scratch word for the
        // runtime handle so it survives the arg stash + emit_depth_check (which
        // clobbers rax in non-B1 mode — the handle reload comes AFTER it, per the
        // advisor's correction). The handle word lives at [rsp+stash_size], past
        // all arg slots [0..n*8-1]; shadow space shifts to stash_size+8, outgoing
        // args to stash_size+8+32+...
        int32_t handle_word = c->is_indirect ? 8 : 0;
        int32_t outgoing = std::max(0, n - 4) * 8;
        int32_t total = round16(stash_size + handle_word + 32 + outgoing);
        // ---- indirect call ONLY: eval the target, run the provenance guard ----
        // BEFORE `sub rsp` (rcx/r11 are free here; the guard clobbers them but no
        // args are in regs yet). rax = the validated handle on return.
        if (c->is_indirect) {
            eval(*c->indirect_target);   // rax = handle (a &fn literal, a fn-typed var, or a native return)
            emit_call_target_guard();    // validates rax against the allowlist; traps if bad; rax survives
        }
        e.sub_reg_imm32(Reg::rsp, total);
        // ---- indirect call: stash the handle at [rsp+stash_size] BEFORE the arg
        // eval loop (args don't touch that offset — it's past all arg slots) ----
        if (c->is_indirect) {
            e.byte(0x48); e.byte(0x89); e.byte(0x84); e.byte(0x24); e.imm32(stash_size);  // mov [rsp+stash_size], rax
        }
        // eval + stash each operand to its word slot(s)
        for (auto& op : ops) {
            int32_t off = op.slot0 * 8;
            if (op.is_struct) {
                // struct-by-value: sema (check_struct_arg_shape) permits a bare
                // local Ident, a struct literal of the param struct, or a call
                // returning the param struct. stash_struct_arg materializes the
                // literal/call forms into a fresh per-arg temp frame slot first
                // (sized into the frame by count_struct_temps_block), then
                // copies the EXACT byte extent (not a rounded-up word count -
                // see copy_bytes) into the stash region, rather than eval()'ing
                // a single register value (a multi-word struct has no such
                // single-register form).
                auto sit = (op.ty && !op.ty->struct_name.empty() && ctx.structs) ? ctx.structs->find(op.ty->struct_name) : ctx.structs->end();
                if (sit != ctx.structs->end())
                    stash_struct_arg(op.e, op.ty, off, sit->second);
            } else {
                eval(*op.e);
                if (op.words == 2) {
                    // slice: rax=ptr, rdx=len (see eval()'s Ident/StringLit cases)
                    e.store_reg_mem(Reg::rsp, off, Reg::rax);
                    e.store_reg_mem(Reg::rsp, off + 8, Reg::rdx);
                } else if (op.ty && op.ty->is_float()) {
                    if(op.ty->prim==Prim::F64)e.movsd_mem_xmm(Reg::rsp,off,Xmm::xmm0);else e.movss_mem_xmm(Reg::rsp,off,Xmm::xmm0);
                } else {
                    e.byte(0x48); e.byte(0x89); e.byte(0x84); e.byte(0x24); e.imm32(off);
                }
            }
        }
        // per-word float width (a slice's/struct's words are always integer)
        std::vector<bool> word_is_float(size_t(n), false), word_is_f64(size_t(n), false);
        for (auto& op : ops) if (op.words==1 && !op.is_struct && op.ty && op.ty->is_float()) {
            word_is_float[size_t(op.slot0)]=true; word_is_f64[size_t(op.slot0)]=op.ty->prim==Prim::F64;
        }
        // place words 0..3 into regs — with Win64 hidden-pointer ABI for
        // >8-byte struct args to NATIVE calls. The Win64 ABI passes a >8-byte
        // struct by hidden pointer (caller allocates, passes ptr in the arg
        // register). The current codegen stashes the struct bytes at [rsp+slot*8];
        // for native calls with >8-byte struct args, we load a POINTER to the
        // stash location into the register instead of the struct's words, and
        // shift subsequent args to account for the struct collapsing to 1 slot.
        static const Reg int_regs[4] = {Reg::rcx, Reg::rdx, Reg::r8, Reg::r9};
        static const Xmm flt_regs[4] = {Xmm::xmm0, Xmm::xmm1, Xmm::xmm2, Xmm::xmm3};
        // Build the physical slot mapping for native calls with >8-byte structs.
        // For non-native calls (script-to-script), ember's private ABI passes
        // words directly (no hidden pointer), so the mapping is identity.
        struct PhysSlot { int logical_word; bool is_struct_ptr; int32_t struct_byte_off; };
        std::vector<PhysSlot> phys_slots;
        if (c->is_native) {
            int phys_pos = 0;
            for (auto& op : ops) {
                int32_t struct_size = 0;
                if (op.is_struct && op.ty && ctx.structs) {
                    auto it = ctx.structs->find(op.ty->struct_name);
                    if (it != ctx.structs->end()) struct_size = it->second.size;
                }
                if (struct_size > 8) {
                    // >8-byte struct: collapses to 1 physical slot (pointer).
                    phys_slots.push_back({phys_pos, true, op.slot0 * 8});
                    phys_pos++;
                    // Skip the remaining words of this struct (they're inside
                    // the struct, not separate arg slots).
                } else {
                    for (int w = 0; w < op.words; ++w) {
                        phys_slots.push_back({phys_pos, false, 0});
                        phys_pos++;
                    }
                }
            }
        } else {
            // Non-native: identity mapping (each word is one physical slot).
            for (int w = 0; w < n; ++w)
                phys_slots.push_back({w, false, 0});
        }
        int n_phys = int(phys_slots.size());
        // Load physical slots 0..3 into registers.
        for (int p = 0; p < n_phys && p < 4; ++p) {
            const auto& ps = phys_slots[size_t(p)];
            if (ps.is_struct_ptr) {
                // Load a pointer to the struct's stash location into the register.
                // lea reg, [rsp+disp] via mov+add (no simple lea_reg_mem in the emitter).
                e.mov_reg_reg(int_regs[p], Reg::rsp);
                e.add_reg_imm32(int_regs[p], ps.struct_byte_off);
            } else {
                int w = ps.logical_word;
                int32_t off = w * 8;
                if (word_is_float[size_t(w)]) {
                    if(word_is_f64[size_t(w)])e.movsd_xmm_mem(flt_regs[p],Reg::rsp,off);else e.movss_xmm_mem(flt_regs[p], Reg::rsp, off);
                } else {
                    e.load_reg_mem(int_regs[p], Reg::rsp, off);
                }
            }
        }
        // Physical slots 4+ -> outgoing stack args.
        for (int p = 4; p < n_phys; ++p) {
            const auto& ps = phys_slots[size_t(p)];
            int32_t dst = stash_size + handle_word + 32 + (p - 4) * 8;
            if (ps.is_struct_ptr) {
                e.mov_reg_reg(Reg::rax, Reg::rsp);
                e.add_reg_imm32(Reg::rax, ps.struct_byte_off);
                e.store_reg_mem(Reg::rsp, dst, Reg::rax);
            } else {
                int32_t src = ps.logical_word * 8;
                e.load_reg_mem(Reg::rax, Reg::rsp, src);
                e.store_reg_mem(Reg::rsp, dst, Reg::rax);
            }
        }
        if (c->is_native) {
            // mov rax, native target; call rax, with combined depth accounting
            emit_counted_named_native(c->native_fn, c->native_binding_name, "native call");
        } else if (!c->module_alias.empty()) {
            // v0.5 cross-module call (mod::fn): kind-2 registry-hop sequence.
            emit_depth_check();
            emit_cross_module_call(*c);
            emit_depth_leave();
        } else if (c->is_indirect) {
            if (non_serializable_reason.empty())
                non_serializable_reason = "function-reference call requires process-local allowlist storage";
            // v1.0 Tier 2 first-class call (docs/planning/plan_FUNCTION_REFS.md §5.1): dispatch
            // through the runtime handle. emit_depth_check may clobber rax
            // (non-B1 mode), so RELOAD the handle from [rsp+stash_size] AFTER it
            // (the advisor's ordering correction), then lea+load+call the slot.
            emit_depth_check();                        // v0.4 stack-depth guard (SAFETY §4)
            e.load_reg_mem(Reg::rax, Reg::rsp, stash_size);  // reload handle AFTER depth check
            e.mov_reg_imm64_external(Reg::r11, AbsFixup::DispatchTableBase);
            e.lea_reg_mem_sib(Reg::r11, Reg::r11, Reg::rax, 3);  // lea r11, [r11 + rax*8]
            e.load_reg_mem(Reg::r11, Reg::r11, 0);               // mov r11, [r11] (load the entry)
            e.call_reg(Reg::r11);                                  // call r11
            emit_depth_leave();
        } else {
            // call [dispatch_base + slot*8] - dispatch-table base is a
            // relocatable imm64 (docs/BUNDLING_AND_EM_MODULES.md Section 2.4): emit 8 zero
            // placeholders + a DispatchTableBase AbsFixup; compile_func fills
            // the placeholder with ctx.dispatch_base after emit (byte-identical
            // to the old raw mov_reg_imm64).
            emit_depth_check();                        // v0.4 stack-depth guard (SAFETY §4)
            e.mov_reg_imm64_external(Reg::r11, AbsFixup::DispatchTableBase);
            e.call_mem(Reg::r11, int32_t(c->script_slot) * 8);
            emit_depth_leave();
        }
        e.add_reg_imm32(Reg::rsp, total);
        // Normalize narrow integer returns at every call boundary.
        if (ex.ty && ex.ty->is_int()) normalize_rax(ex.ty);
        return;
    }
    if (auto* lit = dynamic_cast<const StringLit*>(&ex)) {
        const uint32_t string_off = append_rodata(lit->baked_ptr, size_t(lit->baked_len));
        // slice ABI: rax=ptr, rdx=len.
        // String encryption (default): if the string is XOR-encrypted in
        // rodata, decrypt INLINE into a compiler-hidden temp frame slot
        // (see alloc_str_temp / count_str_temps_block) and yield the slot's
        // address as the slice ptr. The plaintext is TRANSIENT - it lives only
        // in this stack frame for the expression's lifetime and is reclaimed
        // when the frame is torn down (rbp-relative, part of the frame). No
        // heap, no host native call, no leak: the encrypted form alone lives
        // in rodata, and raw strings never appear in the JIT'd executable
        // memory. The same path serves the implicit-to-`string` conversion
        // below (string_from_slice copies out of the frame slot into the host
        // string store, after which the slot's plaintext is dead). Registers:
        // r11 = enc source (volatile), r10 = dest (volatile), al = scratch
        // byte (volatile), then rax = dest ptr, rdx = len. No callee-saved
        // register is touched.
        if (lit->encrypted && lit->baked_key != 0) {
            // Allocate the temp frame slot at this use site. eval is called once
            // per AST node at compile time; the emitted XOR runs per runtime use
            // (e.g. per loop iteration). A literal used textually twice is two
            // separate StringLit nodes, so each gets its own slot (mirrors how
            // struct temps are allocated one-per-materialization-site). The
            // slot's bytes were pre-counted by count_str_temps_block into the
            // frame sizing pass, so alloc_local's next_local_off bump stays
            // within the already-sized frame.
            const int32_t slot_off = alloc_str_temp(lit->baked_len);
            const int64_t len = lit->baked_len;
            const uint8_t key = lit->baked_key;
            // r11 = enc source (rodata base + string_off)
            e.mov_reg_imm64_external(Reg::r11, AbsFixup::FunctionRodataBase, string_off);
            // r10 = rbp; sub r10, slot_off  -> r10 = frame slot address
            // (lea_reg_mem_sib only supports mod=00 [no disp], so mov+sub.)
            e.mov_reg_reg(Reg::r10, Reg::rbp);
            e.sub_reg_imm32(Reg::r10, -slot_off);  // slot_off is negative; -slot_off is positive
            // Inline byte XOR: for each byte i, mov al,[r11+i]; xor al,key; mov [r10+i],al.
            // Unrolled for len <= 256 (covers every in-tree literal; tightest code for
            // the common short-literal case); a runtime loop for longer literals.
            // Displacement encoding: i < 128 fits a signed disp8 (mod=01); i >= 128
            // needs a disp32 (mod=10) because disp8 is signed -128..127 (a raw 0x80
            // disp8 byte would read [r11 - 128], the wrong address). The modrm rm
            // field is r11's low 3 bits (011) for the load and r10's (010) for the
            // store; mod goes 01->10 when switching disp8->disp32.
            auto emit_byte_xor = [&](int64_t i) {
                bool disp32 = i >= 128;
                if (!disp32) {
                    // mov al, [r11 + disp8]  ->  0x41, 0x8A, 0x43, disp8
                    e.byte(0x41); e.byte(0x8A); e.byte(0x43); e.byte(uint8_t(i));
                } else {
                    // mov al, [r11 + disp32] ->  0x41, 0x8A, 0x83, disp32 (4 bytes LE)
                    e.byte(0x41); e.byte(0x8A); e.byte(0x83); e.imm32(int32_t(i));
                }
                // xor al, imm8          ->  0x34, key
                e.byte(0x34); e.byte(key);
                if (!disp32) {
                    // mov [r10 + disp8], al ->  0x41, 0x88, 0x42, disp8
                    e.byte(0x41); e.byte(0x88); e.byte(0x42); e.byte(uint8_t(i));
                } else {
                    // mov [r10 + disp32], al->  0x41, 0x88, 0x82, disp32
                    e.byte(0x41); e.byte(0x88); e.byte(0x82); e.imm32(int32_t(i));
                }
            };
            if (len <= 256) {
                for (int64_t i = 0; i < len; ++i) emit_byte_xor(i);
            } else {
                // Runtime loop: rcx=len (counter), r11=src (post-inc), r10=dst (post-inc), al scratch.
                // After the loop r10 has been incremented `len` times (points PAST the
                // slot), so the post-loop rax must RE-DERIVE the slot base from rbp
                // (NOT use r10). The unrolled path doesn't have this issue (it uses
                // [r10+i] displacements and never mutates r10).
                e.mov_reg_imm64(Reg::rcx, len);
                Label loop = e.alloc_label();
                e.bind(loop);
                // mov al, [r11]        ->  REX.B 0x41, 0x8A, modrm(11,000,011)=0x03
                e.byte(0x41); e.byte(0x8A); e.byte(0x03);
                // xor al, key          ->  0x34, key
                e.byte(0x34); e.byte(key);
                // mov [r10], al        ->  REX.B 0x41, 0x88, modrm(11,000,010)=0x02
                e.byte(0x41); e.byte(0x88); e.byte(0x02);
                // inc r11              ->  REX.WB 0x49, 0xFF, modrm(11,000,011)=0xC3
                e.byte(0x49); e.byte(0xFF); e.byte(0xC3);
                // inc r10              ->  REX.WB 0x49, 0xFF, modrm(11,000,010)=0xC2
                e.byte(0x49); e.byte(0xFF); e.byte(0xC2);
                // dec rcx              ->  REX.W 0x48, 0xFF, modrm(11,001,001)=0xC9
                e.byte(0x48); e.byte(0xFF); e.byte(0xC9);
                // cmp rcx, 0           ->  REX.W 0x48, 0x83, 0xF9, 0x00
                e.byte(0x48); e.byte(0x83); e.byte(0xF9); e.byte(0x00);
                // ember while-loop pattern: conditional FORWARD exit (je done) plus
                // an UNCONDITIONAL back-edge (jmp loop). jcc as a direct back-branch
                // is never used elsewhere in the codebase (all back-edges are jmp).
                Label done = e.alloc_label();
                e.jcc(Cond::e, done);   // rcx == 0 -> exit (forward branch)
                e.jmp(loop);            // rcx != 0 -> iterate (unconditional back)
                e.bind(done);
            }
            // rax = dest ptr (the frame slot BASE). For the unrolled path r10 was
            // never mutated, so rax = r10 is the base. For the runtime-loop path
            // r10 was incremented `len` times (points past the slot), so re-derive
            // the base from rbp instead of using the now-advanced r10.
            if (lit->encrypted && lit->baked_key != 0 && lit->baked_len > 256) {
                e.mov_reg_reg(Reg::rax, Reg::rbp);
                e.sub_reg_imm32(Reg::rax, -slot_off);  // rax = rbp - slot_off = slot base
            } else {
                e.mov_reg_reg(Reg::rax, Reg::r10);
            }
            e.mov_reg_imm64(Reg::rdx, len);
        } else {
            // unencrypted: raw pointer (backward compat / key=0)
            e.mov_reg_imm64_external(Reg::rax, AbsFixup::FunctionRodataBase, string_off);
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
            e.sub_reg_imm32(Reg::rsp, call_sz);
            emit_counted_named_native(lit->to_string_native_fn,
                                      lit->to_string_native_name,
                                      "string conversion native");
            e.add_reg_imm32(Reg::rsp, call_sz);
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
        int32_t width = value_bytes(elem, ctx.structs);
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
            } else {
                // c3: array/slice GLOBAL base. Resolve the global's typed byte
                // offset; for a fixed-array global set base_reg=r10 (holds
                // globals_base) + base_off=global_offset; for a slice global
                // eval the slice (yields {rax=absolute ptr, rdx=len}) and
                // reuse the slice-local path.
                const auto* gidx = ctx.globals_index ? ctx.globals_index : (g_globals_for_codegen ? &g_globals_for_codegen->index : nullptr);
                const auto* goffsets = ctx.globals_offsets ? ctx.globals_offsets : (g_globals_for_codegen ? &g_globals_for_codegen->offsets : nullptr);
                const auto* gtypes = ctx.globals_types ? ctx.globals_types : (g_globals_for_codegen ? &g_globals_for_codegen->types : nullptr);
                if (gidx && gtypes) {
                    int32_t goff = 0; bool found = false;
                    if (goffsets) { auto oit = goffsets->find(bid->name); if (oit != goffsets->end()) { goff = int32_t(oit->second); found = true; } }
                    if (!found) { auto gi = gidx->find(bid->name); if (gi != gidx->end()) { goff = int32_t(gi->second) * 8; found = true; } }
                    if (found) {
                        auto tit = gtypes->find(bid->name);
                        const Type* gt = (tit != gtypes->end()) ? tit->second : nullptr;
                        if (gt && gt->is_slice) {
                            eval(*ix->base);       // rax=absolute ptr, rdx=len
                            e.mov_reg_reg(Reg::r10, Reg::rax);
                            e.mov_reg_reg(Reg::r9, Reg::rdx);
                            base_reg = Reg::r10; base_off = 0; base_ready = true; is_slice_base = true;
                        } else if (gt && gt->array_len > 0) {
                            e.mov_reg_imm64_external(Reg::r10, AbsFixup::GlobalsBase);
                            base_reg = Reg::r10; base_off = goff; base_ready = true;
                        }
                    }
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
            bool is_f64 = elem && elem->prim == Prim::F64 && !elem->is_slice;
            bool is_signed = elem && (elem->prim==Prim::I8||elem->prim==Prim::I16||
                                       elem->prim==Prim::I32||elem->prim==Prim::I64);
            if (is_f64) e.movsd_xmm_mem(Xmm::xmm0, Reg::r11, base_off);
            else if (is_f32) e.movss_xmm_mem(Xmm::xmm0, Reg::r11, base_off);
            else e.load_elem_to_rax(Reg::r11, base_off, width, is_signed);
        }
        return;
    }
    if (auto* v = dynamic_cast<const ViewExpr*>(&ex)) {
        // arr[..]: fixed array T[N] -> slice T[] {ptr=&arr, len=N}. v1 scope:
        // base must be a bare local Ident (a fixed-array frame slot has a
        // real address to take; a general expression would need a temp) OR
        // a bare GLOBAL Ident of a fixed-array type (c3: the array lives in
        // the globals block, so its address is globals_base + global_offset).
        if (auto* bid = dynamic_cast<const Ident*>(v->base.get())) {
            auto it = locals.find(bid->name);
            if (it != locals.end() && local_types[bid->name]->array_len > 0) {
                const Type* lt = local_types[bid->name];
                // rax = &arr (lea rax, [rbp+off]); rdx = N
                e.byte(0x48); e.byte(0x8D); e.byte(0x85); e.imm32(it->second); // lea rax, [rbp+off]
                e.mov_reg_imm64(Reg::rdx, int64_t(lt->array_len));
            } else {
                // c3: GLOBAL fixed-array base. Resolve the global's typed byte
                // offset (globals_offsets if wired, else index*8) and emit
                // rax = globals_base + global_offset; rdx = array_len. Without
                // this branch the ViewExpr fell through and emitted nothing,
                // leaving rax/rdx as garbage so g[..] yielded a junk slice.
                const auto* gidx = ctx.globals_index ? ctx.globals_index : (g_globals_for_codegen ? &g_globals_for_codegen->index : nullptr);
                const auto* goffsets = ctx.globals_offsets ? ctx.globals_offsets : (g_globals_for_codegen ? &g_globals_for_codegen->offsets : nullptr);
                const auto* gtypes = ctx.globals_types ? ctx.globals_types : (g_globals_for_codegen ? &g_globals_for_codegen->types : nullptr);
                if (gidx && gtypes) {
                    int32_t goff = 0; bool found = false;
                    if (goffsets) { auto oit = goffsets->find(bid->name); if (oit != goffsets->end()) { goff = int32_t(oit->second); found = true; } }
                    if (!found) { auto gi = gidx->find(bid->name); if (gi != gidx->end()) { goff = int32_t(gi->second) * 8; found = true; } }
                    if (found) {
                        auto tit = gtypes->find(bid->name);
                        const Type* gt = (tit != gtypes->end()) ? tit->second : nullptr;
                        if (gt && gt->array_len > 0) {
                            // rax = globals_base + goff; rdx = N. Mirrors the
                            // IndexExpr global fixed-array base above (mov r11,
                            // GlobalsBase; add goff) plus a final mov rax,r11.
                            e.mov_reg_imm64_external(Reg::r11, AbsFixup::GlobalsBase);
                            if (goff != 0) {
                                e.mov_reg_imm64(Reg::rax, int64_t(goff));
                                e.add_reg_reg(Reg::r11, Reg::rax);
                            }
                            e.mov_reg_reg(Reg::rax, Reg::r11);
                            e.mov_reg_imm64(Reg::rdx, int64_t(gt->array_len));
                        }
                    }
                }
            }
        }
        return;
    }
    if (auto* fl = dynamic_cast<const FieldExpr*>(&ex)) {
        // struct field read (p.x). v1 scope: base must be a bare local Ident
        // or a global Ident of a registered struct type (c3: a struct global's
        // field is at [globals_base + global_offset + field_offset]).
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
                        if (ft->prim == Prim::F64 && !ft->is_slice) {
                            e.movsd_xmm_mem(Xmm::xmm0, Reg::rbp, addr_off);
                        } else if (ft->prim == Prim::F32 && !ft->is_slice) {
                            e.movss_xmm_mem(Xmm::xmm0, Reg::rbp, addr_off);
                        } else {
                            bool is_signed = ft->prim==Prim::I8||ft->prim==Prim::I16||
                                             ft->prim==Prim::I32||ft->prim==Prim::I64;
                            e.load_elem_to_rax(Reg::rbp, addr_off, value_bytes(ft, ctx.structs), is_signed);
                        }
                    }
                }
            } else if (ctx.structs) {
                // c3: struct GLOBAL field read. Resolve the global's typed byte
                // offset (globals_offsets if wired, else index*8), then read the
                // field at [globals_base + global_off + field_off].
                const auto* gidx = ctx.globals_index ? ctx.globals_index : (g_globals_for_codegen ? &g_globals_for_codegen->index : nullptr);
                const auto* goffsets = ctx.globals_offsets ? ctx.globals_offsets : (g_globals_for_codegen ? &g_globals_for_codegen->offsets : nullptr);
                const auto* gtypes = ctx.globals_types ? ctx.globals_types : (g_globals_for_codegen ? &g_globals_for_codegen->types : nullptr);
                if (gidx && gtypes) {
                    int32_t goff = 0; bool found = false;
                    if (goffsets) { auto oit = goffsets->find(bid->name); if (oit != goffsets->end()) { goff = int32_t(oit->second); found = true; } }
                    if (!found) { auto gi = gidx->find(bid->name); if (gi != gidx->end()) { goff = int32_t(gi->second) * 8; found = true; } }
                    if (found) {
                        auto tit = gtypes->find(bid->name);
                        const Type* bt = (tit != gtypes->end()) ? tit->second : nullptr;
                        auto sit = bt && !bt->struct_name.empty() ? ctx.structs->find(bt->struct_name) : ctx.structs->end();
                        if (sit != ctx.structs->end()) {
                            auto fit = sit->second.fields.find(fl->field);
                            if (fit != sit->second.fields.end()) {
                                int32_t addr_off = goff + fit->second.offset;
                                const Type* ft = fit->second.ty;
                                // load from [globals_base + addr_off]: mov r11,<reloc>;
                                // then movss/movsd/load_elem from [r11+addr_off].
                                e.mov_reg_imm64_external(Reg::r11, AbsFixup::GlobalsBase);
                                if (ft->prim == Prim::F64 && !ft->is_slice) {
                                    e.movsd_xmm_mem(Xmm::xmm0, Reg::r11, addr_off);
                                } else if (ft->prim == Prim::F32 && !ft->is_slice) {
                                    e.movss_xmm_mem(Xmm::xmm0, Reg::r11, addr_off);
                                } else {
                                    bool is_signed = ft->prim==Prim::I8||ft->prim==Prim::I16||
                                                     ft->prim==Prim::I32||ft->prim==Prim::I64;
                                    e.load_elem_to_rax(Reg::r11, addr_off, value_bytes(ft, ctx.structs), is_signed);
                                }
                            }
                        }
                    }
                }
            }
        } else if (auto* ix = dynamic_cast<const IndexExpr*>(fl->base.get())) {
            // arr[i].field: base is an IndexExpr into an array/slice of
            // structs (e.g. `arr[0].a` on `P[3]` or `s[2].a` on `P[]`). The
            // bare-Ident base above only handles a local/global struct
            // variable's field; an indexed struct element needs the element
            // ADDRESS computed (base + index*struct_size) before the field
            // offset is added. Mirrors IndexExpr's base-resolution logic for
            // the four base kinds (local fixed array, local slice, global
            // fixed array, global slice), then loads the field at
            // [element_base + field_offset].
            const Type* bt = ix->base->ty;                 // array/slice type
            const Type* elem = ix->ty;                     // struct element type (sema sets ix->ty = base->elem)
            if (!elem || elem->struct_name.empty() || !ctx.structs) return;
            auto sit = ctx.structs->find(elem->struct_name);
            if (sit == ctx.structs->end()) return;
            auto fit = sit->second.fields.find(fl->field);
            if (fit == sit->second.fields.end()) return;
            int32_t struct_width = value_bytes(elem, ctx.structs);
            int32_t field_off = fit->second.offset;
            const Type* ft = fit->second.ty;
            // Resolve the array/slice base to (base_reg, base_off), exactly
            // as IndexExpr's eval does. v1 scope: ix->base must be a bare
            // Ident (the same restriction IndexExpr itself enforces).
            Reg base_reg = Reg::rbp;
            int32_t base_off = 0;
            bool base_ready = false;
            bool is_slice_base = false;
            if (auto* bid = dynamic_cast<const Ident*>(ix->base.get())) {
                auto it = locals.find(bid->name);
                if (it != locals.end()) {
                    const Type* lt = local_types[bid->name];
                    if (lt && lt->is_slice) {
                        eval(*ix->base);           // rax=ptr, rdx=len
                        e.mov_reg_reg(Reg::r10, Reg::rax); // r10 = base ptr (survives index eval)
                        e.mov_reg_reg(Reg::r9, Reg::rdx);  // r9 = len (survives index eval)
                        base_reg = Reg::r10; base_off = 0; base_ready = true; is_slice_base = true;
                    } else if (lt && lt->array_len > 0) {
                        base_reg = Reg::rbp; base_off = it->second; base_ready = true;
                    }
                } else {
                    // array/slice GLOBAL base (c3): same resolution as IndexExpr.
                    const auto* gidx = ctx.globals_index ? ctx.globals_index : (g_globals_for_codegen ? &g_globals_for_codegen->index : nullptr);
                    const auto* goffsets = ctx.globals_offsets ? ctx.globals_offsets : (g_globals_for_codegen ? &g_globals_for_codegen->offsets : nullptr);
                    const auto* gtypes = ctx.globals_types ? ctx.globals_types : (g_globals_for_codegen ? &g_globals_for_codegen->types : nullptr);
                    if (gidx && gtypes) {
                        int32_t goff = 0; bool found = false;
                        if (goffsets) { auto oit = goffsets->find(bid->name); if (oit != goffsets->end()) { goff = int32_t(oit->second); found = true; } }
                        if (!found) { auto gi = gidx->find(bid->name); if (gi != gidx->end()) { goff = int32_t(gi->second) * 8; found = true; } }
                        if (found) {
                            auto tit = gtypes->find(bid->name);
                            const Type* gt = (tit != gtypes->end()) ? tit->second : nullptr;
                            if (gt && gt->is_slice) {
                                eval(*ix->base);       // rax=absolute ptr, rdx=len
                                e.mov_reg_reg(Reg::r10, Reg::rax);
                                e.mov_reg_reg(Reg::r9, Reg::rdx);
                                base_reg = Reg::r10; base_off = 0; base_ready = true; is_slice_base = true;
                            } else if (gt && gt->array_len > 0) {
                                e.mov_reg_imm64_external(Reg::r10, AbsFixup::GlobalsBase);
                                base_reg = Reg::r10; base_off = goff; base_ready = true;
                            }
                        }
                    }
                }
            }
            if (base_ready) {
                eval(*ix->index);                  // rax = index
                // Bounds check - same policy as IndexExpr: a slice always
                // checks (len is runtime-only); a fixed array with a
                // compile-time-constant index was already verified by sema;
                // a fixed array with a non-constant index checks here.
                if (is_slice_base) {
                    e.mov_reg_reg(Reg::rcx, Reg::rax);       // stash index (rcx is free here)
                    emit_bounds_check_reg(Reg::rcx, Reg::r9); // idx (unsigned) < len
                } else if (!ix->index_is_const) {
                    e.mov_reg_reg(Reg::rcx, Reg::rax);
                    emit_bounds_check_imm(Reg::rcx, int64_t(bt->array_len));
                }
                if (struct_width > 1) {
                    e.mov_reg_imm64(Reg::rcx, int64_t(struct_width));
                    e.imul_reg_reg(Reg::rax, Reg::rcx);   // rax = index * struct_width
                }
                e.mov_reg_reg(Reg::r11, base_reg);
                e.add_reg_reg(Reg::r11, Reg::rax);        // r11 = element address
                // Field lives at element_base + field_offset; base_off is the
                // array's offset from base_reg (frame slot / globals addend / 0
                // for a slice ptr), so the full load offset is base_off + field_off.
                int32_t addr_off = base_off + field_off;
                if (ft->prim == Prim::F64 && !ft->is_slice) {
                    e.movsd_xmm_mem(Xmm::xmm0, Reg::r11, addr_off);
                } else if (ft->prim == Prim::F32 && !ft->is_slice) {
                    e.movss_xmm_mem(Xmm::xmm0, Reg::r11, addr_off);
                } else {
                    bool is_signed = ft->prim==Prim::I8||ft->prim==Prim::I16||
                                     ft->prim==Prim::I32||ft->prim==Prim::I64;
                    e.load_elem_to_rax(Reg::r11, addr_off, value_bytes(ft, ctx.structs), is_signed);
                }
            }
        }
        return;
    }
    if (auto* s = dynamic_cast<const SizeofExpr*>(&ex)) { e.mov_reg_imm64(Reg::rax, int64_t(s->resolved)); return; }
    if (auto* o = dynamic_cast<const OffsetofExpr*>(&ex)) { e.mov_reg_imm64(Reg::rax, int64_t(o->resolved)); return; }
    if (auto* al = dynamic_cast<const ArrayLit*>(&ex)) {
        // Array-literal rvalue (chunk c2). sema only lets a SLICE ArrayLit
        // reach eval() (a fixed-array ArrayLit is let-init-only and never
        // eval'd - see sema's ArrayLit case + the LetStmt init branch in
        // exec_stmt). Materialize the backing elements into a fresh temp
        // (alloc_arr_temp - a distinct __arrtmp$ slot, pre-reserved by
        // count_arr_temps_block), then yield the slice ABI {rax=ptr, rdx=len}
        // so the existing 2-word slice-arg path (CallExpr's op.words==2 stash)
        // and the slice-return path (ReturnStmt's is_slice_ret) both work for a
        // slice array literal passed as an arg or returned. A fixed-array
        // ArrayLit reaching here is an internal error (sema should have
        // rejected it) - trap rather than silently emit a truncated rax.
        const Type* at = al->ty;
        if (!at || !at->is_slice || !at->elem) {
            emit_trap(int(TrapReason::IllegalInstruction),
                      "internal: fixed-array array literal reached eval() (sema should reject)");
            return;
        }
        const Type* elem_ty = at->elem.get();
        int32_t elem_sz = value_bytes(elem_ty, ctx.structs);
        uint32_t count = uint32_t(al->elements.size());
        int32_t back_off = alloc_arr_temp(elem_ty, count);
        for (size_t i = 0; i < al->elements.size(); ++i) {
            store_value_to_memory(*al->elements[i], elem_ty, Reg::rbp,
                                  back_off + int32_t(i) * elem_sz);
        }
        // rax = lea [rbp+back_off]; rdx = count (slice ABI).
        e.byte(0x48); e.byte(0x8D); e.byte(0x85); e.imm32(back_off); // lea rax, [rbp+back_off]
        e.mov_reg_imm64(Reg::rdx, int64_t(count));
        return;
    }
    // StructLit is handled at its LetStmt init site; it is multi-word.
}

void CG::exec_block(const Block& b) {
    // Snapshot bindings so inner-block shadowing restores on exit, and align
    // one runtime cleanup scope exactly with this lexical Block.
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
    // A loop re-enters the same generated block. Reset all direct sites here,
    // not once in the function prologue, so each iteration has fresh state.
    if (!cleanup_scopes.back().reached_sites.empty()) {
        e.mov_reg_imm64(Reg::rax, 0);
        for (size_t site : cleanup_scopes.back().reached_sites)
            e.store_reg_mem(Reg::rbp, defer_sites[site].flag_offset, Reg::rax);
    }
    for (auto& s : b.stmts) exec_stmt(*s);
    emit_cleanup_scope(cleanup_scopes.size() - 1); // normal fallthrough
    cleanup_scopes.pop_back();
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
                    store_value_to_memory(*kv.second, fit->second.ty, Reg::rbp,
                                          base_off + fit->second.offset);
                }
            }
            return;
        }
        if (auto* alit = dynamic_cast<const ArrayLit*>(ls->init.get())) {
            // Array-literal init (chunk c2). sema guarantees ls->init->ty is
            // the declared array/slice Type (baked onto the ArrayLit) and
            // count+element types already validated. Two shapes:
            //  (a) fixed array T[N]: alloc the local's full N*elem_size frame
            //      slot (alloc_local sizes it via local_width_bytes), then
            //      store each element directly at base_off + i*elem_size via
            //      store_value_to_memory (handles int/float/elem widths).
            //  (b) slice T[]: the local is the 16-byte {ptr,len} pair. Alloc
            //      a SEPARATE backing temp (alloc_arr_temp - a distinct
            //      __arrtmp$ slot, pre-reserved by count_arr_temps_block) of
            //      count*elem_size bytes, store each element there, then store
            //      ptr = lea [rbp+back_off] and len = count into the slice slot.
            //      The backing lives in the frame for the whole fn (over-
            //      reserved but correct - the slice's ptr points at it).
            const Type* at = ls->init->ty;
            const Type* elem_ty = at && at->elem ? at->elem.get() : nullptr;
            int32_t elem_sz = value_bytes(elem_ty, ctx.structs);
            if (at && at->array_len > 0) {
                // fixed array T[N]: elements go directly into the local's slot.
                int32_t base_off = alloc_local(ls->name, at);
                for (size_t i = 0; i < alit->elements.size(); ++i) {
                    store_value_to_memory(*alit->elements[i], elem_ty, Reg::rbp,
                                          base_off + int32_t(i) * elem_sz);
                }
            } else if (at && at->is_slice) {
                // slice T[]: local is {ptr,len}; backing is a separate temp.
                int32_t slot_off = alloc_local(ls->name, at);  // 16 bytes
                uint32_t count = uint32_t(alit->elements.size());
                int32_t back_off = alloc_arr_temp(elem_ty, count);  // count*elem_sz bytes
                for (size_t i = 0; i < alit->elements.size(); ++i) {
                    store_value_to_memory(*alit->elements[i], elem_ty, Reg::rbp,
                                          back_off + int32_t(i) * elem_sz);
                }
                // ptr = lea [rbp+back_off]; len = count. Mirror the slice
                // store idiom (ptr at off, len at off+8) used everywhere else.
                e.byte(0x48); e.byte(0x8D); e.byte(0x85); e.imm32(back_off); // lea rax, [rbp+back_off]
                e.store_reg_mem(Reg::rbp, slot_off, Reg::rax);
                e.mov_reg_imm64(Reg::rax, int64_t(count));
                e.store_reg_mem(Reg::rbp, slot_off + 8, Reg::rax);
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
        if (auto* cast = dynamic_cast<const CastExpr*>(ls->init.get())) {
            const Type* ct = cast->ty;
            const bool aggregate = ct && (ct->array_len > 0 ||
                (!ct->struct_name.empty() && ctx.structs && ctx.structs->count(ct->struct_name) != 0));
            if (aggregate && cast->operand->ty && cast->operand->ty->same(*ct)) {
                // A permitted same-type aggregate cast is a real whole-value
                // no-op, not eval()'s one-register scalar convention.  Sema
                // restricts the source to a local below; copy every byte.
                int32_t dst = alloc_local(ls->name, ct);
                auto* id = dynamic_cast<const Ident*>(cast->operand.get());
                auto src = id ? locals.find(id->name) : locals.end();
                if (src != locals.end())
                    copy_bytes(Reg::rbp, dst, Reg::rbp, src->second, local_width_bytes(ct, ctx.structs));
                else {
                    assert(false && "aggregate cast source is not a local");
                    emit_trap(int(TrapReason::IllegalInstruction), "internal: aggregate cast source is not a local");
                }
                return;
            }
        }
        int32_t off = alloc_local(ls->name, ls->init->ty);
        eval(*ls->init);
        const Type* t = ls->init->ty;
        if (t->is_slice) {
            e.store_reg_mem(Reg::rbp, off, Reg::rax);
            e.store_reg_mem(Reg::rbp, off + 8, Reg::rdx);
        } else if (t->is_float()) store_xmm0_to_rbp(*this, off, t);
        else { normalize_rax(t); store_rax_to_rbp(*this, off); }
        return;
    }
    if (auto* es = dynamic_cast<const ExprStmt*>(&s)) { eval(*es->expr); return; }
    if (auto* ds = dynamic_cast<const DeferStmt*>(&s)) {
        auto it = defer_site_indices.find(ds);
        if (it != defer_site_indices.end()) {
            DeferSite& site = defer_sites[it->second];
            site.locals_at_decl = locals;
            site.types_at_decl = local_types;
            e.mov_reg_imm64(Reg::rax, 1);
            e.store_reg_mem(Reg::rbp, site.flag_offset, Reg::rax);
        }
        return;
    }
    if (auto* rs = dynamic_cast<const ReturnStmt*>(&s)) {
        bool has_defers = has_active_cleanups();
        if (returns_struct_by_ptr()) {
            // struct-by-value return: no register-based result to stash
            // across defers (the callee writes through the hidden pointer
            // directly, at whatever point in this sequence that happens) -
            // sema guarantees rs->value is a bare Ident, a same-return-type
            // forwarding call, or a struct literal of the return type (see
            // check_stmt's ReturnStmt handling). The struct-literal case has
            // no source frame address, so materialize it into a compiler-hidden
            // temp slot first (alloc_struct_temp - sized into the frame by
            // count_struct_temps_block), then copy the temp's bytes through the
            // hidden pointer - mirroring the Ident branch with the temp
            // standing in for the named local.
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
                } else if (auto* sl = dynamic_cast<const StructLit*>(rs->value.get())) {
                    auto sit = ctx.structs->find(f.ret->struct_name);
                    if (sit != ctx.structs->end()) {
                        int32_t temp_off = alloc_struct_temp(f.ret.get());
                        store_value_to_memory(*sl, f.ret.get(), Reg::rbp, temp_off);
                        e.load_reg_mem(Reg::r11, Reg::rbp, struct_ret_ptr_offset);
                        copy_bytes(Reg::r11, 0, Reg::rbp, temp_off, sit->second.size);
                    }
                }
            }
            if (has_defers) emit_cleanups_to(0);
            // Cleanup calls may clobber rax; the hidden-pointer ABI requires
            // the destination pointer to be returned there as well.
            e.load_reg_mem(Reg::rax, Reg::rbp, struct_ret_ptr_offset);
            emit_epilogue();
            return;
        }
        bool is_float_ret = f.ret && f.ret->is_float();
        bool is_f64_ret = f.ret && f.ret->prim == Prim::F64;
        bool is_slice_ret = f.ret && f.ret->is_slice;
        if (rs->value) {
            eval(*rs->value);
            // float return -> xmm0 (already there if float eval); int -> rax (already)
            if (has_defers) {
                // stash the return value(s) across defer evaluation - a
                // defer's own expression may clobber rax/xmm0/rdx.
                // Keep rsp 16-byte aligned while cleanup expressions run:
                // they may make arbitrary script/native calls. A one-word
                // push (or 8-byte SSE spill) would leave rsp%16 == 8 at those
                // call sites and violate Win64 even though the value itself
                // only occupies one word. Use one uniform 16-byte temporary.
                e.sub_reg_imm32(Reg::rsp, 16);
                if (is_float_ret) {
                    if(is_f64_ret)e.movsd_mem_xmm(Reg::rsp,0,Xmm::xmm0);else e.movss_mem_xmm(Reg::rsp,0,Xmm::xmm0);
                } else {
                    e.store_reg_mem(Reg::rsp, 0, Reg::rax);
                    if (is_slice_ret) e.store_reg_mem(Reg::rsp, 8, Reg::rdx);
                }
            }
        }
        if (has_defers) {
            emit_cleanups_to(0);
            if (rs->value) {
                if (is_float_ret) {
                    if(is_f64_ret)e.movsd_xmm_mem(Xmm::xmm0,Reg::rsp,0);else e.movss_xmm_mem(Xmm::xmm0,Reg::rsp,0);
                } else {
                    e.load_reg_mem(Reg::rax, Reg::rsp, 0);
                    if (is_slice_ret) e.load_reg_mem(Reg::rdx, Reg::rsp, 8);
                }
                e.add_reg_imm32(Reg::rsp, 16);
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
        // H-M4-1: a dedicated charged `latch` label is the `continue` target
        // (NOT `top`), so the back-edge budget charge runs before the
        // condition is re-evaluated - mirroring the for/do-while latches.
        // Pre-fix `continue` jumped to `top` and skipped the charge, so a
        // `while(true){...;continue;}` loop completed under any budget.
        Label top = e.alloc_label(), latch = e.alloc_label(), end = e.alloc_label();
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
                active_pin = PinState{*pin_name, locals[*pin_name], Reg::rbx};
                set_pin_here = true;
            }
        }
        loops.push_back({latch, end, false, cleanup_scopes.size()});
        exec_block(ws->body);
        loops.pop_back();
        if (set_pin_here) active_pin.reset();
        e.bind(latch);
        emit_budget_check(block_cost(ws->body), "budget exceeded at loop back-edge");
        e.jmp(top);
        e.bind(end);
        return;
    }
    if (auto* ds = dynamic_cast<const DoWhileStmt*>(&s)) {
        // Bottom-tested loop: body first; continue goes to the condition;
        // budget is charged only on the taken back edge.
        Label body = e.alloc_label(), cond = e.alloc_label(), end = e.alloc_label();
        e.bind(body);
        loops.push_back({cond, end, false, cleanup_scopes.size()});
        exec_block(ds->body);
        loops.pop_back();
        e.bind(cond);
        eval(*ds->cond); e.cmp_reg_imm32(Reg::rax, 0); e.jcc(Cond::e, end);
        emit_budget_check(block_cost(ds->body), "budget exceeded at loop back-edge");
        e.jmp(body);
        e.bind(end);
        return;
    }
    if (auto* fe = dynamic_cast<const ForEachStmt*>(&s)) {
        // for (x in iter) { body } — desugars to a while loop over the slice.
        // The iter is a slice {ptr, len}; x gets the element at each index.
        const Type* iter_ty = fe->iter->ty;
        const Type* elem_ty = iter_ty && iter_ty->elem ? iter_ty->elem.get() : nullptr;
        int32_t esz = value_bytes(elem_ty, ctx.structs);
        if (esz <= 0) esz = 8;
        // Static i64 type for the ptr/len/index slots.
        static const Type i64_ty = make_prim(Prim::I64);
        // Unique names for the for-each internals (a nested for-each would
        // overwrite the outer's locals map entries if they shared names).
        int fe_id = fe_counter++;
        std::string ptr_name = "__fe_ptr$" + std::to_string(fe_id);
        std::string len_name = "__fe_len$" + std::to_string(fe_id);
        std::string idx_name = "__fe_idx$" + std::to_string(fe_id);
        // Alloc frame slots for: ptr, len, index, var.
        int32_t ptr_off = alloc_local(ptr_name, &i64_ty);
        int32_t len_off = alloc_local(len_name, &i64_ty);
        int32_t idx_off = alloc_local(idx_name, &i64_ty);
        int32_t var_off = alloc_local(fe->var, elem_ty ? elem_ty : &i64_ty);
        // Evaluate the iterable → rax=ptr, rdx=len (the slice ABI).
        eval(*fe->iter);
        e.store_reg_mem(Reg::rbp, ptr_off, Reg::rax);
        e.mov_reg_reg(Reg::rax, Reg::rdx);
        e.store_reg_mem(Reg::rbp, len_off, Reg::rax);
        e.mov_reg_imm64(Reg::rax, 0);
        e.store_reg_mem(Reg::rbp, idx_off, Reg::rax);
        // The loop.
        Label top = e.alloc_label(), latch = e.alloc_label(), end = e.alloc_label();
        e.bind(top);
        e.load_reg_mem(Reg::rax, Reg::rbp, idx_off);
        e.load_reg_mem(Reg::rdx, Reg::rbp, len_off);
        e.cmp_reg_reg(Reg::rax, Reg::rdx);
        e.jcc(Cond::ge, end);
        // Load element at [ptr + index*esz].
        e.load_reg_mem(Reg::rax, Reg::rbp, ptr_off);   // rax = ptr
        e.load_reg_mem(Reg::rcx, Reg::rbp, idx_off);   // rcx = index
        uint8_t scale = 0;
        if (esz == 1) scale = 0; else if (esz == 2) scale = 1;
        else if (esz == 4) scale = 2; else scale = 3;
        e.lea_reg_mem_sib(Reg::rax, Reg::rax, Reg::rcx, scale);  // rax = ptr + index*esz
        e.load_elem_to_rax(Reg::rax, 0, esz, false);  // rax = element
        // Store to var's slot.
        e.store_rax_elem(Reg::rbp, var_off, esz);
        loops.push_back({latch, end, false, cleanup_scopes.size()});
        exec_block(fe->body);
        loops.pop_back();
        e.bind(latch);
        e.load_reg_mem(Reg::rax, Reg::rbp, idx_off);
        e.add_reg_imm32(Reg::rax, 1);
        e.store_reg_mem(Reg::rbp, idx_off, Reg::rax);
        emit_budget_check(block_cost(fe->body), "budget exceeded at for-each back-edge");
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
                active_pin = PinState{*pin_name, locals[*pin_name], Reg::rbx};
                set_pin_here = true;
            }
        }
        loops.push_back({step_l, end_l, false, cleanup_scopes.size()});
        exec_block(fs->body);
        loops.pop_back();
        e.bind(step_l);
        if (fs->step) eval(*fs->step);
        emit_budget_check(block_cost(fs->body), "budget exceeded at loop back-edge");
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
        // Win64 r10 is volatile, unlike the old r13 scratch.  Sema permits
        // only IntLit/BoolLit case values; their eval paths are immediate
        // loads into rax and cannot clobber r10, so the subject remains live
        // across the complete compare chain without a nonvolatile save.
        e.mov_reg_reg(Reg::r10, Reg::rax);
        std::vector<Label> case_labels;
        for (size_t i = 0; i < sw->cases.size(); ++i) case_labels.push_back(e.alloc_label());
        Label end_label = e.alloc_label();
        int default_idx = -1;
        for (size_t i = 0; i < sw->cases.size(); ++i) {
            if (sw->cases[i].is_default) { default_idx = int(i); continue; }
            eval(*sw->cases[i].value);
            e.cmp_reg_reg(Reg::r10, Reg::rax);
            e.jcc(Cond::e, case_labels[i]);
        }
        e.jmp(default_idx >= 0 ? case_labels[size_t(default_idx)] : end_label);
        for (size_t i = 0; i < sw->cases.size(); ++i) {
            e.bind(case_labels[i]);
            loops.push_back({Label{0}, end_label, true, cleanup_scopes.size()}); // continue skips switch frames
            exec_block(sw->cases[i].body);
            loops.pop_back();
        }
        e.bind(end_label);
        return;
    }
    if (auto* ms = dynamic_cast<const MatchStmt*>(&s)) {
        // match (expr) { pattern => body, ... _ => default }
        // Like switch but each arm is a separate branch (no fallthrough, no
        // break needed). The subject is compared against each pattern in order;
        // the first match jumps to its arm. The wildcard (_) is the default.
        eval(*ms->subject);
        e.mov_reg_reg(Reg::r10, Reg::rax);  // hold subject in volatile r10
        std::vector<Label> arm_labels;
        for (size_t i = 0; i < ms->arms.size(); ++i) arm_labels.push_back(e.alloc_label());
        Label end_label = e.alloc_label();
        int wildcard_idx = -1;
        for (size_t i = 0; i < ms->arms.size(); ++i) {
            if (ms->arms[i].is_wildcard) { wildcard_idx = int(i); continue; }
            eval(*ms->arms[i].pattern);
            e.cmp_reg_reg(Reg::r10, Reg::rax);
            e.jcc(Cond::e, arm_labels[i]);
        }
        e.jmp(wildcard_idx >= 0 ? arm_labels[size_t(wildcard_idx)] : end_label);
        for (size_t i = 0; i < ms->arms.size(); ++i) {
            e.bind(arm_labels[i]);
            loops.push_back({Label{0}, end_label, true, cleanup_scopes.size()});
            exec_block(ms->arms[i].body);
            loops.pop_back();
            e.jmp(end_label);  // no fallthrough — each arm jumps to end
        }
        e.bind(end_label);
        return;
    }
    if (auto* bs = dynamic_cast<const BlockStmt*>(&s)) { exec_block(bs->block); return; }
    if (dynamic_cast<const BreakStmt*>(&s)) {
        if (!loops.empty()) {
            emit_cleanups_to(loops.back().cleanup_depth);
            e.jmp(loops.back().brk);
        }
        return;
    }
    if (dynamic_cast<const ContinueStmt*>(&s)) {
        // skip past any enclosing switch frame(s) - continue always targets
        // the nearest real loop, never a switch (which is break-only).
        for (int i = int(loops.size()) - 1; i >= 0; --i) {
            if (!loops[size_t(i)].is_switch) {
                emit_cleanups_to(loops[size_t(i)].cleanup_depth);
                e.jmp(loops[size_t(i)].cont);
                break;
            }
        }
        return;
    }
}

// v0.4/M4 instruction-budget cost estimator (docs/spec/SAFETY_AND_SANDBOX.md §3).
// Reach-aware recursive cost: counts statements PLUS the emitted work they
// reach - expression nodes (each lowered operand emits real instructions),
// native-call setup + arg marshalling, aggregate byte copies, switch compare
// chains, and for init/step. Trusted native BODY execution time is explicitly
// OUTSIDE the unit (a single nap() returning under budget is intended, not a
// counter-example): only the JIT call-site setup is charged. Saturating
// arithmetic keeps arbitrarily large legal ASTs encodable as the positive
// imm32 consumed by emit_budget_check (never truncate/wrap negative).
static int64_t cost_add(int64_t a, int64_t b) {
    const int64_t cap = std::numeric_limits<int32_t>::max();
    return a >= cap - b ? cap : a + b;
}
// Charge for a struct/array copy of N bytes: proportional to N (one emitted
// load/store chunk per 8 bytes), floored at 1 so a 1-byte copy still charges.
// Slices are always a 2-word {ptr,len} copy regardless of element count.
static int64_t aggregate_copy_cost(const Type* t, const StructLayoutTable* structs) {
    if (!t) return 1;
    if (t->is_slice) return 2;
    int32_t bytes = CG::value_bytes(t, structs);
    int64_t n = (int64_t(bytes) + 7) / 8;
    return n < 1 ? 1 : n;
}
int64_t CG::block_cost(const Block& b) {
    int64_t n = 0;
    for (auto& s : b.stmts) n = cost_add(n, cost_add(1, stmt_cost(*s)));
    return n < 1 ? 1 : n;  // floor at 1: empty functions/loops still charge
}
// Count AST expression nodes plus native-call setup + arg marshalling. Each
// node ~one lowered instruction; each CallExpr site adds setup+call (+2) plus
// one per marshalled arg; aggregate by-value args add their byte-copy cost.
int64_t CG::expr_cost(const Expr& ex) {
    if (auto* li = dynamic_cast<const IntLit*>(&ex))    { (void)li; return 1; }
    if (dynamic_cast<const FloatLit*>(&ex))             return 1;
    if (dynamic_cast<const BoolLit*>(&ex))              return 1;
    if (dynamic_cast<const StringLit*>(&ex))           return 1;
    if (dynamic_cast<const Ident*>(&ex))               return 1;
    if (dynamic_cast<const FnHandleExpr*>(&ex))         return 1;
    if (dynamic_cast<const SizeofExpr*>(&ex))          return 1;
    if (dynamic_cast<const OffsetofExpr*>(&ex))        return 1;
    if (dynamic_cast<const EnumAccessExpr*>(&ex))      return 1;
    if (auto* b = dynamic_cast<const BinExpr*>(&ex)) {
        int64_t n = 1;  // the binop itself
        n = cost_add(n, expr_cost(*b->lhs));
        n = cost_add(n, expr_cost(*b->rhs));
        if (b->is_overload) n = cost_add(n, 2);  // overload dispatch = native call site
        return n;
    }
    if (auto* u = dynamic_cast<const UnaryExpr*>(&ex))
        return cost_add(1, expr_cost(*u->operand));
    if (auto* c = dynamic_cast<const CastExpr*>(&ex))
        return cost_add(1, expr_cost(*c->operand));
    if (auto* t = dynamic_cast<const TernaryExpr*>(&ex))
        return cost_add(cost_add(cost_add(1, expr_cost(*t->cond)), expr_cost(*t->then_e)), expr_cost(*t->else_e));
    if (auto* a = dynamic_cast<const AssignExpr*>(&ex)) {
        int64_t n = cost_add(1, expr_cost(*a->value));
        if (a->target) n = cost_add(n, expr_cost(*a->target));
        return n;
    }
    if (auto* ix = dynamic_cast<const IndexExpr*>(&ex))
        return cost_add(cost_add(1, expr_cost(*ix->base)), expr_cost(*ix->index));
    if (auto* fx = dynamic_cast<const FieldExpr*>(&ex))
        return cost_add(1, expr_cost(*fx->base));
    if (auto* v = dynamic_cast<const ViewExpr*>(&ex))
        return cost_add(1, expr_cost(*v->base));
    if (auto* sl = dynamic_cast<const StructLit*>(&ex)) {
        int64_t n = 1;
        for (auto& kv : sl->fields) n = cost_add(n, expr_cost(*kv.second));
        return n;
    }
    if (auto* c = dynamic_cast<const CallExpr*>(&ex)) {
        // Call-site setup + the call itself (+2), plus one per marshalled arg,
        // plus aggregate byte-copy cost for any by-value struct/array arg.
        int64_t n = 2;
        if (c->receiver) n = cost_add(n, expr_cost(*c->receiver));
        for (auto& a : c->args) {
            n = cost_add(n, expr_cost(*a));
            n = cost_add(n, 1);  // arg marshalling
            if (a) n = cost_add(n, aggregate_copy_cost(a->ty, ctx.structs));
        }
        if (c->indirect_target) n = cost_add(n, expr_cost(*c->indirect_target));
        return n;
    }
    return 1;  // unknown leaf expression
}
int64_t CG::stmt_cost(const Stmt& s) {
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
        // subject eval + one compare per case + each case body.
        int64_t n = sw->subject ? expr_cost(*sw->subject) : 0;
        n = cost_add(n, int64_t(sw->cases.size()));  // compare chain
        for (auto& c : sw->cases) n = cost_add(n, block_cost(c.body));
        return n;
    }
    if (auto* ms = dynamic_cast<const MatchStmt*>(&s)) {
        int64_t n = ms->subject ? expr_cost(*ms->subject) : 0;
        n = cost_add(n, int64_t(ms->arms.size()));  // compare chain
        for (auto& arm : ms->arms) n = cost_add(n, block_cost(arm.body));
        return n;
    }
    if (auto* ls = dynamic_cast<const LetStmt*>(&s))
        return ls->init ? cost_add(1, expr_cost(*ls->init)) : 1;
    if (auto* rs = dynamic_cast<const ReturnStmt*>(&s))
        return rs->value ? cost_add(1, expr_cost(*rs->value)) : 1;
    if (auto* es = dynamic_cast<const ExprStmt*>(&s))
        return cost_add(1, expr_cost(*es->expr));
    if (auto* ds = dynamic_cast<const DeferStmt*>(&s))
        return ds->expr ? cost_add(1, expr_cost(*ds->expr)) : 1;
    return 1;  // Break/Continue and other leaf statements
}

} // namespace

GlobalsBlock* g_globals_for_codegen = nullptr;

// v1.0 Tier 2 (docs/planning/plan_FUNCTION_REFS.md §5.2): build the registered-fn allowlist.
std::vector<uint8_t> build_fn_allowlist(
    const std::unordered_map<std::string, int>& script_slots, int slot_count) {
    std::vector<uint8_t> bits((size_t(slot_count) + 7) / 8, 0);
    for (const auto& kv : script_slots) {
        int s = kv.second;
        if (s >= 0 && s < slot_count) bits[size_t(s) / 8] |= uint8_t(1u << (s % 8));
    }
    return bits;
}

CompiledFn compile_func(const FuncDecl& f, const CodeGenCtx& ctx) {
    if (ctx.enable_ir_backend) {
        // Obf fallback: the IR path does not handle @obf_keyed/mba/opaque in
        // Stage A (host-DLL build-time phase). If the function uses obf, fall
        // back to the tree-walker for correctness.
        bool uses_obf = false;
        for (auto& ann : f.annotations) {
            if (ann.name == "obf" || ann.name == "obf_keyed") { uses_obf = true; break; }
        }
        if (!uses_obf && !(ctx.obf.mba || ctx.obf.opaque || ctx.obf.keyed)) {
            ThinFunction thf = lower_function(f, ctx);
            if (!thf.blocks.empty()) {
                // Stage C: run IR optimization passes between lower and emit.
                if (ctx.pass_manager) {
                    EmberAnalysisManager am;
                    ctx.pass_manager->run(thf, am);
                }
                return emit_x64(thf, ctx);
            }
            // empty body / lowering gave up (non_serializable) -> fall through to tree-walker
        }
    }
    // ... existing CG tree-walk continues unchanged (default-off path) ...
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
    // We alloc locals during exec. But frame size must be known up front.
    // Simplification: give a generous fixed frame based on a quick count of let-stmts.
    // Count locals: walk and count LetStmt occurrences.
    // sum actual byte widths (slices=16, fixed arrays/structs recursive, else 8)
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
            if (auto* ds = dynamic_cast<const DoWhileStmt*>(s.get())) sum_bytes(ds->body);
            if (auto* fe = dynamic_cast<const ForEachStmt*>(s.get())) {
                // for-each allocates: ptr(8) + len(8) + idx(8) + var(elem_width)
                locals_area += 24;  // ptr + len + idx
                const Type* et = fe->iter && fe->iter->ty && fe->iter->ty->elem ? fe->iter->ty->elem.get() : nullptr;
                locals_area += CG::local_width_bytes(et, ctx.structs);  // var
                sum_bytes(fe->body);
            }
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
    // Reserve frame bytes for the compiler-hidden struct temp slots (see
    // temp_counter / count_struct_temps_block). These are synthesized during
    // exec for struct-by-value general-expression args and struct-literal
    // return values; sum_bytes above only counts user LetStmt locals, so the
    // temps would otherwise overflow the pre-sized frame. Counting the SUM is
    // a safe over-reservation (temps are short-lived, but reserving them as
    // full frame slots keeps alloc_local's offsets within the frame).
    int32_t struct_temp_bytes = 0;
    cg.count_struct_temps_block(f.body, struct_temp_bytes);
    locals_area += struct_temp_bytes;
    // Chunk c2: reserve frame bytes for array-literal backing temps (see
    // arr_temp_counter / count_arr_temps_block). A slice array literal's
    // backing region is synthesized during exec; sum_bytes above only counts
    // user LetStmt locals (the slice's own 16-byte {ptr,len} slot IS counted
    // there via local_width_bytes(ls->init->ty)=16, but the separate backing
    // array is not). Counting the SUM is a safe over-reservation.
    int32_t arr_temp_bytes = 0;
    cg.count_arr_temps_block(f.body, arr_temp_bytes);
    locals_area += arr_temp_bytes;
    // Chunk c3: reserve frame bytes for string-literal inline-decryption
    // temps (see str_temp_counter / count_str_temps_block). An encrypted
    // string literal's plaintext is decrypted inline into a stack temp at
    // each use site; sum_bytes above does not count these compiler-hidden
    // temps. Counting the SUM is a safe over-reservation (each temp is
    // short-lived but reserved as a full frame slot, mirroring struct/arr).
    int32_t str_temp_bytes = 0;
    cg.count_str_temps_block(f.body, str_temp_bytes);
    locals_area += str_temp_bytes;
    // Collect textual sites once for frame layout. Runtime cleanup ownership
    // is established later by exec_block's lexical cleanup-scope stack.
    std::function<void(const Block&)> collect_defers = [&](const Block& b) {
        for (auto& s : b.stmts) {
            if (auto* ds = dynamic_cast<const DeferStmt*>(s.get())) {
                size_t index = cg.defer_sites.size();
                cg.defer_sites.push_back(CG::DeferSite{ds});
                cg.defer_site_indices[ds] = index;
            }
            if (auto* is = dynamic_cast<const IfStmt*>(s.get())) { collect_defers(is->then_b); if (is->has_else) collect_defers(is->else_b); }
            if (auto* ws = dynamic_cast<const WhileStmt*>(s.get())) collect_defers(ws->body);
            if (auto* ds = dynamic_cast<const DoWhileStmt*>(s.get())) collect_defers(ds->body);
            if (auto* fe = dynamic_cast<const ForEachStmt*>(s.get())) collect_defers(fe->body);
            if (auto* bs = dynamic_cast<const BlockStmt*>(s.get())) collect_defers(bs->block);
            if (auto* fs = dynamic_cast<const ForStmt*>(s.get())) collect_defers(fs->body);
            if (auto* sw = dynamic_cast<const SwitchStmt*>(s.get())) for (auto& c : sw->cases) collect_defers(c.body);
            if (auto* ms = dynamic_cast<const MatchStmt*>(s.get())) for (auto& arm : ms->arms) collect_defers(arm.body);
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
    cg.e.sub_reg_imm32(Reg::rsp, cg.frame_size);
    // Save rbx into its reserved frame slot now that the (still 16-byte
    // aligned) frame is live (Item E) - paired with emit_epilogue()'s
    // load_reg_mem restore. Unconditional regardless of whether this
    // function's own pinning heuristic ends up using rbx.
    cg.e.store_reg_mem(Reg::rbp, cg.rbx_save_offset, Reg::rbx);
    // (Stage 1 local regalloc uses the VOLATILE r10 as its holding register, so
    // there is NO callee-saved save/restore in the prologue — zero tax on any
    // function. A RHS containing a call falls back to push/pop since r10 is
    // volatile; a no-call RHS uses r10 with no prologue cost.)

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
    auto spill_word = [&](int32_t w, int32_t dst_off, const Type* float_ty) {
        if (w < 4) {
            if (float_ty) { if(float_ty->prim==Prim::F64)cg.e.movsd_mem_xmm(Reg::rbp,dst_off,flt_arg_regs[w]);else cg.e.movss_mem_xmm(Reg::rbp,dst_off,flt_arg_regs[w]); }
            else cg.e.store_reg_mem(Reg::rbp, dst_off, int_arg_regs[w]);
        } else {
            int32_t src_off = 48 + total_words * 8 + (w - 4) * 8;
            if (float_ty) {
                if(float_ty->prim==Prim::F64){cg.e.movsd_xmm_mem(Xmm::xmm4,Reg::rbp,src_off);cg.e.movsd_mem_xmm(Reg::rbp,dst_off,Xmm::xmm4);}
                else { cg.e.movss_xmm_mem(Xmm::xmm4, Reg::rbp, src_off); cg.e.movss_mem_xmm(Reg::rbp, dst_off, Xmm::xmm4); }
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
        spill_word(0, cg.struct_ret_ptr_offset, nullptr);
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
                    spill_word(w_global, off + byte_pos, nullptr);
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
            spill_word(param_word, off, nullptr);
            spill_word(param_word + 1, off + 8, nullptr);
        } else if (pt->is_float()) {
            spill_word(param_word, off, pt);
        } else {
            spill_word(param_word, off, nullptr);
        }
        param_word += wcount;
    }

    // Reserve one activation flag per textual site. Each owning Block resets
    // its direct flags at runtime entry, including every loop iteration.
    for (auto& site : cg.defer_sites) {
        cg.next_local_off += 8;
        site.flag_offset = -cg.next_local_off;
    }

    // M4: charge the complete recursive body estimate once per function
    // invocation after the frame, hidden pointer, and parameters are safely
    // established, and before annotation gates or body execution. Loop
    // back-edge charges remain in place for repeated work. In portable `.em`
    // CLI mode budget checks are disabled, so this emits no process-local
    // address or rejection metadata.
    cg.emit_budget_check(cg.block_cost(f.body), "budget exceeded at function entry");

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

    // Implicit void return. exec_block already emitted the function body's
    // normal-fallthrough lexical cleanup before returning to us.
    cg.emit_epilogue();

    cg.e.resolve_fixups();

    // Fill the absolute-imm64 placeholders recorded by mov_reg_imm64_external
    // (docs/BUNDLING_AND_EM_MODULES.md Section 2.4). resolve_fixups does not touch these  - 
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
            case AbsFixup::ModuleRegistryBase: v = uint64_t(ctx.registry_base); break; // v0.5 cross-module (ModuleRegistry::base())
            case AbsFixup::FunctionRodataBase: v = uint64_t(cg.rodata.data() + af.addend); break;
        }
        for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i));
    }

    CompiledFn out;
    out.name = f.name;
    out.abs_fixups = cg.e.abs_fixups(); // capture for .em serialization (Section 2.4)
    // Symbol/signature comes from the exact sema resolution, not reverse
    // lookup by pointer. This covers overload-only symbols and pointer aliases.
    for (const auto& pending : cg.pending_natives) {
        out.native_fixups.push_back(pending.binding);
        if (pending.binding.code_offset + 8 > cg.e.code.size()) continue;
        uint64_t v = reinterpret_cast<uintptr_t>(pending.target);
        for (int i = 0; i < 8; ++i)
            cg.e.code[pending.binding.code_offset + i] = uint8_t(v >> (8 * i));
    }

    // --- Stage 1: post-emit peephole (docs/spec/CODEGEN_OPTIMIZATION_DESIGN.md §4.5) ---
    // Runs ONLY when ctx.enable_peephole (default false -> byte-identical to
    // the pre-Stage-1 tree-walker, the gate holds). Runs AFTER resolve_fixups
    // AND after the AbsFixup/native-fixup patching above, so: (a) the guarded
    // regions (AbsFixup/NativeFixup code_offsets) now point at the real filled
    // addresses and the peephole skips them (they are relocatable; the `.em`
    // serializer reads the fixup list by offset, so they must stay 10 bytes);
    // (b) the plain `mov r, imm64` literals (IntLit loads, baked fn ptrs) read
    // their final imm64 values, so the SmartImm range checks see the real
    // constants. The peephole is a strictly local in-place rewrite padded with
    // trailing NOPs, so NO label offset shifts and NO branch fixup needs re-
    // resolving. out.abs_fixups / out.native_fixups (captured above) keep their
    // original code_offsets (the peephole does not touch the guarded regions).
    if (ctx.enable_peephole && !cg.e.code.empty()) {
        PeepholeGuardedRegions guarded;
        for (const auto& af : out.abs_fixups) guarded.imm64_offsets.insert(af.code_offset);
        for (const auto& nf : out.native_fixups) guarded.imm64_offsets.insert(nf.code_offset);
        PeepholeCtx pctx{ cg.e.code, cg.e.resolved_labels_view(), std::move(guarded) };
        auto pipeline = make_stage1_pipeline();
        pipeline.run_all(pctx);
    }

    out.rodata = std::move(cg.rodata);
    out.non_serializable_reason = std::move(cg.non_serializable_reason);
    out.bytes = std::move(cg.e.code);
    return out;
}

} // namespace ember
