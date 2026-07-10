// binding_abi_test - script->native call ABI correctness suite.
//
// Pins the Win64 script->native calling convention implemented in
// src/codegen.cpp's generic CallExpr path (the word-placement logic in
// eval() ~line 1334-1427) and the struct-returning-call hidden-pointer
// path (eval_struct_returning_call ~line 467-536) per docs/spec/BINDING_API.md
// Section 4. The codegen is already correct; this suite exists so a
// future tree-walker->SSA-IR refactor cannot silently break it.
//
// Each test registers a FRESH host C++ function via BindingBuilder
// (src/binding_builder.hpp - the v0.3 ergonomic helper), writes a tiny
// .ember that calls it, runs the full parse->sema->codegen->finalize->call
// pipeline (mirroring examples/em_roundtrip_test.cpp lines 100-175), and
// asserts the host received the values in the registers the ABI promises.
//
// The suite links all six ember_ext_* libs (the vec extension is exercised
// for the struct-handle return comparison in test [2], and the math
// extension's sqrt is a second pin of the f32 path in test [4]) but the
// ABI-under-test natives are the fresh host fns registered here via
// BindingBuilder, so this test is INDEPENDENT of the parallel
// extension-refactor (extension .cpp internals may change; the registered
// names/signatures stay identical, and we only call the few we use by
// registered name through the table).
//
// Pipeline shape copied from em_roundtrip_test.cpp:
//   tokenize -> parse -> slot assignment -> sema(natives, slots, overloads,
//   structs) -> GlobalsBlock -> DispatchTable + CodeGenCtx -> compile_func
//   per fn -> finalize -> table.set -> call. The only difference is the
//   native table is populated via BindingBuilder instead of left empty.

#include "../src/engine.hpp"
#include "../src/dispatch_table.hpp"
#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"
#include "../src/codegen.hpp"
#include "../src/binding_builder.hpp"
#include "../src/jit_memory.hpp"

#include "ext_vec.hpp"
#include "ext_math.hpp"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Fresh host natives - the ABI under test. One per Win64 ABI class. Each is
// a plain C++ function with a signature matching the NativeSig we register
// for it; the host compiler lays its params/return out per Win64, and
// codegen bakes `mov rax, imm64(fn); call rax`, so the script call lands on
// the host's real entry with zero marshalling. A mismatch here means the
// script placed a value in the wrong slot (an ABI bug we want to surface).
// ---------------------------------------------------------------------------

// [1] struct-by-value ARG, <=8B. Win64: an 8-byte aggregate is passed in
// ONE GP register (rcx, word 0). The host reads the struct's fields out of
// that one register. Script struct Pair8 { x: i32; y: i32; } = 8B.
struct Pair8 { int32_t x; int32_t y; };
static int64_t n_add_pair8(Pair8 p) { return int64_t(p.x) + int64_t(p.y); }

// Ember-declared structs use the packed declaration-order layout in sema.hpp,
// not the host compiler's default padding. This matching host shape proves a
// nested aggregate whose natural C layout would differ still occupies one
// supported 8-byte Win64 aggregate slot.
#pragma pack(push, 1)
struct PackedInner { uint8_t a; uint16_t b; };
struct PackedOuter { uint8_t tag; int32_t value; PackedInner inner; };
#pragma pack(pop)
static_assert(sizeof(PackedInner) == 3 && sizeof(PackedOuter) == 8);
static int64_t n_read_packed(PackedOuter p) {
    return int64_t(p.inner.a) + int64_t(p.inner.b) + int64_t(p.tag) + int64_t(p.value);
}

// [2] struct-by-value RETURN, >8B, via the hidden-pointer path. Win64: a
// >8-byte aggregate is returned through a hidden first pointer arg (rcx),
// and the real args shift to rdx/r8/r9. Script struct Vec3s { x: f32; y: f32;
// z: f32; } = 12B -> eval_struct_returning_call reserves word 0 for the dest
// pointer. The host fn writes the struct through its hidden pointer arg.
struct Vec3s { float x; float y; float z; };
static void n_make_vec3s(Vec3s* dest, float x, float y, float z) {
    dest->x = x; dest->y = y; dest->z = z;
}

// Host shape for the struct-literal-return / struct-ret-call-as-arg probes.
// A 3x f32 packed aggregate = 12B (matches ember's packed declaration-order
// layout for `struct V3 { x: f32; y: f32; z: f32; }`). Used both as the host
// read-back struct for a script fn that RETURNS a V3 (the hidden-pointer
// return path - the host allocates a V3, passes &it in rcx, the script writes
// through it, the host reads x/y/z back: a NON-CIRCULAR direct-value probe) and
// as the struct-by-value call-argument shape inside the script itself.
struct V3 { float x; float y; float z; };
static_assert(sizeof(V3) == 12, "V3 must be a packed 12-byte 3x f32 aggregate");

// [3] >4 args -> args 5,6 spill to the outgoing stack past the 32-byte shadow
// space. Win64: rcx,rdx,r8,r9 for args 1-4; [rsp+32], [rsp+40] for 5,6.
static int64_t n_sum6(int64_t a, int64_t b, int64_t c, int64_t d,
                      int64_t e, int64_t f) {
    return a + b + c + d + e + f;
}

