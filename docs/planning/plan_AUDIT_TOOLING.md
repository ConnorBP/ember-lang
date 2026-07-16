# Audit Tooling + Coverage Gap Analysis

**Status: IMPLEMENTED THROUGH PHASE 4; re-audited 2026-07-15.** The tree now
has GitHub Actions build/test, sanitizer, cppcheck, clang-tidy, CodeQL, and
libFuzzer jobs; local static-analysis, DrMemory, and gcov scripts; `.em`
loader/lexer/parser fuzz harnesses plus a Windows corpus batch driver; binary
hardening flags; and focused coverage tests. Current coverage is reported as
approximately 85%+ by the project state; generated reports remain
machine-specific and are not checked in. TSAN and commercial/cloud scanners
remain optional future work.

**Original date:** 2026-07-13
**Author:** Investigation into (1) gaps in our current audit coverage and (2) static/dynamic analysis tools we could adopt.
**Build env:** MinGW g++ 15.2.0, C++17, Windows-first, Ninja, x86-64 JIT compiler.

---

## Part 1 — Current coverage map

We have **35 audit reports** in `docs/audit/` and **7 recurring schedules** (s8–s14). All audits are **LLM-based** (sub-agent reads source, reports findings, implements permitted fixes). At the original audit revision, no automated static or dynamic analysis tooling was in the build. This is now superseded by the status block above.

### What the recurring schedules cover

| Schedule | Interval | Scope |
|---|---|---|
| s8 hourly maintenance | 1h | Correctness, security, code quality, compiler warnings, doc accuracy — implements small fixes |
| s9 performance review | 4h | Benchmarks, IR coverage, regalloc spills, pass effectiveness |
| s10 IR review | 6h | Thin IR model, lowering, emission, serialization |
| s11 security review | 8h | .em loader, sandbox, GC, raw execution, threads, FFI gates |
| s12 improvement cycle | 3h | Audit→plan→implement→review for deferred work |
| s13 todo progress | 2h | Tracks deferred todo items, implements one |
| s14 release candidate | 3h | Milestone/release gates, version bumps |

### What the 35 audit reports cover
- **Correctness:** code review, self-hosted correctness, opt pass correctness
- **Security:** .em red-team, sandbox revalidation, GC/raw/threads, attack surface sweep
- **Performance:** speed audit, pass impact benchmark, regalloc
- **Docs/tests:** docs review, completeness, type stress
- **Architecture:** arch design, synthesis

### What we recently added (incident post-mortem)
- `src/safety.{hpp,cpp}`: RSS memory cap (abort at 2 GiB), `DepthGuard` recursion cap, `deadline_expired()`
- GC/JIT alloc RSS checks, test-runner `timeout`, `--load-em` protection, compiler recursion guards, bench deadlines, pass IR-size cap

---

## Part 2 — Gap analysis (what we DON'T systematically check)

Ranked by severity. Our LLM audits are good at *finding* issues by reading code, but they are **not systematic** — they can miss a bug class entirely if no agent happens to look at the right pattern.

