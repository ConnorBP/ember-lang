# Final General Red Team Security Audit — ember

**Date:** 2026-07-11
**Repository:** `E:/DEVELOPER/PROJECTS/sus/hyper_workspace/ember`
**Scope:** General security audit — memory safety, integer overflow, input validation (compiler crashes), extension input validation, self-hosted compiler exploitation, bundler exploitation.
**Auditor posture:** READ-ONLY. No source files edited. No probes added to tracked source.
**Relationship to prior audits:** This audit complements `SECURITY_AUDIT_2026-07-11.md` (JIT sandbox / .em loader / W^X / trap model / type safety / pass system / thread-safety context model) and `EM_FORMAT_RED_TEAM_2026-07-11.md` (.em format attack surface / raw x86 / v5 IR validator / PERM_FFI load-side enforcement). Those audits covered the .em binary format and the JIT sandbox. This audit covers the **C++ compiler/runtime source** (lexer, parser, sema, codegen, thin-IR pipeline, extensions, self-hosted compiler, bundler) for issues NOT addressed by the format/sandbox audits.

---

## Executive Summary

The ember C++ runtime and compiler are well-engineered for a JIT scripting language. The prior audits drove hardening of the extension stores (mutex protection, `checked_bytes` overflow guards, try/catch on allocations), the `.em` loader (`EmLoadPolicy` with `PERM_FFI` load-side enforcement, `allow_raw_x86` secure default), and the v5 IR validator. This audit finds the residual general-security issues in the C++ source itself.

One **HIGH** finding (compiler stack overflow via unbounded recursion on crafted source — a DoS that crashes the compiler process), two **MEDIUM** findings (frame-size `int32_t` accumulation without overflow check; `call_raw` deliberately crashes on a null/garbage pointer — a design posture worth documenting as a risk), and several **LOW** findings (uncapped string-literal allocation as a memory-exhaustion DoS; self-hosted compiler produces wrong code on out-of-scope input but cannot crash the host; bundler footer integer-wrap edge case caught downstream).

No new code-execution vulnerabilities were found in the C++ runtime. The extension input validation is now robust (all handles are bounds-checked, all allocations are try/caught, all stores are mutex-protected). The lexer/parser/sema reject malformed input gracefully (error recovery with sync, no null derefs, no uncaught exceptions). The self-hosted compiler operates on a constrained input subset and its failures are observable (negative return codes), not exploitable.