// [4] f32 args -> xmm0/xmm1 (slot-parallel: word 0 = xmm0, word 1 = xmm1).
static float n_fadd(float a, float b) { return a + b; }

// [5] slice arg -> 2 words: ptr (word 0 = rcx), len (word 1 = rdx). A
// script slice `arr[..]` lowers to {rax=ptr, rdx=len} and is stashed as two
// words, then placed rcx/rdx. Host reads p[0].
static int64_t n_first_byte(uint8_t* p, int64_t len) {
    (void)len;
    return (p && len > 0) ? int64_t(p[0]) : int64_t(-1);
}

// [6] mixed int+float, slot-parallel. Win64: a=i64 -> word0 -> rcx;
// b=f32 -> word1 -> xmm1 (NOT xmm0 - slot-parallel, the float takes the slot
// its POSITION reserves, not the first free float reg); c=i64 -> word2 ->
// r8. THIS IS THE KEY SLOT-PARALLEL TEST.
static int64_t n_mixed_if(int64_t a, float b, int64_t c) {
    return a + int64_t(b) + c;
}

// ---------------------------------------------------------------------------
// Compile-and-call pipeline (the em_roundtrip_test shape). Compiles a source
// string against a native table + empty overloads, returns the dispatch
// table + compiled fns so the caller can invoke the entry by slot. Calls
// back into the caller's `call` lambda with the entry pointer for the named
// function, so each test asserts in its own call shape.
// ---------------------------------------------------------------------------

struct CompiledProgram {
    std::vector<ember::CompiledFn> fns;
    ember::DispatchTable table;
    std::unordered_map<std::string, int> slots;
    std::vector<uint8_t> globals_store;
    std::vector<const ember::FuncDecl*> decls; // for return-type introspection
};

// Compile `src` against `natives`/`overlays`. On any stage failure, prints
// the error and returns nullptr. `entry_name` is sanity-checked to exist.
static CompiledProgram* compile_program(const std::string& src,
                                        const ember::NativeTable& tab,
                                        void* trap_stub = nullptr) {
    using namespace ember;
    auto* prog = new CompiledProgram{ {}, DispatchTable(0), {}, {}, {} };

    // lex
    auto lr = tokenize(src, "<binding_abi>");
    if (!lr.ok) { std::printf("FAIL: lex: %s\n", lr.error.c_str()); delete prog; return nullptr; }

    // parse
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::printf("FAIL: parse: %s\n", pr.error.c_str()); delete prog; return nullptr; }

    // slot assignment
    int si = 0;
    for (auto& fn : pr.program.funcs) { prog->slots[fn.name] = si++; fn.slot = prog->slots[fn.name]; }
    prog->table = DispatchTable(pr.program.funcs.size());

    // struct layouts from script-declared structs
    auto struct_layouts = build_struct_layouts(pr.program);
    pr.program.string_xor_key = 0; // raw rodata (encryption off for this ABI test)

    // sema against the BindingBuilder-built table
    auto sr = sema(pr.program, tab.natives, prog->slots, 0, &tab.overloads, &struct_layouts);
    if (!sr.ok) {
        std::printf("FAIL: sema (%zu errors):\n", sr.errors.size());
        for (auto& e : sr.errors) std::printf("  line %u: %s\n", e.line, e.msg.c_str());
        delete prog; return nullptr;
    }

    // globals block (empty unless the script declares globals)
    GlobalsBlock gb;
    {
        uint32_t gi = 0;
        for (auto& g : pr.program.globals) { gb.index[g.name] = gi++; gb.types[g.name] = g.ty.get(); }
    }
    prog->globals_store.assign(pr.program.globals.size() * 8, 0);
    gb.base = int64_t(prog->globals_store.data());
    g_globals_for_codegen = &gb;

    // codegen ctx
    CodeGenCtx ctx;
    ctx.globals_base = gb.base;
    ctx.dispatch_base = int64_t(prog->table.base());
    ctx.natives = &tab.natives;
    ctx.script_slots = &prog->slots;
    ctx.structs = &struct_layouts;
    ctx.trap_stub = trap_stub;

    // compile + finalize each function
    prog->fns.reserve(pr.program.funcs.size());
    for (auto& fn : pr.program.funcs) {
        prog->decls.push_back(&fn);
        CompiledFn cf = compile_func(fn, ctx);
        if (!finalize(cf)) {
            std::printf("FAIL: alloc_executable for %s\n", fn.name.c_str());
            for (auto& done : prog->fns) if (done.exec) free_executable(done.exec);
            delete prog; return nullptr;
        }
        prog->table.set(fn.slot, cf.entry);
        prog->fns.push_back(std::move(cf));
    }
    return prog;
}

static void free_program(CompiledProgram* p) {
    if (!p) return;
    for (auto& fn : p->fns) if (fn.exec) ember::free_executable(fn.exec);
    delete p;
}

static void* entry_of(CompiledProgram* p, const char* name) {
    auto it = p->slots.find(name);
    return it == p->slots.end() ? nullptr : p->table.get(it->second);
}