| # | Gap | Severity | Why it matters | Currently checked? |
|---|---|---|---|---|
| G1 | **Memory safety (UAF/OOB/double-free/heap corruption)** | 🔴 HIGH | JIT compiler + GC + hot-reload = classic UAF/OOB territory. The RSS cap only catches *unbounded growth*, not corruption. | LLM reads code only — no runtime detection |
| G2 | **Undefined behavior (signed overflow, uninit reads, strict-aliasing)** | 🔴 HIGH | Frame-size `int32_t` accumulators (audit found these), IR sizes, .em lengths. UB is silent corruption. | No |
| G3 | **Concurrency data races** | 🔴 HIGH | thread extension + hot-reload + GC + in-context threads. `call_mutex` is coarse; races in extension stores were already found (Sec-5). | LLM review only — no race detector |
| G4 | **Input fuzzing (lexer/parser/.em loader on malformed input)** | 🔴 HIGH | `.em` loader deserializes untrusted binary — the #1 attack surface. Lexer/parser on pathological input caused the stack-overflow incident. | Some manual edge cases in tests; no systematic fuzzing |
| G5 | **Resource leaks (executable pages, file handles)** | 🟡 MED | Audit found leaked `CompiledFn` exec pages in 4 harnesses. Fixed manually but no automated leak detector. | LLM only |
| G6 | **Code coverage** | 🟡 MED | We don't know which code paths are never exercised. 67 ctest + 274 lang tests but no coverage % measurement. | No |
| G7 | **Binary hardening (stack canaries, ASLR/DEP/CFG, NX)** | 🟡 MED | We ship a JIT compiler that emits executable memory. PE hardening matters for the shipped `ember_cli.exe` + bundled exes. | No |
| G8 | **Static taint analysis on .em loader** | 🟡 MED | Untrusted input → policy enforcement. Manual review found raw-x86 bypass (fixed). No automated taint tracking. | LLM only |
| G9 | **Compiler warnings as errors** | 🟢 LOW | We run warning audits but don't build with `-Werror` or a strict warning set in CI. | Intermittent LLM audits |
| G10 | **Supply chain / dependency audit** | 🟢 LOW | We vendor VST3 SDK 3.8.0 (MIT) + ed25519. License verified manually. No automated SBOM/CVE scan. | One-time manual |
| G11 | **License compliance scanning** | 🟢 LOW | AGPL + commercial dual license. Manual check. | One-time manual |

---

## Part 3 — Tool research (verified for MinGW/Windows, not assumed)

### KEY FINDING: MinGW GCC does NOT support sanitizers on Windows

**Confirmed via research:** GCC's libsanitizer only targets Linux (`x86_64-*-linux*`, etc.). The `configure.tgt` in libsanitizer has no Windows case. Attempting `-fsanitize=address` with MinGW g++ produces `cannot find -lasan`. This is a **long-standing limitation** with no fix as of GCC 15.

**This is the single most important constraint.** The standard ASAN/UBSAN/TSAN workflow is NOT available with our current toolchain (MinGW g++ 15.2.0).

### Tool-by-tool verdict