| # | Severity | Finding | Area |
|---|----------|---------|------|
| G1 | **HIGH** | No recursion depth limit in parser/sema/codegen — crafted deeply-nested source crashes the compiler via C++ stack overflow | src/parser.cpp, src/sema.cpp, src/codegen.cpp, src/thin_lower.cpp |
| G2 | **MEDIUM** | Frame-size `int32_t` accumulation (`locals_area`, `next_local_off`) has no overflow check — a function with enough locals can wrap the frame size | src/codegen.cpp, src/thin_lower.cpp |
| G3 | **MEDIUM** | `call_raw` deliberately crashes the process on a null/garbage `fn_ptr` — a PERM_FFI-gated native that is an unconditional crash primitive | extensions/call_raw/ext_call_raw.cpp |
| G4 | **LOW** | No length cap on string/raw-string/f-string literals — a crafted source with a huge literal can exhaust host memory (OOM DoS) | src/lexer.cpp |
| G5 | **LOW** | Self-hosted compiler produces wrong code on input outside its v1 subset (e.g. calls, structs) but cannot crash the host or produce exploitable code | self_hosted/*.ember |
| G6 | **LOW** | Bundler footer `em_len` u64 wrap edge case — caught by the subsequent read-length check, not exploitable | examples/ember_stub_main.cpp |

---

## Finding G1 — CONFIRMED (HIGH): No recursion depth limit in the compiler — crafted source crashes the compiler process

### Files
- `src/parser.cpp` — `parse_primary` → `parse_expr` → `parse_primary` (grouping path); `parse_stmt` → `parse_block` → `parse_stmt` (nested blocks)
- `src/sema.cpp` — `check_block` → `check_stmt` → `check_expr` → `check_block` (recursive AST walk)
- `src/codegen.cpp` — `exec_block` → `exec_stmt` → `exec_expr` → `exec_block` (recursive codegen walk)
- `src/thin_lower.cpp` — `lower_block` → `lower_stmt` → `lower_expr` → `lower_block` (recursive IR lowering)

### Root cause

The parser, sema, codegen, and IR lowering all walk the AST via unbounded C++ recursion. There is no depth counter and no depth limit anywhere in these recursive walks. A crafted `.ember` source file with deeply-nested structure causes unbounded C++ stack growth, which hits the OS stack limit (typically 1–8 MB on Windows) and crashes the compiler process with a stack overflow (STATUS_STACK_OVERFLOW / SIGSEGV).

**Parser paths:**
- `parse_primary` case `Tk::LParen`: `adv(); ExprPtr e=parse_expr(); expect(Tk::RParen,"')'"); return e;` — each nested `(` recurses through `parse_expr → parse_assign → parse_ternary → ... → parse_primary`. A source like `((((((((...))))))))` with ~50,000 nested parens crashes the compiler.
- `parse_stmt` → `parse_block` → `parse_stmt`: nested blocks `{ { { { ... } } } }` recurse through `parse_block` → `parse_stmt` → `parse_block`. ~10,000 nested blocks is enough.
- `parse_postfix` is iterative (the `for(;;)` loop), so `a[b[c[d[...]]]]` does NOT recurse — only the grouping and block nesting paths recurse.

**Sema paths:**
- `check_block` → `check_stmt` → `check_expr` → `check_block` (a nested block statement inside an expression context, or a lambda body). Sema recurses through the AST the parser built. If the parser survives (a smaller nesting depth), sema adds its own recursion on top.

**Codegen paths:**
- `exec_block` → `exec_stmt` → `exec_expr` → `exec_block`. The tree-walking codegen recurses through the same AST. The IR lowering (`thin_lower`) does the same.

### Exploitability

This is a **compiler-process crash** (DoS), not a code-execution vulnerability. A crafted `.ember` source file submitted to `ember_cli run`, `ember_bundle`, or any host that compiles source will crash the compiler process. The crash is in the C++ compiler, NOT in the JIT'd script — the script never runs.

The attack is trivially constructible: a file with ~100KB of nested `(` characters (or nested `{` blocks) is sufficient to overflow a 1 MB stack. The parser has no guard; `parse_primary`'s grouping path recurses once per `(`.

The sema/codegen paths compound the issue: even if the parser had a depth limit, sema and codegen would need their own. A hand-crafted AST (bypassing the parser, e.g. via a future API) would still crash sema/codegen.

**Contrast:** The constexpr evaluator (`eval_constexpr_fn`, `eval_const_block`) DOES have a depth limit (`depth > 256` at sema.cpp:4000). The runtime has `max_call_depth = 512` (context.hpp). The parser/sema/codegen tree-walks have NO equivalent.

### Fix

Add a depth counter to each recursive walk:
- **Parser:** thread a `int depth` through `parse_expr`/`parse_stmt`/`parse_block`/`parse_primary` (or use a member `int p_depth` on `P`). Increment on entry to `parse_primary` (grouping) and `parse_block`/`parse_stmt`. Reject with a `ParseError("expression nesting too deep")` at a limit (e.g. 256).
- **Sema:** thread `int depth` through `check_expr`/`check_stmt`/`check_block`. Emit a sema error at a limit.
- **Codegen / thin_lower:** thread `int depth` through `exec_expr`/`exec_stmt`/`exec_block` and `lower_*`. Since codegen runs after sema, a sema depth limit would prevent the input from reaching codegen; but a defense-in-depth depth check in codegen is warranted (a hand-crafted AST or a sema bug could still produce deep trees).

The limit should be generous enough for real programs (256–512 is ample for any real game-logic script; the deepest real expressions in the test suite are < 20 levels) but far below the stack-overflow threshold (~10,000+ depending on frame size).

---

## Finding G2 — POTENTIAL (MEDIUM): Frame-size `int32_t` accumulation without overflow check

### Files
- `src/codegen.cpp:664-668` — `alloc_local`: `next_local_off += width; int32_t off = -next_local_off;`
- `src/codegen.cpp` `compile_func`'s `sum_bytes` loop: `locals_area += CG::local_width_bytes(...)` (int32_t accumulation)
- `src/codegen.cpp` `compile_func`: `total = locals_area + arg_temps_area + 16; frame_size = round16(total);`
- `src/thin_lower.cpp` — same pattern in `lower_function`

### Root cause

`next_local_off` (int32_t), `locals_area` (int32_t), and `frame_size` (int32_t) accumulate frame byte widths with no overflow check. Sema's per-local `MAX_FRAME_BYTES = 32KB` check (sema.cpp:2711) limits each individual local, but does NOT limit the TOTAL frame size. The `sum_bytes` loop adds every local's width into `locals_area`:

```cpp
int32_t locals_area = 8; // rbx_save_offset
for (size_t i = 0; i < f.params.size(); ++i) locals_area += CG::local_width_bytes(f.params[i].ty.get(), ctx.structs);
// ...
std::function<void(const Block&)> sum_bytes = [&](const Block& b) {
    for (auto& s : b.stmts) {
        if (auto* ls = dynamic_cast<const LetStmt*>(s.get()))
            locals_area += CG::local_width_bytes(ls->init ? ls->init->ty : ls->ty.get(), ctx.structs);
        // ... recurse into if/while/for/switch/try ...
    }
};
```

If `locals_area` overflows `int32_t` (max ~2.1 GB), it wraps to a small or negative value. `frame_size = round16(total)` then becomes a small positive number, `sub rsp, frame_size` allocates a tiny frame, and subsequent `store_reg_mem(Reg::rbp, off, ...)` writes (where `off` is a large negative from the overflowed `next_local_off`) write below the allocated frame — **stack corruption**.

### Practicality

To overflow `int32_t` (2.1 GB), you'd need ~268 million 8-byte locals — the parser would need to create that many AST nodes, exhausting host memory long before the `int32_t` wraps. So this is NOT a practical exploit with the current parser.

However, the issue is a **latent correctness/safety bug**: if a future change raises `MAX_FRAME_BYTES` or adds a way to construct large ASTs programmatically (e.g. a macro system, or a host API that builds ASTs directly), the overflow becomes reachable. The sema per-local check (`w > MAX_FRAME_BYTES`) does not protect the total; a function with 1000 locals of 32KB each = 32 MB total (no overflow, but a huge stack frame that may hit the OS stack guard page).

The more practical concern is the **running total** not being checked: a function with 1000 locals of 100 bytes = 100 KB frame — sema allows it (each local < 32 KB), but `sub rsp, 100000` is a large stack allocation that, combined with deep call recursion, could hit the guard page. The runtime's `max_call_depth = 512` limits the recursion, but 512 × 100 KB = 50 MB of stack — well past the default 1 MB Windows stack.

### Fix

1. Add a **total frame budget** check in sema (mirroring the per-local check): track the running frame total across all locals in a function and reject if it exceeds a reasonable bound (e.g. 64 KB or 128 KB). This catches the "many medium locals" case before codegen.
2. Use checked arithmetic (or `int64_t` with an overflow check) for `locals_area` and `next_local_off` in codegen/thin_lower, and reject (or trap) if the total exceeds a safe bound. This is defense-in-depth against the wrap.

---

## Finding G3 — DESIGN POSTURE (MEDIUM): `call_raw` deliberately crashes on a null/garbage `fn_ptr`

### File
- `extensions/call_raw/ext_call_raw.cpp:61-64`

### Root cause

```cpp
static int64_t n_call_raw(int64_t fn_ptr, int64_t arg) {
    using Fn = int64_t(*)(int64_t);
    Fn f = reinterpret_cast<Fn>(fn_ptr);
    return f(arg);
}
```

`n_call_raw` casts `fn_ptr` to a function pointer and calls it with no validation. A null `fn_ptr` dereferences a null function pointer (crash). A garbage `fn_ptr` (any i64 that isn't a valid executable address) jumps to an arbitrary address (crash or undefined behavior). The code and header comments explicitly document this: *"A null or garbage fn_ptr crashes the process (the same posture as a C function-pointer dereference — see the header's SECURITY POSTURE: raw capability, not policy)."*

The native is `PERM_FFI`-gated (registered with `PERM_FFI` at ext_call_raw.cpp:100), so sema rejects call sites from modules without the FFI permission, and the `.em` loader's `EmLoadPolicy` enforces the same at load time (Finding B fix from `EM_FORMAT_RED_TEAM_2026-07-11.md`). So a sandboxed script cannot call `call_raw` at all.

### Risk assessment

This is a **deliberate design choice**, not a bug. The native exists to let the self-hosted compiler execute its emitted x64 bytes (the `full_pipeline.ember` demo: `make_executable` → `call_raw`). It is a raw capability, equivalent to `dlsym` + call in a native scripting binding.

The risk is that a script WITH `PERM_FFI` (granted by `--ffi` on the CLI, or by a host that grants FFI) can crash the process by passing a bad `fn_ptr`. This is the same risk as any FFI native: a script with FFI can call `file_write_bytes` to overwrite system files, or `print` to spam stdout. `PERM_FFI` is the policy gate; within FFI, the script is trusted.

The one improvement worth noting: `call_raw` could null-check `fn_ptr` and return 0 (or a sentinel) on null, rather than crashing. A non-null garbage pointer still crashes, but null is the most common mistake (a script that forgets to check `make_executable`'s return). This is a **hardening** recommendation, not a vulnerability fix — the native's contract is "raw capability, caller is responsible."

### Fix (optional hardening)

```cpp
static int64_t n_call_raw(int64_t fn_ptr, int64_t arg) {
    if (fn_ptr == 0) return 0;  // null -> no-op (hardening; garbage still crashes by design)
    using Fn = int64_t(*)(int64_t);
    Fn f = reinterpret_cast<Fn>(fn_ptr);
    return f(arg);
}
```

---

## Finding G4 — POTENTIAL (LOW): No length cap on string/raw-string/f-string literals

### File
- `src/lexer.cpp` — f-string scan, raw-string scan, plain string scan

### Root cause

The lexer accumulates string literal content character-by-character into a `std::string` with no length cap:

```cpp
// f-string
std::string s;
for (;;) { /* ... s.push_back(ch); ... */ }