// Win64 call trampolines (typed reinterpret of the JIT entry).
static int64_t  call0_i64 (void* e)                                    { using F=int64_t(*)();    return reinterpret_cast<F>(e)(); }
static float    call0_f32 (void* e)                                    { using F=float(*)();      return reinterpret_cast<F>(e)(); }
// Call a no-arg script fn that returns a >8B struct via the hidden-pointer
// path (Win64: caller passes the dest address in rcx, callee writes the
// struct through it and returns the same pointer in rax). The host allocates
// the dest, passes &it, and reads the fields back directly - a non-circular
// direct-value read (in-language struct equality could circularly hide a
// codegen bug; the host C struct read cannot).
static V3       call0_struct_v3(void* e)                               { V3 r; using F=V3*(*)(V3*); reinterpret_cast<F>(e)(&r); return r; }

#if defined(__GNUC__) && defined(__x86_64__)
extern "C" uint64_t call_check_nonvolatiles(void* entry);
extern "C" void binding_aligned_trap(ember::context_t*, int, const char*);
extern "C" int64_t binding_call_i64_aligned(void*, int64_t);
asm(
    ".section .rdata,\"dr\"\n"
    ".p2align 4\n"
    "abi_xmm_seed: .quad 0x0123456789abcdef, 0xfedcba9876543210\n"
    ".text\n"
    ".p2align 4\n"
    ".globl binding_aligned_trap\n"
    "binding_aligned_trap:\n"
    "  movaps %xmm0, 8(%rsp)\n"
    "  movq %rdx, %rax\n"
    "  retq\n"
    ".p2align 4\n"
    ".globl binding_call_i64_aligned\n"
    "binding_call_i64_aligned:\n"
    "  pushq %r12\n"
    "  subq $32, %rsp\n"
    "  movq %rcx, %r11\n"
    "  movq %rdx, %rcx\n"
    "  callq *%r11\n"
    "  addq $32, %rsp\n"
    "  popq %r12\n"
    "  retq\n"
    ".p2align 4\n"
    ".globl call_check_nonvolatiles\n"
    "call_check_nonvolatiles:\n"
    "  pushq %rbx\n  pushq %rsi\n  pushq %rdi\n  pushq %r12\n"
    "  pushq %r13\n  pushq %r14\n  pushq %r15\n"
    "  subq $200, %rsp\n"
    "  movdqu %xmm6, 32(%rsp)\n  movdqu %xmm7, 48(%rsp)\n"
    "  movdqu %xmm8, 64(%rsp)\n  movdqu %xmm9, 80(%rsp)\n"
    "  movdqu %xmm10, 96(%rsp)\n  movdqu %xmm11, 112(%rsp)\n"
    "  movdqu %xmm12, 128(%rsp)\n  movdqu %xmm13, 144(%rsp)\n"
    "  movdqu %xmm14, 160(%rsp)\n  movdqu %xmm15, 176(%rsp)\n"
    "  movabsq $0x1122334455667788, %rbx\n"
    "  movabsq $0x1122334455667788, %rsi\n"
    "  movabsq $0x1122334455667788, %rdi\n"
    "  movabsq $0x1122334455667788, %r12\n"
    "  movabsq $0x1122334455667788, %r13\n"
    "  movabsq $0x1122334455667788, %r14\n"
    "  movabsq $0x1122334455667788, %r15\n"
    "  movdqu abi_xmm_seed(%rip), %xmm6\n  movdqa %xmm6, %xmm7\n"
    "  movdqa %xmm6, %xmm8\n  movdqa %xmm6, %xmm9\n"
    "  movdqa %xmm6, %xmm10\n  movdqa %xmm6, %xmm11\n"
    "  movdqa %xmm6, %xmm12\n  movdqa %xmm6, %xmm13\n"
    "  movdqa %xmm6, %xmm14\n  movdqa %xmm6, %xmm15\n"
    "  callq *%rcx\n"
    "  xorq %r10, %r10\n"
    "  movabsq $0x1122334455667788, %rax\n"
    "  cmpq %rax, %rbx\n  je 1f\n  orq $1, %r10\n1:\n"
    "  cmpq %rax, %rsi\n  je 2f\n  orq $2, %r10\n2:\n"
    "  cmpq %rax, %rdi\n  je 3f\n  orq $4, %r10\n3:\n"
    "  cmpq %rax, %r12\n  je 4f\n  orq $8, %r10\n4:\n"
    "  cmpq %rax, %r13\n  je 5f\n  orq $16, %r10\n5:\n"
    "  cmpq %rax, %r14\n  je 6f\n  orq $32, %r10\n6:\n"
    "  cmpq %rax, %r15\n  je 7f\n  orq $64, %r10\n7:\n"
    "  movdqu abi_xmm_seed(%rip), %xmm5\n"
    "  pcmpeqb %xmm5, %xmm6\n  pmovmskb %xmm6, %eax\n  cmp $65535, %eax\n  je 8f\n  orq $128, %r10\n8:\n"
    "  pcmpeqb %xmm5, %xmm7\n  pmovmskb %xmm7, %eax\n  cmp $65535, %eax\n  je 9f\n  orq $128, %r10\n9:\n"
    "  pcmpeqb %xmm5, %xmm8\n  pmovmskb %xmm8, %eax\n  cmp $65535, %eax\n  je 10f\n  orq $128, %r10\n10:\n"
    "  pcmpeqb %xmm5, %xmm9\n  pmovmskb %xmm9, %eax\n  cmp $65535, %eax\n  je 11f\n  orq $128, %r10\n11:\n"
    "  pcmpeqb %xmm5, %xmm10\n  pmovmskb %xmm10, %eax\n  cmp $65535, %eax\n  je 12f\n  orq $128, %r10\n12:\n"
    "  pcmpeqb %xmm5, %xmm11\n  pmovmskb %xmm11, %eax\n  cmp $65535, %eax\n  je 13f\n  orq $128, %r10\n13:\n"
    "  pcmpeqb %xmm5, %xmm12\n  pmovmskb %xmm12, %eax\n  cmp $65535, %eax\n  je 14f\n  orq $128, %r10\n14:\n"
    "  pcmpeqb %xmm5, %xmm13\n  pmovmskb %xmm13, %eax\n  cmp $65535, %eax\n  je 15f\n  orq $128, %r10\n15:\n"
    "  pcmpeqb %xmm5, %xmm14\n  pmovmskb %xmm14, %eax\n  cmp $65535, %eax\n  je 16f\n  orq $128, %r10\n16:\n"
    "  pcmpeqb %xmm5, %xmm15\n  pmovmskb %xmm15, %eax\n  cmp $65535, %eax\n  je 17f\n  orq $128, %r10\n17:\n"
    "  movq %r10, %rax\n"
    "  movdqu 32(%rsp), %xmm6\n  movdqu 48(%rsp), %xmm7\n"
    "  movdqu 64(%rsp), %xmm8\n  movdqu 80(%rsp), %xmm9\n"
    "  movdqu 96(%rsp), %xmm10\n  movdqu 112(%rsp), %xmm11\n"
    "  movdqu 128(%rsp), %xmm12\n  movdqu 144(%rsp), %xmm13\n"
    "  movdqu 160(%rsp), %xmm14\n  movdqu 176(%rsp), %xmm15\n"
    "  addq $200, %rsp\n"
    "  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n"
    "  popq %rdi\n  popq %rsi\n  popq %rbx\n  retq\n"
);
#else
#error "binding_abi_test requires the supported MinGW x64 ABI path"
#endif

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------
static int g_fail = 0;
static const char* passfail(bool ok) { return ok ? "PASS" : "FAIL"; }
static void record(bool ok, const char* tag) {
    std::printf("[%s] %s\n", passfail(ok), tag);
    if (!ok) g_fail = 1;
}

