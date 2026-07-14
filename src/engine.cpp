#include "engine.hpp"
#include "module_instance.hpp"
#include "hot_reload_domain.hpp"  // Red 8: HotReloadDomain::ExecutionGuard (generation guard, no frontend dep)
#include <optional>
#include <stdexcept>
#include <cstring>

#if defined(_MSC_VER) && defined(_M_X64)
#error "MSVC x64 not yet supported; use MinGW"
#endif

namespace ember {

void X64Emitter::resolve_fixups() {
    for (const auto& f : pending) {
        auto it = bound.find(f.label_id);
        if (it == bound.end()) {
            throw std::runtime_error("ember: unbound label " + std::to_string(f.label_id));
        }
        uint32_t target = it->second;
        // rel32 = target - (fixup_offset + 4)
        int32_t rel = int32_t(int64_t(target) - int64_t(f.code_offset + 4));
        code[f.code_offset + 0] = uint8_t(rel);
        code[f.code_offset + 1] = uint8_t(rel >> 8);
        code[f.code_offset + 2] = uint8_t(rel >> 16);
        code[f.code_offset + 3] = uint8_t(rel >> 24);
    }
    pending.clear();
}

CompiledFn compile_add_i64() {
    X64Emitter e;
    e.push(Reg::rbp);
    e.mov_reg_reg(Reg::rbp, Reg::rsp);   // mov rbp, rsp
    e.mov_reg_reg(Reg::rax, Reg::rcx);   // mov rax, rcx  (a)
    e.add_reg_reg(Reg::rax, Reg::rdx);   // add rax, rdx  (b)
    e.mov_reg_reg(Reg::rsp, Reg::rbp);   // mov rsp, rbp
    e.pop(Reg::rbp);
    e.ret();
    return CompiledFn{"add", std::move(e.code), nullptr, nullptr};
}

CompiledFn compile_sub_i64() {
    X64Emitter e;
    e.push(Reg::rbp);
    e.mov_reg_reg(Reg::rbp, Reg::rsp);
    e.mov_reg_reg(Reg::rax, Reg::rcx);
    e.sub_reg_reg(Reg::rax, Reg::rdx);
    e.mov_reg_reg(Reg::rsp, Reg::rbp);
    e.pop(Reg::rbp);
    e.ret();
    return CompiledFn{"sub", std::move(e.code), nullptr, nullptr};
}

CompiledFn compile_mul_i64() {
    X64Emitter e;
    e.push(Reg::rbp);
    e.mov_reg_reg(Reg::rbp, Reg::rsp);
    e.mov_reg_reg(Reg::rax, Reg::rcx);
    e.imul_reg_reg(Reg::rax, Reg::rdx);
    e.mov_reg_reg(Reg::rsp, Reg::rbp);
    e.pop(Reg::rbp);
    e.ret();
    return CompiledFn{"mul", std::move(e.code), nullptr, nullptr};
}

CompiledFn compile_ret_const(int64_t imm) {
    X64Emitter e;
    e.push(Reg::rbp);
    e.mov_reg_reg(Reg::rbp, Reg::rsp);
    e.mov_reg_imm64(Reg::rax, imm);
    e.mov_reg_reg(Reg::rsp, Reg::rbp);
    e.pop(Reg::rbp);
    e.ret();
    return CompiledFn{"ret_const", std::move(e.code), nullptr, nullptr};
}

CompiledFn compile_max_i64() {
    // fn max(a: i64, b: i64) -> i64 { if (a > b) return a; return b; }
    // a=rcx, b=rdx. cmp rcx, rdx; jle .else; mov rax, rcx; jmp .end;
    // .else: mov rax, rdx; .end: <epilogue>
    X64Emitter e;
    e.push(Reg::rbp);
    e.mov_reg_reg(Reg::rbp, Reg::rsp);
    e.cmp_reg_reg(Reg::rcx, Reg::rdx);
    Label else_l = e.alloc_label();
    Label end_l  = e.alloc_label();
    e.jcc(Cond::le, else_l);          // if a <= b, go to else (return b)
    e.mov_reg_reg(Reg::rax, Reg::rcx); // return a
    e.jmp(end_l);
    e.bind(else_l);
    e.mov_reg_reg(Reg::rax, Reg::rdx); // return b
    e.bind(end_l);
    e.mov_reg_reg(Reg::rsp, Reg::rbp);
    e.pop(Reg::rbp);
    e.ret();
    e.resolve_fixups();
    return CompiledFn{"max", std::move(e.code), nullptr, nullptr};
}

CompiledFn compile_fib_i64(int64_t table_base, uint32_t slot) {
    // fn fib(n: i64) -> i64 { if (n <= 1) return n; return fib(n-1)+fib(n-2); }
    // n=rcx. Callee-saved rbx=n, r12=fib(n-1).
    //
    // prologue: push rbp; mov rbp,rsp; push rbx; push r12
    // cmp rcx, 1; jle .base
    // push rbx; push r12
    // mov rbx, rcx              ; rbx = n
    // ; fib(n-1): rcx = n-1
    // mov rcx, rbx; sub rcx, 1
    // mov r11, table_base
    // call [r11 + slot*8]
    // mov r12, rax              ; save fib(n-1)
    // ; fib(n-2): rcx = n-2
    // mov rcx, rbx; sub rcx, 2
    // mov r11, table_base
    // call [r11 + slot*8]
    // add rax, r12              ; rax = fib(n-1) + fib(n-2)
    // pop r12; pop rbx
    // jmp .ret
    // .base: mov rax, rcx        ; return n
    // .ret: pop r12; pop rbx; mov rsp,rbp; pop rbp; ret
    X64Emitter e;
    e.push(Reg::rbp);
    e.mov_reg_reg(Reg::rbp, Reg::rsp);
    // (use rbx/r12 as callee-saved across the recursive calls)
    e.cmp_reg_imm32(Reg::rcx, 1);
    Label base_l = e.alloc_label();
    Label ret_l  = e.alloc_label();
    e.jcc(Cond::le, base_l);
    // n > 1 path
    e.push(Reg::rbx);
    e.push(Reg::r12);
    e.mov_reg_reg(Reg::rbx, Reg::rcx);       // rbx = n
    // fib(n-1)
    e.mov_reg_reg(Reg::rcx, Reg::rbx);
    e.sub_reg_imm32(Reg::rcx, 1);
    e.mov_reg_imm64(Reg::r11, table_base);
    e.call_mem(Reg::r11, int32_t(slot * 8));
    e.mov_reg_reg(Reg::r12, Reg::rax);       // r12 = fib(n-1)
    // fib(n-2)
    e.mov_reg_reg(Reg::rcx, Reg::rbx);
    e.sub_reg_imm32(Reg::rcx, 2);
    e.mov_reg_imm64(Reg::r11, table_base);
    e.call_mem(Reg::r11, int32_t(slot * 8));
    e.add_reg_reg(Reg::rax, Reg::r12);       // rax = fib(n-2) + fib(n-1)
    e.pop(Reg::r12);
    e.pop(Reg::rbx);
    e.jmp(ret_l);
    // base case
    e.bind(base_l);
    e.mov_reg_reg(Reg::rax, Reg::rcx);       // return n
    e.bind(ret_l);
    e.mov_reg_reg(Reg::rsp, Reg::rbp);
    e.pop(Reg::rbp);
    e.ret();
    e.resolve_fixups();
    return CompiledFn{"fib", std::move(e.code), nullptr, nullptr};
}

bool finalize(CompiledFn& fn) {
    std::vector<uint8_t> image = fn.bytes;
    image.insert(image.end(), fn.rodata.begin(), fn.rodata.end());
    if (!fn.rodata.empty()) {
        for (const auto& af : fn.abs_fixups) if (af.kind == AbsFixup::FunctionRodataBase && uint64_t(af.code_offset)+8 <= image.size()) {
            uint64_t v = 0; // patched after RW allocation below
            for(int i=0;i<8;++i) image[af.code_offset+i]=uint8_t(v>>(8*i));
        }
        void* page = alloc_executable_rw(image);
        if (!page) return false;
        uint8_t* bytes=static_cast<uint8_t*>(page);
        // Bounds-check symmetric with pass 1 above: a malformed fixup with
        // code_offset+8 > image.size() must not write past the allocated page.
        for(const auto& af:fn.abs_fixups) if(af.kind==AbsFixup::FunctionRodataBase && uint64_t(af.code_offset)+8 <= image.size()) {
            uint64_t v=reinterpret_cast<uintptr_t>(bytes+fn.bytes.size()+af.addend);
            for(int i=0;i<8;++i)bytes[af.code_offset+i]=uint8_t(v>>(8*i));
        }
        if(!seal_executable(page,image.size())){free_executable(page);return false;}
        fn.exec=fn.entry=page; return true;
    }
    fn.exec = alloc_executable(fn.bytes);
    fn.entry = fn.exec;
    return fn.exec != nullptr;
}

int64_t call_i64_i64_i64(void* entry, int64_t a, int64_t b) {
    using F = int64_t(*)(int64_t, int64_t);
    return reinterpret_cast<F>(entry)(a, b);
}

int64_t call_i64_i64(void* entry) {
    using F = int64_t(*)();
    return reinterpret_cast<F>(entry)();
}

CompiledFn compile_native_passthrough_2arg(void* native_fn) {
    // fn(p: i64, a: i64) -> i64 { return native(p, a); }
    // Win64: p=rcx, a=rdx, return=rax. The native also expects (i64,i64)
    // in rcx/rdx - both already in place from the wrapper's own args, so
    // just mov the native ptr into rax and call.
    //
    // docs/spec/CODEGEN_SPEC.md Section 1/Section 2: must reserve 32 bytes of shadow space below
    // the call (even though native takes <=4 args) - the C++-compiled
    // callee may spill rcx/rdx into it. Without this, the spills corrupt
    // our frame (observed crash).
    X64Emitter e;
    e.push(Reg::rbp);
    e.mov_reg_reg(Reg::rbp, Reg::rsp);
    e.sub_reg_imm32(Reg::rsp, 32);             // shadow space
    e.mov_reg_imm64(Reg::rax, int64_t(native_fn));
    e.call_reg(Reg::rax);
    e.mov_reg_reg(Reg::rsp, Reg::rbp);          // restore (drops shadow space)
    e.pop(Reg::rbp);
    e.ret();
    return CompiledFn{"native_call2", std::move(e.code), nullptr, nullptr};
}

// v1.0 thread-safety (Option B1): raw calls into JIT'd entries with context_t*
// installed in r14.  These are deliberately out-of-line ABI thunks: as normal
// exported Win64 functions they preserve the caller's incoming nonvolatile r14,
// reserve 32 bytes of shadow space, and keep rsp 16-byte aligned at the JIT call.
// They do NOT reset context state or establish a setjmp checkpoint.
#if defined(__GNUC__) && defined(__x86_64__)
extern "C" int64_t ember_call_void_thunk(void*, context_t*);
extern "C" int64_t ember_call_i64_thunk(void*, context_t*, int64_t);
extern "C" int64_t ember_call_i64_i64_thunk(void*, context_t*, int64_t, int64_t);

asm(
    ".text\n"
    ".p2align 4\n"
    ".globl ember_call_void_thunk\n"
    "ember_call_void_thunk:\n"
    "  pushq %r14\n"             // entry rsp%16=8; push -> aligned
    "  subq $32, %rsp\n"         // mandatory Win64 shadow space
    "  movq %rcx, %r11\n"        // entry
    "  movq %rdx, %r14\n"        // B1 context
    "  callq *%r11\n"
    "  addq $32, %rsp\n"
    "  popq %r14\n"
    "  retq\n"
    ".p2align 4\n"
    ".globl ember_call_i64_thunk\n"
    "ember_call_i64_thunk:\n"
    "  pushq %r14\n"
    "  subq $32, %rsp\n"
    "  movq %rcx, %r11\n"        // entry
    "  movq %rdx, %r14\n"        // B1 context
    "  movq %r8, %rcx\n"         // JIT arg 0
    "  callq *%r11\n"
    "  addq $32, %rsp\n"
    "  popq %r14\n"
    "  retq\n"
    ".p2align 4\n"
    ".globl ember_call_i64_i64_thunk\n"
    "ember_call_i64_i64_thunk:\n"
    "  pushq %r14\n"
    "  subq $32, %rsp\n"
    "  movq %rcx, %r11\n"        // entry
    "  movq %rdx, %r14\n"        // B1 context
    "  movq %r8, %rcx\n"         // JIT arg 0
    "  movq %r9, %rdx\n"         // JIT arg 1
    "  callq *%r11\n"
    "  addq $32, %rsp\n"
    "  popq %r14\n"
    "  retq\n"
);

int64_t ember_call_void(void* entry, context_t* ctx) {
    return ember_call_void_thunk(entry, ctx);
}
int64_t ember_call_i64(void* entry, context_t* ctx, int64_t a) {
    return ember_call_i64_thunk(entry, ctx, a);
}
int64_t ember_call_i64_i64(void* entry, context_t* ctx, int64_t a, int64_t b) {
    return ember_call_i64_i64_thunk(entry, ctx, a, b);
}

// ============================================================================
// Red 5 (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md §6.3, §6.4, §6.5,
//   §9.8): the keyed outer thunks + keyed re-entry thunks + test asm helpers.
//
// The keyed thunks install r14 = context_t* and r15 = transient route word,
// invoke the JIT'd entry, clear the transient r15 (xor r15,r15) BEFORE restoring
// the caller's original r14/r15, and return. They are out-of-line Win64
// functions: as exported Win64 callees they preserve the caller's incoming
// nonvolatile r14/r15 (pushed/popped), reserve 32 bytes of shadow space below
// the JIT call, and keep rsp 16-byte aligned at the JIT call. Two pushes (r14,
// r15) + sub $40 (32 shadow + 8 align pad) keeps rsp%16==0 at the call: entry
// rsp%16==8 (after `call`), push r14 -> 0, push r15 -> 8, sub 40 -> 0.
//
// The re-entry thunks (ember_keyed_reentry_*) share the exact same install/
// clean/restore sequence but take the route word as an explicit argument
// (the outer thunk's route word, captured by a re-entrant native). They do NOT
// derive or checkpoint — the enclosing outer call's checkpoint is still live.
//
// Argument mapping (Win64):
//   void_thunk(entry, ctx, route_word):       rcx=entry rdx=ctx r8=route_word
//   i64_thunk(entry, ctx, a, route_word):    rcx=entry rdx=ctx r8=a r9=route_word
//   i64_i64_thunk(entry, ctx, a, b, rw):     rcx=entry rdx=ctx r8=a r9=b rw=[rsp+0x28]
//
// After push r14; push r15; sub $40: rsp = entry - 56, so the 5th arg (rw) is
// at [rsp + 0x60] for the i64_i64 form (entry+0x28 = rsp+56+0x28 = rsp+0x60).
// ============================================================================
extern "C" int64_t ember_keyed_void_thunk(void*, context_t*, uint64_t);
extern "C" int64_t ember_keyed_i64_thunk(void*, context_t*, int64_t, uint64_t);
extern "C" int64_t ember_keyed_i64_i64_thunk(void*, context_t*, int64_t, int64_t, uint64_t);
extern "C" int64_t ember_keyed_reentry_void_thunk(void*, context_t*, uint64_t);
extern "C" int64_t ember_keyed_reentry_i64_thunk(void*, context_t*, int64_t, uint64_t);
extern "C" int64_t ember_keyed_reentry_i64_i64_thunk(void*, context_t*, int64_t, int64_t, uint64_t);

asm(
    ".text\n"
    ".p2align 4\n"
    ".globl ember_keyed_void_thunk\n"
    "ember_keyed_void_thunk:\n"
    "  pushq %r14\n"                  // caller r14
    "  pushq %r15\n"                  // caller r15
    "  subq $40, %rsp\n"              // 32 shadow + 8 align (rsp%16==0 at call)
    "  movq %rcx, %r11\n"             // entry
    "  movq %rdx, %r14\n"             // context_t*
    "  movq %r8, %r15\n"              // transient route word
    "  callq *%r11\n"
    "  xorq %r15, %r15\n"             // §6.3 step 5: clear transient r15 BEFORE restore
    "  addq $40, %rsp\n"
    "  popq %r15\n"                   // restore caller r15
    "  popq %r14\n"                   // restore caller r14
    "  retq\n"
    ".p2align 4\n"
    ".globl ember_keyed_i64_thunk\n"
    "ember_keyed_i64_thunk:\n"
    "  pushq %r14\n"
    "  pushq %r15\n"
    "  subq $40, %rsp\n"
    "  movq %rcx, %r11\n"             // entry
    "  movq %rdx, %r14\n"             // context_t*
    "  movq %r9, %r15\n"              // transient route word (4th arg)
    "  movq %r8, %rcx\n"              // JIT arg 0 (a)
    "  callq *%r11\n"
    "  xorq %r15, %r15\n"
    "  addq $40, %rsp\n"
    "  popq %r15\n"
    "  popq %r14\n"
    "  retq\n"
    ".p2align 4\n"
    ".globl ember_keyed_i64_i64_thunk\n"
    "ember_keyed_i64_i64_thunk:\n"
    "  pushq %r14\n"
    "  pushq %r15\n"
    "  subq $40, %rsp\n"
    // 5th arg (route_word) was at entry [rsp+0x28]; now at [rsp+0x60].
    "  movq 0x60(%rsp), %r15\n"       // transient route word
    "  movq %rcx, %r11\n"             // entry
    "  movq %rdx, %r14\n"             // context_t*
    "  movq %r8, %rcx\n"              // JIT arg 0 (a)
    "  movq %r9, %rdx\n"              // JIT arg 1 (b)
    "  callq *%r11\n"
    "  xorq %r15, %r15\n"
    "  addq $40, %rsp\n"
    "  popq %r15\n"
    "  popq %r14\n"
    "  retq\n"
    // ─── Re-entry thunks (§6.5): same install/clean/restore, route word as arg. ──
    ".p2align 4\n"
    ".globl ember_keyed_reentry_void_thunk\n"
    "ember_keyed_reentry_void_thunk:\n"
    "  pushq %r14\n"
    "  pushq %r15\n"
    "  subq $40, %rsp\n"
    "  movq %rcx, %r11\n"
    "  movq %rdx, %r14\n"
    "  movq %r8, %r15\n"
    "  callq *%r11\n"
    "  xorq %r15, %r15\n"
    "  addq $40, %rsp\n"
    "  popq %r15\n"
    "  popq %r14\n"
    "  retq\n"
    ".p2align 4\n"
    ".globl ember_keyed_reentry_i64_thunk\n"
    "ember_keyed_reentry_i64_thunk:\n"
    "  pushq %r14\n"
    "  pushq %r15\n"
    "  subq $40, %rsp\n"
    "  movq %rcx, %r11\n"
    "  movq %rdx, %r14\n"
    "  movq %r9, %r15\n"
    "  movq %r8, %rcx\n"
    "  callq *%r11\n"
    "  xorq %r15, %r15\n"
    "  addq $40, %rsp\n"
    "  popq %r15\n"
    "  popq %r14\n"
    "  retq\n"
    ".p2align 4\n"
    ".globl ember_keyed_reentry_i64_i64_thunk\n"
    "ember_keyed_reentry_i64_i64_thunk:\n"
    "  pushq %r14\n"
    "  pushq %r15\n"
    "  subq $40, %rsp\n"
    "  movq 0x60(%rsp), %r15\n"
    "  movq %rcx, %r11\n"
    "  movq %rdx, %r14\n"
    "  movq %r8, %rcx\n"
    "  movq %r9, %rdx\n"
    "  callq *%r11\n"
    "  xorq %r15, %r15\n"
    "  addq $40, %rsp\n"
    "  popq %r15\n"
    "  popq %r14\n"
    "  retq\n"
    // ─── Test asm helpers: read/set the caller's r15. ────────────────────
    ".p2align 4\n"
    ".globl ember_read_r15\n"
    "ember_read_r15:\n"
    "  movq %r15, %rax\n"
    "  retq\n"
    ".p2align 4\n"
    ".globl ember_set_r15\n"
    "ember_set_r15:\n"
    "  movq %rcx, %r15\n"
    "  retq\n"
);

int64_t ember_keyed_reentry_void(void* entry, context_t* ctx, uint64_t route_word) {
    return ember_keyed_reentry_void_thunk(entry, ctx, route_word);
}
int64_t ember_keyed_reentry_i64(void* entry, context_t* ctx, int64_t a, uint64_t route_word) {
    return ember_keyed_reentry_i64_thunk(entry, ctx, a, route_word);
}
int64_t ember_keyed_reentry_i64_i64(void* entry, context_t* ctx, int64_t a, int64_t b,
                                    uint64_t route_word) {
    return ember_keyed_reentry_i64_i64_thunk(entry, ctx, a, b, route_word);
}

uint64_t ember_read_r15() {
    uint64_t v;
    __asm__ ("movq %%r15, %0" : "=r"(v));
    return v;
}
void ember_set_r15(uint64_t v) {
    __asm__ ("movq %0, %%r15" : : "r"(v));
}

// ─── Safe keyed host-to-script call APIs (§9.8, §6.3) ───────────────────────
//
// Red 8: the C++ wrappers now (a) resolve the logical entry through the
// instance's CURRENT immutable ModuleDispatchRecord + the transient provider-
// derived route word via the Red 4 resolver (NOT raw entry_table[logical_slot],
// §9.8); (b) hold the applicable hot-reload/generation guard from resolution
// through return or trap (§9.8, §12.4) — the guard is acquired BEFORE
// resolution and released on every normal AND trapped exit (longjmp skips
// destructors, so the guard is manually reset(), not plain RAII across a trap);
// and (c) set the current-keyed-runtime TLS on entry and clear it on every
// normal AND trapped exit so extension natives running under the thunk find
// THIS runtime's per-runtime state (§6.6, §10.3). The caller's r14/r15 are
// restored by the thunk (normal) or by this wrapper's own callee-saved
// prologue/epilogue (trap — the longjmp lands here, and the C++ epilogue
// restores the wrapper's callee-saved r14/r15, which are the caller's values).
//
// `name` resolves against the instance's named_entries map (the host populates
// it from its compiled module's slots); "main" is the conventional entry. The
// instance's mode selects identity or keyed resolution through the record;
// the route word participates in keyed resolution (Red 4's resolver) and in
// r15 installation regardless of mode (the thunk reserves r15 for the whole
// call tree, §6.4).
//
// setjmp/longjmp UB note (§6.5, task constraint): no nontrivial C++ object
// with a destructor is allocated BETWEEN the setjmp and the longjmp. The
// generation guard (std::optional<ExecutionGuard>) is constructed BEFORE the
// setjmp (in the caller / before the core's setjmp), so it is not "between"
// setjmp and longjmp; its destructor runs when the frame is normally exited,
// and the manual reset() on the trapped path releases the domain before that.
// `out` is a reference to the caller's CallResult (lives in the caller's frame,
// never unwound), so assigning its std::string fields after a longjmp is safe.
namespace {

// ─── Red 8: the current keyed-runtime TLS (§6.6, §10.3) ──────────────────────
// thread_local so two concurrently entered OS threads each see their OWN
// current runtime. It identifies a RUNTIME (a ModuleInstance*); it carries NO
// route material (a bare pointer, §6.4). The keyed host boundary sets it on
// entry and clears it on every normal/trapped exit. Extension natives consult
// it (via ember_current_keyed_runtime) to find the current runtime's
// RuntimeExtensionState. Null when no keyed runtime is active on this thread.
thread_local ModuleInstance* g_current_keyed_runtime = nullptr;

// Derive the route word once from the provider via the adapter. Fills `rw`
// and returns true on success; on failure fills `reason` and returns false
// (the thunk is never entered). Used by the call wrappers + the resolvers.
bool derive_route_word(ModuleInstance& inst, const DispatchKeyAdapter& adapter,
                       uint64_t& rw, std::string& reason) {
    auto r = adapter.route_word(ModuleId{inst.module_id, 1}, inst.strategy_version,
                                "ember/dispatch");
    if (!r) {
        reason = r.error.has_value() ? r.error->message
                                      : std::string("route word derivation failed");
        return false;
    }
    rw = *r.value;
    return true;
}

// Resolve a logical slot to a non-null entry through the instance's immutable
// ModuleDispatchRecord + the transient route word (the Red 4 resolver). Returns
// the entry on success or a structured ExtensionError on failure (the safe API
// reports a structured CallResult; the thunk is never entered). This replaces
// the raw entry_table->get(slot) read — resolution now goes through the record
// (identity mode -> physical_slots[logical_slot]; keyed mode -> P(route_word,
// domain, ordinal)) so a keyed module's permuted physical table is never
// indexed by a bare logical slot (§9.8).
ExtensionResult<void*> resolve_slot_through_record(ModuleInstance& inst,
                                                   uint32_t logical_slot,
                                                   uint64_t route_word) {
    if (!inst.entry_table) {
        return make_extension_result_error<void*>(
            "ember-keyed-dispatch", "resolve", "null entry table");
    }
    // If the host did not assemble a record, assemble an identity one now
    // (defensive — make_identity_instance / the test harness assemble eagerly).
    if (inst.record.physical_slots == nullptr && inst.logical_slot_count > 0) {
        assemble_identity_dispatch_record(inst);
    }
    return resolve_keyed_dispatch(&inst.record, logical_slot, route_word);
}

// The shared outer-call core. `entry` is a pre-resolved non-null entry pointer;
// `route_word` is the derived transient route word; `a`/`b` are the i64 args
// (0 for the void form); `argc` selects the thunk (0=void, 1=i64, 2=i64_i64).
// `guard` is the generation guard acquired by the caller BEFORE resolution
// (§9.8, §12.4) — the core releases it on every exit (normal + trap). The TLS
// current-keyed-runtime is set on entry and cleared on every exit. The guard
// is constructed in the CALLER's frame (before this core is entered, and before
// any setjmp here), so no nontrivial object is allocated between the setjmp and
// a longjmp in this frame — the setjmp/longjmp UB rule is respected.
int64_t __attribute__((noinline)) keyed_call_core(ModuleInstance& inst, void* entry, context_t& ctx,
                        uint64_t route_word, int64_t a, int64_t b, int argc,
                        CallResult& out,
                        std::optional<HotReloadDomain::ExecutionGuard>& guard) {
    // Red 8 §6.6/§10.3: identify THIS runtime on the TLS so extension natives
    // running under the thunk find the correct per-runtime state. Cleared on
    // every exit below (normal + trap). Carries NO route material.
    ModuleInstance* prev_runtime = g_current_keyed_runtime;
    g_current_keyed_runtime = &inst;
    // Establish the recoverable checkpoint when the instance has a trap stub.
    // The trap stub longjmps to ctx.checkpoint; the wrapper records the trap,
    // cleans the TLS + guard, and returns. Without a trap stub, traps emit ud2
    // (the legacy default) and are a hard fault — no checkpoint is installed.
    // No nontrivial local is allocated between this setjmp and the thunk call
    // below (the thunk call is the last statement before the normal-exit
    // cleanup), so the longjmp UB rule is respected.
    if (inst.trap_stub) {
        ctx.has_checkpoint = true;
        if (EMBER_SETJMP(ctx.checkpoint)) {
            // Trapped exit: the trap stub longjmp'd back here. Clear the TLS +
            // release the guard (longjmp skipped their normal cleanup). The
            // transient r15 was in the abandoned thunk frame; this wrapper's
            // callee-saved r14/r15 are the caller's values (restored by the
            // C++ epilogue at return), so the caller's registers survive and
            // the transient r15 is cleared (it never escaped the thunk frame).
            g_current_keyed_runtime = prev_runtime;
            guard.reset();
            out.ok = false;
            out.trapped = true;
            out.value = 0;
            out.reason = ctx.last_error.empty()
                ? std::string(trap_reason_str(ctx.last_trap))
                : (std::string(trap_reason_str(ctx.last_trap)) + ": " + ctx.last_error);
            ctx.has_checkpoint = false;
            ctx.reset_for_call();
            return 0;
        }
    }
    int64_t r = 0;
    switch (argc) {
    case 0:
        r = ember_keyed_void_thunk(entry, &ctx, route_word);
        break;
    case 1:
        r = ember_keyed_i64_thunk(entry, &ctx, a, route_word);
        break;
    case 2:
        r = ember_keyed_i64_i64_thunk(entry, &ctx, a, b, route_word);
        break;
    }
    if (inst.trap_stub) ctx.has_checkpoint = false;
    // Normal exit: clear the TLS + release the guard.
    g_current_keyed_runtime = prev_runtime;
    guard.reset();
    out.ok = true;
    out.trapped = false;
    out.value = r;
    out.reason.clear();
    return r;
}

} // namespace

// ─── Red 8: the current keyed-runtime TLS accessor (§6.6, §10.3) ────────────
// g_current_keyed_runtime lives in the anonymous namespace above (inside
// ember); reachable here by unqualified name. Carries NO route material.
struct ModuleInstance* ember_current_keyed_runtime() noexcept {
    return g_current_keyed_runtime;
}

// ─── Red 8: assemble an identity ModuleDispatchRecord on a ModuleInstance ──
// (§9.8, §10.1). Builds a minimal identity record over the instance's borrowed
// entry_table + counts: one identity domain covering [0, logical_slot_count),
// logical routes where route[i].logical_slot == i, an all-set allowlist, and
// the physical slot storage pointing at the instance's entry_table. The keyed
// resolver then resolves a logical slot through the record (identity mode ->
// physical_slots[logical_slot]) instead of raw entry_table[logical_slot].
bool assemble_identity_dispatch_record(ModuleInstance& inst) {
    // Require a borrowed entry table + a nonzero logical count to assemble.
    if (!inst.entry_table) return false;
    const uint32_t n = inst.logical_slot_count;
    if (n == 0) {
        // An empty module: an all-zero record (no routes, no domains, no
        // allowlist). Resolution of any logical slot fails cleanly.
        inst.record_storage = RecordBuilderStorage{};
        inst.record = ModuleDispatchRecord{};
        inst.record.mode = DispatchMode::Identity;
        inst.record.physical_slots = nullptr;
        inst.record.physical_slot_count = 0;
        inst.record.logical_slot_count = 0;
        return true;
    }
    inst.record_storage = RecordBuilderStorage{};
    // One identity domain covering every logical slot (identity mode does not
    // permute; the domain is metadata the resolver cross-checks the route
    // against, so a real + complete record is assembled, not a degenerate one).
    DispatchDomain dom{};
    dom.module_id = inst.module_id;
    dom.visibility = Visibility::Public;
    dom.calling_mode = CallingMode::LegacyContext;
    dom.abi_fingerprint = 0;          // identity record: no per-domain ABI gate
    dom.dispatch_domain = "";
    dom.domain_salt = 0;              // identity mode does not permute (no salt)
    dom.strategy_version = inst.strategy_version;
    dom.physical_base = 0;
    dom.physical_count = n;
    dom.logical_count = n;
    dom.padding_count = 0;
    dom.padding_ordinal = n;          // no padding in identity mode
    dom.logical_slots.resize(n);
    for (uint32_t i = 0; i < n; ++i) dom.logical_slots[i] = i;
    inst.record_storage.domains.clear();
    inst.record_storage.domains.push_back(std::move(dom));
    // Logical routes: route[i].logical_slot == i, domain 0, ordinal i.
    inst.record_storage.routes.clear();
    inst.record_storage.routes.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        LogicalRoute& r = inst.record_storage.routes[i];
        r.logical_slot = i;
        r.domain_index = 0;
        r.ordinal = i;
        r.abi_fingerprint = 0;
        r.visibility = Visibility::Public;
        r.calling_mode = CallingMode::LegacyContext;
        r.dispatch_domain = "";
    }
    // Allowlist: every logical slot is a registered callable (all-set).
    inst.record_storage.allowlist.assign((size_t(n) + 7) / 8, 0);
    for (uint32_t i = 0; i < n; ++i)
        inst.record_storage.allowlist[size_t(i) / 8] |= uint8_t(1u << (i % 8));
    // Physical slot storage: identity mode borrows the instance's entry_table
    // storage directly (the DispatchTable's public atomic slots vector). The
    // record's physical_slots points at slots.data(); publication semantics
    // are the table's. (record_storage.storage is left empty — the borrowed
    // table owns the atomic slots; the record only borrows the base pointer.)
    inst.record = ModuleDispatchRecord{};
    inst.record.mode = DispatchMode::Identity;
    inst.record.strategy_version = inst.strategy_version;
    inst.record.physical_slots = inst.entry_table->slots.data();
    inst.record.physical_slot_count = n;
    inst.record.logical_slot_count = n;
    inst.record.logical_routes = inst.record_storage.routes.data();
    inst.record.domains = inst.record_storage.domains.data();
    inst.record.domain_count = 1;
    inst.record.logical_allowlist = inst.record_storage.allowlist.data();
    inst.record.logical_allowlist_bytes = uint32_t(inst.record_storage.allowlist.size());
    // Identity mode needs no per-physical-slot descriptors; leave them null.
    inst.record.physical_descriptors = nullptr;
    inst.record.physical_descriptor_count = 0;
    inst.record.padding_trap_target = nullptr;
    return true;
}