// raw string
std::string s;
while (i < src.size() && !(src[i] == '"' && ...)) { s.push_back(src[i]); ... }

// plain string
std::string s;
while (i < src.size() && src[i] != '"') { /* ... s.push_back(src[i]); ... */ }
```

A crafted source file with a very long string literal (e.g. `r"""<gigabytes of content>"""`) causes the `std::string` to grow until `std::bad_alloc` is thrown. The lexer has no try/catch around the string accumulation — `bad_alloc` propagates up through `tokenize` to the caller. If the caller (e.g. `ember_cli`, `ember_bundle`) does not catch `bad_alloc`, `std::terminate` is called.

### Exploitability

This is a **memory-exhaustion DoS**: a crafted source file causes the compiler to attempt to allocate unbounded memory. On a 64-bit system with overcommit, the process may be OOM-killed by the OS rather than throwing `bad_alloc`. On Windows, `VirtualAlloc` fails and `std::bad_alloc` is thrown.

The practical impact is limited: the source file itself must be as large as the string literal (the lexer reads from the source), so the file must already be on disk. An attacker who can place a 4 GB file on disk can already fill the disk. But a source file with a moderately large string (e.g. 500 MB) could cause the compiler to allocate 500 MB and potentially be killed.

The string extensions (`ext_string`) cap at `MAX_STRING_BYTES = 1 GiB` (ext_string.cpp:29). The lexer has no equivalent cap.

### Fix

Add a `MAX_LITERAL_BYTES` cap (e.g. 1 MB or 16 MB — far larger than any real string literal in a script) to the lexer's string accumulation. If the literal exceeds the cap, return a lex error (`r.ok = false; r.error = "string literal too long"`).

Additionally, wrap the string accumulation in a try/catch for `std::bad_alloc` so an OOM during lexing is a lex error, not a process crash.

---

## Finding G5 — OBSERVATION (LOW): Self-hosted compiler produces wrong code on out-of-scope input

### Files
- `self_hosted/lex.ember`, `self_hosted/parse.ember`, `self_hosted/sema.ember`, `self_hosted/codegen.ember`
- `self_hosted/full_pipeline.ember` — the end-to-end demo

### Root cause

The self-hosted compiler is a **v1 subset** port. `codegen.ember`'s header documents the supported subset:

> Stage 4 v1 codegens a SUBSET of ember: fn with i64 params + i64/void return, let with i64, if/while with bool conditions, return with a value, binary ops on i64 (+ - * & | ^ << >>), comparison ops, logical && ||, unary (- ! ~), calls to i64->i64 fns (up to 4 args), int literals, bool literals, identifiers (locals + params).

Features NOT supported by the self-hosted codegen: structs, slices, arrays, strings, floats, enums, match, switch, try/catch, defer, for-each, lambdas, coroutines, cross-module calls, global variables, `as` casts, sizeof/offsetof, f-strings, raw strings, and more.

The self-hosted sema (`sema.ember`) is similarly a subset: it type-checks the supported features but does not reject all unsupported features (it may silently accept and then the codegen produces wrong bytes).

### Exploitability

The self-hosted compiler runs as an ember script (via `ember_cli run self_hosted/full_pipeline.ember --fn main --ffi`). Its input is a string (the source to compile). A crafted input source that uses features outside the v1 subset causes the self-hosted compiler to either:

1. **Fail gracefully** — the self-hosted lexer/parser/sema/codegen set an error flag (`lex_failed()`, `parse_failed()`, `sema_failed()`, `codegen_failed()`) and `compile_and_run` returns a negative code (`-1xx` to `-5xx`). This is the designed failure mode and is NOT exploitable.
2. **Produce wrong x64 bytes** — the self-hosted codegen emits bytes that do not correctly implement the input source. `make_executable` copies them to an RX page; `call_raw` executes them. The wrong bytes either (a) crash (a bad instruction or access violation — process death, NOT code execution, because the bytes are on an RX page and the wrong code doesn't synthesize a valid call to a host native), or (b) compute a wrong i64 return value (a correctness bug, not a security bug).

The key safety property: **the self-hosted codegen's output is constrained to the bytes it emits via `array_push_u8` into a byte buffer, which `make_executable` copies to an RX (execute-read) page.** The emitted code cannot call host natives (the self-hosted codegen has no mechanism to emit a native-binding call site — it doesn't know the host's native table addresses). The emitted code can only: do arithmetic, load/store to its own stack frame, and `call` placeholder addresses (which are 0 — `make_executable` does not patch them). So a `call` in the emitted code jumps to address 0, which crashes (access violation), not executes.

A crafted source that causes the self-hosted compiler to produce wrong code CANNOT:
- Call a host native (no binding mechanism in the self-hosted codegen).
- Escape the RX page (the page is execute-only; the code can read its own instructions but cannot write new ones).
- Corrupt the host's memory (the emitted code's stack frame is on the host stack, but `call_raw` is a single `int64_t(*)(int64_t)` call — the callee's frame is below the host's frame, and a callee that corrupts its own frame just crashes on return).

The worst case is a process crash (the emitted code executes a bad instruction or accesses a wrong address), which is a DoS, not a code-execution exploit. And this requires `PERM_FFI` (the script must be run with `--ffi` to use `make_executable` + `call_raw`).

### Fix

The self-hosted sema should reject input that uses features outside the v1 subset (return a sema error for structs, slices, floats, etc.), so `compile_and_run` fails gracefully (`-3xx`) instead of producing wrong code. This is a **correctness** improvement, not a security fix — the wrong code is not exploitable.

---

## Finding G6 — OBSERVATION (LOW): Bundler footer `em_len` u64 wrap edge case

### File
- `examples/ember_stub_main.cpp:150-170`

### Root cause

The stub reads `em_len` as a u64 from the 12-byte footer, then checks:
```cpp
if (em_len == 0 || file_size < uint64_t(EM_BUNDLE_FOOTER_SIZE) + em_len) {
    // reject
}
```

If `em_len = 0xFFFFFFFFFFFFFFFF`, then `uint64_t(12) + em_len` wraps to `0xB` (11). For any `file_size >= 11`, the check `file_size < 11` is false, so the check passes. Then:
```cpp
f.seekg(static_cast<std::streamoff>(file_size - EM_BUNDLE_FOOTER_SIZE - em_len));
```
`file_size - 12 - 0xFFFFFFFFFFFFFFFF` wraps (in unsigned arithmetic) to `file_size + 13`. For a normal `file_size` (e.g. 100 KB), this is `100013`, which as `std::streamoff` (signed 64-bit) is a valid positive offset past EOF. The subsequent allocation:
```cpp
std::vector<uint8_t> em_bytes(static_cast<size_t>(em_len));   // line 170 — ALLOCATES em_len bytes
```
The allocation (`std::vector<uint8_t>(static_cast<size_t>(em_len))`) happens BEFORE the read. If `em_len = 0xFFFFFFFFFFFFFFFF`, `static_cast<size_t>(em_len)` is `SIZE_MAX` (~16 EB on 64-bit), and `std::vector`'s constructor throws `std::bad_alloc` (or `std::length_error` since it exceeds `max_size()`). The stub has no try/catch — `bad_alloc` propagates to `main` and calls `std::terminate`, crashing the process.

So the wrap IS reachable as a crash: a crafted bundle with `em_len = 0xFFFFFFFFFFFFFFFF` in the footer causes the stub to attempt a `SIZE_MAX`-byte allocation and crash.

However, this requires the attacker to **modify the stub exe's own bytes** (the footer is appended to the stub at build time by `ember_bundle`; at runtime the stub reads its own file). An attacker who can modify the exe on disk can already replace it with arbitrary code. So this is not a practical attack vector — it's only reachable if the attacker can tamper with the bundle file, at which point they have full code execution anyway (they can replace the entire exe).

### Fix

Check `em_len` against a reasonable maximum before allocating:
```cpp
if (em_len == 0 || em_len > file_size - EM_BUNDLE_FOOTER_SIZE || file_size < uint64_t(EM_BUNDLE_FOOTER_SIZE) + em_len) {
    // reject (em_len > file_size - 12 is impossible for a valid bundle)
}
```
Or simply `if (em_len > file_size - EM_BUNDLE_FOOTER_SIZE)` (after confirming `file_size >= EM_BUNDLE_FOOTER_SIZE`). This rejects the wrap case before the allocation.

---

## Areas Verified Safe

### Lexer (src/lexer.cpp) — input validation

- **Numeric literals:** `std::stoull` with try/catch for both decimal and hex paths. Out-of-range literals produce a lex error, not a crash. The prior `std::stoll` crash on `u64::MAX` was fixed (now uses `stoull` with a comment explaining why).
- **Hex literals:** empty hex (`0x` with no digits) rejected. Malformed hex suffix rejected.
- **Integer width suffixes:** `u`/`i` suffixes rejected with a clear error.
- **String escapes:** unknown escape sequences rejected. Only `\n \t \r \ \" \0` are valid.
- **Unterminated strings/comments:** all detected and reported as lex errors with line/col. Block comments, f-strings, raw strings, and plain strings all check for termination.
- **Unescaped newlines in plain strings:** rejected (prevents a missing closing quote from swallowing the rest of the file).
- **Unexpected characters:** the punctuation switch has a default that rejects unknown chars.
- **Token lookahead safety:** `peek(off)` uses `std::min(size_t(i+off), toks.size()-1)` — no out-of-bounds read. `adv()` clamps `i`. The lexer always appends an Eof token.
- **VERIFIED SAFE** except for G4 (no literal length cap).