// [1] struct-by-value ARG, <=8B: Pair8 { x: i32; y: i32; } (8B) passed in one
// GP register (rcx). The host reads x,y out of that one 8-byte register.
// Pins the struct-arg copy_bytes path (sema check_struct_arg_shape requires a
// bare local; the script assigns the literal to a local first, then passes
// the local).
//
// Binding note: ember represents EVERY struct value (opaque handle like vec3
// AND script-declared POD like Pair8) as prim=I64 with a struct_name tag (see
// src/sema.cpp's StructLit case + src/parser.cpp's parse_type Ident branch).
// The by-value vs opaque-handle distinction is made by words_for_type looking
// the struct_name up in the StructLayoutTable: present -> by-value
// (ceil(size/8) words, copied via copy_bytes); absent -> opaque handle
// (1 word). So a struct-by-value native param is bound with bind_handle
// (the i64 tag), NOT make_struct (prim=Void, never matches a script type).
static void test_aligned_trap() {
    using namespace ember;
    NativeTable tab;
    CompiledProgram* p = compile_program(
        "fn entry(i: i64) -> i64 { let a: i64[1]; return a[i]; }\n", tab,
        reinterpret_cast<void*>(&binding_aligned_trap));
    if (!p) { record(false, "[0a] aligned trap path (compile)"); return; }
    // OOB invokes the alignment-sensitive movaps stub. Returning is sufficient:
    // a misaligned generated call faults before this test can report success.
    (void)binding_call_i64_aligned(entry_of(p, "entry"), 2);
    free_program(p);
    record(true, "[0a] generated trap call provides aligned Win64 shadow frame");
}

static void test_nonvolatile_preservation() {
    using namespace ember;
    BindingBuilder b;
    b.add("mixed_if", make_prim(Prim::I64),
          { make_prim(Prim::I64), make_prim(Prim::F32), make_prim(Prim::I64) },
          (void*)&n_mixed_if);
    NativeTable tab = b.build();
    CompiledProgram* p = compile_program(
        "fn entry() -> i64 { let mut x: i64 = 0; switch (2) { case 1: x = 1; break; default: x = mixed_if(30, 4.0f, 8); break; } return x; }\n",
        tab);
    if (!p) { record(false, "[0] nonvolatile register preservation (compile)"); return; }
    uint64_t changed = call_check_nonvolatiles(entry_of(p, "entry"));
    free_program(p);
    record(changed == 0, "[0] generated code preserves rbx/rsi/rdi/r12-r15 and XMM6-XMM15");
    if (changed) std::printf("       changed-mask=0x%llx\n", (unsigned long long)changed);
}

