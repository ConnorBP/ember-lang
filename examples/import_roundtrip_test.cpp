// import_roundtrip_test - live `import` cross-module runtime proof
// (RESTRUCTURE_PLAN.md step 1.5 / MODULES.md Section 2 Section 3 Section 4 Section 5).
//
// This is the additive, in-place port of the standalone tree's tests #8 (JIT
// cross-module) and #9 (.em cross-module round-trip) into prism's live ember.
// It links only ember + ember_frontend (no prism/prism_script_host) so it stays
// free of the panel-API link breakage that currently stops ember_cli - exactly
// like em_roundtrip_test, which it shares the real parser pipeline with.
//
// DESIGN PATH (documented per the task spec):
//   prism's parser has no `::` / `import` / cross-module call grammar yet
//   (the live-`import` *call* form is MODULES.md Section 6 Tier-6 future work; this
//   port lands the *runtime registry + cross-module call sequence + .em kind-2
//   reloc*, not the surface syntax). So the test uses the REAL parser for the
//   callee's function body and hand-assembles ONLY the caller's cross-module
//   call sequence - composing prism's existing X64Emitter primitives
//   (mov_reg_imm64_external + load_reg_mem + load_reg_mem + call_reg + a
//   minimal Win64 prologue/epilogue), mirroring the standalone's
//   build_caller_cross_module. This is strictly BETTER than the standalone's
//   hand-built callee: here the callee is the real parsed `double_it`, proving
//   the registry hop resolves into a parser→sema→codegen-produced module.
//
// Two tests:
//   A. JIT cross-module: parse+compile callee `double_it`, register it in a
//      ModuleRegistry; hand-assemble the caller whose call site bakes the
//      registry base (mov_reg_imm64_external kind 2) + module_id*8 + slot*8;
//      JIT-fill the kind-2 placeholder with registry->base(); call caller(21)
//      -> 42.
//   B. .em cross-module round-trip: serialize BOTH modules to .em, build a
//      FRESH ModuleRegistry, load both with load_em_file(..., &registry)
//      (kind-2 patches with the FRESH registry's base), register both, call
//      the loaded caller(21) -> 42. Proves the full live-import subsystem:
//      serialize two modules -> load -> register in a fresh registry -> link
//      (the kind-2 patch IS the link) -> cross-call through the registry.

#include "../src/x64_emitter.hpp"
#include "../src/jit_memory.hpp"
#include "../src/em_file.hpp"
#include "../src/em_writer.hpp"
#include "../src/em_loader.hpp"
#include "../src/module_registry.hpp"
#include "../src/dispatch_table.hpp"
#include "../src/engine.hpp"
#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"
#include "../src/codegen.hpp"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

// Win64 i64(i64) call: the compiled functions take one i64 arg (rcx) and
// return i64 (rax), matching double_it(x: i64) -> i64 and caller(x: i64) -> i64.
static int64_t call_i64_i64(void* entry, int64_t a) {
    using F = int64_t(*)(int64_t);
    return reinterpret_cast<F>(entry)(a);
}

// =====================================================================
// CALLEE (Module B): fn double_it(x: i64) -> i64 { return x * 2; }
// Built via the REAL parser→sema→codegen pipeline (no hand-assembly). Returns
// a finalized CompiledFn + the slot table it lives in, the way prism_script_host
// builds any parsed function.
// =====================================================================
struct CalleeModule {
    ember::CompiledFn fn;                                   // double_it
    ember::EmSignature signature;                           // copied from real FuncDecl
    ember::DispatchTable table;                              // B's own dispatch table (slot 0 = double_it)
    std::unordered_map<std::string, int> slots;              // name -> slot
    std::vector<uint8_t> globals_store;                      // empty (no globals)
    ember::GlobalsBlock gb;                                  // empty globals block
};