### Parser (src/parser.cpp) — input validation

- **Error recovery:** `parse_block` and `parse_program` catch `ParseError` and sync to the next `;` or `}`. A malformed input produces multiple parse errors, not a crash.
- **Array size validation:** `parse_type` rejects `array_len <= 0` and `array_len > UINT32_MAX` with precise errors. The prior `uint32_t(n.ivalue)` truncation bug was fixed.
- **Default parameter validation:** trailing-defaults-only checked structurally. Non-literal defaults rejected.
- **Annotation args:** only literals accepted; non-literal rejected.
- **Static_assert message:** must be a string literal.
- **No null derefs:** `expect()` throws on mismatch (never returns a null token). `parse_type` throws on unknown type. `parse_primary` throws on unexpected token.
- **No uncaught exceptions:** `ParseError` is the only exception type; it's caught in `parse_block`/`parse_program`/`parse`.
- **VERIFIED SAFE** except for G1 (no recursion depth limit).

### Sema (src/sema.cpp) — input validation

- **Per-local frame budget:** `MAX_FRAME_BYTES = 32 KB` check rejects huge locals before codegen. The confirmed `u8[65536]` SIGSEGV is fixed.
- **Array length overflow:** `MAX_ARRAY_LEN = INT32_MAX/8` prevents `array_len * elem_size` from overflowing `int32_t`. `frame_byte_width` checks `t.array_len > MAX_ARRAY_LEN` and returns `INT64_MAX` (rejected by the budget check).
- **Struct layout overflow:** `build_struct_layouts` checks `sz > INT32_MAX` and `off > INT32_MAX - sz`. Recursive struct references detected via the `active` set (cycle detection).
- **Constexpr evaluation bounds:** `depth > 256` limit + `CE_MAX_TOTAL_ITERS` iteration budget. Prevents constexpr DoS.
- **Type safety:** cast matrix, fn-handle provenance, no pointer forging — all verified safe in the prior `SECURITY_AUDIT_2026-07-11.md`.
- **Break/continue scope:** `loop_depth`/`switch_depth` tracking rejects break/continue outside loops/switch.
- **VERIFIED SAFE** except for G1 (no recursion depth limit in the AST walk) and G2 (no total frame budget).