static void test_struct_by_value_arg() {
    using namespace ember;
    BindingBuilder b;
    b.add("add_pair8", make_prim(Prim::I64),
          { bind_handle("Pair8") },
          (void*)&n_add_pair8);
    NativeTable tab = b.build();

    const std::string src =
        "struct Pair8 { x: i32; y: i32; }\n"
        "fn entry() -> i64 {\n"
        "    let p: Pair8 = Pair8 { x: 30, y: 12 };\n"
        "    return add_pair8(p);\n"
        "}\n";

    CompiledProgram* p = compile_program(src, tab);
    if (!p) { record(false, "[1] struct-by-value ARG <=8B (compile)"); return; }
    int64_t r = call0_i64(entry_of(p, "entry"));
    free_program(p);
    // x=30, y=12 -> 30+12 = 42. Verifies the host read BOTH fields out of the
    // single rcx word (if the struct weren't copied/passed right, the host
    // would read garbage for one or both fields).
    record(r == 42, "[1] struct-by-value ARG <=8B: add_pair8(30,12)==42");
    std::printf("       got=%lld\n", (long long)r);
}

// [2] struct-by-value RETURN, >8B, hidden-pointer path. Vec3s { f32,f32,f32 }
// = 12B. Host fn takes the dest pointer as its hidden word-0 arg and writes
// the struct through it. Script reads fields back via FieldExpr. ALSO pins
// the vec extension's vec3_new handle-return path (a DIFFERENT, i64-handle
// convention) as a regression baseline so both struct-return shapes stay
// covered.
static void test_nested_packed_arg() {
    using namespace ember;
    BindingBuilder b;
    b.add("read_packed", make_prim(Prim::I64), { bind_handle("PackedOuter") },
          (void*)&n_read_packed);
    NativeTable tab = b.build();
    const std::string src =
        "struct PackedInner { a: u8; b: u16; }\n"
        "struct PackedOuter { tag: u8; value: i32; inner: PackedInner; }\n"
        "fn entry() -> i64 { let p: PackedOuter = PackedOuter { tag: 3, value: 36, inner: PackedInner { a: 1, b: 2 } }; return read_packed(p); }\n";
    CompiledProgram* p = compile_program(src, tab);
    if (!p) { record(false, "[1b] nested packed aggregate (compile)"); return; }
    int64_t r = call0_i64(entry_of(p, "entry"));
    free_program(p);
    record(r == 42, "[1b] trailing nested field at offset 5 uses its exact 3-byte packed extent");
}

static void expect_sema_reject(const std::string& src, const ember::NativeTable& tab,
                               const char* tag) {
    using namespace ember;
    auto lr = tokenize(src, "<binding-abi-negative>");
    bool rejected = !lr.ok;
    if (lr.ok) {
        auto pr = parse(std::move(lr.toks));
        rejected = !pr.ok;
        if (pr.ok) {
            std::unordered_map<std::string, int> slots;
            int slot = 0;
            for (auto& fn : pr.program.funcs) { fn.slot = slot; slots[fn.name] = slot++; }
            auto layouts = build_struct_layouts(pr.program);
            rejected = !sema(pr.program, tab.natives, slots, 0, &tab.overloads, &layouts).ok;
        }
    }
    record(rejected, tag);
}

static void test_rejected_aggregate_shapes() {
    using namespace ember;
    BindingBuilder b;
    b.add("take_big", make_prim(Prim::I64), { bind_handle("Big") }, (void*)&n_add_pair8);
    NativeTable tab = b.build();
    expect_sema_reject(
        "struct Big { a: i64; b: i64; } fn main() -> i64 { let v: Big = Big { a: 1, b: 2 }; return take_big(v); }\n",
        tab, "[1c] native by-value aggregate argument >8 bytes rejected");
    NativeTable empty;
    expect_sema_reject(
        "struct Node { value: i64; next: Node; } fn main() -> i64 { return 0; }\n",
        empty, "[1d] recursive aggregate layout rejected explicitly");
}

static void test_struct_by_value_ret() {
    using namespace ember;
    BindingBuilder b;
    b.add("make_vec3s", bind_handle("Vec3s"),
          { make_prim(Prim::F32), make_prim(Prim::F32), make_prim(Prim::F32) },
          (void*)&n_make_vec3s);
    NativeTable tab = b.build();

    const std::string src =
        "struct Vec3s { x: f32; y: f32; z: f32; }\n"
        "fn entry() -> f32 {\n"
        "    let v: Vec3s = make_vec3s(1.0f, 2.0f, 3.0f);\n"
        "    return v.x + v.y + v.z;\n"
        "}\n";

    CompiledProgram* p = compile_program(src, tab);
    if (!p) { record(false, "[2] struct-by-value RET >8B (compile)"); return; }
    float r = call0_f32(entry_of(p, "entry"));
    free_program(p);
    // 1+2+3 = 6.0f. Verifies the callee wrote all three fields through the
    // hidden pointer AND the script read them back correctly (FieldExpr loads).
    record(r == 6.0f, "[2] struct-by-value RET >8B: make_vec3s(1,2,3).x+y+z==6.0");
    std::printf("       got=%f\n", (double)r);
}

