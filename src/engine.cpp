#include "engine.hpp"
#include "module_instance.hpp"
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

uint64_t ember_read_r15() { return 0; /* placeholder; asm above provides the symbol */ }
void ember_set_r15(uint64_t /*v*/) { /* placeholder; asm above provides the symbol */ }

// ─── Safe keyed host-to-script call APIs (§9.8, §6.3) ───────────────────────
//
// The C++ wrappers. They derive the route word once from the provider via the
// adapter (provider failure -> structured CallResult failure, thunk never
// entered), resolve the logical entry against the instance's dispatch record,
// establish a setjmp checkpoint on the supplied context_t (when the instance
// has a trap stub), install r14/r15 via the keyed asm thunk, invoke, and report
// a structured CallResult on every normal/trap path. The caller's r14/r15 are
// restored by the thunk (normal) or by this wrapper's own callee-saved
// prologue/epilogue (trap — the longjmp lands here, and the C++ epilogue
// restores the wrapper's callee-saved r14/r15, which are the caller's values).
//
// `name` resolves against the instance's entry_table (the host's CompiledFn
// dispatch table). "main" is the conventional entry; any exported name works.
// The instance's mode selects identity resolution (the entry table is indexed
// by logical slot); keyed resolution through the Red 4 resolver is the Red 6/
// Red 7 call-lowering emit, NOT this phase — the outer thunk's contract is the
// r15 lifecycle, which holds regardless of whether the resolver permutes (§6.4:
// reserve r15 for the whole call tree).
namespace {

// Resolve `name` to a non-null entry pointer against the instance's dispatch
// record. Returns nullptr on a missing/bad identity (the safe API reports a
// structured failure; the thunk is never entered).
void* resolve_named_entry(ModuleInstance& inst, const std::string& name) {
    if (!inst.entry_table) return nullptr;
    // The host's CompiledFn table is keyed by logical slot, with the slot map
    // held by the host's Compiled struct. The instance borrows the table; the
    // name->slot lookup is the host's responsibility in this phase — the safe
    // API uses the instance's `named_entries` map when the host populated it,
    // falling back to a slot-0 "main" convention. For the Red 5 thunk contract
    // we resolve via the instance's optional name map; a host that compiled
    // with the test harness populates it.
    auto it = inst.named_entries.find(name);
    if (it == inst.named_entries.end()) return nullptr;
    uint32_t slot = it->second;
    if (slot >= inst.physical_slot_count) return nullptr;
    void* e = inst.entry_table->get(slot);
    return e;
}

// The shared outer-call core. Returns the i64 result + fills `out` with the
// structured CallResult. `entry` is a pre-resolved non-null entry pointer;
// `route_word` is the derived transient route word; `a`/`b` are the i64 args
// (0 for the void form); `argc` selects the thunk (0=void, 1=i64, 2=i64_i64).
int64_t keyed_call_core(ModuleInstance& inst, void* entry, context_t& ctx,
                        uint64_t route_word, int64_t a, int64_t b, int argc,
                        CallResult& out) {
    // Establish the recoverable checkpoint when the instance has a trap stub.
    // The trap stub (a host function the JIT'd code calls on a trap) longjmps
    // to ctx.checkpoint; the wrapper records the trap in `out` and returns.
    // Without a trap stub, traps emit ud2 (the legacy default) and are a hard
    // fault — the wrapper does NOT install a checkpoint in that case.
    if (inst.trap_stub) {
        ctx.has_checkpoint = true;
        if (__builtin_setjmp(ctx.checkpoint)) {
            // Trapped exit: the trap stub longjmp'd back here. The transient
            // r15 was in the abandoned thunk frame; this wrapper's callee-saved
            // r14/r15 are the caller's values (restored by the C++ epilogue at
            // return), so the caller's registers survive and the transient r15
            // is cleared (it never escaped the thunk frame). Report the trap.
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
    out.ok = true;
    out.trapped = false;
    out.value = r;
    out.reason.clear();
    return r;
}

} // namespace

CallResult ember_call_keyed_void(ModuleInstance& inst, const std::string& name,
                                 context_t& ctx, const DispatchKeyAdapter& adapter) {
    CallResult out;
    // 1. Derive the route word ONCE (§6.3 step 1). Provider failure -> the
    //    thunk is never entered; report a structured failure.
    auto rw = adapter.route_word(ModuleId{inst.module_id, 1}, inst.strategy_version,
                                 "ember/dispatch");
    if (!rw) {
        out.ok = false;
        out.trapped = false;
        out.value = 0;
        out.reason = rw.error.has_value() ? rw.error->message : std::string("route word derivation failed");
        return out;
    }
    // 2. Resolve the logical entry.
    void* entry = resolve_named_entry(inst, name);
    if (!entry) {
        out.ok = false;
        out.trapped = false;
        out.value = 0;
        out.reason = "keyed call: entry '" + name + "' not found in module '" + inst.module_id + "'";
        return out;
    }
    // 3-7. Checkpoint, install r14/r15, invoke, clean, restore.
    keyed_call_core(inst, entry, ctx, *rw.value, 0, 0, 0, out);
    return out;
}

CallResult ember_call_keyed_i64(ModuleInstance& inst, const std::string& name,
                                context_t& ctx, int64_t a,
                                const DispatchKeyAdapter& adapter) {
    CallResult out;
    auto rw = adapter.route_word(ModuleId{inst.module_id, 1}, inst.strategy_version,
                                 "ember/dispatch");
    if (!rw) {
        out.ok = false; out.reason = rw.error.has_value() ? rw.error->message : std::string("route word derivation failed");
        return out;
    }
    void* entry = resolve_named_entry(inst, name);
    if (!entry) {
        out.ok = false; out.reason = "keyed call: entry '" + name + "' not found";
        return out;
    }
    keyed_call_core(inst, entry, ctx, *rw.value, a, 0, 1, out);
    return out;
}

CallResult ember_call_keyed_i64_i64(ModuleInstance& inst, const std::string& name,
                                    context_t& ctx, int64_t a, int64_t b,
                                    const DispatchKeyAdapter& adapter) {
    CallResult out;
    auto rw = adapter.route_word(ModuleId{inst.module_id, 1}, inst.strategy_version,
                                 "ember/dispatch");
    if (!rw) {
        out.ok = false; out.reason = rw.error.has_value() ? rw.error->message : std::string("route word derivation failed");
        return out;
    }
    void* entry = resolve_named_entry(inst, name);
    if (!entry) {
        out.ok = false; out.reason = "keyed call: entry '" + name + "' not found";
        return out;
    }
    keyed_call_core(inst, entry, ctx, *rw.value, a, b, 2, out);
    return out;
}
#else
#error "B1 context thunks require MinGW GNU assembly on x64; MSVC x64 not yet supported; use MinGW"
#endif

} // namespace ember