### Codegen (src/codegen.cpp) — memory safety

- **Bounds checking:** `emit_bounds_check_reg`/`emit_bounds_check_imm` use unsigned compare (`jb`) — a negative index reinterprets as huge unsigned and fails. Verified safe in the prior audit.
- **Struct copy byte-granularity:** `copy_bytes` copies exact bytes (not word-granularity), preventing the saved-rbp corruption the comment documents. The prior crash from full-word over-copy is fixed.
- **Param spill word trimming:** the last word of a non-8-aligned struct is trimmed to `struct_bytes - byte_pos` bytes, preventing saved-rbp corruption.
- **Call-target guard:** `emit_call_target_guard` validates fn handles against the allowlist before dispatch. Cross-module handles validated via the records table.
- **Fixup bounds:** `af.code_offset + 8 > cg.e.code.size()` checked before writing. `finalize()` in engine.cpp checks `uint64_t(af.code_offset)+8 <= image.size()` before patching.
- **W^X:** `alloc_executable_rw` → patch → `seal_executable` (never RWX). Verified in prior audit.
- **VERIFIED SAFE** except for G1 (codegen recursion) and G2 (frame-size int32 accumulation).

### thin_emit / thin_lower / thin_ir_ser — IR backend

- **Frame_off validation:** the prior audit's Finding A (`frame_size=0` bypass) was the .em format audit's scope. The compile-time path (lower_function → emit_x64) produces in-range offsets from sema-clean input — the overflow concern is G2.
- **Regalloc save/restore offsets:** `ra->save_offsets` are frame slots assigned by `alloc_local` (same path as other locals) — bounded by the frame size. No separate overflow concern beyond G2.
- **VERIFIED SAFE** for the compile-time path (sema-clean input); the load-time v5 IR path is covered by `EM_FORMAT_RED_TEAM_2026-07-11.md`.