// [2b] vec3 extension handle-return baseline: vec3_new returns an i64 handle
// (a DIFFERENT convention than the hidden-pointer struct return above - the
// handle is a single i64 in rax, the struct is written through a hidden
// pointer). Both shapes must stay correct independently.
static void test_vec3_handle_ret() {
    using namespace ember;
    // The vec3 extension registers vec3_new/x/y/z into a NativeSig map (the
    // same shape BindingBuilder.build().natives produces - sema consumes a
    // NativeSig map either way). We pull the extension's real fn ptrs here so
    // the test is independent of the extension-refactor's internal layout:
    // only the registered names/signatures (vec3_new: 3 f32 -> vec3 handle;
    // vec3_x/y/z: i64 handle -> f32) matter, and those are the stable public
    // contract.
    std::unordered_map<std::string, NativeSig> natives;
    ext_vec::register_natives(natives);
    NativeTable tab;
    tab.natives = std::move(natives);

    const std::string src =
        "fn entry() -> f32 {\n"
        "    let v: vec3 = vec3_new(1.0f, 2.0f, 3.0f);\n"
        "    return vec3_x(v) + vec3_y(v) + vec3_z(v);\n"
        "}\n";
    // Script sees vec3 as a nominal opaque handle. vec3_x/y/z take
    // that i64 and return the stored f32. struct_layouts empty (no script
    // struct) -> handle passed as 1 word (rcx), returned as 1 word (rax).

    CompiledProgram* p = compile_program(src, tab);
    if (!p) { record(false, "[2b] vec3 handle-ret (compile)"); return; }
    float r = call0_f32(entry_of(p, "entry"));
    free_program(p);
    ext_vec::reset();
    record(r == 6.0f, "[2b] vec3 handle-ret baseline: vec3_new(1,2,3) x+y+z==6.0");
    std::printf("       got=%f\n", (double)r);
}

// [2c] struct-literal RETURN directly: `return V3 { ... };` from a script fn
// that returns a struct by pointer (the >8B hidden-pointer return path). The
// relaxation lets a struct-returning fn `return` a struct literal directly
// (today rejected - sema required a named local first). Codegen materializes
// the literal into a compiler-hidden temp frame slot, then copies the temp's
// bytes through the hidden return pointer. NON-CIRCULAR: the host allocates a
// V3, passes &it as the hidden word-0, calls the script fn, and reads x/y/z
// back directly (the C struct read cannot circularly hide a codegen bug the
// way an in-language struct equality could). Reverting the fix -> sema rejects
// `return V3 { ... };` -> compile fails -> this records false.
static void test_struct_lit_return_direct() {
    using namespace ember;
    NativeTable tab;  // pure script: v3_lit_up takes no args, returns V3 - no natives
    const std::string src =
        "struct V3 { x: f32; y: f32; z: f32; }\n"
        "fn v3_lit_up() -> V3 { return V3 { x: 1.0f, y: 2.0f, z: 3.0f }; }\n"
        "fn main() -> i64 { return 0; }\n";
    CompiledProgram* p = compile_program(src, tab);
    if (!p) { record(false, "[2c] struct-literal return direct (compile)"); return; }
    V3 r = call0_struct_v3(entry_of(p, "v3_lit_up"));
    free_program(p);
    // 1+2+3 = 6.0. Verifies the struct-literal materialized into the temp and
    // was copied byte-exact through the hidden return pointer (a wrong field
    // order, a missed field, or a stale-temp copy would read back garbage).
    record(r.x + r.y + r.z == 6.0f, "[2c] struct-literal return: V3{1,2,3}.x+y+z==6.0");
    std::printf("       got=(%f,%f,%f) sum=%f\n", (double)r.x, (double)r.y, (double)r.z, (double)(r.x+r.y+r.z));
}

// [2d] struct-by-value arg as a general expression: `v3_dot(v3_up(), v3_up())`
// where each arg is a struct-returning CALL (not a bare local). The relaxation
// lets a struct-returning call be used directly as a struct-by-value argument
// (today rejected - sema required a bare local). Codegen materializes each
// struct-returning-call arg into its own compiler-hidden temp frame slot
// (forwarding the temp's address as the inner call's hidden word-0, the
// callee writes through it), then copies the temp's bytes into the outer
// call's arg stash. NON-CIRCULAR: the host reads the f32 return of main
// directly. Reverting the fix -> sema rejects `v3_dot(v3_up(), v3_up())` ->
// compile fails -> this records false.
static void test_struct_arg_general_expr() {
    using namespace ember;
    NativeTable tab;  // pure script: v3_up/v3_dot/main, no natives
    const std::string src =
        "struct V3 { x: f32; y: f32; z: f32; }\n"
        "fn v3_up() -> V3 { return V3 { x: 0.0f, y: 1.0f, z: 0.0f }; }\n"
        "fn v3_dot(a: V3, b: V3) -> f32 { return a.x*b.x + a.y*b.y + a.z*b.z; }\n"
        "fn main() -> f32 { return v3_dot(v3_up(), v3_up()); }\n";
    CompiledProgram* p = compile_program(src, tab);
    if (!p) { record(false, "[2d] struct-ret-call as struct-by-value arg (compile)"); return; }
    float r = call0_f32(entry_of(p, "main"));
    free_program(p);
    // v3_up()=(0,1,0); dot((0,1,0),(0,1,0)) = 0+1+0 = 1.0. Verifies BOTH
    // struct-ret-call args were materialized into distinct temps (NOT aliased -
    // a single reused slot would have the second v3_up() overwrite the first
    // before the copy, but since both are (0,1,0) the value still happens to
    // be 1.0; the distinctness is pinned by the nested probe below which would
    // break under aliasing) and copied into the arg stash byte-exact.
    record(r == 1.0f, "[2d] v3_dot(v3_up(),v3_up())==1.0 (struct-ret-call as arg)");
    std::printf("       got=%f\n", (double)r);
}