// ─── Red 8: structured keyed entry resolvers (§9.8) ────────────────────────
// These hold the generation guard for the duration of the resolution ONLY —
// the guard is released before the pointer is returned, so a returned pointer
// that ESCAPES the resolver is NOT protected by it. A host that lets the
// pointer escape MUST use resolve_entry_keyed_leased (which holds the guard
// across body) or take its own guard — never assume this guard outlives the
// call (§12.4, task constraint: do not falsely claim a dropped guard protects
// an escaped pointer).
ExtensionResult<void*> resolve_entry_keyed(ModuleInstance& inst,
                                           const LogicalCallableId& id,
                                           const DispatchKeyAdapter& adapter) {
    uint64_t rw = 0;
    std::string reason;
    if (!derive_route_word(inst, adapter, rw, reason)) {
        return make_extension_result_error<void*>(
            "ember-keyed-dispatch", "resolve", reason);
    }
    // Hold the generation guard across the resolution (§9.8, §12.4). Acquired
    // before the resolve, released before return. The route word is transient.
    std::optional<HotReloadDomain::ExecutionGuard> guard;
    if (inst.reload_domain) guard.emplace(inst.reload_domain->guard());
    auto sr = resolve_slot_through_record(inst, id.logical_slot, rw);
    guard.reset();
    return sr;
}