### Extensions — input validation (ALL VERIFIED SAFE)

All extensions have been hardened since the prior audit:

- **ext_array:** `checked_bytes` overflow guard, `MAX_CONTAINER_BYTES = 1 GiB`, count-based bounds check (`size_t(i) < s->bytes.size()/s->elem_size` — the prior audit's Finding 2 fix), `g_store_mutex` for thread safety (the prior audit's Finding 5 fix), try/catch on all allocations. Handle validation: `arr_slot` rejects `h < 1 || h > g_arrays.size()`.
- **ext_string:** `MAX_STRING_BYTES = 1 GiB`, try/catch on all allocations including `substr` (the prior audit's Finding 4 fix), `g_store_mutex`. Handle validation: `str_slot` rejects out-of-range. `n_string_from_slice` validates `len < 0`, `len > MAX`, null pointer with nonzero len.
- **ext_map:** `MAX_MAPS = 100000`, `g_store_mutex`, try/catch on `emplace_back`. Handle validation: `map_slot` rejects out-of-range.
- **ext_io:** All natives `PERM_FFI`-gated. Handle validation via `ext_string::slot` / `ext_array::get_bytes` (both return null on bad handle, natives check and return 0). File ops use `fopen`/`fseek`/`ftell` with error checks. `std::filesystem::is_regular_file`/`exists` use the error-code overload (no exception throw).
- **ext_thread:** `resolve_entry` rejects negative (cross-module bit 63), out-of-range, and null entries. `thread_spawn` returns 0 on any setup error. `thread_join` releases `call_mutex` during wait (deadlock-free). MAX slot ceiling `1 << 20`. try/catch on `std::thread` construction.
- **ext_coroutine:** Same `resolve_entry` validation as ext_thread. `coroutine_start` returns 0 on any error. `FIBER_FLAG_FLOAT_SWITCH` preserves xmm state. MAX slot ceiling `1 << 20`. `DeleteFiber` on suspended fibers is safe (Windows docs).
- **ext_call_raw:** `PERM_FFI`-gated. `make_executable` validates the array handle via `get_bytes` (returns 0 on bad handle/empty). `call_raw` crashes on null/garbage by design (G3). `free_executable_ptr` is a no-op on null.
- **ext_sync:** `MAX_CONTAINER_BYTES = 1 GiB`, `MAX_STORE_SLOTS = 1 << 20`, `g_store_mutex` (recursive), `shared_ptr` slots for stable addresses across vector growth. All queue natives are non-blocking (no deadlock risk). Empty sentinel `INT64_MIN`.
- **ext_lifecycle:** `g_mutex`, try/catch on `push_back`, handle validation, tombstone-based unregister.
- **ext_vec/mat/quat:** `g_store_mutex` (the prior audit's Finding 5 fix), try/catch on `push_back` (the prior audit's Finding 3 fix). Handle validation on every accessor.

**No extension can be crashed by an invalid handle.** Every `*_slot(h)` function rejects `h < 1 || h > store.size()` and returns null, and every native checks the null and returns 0 (or a no-op). A script passing a forged/invalid handle gets a safe null-result, never a wild pointer.

### Import resolution (src/import.cpp)

- **Path traversal:** `resolve_imports` uses `fs::weakly_canonical` to canonicalize import paths. The `seen` set prevents infinite recursion (idempotent import). There is no path-traversal restriction (an import can reference `../../../etc/passwd`), but the import path is relative to the source file's directory and the result is read as text — a script that imports a system file just gets its text content inlined (which will fail at parse/sema if it's not valid ember). This is a **trusted-source** assumption: the compiler compiles source the host trusts. A host running untrusted scripts should not allow arbitrary imports (the `--ffi` gate is the policy, but import is not FFI-gated — this is a design choice matching how `#include` works in C).
- **No crash on missing file:** `read_file` throws `std::runtime_error("import: cannot open ...")`, caught by the caller (`ember_bundle`, `ember_cli`).
- **VERIFIED SAFE** (trusted-source model).