// [2e] NESTED struct-ret-call as a struct-by-value arg to ANOTHER struct-
// returning call: `v3_shift(v3_up())` where v3_shift returns V3 (so the call
// goes through eval_struct_returning_call's arg-stash, whose struct-ret-call
// arg v3_up() is recursively materialized into a temp). This pins the second
// arg-stash site (eval_struct_returning_call's op.is_struct path) AND the
// per-arg temp distinctness in a context that WOULD break under temp aliasing
// (v3_up writes (0,1,0) into the temp; v3_shift reads it, shifts to (1,1,0);
// if the temp were wrongly reused/aliased the shift would read stale data).
// main returns the shifted V3; the host reads it back directly. NON-CIRCULAR.
// Reverting the fix -> sema rejects `v3_shift(v3_up())` -> compile fails ->
// this records false.
static void test_nested_struct_ret_call_arg() {
    using namespace ember;
    NativeTable tab;
    const std::string src =
        "struct V3 { x: f32; y: f32; z: f32; }\n"
        "fn v3_up() -> V3 { return V3 { x: 0.0f, y: 1.0f, z: 0.0f }; }\n"
        "fn v3_shift(a: V3) -> V3 { return V3 { x: a.x + 1.0f, y: a.y, z: a.z }; }\n"
        "fn main() -> V3 { return v3_shift(v3_up()); }\n";
    CompiledProgram* p = compile_program(src, tab);
    if (!p) { record(false, "[2e] nested struct-ret-call arg (compile)"); return; }
    V3 r = call0_struct_v3(entry_of(p, "main"));
    free_program(p);
    // v3_up()=(0,1,0); v3_shift((0,1,0)) = (1,1,0). Verifies the inner
    // v3_up() arg was materialized into a temp and v3_shift read it correctly
    // (x+1 -> 1, y,z unchanged) AND the result was forwarded through main's
    // own hidden return pointer. A temp-aliasing or wrong-slot bug would read
    // back a wrong x (e.g. 0 if v3_shift read the temp before v3_up wrote it).
    record(r.x == 1.0f && r.y == 1.0f && r.z == 0.0f,
           "[2e] v3_shift(v3_up())==(1,1,0) (nested struct-ret-call arg)");
    std::printf("       got=(%f,%f,%f)\n", (double)r.x, (double)r.y, (double)r.z);
}

// [3] >4 args: args 5,6 spill to the outgoing stack past the 32-byte shadow
// space. sum6(a,b,c,d,e,f) = a+b+c+d+e+f. Args 1-4 in rcx/rdx/r8/r9, args
// 5,6 at [rsp+32]/[rsp+40] (the codegen's `words 4+ -> outgoing stack` path).
static void test_six_args() {
    using namespace ember;
    BindingBuilder b;
    b.add("sum6", make_prim(Prim::I64),
          { make_prim(Prim::I64), make_prim(Prim::I64), make_prim(Prim::I64),
            make_prim(Prim::I64), make_prim(Prim::I64), make_prim(Prim::I64) },
          (void*)&n_sum6);
    NativeTable tab = b.build();

    const std::string src =
        "fn entry() -> i64 {\n"
        "    return sum6(1, 2, 3, 4, 5, 6);\n"
        "}\n";
    CompiledProgram* p = compile_program(src, tab);
    if (!p) { record(false, "[3] >4 args spill (compile)"); return; }
    int64_t r = call0_i64(entry_of(p, "entry"));
    free_program(p);
    // 1+2+3+4+5+6 = 21. If args 5,6 weren't placed on the outgoing stack
    // (e.g. a refactor that dropped the `words 4+` loop), the host would read
    // garbage for e,f and return the wrong sum.
    record(r == 21, "[3] >4 args spill: sum6(1..6)==21 (args 5,6 on stack)");
    std::printf("       got=%lld\n", (long long)r);
}

// [4] f32 args: fadd(a,b) -> a+b. Word 0 = xmm0, word 1 = xmm1 (slot-parallel
// for floats - the float word goes to the float reg of its SLOT index, not
// the first free float reg). Also pins the math extension's sqrt as a second
// f32->f32 path.
static void test_f32_args() {
    using namespace ember;
    BindingBuilder b;
    b.add("fadd", make_prim(Prim::F32),
          { make_prim(Prim::F32), make_prim(Prim::F32) },
          (void*)&n_fadd);
    NativeTable tab = b.build();

    const std::string src =
        "fn entry() -> f32 {\n"
        "    return fadd(1.5f, 2.5f);\n"
        "}\n";
    CompiledProgram* p = compile_program(src, tab);
    if (!p) { record(false, "[4] f32 args (compile)"); return; }
    float r = call0_f32(entry_of(p, "entry"));
    free_program(p);
    record(r == 4.0f, "[4] f32 args xmm0/xmm1: fadd(1.5,2.5)==4.0");
    std::printf("       got=%f\n", (double)r);

    // [4b] math extension sqrt (second f32->f32 pin, independent of fadd).
    std::unordered_map<std::string, NativeSig> mn;
    ext_math::register_natives(mn);
    NativeTable mtab; mtab.natives = std::move(mn);
    const std::string src2 =
        "fn entry() -> f32 {\n"
        "    return sqrt(9.0f);\n"
        "}\n";
    CompiledProgram* p2 = compile_program(src2, mtab);
    if (!p2) { record(false, "[4b] math sqrt (compile)"); return; }
    float r2 = call0_f32(entry_of(p2, "entry"));
    free_program(p2);
    record(r2 == 3.0f, "[4b] math sqrt f32->f32: sqrt(9.0)==3.0");
    std::printf("       got=%f\n", (double)r2);
}