ExtensionResult<void*> resolve_entry_by_name_keyed(ModuleInstance& inst,
                                                   std::string_view name,
                                                   const DispatchKeyAdapter& adapter) {
    auto it = inst.named_entries.find(std::string(name));
    if (it == inst.named_entries.end()) {
        return make_extension_result_error<void*>(
            "ember-keyed-dispatch", "resolve",
            "keyed call: entry '" + std::string(name) + "' not found in module '" +
            inst.module_id + "'");
    }
    LogicalCallableId id{it->second, 0};
    return resolve_entry_keyed(inst, id, adapter);
}

// ─── Red 8: the lifetime-safe scoped-lease resolver (§9.8, §12.4) ───────────
// Holds the generation guard for the duration of body(entry) and releases it
// on normal return OR a recovered trap inside body. This is the sanctioned
// form for a resolved pointer that must escape the bare resolver's guarded
// region: the guard + TLS are live for body's whole execution, so the entry is
// protected for as long as it is actually used.
LeaseResult resolve_entry_keyed_leased(ModuleInstance& inst,
                                       const LogicalCallableId& id,
                                       context_t& ctx, int64_t arg,
                                       KeyedLeaseBody body,
                                       const DispatchKeyAdapter& adapter) {
    LeaseResult out;
    if (!body) {
        out.reason = "keyed lease: null body"; return out;
    }
    uint64_t rw = 0;
    std::string reason;
    if (!derive_route_word(inst, adapter, rw, reason)) {
        out.reason = reason; return out;
    }
    // Acquire the generation guard BEFORE resolution (§9.8, §12.4) and hold it
    // across body. Constructed before the setjmp below so no nontrivial object
    // is allocated between the setjmp and a longjmp (UB rule respected).
    std::optional<HotReloadDomain::ExecutionGuard> guard;
    if (inst.reload_domain) guard.emplace(inst.reload_domain->guard());
    auto sr = resolve_slot_through_record(inst, id.logical_slot, rw);
    if (!sr) {
        guard.reset();
        out.reason = sr.error.has_value() ? sr.error->message : std::string("resolve failed");
        return out;
    }
    void* entry = *sr.value;
    if (!entry) { guard.reset(); out.reason = "keyed lease: resolved entry is null"; return out; }
    // Identify THIS runtime on the TLS for body (extension natives may run).
    ModuleInstance* prev_runtime = g_current_keyed_runtime;
    g_current_keyed_runtime = &inst;
    if (inst.trap_stub) {
        ctx.has_checkpoint = true;
        if (EMBER_SETJMP(ctx.checkpoint)) {
            // Trapped inside body: clean TLS + guard (longjmp skipped them).
            g_current_keyed_runtime = prev_runtime;
            guard.reset();
            out.ok = false;
            out.trapped = true;
            out.reason = ctx.last_error.empty()
                ? std::string(trap_reason_str(ctx.last_trap))
                : (std::string(trap_reason_str(ctx.last_trap)) + ": " + ctx.last_error);
            ctx.has_checkpoint = false;
            ctx.reset_for_call();
            return out;
        }
    }
    int64_t v = body(entry, &ctx, arg);
    if (inst.trap_stub) ctx.has_checkpoint = false;
    g_current_keyed_runtime = prev_runtime;
    guard.reset();
    out.ok = true;
    out.value = v;
    return out;
}