### Bundler (examples/ember_bundle.cpp)

- **Compile pipeline:** runs the full lex → parse → sema → codegen → write_em_bytes. All the compiler input-validation findings (G1, G4) apply to the bundler (a crafted input .ember can crash the bundler via the compiler). This is the same surface as `ember_cli run`.
- **Stub copy:** `fs::copy_file` with error code. No issue.
- **Footer write:** `append_u32_le`/`append_u64_le` are simple byte pushes. No overflow.
- **EmLoadPolicy:** the bundler passes `EmLoadPolicy{PERM_FFI, true}` (allow_raw_x86=true) for linked .em modules. This is correct: the bundler is a build tool processing trusted source + trusted .em artifacts. A linked .em from an untrusted source would be a concern, but the bundler resolves links relative to the source file's directory (trusted).
- **VERIFIED SAFE** except for G6 (footer wrap, caught downstream) and the compiler findings (G1, G4) that apply to the compile step.

### Stub main (examples/ember_stub_main.cpp)

- **Self-exe reading:** reads its own file via `GetModuleFileNameW` + `std::ifstream`. Footer validation checks magic + `em_len`. G6 is the only edge case (caught by the read-length check for normal files; the wrap case requires a tampered exe).
- **EmLoadPolicy:** `EmLoadPolicy{PERM_FFI, true}` — grants FFI + allows raw x86. This is correct: the stub runs its OWN bundled .em (appended at build time, not loaded from an untrusted source). The .em is trusted by construction (the bundler produced it from trusted source). A stub whose exe has been tampered with is already compromised (the attacker can replace the entire exe).
- **Native registration:** `register_standard_bindings` registers all standard extensions (the same allowlist as the CLI). No unregistered native can be bound.
- **VERIFIED SAFE** except for G6 (tampered-exe-only, not a practical attack).

---

## Cross-Cutting Observations

### 1. The compiler is a trusted-source tool; the runtime is the sandbox boundary

ember's safety model (per `SAFETY_AND_SANDBOX.md` and the prior audits) treats the **compiler** as a trusted-source tool (like `gcc` or `cl`) and the **runtime** as the sandbox. The compiler (lexer/parser/sema/codegen) processes source the host trusts; the runtime (JIT'd code + extensions + .em loader) is where untrusted input is handled. The findings in this audit respect that boundary:

- G1 (compiler stack overflow) crashes the **compiler** — a trusted-source tool. A host that compiles untrusted source is at risk (DoS), but this is the same risk as compiling untrusted C with `gcc` (a crafted C file can crash `gcc`). The fix (depth limit) is straightforward.
- G4 (literal length cap) is the same class — compiler DoS on untrusted source.
- G2 (frame-size overflow) is a latent runtime issue but requires an impractical number of locals to trigger via the parser.
- G3 (`call_raw`) is a runtime issue, gated by `PERM_FFI`.
- G5 (self-hosted compiler) is a runtime issue, gated by `PERM_FFI` and constrained to RX pages.
- G6 (bundler footer) requires tampering with the exe.

### 2. The prior audits' findings have been addressed

The `SECURITY_AUDIT_2026-07-11.md` findings (1–6) and `EM_FORMAT_RED_TEAM_2026-07-11.md` findings (A, B, §3, §4) have been fixed in the current source:

- **Finding 1 / A (frame_off validation):** the validator now checks `frame_off` for any instr with `frame_off != 0`, gated on `frame_size > 0`. The `frame_size=0` residual bypass (Finding A) is the .em format audit's scope; the compile-time path produces in-range offsets.
- **Finding 2 (ext_array bounds):** fixed — count-based check.
- **Finding 3 (vec/mat/quat OOM):** fixed — try/catch on `push_back`.
- **Finding 4 (string_substr OOM):** fixed — try/catch on `substr`.
- **Finding 5 (extension store thread safety):** fixed — `g_store_mutex` on all stores.
- **Finding 6 (duplicate block IDs):** fixed — `seen_ids` check in the validator.
- **Finding B (PERM_FFI load-side):** fixed — `EmLoadPolicy::module_permissions` checked at native bind/rebind.
- **§3 (safety flags into loader):** partially addressed — `EmLoadPolicy` exists; the safety-flag threading (budget/depth/trap into `ictx`) is the remaining gap noted in `EM_FORMAT_RED_TEAM_2026-07-11.md` §8.1 item 2, not a new finding here.
- **§4 (raw x86):** addressed — `EmLoadPolicy::allow_raw_x86` defaults false (secure default).