// [5] slice arg: first_byte(p, len) -> p[0]. A script slice lowers to ONE
// script arg of slice type that the codegen expands into 2 words at the call
// site (ptr=word0=rcx, len=word1=rdx), but the NATIVE SIGNATURE declares it
// as ONE slice-typed param (sema checks script-arg count against
// NativeSig::params count). The host C++ fn takes the 2 words as 2 separate
// params (uint8_t* p, int64_t len) - the 1-script-arg -> 2-host-params split
// is the ABI contract. Mirrors the string extension's string_from_slice
// (1 slice<u8> NativeSig param, host n_string_from_slice(uint8_t*,int64_t)).
// Here the slice comes from a string literal (a slice<u8> baked into rodata).
static void test_slice_arg() {
    using namespace ember;
    BindingBuilder b;
    b.add("first_byte", make_prim(Prim::I64),
          { make_slice(std::make_shared<Type>(make_prim(Prim::U8))) },
          (void*)&n_first_byte);
    NativeTable tab = b.build();

    // The string literal "ABC" bakes as a slice<u8> {ptr,len=3}; passing it to
    // first_byte places ptr in rcx and len in rdx. The host returns p[0] = 'A'
    // = 65.
    const std::string src =
        "fn entry() -> i64 {\n"
        "    return first_byte(\"ABC\");\n"
        "}\n";
    CompiledProgram* p = compile_program(src, tab);
    if (!p) { record(false, "[5] slice arg (compile)"); return; }
    int64_t r = call0_i64(entry_of(p, "entry"));
    free_program(p);
    record(r == 65, "[5] slice arg ptr+len 2 words: first_byte(\"ABC\")==65 ('A')");
    std::printf("       got=%lld\n", (long long)r);
}

// [6] mixed int+float, slot-parallel. mixed_if(a: i64, b: f32, c: i64) ->
// a + (i64)b + c. Win64: a=word0=rcx, b=word1=xmm1 (NOT xmm0!), c=word2=r8.
// The trap is assuming the float goes to the first free FLOAT reg (xmm0) -
// it does NOT; slot-parallel means b is placed at slot 1, which for a float
// word is xmm1. THIS IS THE KEY SLOT-PARALLEL TEST.
static void test_mixed_slot_parallel() {
    using namespace ember;
    BindingBuilder b;
    b.add("mixed_if", make_prim(Prim::I64),
          { make_prim(Prim::I64), make_prim(Prim::F32), make_prim(Prim::I64) },
          (void*)&n_mixed_if);
    NativeTable tab = b.build();

    // a=100, b=3.5f (truncates to 3 as i64), c=7 -> 100+3+7 = 110.
    // If b landed in xmm0 instead of xmm1, the host would read b from xmm1
    // (garbage) and return the wrong value.
    const std::string src =
        "fn entry() -> i64 {\n"
        "    return mixed_if(100, 3.5f, 7);\n"
        "}\n";
    CompiledProgram* p = compile_program(src, tab);
    if (!p) { record(false, "[6] mixed slot-parallel (compile)"); return; }
    int64_t r = call0_i64(entry_of(p, "entry"));
    free_program(p);
    record(r == 110, "[6] mixed slot-parallel: mixed_if(100,3.5f,7)==110 (b in xmm1)");
    std::printf("       got=%lld\n", (long long)r);
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== ember binding ABI correctness suite ===\n");
    std::printf("(pins script->native Win64 call ABI per BINDING_API.md Sec 4)\n\n");

    test_nonvolatile_preservation(); // [0]
    test_aligned_trap();           // [0a]
    test_struct_by_value_arg();    // [1]
    test_nested_packed_arg();      // [1b]
    test_rejected_aggregate_shapes(); // [1c/d]
    test_struct_by_value_ret();    // [2]
    test_vec3_handle_ret();       // [2b]
    test_struct_lit_return_direct();  // [2c]
    test_struct_arg_general_expr();  // [2d]
    test_nested_struct_ret_call_arg();  // [2e]
    test_six_args();               // [3]
    test_f32_args();               // [4] + [4b]
    test_slice_arg();              // [5]
    test_mixed_slot_parallel();    // [6]

    std::printf("\nbinding ABI suite: %s\n", g_fail == 0 ? "PASS" : "FAIL");
    return g_fail == 0 ? 0 : 1;
}
