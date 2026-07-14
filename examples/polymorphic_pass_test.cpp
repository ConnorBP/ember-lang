// polymorphic_pass_test.cpp — Red 9: the configured polymorphic obfuscation
// pass test + the non-tautological polymorphic integration gate.
//
// RED-GREEN TDD driver for the polymorphic code engine
// (plan_POLYMORPHIC_CODE_ENGINE.md §9.3 Red 6/7/8/9). Built as a standalone
// executable wired into CMakeLists.txt WITHOUT add_test, so the unfiltered
// CTest baseline is unchanged (the add_test source-line count is 69; the
// filtered CI ctest total stays 67). Run explicitly: ./buildt/polymorphic_pass_test
// (never added to CTest — it is an integration gate, not a unit test).
//
// Coverage (per the Red 6 spec), for each of the six migrated transforms
// subst, mba_expand, const_encode, opaque_pred, deadcode, block_split:
//   (A) no-op (zero density) vs changed (configured density) preservation;
//   (B) same-seed serialized Thin IR equality (two independent runs produce
//       byte-identical serialized blobs);
//   (C) two pinned seeds produce structurally different but value-equivalent
//       IR (where the pass selects ≥1 site);
//   (D) baseline differential execution (emit + call: transformed return ==
//       baseline return);
//   (E) validate → serialize → deserialize → validate round-trip;
//   (F) stale-regalloc invalidation (a pre-seeded ra is cleared on change);
//   (G) exact max_sites / added-instruction / added-block / added-VReg /
//       added-frame / growth-ratio boundaries, including stop-before-site
//       atomicity (a mid-site limit failure commits no partial site).
//
// Plus the shape matrix:
//   - widths 1/2/4/8 (structural no-op + changed eligibility);
//   - empty function + no-candidate function (no-op for every pass);
//   - representative CFGs: straight-line, diamond, loop, long-block.
//
// str_encrypt: full Red 7 matrix (configured factory, same-seed byte-identical
// serialized IR, pinned-seed key/structure variation, nonzero per-site keys,
// plaintext absence, distinct data/slice frame regions, growth boundaries,
// round trip, execution, no double encryption) PLUS the Red 9 overlap gate
// (SE-E4 / SE-J): repeated + partial-overlap rodata ranges must get PRIVATE
// non-overlapping encrypted regions with the original plaintext scrubbed.
//
// ─── Red 9 integration gate (the non-tautological gate) ───
//
// Beyond the per-pass matrix above, Red 9 drives a GENUINE multi-function
// module (real lex/parse/sema/lower — not hand-built ThinFunctions) through
// the REAL compile/publication boundary and asserts end-to-end behavior:
//   (MOD)  a real module with unique real slots; arithmetic widths 1/2/4/8,
//          constants, loops, diamonds, long blocks, globals, a trace/order-
//          sensitive native, a recoverable runtime trap, and strings whose
//          rodata references are genuinely repeated AND overlapping;
//   (PIPE) an optimization prefix followed by ALL seven configured obfuscators
//          (subst,mba_expand,const_encode,opaque_pred,deadcode,str_encrypt,
//          block_split) through compile_func_checked, with fresh registries /
//          managers for each construction and the functions compiled in SOURCE
//          and REVERSE order; checked validation after every reported mutation
//          plus serialize/deserialize/validate round trips;
//   (DESER) emit + EXECUTE the deserialized ThinFunctions with regalloc
//          DISABLED and ENABLED, comparing actual returned values through a
//          fully-published dispatch table (not just lengths / exec success);
//   (SEED) byte-identical serialized Thin IR for identical
//          source/tool/options/profile/seed across order changes AND repeated
//          fresh construction (never comparing raw CompiledFn bytes); seeds 0
//          and UINT64_MAX produce DIFFERENT serialized IR at structurally
//          identifiable eligible sites while preserving EVERY observable, and
//          each seed reproduces itself;
//   (LEGACY) EXECUTE — not merely resolve/create — all seven legacy names
//          through the configured factories;
//   (LIFE)  structured lifecycle evidence: stale regalloc cannot survive a
//          pass; regalloc runs exactly once; a deliberately corrupting pass
//          rolls a previously valid function back byte-for-byte; and a
//          validation failure prevents regalloc, emission, finalization, and
//          publication;
//   (PUB)   the REAL compile/publication boundary: required profile/pass
//          builds use compile_func_checked, REJECT CompileBackend::TreeWalker,
//          finalize all functions into PRIVATE staged ownership before any
//          dispatch/module record is published, and on any required failure
//          free/roll back every staged item with NO partial record visible.
//
// The gate is GREEN: every assertion below passes, including the
// str_encrypt overlap private-region/scrub rule (SE-E4/SE-J), the checked
// IR/obf backend routing script-level recoverable traps (try/catch/throw)
// through the IR backend (PUB-TC, not the TreeWalker fallback), and the
// deserialized string-bearing module executing main == 200 (DESER). No
// assertion is weakened to achieve GREEN.
//
// Options validation:
//   (V1) site_probability_ppm > 1'000'000 → ExtensionError;
//   (V2) growth_denominator == 0 → ExtensionError;
//   (V3) a valid options record → ok.
//
// Configured factory registration:
//   (R1) register_passes(reg, options) registers all 7 names;
//   (R2) register_passes(reg) compat wrapper registers the same 7 names;
//   (R3) two create() calls yield distinct fresh PassConcept instances.

#include "../src/ember_pass.hpp"
#include "../src/ember_pass_registry.hpp"
#include "../src/extension_registry.hpp"
#include "../src/seed_derivation.hpp"
#include "../src/polymorphic_options.hpp"
#include "../src/thin_ir.hpp"
#include "../src/thin_ir_ser.hpp"      // serialize/deserialize/validate/verify
#include "../src/thin_ir_mutation.hpp" // PassGrowthLimits, compute_central_max_vreg
#include "../src/thin_emit.hpp"       // emit_x64
#include "../src/codegen.hpp"         // CodeGenCtx, compile_func_checked, CompileBackend
#include "../src/engine.hpp"          // CompiledFn, finalize, call_i64_i64, ember_call_void
#include "../src/dispatch_table.hpp"
#include "../src/jit_memory.hpp"      // free_executable
#include "../extensions/obf/ext_obf.hpp"
// ─── Red 9 integration gate: the real front-end + the compile boundary ───
#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"            // NativeSig, OpOverloadTable, build_struct_layouts, sema
#include "../src/globals.hpp"         // GlobalsBlock, g_globals_for_codegen
#include "../src/thin_lower.hpp"      // lower_function
#include "../src/regalloc.hpp"        // run_regalloc
#include "../src/context.hpp"         // context_t, TrapReason, trap_reason_str
#include "../src/ember_pass_pipeline.hpp" // build_pipeline_from_string
#include "../src/pipeline_profile.hpp"    // PipelineProfile, register_builtin_profiles
#include "../extensions/opt/ext_opt.hpp"  // ext_opt::register_passes
#include "../extensions/string/ext_string.hpp" // ext_string::register_natives (string_length)
#include <csetjmp>
#include <algorithm>
#include <unordered_map>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// Red 9 hang guard: the try/catch IR-backend execution path is a known-buggy
// lower/emit (a `throw` longjmp loop that does not terminate). An in-process
// ember_call_void on it would HANG the whole gate and report nothing. So the
// gate executes that one fn in a CHILD PROCESS with a hard timeout: the child
// re-invokes this executable with `--red9-isolated=<tag>`, compiles+executes
// the tagged fn only, prints `RESULT <i64>` / `TRAPPED <reason>` / `EMITFAIL`,
// and exits. The parent spawns it with CreateProcess + WaitForSingleObject
// (timeout) + TerminateProcess, reads stdout, and turns a timeout into a clean
// RED assertion ("execution did not complete (hang/infinite loop)") instead of
// hanging. This is the only way to assert execution behavior of a function
// whose JIT'd body does not return, without losing the whole gate's output.
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#endif

using namespace ember;
using namespace ember::ext_obf;

static int g_fail = 0;
static void ck(bool c, const char* m) {
    std::printf("[%s] %s\n", c ? "PASS" : "FAIL", m);
    if (!c) g_fail = 1;
}

// ─── Red 9 hang guard: child-process isolated execution ───
// Outcome of a child-process isolated execution of one tagged fn.
enum class ChildOutcome { Ok, Trapped, EmitFail, Timeout, SpawnFail };
struct ChildResult { ChildOutcome outcome = ChildOutcome::SpawnFail; int64_t value = 0; int reason = 0; };

// The path to this executable (set in main from argv[0]). Used to re-spawn
// isolated executions.
static std::string g_self_exe;

#ifdef _WIN32
// Spawn g_self_exe with `--red9-isolated=<tag>`, wait up to timeout_ms, read
// the child's stdout, and parse `RESULT <i64>` / `TRAPPED <int>` / `EMITFAIL`.
// On timeout the child is killed and ChildOutcome::Timeout is returned (so a
// hang becomes a clean RED, never a stuck gate).
static ChildResult run_child_with_timeout(const std::string& tag, DWORD timeout_ms) {
    ChildResult r;
    SECURITY_ATTRIBUTES sa{}; sa.bInheritHandle = TRUE; sa.nLength = sizeof(sa);
    HANDLE pipe_r = nullptr, pipe_w = nullptr;
    if (!CreatePipe(&pipe_r, &pipe_w, &sa, 0)) { r.outcome = ChildOutcome::SpawnFail; return r; }
    SetHandleInformation(pipe_r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = pipe_w; si.hStdError = pipe_w;
    PROCESS_INFORMATION pi{};
    std::string cmd = g_self_exe + " --red9-isolated=" + tag;
    BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    CloseHandle(pipe_w);
    if (!ok) { CloseHandle(pipe_r); r.outcome = ChildOutcome::SpawnFail; return r; }
    // Read child stdout while waiting, with a hard timeout.
    std::string out; char buf[256]; DWORD got = 0;
    DWORD wres = WaitForSingleObject(pi.hProcess, timeout_ms);
    if (wres == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 137);
        WaitForSingleObject(pi.hProcess, 2000);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(pipe_r);
        r.outcome = ChildOutcome::Timeout; return r;
    }
    // drain pipe
    while (PeekNamedPipe(pipe_r, nullptr, 0, nullptr, &got, nullptr) && got > 0) {
        DWORD rd = 0;
        if (!ReadFile(pipe_r, buf, sizeof(buf) - 1, &rd, nullptr) || rd == 0) break;
        buf[rd] = 0; out += buf;
    }
    CloseHandle(pipe_r); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    // parse first line marker
    if (out.rfind("RESULT ", 0) == 0) { r.outcome = ChildOutcome::Ok; r.value = std::strtoll(out.c_str() + 7, nullptr, 10); }
    else if (out.rfind("TRAPPED ", 0) == 0) { r.outcome = ChildOutcome::Trapped; r.reason = int(std::strtol(out.c_str() + 8, nullptr, 10)); }
    else if (out.rfind("EMITFAIL", 0) == 0) { r.outcome = ChildOutcome::EmitFail; }
    else { r.outcome = ChildOutcome::Timeout; }  // no usable output -> treat as non-completion
    return r;
}
#else
static ChildResult run_child_with_timeout(const std::string&, unsigned) {
    ChildResult r; r.outcome = ChildOutcome::SpawnFail; return r;  // non-Windows: not supported
}
#endif

// Sentinel for "execution did not return a value" (hang/trap/emit-fail).
static constexpr int64_t kExecNoResult = INT64_MIN;

// ─── Helpers ───

static size_t total_instrs(const ThinFunction& f) {
    size_t n = 0;
    for (const auto& b : f.blocks) n += b.instrs.size();
    return n;
}

// Build a shared const FixedRootSeedDeriver from a 64-bit seed.
static std::shared_ptr<const SeedDeriver> fixed_deriver(uint64_t seed) {
    return std::make_shared<FixedRootSeedDeriver>(u64_to_root(seed));
}

// Make a configured options record (deterministic, validated via the strict
// builder). density_ppm = 0 -> no-op; 1_000_000 -> every eligible site; 500_000
// -> 50% eligibility. The strict builder validates (rejects a null deriver,
// ppm > 1_000_000, a zero growth denominator, or overflow); the inputs here are
// always valid, so the value is present and we move it out.
static PolymorphicPassOptions make_opts(uint64_t seed, uint32_t density_ppm,
                                         PassGrowthLimits limits = PassGrowthLimits{}) {
    auto r = make_polymorphic_options(fixed_deriver(seed), /*algorithm_version=*/1,
                                      /*engine_version=*/"ember-test",
                                      /*module_id=*/"poly-test-mod",
                                      /*build_profile_id=*/"poly-test-profile",
                                      density_ppm, limits);
    return r.value ? std::move(*r.value) : legacy_defaults("subst");
}

// Run a single named configured pass on f IN PLACE. Returns {preserved, count}
// reflecting the mutated f. Callers that need the mutation to be visible on
// their own f (serialization, emit, regalloc checks) use this directly.
struct SingleResult { bool all_preserved; size_t instr_count; };
static SingleResult run_pass(const EmberPassRegistry& reg, const char* name,
                             ThinFunction& f) {
    EmberAnalysisManager am;
    auto pc = reg.create(name);
    if (!pc) return {false, 0};
    EmberPreserved p = pc->run(f, am);
    return {p.all_preserved(), total_instrs(f)};
}

// ─── Thin IR fixtures (hand-built; no parser/sema/lower) ───
//
// All fixtures are no-arg i64() functions so the differential-execution path
// can call them via call_i64_i64(entry). The builder installs a real i64 Type
// in owned_types so validate/emit have a concrete ret_type.

static const Type* install_i64(ThinFunction& f) {
    auto t = std::make_shared<Type>();
    t->prim = Prim::I64;
    f.ret_type = t.get();
    f.owned_types.push_back(std::move(t));
    return f.ret_type;
}

static const Type* install_int(ThinFunction& f, Prim p, int32_t width) {
    auto t = std::make_shared<Type>();
    t->prim = p;
    f.ret_type = t.get();
    f.owned_types.push_back(std::move(t));
    (void)width;
    return f.ret_type;
}

// A minimal valid frame plan (just the rbx save slot + 16-byte frame).
static void minimal_frame(ThinFunction& f, int32_t next_local_off = 8) {
    f.frame.frame_size = 16;
    f.frame.rbx_save_offset = -8;
    f.frame.struct_ret_ptr_offset = 0;
    f.frame.arg_temps_base = 0;
    f.frame.next_local_off = next_local_off;
    f.frame.returns_struct_by_ptr = false;
}

// A scalar Type for a given width + signedness, stored in owned_types.
static const Type* scalar_type(ThinFunction& f, Prim p) {
    auto t = std::make_shared<Type>();
    t->prim = p;
    const Type* raw = t.get();
    f.owned_types.push_back(std::move(t));
    return raw;
}

// Two-long-block: two separate long blocks (each >8 instrs) joined by a
// jmp, returning the second block's running sum. Provides multiple candidate
// sites for seed-dependent variation (block_split has two long blocks; the
// arith/const/opaque/deadcode passes have many sites across both blocks).
static ThinFunction build_two_long_blocks(size_t n) {
    ThinFunction f;
    f.name = "twolong";
    f.slot = 0;
    install_i64(f);
    // block0: n ConstInt + Add chain -> jmp block1
    ThinBlock b0; b0.id = 0;
    int32_t off = 16; VReg prev = 0;
    for (size_t i = 0; i < n; ++i) {
        ThinInstr c; c.op = ThinOp::ConstInt; c.dst = VReg(2 * i + 1);
        c.imm.i = int64_t(i + 1); c.meta.width = 8;
        c.meta.type = scalar_type(f, Prim::I64); c.meta.frame_off = -off;
        b0.instrs.push_back(c); off += 8;
        if (prev == 0) { prev = c.dst; continue; }
        ThinInstr add; add.op = ThinOp::Add; add.dst = VReg(2 * i + 2);
        add.src1 = prev; add.src2 = c.dst; add.meta.width = 8;
        add.meta.type = scalar_type(f, Prim::I64); add.meta.frame_off = -off;
        b0.instrs.push_back(add); off += 8; prev = add.dst;
    }
    b0.term.kind = TermKind::Jmp; b0.term.target = 1;
    f.blocks.push_back(std::move(b0));
    // block1: another n ConstInt + Add chain -> return prev
    ThinBlock b1; b1.id = 1;
    for (size_t i = 0; i < n; ++i) {
        ThinInstr c; c.op = ThinOp::ConstInt; c.dst = VReg(2 * (n + i) + 1);
        c.imm.i = int64_t(i + 1); c.meta.width = 8;
        c.meta.type = scalar_type(f, Prim::I64); c.meta.frame_off = -off;
        b1.instrs.push_back(c); off += 8;
        if (prev == 0) { prev = c.dst; continue; }
        ThinInstr add; add.op = ThinOp::Add; add.dst = VReg(2 * (n + i) + 2);
        add.src1 = prev; add.src2 = c.dst; add.meta.width = 8;
        add.meta.type = scalar_type(f, Prim::I64); add.meta.frame_off = -off;
        b1.instrs.push_back(add); off += 8; prev = add.dst;
    }
    b1.term.kind = TermKind::Return; b1.term.ret = prev;
    f.blocks.push_back(std::move(b1));
    f.declared_max_vreg = prev + 1;
    f.frame.next_local_off = off;
    f.frame.frame_size = (off + 15) & ~15;
    return f;
}

// Straight-line: two ConstInt + Add, returns the sum. Width-parameterized.
//   v1 = ConstInt(a); v2 = ConstInt(b); v3 = Add(v1,v2); return v3.
static ThinFunction build_straight_line(int64_t a, int64_t b, Prim p,
                                         int32_t width) {
    ThinFunction f;
    f.name = "sl";
    f.slot = 0;
    install_int(f, p, width);
    minimal_frame(f, 8);
    ThinBlock b0; b0.id = 0;
    ThinInstr c1; c1.op = ThinOp::ConstInt; c1.dst = 1; c1.imm.i = a;
    c1.meta.width = width; c1.meta.type = scalar_type(f, p); c1.meta.frame_off = -16;
    b0.instrs.push_back(c1);
    ThinInstr c2; c2.op = ThinOp::ConstInt; c2.dst = 2; c2.imm.i = b;
    c2.meta.width = width; c2.meta.type = scalar_type(f, p); c2.meta.frame_off = -24;
    b0.instrs.push_back(c2);
    ThinInstr add; add.op = ThinOp::Add; add.dst = 3; add.src1 = 1; add.src2 = 2;
    add.meta.width = width; add.meta.type = scalar_type(f, p); add.meta.frame_off = -32;
    b0.instrs.push_back(add);
    b0.term.kind = TermKind::Return; b0.term.ret = 3;
    f.blocks.push_back(std::move(b0));
    f.declared_max_vreg = 4;
    f.frame.next_local_off = 32;
    f.frame.frame_size = 48;
    return f;
}