static CalleeModule build_callee_double_it(const std::string& src) {
    using namespace ember;
    CalleeModule m{ {}, {}, DispatchTable(1), {}, {}, {} };

    // ---- lex ----
    auto lr = tokenize(src, "<callee>");
    if (!lr.ok) { std::printf("FAIL callee lex: %s\n", lr.error.c_str()); std::exit(1); }

    // ---- parse ----
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::printf("FAIL callee parse: %s\n", pr.error.c_str()); std::exit(1); }
    if (pr.program.funcs.size() != 1) {
        std::printf("FAIL callee: expected 1 func, got %zu\n", pr.program.funcs.size());
        std::exit(1);
    }

    // ---- slot assignment (slot 0 = double_it) ----
    int si = 0;
    for (auto& fn : pr.program.funcs) { m.slots[fn.name] = si++; fn.slot = m.slots[fn.name]; }

    // ---- sema (empty natives/overloads/structs - double_it needs none) ----
    std::unordered_map<std::string, NativeSig> natives; // empty
    auto struct_layouts = build_struct_layouts(pr.program);
    pr.program.string_xor_key = 0xA5;
    auto sr = sema(pr.program, natives, m.slots, 0, nullptr, &struct_layouts);
    if (!sr.ok) {
        std::printf("FAIL callee sema (%zu errors):\n", sr.errors.size());
        for (auto& e : sr.errors) std::printf("  line %u: %s\n", e.line, e.msg.c_str());
        std::exit(1);
    }

    // ---- globals block (empty) ----
    m.globals_store.resize(0);
    m.gb.base = int64_t(m.globals_store.data());
    g_globals_for_codegen = &m.gb;

    // ---- codegen ctx ----
    CodeGenCtx ctx;
    ctx.globals_base = m.gb.base;
    ctx.dispatch_base = int64_t(m.table.base());
    ctx.natives = &natives;
    ctx.script_slots = &m.slots;
    ctx.structs = &struct_layouts;

    // ---- compile + finalize double_it ----
    m.signature.ret=pr.program.funcs[0].ret?*pr.program.funcs[0].ret:Type{};
    for(const auto& p:pr.program.funcs[0].params)m.signature.params.push_back(p.ty?*p.ty:Type{});
    m.fn = compile_func(pr.program.funcs[0], ctx);
    if (!finalize(m.fn)) { std::printf("FAIL callee: alloc_executable\n"); std::exit(1); }
    m.table.set(pr.program.funcs[0].slot, m.fn.entry);
    return m;
}

// =====================================================================
// CALLER (Module A): fn caller(x: i64) -> i64 { return double_it(x); }
// HAND-ASSEMBLED via prism's X64Emitter primitives (the parser has no
// cross-module call form yet - MODULES.md Section 6 is Tier-6 future syntax; this
// port lands the runtime mechanism, not the surface grammar). The function
// body is the Section 3 cross-module call sequence composed from existing primitives:
//   mov  r11, <registry_base>          ; mov_reg_imm64_external - kind 2
//                                       ;   (8 zero placeholders + AbsFixup
//                                       ;   {imm_off, ModuleRegistryBase})
//   mov  r11, [r11 + module_id*8]      ; load_reg_mem - callee's DispatchTable*
//   mov  r11, [r11 + slot*8]           ; load_reg_mem - callee's slots[slot]
//   call r11                            ; call_reg - same final step as intra-
//                                       ;   module (CODEGEN_SPEC.md Section 7)
// `module_id` and `slot` are compile-time displacements (constants post-link);
// only `registry_base` is position-dependent and gets the kind-2 reloc.
//
// Frame/alignment: the caller makes one call (to the callee), so we reserve
// 32 bytes shadow space. The arg (x) is in rcx (Win64 slot 0) and is the
// callee's arg too - pass it straight through (no mov, no spill: rcx already
// holds x and the callee consumes it from rcx). No callee-saved (we don't hold
// any value across the call - the callee's result comes back in rax and we
// return it directly). Alignment: at entry rsp ≡ 8 (mod 16); `push rbp` ->
// rsp ≡ 0; no callee-saved pushes; `sub rsp, 32` -> rsp ≡ 0 at the call site
// (32 ≡ 0 mod 16). ✓
//
// `registry_base` is the JIT-fill value written into the kind-2 placeholder
// after emit (same fill-the-placeholder pattern as codegen.cpp's JIT fill for
// DispatchTableBase). At .em-write time the serializer records the reloc; at
// .em-load time the loader patches it with the fresh registry's base().
// =====================================================================
struct CallerModule {
    ember::CompiledFn fn;                                   // caller (hand-assembled)
    std::vector<uint8_t> bytes;                             // post-resolve, post-JIT-fill bytes (for .em serialize)
    ember::DispatchTable table;                             // A's own dispatch table (slot 0 = caller)
};