| Tool | Catches | Works on MinGW g++ Windows? | Effort | Verdict |
|---|---|---|---|---|
| **ASAN** (AddressSanitizer) | UAF, OOB, heap corruption, leaks | ❌ **NO** with GCC. ✅ YES with LLVM-MinGW (Clang) or clang-cl | Medium — requires switching compiler for sanitizer builds | **ADOPT (via Clang)** — see Part 4 |
| **UBSAN** (UndefinedBehaviorSanitizer) | signed overflow, null deref, uninit, shift OOB | ❌ NO with GCC on Windows. ✅ YES with LLVM-MinGW/Clang | Low (flag-only once Clang is available) | **ADOPT (via Clang)** |
| **TSAN** (ThreadSanitizer) | data races | ⚠️ Partial — TSAN on Windows is experimental even with Clang. Linux-only for GCC. | High — unreliable on Windows | **MAYBE / defer** — test on Linux port if/when available |
| **LSAN** (LeakSanitizer) | memory leaks | Part of ASAN — same Windows limitation for GCC. Works with Clang ASAN. | Low with ASAN | **ADOPT (via Clang ASAN)** |
| **MSAN** (MemorySanitizer) | uninit reads | Linux-only | — | **SKIP** (not on Windows) |
| **cppcheck** | Static: null deref, OOB, resource leaks, some UB | ✅ YES — cross-platform, works on MinGW source | Low — standalone, no compiler switch | **ADOPT** — cheap, no toolchain change |
| **clang-tidy** | Static: bugprone, cert, cppcoreguidelines, performance, misc | ✅ YES — needs a `compile_commands.json` (CMake exports this with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`) | Medium — configure checks, suppress noise | **ADOPT** — high value static checks |
| **Clang/LLVM-MinGW** (alt compiler) | Enables ASAN/UBSAN/LSAN on Windows | ✅ YES — LLVM-MinGW is a complete MinGW-w64 + Clang toolchain for Windows, explicitly supports ASAN + UBSAN | High — second build config, but ABI-compatible with GCC MinGW | **ADOPT** — this is the unlock for sanitizers |
| **PVS-Studio** | Static: deep dataflow, 64-bit, UB, security | ✅ YES (commercial, free trial / open-source license for OSS) | Medium — license setup | **MAYBE** — try the free OSS license |
| **Coverity Scan** | Static: deep dataflow | ✅ YES (free for OSS, cloud-based) | Medium — upload builds | **MAYBE** — free for OSS |
| **SonarQube/SonarCloud** | Static + metrics + security hotspots | ✅ YES (SonarCloud free for OSS) | Medium — CI integration | **MAYBE** |
| **CodeQL** (GitHub) | Static: variant analysis, security queries | ✅ YES — GitHub-native, C++ supported | Medium — workflow setup | **ADOPT** — free, GitHub-native, good for security |
| **Semgrep** | Static: pattern-based security rules | ✅ YES — C++ supported, fast | Low — rules + CI | **ADOPT** — cheap security rules |
| **libFuzzer** | In-process fuzzing | ⚠️ Works with Clang (not GCC). Needs LLVM-MinGW. | High — write harnesses | **ADOPT (via Clang)** — critical for .em loader |
| **AFL++** | Out-of-process fuzzing | ⚠️ Windows port exists (AFL++ on Windows via Cygwin/WSL). Clang preferred. | High | **MAYBE** — libFuzzer is easier first |
| **DrMemory** | Dynamic memory checker (Windows-native) | ✅ YES — runs on Windows, works on MinGW binaries | Low — run binary under DrMemory | **ADOPT** — no compiler switch needed! |
| **Valgrind** | Dynamic memory + helgrind (races) | ❌ Linux only | — | **SKIP** (not on Windows; revisit on Linux port) |
| **DynamoRIO / Pin** | Instrumentation frameworks | ✅ DynamoRIO works on Windows | High | **SKIP** — overkill for now |
| **gcov / llvm-cov** | Code coverage | ✅ gcov with GCC (`-fprofile-arcs -ftest-coverage`), llvm-cov with Clang | Low — flags + report tool | **ADOPT** — know our coverage % |
| **GitHub Actions CI** | Automated build + test + analysis on push | ✅ YES — runs on Windows runners | Medium — workflow file | **ADOPT** — we have no CI at all |

---

## Part 4 — Concrete adoption plan (prioritized, phased)

### Phase 1 — Zero toolchain change — DONE

**1a. cppcheck** — static analysis, no compiler switch
```bash
# Install: pacman -S mingw-w64-x86_64-cppcheck  (MSYS2) or download from cppcheck.org
cppcheck --enable=warning,performance,portability,style --inline-suppr \
         --suppress=missingIncludeSystem -I src -I include src/ extensions/ examples/ 2>&1 | tee cppcheck.txt
```
Catches: null deref, OOB, resource leaks, some UB, style issues. Runs on our existing GCC source. **Adopt immediately.**

**1b. clang-tidy** — static analysis, needs compile_commands.json
```bash
# CMakeLists.txt: add option
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Run:
clang-tidy -p buildt --checks='-*,bugprone-*,cert-*,cppcoreguidelines-*,performance-*,misc-*,clang-analyzer-*' \
          $(find src extensions -name '*.cpp') 2>&1 | tee clang-tidy.txt
```
Needs a Clang install (MSYS2: `pacman -S mingw-w64-x86_64-clang-tools-extra`). Does NOT require switching our compiler — just needs the compile DB. **Adopt immediately** (start with `bugprone-*` + `clang-analyzer-*` checks, expand later).

**1c. DrMemory** — dynamic memory checker, runs on our existing MinGW binaries
```bash
# Download from dynamorio.org/drmemory, then:
drmemory -- buildt/ember_cli.exe run tests/lang/optimization_validation.ember --fn main
drmemory -- buildt/ember_cli.exe test tests/lang
```
Catches: UAF, OOB, double-free, uninit reads — **on Windows, with our existing GCC build, no compiler change.** This is our fastest path to runtime memory-safety detection. **Adopt immediately.**

**1d. gcov coverage** — know what we're not testing
```bash
# Add a coverage build config:
cmake -G Ninja -DCMAKE_CXX_FLAGS="-fprofile-arcs -ftest-coverage" -DCMAKE_EXE_LINKER_FLAGS="-lgcov" ..
# Build, run ctest, then:
gcovr --root . --html-details coverage.html
```
Tells us coverage %. **Adopt immediately** — we don't even know our current coverage.

**1e. Semgrep** — cheap security rules
```bash
# pip install semgrep; or use the Semgrep CI app
semgrep --config=p/c --config=p/security-audit src/ extensions/
```
Fast pattern-based security scan. **Adopt immediately.**

### Phase 2 — GitHub Actions CI — DONE

Create `.github/workflows/ci.yml`:
- **Job 1 (Windows + MinGW g++):** the build we already do — `cmake -G Ninja`, build, `ctest -E bench -LE soak`. This catches regressions on every push.
- **Job 2 (cppcheck):** run Phase 1a, fail on new findings.
- **Job 3 (clang-tidy):** run Phase 1b, fail on `bugprone-*` / `cert-*`.
- **Job 4 (Semgrep):** run Phase 1e.
- **Job 5 (CodeQL):** GitHub-native code scanning — `github/codeql-action/init@v2` with `cpp` language. Free, runs in CI.

We have **no CI at all** right now — every audit is a manually-dispatched agent. CI on push is the single biggest process gap. **Adopt next.**

### Phase 3 — Sanitizer builds — DONE in Linux CI (the reliable supported configuration)

Install **LLVM-MinGW** (Clang-based MinGW toolchain, ABI-compatible with GCC MinGW) from mingw-w64.org/downloads or MSYS2 (`pacman -S mingw-w64-x86_64-clang`). Add a second CMake build config:
```bash
# Sanitizer build (Clang):
cmake -G Ninja -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" ..
cmake --build . -j 8
ctest -E bench -LE soak    # runs all tests under ASAN+UBSAN
```
This unlocks:
- **ASAN** → UAF, OOB, heap corruption, double-free (G1)
- **UBSAN** → signed overflow, null deref, uninit, shift OOB (G2)
- **LSAN** (part of ASAN) → memory leaks (G5)

Run this as a CI job (don't ship the sanitizer build — it's a test-only config). **This is the highest-value single change** — it catches the entire memory-safety + UB gap with one toolchain.

### Phase 4 — Fuzzing — DONE for `.em` loader, lexer, and parser entry surfaces

With Clang available, add **libFuzzer** harnesses for the untrusted-input surfaces:

**4a. .em loader fuzzer** (G4 — security-critical):
```cpp
// fuzz_em_loader.cpp
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Write data to a temp .em file, call load_em_file, expect either
    // clean load or clean rejection — no crash, no UB.
    load_em_file_from_bytes(data, size, ...);
    return 0;
}
```
Compile: `clang++ -fsanitize=fuzzer,address fuzz_em_loader.cpp -lember`. The `.em` loader is our #1 attack surface — fuzzing it is the highest-value fuzz target.

**4b. lexer/parser fuzzer** (G4):
```cpp
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string src(reinterpret_cast<const char*>(data), size);
    auto lr = ember::tokenize(src, "<fuzz>");
    if (lr.ok) { auto pr = ember::parse(std::move(lr.toks)); }
    return 0;  // no crash expected on any input
}
```
The incident (stack overflow from deep nesting) would have been caught by a parser fuzzer + the recursion guards we just added.

**4c. IR pass pipeline fuzzer** (pass correctness on weird IR):
Generate random ThinFunctions, run the pass pipeline, assert the result is value-preserving (the optimization_validation.ember = 177 invariant, but on generated IR).

### Phase 5 — Deferred / lower-priority

- **PVS-Studio / Coverity / SonarCloud** — try the free OSS tier, evaluate against cppcheck+clang-tidy; adopt if they find things the free tools miss.
- **TSAN** — revisit when the Linux port lands (TSAN is reliable on Linux).
- **Binary hardening** — add `-fstack-protector-strong`, verify `/DYNAMICBASE /NXCOMPAT` (MinGW linker defaults), check CFG. Low effort, do as a CMake flags pass.
- **Supply chain** — `cargo audit`-equivalent doesn't exist for vendored C++ deps; manual SBOM is the realistic option.

---

## Part 5 — Fuzzing targets (specific entry points)

| Target | File | Input | What it catches | Priority |
|---|---|---|---|---|
| `.em` loader | `src/em_loader.cpp` `load_em_file` | Arbitrary bytes as a .em blob | Malformed-input crashes, validation bypasses, OOB reads | 🔴 #1 |
| Lexer | `src/lexer.cpp` `tokenize` | Arbitrary bytes as source | Tokenization panics, infinite loops, OOB | 🟡 #2 |
| Parser | `src/parser.cpp` `parse` | Arbitrary/fuzzed token streams | Stack overflow (now guarded), malformed AST | 🟡 #3 |
| Sema | `src/sema.cpp` `sema` | Fuzzed AST from parser | Crash on pathological types, deep recursion | 🟢 #4 |
| IR passes | `extensions/opt/ext_obf.cpp` | Random ThinFunctions | Non-value-preserving transforms, non-convergence | 🟢 #5 |

---

## Part 6 — CI recommendation (GitHub Actions sketch)

```yaml
# .github/workflows/ci.yml
name: ci
on: [push, pull_request]
jobs:
  build-test:        # Windows + MinGW g++ (our supported path)
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4
        with: { submodules: true }
      - uses: msys2/setup-msys2@v2
        with: { msystem: MINGW64, install: g++, ninja, cmake }
      - shell: msys2 {0}
        run: |
          cmake -G Ninja -DCMAKE_CXX_COMPILER=g++ -B build
          cmake --build build -j 8
          cd build && ctest -E bench -LE soak --timeout 60

  cppcheck:
    runs-on: ubuntu-latest
    steps: [checkout, apt: cppcheck, run: cppcheck --enable=warning,style src/ extensions/]

  clang-tidy:
    runs-on: ubuntu-latest
    steps: [checkout, cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON, clang-tidy -p build src/*.cpp]

  semgrep:
    runs-on: ubuntu-latest
    steps: [checkout, uses: returntocorp/semgrep-action@v1]

  codeql:
    runs-on: ubuntu-latest
    permissions: { security-events: write }
    steps:
      - uses: github/codeql-action/init@v3
        with: { languages: cpp }
      - uses: github/codeql-action/analyze@v3

  sanitizer:         # Clang ASAN+UBSAN (Phase 3)
    runs-on: windows-latest
    steps: [msys2 setup, install: clang, cmake -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined", build, ctest]
```

---

## Summary — what to do first

1. **cppcheck** (now, zero toolchain change) — static analysis on our GCC source
2. **DrMemory** (now, zero toolchain change) — runtime memory checks on our GCC binaries
3. **clang-tidy** (now, just needs Clang tools installed) — static bug-finder
4. **gcov coverage** (now) — know what we're not testing
5. **GitHub Actions CI** (next) — run build+test+cppcheck+semgrep+CodeQL on every push
6. **Clang sanitizer build** (next) — ASAN+UBSAN+LSAN via LLVM-MinGW → closes G1, G2, G5
7. **libFuzzer on .em loader** (then) — closes G4, the #1 security gap

The biggest insight: **our MinGW g++ toolchain cannot do sanitizers on Windows, but LLVM-MinGW (Clang) can — and it's ABI-compatible.** Adding a Clang sanitizer build config is the single highest-value change. DrMemory is the fastest zero-toolchain-change path to runtime memory detection today.