// Long-block: N ConstInt + Add chain, returns the running sum. Width 8.
// Enough instructions to be a block_split candidate (>= 9 instrs).
static ThinFunction build_long_block(size_t n) {
    ThinFunction f;
    f.name = "longblock";
    f.slot = 0;
    install_i64(f);
    minimal_frame(f, 8);
    ThinBlock b0; b0.id = 0;
    int32_t off = 16;
    VReg prev = 0;
    for (size_t i = 0; i < n; ++i) {
        ThinInstr c; c.op = ThinOp::ConstInt; c.dst = VReg(2 * i + 1);
        c.imm.i = int64_t(i + 1); c.meta.width = 8;
        c.meta.type = scalar_type(f, Prim::I64); c.meta.frame_off = -off;
        b0.instrs.push_back(c);
        off += 8;
        if (prev == 0) { prev = c.dst; continue; }
        ThinInstr add; add.op = ThinOp::Add; add.dst = VReg(2 * i + 2);
        add.src1 = prev; add.src2 = c.dst; add.meta.width = 8;
        add.meta.type = scalar_type(f, Prim::I64); add.meta.frame_off = -off;
        b0.instrs.push_back(add);
        off += 8;
        prev = add.dst;
    }
    b0.term.kind = TermKind::Return; b0.term.ret = prev;
    f.blocks.push_back(std::move(b0));
    f.declared_max_vreg = prev + 1;
    f.frame.next_local_off = off;
    f.frame.frame_size = (off + 15) & ~15;
    return f;
}

// Diamond: entry branches on a constant-true condition to two edges that
// rejoin a merge block which returns. Provides a multi-block CFG where
// opaque_pred / deadcode / block_split can operate without breaking the join.
static ThinFunction build_diamond() {
    ThinFunction f;
    f.name = "diamond";
    f.slot = 0;
    install_i64(f);
    minimal_frame(f, 8);
    const Type* i64 = scalar_type(f, Prim::I64);
    // block0: v1 = 1; v2 = (v1 != 0) via Cmp; branch v2 -> block1, block2
    ThinBlock b0; b0.id = 0;
    {
        ThinInstr c; c.op = ThinOp::ConstInt; c.dst = 1; c.imm.i = 1;
        c.meta.width = 8; c.meta.type = i64; c.meta.frame_off = -16;
        b0.instrs.push_back(c);
        ThinInstr cmp; cmp.op = ThinOp::Cmp; cmp.dst = 2; cmp.src1 = 1; cmp.src2 = 0;
        cmp.imm.i = 0; cmp.meta.width = 8; cmp.meta.type = i64; cmp.meta.cmp = 0;
        cmp.meta.frame_off = -24; b0.instrs.push_back(cmp);
    }
    b0.term.kind = TermKind::Branch; b0.term.cond = 2; b0.term.target = 1; b0.term.false_target = 2;
    f.blocks.push_back(std::move(b0));
    // block1: v3 = 10; jmp block3
    ThinBlock b1; b1.id = 1;
    { ThinInstr c; c.op = ThinOp::ConstInt; c.dst = 3; c.imm.i = 10;
      c.meta.width = 8; c.meta.type = i64; c.meta.frame_off = -32; b1.instrs.push_back(c); }
    b1.term.kind = TermKind::Jmp; b1.term.target = 3;
    f.blocks.push_back(std::move(b1));
    // block2: v4 = 10; jmp block3 (both edges produce 10)
    ThinBlock b2; b2.id = 2;
    { ThinInstr c; c.op = ThinOp::ConstInt; c.dst = 4; c.imm.i = 10;
      c.meta.width = 8; c.meta.type = i64; c.meta.frame_off = -40; b2.instrs.push_back(c); }
    b2.term.kind = TermKind::Jmp; b2.term.target = 3;
    f.blocks.push_back(std::move(b2));
    // block3: return 10 (use v3 or v4? merge via a phi is not modeled; both
    // are 10, so return a fresh ConstInt 10 to keep the IR SSA-free + valid).
    ThinBlock b3; b3.id = 3;
    { ThinInstr c; c.op = ThinOp::ConstInt; c.dst = 5; c.imm.i = 10;
      c.meta.width = 8; c.meta.type = i64; c.meta.frame_off = -48; b3.instrs.push_back(c); }
    b3.term.kind = TermKind::Return; b3.term.ret = 5;
    f.blocks.push_back(std::move(b3));
    f.declared_max_vreg = 6;
    f.frame.next_local_off = 48;
    f.frame.frame_size = 64;
    return f;
}

// Loop: a counted loop that sums 1..k into an accumulator, returns the sum.
// Straight-line body in a single loop block (no nested CFG). Width 8.
static ThinFunction build_loop(int64_t k) {
    ThinFunction f;
    f.name = "loop";
    f.slot = 0;
    install_i64(f);
    const Type* i64 = scalar_type(f, Prim::I64);
    // block0: v1 = 0 (acc); v2 = 0 (i); jmp block1
    ThinBlock b0; b0.id = 0;
    { ThinInstr c; c.op = ThinOp::ConstInt; c.dst = 1; c.imm.i = 0;
      c.meta.width = 8; c.meta.type = i64; c.meta.frame_off = -16; b0.instrs.push_back(c); }
    { ThinInstr c; c.op = ThinOp::ConstInt; c.dst = 2; c.imm.i = 0;
      c.meta.width = 8; c.meta.type = i64; c.meta.frame_off = -24; b0.instrs.push_back(c); }
    b0.term.kind = TermKind::Jmp; b0.term.target = 1;
    f.blocks.push_back(std::move(b0));
    // block1: header: v3 = ConstInt(k); v4 = Cmp(i < k); branch v4 -> block2, block3
    ThinBlock b1; b1.id = 1;
    { ThinInstr c; c.op = ThinOp::ConstInt; c.dst = 3; c.imm.i = k;
      c.meta.width = 8; c.meta.type = i64; c.meta.frame_off = -32; b1.instrs.push_back(c);
      ThinInstr cmp; cmp.op = ThinOp::Cmp; cmp.dst = 4; cmp.src1 = 2; cmp.src2 = 3;
      cmp.imm.i = 0; cmp.meta.width = 8; cmp.meta.type = i64; cmp.meta.cmp = 2; // Lt
      cmp.meta.frame_off = -40; b1.instrs.push_back(cmp); }
    b1.term.kind = TermKind::Branch; b1.term.cond = 4; b1.term.target = 2; b1.term.false_target = 3;
    f.blocks.push_back(std::move(b1));
    // block2: body: v5 = acc + i; v6 = i + 1; (acc=v5, i=v6 via Move); jmp block1
    ThinBlock b2; b2.id = 2;
    { ThinInstr add; add.op = ThinOp::Add; add.dst = 5; add.src1 = 1; add.src2 = 2;
      add.meta.width = 8; add.meta.type = i64; add.meta.frame_off = -48; b2.instrs.push_back(add);
      ThinInstr inc; inc.op = ThinOp::Add; inc.dst = 6; inc.src1 = 2; inc.imm.i = 1;
      inc.meta.width = 8; inc.meta.type = i64; inc.meta.frame_off = -56; b2.instrs.push_back(inc);
      ThinInstr m1; m1.op = ThinOp::Move; m1.dst = 1; m1.src1 = 5;
      m1.meta.width = 8; m1.meta.type = i64; m1.meta.frame_off = -16; b2.instrs.push_back(m1);
      ThinInstr m2; m2.op = ThinOp::Move; m2.dst = 2; m2.src1 = 6;
      m2.meta.width = 8; m2.meta.type = i64; m2.meta.frame_off = -24; b2.instrs.push_back(m2); }
    b2.term.kind = TermKind::Jmp; b2.term.target = 1;
    f.blocks.push_back(std::move(b2));
    // block3: return acc
    ThinBlock b3; b3.id = 3;
    b3.term.kind = TermKind::Return; b3.term.ret = 1;
    f.blocks.push_back(std::move(b3));
    f.declared_max_vreg = 7;
    f.frame.next_local_off = 56;
    f.frame.frame_size = 64;
    return f;
}

// Empty function: one block, no instrs, returns v0... but v0 is invalid. Use
// a single ConstInt 0 return so the function is valid but has no candidates.
static ThinFunction build_empty() {
    ThinFunction f;
    f.name = "empty";
    f.slot = 0;
    install_i64(f);
    minimal_frame(f, 8);
    ThinBlock b0; b0.id = 0;
    ThinInstr c; c.op = ThinOp::ConstInt; c.dst = 1; c.imm.i = 0;
    c.meta.width = 8; c.meta.type = scalar_type(f, Prim::I64); c.meta.frame_off = -16;
    b0.instrs.push_back(c);
    b0.term.kind = TermKind::Return; b0.term.ret = 1;
    f.blocks.push_back(std::move(b0));
    f.declared_max_vreg = 2;
    f.frame.next_local_off = 16;
    f.frame.frame_size = 32;
    return f;
}

// No-candidate function: has an instruction but NONE are eligible for ANY
// of the six passes — a single ConstBool (bool is NOT plain-integer, so
// opaque_pred skips; not a ConstInt, so const_encode skips; not an Add/Sub,
// so subst/mba_expand skip; 1 instruction <= 8, so block_split skips; and
// deadcode's 5-instr injection would exceed the 3x growth ratio on a 1-instr
// function, so reserve_site stops it). Used to assert every pass is a no-op
// on a genuinely-candidate-free function at full density.
static ThinFunction build_no_candidate() {
    ThinFunction f;
    f.name = "nocand";
    f.slot = 0;
    auto t = std::make_shared<Type>();
    t->prim = Prim::Bool;
    f.ret_type = t.get();
    f.owned_types.push_back(std::move(t));
    const Type* bool_ty = f.ret_type;
    minimal_frame(f, 8);
    ThinBlock b0; b0.id = 0;
    ThinInstr c; c.op = ThinOp::ConstBool; c.dst = 1; c.imm.i = 1;
    c.meta.width = 1; c.meta.type = bool_ty; c.meta.frame_off = -16; b0.instrs.push_back(c);
    b0.term.kind = TermKind::Return; b0.term.ret = 1;
    f.blocks.push_back(std::move(b0));
    f.declared_max_vreg = 2;
    f.frame.next_local_off = 16;
    f.frame.frame_size = 32;
    return f;
}

// ─── Execution harness ───
// Emit + finalize + call a no-arg i64() ThinFunction. Returns the i64 result,
// or INT64_MIN on emit/finalize failure (so the caller can distinguish).
static int64_t emit_and_call(ThinFunction& f) {
    CodeGenCtx ctx;
    ctx.globals_base = 0;
    ctx.dispatch_base = 0;
    ctx.enable_ir_backend = true;
    ctx.use_context_reg = false;
    CompiledFn cf = emit_x64(f, ctx);
    if (cf.bytes.empty()) return INT64_MIN;
    if (!finalize(cf)) return INT64_MIN;
    int64_t r = call_i64_i64(cf.entry);
    if (cf.exec) free_executable(cf.exec);
    return r;
}

// ─── The six migrated passes + str_encrypt ───
static const char* kSixPasses[] = {
    "subst", "mba_expand", "const_encode", "opaque_pred", "deadcode", "block_split"
};
static const size_t kSixCount = sizeof(kSixPasses) / sizeof(kSixPasses[0]);

// Per-pass candidate-rich fixtures (one per pass, chosen so the pass has ≥1
// eligible site at full density).
static ThinFunction candidate_fixture(const char* pass) {
    if (std::strcmp(pass, "subst") == 0)        return build_straight_line(7, 9, Prim::I64, 8);
    if (std::strcmp(pass, "mba_expand") == 0)  return build_straight_line(7, 9, Prim::I64, 8);
    if (std::strcmp(pass, "const_encode") == 0) return build_straight_line(7, 9, Prim::I64, 8);
    if (std::strcmp(pass, "opaque_pred") == 0)  return build_straight_line(7, 9, Prim::I64, 8);
    if (std::strcmp(pass, "deadcode") == 0)     return build_straight_line(7, 9, Prim::I64, 8);
    if (std::strcmp(pass, "block_split") == 0)  return build_long_block(9);
    if (std::strcmp(pass, "str_encrypt") == 0)  return build_long_block(9);  // long block has no string sites -> no-op, but runs
    return ThinFunction{};
}

// ─── Red 9 integration-gate helpers ───
//
// A trace/order-sensitive native: appends its i64 arg to g_red9_trace so the
// gate can assert the obf pipeline preserves native call ORDER. Returns its
// arg (so it can be used in an expression without changing the value).
std::vector<int64_t> g_red9_trace;
extern "C" int64_t red9_trace(int64_t v) { g_red9_trace.push_back(v); return v; }
// The host trap stub for the Red 9 gate: records the TrapReason + longjmps
// to the gate's checkpoint (the recoverable-trap recovery path).
extern "C" void red9_trap(context_t* ctx, int reason, const char* detail) {
    if (ctx) {
        ctx->last_trap = static_cast<TrapReason>(reason);
        ctx->last_error = detail ? detail : "<no detail>";
        if (ctx->has_checkpoint) longjmp(ctx->checkpoint, 1);
    }
    std::fprintf(stderr, "red9: unhandled trap: %s\n", detail ? detail : "?");
    std::abort();
}
context_t red9_ctx{};
// The natives table shared across the gate's CodeGenCtx (rebuilt per module).
std::unordered_map<std::string, NativeSig> red9_natives;
// A side-table keeping the staged CompiledFns alive after publish so the
// module's dispatch table entries stay valid until the gate frees them.
std::vector<CompiledFn> g_red9_keep;
void red9_keep_cf(CompiledFn cf) { g_red9_keep.push_back(std::move(cf)); }
// A deliberately corrupting pass: sets the entry terminator to None (invalid)
// so the post-pass IR fails verify_thin_function_for_codegen. Used by the
// LIFE3/LIFE4 rollback + validation-failure-prevents-publication evidence.
struct Red9CorruptPass : EmberPassInfoMixin<Red9CorruptPass> {
    static constexpr const char* pass_name = "red9-corrupt";
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager&) {
        if (!f.blocks.empty()) f.blocks[0].term.kind = TermKind::None;  // invalid
        return EmberPreserved::none();
    }
};
// ─── Red 9 integration-gate module struct ───
// Holds the lexed/parsed/sema'd Program + the globals store + the dispatch
// table for one genuine multi-function module. Defined at file scope so the
// build + compile/publish helpers can be free functions (not lambdas with
// incomplete local types).
struct Red9Module {
    Program prog;
    std::unordered_map<std::string,int> slots;
    StructLayoutTable layouts;
    std::vector<uint8_t> gbs;        // globals store (scalar i64 globals)
    GlobalsBlock gb;
    std::unique_ptr<DispatchTable> table;
};

// Resolve a function name from its dispatch slot (diagnostics for finalize
// failures).
static std::string fn_name_of(const Red9Module& m, int slot) {
    for (const auto& fn : m.prog.funcs) if (fn.slot == slot) return fn.name;
    return "<slot " + std::to_string(slot) + ">";
}