// finalize a CompiledFn from a pre-built byte vector (the caller is hand-
// assembled, so it does not go through compile_func; it needs its bytes
// published to an exec page and its entry set). Mirrors engine.cpp's finalize
// (alloc_executable + set exec/entry), but takes the bytes explicitly since the
// hand-assembled caller does not populate fn.bytes itself until here.
static bool finalize_from_bytes(ember::CompiledFn& fn, const std::vector<uint8_t>& bytes) {
    void* p = ember::alloc_executable(bytes);
    if (!p) return false;
    fn.exec = p;
    fn.entry = p;
    fn.bytes = bytes;
    return true;
}

static CallerModule build_caller_cross_module(uint32_t module_id, uint32_t slot,
                                              void* registry_base) {
    using namespace ember;
    CallerModule m{ {}, {}, DispatchTable(1) };

    X64Emitter e;
    // ---- minimal Win64 prologue (no callee-saved, 32-byte shadow frame) ----
    e.push(Reg::rbp);
    e.mov_reg_reg(Reg::rbp, Reg::rsp);          // mov rbp, rsp
    e.sub_reg_imm32(Reg::rsp, 32);              // sub rsp, 32 (shadow space)

    // ---- cross-module call sequence (MODULES.md Section 3) ----
    // mov r11, <registry_base> - external-reloc form (kind 2). 8 zero
    // placeholder bytes + AbsFixup{imm_off, ModuleRegistryBase} on e.
    e.mov_reg_imm64_external(Reg::r11, AbsFixup::ModuleRegistryBase);
    // mov r11, [r11 + module_id*8] - load the callee's DispatchTable* from
    // the registry. module_id*8 is a compile-time disp (module_id is a
    // constant post-link); the disp fits in an int32 for any realistic
    // module_id.
    int32_t id_disp = static_cast<int32_t>(static_cast<uint64_t>(module_id) * 8);
    e.load_reg_mem(Reg::r11, Reg::r11, id_disp);
    // mov r11, [r11 + slot*8] - load slots[slot], the callee's entry.
    int32_t slot_disp = static_cast<int32_t>(static_cast<uint64_t>(slot) * 8);
    e.load_reg_mem(Reg::r11, Reg::r11, slot_disp);
    // call r11 - same final step as intra-module (Section 7).
    e.call_reg(Reg::r11);                       // rax = double_it(x)

    // ---- minimal Win64 epilogue (undo the frame, pop rbp, ret) ----
    e.add_reg_imm32(Reg::rsp, 32);              // add rsp, 32
    e.pop(Reg::rbp);
    e.ret();

    e.resolve_fixups();

    // JIT fill: resolve_fixups does NOT touch AbsFixups, so the driver writes
    // the real registry_base into the kind-2 placeholder here. Same fill-the-
    // placeholder pattern as codegen.cpp's DispatchTableBase JIT fill.
    std::vector<uint8_t> code = e.code;
    for (const auto& af : e.abs_fixups()) {
        if (af.code_offset + 8 > code.size()) continue; // sanity
        uint64_t addr = (af.kind == AbsFixup::ModuleRegistryBase)
            ? reinterpret_cast<uint64_t>(registry_base) : 0;
        for (int i = 0; i < 8; ++i)
            code[af.code_offset + i] = static_cast<uint8_t>((addr >> (8 * i)) & 0xFF);
    }

    m.bytes = code; // captured BEFORE finalize (clean copy for the serializer)
    m.fn.name = "caller";
    m.fn.abs_fixups = e.abs_fixups(); // capture for .em serialization (the kind-2 reloc)
    if (!finalize_from_bytes(m.fn, code)) { std::printf("FAIL caller: alloc_executable\n"); std::exit(1); }
    m.table.set(0, m.fn.entry);
    return m;
}

