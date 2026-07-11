// codegen_opt_test — pins the Stage 1 codegen-optimization peephole + local
// regalloc rewrites' VALUE EQUIVALENCE (docs/spec/CODEGEN_OPTIMIZATION_DESIGN.md §4.7:
// "a new codegen_opt_test pins each peephole rewrite's byte equivalence — the
// rewritten sequence computes the same value").
//
// The rewritten bytes DIFFER (the SmartImm pass shrinks `mov rax,imm64` to
// `mov eax,imm32` / `mov rax,imm32`; the local regalloc replaces `push/pop`
// with `mov r12/rax`), but the COMPUTED VALUE is identical. This is the
// correctness pin: for each rewrite, compile the SAME source with flags OFF
// and flags ON, call both, assert identical return values. A regression in a
// peephole (a wrong shrink) or the regalloc (a wrong r12 clobber) turns the
// value assertion red.
//
// Non-circular: the assertion reads the i64 return out of rax in C (the
// array_lit_test / v0_4_hardening shape), NOT an in-language comparison that
// could hide a codegen bug behind its own buggy codegen.
//
// Also pins:
//   - flags OFF = byte-identical to the pre-Stage-1 tree-walker: the SAME
//     source compiled with enable_peephole=false / enable_local_regalloc=false
//     produces bytes identical to a fresh compile of the same source (the flags
//     are inert by default; the gate's "byte-identical when off" contract).
//   - flags ON where a rewrite applies produces DIFFERENT (shorter or NOP-
//     padded) bytes than flags OFF — proving the pass actually ran (not a
//     no-op gate that silently skipped). This is the "the flag works" pin.
//
// Links only the core libs (ember ember_frontend ember_import) - the probes
// are pure language features (int arithmetic, no natives), so this test is
// INDEPENDENT of the extension refactor (no ember_ext_* link needed).

#include "../src/engine.hpp"
#include "../src/dispatch_table.hpp"
#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"
#include "../src/codegen.hpp"
#include "../src/globals.hpp"
#include "../src/binding_builder.hpp"  // NativeTable (empty here)

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace ember;

struct M {
    std::vector<CompiledFn> fns;
    std::unique_ptr<DispatchTable> table;
    std::unordered_map<std::string, int> slots;
    GlobalsBlock gb;
    std::vector<uint8_t> gbs;
    Program prog;
    M() : table(std::make_unique<DispatchTable>(0)) {}
};

static int g_fail = 0;
static void ck(bool c, const char* m) {
    std::printf("[%s] %s\n", c ? "PASS" : "FAIL", m);
    if (!c) g_fail = 1;
}

// Compile `src` with the Stage-1 flags set per `peephole`/`regalloc`. Returns
// the module + (via out_bytes) the compiled `main`'s raw bytes BEFORE
// finalize (so the test can diff bytes between flag configurations — finalize
// makes the bytes executable, which mprotects the page but leaves the bytes).
static std::unique_ptr<M> compile(const std::string& src, bool peephole, bool regalloc,
                                   std::vector<uint8_t>* main_bytes = nullptr) {
    auto m = std::make_unique<M>();
    auto lr = tokenize(src, "<codegen_opt>");
    if (!lr.ok) { std::printf("FAIL: lex: %s\n", lr.error.c_str()); return nullptr; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::printf("FAIL: parse: %s\n", pr.error.c_str()); return nullptr; }
    m->prog = std::move(pr.program);
    int si = 0;
    for (auto& fn : m->prog.funcs) { m->slots[fn.name] = si++; fn.slot = m->slots[fn.name]; }
    NativeTable nt;  // empty: no natives used by these probes
    auto layouts = build_struct_layouts(m->prog);
    m->prog.string_xor_key = 0;  // no string natives here; keep raw
    auto sr = sema(m->prog, nt.natives, m->slots, 0, &nt.overloads, &layouts);
    if (!sr.ok) {
        std::printf("FAIL: sema (%zu errors):\n", sr.errors.size());
        for (auto& e : sr.errors) std::printf("  line %u: %s\n", e.line, e.msg.c_str());
        return nullptr;
    }
    m->gbs.assign(m->prog.globals.size() * 8, 0);
    m->gb.base = int64_t(m->gbs.data());
    { uint32_t gi = 0; for (auto& g : m->prog.globals) { m->gb.index[g.name] = gi++; m->gb.types[g.name] = g.ty.get(); } }
    g_globals_for_codegen = &m->gb;
    m->table = std::make_unique<DispatchTable>(m->prog.funcs.size());
    CodeGenCtx ctx;
    ctx.globals_base = m->gb.base;
    ctx.dispatch_base = int64_t(m->table->base());
    ctx.natives = &nt.natives;
    ctx.script_slots = &m->slots;
    ctx.structs = &layouts;
    ctx.enable_peephole = peephole;
    ctx.enable_local_regalloc = regalloc;
    for (auto& fn : m->prog.funcs) {
        auto cf = compile_func(fn, ctx);
        if (main_bytes && fn.name == "main") *main_bytes = cf.bytes;  // capture pre-finalize bytes
        if (!finalize(cf)) { std::printf("FAIL: alloc_executable for %s\n", fn.name.c_str()); return nullptr; }
        m->table->set(fn.slot, cf.entry);
        m->fns.push_back(std::move(cf));
    }
    return m;
}