// ─── Red 8: shared name/slot -> CallResult driver ───────────────────────────
// Derives the route word, acquires the generation guard BEFORE resolution,
// resolves through the record, and enters the keyed call core. Used by both
// the retained name forms and the by-slot overloads so the guard-acquire-order
// + cleanup invariants are identical across every entry shape.
static CallResult keyed_call_driver(ModuleInstance& inst, uint32_t logical_slot,
                                    context_t& ctx, int64_t a, int64_t b, int argc,
                                    const DispatchKeyAdapter& adapter) {
    CallResult out;
    uint64_t rw = 0; std::string reason;
    if (!derive_route_word(inst, adapter, rw, reason)) {
        out.ok = false; out.reason = reason; return out;
    }
    // Acquire the generation guard BEFORE resolution (§9.8, §12.4) and hold it
    // through return or trap. Constructed before keyed_call_core's setjmp so
    // no nontrivial object is allocated between that setjmp and a longjmp.
    std::optional<HotReloadDomain::ExecutionGuard> guard;
    if (inst.reload_domain) guard.emplace(inst.reload_domain->guard());
    auto sr = resolve_slot_through_record(inst, logical_slot, rw);
    if (!sr) {
        guard.reset();
        out.ok = false;
        out.reason = sr.error.has_value() ? sr.error->message : std::string("resolve failed");
        return out;
    }
    void* entry = *sr.value;
    if (!entry) { guard.reset(); out.ok = false; out.reason = "keyed call: resolved entry is null"; return out; }
    keyed_call_core(inst, entry, ctx, rw, a, b, argc, out, guard);
    return out;
}