// (finalize_from_bytes is defined above build_caller_cross_module.)

// Build the EmModule from a single CompiledFn + its slot, the way a host
// serializer would (BUNDLING_AND_EM_MODULES.md Section 2.3). `relocs` are filled from
// `CompiledFn::abs_fixups`. Mirrors em_roundtrip_test's build_em_module for the
// single-function case.
static ember::EmModule build_em_module_single(const ember::CompiledFn& fn,
                                              uint32_t slot,
                                              const ember::EmSignature& signature) {
    using namespace ember;
    EmModule mod;
    EmFunctionRecord rec;
    rec.name = fn.name;
    rec.slot_index = slot;
    rec.code = fn.bytes;
    rec.rodata = fn.rodata;
    rec.non_serializable_reason=fn.non_serializable_reason;
    rec.signature = signature;
    for(const auto& nf:fn.native_fixups){EmNativeBinding b;b.offset=nf.code_offset;b.name=nf.name;b.signature.ret=nf.ret;b.signature.params=nf.params;rec.native_bindings.push_back(std::move(b));}
    rec.relocs.reserve(fn.abs_fixups.size());
    for (const auto& af : fn.abs_fixups) {
        EmReloc r;
        r.offset = af.code_offset;
        r.kind = static_cast<uint8_t>(af.kind);
        r.addend = af.addend;
        rec.relocs.push_back(r);
    }
    mod.functions.push_back(std::move(rec));
    mod.globals.clear();                  // neither fn uses globals
    mod.entry_slot = slot;                // the single fn is @entry
    mod.name_table = { {fn.name, slot} };
    return mod;
}