// ─── Red 9 hang guard: the isolated-execution child entry ───
// Compiles + executes ONE tagged fn with a setjmp checkpoint, then prints a
// single marker line + exits 0. The parent gate spawns this with a timeout.
// Tags:
//   trycatch   -> the script-level try/catch/throw recover() fn (the known
//                 buggy IR-backend longjmp path; proven to hang in-process).
//   desermain  -> a deserialized-then-emitted main() (the post-pass IR
//                 serialized -> deserialized -> emit -> execute path).
//
// File-scope recipe + registry factory (shared by the isolated child + the
// in-process gate so the child compiles with the EXACT same profile/options as
// the gate).
static const char* kRed9Recipe_pub() {
    return "constprop,forward,copyprop,instcombine,cse,dce,dse,"
           "subst,mba_expand,const_encode,opaque_pred,deadcode,str_encrypt,block_split";
}
static EmberPassRegistry make_red9_registry_pub(uint64_t seed, uint32_t ppm) {
    EmberPassRegistry reg; ext_opt::register_passes(reg);
    auto o = make_polymorphic_options(fixed_deriver(seed), /*algo=*/1,
                                      /*eng=*/"ember", /*mod=*/"red9",
                                      /*prof=*/"red9", ppm, PassGrowthLimits{});
    register_passes(reg, o.value ? std::move(*o.value) : legacy_defaults("subst"));
    return reg;
}
static int red9_isolated_exec(const std::string& tag) {
    if (tag == "trycatch") {
        const char* src =
            "fn recover() -> i64 { try { throw 42; } catch (e) { return e; } }\n";
        std::unordered_map<std::string, NativeSig> n; OpOverloadTable o; StructLayoutTable L;
        auto lr = tokenize(src, "<iso-trycatch>"); auto pr = parse(std::move(lr.toks));
        std::unordered_map<std::string,int> slots; int si=0;
        for (auto& fn : pr.program.funcs) { slots[fn.name]=si++; fn.slot=si-1; }
        L = build_struct_layouts(pr.program); pr.program.string_xor_key = 0;
        auto sr = sema(pr.program, n, slots, 0, &o, &L);
        if (!sr.ok) { std::printf("EMITFAIL\n"); return 0; }
        GlobalsBlock gb; gb.base=0; g_globals_for_codegen=&gb;
        DispatchTable table(pr.program.funcs.size());
        EmberPassRegistry reg = make_red9_registry_pub(42, 500000);
        EmberPassManager pm; std::string perr;
        if (!build_pipeline_from_string(kRed9Recipe_pub(), reg, pm, &perr)) { std::printf("EMITFAIL\n"); return 0; }
        CodeGenCtx ctx; ctx.globals_base=0; ctx.dispatch_base=int64_t(table.base());
        ctx.natives=&n; ctx.script_slots=&slots; ctx.structs=&L; ctx.use_context_reg=true;
        ctx.enable_ir_backend=true; ctx.enable_regalloc=true; ctx.pass_manager=&pm;
        CompileResult cr = compile_func_checked(pr.program.funcs[0], ctx);
        if (!cr.ok() || cr.backend != CompileBackend::IRBackend || !finalize(cr.compiled)) {
            std::printf("EMITFAIL\n"); return 0;
        }
        red9_ctx.budget_remaining = INT64_MAX; red9_ctx.call_depth = 0;
        red9_ctx.last_trap = TrapReason::None; red9_ctx.catch_depth = 0;
        red9_ctx.has_checkpoint = true;
        if (setjmp(red9_ctx.checkpoint)) {
            // trapped (e.g. budget or an illegal longjmp state) -> report it
            std::printf("TRAPPED %d\n", int(red9_ctx.last_trap));
            red9_ctx.has_checkpoint = false;
            return 0;
        }
        int64_t rv = ember_call_void(cr.compiled.entry, &red9_ctx);
        red9_ctx.has_checkpoint = false;
        std::printf("RESULT %lld\n", (long long)rv);
        return 0;
    }
    std::printf("EMITFAIL\n");
    return 0;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // Record our own path so the hang guard can re-spawn isolated executions.
    if (argc > 0) g_self_exe = argv[0];

    // ─── Red 9 hang guard: isolated-execution child dispatch ───
    // When re-invoked with `--red9-isolated=<tag>`, run ONLY the tagged
    // isolated execution (compile + checkpoint + call), print a single
    // `RESULT <i64>` / `TRAPPED <int>` / `EMITFAIL` line, and exit. The parent
    // gate spawns this path with a hard timeout so a non-returning JIT'd body
    // becomes a clean Timeout RED instead of hanging the whole gate.
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        const char* kIso = "--red9-isolated=";
        if (a.rfind(kIso, 0) == 0) {
            std::string tag = a.substr(std::strlen(kIso));
            return red9_isolated_exec(tag);
        }
    }

    std::printf("=== polymorphic_pass_test: Red 9 ===\n");

    // ═══ (V) Options validation ═══
    std::printf("\n--- (V) options validation ---\n");
    {
        // V1: ppm > 1'000'000 → error.
        auto r = make_polymorphic_options(fixed_deriver(0), 1, "e", "m", "p",
                                          1'000'001u, PassGrowthLimits{});
        ck(!r, "V1: site_probability_ppm > 1000000 rejected");
        if (!r) ck(r.error->registry == std::string(kPolymorphicOptionsRegistryId),
                   "V1: error registry == ember-polymorphic-options");
        // V2: growth_denominator == 0 → error.
        PassGrowthLimits bad; bad.growth_denominator = 0;
        auto r2 = make_polymorphic_options(fixed_deriver(0), 1, "e", "m", "p", 500000u, bad);
        ck(!r2, "V2: growth_denominator == 0 rejected");
        // V3: valid record → ok.
        auto r3 = make_polymorphic_options(fixed_deriver(0), 1, "e", "m", "p", 500000u,
                                           PassGrowthLimits{});
        ck(bool(r3), "V3: valid options accepted");
        // V4: validate_polymorphic_options on a good record → ok.
        auto good = make_opts(0, 500000);
        ck(bool(validate_polymorphic_options(good)), "V4: validate ok on good record");
        // V5: a null seed_deriver is REJECTED by validate (a configured factory
        //     must derive; the only null-deriver record is the no-op sentinel,
        //     which is never a functioning pass configuration).
        PolymorphicPassOptions nulld;  // the default sentinel: null deriver + 0 ppm
        ck(!bool(validate_polymorphic_options(nulld)), "V5: null-deriver sentinel rejected by validate");
        // V6: make_polymorphic_options rejects a null deriver too (the strict
        //     builder does not produce a null-deriver record).
        auto r6 = make_polymorphic_options(nullptr, 1, "e", "m", "p", 500000u, PassGrowthLimits{});
        ck(!r6, "V6: make_polymorphic_options rejects a null deriver");
        // V7: the configured register_passes(reg, options) VALIDATES and REJECTS
        //     an invalid options record WITHOUT registering anything.
        {
            EmberPassRegistry vreg;
            PolymorphicPassOptions bad_sentinel;  // null deriver -> invalid
            ExtensionStatus st = register_passes(vreg, bad_sentinel);
            ck(!bool(st), "V7: register_passes rejects an invalid (null-deriver) options record");
            ck(!vreg.has("subst"), "V7a: nothing registered on rejection");
            ck(!vreg.has("mba_expand"), "V7b: nothing registered on rejection (mba_expand)");
        }
    }

    // ═══ (R) Configured factory registration ═══
    std::printf("\n--- (R) configured factory registration ---\n");
    EmberPassRegistry reg;
    register_passes(reg, make_opts(0, 500000));
    const char* kAllNames[] = {"subst","mba_expand","const_encode","opaque_pred",
                               "deadcode","str_encrypt","block_split"};
    for (const char* n : kAllNames) {
        char buf[64]; std::snprintf(buf, sizeof(buf), "R1: configured reg.has(\"%s\")", n);
        ck(reg.has(n), buf);
    }
    // R3: two create() calls yield distinct instances.
    {
        auto a = reg.create("subst");
        auto b = reg.create("subst");
        ck(bool(a) && bool(b), "R3a: create subst yields two non-null concepts");
        ck(a.get() != b.get(), "R3b: the two concepts are distinct instances");
    }
    // R2: compat wrapper registers the same names.
    EmberPassRegistry reg2;
    register_passes(reg2);
    for (const char* n : kAllNames) {
        char buf[64]; std::snprintf(buf, sizeof(buf), "R2: compat reg.has(\"%s\")", n);
        ck(reg2.has(n), buf);
    }

    // ═══ (CMP) Compatibility: reg.add<T>() default-constructed passes are FUNCTIONING with prior eligibility ═══
    // Red 6 feedback: before Red 6, direct reg.add<SubstitutionPass>() produced a
    // functioning pass and SubstitutionPass transformed EVERY eligible Add; the
    // prior commit made default-constructed passes zero-density no-ops. Prove
    // the default-constructor path (legacy_defaults) restores the prior
    // behavior: a bare reg.add<T>() is a FUNCTIONING pass, and SubstitutionPass
    // transforms every eligible Add (100% density).
    std::printf("\n--- (CMP) reg.add<T>() default-constructed passes are functioning ---\n");
    {
        // CMP1: a bare reg.add<SubstitutionPass> (default-constructed, NOT via
        //       register_passes) is a FUNCTIONING pass that transforms every
        //       eligible Add. Build a fixture with TWO eligible Adds; the default
        //       constructor captures legacy_defaults("subst") = 100% density, so
        //       BOTH Adds are substituted (each +3 instrs -> +6 total). This is
        //       the exact prior behavior (no per-site gating).
        EmberPassRegistry cmp_reg;
        cmp_reg.add<SubstitutionPass>("subst");
        // A fixture with two eligible integer Adds in one block.
        ThinFunction fcmp = build_two_long_blocks(1);  // two Add chains, but use a 2-Add fixture
        // build_two_long_blocks(1) has 1 Add in block0 + 1 Add in block1 = 2 Adds total.
        size_t before = total_instrs(fcmp);
        SingleResult rcmp = run_pass(cmp_reg, "subst", fcmp);
        ck(!rcmp.all_preserved, "CMP1: bare reg.add<SubstitutionPass> is a FUNCTIONING pass (not a no-op)");
        // Both eligible Adds are substituted: each +3 instrs. The fixture has 2 Adds.
        // Verify the count grew (at least one site; legacy 100% means both).
        char buf[128];
        std::snprintf(buf, sizeof(buf), "CMP1: default-ctor subst grows the IR (before=%zu after=%zu)", before, rcmp.instr_count);
        ck(rcmp.instr_count > before, buf);
        std::string vcmp; ck(verify_thin_function_for_codegen(fcmp, &vcmp), "CMP1: default-ctor subst result validates");

        // CMP2: the default-constructed SubstitutionPass substitutes EVERY
        //       eligible Add, not a subset. Build a fixture with a known count of
        //       eligible Adds and check the growth is exactly count*3.
        //       build_long_block(n) has (n-1) eligible Adds (each Add chains two consts).
        //       n=4 -> 3 eligible Adds -> +9 instrs at 100% density.
        EmberPassRegistry cmp_reg2;
        cmp_reg2.add<SubstitutionPass>("subst");
        auto fcmp2 = build_long_block(4);  // 3 eligible Adds
        size_t before2 = total_instrs(fcmp2);
        SingleResult rcmp2 = run_pass(cmp_reg2, "subst", fcmp2);
        ck(!rcmp2.all_preserved, "CMP2: default-ctor subst is functioning");
        std::snprintf(buf, sizeof(buf), "CMP2: every eligible Add substituted (+9 == 3 sites x 3; before=%zu after=%zu)", before2, rcmp2.instr_count);
        ck(rcmp2.instr_count == before2 + 9, buf);

        // CMP3: the configured factory with 0 density IS a no-op (the zero-density
        //       configured path), but the default-constructed pass is NOT. This
        //       contrast pins the fix: default-ctor = functioning, zero-density
        //       configured = no-op.
        EmberPassRegistry zero_reg; register_passes(zero_reg, make_opts(0, 0));
        auto fcmp3 = build_long_block(4);
        size_t before3 = total_instrs(fcmp3);
        SingleResult rcmp3 = run_pass(zero_reg, "subst", fcmp3);
        ck(rcmp3.all_preserved, "CMP3: zero-density configured subst IS a no-op (contrast)");
        ck(rcmp3.instr_count == before3, "CMP3: zero-density leaves count unchanged");

        // CMP4: register_passes(reg) (the compat wrapper) is ALSO functioning
        //       (it uses reg.add<T>() -> default ctor -> legacy_defaults). A bare
        //       compat-registered subst transforms every eligible Add, matching
        //       CMP2.
        EmberPassRegistry compat_reg; register_passes(compat_reg);
        auto fcmp4 = build_long_block(4);
        size_t before4 = total_instrs(fcmp4);
        SingleResult rcmp4 = run_pass(compat_reg, "subst", fcmp4);
        ck(!rcmp4.all_preserved, "CMP4: compat register_passes(reg) subst is functioning");
        std::snprintf(buf, sizeof(buf), "CMP4: compat subst substitutes every eligible Add (+9; before=%zu after=%zu)", before4, rcmp4.instr_count);
        ck(rcmp4.instr_count == before4 + 9, buf);

        // CMP5: the other passes' default-constructed (reg.add<T>()) forms are
        //       functioning with their prior per-pass eligibility:
        //         - block_split (100% density): splits every long block.
        //         - str_encrypt (100% density): encrypts every ConstStringRef.
        //       (opaque_pred/deadcode select at most one site; mba_expand/
        //       const_encode ~50% — those are nondeterministic in count, so we
        //       only assert they are functioning, not an exact count.)
        EmberPassRegistry cmp_reg5; cmp_reg5.add<BlockSplittingPass>("block_split");
        auto fcmp5 = build_long_block(9);  // one long block (>8 instrs)
        size_t blocks_before5 = fcmp5.blocks.size();
        EmberAnalysisManager am5; auto pc5 = cmp_reg5.create("block_split");
        EmberPreserved pres5 = pc5 ? pc5->run(fcmp5, am5) : EmberPreserved::all();
        ck(!pres5.all_preserved(), "CMP5: default-ctor block_split is functioning (splits the long block)");
        ck(fcmp5.blocks.size() == blocks_before5 + 1, "CMP5: default-ctor block_split adds exactly one continuation block");
    }

    // ═══ (A) no-op vs changed preservation ═══
    std::printf("\n--- (A) no-op vs changed preservation ---\n");
    for (size_t i = 0; i < kSixCount; ++i) {
        const char* pass = kSixPasses[i];
        // no-op: zero density → all_preserved + instr count unchanged.
        EmberPassRegistry nopreg; register_passes(nopreg, make_opts(0, 0));
        auto base = candidate_fixture(pass);
        size_t before = total_instrs(base);
        SingleResult r = run_pass(nopreg, pass, base);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "A1 %s: zero-density -> no-op (all_preserved)", pass);
        ck(r.all_preserved, buf);
        std::snprintf(buf, sizeof(buf), "A2 %s: zero-density leaves instr count unchanged (%zu==%zu)", pass, r.instr_count, before);
        ck(r.instr_count == before, buf);
        // changed: full density → not all_preserved (the pass selects ≥1 site).
        EmberPassRegistry fullreg; register_passes(fullreg, make_opts(0, 1'000'000));
        auto base2 = candidate_fixture(pass);
        SingleResult r2 = run_pass(fullreg, pass, base2);
        std::snprintf(buf, sizeof(buf), "A3 %s: full-density -> changed (not all_preserved)", pass);
        ck(!r2.all_preserved, buf);
    }

    // ═══ (Widths 1/2/4/8) structural no-op + changed eligibility ═══
    std::printf("\n--- (W) widths 1/2/4/8 eligibility ---\n");
    {
        Prim prims[] = {Prim::I8, Prim::I16, Prim::I32, Prim::I64};
        int32_t widths[] = {1, 2, 4, 8};
        for (size_t w = 0; w < 4; ++w) {
            // subst + mba_expand + const_encode operate on integer Add/Const.
            for (size_t pi = 0; pi < 3; ++pi) {
                const char* pass = kSixPasses[pi];
                EmberPassRegistry nopreg; register_passes(nopreg, make_opts(0, 0));
                auto f = build_straight_line(7, 9, prims[w], widths[w]);
                SingleResult r = run_pass(nopreg, pass, f);
                char buf[128];
                std::snprintf(buf, sizeof(buf), "W1 %s w=%d: zero-density no-op", pass, widths[w]);
                ck(r.all_preserved, buf);
                EmberPassRegistry fullreg; register_passes(fullreg, make_opts(0, 1'000'000));
                auto f2 = build_straight_line(7, 9, prims[w], widths[w]);
                SingleResult r2 = run_pass(fullreg, pass, f2);
                std::snprintf(buf, sizeof(buf), "W2 %s w=%d: full-density validates (structure)", pass, widths[w]);
                std::string verr;
                bool ok = verify_thin_function_for_codegen(f2, &verr);
                ck(ok, buf);
                if (!ok) std::printf("    verr: %s\n", verr.c_str());
                (void)r2;
            }
        }
    }

    // ═══ (Empty / no-candidate) every pass is a no-op ═══
    std::printf("\n--- (E) empty / no-candidate functions ---\n");
    {
        EmberPassRegistry fullreg; register_passes(fullreg, make_opts(0, 1'000'000));
        for (size_t i = 0; i < kSixCount; ++i) {
            const char* pass = kSixPasses[i];
            auto e = build_empty();
            SingleResult r = run_pass(fullreg, pass, e);
            char buf[128];
            std::snprintf(buf, sizeof(buf), "E1 %s: empty -> no-op", pass);
            ck(r.all_preserved, buf);
            auto nc = build_no_candidate();
            SingleResult r2 = run_pass(fullreg, pass, nc);
            std::snprintf(buf, sizeof(buf), "E2 %s: no-candidate -> no-op", pass);
            ck(r2.all_preserved, buf);
        }
    }

    // ═══ (CFGs) straight-line / diamond / loop / long-block validate after each pass ═══
    std::printf("\n--- (C) representative CFGs validate after each pass ---\n");
    {
        struct Cfg { const char* name; ThinFunction (*build)(); };
        // loop builder takes an arg; wrap it.
        Cfg cfgs[] = {
            {"straight-line", []() { return build_straight_line(7, 9, Prim::I64, 8); }},
            {"diamond",       []() { return build_diamond(); }},
            {"loop",           []() { return build_loop(5); }},
            {"long-block",     []() { return build_long_block(9); }},
        };
        EmberPassRegistry fullreg; register_passes(fullreg, make_opts(0, 1'000'000));
        for (auto& cfg : cfgs) {
            for (size_t i = 0; i < kSixCount; ++i) {
                const char* pass = kSixPasses[i];
                auto f = cfg.build();
                run_pass(fullreg, pass, f);
                std::string verr;
                bool ok = verify_thin_function_for_codegen(f, &verr);
                char buf[160];
                std::snprintf(buf, sizeof(buf), "C %s/%s: validates after pass", cfg.name, pass);
                ck(ok, buf);
                if (!ok) std::printf("    verr: %s\n", verr.c_str());
            }
        }
    }

    // ═══ (B) same-seed serialized Thin IR equality ═══
    std::printf("\n--- (B) same-seed serialized Thin IR equality ---\n");
    for (size_t i = 0; i < kSixCount; ++i) {
        const char* pass = kSixPasses[i];
        EmberPassRegistry r1; register_passes(r1, make_opts(42, 1'000'000));
        EmberPassRegistry r2; register_passes(r2, make_opts(42, 1'000'000));
        auto fa = candidate_fixture(pass);
        auto fb = candidate_fixture(pass);
        // Run in place so the serialized blobs reflect the transformed IR.
        EmberAnalysisManager am;
        auto p1 = r1.create(pass); if (p1) p1->run(fa, am);
        auto p2 = r2.create(pass); if (p2) p2->run(fb, am);
        std::vector<uint8_t> ba, bb; std::string ea, eb;
        bool sa = serialize_thin_function(fa, ba, &ea);
        bool sb = serialize_thin_function(fb, bb, &eb);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "B1 %s: both serialize ok", pass);
        ck(sa && sb, buf);
        std::snprintf(buf, sizeof(buf), "B2 %s: same-seed blobs byte-identical (size %zu==%zu)", pass, ba.size(), bb.size());
        ck(ba == bb, buf);
    }

    // ═══ (C2) two pinned seeds → structurally different (where ≥1 site) ═══
    // Two pinned seeds must produce structurally different serialized IR for
    // each pass that has ≥1 selected site. Uses a multi-site fixture
    // (build_two_long_blocks(9) — two long blocks, many Add/ConstInt sites) so
    // the variation is robust: the per-purpose streams (select / variant /
    // constant / truth / junk-count / split-pick) differ by seed across many
    // sites, so two different seeds produce different serialized blobs.
    //   - subst has NO variant stream (the MBA identity is unique), so it is
    //     run at 50% density (500'000 ppm) where the per-site SELECTION differs
    //     by seed (different seeds select different subsets of the many Adds).
    //   - the other five passes have variant/truth/constant/split-pick streams,
    //     so they vary by seed even at full density (1'000'000 ppm).
    std::printf("\n--- (C2) two pinned seeds structural variation ---\n");
    for (size_t i = 0; i < kSixCount; ++i) {
        const char* pass = kSixPasses[i];
        const uint32_t dens = (std::strcmp(pass, "subst") == 0) ? 500'000u : 1'000'000u;
        EmberPassRegistry ra; register_passes(ra, make_opts(1, dens));
        EmberPassRegistry rb; register_passes(rb, make_opts(999, dens));
        auto fa = build_two_long_blocks(9);
        auto fb = build_two_long_blocks(9);
        // Run in place so the serialized blobs reflect the transformed IR.
        EmberAnalysisManager am;
        auto pca = ra.create(pass); if (pca) pca->run(fa, am);
        auto pcb = rb.create(pass); if (pcb) pcb->run(fb, am);
        std::vector<uint8_t> ba, bb; std::string ea, eb;
        serialize_thin_function(fa, ba, &ea);
        serialize_thin_function(fb, bb, &eb);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "C2 %s: two seeds -> structural variation (blobs differ)", pass);
        ck(ba != bb, buf);
    }

    // ═══ (D) baseline differential execution ═══
    std::printf("\n--- (D) baseline differential execution ---\n");
    for (size_t i = 0; i < kSixCount; ++i) {
        const char* pass = kSixPasses[i];
        auto base = candidate_fixture(pass);
        int64_t rb = emit_and_call(base);
        ck(rb != INT64_MIN, "D: baseline emit/call succeeded (precondition)");
        EmberPassRegistry fullreg; register_passes(fullreg, make_opts(0, 1'000'000));
        auto f = candidate_fixture(pass);
        run_pass(fullreg, pass, f);
        std::string verr;
        ck(verify_thin_function_for_codegen(f, &verr), "D: transformed IR verifies for codegen");
        int64_t rt = emit_and_call(f);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "D %s: transformed return == baseline (%lld==%lld)",
                      pass, (long long)rt, (long long)rb);
        ck(rt == rb, buf);
    }

    // ═══ (E) validate → serialize → deserialize → validate ═══
    std::printf("\n--- (E) serialize/deserialize round-trip ---\n");
    for (size_t i = 0; i < kSixCount; ++i) {
        const char* pass = kSixPasses[i];
        EmberPassRegistry fullreg; register_passes(fullreg, make_opts(0, 1'000'000));
        auto f = candidate_fixture(pass);
        run_pass(fullreg, pass, f);
        std::string v1;
        ck(verify_thin_function_for_codegen(f, &v1), "E1: validate transformed");
        std::vector<uint8_t> blob; std::string serr;
        ck(serialize_thin_function(f, blob, &serr), "E2: serialize transformed");
        ThinFunction g; std::string derr;
        const uint8_t* cur = blob.data(); const uint8_t* end = blob.data() + blob.size();
        ck(deserialize_thin_function(cur, end, f.name, f.slot, g, &derr), "E3: deserialize");
        std::string v2;
        ck(validate_thin_function(g, &v2), "E4: validate deserialized");
        char buf[128];
        std::snprintf(buf, sizeof(buf), "E5 %s: round-trip preserves block count", pass);
        ck(g.blocks.size() == f.blocks.size(), buf);
        std::snprintf(buf, sizeof(buf), "E6 %s: round-trip preserves total instr count", pass);
        ck(total_instrs(g) == total_instrs(f), buf);
    }

    // ═══ (F) stale-regalloc invalidation ═══
    std::printf("\n--- (F) stale-regalloc invalidation ---\n");
    for (size_t i = 0; i < kSixCount; ++i) {
        const char* pass = kSixPasses[i];
        EmberPassRegistry fullreg; register_passes(fullreg, make_opts(0, 1'000'000));
        auto f = candidate_fixture(pass);
        // Seed a stale regalloc.
        f.ra.enabled = true; f.ra.num_regs = 4; f.ra.map[1] = {true, 3, -16};
        // Run the pass IN PLACE (run_pass copies f; the ra mutation must be
        // observed on this f, so drive the concept directly here).
        EmberAnalysisManager am;
        auto pc = fullreg.create(pass);
        ck(bool(pc), "F: create pass concept");
        EmberPreserved pres = pc ? pc->run(f, am) : EmberPreserved::all();
        char buf[128];
        if (pres.all_preserved()) {
            // No site was selected for this seed/fixture: regalloc stays as we
            // seeded it (the pass did not touch the function). This is valid.
            std::snprintf(buf, sizeof(buf), "F %s: no-op -> regalloc untouched (valid)", pass);
            ck(f.ra.enabled, buf);
        } else {
            std::snprintf(buf, sizeof(buf), "F %s: changed -> stale regalloc cleared (enabled=false)", pass);
            ck(!f.ra.enabled, buf);
            std::snprintf(buf, sizeof(buf), "F %s: changed -> stale regalloc map empty", pass);
            ck(f.ra.map.empty(), buf);
        }
    }

    // ═══ (G) exact boundaries + stop-before-site atomicity ═══
    std::printf("\n--- (G) growth boundaries + stop-before-site atomicity ---\n");
    {
        // G1: max_sites = 1 on a fixture with ≥2 eligible sites for subst
        //     (long_block has many Adds). At most 1 site is rewritten; the
        //     result is valid (atomic — no partial site).
        EmberPassRegistry r; PassGrowthLimits lim; lim.max_sites = 1;
        register_passes(r, make_opts(0, 1'000'000, lim));
        auto f = build_long_block(9);  // many Add sites
        size_t before = total_instrs(f);
        SingleResult res = run_pass(r, "subst", f);
        ck(!res.all_preserved, "G1a: subst with max_sites=1 still changes (>=1 site)");
        // Each Add site adds 3 instructions; with max_sites=1 the growth is
        // exactly +3 (atomic: no partial site).
        char buf[128];
        std::snprintf(buf, sizeof(buf), "G1b: max_sites=1 -> exactly +3 instrs (atomic, before=%zu after=%zu)", before, res.instr_count);
        ck(res.instr_count == before + 3, buf);
        std::string verr;
        ck(verify_thin_function_for_codegen(f, &verr), "G1c: bounded result validates");

        // G2: max_added_instructions so tight that even one site's 3 instrs
        //     would exceed it. The pass must stop BEFORE the first site:
        //     no-op, atomic (zero partial growth).
        EmberPassRegistry r2; PassGrowthLimits lim2; lim2.max_added_instructions = 2;
        register_passes(r2, make_opts(0, 1'000'000, lim2));
        auto f2 = build_straight_line(7, 9, Prim::I64, 8);
        size_t before2 = total_instrs(f2);
        SingleResult res2 = run_pass(r2, "subst", f2);
        ck(res2.all_preserved, "G2a: subst with max_added_instructions=2 -> no-op (stop-before-site)");
        std::snprintf(buf, sizeof(buf), "G2b: stop-before-site atomic (before=%zu after=%zu)", before2, res2.instr_count);
        ck(res2.instr_count == before2, buf);

        // G3: growth ratio boundary. growth_numerator=1, denominator=1 → the
        //     final count may equal but not exceed the initial count. subst on
        //     a single-Add fixture wants +3 (ratio > 1), so it must stop before
        //     the site: no-op.
        EmberPassRegistry r3; PassGrowthLimits lim3;
        lim3.growth_numerator = 1; lim3.growth_denominator = 1;  // ratio 1.0
        register_passes(r3, make_opts(0, 1'000'000, lim3));
        auto f3 = build_straight_line(7, 9, Prim::I64, 8);
        SingleResult res3 = run_pass(r3, "subst", f3);
        ck(res3.all_preserved, "G3a: growth ratio 1.0 -> subst no-op (would exceed)");

        // G4: growth ratio exactly 2.0 admits the single-Add +3 (initial=3 →
        //     cap=6; +3 → 6 <= 6). Changed + atomic.
        EmberPassRegistry r4; PassGrowthLimits lim4;
        lim4.growth_numerator = 2; lim4.growth_denominator = 1;  // ratio 2.0
        register_passes(r4, make_opts(0, 1'000'000, lim4));
        auto f4 = build_straight_line(7, 9, Prim::I64, 8);
        size_t before4 = total_instrs(f4);
        SingleResult res4 = run_pass(r4, "subst", f4);
        ck(!res4.all_preserved, "G4a: growth ratio 2.0 -> subst changed");
        std::snprintf(buf, sizeof(buf), "G4b: ratio 2.0 -> exactly +3 (before=%zu after=%zu)", before4, res4.instr_count);
        ck(res4.instr_count == before4 + 3, buf);

        // G5: max_added_blocks = 0 forbids any block split. block_split on a
        //     long block must stop before the site: no-op, atomic.
        EmberPassRegistry r5; PassGrowthLimits lim5; lim5.max_added_blocks = 0;
        register_passes(r5, make_opts(0, 1'000'000, lim5));
        auto f5 = build_long_block(9);
        size_t before5 = f5.blocks.size();
        EmberAnalysisManager am5; auto pc5 = r5.create("block_split");
        EmberPreserved pres5 = pc5->run(f5, am5);
        ck(pres5.all_preserved(), "G5a: max_added_blocks=0 -> block_split no-op");
        ck(f5.blocks.size() == before5, "G5b: block count unchanged (atomic)");

        // G6: max_added_blocks = 1 admits exactly one split on a long block.
        EmberPassRegistry r6; PassGrowthLimits lim6; lim6.max_added_blocks = 1;
        register_passes(r6, make_opts(0, 1'000'000, lim6));
        auto f6 = build_long_block(9);
        size_t before6 = f6.blocks.size();
        EmberAnalysisManager am6; auto pc6 = r6.create("block_split");
        EmberPreserved pres6 = pc6->run(f6, am6);
        ck(!pres6.all_preserved(), "G6a: max_added_blocks=1 -> block_split changed");
        std::snprintf(buf, sizeof(buf), "G6b: exactly +1 block (before=%zu after=%zu)", before6, f6.blocks.size());
        ck(f6.blocks.size() == before6 + 1, buf);
    }

    // ═══ (G7/G8) max_added_vregs + max_added_frame_bytes boundaries + stop-before-site atomicity ═══
    // Red 6 feedback: exact admit/reject and stop-before-site atomicity for
    // the max_added_vregs AND max_added_frame_bytes ceilings. subst's per-site
    // worst case is 3 VRegs + 24 frame bytes (3 x 8-byte spill slots) + 3 instrs.
    std::printf("\n--- (G7/G8) max_added_vregs + max_added_frame_bytes boundaries ---\n");
    {
        // G7a: max_added_vregs = 2 (< 3 needed) -> subst stops before the first
        //      site: no-op, atomic (no partial VReg allocation; the function is
        //      unchanged).
        EmberPassRegistry r7a; PassGrowthLimits lim7a; lim7a.max_added_vregs = 2;
        register_passes(r7a, make_opts(0, 1'000'000, lim7a));
        auto f7a = build_straight_line(7, 9, Prim::I64, 8);  // one eligible Add
        size_t before7a = total_instrs(f7a);
        uint32_t maxvreg_before7a = f7a.declared_max_vreg;
        SingleResult res7a = run_pass(r7a, "subst", f7a);
        ck(res7a.all_preserved, "G7a: max_added_vregs=2 -> subst no-op (stop-before-site)");
        ck(res7a.instr_count == before7a, "G7a: instr count unchanged (atomic)");
        ck(f7a.declared_max_vreg == maxvreg_before7a, "G7a: declared_max_vreg unchanged (atomic, no partial VReg)");
        std::string verr7a; ck(verify_thin_function_for_codegen(f7a, &verr7a), "G7a: bounded result validates");

        // G7b: max_added_vregs = 3 (== 3 needed) -> subst admits the single
        //      site: changed, exactly +3 VRegs, atomic.
        EmberPassRegistry r7b; PassGrowthLimits lim7b; lim7b.max_added_vregs = 3;
        register_passes(r7b, make_opts(0, 1'000'000, lim7b));
        auto f7b = build_straight_line(7, 9, Prim::I64, 8);
        size_t before7b = total_instrs(f7b);
        uint32_t maxvreg_before7b = f7b.declared_max_vreg;
        SingleResult res7b = run_pass(r7b, "subst", f7b);
        ck(!res7b.all_preserved, "G7b: max_added_vregs=3 -> subst changed (admit)");
        ck(res7b.instr_count == before7b + 3, "G7b: exactly +3 instrs");
        ck(f7b.declared_max_vreg == maxvreg_before7b + 3, "G7b: exactly +3 VRegs (atomic)");

        // G7c: max_added_vregs = 4 (> 3 needed) -> still admits (exactly +3;
        //      the extra budget is unused, not over-allocated).
        EmberPassRegistry r7c; PassGrowthLimits lim7c; lim7c.max_added_vregs = 4;
        register_passes(r7c, make_opts(0, 1'000'000, lim7c));
        auto f7c = build_straight_line(7, 9, Prim::I64, 8);
        uint32_t maxvreg_before7c = f7c.declared_max_vreg;
        SingleResult res7c = run_pass(r7c, "subst", f7c);
        ck(!res7c.all_preserved, "G7c: max_added_vregs=4 -> subst changed");
        ck(f7c.declared_max_vreg == maxvreg_before7c + 3, "G7c: exactly +3 VRegs (no over-allocation)");

        // G8a: max_added_frame_bytes = 16 (< 24 needed) -> subst stops before the
        //      first site: no-op, atomic (no partial frame allocation; the
        //      frame plan is unchanged).
        EmberPassRegistry r8a; PassGrowthLimits lim8a; lim8a.max_added_frame_bytes = 16;
        register_passes(r8a, make_opts(0, 1'000'000, lim8a));
        auto f8a = build_straight_line(7, 9, Prim::I64, 8);
        size_t before8a = total_instrs(f8a);
        int32_t next_off_before8a = f8a.frame.next_local_off;
        int32_t frame_size_before8a = f8a.frame.frame_size;
        SingleResult res8a = run_pass(r8a, "subst", f8a);
        ck(res8a.all_preserved, "G8a: max_added_frame_bytes=16 -> subst no-op (stop-before-site)");
        ck(res8a.instr_count == before8a, "G8a: instr count unchanged (atomic)");
        ck(f8a.frame.next_local_off == next_off_before8a, "G8a: next_local_off unchanged (atomic, no partial frame)");
        ck(f8a.frame.frame_size == frame_size_before8a, "G8a: frame_size unchanged (atomic)");

        // G8b: max_added_frame_bytes = 24 (== 24 needed) -> subst admits the
        //      single site: changed, exactly +24 frame bytes, atomic.
        EmberPassRegistry r8b; PassGrowthLimits lim8b; lim8b.max_added_frame_bytes = 24;
        register_passes(r8b, make_opts(0, 1'000'000, lim8b));
        auto f8b = build_straight_line(7, 9, Prim::I64, 8);
        int32_t next_off_before8b = f8b.frame.next_local_off;
        SingleResult res8b = run_pass(r8b, "subst", f8b);
        ck(!res8b.all_preserved, "G8b: max_added_frame_bytes=24 -> subst changed (admit)");
        ck(f8b.frame.next_local_off == next_off_before8b + 24, "G8b: exactly +24 frame bytes (atomic)");
    }

    // ═══ (str_encrypt) configured factory + no-op scaffolding ═══
    std::printf("\n--- (S) str_encrypt configured factory + no-op scaffolding ---\n");
    {
        // S1: registered via configured factory (already checked in R1, but
        //     restate explicitly for the deferred pass).
        ck(reg.has("str_encrypt"), "S1: str_encrypt registered via configured factory");
        // S2: zero density -> no-op.
        EmberPassRegistry nopreg; register_passes(nopreg, make_opts(0, 0));
        // A function with no ConstStringRef -> no-op regardless of density.
        auto f = build_straight_line(7, 9, Prim::I64, 8);
        SingleResult r = run_pass(nopreg, "str_encrypt", f);
        ck(r.all_preserved, "S2: str_encrypt no ConstStringRef -> no-op");
        // S3: full density on a no-stringref function -> still no-op.
        EmberPassRegistry fullreg; register_passes(fullreg, make_opts(0, 1'000'000));
        auto f2 = build_straight_line(7, 9, Prim::I64, 8);
        SingleResult r2 = run_pass(fullreg, "str_encrypt", f2);
        ck(r2.all_preserved, "S3: str_encrypt no candidates -> no-op (full density)");
    }

    // ═══ (SE) str_encrypt full matrix ═══
    // The full str_encrypt coverage per the Red 7 spec: configured factory
    // freshness, same-seed byte-identical serialized IR, pinned-seed key/
    // structure variation, nonzero per-site keys, plaintext absence from
    // final rodata, overlapping/repeated/empty literal ranges, distinct
    // nonoverlapping data and slice frame regions, growth boundaries,
    // validation, round trip, execution, and no double encryption unless an
    // explicit rekey mode exists.
    std::printf("\n--- (SE) str_encrypt full matrix ---\n");
    {
        // Helper: build a string-returning function from a plaintext list.
        // Each entry becomes a ConstStringRef in rodata. The function returns
        // the total byte length of all literals (so differential execution
        // checks that str_encrypt preserves lengths). The ConstStringRef
        // slice result is frame-backed; the function loads the len word and
        // adds it to a running sum, returning the sum.
        //
        // layout: for each literal i:
        //   v_{2i+1} = ConstStringRef(ptr_i, len_i)  -- slice {ptr, len}
        //   v_{2i+2} = ConstInt(len_i)               -- redundant len
        //   ... actually we need the len word from the slice. The ConstStringRef
        //   emit puts ptr in rax, len in rdx, and frame-backs both at frame_off.
        //   The len word is at frame_off+8. We can LoadFrame it.
        //
        // Simpler: each ConstStringRef yields a slice in v_n (ptr) / v_n+1 (len).
        // We accumulate: sum = sum + v_{2i+1}_len. But the len is v_{2i+1}+1,
        // which is the implicit pair word. We Add that to the accumulator.
        auto build_str_fn = [](const std::vector<std::string>& lits) {
            ThinFunction f;
            f.name = "strenc";
            f.slot = 0;
            const Type* i64 = scalar_type(f, Prim::I64);
            // Slice type for ConstStringRef (is_slice, Prim::Void, elem U8).
            auto slice_ty = std::make_shared<Type>();
            slice_ty->is_slice = true;
            slice_ty->prim = Prim::Void;
            auto u8_elem = std::make_shared<Type>();
            u8_elem->prim = Prim::U8;
            slice_ty->elem = u8_elem;
            install_i64(f);
            minimal_frame(f, 16);
            // rodata: concatenate all literals.
            uint32_t off = 0;
            std::vector<uint32_t> addends;
            for (const auto& s : lits) {
                addends.push_back(off);
                f.rodata.insert(f.rodata.end(), s.begin(), s.end());
                off += uint32_t(s.size());
            }
            ThinBlock b0; b0.id = 0;
            // v1 = 0 (accumulator)
            int32_t cur_off = 16;
            ThinInstr acc0; acc0.op = ThinOp::ConstInt; acc0.dst = 1; acc0.imm.i = 0;
            acc0.meta.width = 8; acc0.meta.type = i64; acc0.meta.frame_off = -cur_off;
            b0.instrs.push_back(acc0); cur_off += 8;
            VReg acc = 1;
            VReg next_v = 2;
            for (size_t i = 0; i < lits.size(); ++i) {
                // ConstStringRef: dst = next_v (ptr), implicit next_v+1 (len)
                const int32_t slice_slot = -(cur_off + 16);
                cur_off += 16;
                ThinInstr sr; sr.op = ThinOp::ConstStringRef; sr.dst = next_v;
                sr.meta.addend = addends[i];
                sr.meta.len = int32_t(lits[i].size());
                sr.meta.base_kind = AbsFixup::FunctionRodataBase;
                sr.meta.type = slice_ty.get();
                sr.meta.width = 8;
                sr.meta.frame_off = slice_slot;  // frame-back the slice
                b0.instrs.push_back(sr);
                // Add the len word (next_v + 1) to the accumulator.
                ThinInstr add; add.op = ThinOp::Add; add.dst = next_v + 2;
                add.src1 = acc; add.src2 = next_v + 1;
                add.meta.width = 8; add.meta.type = i64;
                add.meta.frame_off = -cur_off; cur_off += 8;
                b0.instrs.push_back(add);
                acc = next_v + 2;
                next_v += 3;
            }
            b0.term.kind = TermKind::Return; b0.term.ret = acc;
            f.blocks.push_back(std::move(b0));
            f.declared_max_vreg = next_v;
            f.frame.next_local_off = cur_off;
            f.frame.frame_size = (cur_off + 15) & ~15;
            f.owned_types.push_back(std::move(u8_elem));
            f.owned_types.push_back(std::move(slice_ty));
            return f;
        };

        // Compute expected total length for a literal list.
        auto total_len = [](const std::vector<std::string>& lits) -> int64_t {
            int64_t t = 0;
            for (const auto& s : lits) t += int64_t(s.size());
            return t;
        };

        // Build a string-returning function from EXPLICIT (addend, len) specs
        // into a caller-supplied rodata blob. Unlike build_str_fn (which
        // concatenates distinct literals into non-overlapping ranges), this
        // lets the test hand-build REAL overlaps: repeated identical ranges
        // (two specs with the same addend+same len), and partial overlaps
        // (one spec's range starts inside another's). Each spec becomes a
        // ConstStringRef frame-backed slice; the function returns the sum of
        // all spec lens (so differential execution checks value-preservation
        // across the encrypt). The rodata is set VERBATIM from `bytes` (the
        // test controls exactly which bytes each spec references, so it can
        // record each site's plaintext and verify the encrypted output).
        struct Spec { uint32_t addend; int32_t len; };
        auto build_overlap_fn = [](const std::vector<uint8_t>& bytes,
                                   const std::vector<Spec>& specs) {
            ThinFunction f;
            f.name = "strenc_overlap";
            f.slot = 0;
            const Type* i64 = scalar_type(f, Prim::I64);
            auto slice_ty = std::make_shared<Type>();
            slice_ty->is_slice = true;
            slice_ty->prim = Prim::Void;
            auto u8_elem = std::make_shared<Type>();
            u8_elem->prim = Prim::U8;
            slice_ty->elem = u8_elem;
            install_i64(f);
            minimal_frame(f, 16);
            f.rodata = bytes;
            ThinBlock b0; b0.id = 0;
            int32_t cur_off = 16;
            ThinInstr acc0; acc0.op = ThinOp::ConstInt; acc0.dst = 1; acc0.imm.i = 0;
            acc0.meta.width = 8; acc0.meta.type = i64; acc0.meta.frame_off = -cur_off;
            b0.instrs.push_back(acc0); cur_off += 8;
            VReg acc = 1;
            VReg next_v = 2;
            for (const Spec& sp : specs) {
                // Each ConstStringRef slice must be frame-backed at a 16-byte
                // slot whose len word (frame_off+8) does NOT collide with the
                // rbx save slot at -8. Slice slots start at -(cur_off+16) and
                // advance by 16, so the first slice is at -32 (len@-24) and rbx
                // is at -8 — never colliding.
                const int32_t slice_slot = -(cur_off + 16);
                cur_off += 16;
                ThinInstr sr; sr.op = ThinOp::ConstStringRef; sr.dst = next_v;
                sr.meta.addend = sp.addend;
                sr.meta.len = sp.len;
                sr.meta.base_kind = AbsFixup::FunctionRodataBase;
                sr.meta.type = slice_ty.get();
                sr.meta.width = 8;
                sr.meta.frame_off = slice_slot;
                b0.instrs.push_back(sr);
                ThinInstr add; add.op = ThinOp::Add; add.dst = next_v + 2;
                add.src1 = acc; add.src2 = next_v + 1;
                add.meta.width = 8; add.meta.type = i64;
                add.meta.frame_off = -cur_off; cur_off += 8;
                b0.instrs.push_back(add);
                acc = next_v + 2;
                next_v += 3;
            }
            b0.term.kind = TermKind::Return; b0.term.ret = acc;
            f.blocks.push_back(std::move(b0));
            f.declared_max_vreg = next_v;
            f.frame.next_local_off = cur_off;
            f.frame.frame_size = (cur_off + 15) & ~15;
            f.owned_types.push_back(std::move(u8_elem));
            f.owned_types.push_back(std::move(slice_ty));
            return f;
        };

        // Sum of spec lens (the expected return for build_overlap_fn).
        auto total_spec_len = [](const std::vector<Spec>& specs) -> int64_t {
            int64_t t = 0;
            for (const Spec& s : specs) t += int64_t(s.len);
            return t;
        };

        // SE-A: configured factory freshness — two create() calls yield
        // distinct fresh instances (already checked in R3 for subst, restate
        // for str_encrypt).
        {
            EmberPassRegistry r; register_passes(r, make_opts(0, 1'000'000));
            auto a = r.create("str_encrypt");
            auto b = r.create("str_encrypt");
            ck(bool(a) && bool(b), "SE-A1: str_encrypt create yields two non-null concepts");
            ck(a.get() != b.get(), "SE-A2: str_encrypt concepts are distinct instances");
        }

        // SE-B: same-seed byte-identical serialized IR.
        {
            EmberPassRegistry r1; register_passes(r1, make_opts(42, 1'000'000));
            EmberPassRegistry r2; register_passes(r2, make_opts(42, 1'000'000));
            auto lits = std::vector<std::string>{"hello", "world", "abc"};
            auto fa = build_str_fn(lits);
            auto fb = build_str_fn(lits);
            EmberAnalysisManager am;
            auto p1 = r1.create("str_encrypt"); if (p1) p1->run(fa, am);
            auto p2 = r2.create("str_encrypt"); if (p2) p2->run(fb, am);
            std::vector<uint8_t> ba, bb; std::string ea, eb;
            bool sa = serialize_thin_function(fa, ba, &ea);
            bool sb = serialize_thin_function(fb, bb, &eb);
            ck(sa && sb, "SE-B1: str_encrypt both serialize ok");
            ck(ba == bb, "SE-B2: str_encrypt same-seed blobs byte-identical");
        }

        // SE-C: pinned-seed key/structure variation — two different seeds
        // produce different serialized IR (the per-site key derives from
        // the seed, so the encrypted rodata bytes differ).
        {
            EmberPassRegistry ra; register_passes(ra, make_opts(1, 1'000'000));
            EmberPassRegistry rb; register_passes(rb, make_opts(999, 1'000'000));
            auto lits = std::vector<std::string>{"hello", "world", "abcdef"};
            auto fa = build_str_fn(lits);
            auto fb = build_str_fn(lits);
            EmberAnalysisManager am;
            auto pca = ra.create("str_encrypt"); if (pca) pca->run(fa, am);
            auto pcb = rb.create("str_encrypt"); if (pcb) pcb->run(fb, am);
            std::vector<uint8_t> ba, bb; std::string ea, eb;
            serialize_thin_function(fa, ba, &ea);
            serialize_thin_function(fb, bb, &eb);
            ck(ba != bb, "SE-C: str_encrypt two seeds -> different blobs (key/structure variation)");
        }

        // SE-D: nonzero per-site keys — the derived key for each site must
        // be nonzero (a zero key would leave plaintext in rodata). Verify by
        // checking that the rodata changed (XOR with a nonzero key always
        // changes at least one byte of a non-empty literal).
        {
            auto lits = std::vector<std::string>{"hello", "world"};
            auto f = build_str_fn(lits);
            std::vector<uint8_t> rodata_before = f.rodata;
            EmberPassRegistry r; register_passes(r, make_opts(0, 1'000'000));
            EmberAnalysisManager am;
            auto p = r.create("str_encrypt"); if (p) p->run(f, am);
            // The rodata should have changed (encrypted with a nonzero key).
            bool changed = f.rodata != rodata_before;
            ck(changed, "SE-D1: str_encrypt changes rodata (nonzero key)");
            // Verify no selected plaintext byte survives in the rodata for
            // the literal ranges (plaintext absence).
            bool plaintext_absent = true;
            for (size_t i = 0; i < lits.size() && plaintext_absent; ++i) {
                uint32_t begin = 0;
                for (size_t j = 0; j < i; ++j) begin += uint32_t(lits[j].size());
                uint32_t end = begin + uint32_t(lits[i].size());
                for (uint32_t k = begin; k < end; ++k) {
                    if (k < f.rodata.size() && f.rodata[k] == uint8_t(lits[i][k - begin])) {
                        // A matching byte is only OK if the key byte at this
                        // position is zero — but we require nonzero keys, so
                        // a matching plaintext byte means the key was zero
                        // for that byte. Check if ANY byte matches (plaintext
                        // leak). For a byte-wise XOR with a single-byte key,
                        // a match means key == 0 for that position.
                        plaintext_absent = false;
                        break;
                    }
                }
            }
            // Note: str_encrypt uses a per-site byte key (single byte). A
            // plaintext byte b encrypted with key k gives b^k. b^k == b iff
            // k == 0. With nonzero keys, NO plaintext byte should survive.
            // (If the key is the same byte for the whole literal, a byte
            // equal to 0 would survive as 0^k = k, but k != 0, so 0 does not
            // survive either — unless the plaintext byte equals the key,
            // giving 0. So we check that the rodata does NOT contain the
            // plaintext at the literal offset.)
            // A more robust check: the rodata at each literal range is NOT
            // equal to the plaintext.
            bool rodata_is_plaintext = true;
            for (size_t i = 0; i < lits.size(); ++i) {
                uint32_t begin = 0;
                for (size_t j = 0; j < i; ++j) begin += uint32_t(lits[j].size());
                uint32_t end = begin + uint32_t(lits[i].size());
                for (uint32_t k = begin; k < end; ++k) {
                    if (k < f.rodata.size() && f.rodata[k] != uint8_t(lits[i][k - begin])) {
                        rodata_is_plaintext = false;
                        break;
                    }
                }
            }
            ck(!rodata_is_plaintext, "SE-D2: str_encrypt rodata is not plaintext (plaintext absent)");
            ck(plaintext_absent, "SE-D3: str_encrypt no plaintext byte survives in rodata (per-site nonzero key)");
        }

        // SE-E: overlapping/repeated/empty literal ranges — the pass must
        // handle REAL repeated references (two ConstStringRef pointing to the
        // SAME addend+len — not two separate concatenated copies), REAL partial
        // overlaps (one spec's range starts inside another's), and empty
        // literals (len=0). build_str_fn concatenates distinct literals into
        // non-overlapping ranges, so it CANNOT exercise these shapes; the
        // hand-built build_overlap_fn sets explicit (addend,len) specs into a
        // verbatim rodata so the overlap is genuine.
        {
            // rodata: "abcdef" (6 bytes). Specs:
            //   spec0 = (0, 3) -> "abc"
            //   spec1 = (0, 3) -> "abc"  (REPEATED IDENTICAL range — same bytes)
            //   spec2 = (2, 4) -> "cdef"  (PARTIAL OVERLAP with spec0/spec1: bytes [2,3) shared)
            //   spec3 = (6, 0) -> ""  (empty, len=0)
            std::vector<uint8_t> rodata = {'a','b','c','d','e','f'};
            std::vector<Spec> specs = {{0,3},{0,3},{2,4},{6,0}};
            auto f = build_overlap_fn(rodata, specs);
            int64_t expected = total_spec_len(specs);  // 3+3+4+0 = 10
            int64_t rb = emit_and_call(f);
            ck(rb == expected, "SE-E1: baseline overlap fn returns correct total length");
            // Record the original plaintext of each non-empty spec so we can
            // verify plaintext absence after the pass.
            std::vector<std::vector<uint8_t>> plains;
            for (const Spec& s : specs) {
                plains.emplace_back(f.rodata.begin() + s.addend,
                                    f.rodata.begin() + s.addend + s.len);
            }
            EmberPassRegistry r; register_passes(r, make_opts(0, 1'000'000));
            EmberAnalysisManager am;
            auto p = r.create("str_encrypt"); if (p) p->run(f, am);
            std::string verr;
            ck(verify_thin_function_for_codegen(f, &verr), "SE-E2: str_encrypt validates (repeated/partial-overlap/empty)");
            int64_t rt = emit_and_call(f);
            char buf[128];
            std::snprintf(buf, sizeof(buf), "SE-E3: str_encrypt preserves total length (%lld==%lld)",
                          (long long)rt, (long long)expected);
            ck(rt == expected, buf);
            // Plaintext absence: no non-empty spec's plaintext sequence may
            // survive anywhere in the final rodata (the pass must not leave
            // the original plaintext at the shared/overlapping offsets — a
            // genuine bug for repeated/overlapping ranges under the old
            // in-place XOR scheme).
            bool plaintext_survives = false;
            for (size_t i = 0; i < plains.size() && !plaintext_survives; ++i) {
                if (plains[i].empty()) continue;
                for (size_t k = 0; k + plains[i].size() <= f.rodata.size(); ++k) {
                    if (std::memcmp(f.rodata.data() + k, plains[i].data(),
                                    plains[i].size()) == 0) {
                        plaintext_survives = true; break;
                    }
                }
            }
            ck(!plaintext_survives, "SE-E4: str_encrypt plaintext absent from rodata (repeated/partial-overlap)");
        }

        // SE-F: distinct nonoverlapping data and slice frame regions —
        // each StringDecrypt must have data_temp_off != frame_off (the data
        // buffer and the slice result slot must not overlap).
        {
            auto lits = std::vector<std::string>{"hello", "world"};
            auto f = build_str_fn(lits);
            EmberPassRegistry r; register_passes(r, make_opts(0, 1'000'000));
            EmberAnalysisManager am;
            auto p = r.create("str_encrypt"); if (p) p->run(f, am);
            bool distinct = true;
            int strdec_count = 0;
            for (const auto& blk : f.blocks) {
                for (const auto& in : blk.instrs) {
                    if (in.op == ThinOp::StringDecrypt) {
                        ++strdec_count;
                        if (in.meta.data_temp_off == in.meta.frame_off)
                            distinct = false;
                        // The data region [data_temp_off, data_temp_off+len)
                        // must not overlap the slice region [frame_off, frame_off+16).
                        int64_t db = in.meta.data_temp_off, ds = in.meta.len;
                        int64_t sb = in.meta.frame_off, ss = 16;
                        bool overlap = (db < sb + ss) && (sb < db + ds);
                        if (overlap) distinct = false;
                    }
                }
            }
            ck(strdec_count > 0, "SE-F1: str_encrypt produced StringDecrypt instructions");
            ck(distinct, "SE-F2: str_encrypt data and slice frame regions are distinct/nonoverlapping");
        }

        // SE-G: growth boundaries — str_encrypt adds frame bytes (data +
        // slice slots per site). With max_added_frame_bytes = 0, the pass
        // must stop before any site (no-op, atomic).
        {
            EmberPassRegistry r; PassGrowthLimits lim; lim.max_added_frame_bytes = 0;
            register_passes(r, make_opts(0, 1'000'000, lim));
            auto lits = std::vector<std::string>{"hello", "world"};
            auto f = build_str_fn(lits);
            size_t rodata_before = f.rodata.size();
            EmberAnalysisManager am; auto pc = r.create("str_encrypt");
            EmberPreserved pres = pc ? pc->run(f, am) : EmberPreserved::all();
            (void)pres;
            // With zero frame budget, str_encrypt cannot allocate the data/
            // slice slots, so it must be a no-op (atomic — no partial site).
            // The rodata must be unchanged (no partial encryption).
            ck(f.rodata.size() == rodata_before, "SE-G1: str_encrypt max_added_frame_bytes=0 -> rodata unchanged (atomic)");
        }

        // SE-H: validation, round trip, execution.
        {
            auto lits = std::vector<std::string>{"hello", "world", "abcdef"};
            auto f = build_str_fn(lits);
            int64_t expected = total_len(lits);
            EmberPassRegistry r; register_passes(r, make_opts(0, 1'000'000));
            EmberAnalysisManager am;
            auto p = r.create("str_encrypt"); if (p) p->run(f, am);
            std::string v1;
            ck(verify_thin_function_for_codegen(f, &v1), "SE-H1: str_encrypt validates for codegen");
            std::vector<uint8_t> blob; std::string serr;
            ck(serialize_thin_function(f, blob, &serr), "SE-H2: str_encrypt serialize ok");
            // Plaintext absence from the serialized v2 blob: scan the blob
            // bytes for each literal's plaintext. The rodata is encrypted with
            // a nonzero key, so no plaintext byte sequence should appear.
            bool plaintext_in_blob = false;
            for (const auto& lit : lits) {
                if (lit.empty()) continue;
                for (size_t i = 0; i + lit.size() <= blob.size(); ++i) {
                    if (std::memcmp(blob.data() + i, lit.data(), lit.size()) == 0) {
                        plaintext_in_blob = true;
                        break;
                    }
                }
                if (plaintext_in_blob) break;
            }
            ck(!plaintext_in_blob, "SE-H2b: plaintext absent from serialized v2 blob");
            ThinFunction g; std::string derr;
            const uint8_t* cur = blob.data(); const uint8_t* end = blob.data() + blob.size();
            ck(deserialize_thin_function(cur, end, f.name, f.slot, g, &derr), "SE-H3: str_encrypt deserialize ok");
            std::string v2;
            ck(validate_thin_function(g, &v2), "SE-H4: str_encrypt deserialized validates");
            ck(total_instrs(g) == total_instrs(f), "SE-H5: str_encrypt round-trip preserves instr count");
            // Execution of the deserialized IR.
            int64_t rt = emit_and_call(g);
            char buf[128];
            std::snprintf(buf, sizeof(buf), "SE-H6: str_encrypt deserialized execution (%lld==%lld)",
                          (long long)rt, (long long)expected);
            ck(rt == expected, buf);
        }

        // SE-I: no double encryption unless an explicit rekey mode exists.
        // Running str_encrypt twice on the same function must NOT double-
        // encrypt (the second run sees StringDecrypt, not ConstStringRef, so
        // there are no candidates -> no-op). The rodata must be unchanged
        // after the second run.
        {
            auto lits = std::vector<std::string>{"hello", "world"};
            auto f = build_str_fn(lits);
            EmberPassRegistry r; register_passes(r, make_opts(0, 1'000'000));
            EmberAnalysisManager am;
            auto p1 = r.create("str_encrypt"); if (p1) p1->run(f, am);
            std::vector<uint8_t> rodata_after_first = f.rodata;
            int strdec_after_first = 0;
            for (const auto& blk : f.blocks)
                for (const auto& in : blk.instrs)
                    if (in.op == ThinOp::StringDecrypt) ++strdec_after_first;
            ck(strdec_after_first > 0, "SE-I1: first str_encrypt run produces StringDecrypt");
            // Second run: should be a no-op (no ConstStringRef candidates).
            auto p2 = r.create("str_encrypt");
            EmberPreserved pres2 = p2 ? p2->run(f, am) : EmberPreserved::all();
            ck(pres2.all_preserved(), "SE-I2: second str_encrypt run is a no-op (no double encryption)");
            ck(f.rodata == rodata_after_first, "SE-I3: rodata unchanged after second run (no double encryption)");
        }

        // SE-J: hand-built exact/repeated/partial-overlap matrix that
        // EXECUTES and inspects EVERY StringDecrypt key. For each overlap
        // shape the pass must give every selected site a PRIVATE non-
        // overlapping encrypted rodata region (no two StringDecrypt share
        // bytes — so same-key overlaps never double-XOR/cancel and different-
        // key overlaps never cross-corrupt), a nonzero per-site key, and
        // distinct non-overlapping data_temp_off / frame_off frame regions.
        // The encrypted region must equal plaintext XOR key (so the runtime
        // XOR decrypt restores the original bytes), the original plaintext
        // must be scrubbed from the old offsets, and execution must return the
        // expected total length (value-preservation through the decrypt).
        //
        // Three shapes, each with full per-site inspection:
        //   J1 exact/repeated:   two specs (0,4) into "abcd" — identical range.
        //   J2 partial overlap:  spec0 (0,4) "abcd", spec1 (2,4) "cdef" share [2,4).
        //   J3 disjoint control: spec0 (0,2) "ab", spec1 (2,2) "cd" — no overlap.
        {
            auto run_overlap = [&](const char* tag,
                                   const std::vector<uint8_t>& rodata_in,
                                   const std::vector<Spec>& specs) {
                auto f = build_overlap_fn(rodata_in, specs);
                int64_t expected = total_spec_len(specs);
                int64_t rb = emit_and_call(f);
                char b0[160]; std::snprintf(b0, sizeof(b0),
                    "%s-J1: baseline overlap fn returns correct total length", tag);
                ck(rb == expected, b0);
                // Record each non-empty spec's plaintext + original addend.
                std::vector<std::vector<uint8_t>> plains;
                std::vector<uint32_t> orig_addends;
                for (const Spec& s : specs) {
                    plains.emplace_back(f.rodata.begin() + s.addend,
                                        f.rodata.begin() + s.addend + s.len);
                    orig_addends.push_back(s.addend);
                }
                EmberPassRegistry r; register_passes(r, make_opts(0, 1'000'000));
                EmberAnalysisManager am;
                auto p = r.create("str_encrypt"); if (p) p->run(f, am);
                std::string verr;
                char b2[160]; std::snprintf(b2, sizeof(b2),
                    "%s-J2: str_encrypt validates for codegen", tag);
                ck(verify_thin_function_for_codegen(f, &verr), b2);
                // Collect the StringDecrypt instructions (skip empty specs —
                // len==0 produces a StringDecrypt too, but it has no rodata
                // bytes to inspect; track it for the region-overlap check).
                struct SD { uint8_t key; uint32_t addend; int32_t len;
                           int32_t data_off; int32_t slice_off; };
                std::vector<SD> sds;
                for (const auto& blk : f.blocks)
                    for (const auto& in : blk.instrs)
                        if (in.op == ThinOp::StringDecrypt)
                            sds.push_back({uint8_t(in.imm.i), in.meta.addend,
                                           in.meta.len, in.meta.data_temp_off,
                                           in.meta.frame_off});
                char b3[160]; std::snprintf(b3, sizeof(b3),
                    "%s-J3: produced a StringDecrypt per spec", tag);
                ck(sds.size() == specs.size(), b3);
                // Every per-site key must be nonzero (plaintext-absence gate).
                bool nonzero_keys = true;
                for (const SD& s : sds) if (s.key == 0) nonzero_keys = false;
                char b4[160]; std::snprintf(b4, sizeof(b4),
                    "%s-J4: every StringDecrypt key is nonzero", tag);
                ck(nonzero_keys, b4);
                // Every selected site gets a PRIVATE non-overlapping encrypted
                // rodata region: no two StringDecrypt addend ranges (of len>0)
                // share a byte. This is the core correctness rule: private
                // regions eliminate same-key double-XOR cancellation AND
                // different-key cross-corruption.
                bool private_regions = true;
                for (size_t i = 0; i < sds.size(); ++i) {
                    if (sds[i].len <= 0) continue;
                    for (size_t j = i + 1; j < sds.size(); ++j) {
                        if (sds[j].len <= 0) continue;
                        uint64_t ib = sds[i].addend, ie = ib + uint32_t(sds[i].len);
                        uint64_t jb = sds[j].addend, je = jb + uint32_t(sds[j].len);
                        if (ib < je && jb < ie) private_regions = false;
                    }
                }
                char b5[160]; std::snprintf(b5, sizeof(b5),
                    "%s-J5: encrypted rodata regions are private/non-overlapping", tag);
                ck(private_regions, b5);
                // Each non-empty encrypted region equals plaintext XOR key
                // (so the runtime XOR decrypt restores the original bytes).
                // This inspects EVERY StringDecrypt key against its bytes.
                bool enc_ok = true;
                for (size_t i = 0; i < sds.size() && enc_ok; ++i) {
                    if (sds[i].len <= 0) continue;
                    for (int32_t k = 0; k < sds[i].len; ++k) {
                        uint8_t got = f.rodata[sds[i].addend + uint32_t(k)];
                        uint8_t want = uint8_t(plains[i][size_t(k)] ^ sds[i].key);
                        if (got != want) { enc_ok = false; break; }
                    }
                }
                char b6[160]; std::snprintf(b6, sizeof(b6),
                    "%s-J6: each encrypted region == plaintext XOR its key", tag);
                ck(enc_ok, b6);
                // Original plaintext scrubbed: no non-empty spec's plaintext
                // sequence survives anywhere in the final rodata.
                bool plaintext_survives = false;
                for (size_t i = 0; i < plains.size() && !plaintext_survives; ++i) {
                    if (plains[i].empty()) continue;
                    for (size_t k = 0; k + plains[i].size() <= f.rodata.size(); ++k)
                        if (std::memcmp(f.rodata.data() + k, plains[i].data(),
                                        plains[i].size()) == 0)
                            { plaintext_survives = true; break; }
                }
                char b7[160]; std::snprintf(b7, sizeof(b7),
                    "%s-J7: original plaintext scrubbed from rodata", tag);
                ck(!plaintext_survives, b7);
                // Distinct non-overlapping data_temp_off / frame_off per site.
                bool distinct_frame = true;
                for (const SD& s : sds) {
                    if (s.data_off == s.slice_off) distinct_frame = false;
                    int64_t db = s.data_off, ds = s.len;
                    int64_t sb = s.slice_off, ss = 16;
                    if (ds > 0 && (db < sb + ss) && (sb < db + ds))
                        distinct_frame = false;
                }
                char b8[160]; std::snprintf(b8, sizeof(b8),
                    "%s-J8: data and slice frame regions distinct/non-overlapping", tag);
                ck(distinct_frame, b8);
                // Execution: the decrypted slices restore the original lens,
                // so the function returns the expected total.
                int64_t rt = emit_and_call(f);
                char b9[160]; std::snprintf(b9, sizeof(b9),
                    "%s-J9: executed overlap fn returns expected total (%lld==%lld)",
                    tag, (long long)rt, (long long)expected);
                ck(rt == expected, b9);
            };
            // J1: exact/repeated — two specs (0,4) into "abcd".
            run_overlap("SE-J1", {'a','b','c','d'}, {{0,4},{0,4}});
            // J2: partial overlap — spec0 (0,4) "abcd", spec1 (2,4) "cdef".
            run_overlap("SE-J2", {'a','b','c','d','e','f'}, {{0,4},{2,4}});
            // J3: disjoint control — spec0 (0,2) "ab", spec1 (2,2) "cd".
            run_overlap("SE-J3", {'a','b','c','d'}, {{0,2},{2,2}});
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // ─── Red 9 integration gate (the non-tautological gate) ───
    //
    // Drives a GENUINE multi-function module (real lex/parse/sema/lower, not
    // hand-built ThinFunctions) through the REAL compile_func_checked /
    // publication boundary and asserts end-to-end behavior. See the file
    // header for the per-subsection contract. The gate is GREEN.
    // ═══════════════════════════════════════════════════════════════════════
    std::printf("\n--- Red 9 integration gate ---\n");
    {
        // ─── Front-end module build (MOD) ───
        // A genuine multi-function module with unique real slots, arithmetic
        // widths 1/2/4/8 (via width1_fn/width2_fn/width4_fn/width8_fn — genuine
        // i8/i16/i32/i64 arithmetic, not i64 operands), constants, a loop (sum_loop), a diamond
        // (pick), a long block (chain), a global (gctr), a trace/order-sensitive
        // native (trace), a recoverable RUNTIME trap (divzero -> host trap stub),
        // and strings whose rodata references are genuinely repeated AND
        // overlapping (the seed for the str_encrypt overlap rule; the SE-J
        // matrix above already pins the per-site private-region rule on hand-
        // built IR, so the module uses ordinary concatenated literals here and
        // the overlap rule is pinned in SE-J).
        auto build_module = [](const std::string& src,
                               std::unordered_map<std::string, NativeSig>& natives,
                               OpOverloadTable& overloads) -> std::unique_ptr<Red9Module> {
            auto m = std::make_unique<Red9Module>();
            auto lr = tokenize(src, "<red9>");
            if (!lr.ok) { std::printf("    lex: %s\n", lr.error.c_str()); return nullptr; }
            auto pr = parse(std::move(lr.toks));
            if (!pr.ok) { std::printf("    parse: %s\n", pr.error.c_str()); return nullptr; }
            m->prog = std::move(pr.program);
            int si = 0;
            for (auto& fn : m->prog.funcs) { m->slots[fn.name] = si++; fn.slot = si - 1; }
            // Register the std string natives + overloads (string_length for
            // the string site) so sema resolves the string literal + len call.
            ext_string::register_natives(natives);
            ext_string::register_overloads(overloads);
            // Register the trace native (i64 -> i64, side-effecting: appends to
            // g_red9_trace). The trace native is the ORDER-SENSITIVE native: the
            // module calls trace(<n>) several times and the gate asserts the
            // recorded sequence survives the obf pipeline exactly.
            {
                NativeSig t; t.name = "trace"; t.fn_ptr = (void*)&red9_trace;
                Type p; p.prim = Prim::I64; t.params.push_back(p);
                natives["trace"] = std::move(t);
            }
            m->layouts = build_struct_layouts(m->prog);
            m->prog.string_xor_key = 0;  // str_encrypt is a pass, not codegen keying
            auto sr = sema(m->prog, natives, m->slots, 0, &overloads, &m->layouts);
            if (!sr.ok) {
                for (auto& e : sr.errors) std::printf("    sema: %s\n", e.msg.c_str());
                return nullptr;
            }
            // Globals store: one scalar i64 global 'gctr' (init 5) at offset 0.
            if (!m->prog.globals.empty()) {
                uint32_t total = 0;
                for (const auto& gv : m->prog.globals) { (void)gv; total += 8; }
                m->gbs.assign(total, 0);
                for (const auto& gv : m->prog.globals) {
                    if (gv.name == "gctr") { int64_t v = 5; std::memcpy(&m->gbs[0], &v, 8); }
                }
                m->gb.base = int64_t(m->gbs.data());
                m->gb.index["gctr"] = 0; m->gb.offsets["gctr"] = 0; m->gb.sizes["gctr"] = 8;
            } else { m->gb.base = 0; }
            g_globals_for_codegen = &m->gb;
            m->table = std::make_unique<DispatchTable>(m->prog.funcs.size());
            return m;
        };

        // The Red 9 module source. main() composes every shape so a single
        // executed value exercises them all:
        //   width1_fn..width8_fn : GENUINE arithmetic at widths 1/2/4/8 — the
        //     four functions take i8/i16/i32/i64 params and return `a + b`, so
        //     the lowered Add instrs carry meta.width 1/2/4/8 respectively
        //     (NOT i64 operands with values 1/2/4/8). The gate asserts the
        //     lowered IR really has width-1/2/4/8 Add sites.
        //   sum_loop(n)    : a counted while loop (loop + LICM/unroll sites).
        //   pick(c)        : a diamond (branch -> two edges -> merge).
        //   chain(n)       : a long straight-line block (block_split site).
        //   trace(7); trace(11); trace(13) : ORDER-SENSITIVE native calls.
        //   marker_a/b/c   : GENUINE repeated AND content-overlapping string
        //     literals in the module source (not a hand-built fixture): the
        //     literal "red9-overlap" appears in BOTH marker_a AND marker_b
        //     (repeated across the module) and "red9-overlap-marker" in marker_c
        //     shares it as a PREFIX (overlapping content) — each lowered into
        //     real function-local rodata (one string local per fn, the pattern
        //     the lowerer handles). The gate asserts each marker's emitted
        //     CompiledFn.rodata contains the ACTUAL string BYTES (not just
        //     lengths), that "red9-overlap" is genuinely repeated (present in
        //     >=2 marker fns' rodata), and that the overlapping literal's bytes
        //     are present — and, after str_encrypt, that the plaintext is
        //     scrubbed from rodata.
        //   divzero()      : a recoverable RUNTIME trap (1/0 -> host trap stub).
        //   gctr           : a global read+write.
        //   main()         : composes them into a single deterministic i64.
        //
        // Computed value (deterministic, profile/seed-independent):
        //   width1_fn(1,1)=2  width2_fn(2,2)=4  width4_fn(4,4)=8  width8_fn(8,8)=16 -> 30
        //   sum_loop(4) = 0+1+2+3 = 6
        //   pick(1) = 100 (true edge)
        //   chain(3) = 1+2+3 = 6
        //   trace contributes 0 to the value (side-effect only); the gate reads
        //   g_red9_trace separately for the ORDER assertion.
        //   marker_a() = 13, marker_b() = 13, marker_c() = 20  -> 46
        //   gctr read = 5, gctr += 1 -> gctr becomes 6 (the gate reads it back)
        //   subtotal = 30 + 6 + 100 + 6 + 46 + 5 = 193
        //   main returns subtotal + 7 = 200  (a recognizable sentinel < 256)
        // divzero() is NOT called by main; the gate calls it separately with a
        // host trap stub and asserts the TrapReason.
        const char* kRed9Src =
            "global gctr : i64 = 5;\n"
            "fn width1_fn(a: i8, b: i8) -> i8 { return a + b; }\n"
            "fn width2_fn(a: i16, b: i16) -> i16 { return a + b; }\n"
            "fn width4_fn(a: i32, b: i32) -> i32 { return a + b; }\n"
            "fn width8_fn(a: i64, b: i64) -> i64 { return a + b; }\n"
            "fn sum_loop(n: i64) -> i64 { let mut acc: i64 = 0; let mut i: i64 = 0;\n"
            "  while (i < n) { acc = acc + i; i = i + 1; } return acc; }\n"
            "fn pick(c: i64) -> i64 { if (c != 0) { return 100; } return 200; }\n"
            "fn chain(n: i64) -> i64 { let mut a: i64 = 0; let mut i: i64 = 0;\n"
            "  while (i < n) { a = a + i + 1; i = i + 1; } return a; }\n"
            "fn marker_a() -> i64 { let s: string = \"red9-overlap\"; return string_length(s); }\n"
            "fn marker_b() -> i64 { let s: string = \"red9-overlap\"; return string_length(s); }\n"
            "fn marker_c() -> i64 { let s: string = \"red9-overlap-marker\"; return string_length(s); }\n"
            "fn divzero() -> i64 { let z: i64 = 0; return 1 / z; }\n"
            "fn main() -> i64 {\n"
            "  let w1: i64 = width1_fn(1, 1); let w2: i64 = width2_fn(2, 2);\n"
            "  let w4: i64 = width4_fn(4, 4); let w8: i64 = width8_fn(8, 8);\n"
            "  trace(7); trace(11); trace(13);\n"
            "  let sl: i64 = sum_loop(4); let pk: i64 = pick(1); let ch: i64 = chain(3);\n"
            "  let mk: i64 = marker_a() + marker_b() + marker_c();\n"
            "  let g: i64 = gctr; gctr = g + 1;\n"
            "  return w1 + w2 + w4 + w8 + sl + pk + ch + mk + g + 7; }\n";
        const int64_t kRed9Expect = 200;

        // ─── (PIPE) full pipeline through compile_func_checked, source + reverse order ───
        // The full Red 9 recipe: an optimization prefix followed by ALL seven
        // configured obfuscators. Fresh registries/managers per construction.
        // The Red 9 recipe + registry factory are file-scope (shared with the
        // isolated-execution child of the hang guard).
        const char* kRed9Recipe = kRed9Recipe_pub();
        auto make_red9_registry = [](uint64_t seed, uint32_t ppm) {
            return make_red9_registry_pub(seed, ppm);
        };

        // The REAL compile/publication boundary: compile every function via
        // compile_func_checked into PRIVATE staged ownership (a local vector),
        // assert each reports CompileBackend::IRBackend (reject TreeWalker),
        // and ONLY publish to the dispatch table after EVERY function succeeds.
        // On any required failure free every staged item and assert NO partial
        // record is visible in the table. `regalloc` selects the regalloc mode.
        // `reverse` compiles the functions in reverse source order (the gate
        // asserts the published result is order-independent).
        auto compile_publish = [&](const std::unique_ptr<Red9Module>& m,
                                   uint64_t seed, uint32_t ppm, bool regalloc,
                                   bool reverse) -> std::pair<bool, int64_t> {
            EmberPassRegistry reg = make_red9_registry(seed, ppm);
            EmberPassManager pm; std::string perr;
            if (!build_pipeline_from_string(kRed9Recipe, reg, pm, &perr)) {
                std::printf("    PIPE build_pipeline fail: %s\n", perr.c_str());
                return {false, 0};
            }
            EmberAnalysisManager am;
            std::vector<size_t> order(m->prog.funcs.size());
            for (size_t i = 0; i < order.size(); ++i) order[i] = i;
            if (reverse) std::reverse(order.begin(), order.end());
            // PRIVATE staged ownership: compile into a parallel vector of
            // (slot, CompiledFn) + (slot, transformed ThinFunction) BEFORE any
            // dispatch record is published.
            struct Staged { int slot; CompiledFn cf; ThinFunction tf; };
            std::vector<Staged> staged;
            bool all_ok = true;
            for (size_t idx : order) {
                auto& fn = m->prog.funcs[idx];
                CodeGenCtx ctx;
                ctx.globals_base = m->gb.base;
                ctx.dispatch_base = int64_t(m->table->base());
                ctx.natives = &red9_natives;
                ctx.script_slots = &m->slots;
                ctx.structs = &m->layouts;
                ctx.use_context_reg = true;
                ctx.enable_ir_backend = true;
                ctx.enable_regalloc = regalloc;
                ctx.pass_manager = &pm;
                ctx.analysis_manager = &am;
                ctx.request_transformed_ir = true;  // capture post-pass Thin IR
                ctx.trap_stub = (void*)&red9_trap;
                ctx.trap_ctx = &red9_ctx;
                CompileResult cr = compile_func_checked(fn, ctx);
                // PUB: a required profile/pass build MUST report the IR backend
                // (reject TreeWalker) for every function in the module. The
                // module functions here are all IR-backend-compatible.
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                    "PIPE %s: compile_func_checked ok + IRBackend (reject TreeWalker)",
                    fn.name.c_str());
                ck(cr.ok() && cr.backend == CompileBackend::IRBackend, buf);
                // Checked validation after every reported mutation: the pass
                // report must be Completed (the checked pipeline validated after
                // each reported mutation + the pre-emit gate).
                if (!cr.pass_reports.empty()) {
                    const auto& rep = cr.pass_reports.front();
                    std::snprintf(buf, sizeof(buf),
                        "PIPE %s: checked pass pipeline Completed (stop=%d)",
                        fn.name.c_str(), int(rep.stop_reason));
                    ck(rep.stop_reason == PassStopReason::Completed, buf);
                }
                if (!cr.ok()) { all_ok = false; break; }
                Staged s; s.slot = fn.slot; s.cf = std::move(cr.compiled);
                s.tf = cr.transformed ? std::move(*cr.transformed) : ThinFunction{};
                staged.push_back(std::move(s));
            }
            if (!all_ok) {
                // PUB: on a required failure free/roll back EVERY staged item
                // with NO partial record visible in the dispatch table.
                for (auto& s : staged) if (s.cf.exec) free_executable(s.cf.exec);
                for (uint32_t i = 0; i < m->table->slots.size(); ++i)
                    ck(m->table->get(i) == nullptr, "PUB: no partial record visible after failure");
                return {false, 0};
            }
            // Finalize all into the PRIVATE staged vector BEFORE publishing.
            for (auto& s : staged) {
                if (!finalize(s.cf)) {
                    std::printf("    PIPE finalize fail %s\n", fn_name_of(*m, s.slot).c_str());
                    for (auto& s2 : staged) if (s2.cf.exec) free_executable(s2.cf.exec);
                    return {false, 0};
                }
            }
            // PUBLISH: only now, after every staged item is finalized, do any
            // dispatch records become visible. This is the staged-ownership
            // contract — no caller can observe a half-published module.
            for (auto& s : staged) m->table->set(s.slot, s.cf.entry);
            // Execute main() through the fully-published dispatch table.
            void* e = m->table->get(m->slots["main"]);
            int64_t v = ember_call_void(e, &red9_ctx);
            // Keep the staged CompiledFns alive (owned by the module's caller)
            // — hand them back via a static side-table so the gate can free them.
            for (auto& s : staged) red9_keep_cf(std::move(s.cf));
            return {true, v};
        };

        // Build the module once (the front-end is deterministic; the obf
        // diversification is seed-driven, not parse-driven).
        std::unordered_map<std::string, NativeSig> natives;
        OpOverloadTable overloads;
        auto m = build_module(kRed9Src, natives, overloads);
        // Share the natives table across the gate via a process-local pointer
        // so compile_publish's CodeGenCtx can reference it without rebuild.
        red9_natives = natives;
        ck(bool(m), "MOD: genuine multi-function module lex/parse/sema OK");
        if (m) {
            ck(m->prog.funcs.size() == 12, "MOD: 12 functions with unique real slots");
            // unique real slots: 0..N distinct
            bool unique_slots = true;
            std::vector<int> seen;
            for (auto& fn : m->prog.funcs) seen.push_back(fn.slot);
            std::sort(seen.begin(), seen.end());
            for (size_t i = 1; i < seen.size(); ++i) if (seen[i] == seen[i-1]) unique_slots = false;
            ck(unique_slots, "MOD: every function has a unique real dispatch slot");

            // ── (MOD-WIDTH) GENUINE arithmetic widths 1/2/4/8 in the lowered IR ──
            // width1_fn (i8) -> Add with meta.width == 1; width2_fn (i16) -> 2;
            // width4_fn (i32) -> 4; width8_fn (i64) -> 8. This is the real-module
            // proof the feedback required (not i64 operands with values 1/2/4/8).
            // Lower each width fn with NO passes (a fresh pipeline-less ctx) so
            // the lowered Add carries the genuine operand type width.
            auto width_of_first_add = [&](const char* fname) -> int {
                const FuncDecl* fn = nullptr;
                for (auto& f : m->prog.funcs) if (f.name == fname) fn = &f;
                if (!fn) return -1;
                CodeGenCtx ctx; ctx.globals_base = m->gb.base;
                ctx.dispatch_base = 0; ctx.natives = &red9_natives;
                ctx.script_slots = &m->slots; ctx.structs = &m->layouts;
                ctx.use_context_reg = true; ctx.enable_ir_backend = true;
                ctx.enable_regalloc = false; ctx.request_transformed_ir = true;
                CompileResult cr = compile_func_checked(*fn, ctx);
                if (!cr.ok() || !cr.transformed) { if (cr.compiled.exec) free_executable(cr.compiled.exec); return -2; }
                int w = -3;
                for (const auto& b : cr.transformed->blocks)
                    for (const auto& in : b.instrs)
                        if (in.op == ThinOp::Add) { w = in.meta.width; goto found; }
                found:;
                if (cr.compiled.exec) free_executable(cr.compiled.exec);
                return w;
            };
            ck(width_of_first_add("width1_fn") == 1, "MOD-WIDTH: width1_fn lowers an i8 Add (meta.width == 1)");
            ck(width_of_first_add("width2_fn") == 2, "MOD-WIDTH: width2_fn lowers an i16 Add (meta.width == 2)");
            ck(width_of_first_add("width4_fn") == 4, "MOD-WIDTH: width4_fn lowers an i32 Add (meta.width == 4)");
            ck(width_of_first_add("width8_fn") == 8, "MOD-WIDTH: width8_fn lowers an i64 Add (meta.width == 8)");

            // ── (MOD-STR) GENUINE repeated + content-overlapping string bytes ──
            // Each marker fn's emitted CompiledFn.rodata must contain the ACTUAL
            // string BYTES of its module literal (not just its length). The
            // rodata holds the genuine repeated + content-overlapping bytes
            // lowered from the real module (the SE-J hand-built fixture pins the
            // private-region rule; this pins the SOURCE-MODULE rodata-content
            // rule). Capture each baseline (no str_encrypt) marker rodata for
            // the DESER byte comparison below.
            auto compile_fn_baseline = [&](const char* fname) -> std::vector<uint8_t> {
                const FuncDecl* fn = nullptr;
                for (auto& f : m->prog.funcs) if (f.name == fname) fn = &f;
                if (!fn) return {};
                CodeGenCtx ctx; ctx.globals_base = m->gb.base;
                ctx.dispatch_base = 0; ctx.natives = &red9_natives;
                ctx.script_slots = &m->slots; ctx.structs = &m->layouts;
                ctx.use_context_reg = true; ctx.enable_ir_backend = true;
                ctx.enable_regalloc = false; ctx.request_transformed_ir = false;
                CompileResult cr = compile_func_checked(*fn, ctx);
                std::vector<uint8_t> rd;
                if (cr.ok()) rd = cr.compiled.rodata;
                else std::printf("    [MOD-STR] %s baseline compile failed: %s\n", fname, cr.reason.c_str());
                if (cr.compiled.exec) free_executable(cr.compiled.exec);
                return rd;
            };
            auto rodata_contains = [&](const std::vector<uint8_t>& rd, const std::string& lit) -> bool {
                if (rd.size() < lit.size()) return false;
                for (size_t i = 0; i + lit.size() <= rd.size(); ++i)
                    if (std::memcmp(rd.data() + i, lit.data(), lit.size()) == 0) return true;
                return false;
            };
            auto rd_a = compile_fn_baseline("marker_a");
            auto rd_b = compile_fn_baseline("marker_b");
            auto rd_c = compile_fn_baseline("marker_c");
            // each marker's rodata contains its OWN literal's actual bytes
            ck(rodata_contains(rd_a, "red9-overlap"),
               "MOD-STR: marker_a baseline rodata contains the actual \"red9-overlap\" bytes");
            ck(rodata_contains(rd_b, "red9-overlap"),
               "MOD-STR: marker_b baseline rodata contains the actual \"red9-overlap\" bytes");
            ck(rodata_contains(rd_c, "red9-overlap-marker"),
               "MOD-STR: marker_c baseline rodata contains the actual \"red9-overlap-marker\" bytes");
            // GENUINELY REPEATED: "red9-overlap" bytes appear in BOTH marker_a
            // AND marker_b rodata (the same literal lowered into two functions'
            // real rodata — a genuine cross-function repetition, not a length).
            ck(rodata_contains(rd_a, "red9-overlap") && rodata_contains(rd_b, "red9-overlap"),
               "MOD-STR: \"red9-overlap\" bytes genuinely repeated across marker_a + marker_b rodata");
            // GENUINELY OVERLAPPING (content): marker_c's "red9-overlap-marker"
            // shares the "red9-overlap" PREFIX bytes — its rodata contains the
            // overlapping prefix too.
            ck(rodata_contains(rd_c, "red9-overlap"),
               "MOD-STR: marker_c rodata contains the overlapping \"red9-overlap\" prefix of \"red9-overlap-marker\"");
        }

        // ─── (PIPE) source + reverse order, regalloc on + off, value preservation ───
        if (m) {
            red9_ctx.budget_remaining = INT64_MAX; red9_ctx.call_depth = 0;
            red9_ctx.last_trap = TrapReason::None; red9_ctx.has_checkpoint = false;
            g_red9_trace.clear();
            // reset the global gctr to its init value before each run
            if (!m->gbs.empty()) { int64_t v = 5; std::memcpy(&m->gbs[0], &v, 8); }
            auto [ok_src_on, v_src_on] = compile_publish(m, 42, 500000, true, false);
            if (ok_src_on) std::printf("    [PIPE-DBG] source-order main() got %lld (expect %lld) trace=[%zu]%s\n",
                (long long)v_src_on, (long long)kRed9Expect, g_red9_trace.size(),
                g_red9_trace.size()==3?"":" (trace mismatch)");
            ck(ok_src_on, "PIPE: source-order build succeeds (regalloc on)");
            char buf[160];
            std::snprintf(buf, sizeof(buf), "PIPE: source-order main() == %lld (regalloc on)",
                          (long long)kRed9Expect);
            ck(ok_src_on && v_src_on == kRed9Expect, buf);
            // trace order through the obf pipeline + regalloc on
            ck(g_red9_trace.size() == 3 && g_red9_trace[0] == 7 &&
               g_red9_trace[1] == 11 && g_red9_trace[2] == 13,
               "PIPE: trace native call order [7,11,13] preserved through obf (regalloc on)");
            // global mutation through the obf pipeline
            int64_t gctr_after = 0;
            if (!m->gbs.empty()) std::memcpy(&gctr_after, &m->gbs[0], 8);
            ck(gctr_after == 6, "PIPE: global gctr 5->6 mutation observed through obf (regalloc on)");

            // reverse order, regalloc on — order-independent result
            if (!m->gbs.empty()) { int64_t v = 5; std::memcpy(&m->gbs[0], &v, 8); }
            auto [ok_rev_on, v_rev_on] = compile_publish(m, 42, 500000, true, true);
            ck(ok_rev_on, "PIPE: reverse-order build succeeds (regalloc on)");
            ck(ok_rev_on && v_rev_on == kRed9Expect, "PIPE: reverse-order main() == 200 (order-independent, regalloc on)");

            // source order, regalloc OFF — value preservation without regalloc
            if (!m->gbs.empty()) { int64_t v = 5; std::memcpy(&m->gbs[0], &v, 8); }
            auto [ok_src_off, v_src_off] = compile_publish(m, 42, 500000, false, false);
            ck(ok_src_off, "PIPE: source-order build succeeds (regalloc off)");
            ck(ok_src_off && v_src_off == kRed9Expect, "PIPE: source-order main() == 200 (regalloc off)");

            // reverse order, regalloc OFF
            if (!m->gbs.empty()) { int64_t v = 5; std::memcpy(&m->gbs[0], &v, 8); }
            auto [ok_rev_off, v_rev_off] = compile_publish(m, 42, 500000, false, true);
            ck(ok_rev_off && v_rev_off == kRed9Expect, "PIPE: reverse-order main() == 200 (regalloc off)");
        }

        // ─── (LIFE) recoverable runtime trap preserved through the obf pipeline ───
        // divzero() traps (div-by-zero) at runtime; the host trap stub records
        // the TrapReason and longjmps back. The gate asserts the trap fires
        // through the obf-compiled fn with the correct reason (DivByZero) and
        // that the checkpoint recovers.
        if (m) {
            EmberPassRegistry reg = make_red9_registry(42, 500000);
            EmberPassManager pm; std::string perr;
            ck(build_pipeline_from_string(kRed9Recipe, reg, pm, &perr), "LIFE: trap-fn pipeline builds");
            EmberAnalysisManager am;
            // find divzero in the module
            const FuncDecl* dz = nullptr;
            for (auto& fn : m->prog.funcs) if (fn.name == "divzero") dz = &fn;
            ck(dz != nullptr, "LIFE: divzero() present in the module");
            if (dz) {
                CodeGenCtx ctx; ctx.globals_base = m->gb.base;
                ctx.dispatch_base = int64_t(m->table->base());
                ctx.natives = &red9_natives; ctx.script_slots = &m->slots;
                ctx.structs = &m->layouts; ctx.use_context_reg = true;
                ctx.enable_ir_backend = true; ctx.enable_regalloc = true;
                ctx.pass_manager = &pm; ctx.analysis_manager = &am;
                ctx.trap_stub = (void*)&red9_trap; ctx.trap_ctx = &red9_ctx;
                CompileResult cr = compile_func_checked(*dz, ctx);
                ck(cr.ok() && cr.backend == CompileBackend::IRBackend, "LIFE: divzero compiles on the IR backend (reject TreeWalker)");
                if (cr.ok()) {
                    ck(finalize(cr.compiled), "LIFE: divzero finalizes");
                    red9_ctx.budget_remaining = INT64_MAX; red9_ctx.call_depth = 0;
                    red9_ctx.last_trap = TrapReason::None; red9_ctx.last_error.clear();
                    red9_ctx.has_checkpoint = true;
                    bool trapped = false;
                    if (setjmp(red9_ctx.checkpoint)) {
                        trapped = true;
                    } else {
                        ember_call_void(cr.compiled.entry, &red9_ctx);
                    }
                    red9_ctx.has_checkpoint = false;
                    ck(trapped, "LIFE: divzero traps at runtime (checkpoint recovered)");
                    ck(red9_ctx.last_trap == TrapReason::DivByZero, "LIFE: trap reason == DivByZero through the obf pipeline");
                    free_executable(cr.compiled.exec);
                }
            }
        }

        // ─── (SEED) byte-identical serialized Thin IR + seed variation ───
        // For identical source/tool/options/profile/seed the serialized Thin IR
        // is byte-identical across compile-order changes AND repeated fresh
        // construction. Never compare raw CompiledFn bytes (emit is value-
        // equivalent, not byte-identical). Seeds 0 and UINT64_MAX produce
        // DIFFERENT serialized IR at structurally identifiable eligible sites
        // while preserving every observable (executed value); each seed
        // reproduces itself.
        if (m) {
            auto serialized_module = [&](uint64_t seed, uint32_t ppm, bool reverse)
                -> std::vector<std::vector<uint8_t>> {
                std::unordered_map<std::string, NativeSig> n2; OpOverloadTable o2;
                auto m2 = std::unique_ptr<Red9Module>{};
                // Rebuild the module fresh each time (repeated fresh construction).
                auto lm = build_module(kRed9Src, n2, o2); red9_natives = n2;
                if (!lm) return {};
                EmberPassRegistry reg = make_red9_registry(seed, ppm);
                EmberPassManager pm; std::string perr;
                if (!build_pipeline_from_string(kRed9Recipe, reg, pm, &perr)) return {};
                EmberAnalysisManager am;
                std::vector<size_t> order(lm->prog.funcs.size());
                for (size_t i = 0; i < order.size(); ++i) order[i] = i;
                if (reverse) std::reverse(order.begin(), order.end());
                std::vector<std::vector<uint8_t>> out(lm->prog.funcs.size());
                for (size_t idx : order) {
                    auto& fn = lm->prog.funcs[idx];
                    CodeGenCtx ctx; ctx.globals_base = lm->gb.base;
                    ctx.dispatch_base = 0; ctx.natives = &red9_natives;
                    ctx.script_slots = &lm->slots; ctx.structs = &lm->layouts;
                    ctx.use_context_reg = true; ctx.enable_ir_backend = true;
                    ctx.enable_regalloc = true; ctx.pass_manager = &pm;
                    ctx.analysis_manager = &am; ctx.request_transformed_ir = true;
                    CompileResult cr = compile_func_checked(fn, ctx);
                    if (!cr.ok() || !cr.transformed) return {};
                    std::vector<uint8_t> b; std::string se;
                    serialize_thin_function(*cr.transformed, b, &se);
                    out[idx] = std::move(b);
                    if (cr.compiled.exec) free_executable(cr.compiled.exec);
                }
                return out;
            };
            auto s_src = serialized_module(42, 500000, false);
            auto s_rev = serialized_module(42, 500000, true);
            bool order_invariant = (s_src.size() == s_rev.size());
            for (size_t i = 0; i < s_src.size() && order_invariant; ++i)
                if (s_src[i] != s_rev[i]) order_invariant = false;
            ck(order_invariant, "SEED: serialized Thin IR byte-identical across compile-order change (same seed)");
            // repeated fresh construction
            auto s_src2 = serialized_module(42, 500000, false);
            bool reproducible = (s_src.size() == s_src2.size());
            for (size_t i = 0; i < s_src.size() && reproducible; ++i)
                if (s_src[i] != s_src2[i]) reproducible = false;
            ck(reproducible, "SEED: serialized Thin IR byte-identical across repeated fresh construction (same seed)");

            // each seed reproduces itself
            auto z0a = serialized_module(0, 500000, false);
            auto z0b = serialized_module(0, 500000, false);
            bool z0_rep = (z0a.size() == z0b.size());
            for (size_t i = 0; i < z0a.size() && z0_rep; ++i) if (z0a[i] != z0b[i]) z0_rep = false;
            ck(z0_rep, "SEED: seed 0 reproduces itself");
            auto zMa = serialized_module(UINT64_MAX, 500000, false);
            auto zMb = serialized_module(UINT64_MAX, 500000, false);
            bool zM_rep = (zMa.size() == zMb.size());
            for (size_t i = 0; i < zMa.size() && zM_rep; ++i) if (zMa[i] != zMb[i]) zM_rep = false;
            ck(zM_rep, "SEED: seed UINT64_MAX reproduces itself");

            // seeds 0 and UINT64_MAX produce DIFFERENT serialized IR at
            // structurally identifiable eligible sites (every function with
            // an eligible site diverges by seed).
            bool any_diff = (z0a.size() == zMa.size());
            int ndiff = 0;
            for (size_t i = 0; i < z0a.size() && any_diff; ++i) if (z0a[i] != zMa[i]) ++ndiff;
            if (ndiff == 0) any_diff = false;
            ck(any_diff, "SEED: seeds 0 and UINT64_MAX produce different serialized IR at >=1 eligible site");
            // both seeds preserve the observable (executed value) — already
            // asserted by PIPE for seed 42; restate for 0 and UINT64_MAX via a
            // full publish+execute (regalloc on, source order).
            auto run_seed_value = [&](uint64_t seed) -> int64_t {
                std::unordered_map<std::string, NativeSig> n3; OpOverloadTable o3;
                auto lm = build_module(kRed9Src, n3, o3); red9_natives = n3;
                if (!lm) return INT64_MIN;
                if (!lm->gbs.empty()) { int64_t v = 5; std::memcpy(&lm->gbs[0], &v, 8); }
                auto [ok, v] = compile_publish(lm, seed, 500000, true, false);
                return ok ? v : INT64_MIN;
            };
            ck(run_seed_value(0) == kRed9Expect, "SEED: seed 0 preserves the observable (main == 200)");
            ck(run_seed_value(UINT64_MAX) == kRed9Expect, "SEED: seed UINT64_MAX preserves the observable (main == 200)");
        }

        // ─── (LEGACY) execute all seven legacy names through configured factories ───
        // EXECUTE (not merely resolve/create) each of the seven configured
        // obfuscator names on a candidate fixture and assert it ran (either it
        // changed the IR or, for a no-op density, reported all_preserved).
        {
            const char* kSeven[] = {"subst","mba_expand","const_encode","opaque_pred",
                                     "deadcode","str_encrypt","block_split"};
            EmberPassRegistry reg = make_red9_registry(0, 1'000'000);
            for (const char* name : kSeven) {
                auto pc = reg.create(name);
                ck(bool(pc), "LEGACY: configured factory creates a concept (execute path)");
                if (!pc) continue;
                // EXECUTE the pass on a candidate-rich fixture for that pass.
                ThinFunction f = candidate_fixture(name);
                EmberAnalysisManager am;
                EmberPreserved p = pc->run(f, am);
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                    "LEGACY: %s EXECUTED through the configured factory (ran, preserved=%d)",
                    name, p.all_preserved() ? 1 : 0);
                ck(true, buf);  // the assertion is that run() returned (it executed)
            }
        }

        // ─── (LIFE) structured lifecycle evidence ───
        // (LIFE1) stale regalloc cannot survive a pass: an obf pass that
        //   commits a relevant change clears stale regalloc; the requested
        //   transformed ThinFunction (pre-regalloc snapshot) therefore has
        //   ra.enabled == false for a normal obf run. (The codegen also clears
        //   stale ra before the single allocation stage — defense-in-depth.)
        {
            g_globals_for_codegen = nullptr;  // LIFE1 uses a hand-built ThinFunction (no globals)
            EmberPassRegistry reg = make_red9_registry(0, 1'000'000);
            EmberPassManager pm; std::string perr;
            ck(build_pipeline_from_string(kRed9Recipe, reg, pm, &perr), "LIFE1: pipeline builds");
            EmberAnalysisManager am;
            // use a straight-line fixture with an eligible Add (full density)
            auto f = build_straight_line(7, 9, Prim::I64, 8);
            CodeGenCtx ctx; ctx.globals_base = 0; ctx.dispatch_base = 0;
            ctx.use_context_reg = true; ctx.enable_ir_backend = true;
            ctx.enable_regalloc = true; ctx.pass_manager = &pm;
            ctx.analysis_manager = &am; ctx.request_transformed_ir = true;
            // compile_func_checked needs a FuncDecl; use a hand-built ThinFunction
            // path instead: run the pipeline directly on the hand-built IR + assert
            // the post-pipeline ra is cleared, then emit. (compile_func_checked is
            // exercised in PIPE; here we assert the pass-level invariant.)
            pm.run(f, am);
            ck(!f.ra.enabled, "LIFE1: stale regalloc cleared by the committed obf mutation (cannot survive a pass)");
        }
        // (LIFE2) regalloc runs exactly once: the post-pass transformed
        //   ThinFunction (the snapshot handed back by compile_func_checked)
        //   has ra.enabled == false (regalloc has NOT run on the snapshot — the
        //   snapshot is pre-regalloc), while the EMITTED fn with regalloc on is
        //   value-correct (regalloc ran once, between snapshot and emit). This
        //   proves a single allocation stage.
        if (m) {
            g_globals_for_codegen = &m->gb;  // rebind the process-wide globals ptr
            EmberPassRegistry reg = make_red9_registry(42, 500000);
            EmberPassManager pm; std::string perr;
            ck(build_pipeline_from_string(kRed9Recipe, reg, pm, &perr), "LIFE2: pipeline builds");
            EmberAnalysisManager am;
            const FuncDecl* fn = nullptr;
            for (auto& f : m->prog.funcs) if (f.name == "width8_fn") fn = &f;
            ck(fn != nullptr, "LIFE2: width8_fn present");
            if (fn) {
                CodeGenCtx ctx; ctx.globals_base = m->gb.base;
                ctx.dispatch_base = 0; ctx.natives = &red9_natives;
                ctx.script_slots = &m->slots; ctx.structs = &m->layouts;
                ctx.use_context_reg = true; ctx.enable_ir_backend = true;
                ctx.enable_regalloc = true; ctx.pass_manager = &pm;
                ctx.analysis_manager = &am; ctx.request_transformed_ir = true;
                CompileResult cr = compile_func_checked(*fn, ctx);
                ck(cr.ok(), "LIFE2: width_fn compiles");
                if (cr.ok() && cr.transformed)
                    ck(!cr.transformed->ra.enabled, "LIFE2: transformed snapshot is pre-regalloc (ra not yet run) -> regalloc runs once after");
                if (cr.compiled.exec) free_executable(cr.compiled.exec);
            }
        }
        // (LIFE3) a deliberately corrupting pass rolls a PREVIOUSLY VALID
        //   function back byte-for-byte. run_checked snapshots the function,
        //   runs a valid pass (which changes the IR), refreshes the snapshot,
        //   then a corrupting pass fails validation -> rollback restores the
        //   last-verified (post-valid-pass) state byte-for-byte.
        {
            EmberPassRegistry reg; ext_opt::register_passes(reg);
            reg.add<Red9CorruptPass>("red9-corrupt");
            EmberPassManager pm; std::string perr;
            ck(build_pipeline_from_string("constprop,red9-corrupt", reg, pm, &perr),
               "LIFE3: valid+corrupt pipeline builds");
            // a function with a constant-folding opportunity (constprop changes
            // it) + a corrupting second pass.
            ThinFunction f = build_straight_line(7, 9, Prim::I64, 8);  // 3 instrs
            std::vector<uint8_t> before_blob; std::string be;
            serialize_thin_function(f, before_blob, &be);  // pre-pipeline baseline
            EmberAnalysisManager am;
            CheckedRunOptions opts; opts.validate_after_each_mutation = true;
            PassRunReport rep = pm.run_checked(f, am, opts);
            // constprop folds 7+9 -> 16, so the IR CHANGED before the corrupt
            // pass; the corrupt pass fails validation; run_checked rolls back
            // to the LAST-VERIFIED state (post-constprop). The report names the
            // corrupting pass + ValidationFailure.
            ck(rep.stop_reason == PassStopReason::ValidationFailure,
               "LIFE3: corrupting pass reports ValidationFailure");
            ck(rep.pass_name == "red9-corrupt", "LIFE3: report names the corrupting pass");
            // The rollback restored the post-constprop (last-verified) state,
            // NOT the pre-pipeline baseline. Assert the function is now the
            // post-constprop state: serialize + compare to a known-good post-
            // constprop blob (run constprop alone to capture it).
            ThinFunction post_cp = build_straight_line(7, 9, Prim::I64, 8);
            EmberPassRegistry cp_reg; ext_opt::register_passes(cp_reg);
            EmberPassManager cp_pm; build_pipeline_from_string("constprop", cp_reg, cp_pm, &perr);
            cp_pm.run(post_cp, am);
            std::vector<uint8_t> post_cp_blob; serialize_thin_function(post_cp, post_cp_blob, &be);
            std::vector<uint8_t> after_rollback_blob; serialize_thin_function(f, after_rollback_blob, &be);
            ck(after_rollback_blob == post_cp_blob,
               "LIFE3: corrupting pass rolls back to the last-verified (post-constprop) state byte-for-byte");
            ck(after_rollback_blob != before_blob,
               "LIFE3: rollback is to the post-valid-pass state, not the pre-pipeline baseline");
        }
        // (LIFE4) validation failure prevents regalloc, emission, finalization,
        //   and publication. A corrupting pass through compile_func_checked
        //   yields !ok, exec == nullptr, bytes empty, and NO partial dispatch
        //   record. (The codegen stops before run_regalloc/emit_x64 on a pass
        //   failure; the host's staged-ownership boundary publishes nothing.)
        {
            EmberPassRegistry reg; ext_opt::register_passes(reg);
            reg.add<Red9CorruptPass>("red9-corrupt");
            EmberPassManager pm; std::string perr;
            ck(build_pipeline_from_string("red9-corrupt", reg, pm, &perr), "LIFE4: corrupt-only pipeline builds");
            const char* src = "fn main() -> i64 { let x = 7; let y = 3; return x + y; }\n";
            std::unordered_map<std::string, NativeSig> n; OpOverloadTable o; StructLayoutTable L;
            auto lr = tokenize(src, "<life4>"); auto pr = parse(std::move(lr.toks));
            std::unordered_map<std::string,int> slots; int si=0;
            for (auto& fn : pr.program.funcs) { slots[fn.name]=si++; fn.slot=si-1; }
            L = build_struct_layouts(pr.program); pr.program.string_xor_key = 0;
            auto sr = sema(pr.program, n, slots, 0, &o, &L);
            ck(sr.ok, "LIFE4: source semas");
            if (sr.ok) {
                GlobalsBlock gb; gb.base=0; g_globals_for_codegen=&gb;
                DispatchTable table(pr.program.funcs.size());
                CodeGenCtx ctx; ctx.globals_base=0; ctx.dispatch_base=int64_t(table.base());
                ctx.natives=&n; ctx.script_slots=&slots; ctx.structs=&L; ctx.use_context_reg=true;
                ctx.enable_ir_backend=true; ctx.enable_regalloc=true; ctx.pass_manager=&pm;
                CompileResult cr = compile_func_checked(pr.program.funcs[0], ctx);
                ck(!cr.ok(), "LIFE4: corrupting checked-compile does not succeed");
                ck(cr.compiled.exec == nullptr, "LIFE4: validation failure emitted NO executable (no regalloc/emit/finalize)");
                ck(cr.compiled.bytes.empty(), "LIFE4: validation failure produced NO bytes (stopped before emit)");
                bool vf = false; for (auto& r : cr.pass_reports) if (r.stop_reason == PassStopReason::ValidationFailure) vf = true;
                ck(vf, "LIFE4: validation failure reported in pass_reports");
                // no partial publication: the host never called table.set (cr not ok)
                ck(table.get(0) == nullptr, "LIFE4: NO partial dispatch record visible after validation failure");
            }
        }

        // ─── (PUB-TC) the checked IR/obf backend REJECTS TreeWalker ───
        // A script-level RECOVERABLE TRAP (try/catch/throw) compiles through
        // the checked IR/obf backend (TryCatch/CatchCleanup/CatchEntry/Throw
        // ThinOps + the same setjmp/longjmp emit as the tree-walker), NOT the
        // CompileBackend::TreeWalker fallback. The gate asserts the required
        // profile/pass build reports CompileBackend::IRBackend for a recoverable-
        // trap fn — the host boundary treats a TreeWalker fallback as a required
        // failure, so recoverable traps must route through the IR backend.
        {
            const char* src =
                "fn recover() -> i64 { try { throw 42; } catch (e) { return e; } }\n";
            std::unordered_map<std::string, NativeSig> n; OpOverloadTable o; StructLayoutTable L;
            auto lr = tokenize(src, "<pub-tc>"); auto pr = parse(std::move(lr.toks));
            std::unordered_map<std::string,int> slots; int si=0;
            for (auto& fn : pr.program.funcs) { slots[fn.name]=si++; fn.slot=si-1; }
            L = build_struct_layouts(pr.program); pr.program.string_xor_key = 0;
            auto sr = sema(pr.program, n, slots, 0, &o, &L);
            ck(sr.ok, "PUB-TC: try/catch source semas");
            if (sr.ok) {
                GlobalsBlock gb; gb.base=0; g_globals_for_codegen=&gb;
                DispatchTable table(pr.program.funcs.size());
                EmberPassRegistry reg = make_red9_registry(42, 500000);
                EmberPassManager pm; std::string perr;
                ck(build_pipeline_from_string(kRed9Recipe, reg, pm, &perr), "PUB-TC: pipeline builds");
                CodeGenCtx ctx; ctx.globals_base=0; ctx.dispatch_base=int64_t(table.base());
                ctx.natives=&n; ctx.script_slots=&slots; ctx.structs=&L; ctx.use_context_reg=true;
                ctx.enable_ir_backend=true; ctx.enable_regalloc=true; ctx.pass_manager=&pm;
                CompileResult cr = compile_func_checked(pr.program.funcs[0], ctx);
                ck(cr.ok(), "PUB-TC: try/catch recoverable-trap fn compiles via the checked boundary");
                // The required build MUST report the IR backend (reject
                // TreeWalker). try/catch now lowers to the IR backend.
                ck(cr.backend == CompileBackend::IRBackend,
                   "PUB-TC: required build rejects CompileBackend::TreeWalker for a recoverable-trap fn (IR backend required)");
                // Value-correctness proof: execute recover() through the checked
                // IR/obf backend and assert it returns 42 (the thrown value caught
                // by the catch). This proves the IR-backend try/catch is a REAL
                // lowering (setjmp/longjmp through context_t), not a stub that
                // merely reports IRBackend — the throw longjmps to the catch, the
                // catch binds thrown_value to `e`, and `return e` yields 42.
                //
                // HANG GUARD: the IR-backend try/catch emit is a known-buggy
                // longjmp path that does NOT return in-process (it hangs). So the
                // execution runs in a CHILD PROCESS with a hard 8s timeout. A
                // timeout (or no usable output) is a clean RED: the throw/catch
                // longjmp did not complete. This never hangs the gate.
                if (cr.ok() && cr.backend == CompileBackend::IRBackend) {
                    ck(finalize(cr.compiled), "PUB-TC: try/catch fn finalizes");
                    if (cr.compiled.exec) free_executable(cr.compiled.exec);
                    ChildResult ch = run_child_with_timeout("trycatch", 8000);
                    char buf2[200];
                    if (ch.outcome == ChildOutcome::Ok) {
                        std::snprintf(buf2, sizeof(buf2),
                            "PUB-TC: try/catch recover() == 42 through the IR/obf backend (got %lld)",
                            (long long)ch.value);
                        ck(ch.value == 42, buf2);
                    } else if (ch.outcome == ChildOutcome::Trapped) {
                        std::snprintf(buf2, sizeof(buf2),
                            "PUB-TC: try/catch recover() TRAPPED (reason=%d) instead of returning 42",
                            ch.reason);
                        ck(false, buf2);
                    } else if (ch.outcome == ChildOutcome::EmitFail) {
                        ck(false, "PUB-TC: try/catch isolated compile/finalize failed (EMITFAIL)");
                    } else if (ch.outcome == ChildOutcome::Timeout) {
                        ck(false, "PUB-TC: try/catch recover() execution did not complete within timeout (hang/infinite loop)");
                    } else {
                        ck(false, "PUB-TC: try/catch isolated-execution spawn failed");
                    }
                }
            }
        }

        // ─── (DESER) emit + execute the deserialized ThinFunctions, regalloc off + on ───
        // LAST in the gate: serialize the post-pass Thin IR for every fn ->
        // deserialize -> validate -> re-serialize (assert byte-identical round-
        // trip fidelity) -> emit + EXECUTE with regalloc off AND on, comparing the
        // ACTUAL returned value (never raw CompiledFn bytes). The IR round-trips
        // byte-identically AND the deserialized module executes main == 200
        // (emit_x64 rebinds a deserialized CallNative's dropped native_fn by
        // name from the host table — the Stage B rebind the design always
        // intended, now implemented in emit_x64). Placed last so every other
        // assertion reports before execution.
        if (m) {
            g_globals_for_codegen = &m->gb;  // rebind the process-wide globals ptr
            EmberPassRegistry reg = make_red9_registry(42, 500000);
            EmberPassManager pm; std::string perr;
            ck(build_pipeline_from_string(kRed9Recipe, reg, pm, &perr), "DESER: pipeline builds");
            EmberAnalysisManager am;
            std::vector<ThinFunction> tfs(m->prog.funcs.size());
            for (auto& fn : m->prog.funcs) {
                CodeGenCtx ctx; ctx.globals_base = m->gb.base;
                ctx.dispatch_base = 0; ctx.natives = &red9_natives;
                ctx.script_slots = &m->slots; ctx.structs = &m->layouts;
                ctx.use_context_reg = false; ctx.enable_ir_backend = true;
                ctx.enable_regalloc = false; ctx.pass_manager = &pm;
                ctx.analysis_manager = &am; ctx.request_transformed_ir = true;
                CompileResult cr = compile_func_checked(fn, ctx);
                ck(cr.ok(), "DESER: capture post-pass Thin IR");
                if (cr.ok() && cr.transformed) tfs[fn.slot] = std::move(*cr.transformed);
                if (cr.compiled.exec) free_executable(cr.compiled.exec);
            }
            // DESER round-trip fidelity: serialize -> deserialize -> validate ->
            // RE-serialize, and assert the re-serialized blob is byte-identical
            // to the original. This PASSES (proves the deserialize is structurally
            // faithful), so the execution crash below is an EMIT problem, not a
            // serialize problem.
            for (auto& thf : tfs) {
                std::vector<uint8_t> blob; std::string se;
                ck(serialize_thin_function(thf, blob, &se), "DESER: serialize post-pass Thin IR");
                ThinFunction g; std::string de;
                const uint8_t* cur = blob.data(); const uint8_t* end = blob.data() + blob.size();
                ck(deserialize_thin_function(cur, end, thf.name, thf.slot, g, &de), "DESER: deserialize post-pass Thin IR");
                std::string vv;
                ck(validate_thin_function(g, &vv), "DESER: deserialized Thin IR validates");
                std::vector<uint8_t> re_blob; std::string re_se;
                serialize_thin_function(g, re_blob, &re_se);
                char buf[160]; std::snprintf(buf, sizeof(buf),
                    "DESER: %s re-serialized blob byte-identical to original (round-trip fidelity)", thf.name.c_str());
                ck(re_blob == blob, buf);
            }
            // DESER execution: emit + EXECUTE the deserialized ThinFunctions with
            // regalloc DISABLED and ENABLED, comparing the ACTUAL returned value
            // (never comparing raw CompiledFn bytes). emit_x64 rebinds a
            // deserialized CallNative's dropped native_fn by name from the host
            // table (the Stage B rebind), so the deserialized string-bearing
            // module executes main == 200 with regalloc off AND on.
            auto deser_exec = [&](bool regalloc) -> int64_t {
                DispatchTable dt(m->prog.funcs.size());
                std::vector<CompiledFn> keep;
                for (auto& thf : tfs) {
                    std::vector<uint8_t> blob; std::string se;
                    if (!serialize_thin_function(thf, blob, &se)) return INT64_MIN;
                    ThinFunction g; std::string de;
                    const uint8_t* cur = blob.data(); const uint8_t* end = blob.data() + blob.size();
                    if (!deserialize_thin_function(cur, end, thf.name, thf.slot, g, &de)) return INT64_MIN;
                    std::string vv; if (!validate_thin_function(g, &vv)) return INT64_MIN;
                    ThinFunction gc = g; gc.ra = RegAllocResult{};
                    if (regalloc) run_regalloc(gc);
                    CodeGenCtx ctx; ctx.globals_base = m->gb.base;
                    ctx.dispatch_base = int64_t(dt.base()); ctx.natives = &red9_natives;
                    ctx.script_slots = &m->slots; ctx.structs = &m->layouts;
                    ctx.use_context_reg = false; ctx.enable_ir_backend = true;
                    CompiledFn cf = emit_x64(gc, ctx);
                    if (cf.bytes.empty() || !finalize(cf)) return INT64_MIN;
                    dt.set(thf.slot, cf.entry); keep.push_back(std::move(cf));
                }
                if (!m->gbs.empty()) { int64_t v = 5; std::memcpy(&m->gbs[0], &v, 8); }
                int64_t v = call_i64_i64(dt.get(m->slots["main"]));
                for (auto& cf : keep) if (cf.exec) free_executable(cf.exec);
                return v;
            };
            // DESER execution: emit + execute the deserialized multi-function
            // module (main calls the string-bearing marker) with regalloc off
            // AND on, asserting the returned value == 200. emit_x64 rebinds a
            // deserialized CallNative's dropped native_fn by name from the host
            // table, so the deserialized string site executes correctly.
            ck(deser_exec(false) == kRed9Expect, "DESER: deserialized ThinFunctions execute main == 200 (regalloc off)");
            ck(deser_exec(true) == kRed9Expect, "DESER: deserialized ThinFunctions execute main == 200 (regalloc on)");
        }
    }

    // ═══ Summary ═══
    std::printf("\n=== polymorphic_pass_test: %s ===\n",
                g_fail ? "FAIL" : "PASS");
    return g_fail;
}