static int64_t call0_i64(M& m, const std::string& fn) {
    auto it = m.slots.find(fn);
    if (it == m.slots.end()) return -1;
    void* e = m.table->get(it->second);
    using F = int64_t (*)();
    return reinterpret_cast<F>(e)();
}

int main() {
    std::printf("=== codegen Stage 1 optimization — value-equivalence pin ===\n");
    std::printf("(docs/spec/CODEGEN_OPTIMIZATION_DESIGN.md §4.7: each rewrite computes the same value)\n\n");

    // ======================================================================
    // Part A: the flags-OFF = byte-identical contract (the gate's "no flag-day
    // rewrite" discipline). The SAME source compiled twice with both flags off
    // produces byte-identical `main` bytes. (Both compiles go through the
    // default tree-walker; neither pass runs.)
    // ======================================================================
    {
        const char* src = "fn main() -> i64 { let a: i64 = 7; let b: i64 = 3; return a + b * 2 - 1; }\n";
        std::vector<uint8_t> b1, b2;
        auto m1 = compile(src, false, false, &b1);
        auto m2 = compile(src, false, false, &b2);
        ck(m1.get() && m2.get(), "[A0] flags-off compiles (x2)");
        if (m1 && m2) {
            ck(b1 == b2, "[A1] flags-off = byte-identical across two compiles (no hidden nondeterminism)");
            ck(call0_i64(*m1, "main") == 12, "[A2] flags-off value: 7+3*2-1 == 12");
        }
    }

    // ======================================================================
    // Part B: SmartImm peephole (design W4) — `mov rax, imm64` -> cheaper imm32
    // forms for small literals. Value-equivalence: flags-on computes the same
    // value; bytes differ (shorter). Covers:
    //   B1: u32-fit literal (return 42) -> mov eax, imm32 (5 bytes, zero-extends).
    //   B2: s32-fit negative literal (return -100) -> mov rax, imm32 (7 bytes,
    //       sign-extended).
    //   B3: large literal (return 0x123456789ABCDEF0, >u32) -> NO shrink
    //       (value preserved, bytes IDENTICAL — the range check rejects it).
    //   B4: peephole in a loop (the loop body's small literals shrink each
    //       iteration — value preserved over many iters).
    // ======================================================================
    std::printf("\n--- SmartImm (W4) ---\n");
    {
        const char* src = "fn main() -> i64 { return 42; }\n";
        std::vector<uint8_t> bo, bn;
        auto mo = compile(src, false, false, &bo);
        auto mn = compile(src, true,  false, &bn);
        ck(mo.get() && mn.get(), "[B1] u32-fit literal compiles (off + on)");
        if (mo && mn) {
            ck(call0_i64(*mo, "main") == 42, "[B1] flags-off value: 42");
            ck(call0_i64(*mn, "main") == 42, "[B1] flags-on  value: 42 (mov eax,imm32 zero-extends)");
            ck(bo != bn, "[B1] flags-on bytes DIFFER (peephole ran — not a silent no-op gate)");
            // The on-bytes may be shorter or NOP-padded; either way they are not
            // byte-identical to off (the rewrite fired).
        }
    }
    {
        const char* src = "fn main() -> i64 { return -100; }\n";
        std::vector<uint8_t> bo, bn;
        auto mo = compile(src, false, false, &bo);
        auto mn = compile(src, true,  false, &bn);
        ck(mo.get() && mn.get(), "[B2] s32-fit negative literal compiles (off + on)");
        if (mo && mn) {
            ck(call0_i64(*mo, "main") == -100, "[B2] flags-off value: -100");
            ck(call0_i64(*mn, "main") == -100, "[B2] flags-on  value: -100 (mov rax,imm32 sign-extends)");
            ck(bo != bn, "[B2] flags-on bytes DIFFER");
        }
    }
    {
        // 0x123456789ABCDEF0 > UINT32_MAX and outside INT32 range -> SmartImm rejects.
        const char* src = "fn main() -> i64 { return 1311768467463790320; }\n"; // 0x123456789ABCDEF0
        std::vector<uint8_t> bo, bn;
        auto mo = compile(src, false, false, &bo);
        auto mn = compile(src, true,  false, &bn);
        ck(mo.get() && mn.get(), "[B3] >u32 literal compiles (off + on)");
        if (mo && mn) {
            ck(call0_i64(*mo, "main") == 1311768467463790320LL, "[B3] flags-off value: 0x123456789ABCDEF0");
            ck(call0_i64(*mn, "main") == 1311768467463790320LL, "[B3] flags-on  value: 0x123456789ABCDEF0 (no shrink — value preserved)");
            ck(bo == bn, "[B3] flags-on bytes IDENTICAL (range check rejected the >u32 imm — no rewrite)");
        }
    }
    {
        // Small literals in a loop body shrink each iteration; value preserved.
        const char* src =
            "fn main() -> i64 { let mut s: i64 = 0; let mut i: i64 = 0; "
            "while (i < 1000) { s = s + 1; i = i + 1; } return s; }\n";
        auto mo = compile(src, false, false);
        auto mn = compile(src, true,  false);
        ck(mo.get() && mn.get(), "[B4] small-literal-in-loop compiles (off + on)");
        if (mo && mn) {
            ck(call0_i64(*mo, "main") == 1000, "[B4] flags-off value: loop sum == 1000");
            ck(call0_i64(*mn, "main") == 1000, "[B4] flags-on  value: loop sum == 1000 (per-iter shrinks preserve value)");
        }
    }

    // ======================================================================
    // Part C: local regalloc (design W1) — BinExpr integer LHS in r12 across
    // the RHS eval instead of push/pop. Value-equivalence across inputs and
    // nesting shapes:
    //   C1: simple `a+b` (the outermost BinExpr claims r12).
    //   C2: nested `a + b * c` (outer + claims r12; inner * falls back to
    //       push/pop — correctness: a single holding register can't nest).
    //   C3: BinExpr with a script CALL in the RHS (r12 is callee-saved; the
    //       call preserves the caller's lhs across the callee).
    //   C4: BinExpr in a tight loop (the r12 claim/release per statement,
    //       iterated — value preserved over many iters).
    //   C5: a chain of BinExprs in one statement (`a+b+c+d`) — each is a
    //       separate outermost BinExpr (left-assoc parse -> ((a+b)+c)+d),
    //       each claims/releases r12 sequentially.
    // ======================================================================
    std::printf("\n--- local regalloc (W1) ---\n");
    {
        const char* src = "fn main() -> i64 { let a: i64 = 100; let b: i64 = 23; return a + b; }\n";
        std::vector<uint8_t> bo, bn;
        auto mo = compile(src, false, false, &bo);
        auto mn = compile(src, false, true,  &bn);
        ck(mo.get() && mn.get(), "[C1] simple a+b compiles (off + regalloc)");
        if (mo && mn) {
            ck(call0_i64(*mo, "main") == 123, "[C1] flags-off value: 100+23 == 123");
            ck(call0_i64(*mn, "main") == 123, "[C1] regalloc  value: 100+23 == 123 (lhs in r12 across rhs)");
            ck(bo != bn, "[C1] regalloc bytes DIFFER (push/pop -> mov r12/rax)");
        }
    }
    {
        // Nested: outer + holds lhs(a) in r12; inner * (b*c) falls back to push/pop
        // (r12 occupied). Both paths must produce the right value.
        const char* src = "fn main() -> i64 { let a: i64 = 10; let b: i64 = 20; let c: i64 = 3; return a + b * c; }\n";
        auto mo = compile(src, false, false);
        auto mn = compile(src, false, true);
        ck(mo.get() && mn.get(), "[C2] nested a+(b*c) compiles (off + regalloc)");
        if (mo && mn) {
            ck(call0_i64(*mo, "main") == 70, "[C2] flags-off value: 10+20*3 == 70");
            ck(call0_i64(*mn, "main") == 70, "[C2] regalloc  value: 10+20*3 == 70 (outer r10, inner push/pop fallback)");
        }
    }
    {
        // BinExpr with a script call in the RHS: r10 is VOLATILE (clobbered by a
        // call), so a call-bearing RHS falls back to push/pop — the regalloc is
        // correct (the value is preserved via the stack fallback), and the bytes
        // for THIS shape are IDENTICAL to flags-off (the regalloc didn't fire on
        // the call-bearing RHS). This is the design's no-tax discipline: a
        // volatile holding register can't survive a call, so we don't try.
        const char* src =
            "fn inc(x: i64) -> i64 { return x + 1; }\n"
            "fn main() -> i64 { let a: i64 = 41; return a + inc(1); }\n";
        std::vector<uint8_t> bo, bn;
        auto mo = compile(src, false, false, &bo);
        auto mn = compile(src, false, true,  &bn);
        ck(mo.get() && mn.get(), "[C3] BinExpr with call-in-rhs compiles (off + regalloc)");
        if (mo && mn) {
            ck(call0_i64(*mo, "main") == 43, "[C3] flags-off value: 41+inc(1) == 43");
            ck(call0_i64(*mn, "main") == 43, "[C3] regalloc  value: 41+inc(1) == 43 (push/pop fallback — r10 volatile)");
            // Note: a raw byte comparison (bo == bn) is confounded by baked
            // dispatch-base addresses (each compile() makes a fresh DispatchTable
            // at a different address), so we do NOT assert byte-identity here.
            // The VALUE pin above is the correctness pin; the fallback's
            // correctness is that the value matches flags-off.
        }
    }
    {
        // Regalloc in a tight loop: the r10 claim/release per iteration.
        const char* src =
            "fn main() -> i64 { let mut s: i64 = 0; let mut i: i64 = 0; "
            "while (i < 5000) { s = s + i; i = i + 1; } return s; }\n";
        auto mo = compile(src, false, false);
        auto mn = compile(src, false, true);
        ck(mo.get() && mn.get(), "[C4] regalloc-in-loop compiles (off + regalloc)");
        if (mo && mn) {
            // 0+1+...+4999 = 4999*5000/2 = 12497500
            ck(call0_i64(*mo, "main") == 12497500, "[C4] flags-off value: loop sum == 12497500");
            ck(call0_i64(*mn, "main") == 12497500, "[C4] regalloc  value: loop sum == 12497500 (r10 claim/release per iter)");
        }
    }
    {
        // Chain of BinExprs: ((a+b)+c)+d — three outermost BinExprs, each claims
        // r12 in turn (the previous released it).
        const char* src = "fn main() -> i64 { let a: i64 = 1; let b: i64 = 2; let c: i64 = 3; let d: i64 = 4; return a + b + c + d; }\n";
        auto mo = compile(src, false, false);
        auto mn = compile(src, false, true);
        ck(mo.get() && mn.get(), "[C5] chained a+b+c+d compiles (off + regalloc)");
        if (mo && mn) {
            ck(call0_i64(*mo, "main") == 10, "[C5] flags-off value: 1+2+3+4 == 10");
            ck(call0_i64(*mn, "main") == 10, "[C5] regalloc  value: 1+2+3+4 == 10 (sequential r12 claims)");
        }
    }
    {
        // Regalloc with a runtime arg (the value depends on the input x): proves
        // the regalloc holds the RIGHT lhs (the actual x, not a stale r12) across
        // a RHS that reads another local.
        const char* src =
            "fn f(x: i64) -> i64 { let k: i64 = 7; return x + k * 3; }\n"
            "fn main() -> i64 { return f(100) + f(-5); }\n";
        auto mo = compile(src, false, false);
        auto mn = compile(src, false, true);
        ck(mo.get() && mn.get(), "[C6] runtime-arg regalloc compiles (off + regalloc)");
        if (mo && mn) {
            // f(100)=100+21=121; f(-5)=-5+21=16; main=121+16=137
            ck(call0_i64(*mo, "main") == 137, "[C6] flags-off value: f(100)+f(-5) == 137");
            ck(call0_i64(*mn, "main") == 137, "[C6] regalloc  value: f(100)+f(-5) == 137");
        }
    }
    {
        // BinExpr with a SIGNED Div/Mod in the RHS: emit_integer_divmod's signed
        // overflow check uses r10 (mov r10, INT64_MIN), so a signed-div-bearing
        // RHS clobbers r10 — the regalloc must fall back to push/pop (the stack
        // holds lhs across the divmod). This pins the r10-clobber-conservatism: a
        // wrong regalloc here would corrupt lhs (rax restored from a clobbered r10).
        const char* src =
            "fn main() -> i64 { let a: i64 = 1000; let b: i64 = 7; return a + b % 3; }\n";
        auto mo = compile(src, false, false);
        auto mn = compile(src, false, true);
        ck(mo.get() && mn.get(), "[C7] BinExpr with signed-mod-in-rhs compiles (off + regalloc)");
        if (mo && mn) {
            ck(call0_i64(*mo, "main") == 1001, "[C7] flags-off value: 1000+7%3 == 1001");
            ck(call0_i64(*mn, "main") == 1001, "[C7] regalloc  value: 1000+7%3 == 1001 (push/pop fallback — signed mod clobbers r10)");
        }
    }
    {
        // BinExpr with a local-SLICE index in the RHS: the IndexExpr slice-base
        // path holds the slice ptr in r10 across the index eval, so it clobbers
        // r10 — the regalloc must fall back to push/pop. Pins the local-slice-base
        // r10-clobber case (the array_lit [8] regression: a wrong regalloc here
        // corrupts lhs because rax is restored from a clobbered r10).
        const char* src =
            "fn main() -> i64 { let s: i64[] = [256, 1000]; return s[0] + s[1]; }\n";
        auto mo = compile(src, false, false);
        auto mn = compile(src, false, true);
        ck(mo.get() && mn.get(), "[C8] BinExpr with local-slice-index-in-rhs compiles (off + regalloc)");
        if (mo && mn) {
            ck(call0_i64(*mo, "main") == 1256, "[C8] flags-off value: s[0]+s[1] == 1256");
            ck(call0_i64(*mn, "main") == 1256, "[C8] regalloc  value: s[0]+s[1] == 1256 (push/pop fallback — local slice base clobbers r10)");
        }
    }

    // ======================================================================
    // Part D: BOTH flags on together (the full Stage 1). The peephole runs
    // AFTER the regalloc's tree-walker emit; they compose. Value preserved.
    // ======================================================================
    std::printf("\n--- both flags on (peephole + regalloc compose) ---\n");
    {
        const char* src =
            "fn main() -> i64 { let mut s: i64 = 0; let mut i: i64 = 1; "
            "while (i < 1000) { s = s + i * 2 + 1; i = i + 1; } return s; }\n";
        auto mo = compile(src, false, false);
        auto mn = compile(src, true,  true);
        ck(mo.get() && mn.get(), "[D1] both-flags-on compiles (off vs peephole+regalloc)");
        if (mo && mn) {
            // s = sum_{i=1}^{999} (2i+1) = 2*sum(i) + 999 = 2*(999*1000/2) + 999 = 999000 + 999 = 999999
            ck(call0_i64(*mo, "main") == 999999, "[D1] flags-off value: loop == 999999");
            ck(call0_i64(*mn, "main") == 999999, "[D1] both-on   value: loop == 999999 (peephole+regalloc compose)");
        }
    }

    // ======================================================================
    // Part E: the regalloc uses the VOLATILE r10, so there is NO callee-saved
    // save/restore to test (zero prologue tax by design — a volatile holding
    // register needs no save/restore, unlike a callee-saved one which would
    // need per-function save/restore and could net worse on call-heavy code).
    // The value assertion exercises the regalloc round-trip.
    // ======================================================================
    std::printf("\n--- volatile holding register (r10, no prologue tax) ---\n");
    {
        // A function that uses the regalloc (BinExpr) and returns; r10 is volatile
        // so the C caller is unaffected (a caller doesn't keep state in a volatile
        // across a call). The value assertion exercises the regalloc round-trip.
        const char* src =
            "fn work(a: i64, b: i64) -> i64 { return a + b + a + b; }\n"
            "fn main() -> i64 { return work(3, 4); }\n";
        auto mn = compile(src, false, true);
        ck(mn.get(), "[E1] regalloc'd fn compiles");
        if (mn) {
            ck(call0_i64(*mn, "main") == 14, "[E1] regalloc'd fn returns correct value (volatile r10 round-trip)");
        }
    }

    std::printf("\ncodegen_opt_test: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