int main() {
    using namespace ember;
    int failures = 0;
    auto passfail = [&](bool ok) { return ok ? "PASS" : "FAIL"; };

    // =====================================================================
    // Test A: JIT cross-module call through the registry (MODULES.md Section 2/Section 3).
    // Caller's caller(21) must return 42 (callee doubled it), resolved at call
    // time via registry[module_id_B] -> B's DispatchTable* -> slots[0] -> double_it.
    // =====================================================================
    std::printf("=== Test A: JIT cross-module ===\n");
    {
        // ---- callee B: real parsed double_it ----
        const std::string callee_src =
            "fn double_it(x: i64) -> i64 { return x * 2; }\n";
        CalleeModule b = build_callee_double_it(callee_src);
        // double_it is B's slot 0; B's dispatch table base is b.table.base().
        std::printf("  callee double_it: parsed+compiled, entry=%p, relocs=%zu\n",
                    b.fn.entry, b.fn.abs_fixups.size());

        // ---- fresh registry (capacity 4: B, A, + 2 spare headroom) ----
        ModuleRegistry registry(/*capacity=*/4);
        std::printf("[A1] %s: ModuleRegistry constructed (capacity=4, base=%p, count=0)\n",
                    passfail(registry.base() != nullptr && registry.count() == 0),
                    registry.base());
        if (!(registry.base() != nullptr && registry.count() == 0)) failures++;

        // ---- register B -> get module_id_B (must be 0, dense id space) ----
        std::string err;
        uint32_t id_B = registry.register_module("B", b.table.base(), &err);
        bool a2 = (id_B != UINT32_MAX && id_B == 0 && err.empty());
        std::printf("[A2] %s: register B (double_it) -> id=%u\n", passfail(a2), id_B);
        if (!a2) failures++;

        // ---- sanity: resolve(id_B) returns B's dispatch table base ----
        bool a3 = (registry.resolve(id_B) == b.table.base());
        std::printf("[A3] %s: registry.resolve(id_B) == B's dispatch table base\n", passfail(a3));
        if (!a3) failures++;

        // ---- caller A: hand-assembled, module_id=id_B, slot=0, registry base ----
        CallerModule a = build_caller_cross_module(/*module_id=*/id_B, /*slot=*/0,
                                                    /*registry_base=*/registry.base());
        std::printf("  caller: hand-assembled, entry=%p, kind-2 relocs=%zu\n",
                    a.fn.entry, a.fn.abs_fixups.size());
        // caller has exactly one kind-2 reloc (the registry-base load).
        bool a3b = (a.fn.abs_fixups.size() == 1 &&
                    a.fn.abs_fixups[0].kind == AbsFixup::ModuleRegistryBase);
        std::printf("[A3b] %s: caller has exactly 1 kind-2 (ModuleRegistryBase) reloc\n",
                    passfail(a3b));
        if (!a3b) failures++;

        // ---- register A -> get module_id_A (must be 1) ----
        uint32_t id_A = registry.register_module("A", a.table.base(), &err);
        bool a4 = (id_A != UINT32_MAX && id_A == 1 && err.empty());
        std::printf("[A4] %s: register A (caller) -> id=%u\n", passfail(a4), id_A);
        if (!a4) failures++;

        // ---- the proof: call A's caller(21) -> must hop the registry to B's
        // double_it and return 42. ----
        auto* caller_fn = reinterpret_cast<int64_t(*)(int64_t)>(a.fn.entry);
        int64_t r21 = caller_fn(21);
        bool a5 = (r21 == 42);
        std::printf("[A5] %s: cross-module JIT: caller(21)==42 (B doubled it via registry), got %lld\n",
                    passfail(a5), (long long)r21);
        if (!a5) failures++;

        if (a5) {
            // a couple more inputs to be sure it isn't a coincidence
            int64_t r0 = caller_fn(0), r7 = caller_fn(7), rm = caller_fn(-5);
            std::printf("     caller(0)==0 : %s (got %lld)\n", passfail(r0==0),  (long long)r0);
            std::printf("     caller(7)==14: %s (got %lld)\n", passfail(r7==14),  (long long)r7);
            std::printf("     caller(-5)==-10: %s (got %lld)\n", passfail(rm==-10),(long long)rm);
            if (!(r0==0 && r7==14 && rm==-10)) failures++;
        }

        // ---- exercise the registry's reload property (Section 4): re-register B
        // under the same name with a DIFFERENT table and confirm the id is
        // unchanged (slot stability lifted one level up) and the caller still
        // works through the new table. ----
        if (a5) {
            CalleeModule b2 = build_callee_double_it(callee_src); // fresh double_it page
            uint32_t id_B2 = registry.register_module("B", b2.table.base(), &err);
            bool a6 = (id_B2 == id_B && err.empty());
            std::printf("[A6] %s: reload B (re-register 'B') -> SAME module_id (slot stability Section 4), id=%u\n",
                        passfail(a6), id_B2);
            if (!a6) failures++;

            bool a7 = (registry.resolve(id_B) == b2.table.base()); // now the NEW table
            std::printf("[A7] %s: registry.resolve(id_B) now returns B2's table (reload swap)\n", passfail(a7));
            if (!a7) failures++;

            int64_t r21_after = caller_fn(21);
            bool a8 = (r21_after == 42);
            std::printf("[A8] %s: caller(21)==42 AFTER B reload (caller bytes unchanged), got %lld\n",
                        passfail(a8), (long long)r21_after);
            if (!a8) failures++;
            // NOTE: b2 must outlive the calls above; it is a local destroyed at
            // block end. The registry holds a raw pointer into b2.table; the
            // calls happen before block exit, so it stays valid here.
        }

        // keep b, a, registry alive until block end (after all calls above).
    }

    // =====================================================================
    // Test B: the FULL live-import runtime - serialize TWO modules to .em,
    // load both into a FRESH ModuleRegistry, link, and cross-call. No parser
    // for the caller (hand-assembled, same as Test A); the callee is the real
    // parsed double_it, serialized exactly the way em_roundtrip_test serializes.
    //
    // The kind-2 reloc in A's loaded code patches with the fresh registry's
    // base (em_loader.cpp's new kind-2 case, fed by the `registry` argument to
    // load_em_file). This proves the entire live-import subsystem: serialize
    // two modules -> load -> register in a fresh registry -> link (the kind-2
    // patch IS the link) -> cross-call through the registry - end to end.
    // =====================================================================
    std::printf("\n=== Test B: .em cross-module round-trip ===\n");
    {
        // ---- Step 1: build both modules JIT-side (to get publishable bytes
        // and relocs), but do NOT call them yet. We need a registry to build A
        // (A's call site needs a registry base for the JIT fill), so construct
        // a JIT-side registry just for the build - the .em-loaded registry is
        // fresh and built in step 4 below. The JIT-side registry is throwaway:
        // its only job is to give A's build a stable base to bake and to give
        // B a module_id for A's call-site displacement. ----
        const std::string callee_src =
            "fn double_it(x: i64) -> i64 { return x * 2; }\n";
        ModuleRegistry jit_registry(/*capacity=*/4);
        CalleeModule b = build_callee_double_it(callee_src);
        std::string err;
        uint32_t id_B = jit_registry.register_module("B", b.table.base(), &err);
        bool b1 = (id_B != UINT32_MAX);
        std::printf("[B1] %s: JIT-side build: register B -> id=%u\n", passfail(b1), id_B);
        if (!b1) failures++;

        CallerModule a = build_caller_cross_module(/*module_id=*/id_B, /*slot=*/0,
                                                    /*registry_base=*/jit_registry.base());
        uint32_t id_A = jit_registry.register_module("A", a.table.base(), &err);
        bool b2 = (id_A != UINT32_MAX);
        std::printf("[B2] %s: JIT-side build: register A -> id=%u\n", passfail(b2), id_A);
        if (!b2) failures++;

        // ---- sanity: the JIT-built A calls through the JIT registry and
        // returns 42. Gate the .em round-trip on this (it is meaningless if
        // the JIT side doesn't work). ----
        auto* jit_caller = reinterpret_cast<int64_t(*)(int64_t)>(a.fn.entry);
        int64_t jit_r = jit_caller(21);
        bool b3 = (jit_r == 42);
        std::printf("[B3] %s: JIT-side caller(21)==42 (gate for the .em round-trip), got %lld\n",
                    passfail(b3), (long long)jit_r);
        if (!b3) failures++;

        // ---- Step 2: serialize both modules to .em. Each its own file. B has
        // NO relocs (leaf, no registry hop); A has one kind-2 reloc (the
        // registry-base load). The kind-2 reloc is what the loader will patch
        // with the FRESH registry's base in step 5. ----
        auto write_module = [&](const char* path, const EmModule& mod) -> bool {
            std::string werr;
            bool ok = write_em_file(mod, path, &werr);
            if (!ok) std::printf("    write_em_file(%s) error: %s\n", path, werr.c_str());
            return ok;
        };

        // callee B: real parsed double_it -> bytes from b.fn.bytes (post-fill),
        // relocs from b.fn.abs_fixups (empty for double_it). slot 0.
        EmModule mod_B = build_em_module_single(b.fn, /*slot=*/0, b.signature);
        // caller A: hand-assembled -> bytes from a.fn.bytes, relocs from
        // a.fn.abs_fixups (one kind-2). slot 0.
        EmSignature caller_signature; caller_signature.ret=make_prim(Prim::I64); caller_signature.params.push_back(make_prim(Prim::I64));
        EmModule mod_A = build_em_module_single(a.fn, /*slot=*/0, caller_signature);

        std::filesystem::path tmp_dir = std::filesystem::temp_directory_path();
        std::filesystem::path path_A = tmp_dir / "import_roundtrip_A.em";
        std::filesystem::path path_B = tmp_dir / "import_roundtrip_B.em";
        bool wrote_A = write_module(path_A.string().c_str(), mod_A);
        bool wrote_B = write_module(path_B.string().c_str(), mod_B);
        std::printf("[B4] %s: write_em_file module A (caller, 1 kind-2 reloc) -> %s\n",
                    passfail(wrote_A), path_A.string().c_str());
        std::printf("[B5] %s: write_em_file module B (double_it, 0 relocs) -> %s\n",
                    passfail(wrote_B), path_B.string().c_str());
        if (!wrote_A) failures++;
        if (!wrote_B) failures++;

        if (!wrote_A || !wrote_B || jit_r != 42) {
            // Clean up any partial files and skip the load half - it can't
            // prove anything if the writes failed or the JIT gate failed.
            std::filesystem::remove(path_A); std::filesystem::remove(path_B);
            std::printf("[B] skipped load half (write/gate failure)\n");
        } else {
            // ---- Step 3/4: FRESH registry + load both .em files. The fresh
            // registry's base is at a DIFFERENT address than jit_registry's
            // (a different object), so the kind-2 patch in A's loaded code is
            // the real test: it must repoint at the fresh registry, not the
            // JIT one. Register B first (so id_B_loaded is assigned), then A. ----
            ModuleRegistry fresh_registry(/*capacity=*/4);
            bool b6 = (fresh_registry.base() != nullptr &&
                       fresh_registry.base() != jit_registry.base());
            std::printf("[B6] %s: fresh ModuleRegistry constructed (base=%p != JIT registry base=%p)\n",
                        passfail(b6), fresh_registry.base(), jit_registry.base());
            if (!b6) failures++;

            // Load B (no registry needed - B has no kind-2 relocs; pass
            // nullptr so any stray kind-2 in B would be a loud error, though B
            // has none).
            LoadedModule loaded_B;
            std::string lerr_B;
            std::unordered_map<std::string,NativeSig> no_natives;
            bool loaded_B_ok = load_em_file(path_B.string().c_str(), loaded_B, &lerr_B,
                                            /*registry=*/nullptr, &no_natives);
            std::printf("[B7] %s: load_em_file module B (no registry needed)\n", passfail(loaded_B_ok));
            if (!loaded_B_ok) { std::printf("    load_em_file(B) error: %s\n", lerr_B.c_str()); failures++; }
            else if (loaded_B.entry() == nullptr) { std::printf("    [B7] FAIL: loaded B entry null\n"); failures++; }

            // Load A WITH the fresh registry - A's kind-2 reloc patches with
            // fresh_registry.base().
            LoadedModule loaded_A;
            std::string lerr_A;
            bool loaded_A_ok = load_em_file(path_A.string().c_str(), loaded_A, &lerr_A,
                                            /*registry=*/&fresh_registry, &no_natives);
            std::printf("[B8] %s: load_em_file module A (kind-2 reloc -> fresh registry base)\n", passfail(loaded_A_ok));
            if (!loaded_A_ok) { std::printf("    load_em_file(A) error: %s\n", lerr_A.c_str()); failures++; }

            if (!loaded_B_ok || !loaded_A_ok || loaded_A.entry() == nullptr || loaded_B.entry() == nullptr) {
                std::filesystem::remove(path_A); std::filesystem::remove(path_B);
                std::printf("[B] skipped cross-call (load failure)\n");
            } else {
                // ---- Step 5: register both loaded modules in the fresh
                // registry. The order matters: B first (so its id matches the
                // displacement baked into A's caller - A was built against
                // id_B==0 in the JIT registry; the fresh registry must assign
                // B id 0 too for A's baked displacement to land on B's table).
                // Module ids are dense in registration order, so registering B
                // first gives it id 0, matching A's baked module_id*8 == 0*8. ----
                uint32_t id_B_loaded = fresh_registry.register_module("B",
                                            loaded_B.dispatch.data(), &err);
                uint32_t id_A_loaded = fresh_registry.register_module("A",
                                            loaded_A.dispatch.data(), &err);
                bool b9 = (id_B_loaded == 0);
                bool b10 = (id_A_loaded == 1);
                std::printf("[B9] %s: register loaded B -> id=%u (matches A's baked displacement)\n",
                            passfail(b9), id_B_loaded);
                std::printf("[B10] %s: register loaded A -> id=%u\n", passfail(b10), id_A_loaded);
                if (!b9) failures++;
                if (!b10) failures++;

                // ---- Step 6: the proof. Call loaded A's caller(21). It must
                // hop the FRESH registry -> loaded B's double_it -> return 42.
                // The caller's entry is loaded_A.dispatch[0] (A's slot 0). ----
                void* a_entry = loaded_A.entry(); // entry_slot == 0
                bool b11 = (a_entry != nullptr);
                std::printf("[B11] %s: loaded A entry non-null\n", passfail(b11));
                if (!b11) failures++;

                if (a_entry) {
                    auto* lcaller = reinterpret_cast<int64_t(*)(int64_t)>(a_entry);
                    int64_t r21 = lcaller(21);
                    bool b12 = (r21 == 42);
                    std::printf("[B12] %s: cross-module .em round-trip: loaded caller(21)==42, got %lld\n",
                                passfail(b12), (long long)r21);
                    if (!b12) failures++;

                    if (b12) {
                        int64_t r0 = lcaller(0), r7 = lcaller(7), rm = lcaller(-5);
                        std::printf("     loaded caller(0)==0  : %s (got %lld)\n", passfail(r0==0),   (long long)r0);
                        std::printf("     loaded caller(7)==14 : %s (got %lld)\n", passfail(r7==14),  (long long)r7);
                        std::printf("     loaded caller(-5)==-10: %s (got %lld)\n", passfail(rm==-10),(long long)rm);
                        if (!(r0==0 && r7==14 && rm==-10)) failures++;
                    }

                    // Also confirm loaded B's double_it works standalone via
                    // its own entry (loaded_B.entry()) - proves B's page is
                    // executable and correct independent of the cross-call.
                    void* b_entry = loaded_B.entry();
                    if (b_entry) {
                        auto* ldblit = reinterpret_cast<int64_t(*)(int64_t)>(b_entry);
                        bool b13 = (ldblit(21) == 42);
                        std::printf("[B13] %s: loaded B double_it(21)==42 standalone\n", passfail(b13));
                        if (!b13) failures++;
                    } else {
                        std::printf("[B13] FAIL: loaded B double_it(21)==42 standalone (entry null)\n");
                        failures++;
                    }
                }
                // loaded_A / loaded_B must outlive the calls above; they're
                // destroyed at block end (freeing their exec pages via the
                // LoadedModule destructor's free_executable loop). The
                // fresh_registry does not own the pages - it holds raw
                // dispatch-table pointers into loaded_A/B, which stay valid
                // until loaded_A/B are destroyed. Scope-exit destroys in
                // reverse declaration order, so the loaded modules go before
                // the registry - the registry just holds dangling pointers at
                // that point, but no call happens after the loaded modules are
                // destroyed. Correct.
            }
            // Always clean up the temp .em files, even on load failure.
            std::filesystem::remove(path_A); std::filesystem::remove(path_B);
        }
        // Keep the JIT-side objects (a, b, jit_registry) alive until here  - 
        // they're destroyed at block end, after all the loaded-module calls
        // above. (They're not used after the serialize step, but keeping them
        // alive is harmless and matches the scoping of Test A.)
    }

    std::printf("\nimport cross-module: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