// ─── Retained name forms (§9.8) ────────────────────────────────────────────
CallResult ember_call_keyed_void(ModuleInstance& inst, const std::string& name,
                                 context_t& ctx, const DispatchKeyAdapter& adapter) {
    auto it = inst.named_entries.find(name);
    if (it == inst.named_entries.end()) {
        CallResult out;
        out.ok = false;
        out.reason = "keyed call: entry '" + name + "' not found in module '" + inst.module_id + "'";
        return out;
    }
    return keyed_call_driver(inst, it->second, ctx, 0, 0, 0, adapter);
}

CallResult ember_call_keyed_i64(ModuleInstance& inst, const std::string& name,
                                context_t& ctx, int64_t a,
                                const DispatchKeyAdapter& adapter) {
    auto it = inst.named_entries.find(name);
    if (it == inst.named_entries.end()) {
        CallResult out;
        out.ok = false;
        out.reason = "keyed call: entry '" + name + "' not found";
        return out;
    }
    return keyed_call_driver(inst, it->second, ctx, a, 0, 1, adapter);
}

CallResult ember_call_keyed_i64_i64(ModuleInstance& inst, const std::string& name,
                                    context_t& ctx, int64_t a, int64_t b,
                                    const DispatchKeyAdapter& adapter) {
    auto it = inst.named_entries.find(name);
    if (it == inst.named_entries.end()) {
        CallResult out;
        out.ok = false;
        out.reason = "keyed call: entry '" + name + "' not found";
        return out;
    }
    return keyed_call_driver(inst, it->second, ctx, a, b, 2, adapter);
}

// ─── Red 8: by-logical-slot overloads (§9.8, §12.4) ─────────────────────────
CallResult ember_call_keyed_void_by_slot(ModuleInstance& inst, uint32_t logical_slot,
                                         context_t& ctx,
                                         const DispatchKeyAdapter& adapter) {
    return keyed_call_driver(inst, logical_slot, ctx, 0, 0, 0, adapter);
}

CallResult ember_call_keyed_i64_by_slot(ModuleInstance& inst, uint32_t logical_slot,
                                        context_t& ctx, int64_t a,
                                        const DispatchKeyAdapter& adapter) {
    return keyed_call_driver(inst, logical_slot, ctx, a, 0, 1, adapter);
}

CallResult ember_call_keyed_i64_i64_by_slot(ModuleInstance& inst, uint32_t logical_slot,
                                            context_t& ctx, int64_t a, int64_t b,
                                            const DispatchKeyAdapter& adapter) {
    return keyed_call_driver(inst, logical_slot, ctx, a, b, 2, adapter);
}
#else
#error "B1 context thunks require MinGW GNU assembly on x64; MSVC x64 not yet supported; use MinGW"
#endif

} // namespace ember
