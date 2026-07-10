#include "engine.hpp"
#include <stdexcept>

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
    // CODEGEN_SPEC.md Section 1/Section 2: must reserve 32 bytes of shadow space below
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
);

int64_t ember_call_void(void* entry, context_t* ctx) {
    return ember_call_void_thunk(entry, ctx);
}
int64_t ember_call_i64(void* entry, context_t* ctx, int64_t a) {
    return ember_call_i64_thunk(entry, ctx, a);
}
#else
#error "B1 context thunks require MinGW GNU assembly on x64; MSVC x64 not yet supported; use MinGW"
#endif

} // namespace ember