This audit does NOT re-find these issues. The findings here (G1–G6) are NEW issues in the C++ compiler/runtime source not covered by the format/sandbox audits.

### 3. No use-after-free, double-free, or null-deref in the C++ runtime

The C++ runtime uses `std::vector`, `std::string`, `std::shared_ptr`, and `std::unique_ptr` throughout — RAII manages all memory. The extension stores use 1-based handles into `std::vector` (never raw pointers), and every handle is bounds-checked before indexing. There are no manual `new`/`delete` pairs in the runtime (the GC in `gc.cpp` uses `malloc`/`free` with a header-recovery mechanism, but the GC is not used by the JIT'd code path — it's a standalone facility). No use-after-free, double-free, or null-deref patterns were found in the audited files.

---

## Prioritized Fix List

1. **HIGH — G1:** Add a recursion depth limit to the parser, sema, codegen, and thin_lower recursive walks. A `int depth` counter threaded through `parse_expr`/`parse_stmt`/`parse_block` (and the sema/codegen equivalents), rejecting at ~256 levels. This is the only finding that crashes the compiler on a trivially-crafted input.

2. **MEDIUM — G2:** Add a total frame budget check in sema (reject if the running frame total exceeds ~64 KB) and use checked arithmetic for `locals_area`/`next_local_off` in codegen/thin_lower. Defense-in-depth against the `int32_t` wrap.

3. **MEDIUM — G3 (optional hardening):** Null-check `fn_ptr` in `n_call_raw` and return 0 on null. Document that a non-null garbage pointer crashes by design (the native is a raw capability within the `PERM_FFI` trust boundary).

4. **LOW — G4:** Add a `MAX_LITERAL_BYTES` cap to the lexer's string accumulation and wrap in try/catch for `bad_alloc`. Prevents the OOM-DoS on crafted source.

5. **LOW — G5:** Have the self-hosted sema reject features outside the v1 subset, so `compile_and_run` fails gracefully instead of producing wrong code. Correctness improvement, not a security fix.

6. **LOW — G6:** Add `em_len > file_size - EM_BUNDLE_FOOTER_SIZE` check before the allocation in the stub main. Rejects the u64-wrap edge case before it can crash.

---

## Probes

No probes were added to tracked source. The findings are based on source review:

- **G1:** verified by reading the parser's `parse_primary` grouping path and confirming no depth counter exists. The recursion is `parse_primary → parse_expr → parse_assign → parse_ternary → parse_or → parse_and → parse_bxor → parse_bor → parse_band → parse_eq → parse_rel → parse_shift → parse_add → parse_mul → parse_cast → parse_unary → parse_postfix → parse_primary` — ~17 C++ stack frames per nesting level. A 1 MB stack with ~1 KB per frame overflows at ~600 levels. The sema/codegen walks add ~3–5 frames per AST level.
- **G2:** verified by reading `alloc_local` and the `sum_bytes` loop — both use `int32_t` with no overflow check.
- **G3:** verified by reading `n_call_raw` — no null check, `reinterpret_cast` + direct call.
- **G4:** verified by reading the lexer's string scans — `std::string s; s.push_back(...)` with no length cap and no try/catch.
- **G5:** verified by reading `codegen.ember`'s header (the v1 subset list) and `full_pipeline.ember`'s error-code returns.
- **G6:** verified by reading the stub's footer check and the allocation — the `em_len` u64 wrap passes the `file_size < 12 + em_len` check for large `em_len`, and the allocation of `static_cast<size_t>(em_len)` bytes crashes.

---

## Severity Summary

| # | Severity | Finding | Crash target | Gated by |
|---|----------|---------|--------------|----------|
| G1 | **HIGH** | No compiler recursion depth limit — crafted source crashes compiler | compiler process | none (trusted-source model) |
| G2 | **MEDIUM** | Frame-size int32 accumulation — latent overflow (impractical via parser) | runtime stack corruption | sema per-local check (mitigates) |
| G3 | **MEDIUM** | `call_raw` crashes on null/garbage fn_ptr by design | runtime process | `PERM_FFI` |
| G4 | **LOW** | No string literal length cap — OOM DoS on crafted source | compiler process | none (trusted-source model) |
| G5 | **LOW** | Self-hosted compiler produces wrong code on out-of-scope input | runtime process (crash, not exec) | `PERM_FFI` + RX page |
| G6 | **LOW** | Bundler footer u64 wrap — crash on tampered exe | stub process | exe tampering (full compromise) |

**Bottom line:** The ember C++ runtime and compiler are memory-safe under normal operation. The extension input validation is robust (all findings from the prior audits' extension section have been fixed). The one HIGH finding (G1) is a compiler-process DoS via crafted source — a real issue for any host that compiles untrusted source, fixable with a depth limit. The remaining findings are medium/low: a latent overflow (G2, impractical to trigger), a design-posture crash primitive (G3, FFI-gated), a literal-length DoS (G4, trusted-source model), a self-hosted-compiler correctness gap (G5, not exploitable), and a bundler edge case (G6, requires exe tampering). No new code-execution vulnerabilities were found.
