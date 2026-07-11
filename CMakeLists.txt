# ember - canonical language build (RESTRUCTURE_PLAN.md Section 2).
#
# Defines the three ember libraries (ember, ember_frontend, ember_import) and
# the two .em/import round-trip test executables. Consumed by prism via
# add_subdirectory() - prism links ember/ember_frontend/ember_import and gets
# their PUBLIC include dirs (src/) transitively. The obfuscation features
# (string encryption, MBA, CPUID gate) are part of the language codegen and
# ship here; prism wires the host-side natives (__str_decrypt etc.).

cmake_minimum_required(VERSION 3.16)
project(ember VERSION 1.0.0 LANGUAGES CXX)
# The vendored ed25519 (thirdparty/ed25519) is C99; enable the C language so
# CMake links it correctly (F2 .em content authentication). ember itself stays C++17.
enable_language(C)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    set(CMAKE_C_STANDARD 99)
    set(CMAKE_C_STANDARD_REQUIRED ON)
    set(CMAKE_C_EXTENSIONS OFF)
endif()

if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "MSVC x64 not yet supported; use MinGW")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()

# ---- vendored Ed25519 (F2 .em content authentication, docs/spec/SPEC_AUDIT_2026-07-10.md F2) ----
# orlp/ed25519 (public domain, see thirdparty/ed25519/license.txt) — the small,
# portable Ed25519 implementation ember links for `.em` signature sign/verify.
# Compiled as C99 (the sources are C; g++ compiles them in C++ mode as .c via
# language-per-source). ED25519_NO_SEED excludes seed.c's CSPRNG (wincrypt on
# Windows — a platform coupling the audit asked ember to avoid for portability);
# the test harness uses a DETERMINISTIC 32-byte seed so a tamper test is
# reproducible, and production hosts that want a CSPRNG seed provide their own
# 32-byte seed to ember::ed25519::keypair_from_seed.
add_library(ember_ed25519 STATIC
    thirdparty/ed25519/fe.c
    thirdparty/ed25519/ge.c
    thirdparty/ed25519/sc.c
    thirdparty/ed25519/sha512.c
    thirdparty/ed25519/keypair.c
    thirdparty/ed25519/sign.c
    thirdparty/ed25519/verify.c
    thirdparty/ed25519/add_scalar.c
    thirdparty/ed25519/key_exchange.c
)
target_include_directories(ember_ed25519 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/ed25519)
target_compile_definitions(ember_ed25519 PUBLIC ED25519_NO_SEED)
if(NOT MSVC)
    target_compile_options(ember_ed25519 PRIVATE -Wno-sign-compare -Wno-unused-parameter -Wno-unused-variable -Wno-missing-field-initializers)
endif()

# ---- core runtime lib: JIT memory, engine, .em serialize/load, module registry ----
add_library(ember STATIC
    src/jit_memory.cpp
    src/engine.cpp
    src/em_writer.cpp
    src/em_loader.cpp
    src/module_registry.cpp
)
target_include_directories(ember PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ember PUBLIC ember_ed25519)  # F2 .em content authentication

# ---- frontend lib: lexer, types, parser, sema, codegen ----
add_library(ember_frontend STATIC
    src/lexer.cpp
    src/types.cpp
    src/parser.cpp
    src/sema.cpp
    src/codegen.cpp
    src/peephole.cpp
)
target_include_directories(ember_frontend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ember_frontend PUBLIC ember)

# ---- import lib: textual `import "path";` inclusion resolver (the bundle mechanism, BUNDLING_AND_EM_MODULES.md Section 1.2) ----
add_library(ember_import STATIC src/import.cpp)
target_include_directories(ember_import PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ---- extensions: reusable non-cheat-specific language addons (RESTRUCTURE_PLAN.md Section 6) ----
# An extension is a set of ember::NativeSig registrations + OpOverloadTable
# entries + host C++ backing, NOT a grammar/type-system change (those are
# ROADMAP Tier 1+ language features). These are the docs/ROADMAP.md Tier 0
# standard addon set relocated out of prism's prism_script_host.cpp by the Section 6
# audit (see ember/extensions/AUDIT.md): vec, quat, mat, string, array, math,
# sync, and lifecycle. Each is a self-contained TU depending only on ember's
# public headers (sema.hpp/ast.hpp) + the stdlib; no prism dependency. A
# consumer (prism, or a future second consumer) links whichever extensions
# it wants and calls the extension's register_natives/register_overloads/
# reset from its own host native-table builder. See ember/extensions/README.md.
function(ember_add_extension name src)
    add_library(ember_ext_${name} STATIC ${src})
    target_include_directories(ember_ext_${name} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/extensions/${name})
    target_link_libraries(ember_ext_${name} PUBLIC ember_frontend)
endfunction()

ember_add_extension(vec       extensions/vec/ext_vec.cpp)
ember_add_extension(quat      extensions/quat/ext_quat.cpp)
ember_add_extension(mat       extensions/mat/ext_mat.cpp)
ember_add_extension(string   extensions/string/ext_string.cpp)
ember_add_extension(array    extensions/array/ext_array.cpp)
ember_add_extension(math     extensions/math/ext_math.cpp)
ember_add_extension(sync     extensions/sync/ext_sync.cpp)
ember_add_extension(lifecycle extensions/lifecycle/ext_lifecycle.cpp)

# ---- tests: .em pre-compile round-trip + live import cross-module round-trip ----
# These link only ember + ember_frontend (no prism/prism_script_host) so they
# stay free of the prism panel-API link surface - the test functions use no
# natives/globals/structs, so an empty native table is a valid sema input.
enable_testing()

add_executable(em_roundtrip_test examples/em_roundtrip_test.cpp)
target_link_libraries(em_roundtrip_test PRIVATE ember ember_frontend)
add_test(NAME em_roundtrip COMMAND em_roundtrip_test)

add_executable(import_roundtrip_test examples/import_roundtrip_test.cpp)
target_link_libraries(import_roundtrip_test PRIVATE ember ember_frontend)
add_test(NAME import_roundtrip COMMAND import_roundtrip_test)

add_executable(em_loader_hardening_test examples/em_loader_hardening_test.cpp)
target_link_libraries(em_loader_hardening_test PRIVATE ember)
add_test(NAME em_loader_hardening COMMAND em_loader_hardening_test)

# ext_runtime_test: runtime coverage for the relocated extensions (RESTRUCTURE_PLAN.md Section 6).
# em_roundtrip/import_roundtrip call NO relocated native; this one calls the math
# extension's sqrt through the full pipeline, proving the extension relocation
# registers correctly at runtime (not just links cleanly). Links ember_ext_math
# directly (no prism_script_host) to prove the extensions are reusable outside prism.
add_executable(ext_runtime_test examples/ext_runtime_test.cpp)
target_link_libraries(ext_runtime_test PRIVATE ember ember_frontend ember_ext_math)
add_test(NAME ext_runtime COMMAND ext_runtime_test)

# ext_sync_test: cross-thread sync primitives (atomics, swap buffer, SPSC/MPSC/MPMC
# queues) per docs/plan_SYNC_QUEUES.md. Tier-1 single-thread functional coverage
# (each primitive through the full lex->parse->sema->codegen->JIT->call path) +
# Tier-2 multi-thread stress using the _host accessors (only the script thread
# calls ember; host threads produce/consume via C++ accessors -- the U2 shape,
# never two ember-calling threads on one context). Links ember_ext_sync directly.
add_executable(ext_sync_test examples/ext_sync_test.cpp)
target_link_libraries(ext_sync_test PRIVATE ember ember_frontend ember_ext_sync)
add_test(NAME ext_sync COMMAND ext_sync_test)
set_tests_properties(ext_sync PROPERTIES TIMEOUT 60)

# ext_lifecycle_test: dynamic routine registration (plan_FUNCTION_REFS.md §6).
# register_routine(&fn, data) -> id + unregister_routine(id); the host drives
# the stored routines via host_routines(). Links ember_ext_lifecycle directly.
add_executable(ext_lifecycle_test examples/ext_lifecycle_test.cpp)
target_link_libraries(ext_lifecycle_test PRIVATE ember ember_frontend ember_ext_lifecycle)
add_test(NAME ext_lifecycle COMMAND ext_lifecycle_test)

# Extension registration smoke test (RESTRUCTURE_PLAN.md Section 6): validates the
# relocated non-cheat extensions (vec/quat/mat/string/array/math/sync/lifecycle) register
# their NativeSig + OpOverloadTable entries via the extension entry points.
# Links ONLY the extension libs + ember_frontend (no prism/prism_script_host)
# - proving the extensions are pure ember and self-contained. The end-to-end
# JIT exercise of the same natives is covered by prism's ember_cli path
# (vector_math_demo.ember).
add_executable(ext_registration_test examples/ext_registration_test.cpp)
target_link_libraries(ext_registration_test PRIVATE
    ember_ext_vec ember_ext_quat ember_ext_mat ember_ext_string ember_ext_array ember_ext_math
    ember_ext_sync ember_ext_lifecycle ember_frontend)
add_test(NAME ext_registration COMMAND ext_registration_test)

# v0.4 hardening regression: pins the red-team V5 (W^X JIT memory) and
# V6 (per-frame byte budget + int32 struct-sizing overflow) mitigations.
# Self-contained (inlined payloads + a VirtualQuery W^X probe). Links the
# core vec/quat/mat/string/array/math extension libs + ember
# (for alloc_executable/VirtualQuery).
add_executable(v0_4_hardening_test examples/v0_4_hardening_test.cpp)
target_link_libraries(v0_4_hardening_test PRIVATE
    ember ember_frontend ember_import
    ember_ext_vec ember_ext_quat ember_ext_mat
    ember_ext_string ember_ext_array ember_ext_math)
if(WIN32)
    target_link_libraries(v0_4_hardening_test PRIVATE kernel32)
endif()
add_test(NAME v0_4_hardening COMMAND v0_4_hardening_test)

# v1.0 context thread-safety (Option D + B1): pins the per-call context-register
# (r14) indirection so one compiled body serves N per-thread context_t's, each
# thread's trap longjmp'ing to ITS OWN checkpoint. Mirrors v0_4's harness but
# compiles with use_context_reg=true and calls via ember_call_void (r14=ctx).
add_executable(thread_safety_test examples/thread_safety_test.cpp)
target_link_libraries(thread_safety_test PRIVATE
    ember ember_frontend ember_import
    ember_ext_vec ember_ext_quat ember_ext_mat
    ember_ext_string ember_ext_array ember_ext_math)
if(WIN32)
    target_link_libraries(thread_safety_test PRIVATE kernel32)
endif()
add_test(NAME thread_safety COMMAND thread_safety_test)
set_tests_properties(thread_safety PROPERTIES TIMEOUT 30)

# Win64 ABI regressions: switch volatile scratch, raw B1 thunk r14
# preservation, and an aligned-SSE trap stub called from both stack parities.
add_executable(win64_abi_test examples/win64_abi_test.cpp)
target_link_libraries(win64_abi_test PRIVATE ember ember_frontend)
add_test(NAME win64_abi COMMAND win64_abi_test)

# v1.0 Tier 2 first-class function references: `&fn` handles + `handle(args)`
# indirect call, with the REDSHELL guard #6 (validate the runtime i64 against a
# registered-fn allowlist bitset before indexing the dispatch table, else
# BadCallTarget trap). Mirrors v0_4's harness + builds the fn allowlist.
add_executable(function_refs_test examples/function_refs_test.cpp)
target_link_libraries(function_refs_test PRIVATE
    ember ember_frontend ember_import
    ember_ext_vec ember_ext_quat ember_ext_mat
    ember_ext_string ember_ext_array ember_ext_math)
if(WIN32)
    target_link_libraries(function_refs_test PRIVATE kernel32)
endif()
add_test(NAME function_refs COMMAND function_refs_test)

# v0.5 live-modules stress matrix: bidirectional script<->.em cross-module
# linking through the real grammar (link directive + mod::fn calls). Covers
# the user's ask: scripts and .em bundles both able to use either form,
# compiled modules referencing others at runtime. Stresses the .em binary
# type in every direction. Links the six core language addons + module/em libs.
add_executable(v0_5_live_modules_test examples/v0_5_live_modules_test.cpp)
target_link_libraries(v0_5_live_modules_test PRIVATE
    ember ember_frontend ember_import
    ember_ext_vec ember_ext_quat ember_ext_mat
    ember_ext_string ember_ext_array ember_ext_math)
if(WIN32)
    target_link_libraries(v0_5_live_modules_test PRIVATE kernel32)
endif()
add_test(NAME v0_5_live_modules COMMAND v0_5_live_modules_test)

# F1 pub/priv visibility (docs/spec/SPEC_AUDIT_2026-07-10.md F1): a bundled .em
# module's exported surface is a SUBSET of its functions. `pub fn`/bare `fn`
# are exported; `priv fn` is hidden from the .em name directory (the export
# table) and a cross-module `mod::priv_fn()` call is a sema error. Non-circular:
# cases A-D fail with the F1 fix reverted (see the test's header comment for
# the per-case break). Pins the backward-compat decision (fn=public, priv=private).
add_executable(pub_priv_test examples/pub_priv_test.cpp)
target_link_libraries(pub_priv_test PRIVATE
    ember ember_frontend ember_import
    ember_ext_vec ember_ext_quat ember_ext_mat
    ember_ext_string ember_ext_array ember_ext_math)
if(WIN32)
    target_link_libraries(pub_priv_test PRIVATE kernel32)
endif()
add_test(NAME pub_priv COMMAND pub_priv_test)

# ---- F2: signed .em content authentication (docs/spec/SPEC_AUDIT_2026-07-10.md F2) ----
# A NON-CIRCULAR regression for the Ed25519-over-`.em` content-auth path:
#   - case A: a SIGNED v4 .em verifies + runs (the happy path).
#   - case B: a TAMPERED signed .em (flip a CODE byte AFTER signing) is REJECTED
#     at load with a clear "Ed25519 verification FAILED" error — NOT executed
#     (no segfault, no exec page published). This is the load-bearing assertion.
#   - case C: a signed .em whose signature block is corrupted is rejected.
#   - case D: dev-mode (no key) rejects a v4 module; signed-only mode rejects an
#     unsigned v3 module. Pins the key-management policy.
#   - case E: the unsigned v3 path still loads (backward compat — F2 is additive).
# The test links ember (which links ember_ed25519) + ember_frontend so it can
# JIT-compile a real function, serialize it SIGNED, load it back, and tamper a
# code byte in a temp copy. Mirrors pub_priv_test's emit_em shape. See
# examples/em_signed_test.cpp.
add_executable(em_signed_test examples/em_signed_test.cpp)
target_link_libraries(em_signed_test PRIVATE ember ember_frontend)
add_test(NAME em_signed COMMAND em_signed_test)

# ---- lang regression suite: ember_check (parse-only) + sema_check (parse+sema) +
# the run_lang_tests.sh runner that classifies tests/lang/{valid,invalid,sema_valid,
# sema_invalid,import_*}.ember against the two check exes. Ported from prism's
# tests/run_lang_tests.sh (lang-suite portion only); the .ember corpus under
# tests/lang/ is ported verbatim from prism/tests/lang/. The CLI run-via-CLI
# block for the import_* cases soft-fails (SKIP) if ember_cli.exe isn't built
# yet - parse/sema are the hard requirement. See tests/run_lang_tests.sh.
add_executable(ember_check examples/ember_check.cpp)
target_link_libraries(ember_check PRIVATE ember ember_frontend ember_import)

# sema_check links the six core extension libs so scripts that call array/string
# (or any standard-addon) natives sema-check against the registered set -
# EXACTLY the ext_registration_test registration shape (register_natives for
# those six, register_overloads for vec/quat/mat/string), not prism's process/
# render natives.
add_executable(sema_check examples/sema_check.cpp)
target_link_libraries(sema_check PRIVATE
    ember ember_frontend ember_import
    ember_ext_vec ember_ext_quat ember_ext_mat ember_ext_string ember_ext_array ember_ext_math)

# The suite: run the lang classifier script, passing the build dir so it finds
# ember_check.exe / sema_check.exe / ember_cli.exe. bash is required (the script
# is bash, not sh - it uses `local` and `[[ ]]`-style constructs via `set -u`).
find_program(BASH_EXE bash REQUIRED)
add_test(NAME lang_suite
    COMMAND "${BASH_EXE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_lang_tests.sh"
            "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(lang_suite PROPERTIES TIMEOUT 120)

# ---- standalone ember CLI: prism-decoupled runner ----
# A minimal host that exercises the full parse->sema->codegen->finalize->call
# pipeline (the em_roundtrip_test shape) with the full eight-extension addon
# surface registered (the ext_registration_test shape). Reads a .ember file,
# resolves `import "path";` inlining, compiles, calls the entry fn (default
# main), and exits with the i64 return code (or 0 for void). No prism linkage:
# no VFS, no backends, no proc/shader api, no pak/assets, no print natives.
add_executable(ember_cli examples/ember_cli.cpp)
target_link_libraries(ember_cli PRIVATE
    ember ember_frontend ember_import
    ember_ext_vec ember_ext_quat ember_ext_mat
    ember_ext_string ember_ext_array ember_ext_math
    ember_ext_sync ember_ext_lifecycle)

# End-to-end H12 regression: invoke the real CLI emit-em path, then load the
# artifact and inspect its nonzero initialized-global bytes.
add_executable(em_cli_emit_test examples/em_cli_emit_test.cpp)
target_link_libraries(em_cli_emit_test PRIVATE ember)
add_test(NAME em_cli_emit COMMAND em_cli_emit_test $<TARGET_FILE:ember_cli>)
set_tests_properties(em_cli_emit PROPERTIES TIMEOUT 30)

# ---- binding ABI correctness suite (BINDING_API.md Sec 4) ----
# Pins the script->native Win64 call ABI (struct-by-value arg <=8B,
# struct-by-value return >8B hidden-pointer, >4-arg stack spill, f32 in xmm
# slot-parallel, slice as ptr+len two words, mixed int+float slot-parallel)
# so a future tree-walker->SSA-IR refactor cannot silently break it. The
# codegen is already correct; this is the regression net. Registers FRESH
# host fns via BindingBuilder (src/binding_builder.hpp) + links the six core
# ember_ext_* (the vec extension's vec3_new handle-return is exercised as a
# struct-return-path comparison in test [2b], and the math extension's sqrt
# as a second f32 pin in test [4b]). Independent of the parallel extension
# refactor: ABI-under-test natives are registered here, not in extension .cpp.
add_executable(binding_abi_test examples/binding_abi_test.cpp)
target_link_libraries(binding_abi_test PRIVATE
    ember ember_frontend ember_import
    ember_ext_vec ember_ext_quat ember_ext_mat
    ember_ext_string ember_ext_array ember_ext_math)
add_test(NAME binding_abi COMMAND binding_abi_test)
# No add_test here: a smoke test needs a script asset, coordinated separately
# (the lang_suite worker owns the script-driven tests). The target building
# cleanly + `ember_cli run <file> --fn main` working IS the validation gate
# (see examples/ember_cli.cpp header for the run contract).

# Windows: ember's jit_memory uses VirtualAlloc/FlushInstructionCache.
# MinGW links kernel32 by default; MSVC pulls it via the default UCRT libs.
# Be explicit for portability when consumed by other projects.
if(WIN32)
    target_link_libraries(ember PUBLIC kernel32)
endif()

# ---- v0.6 benchmark harness: ember (native JIT) vs AngelScript (bytecode interpreter) ----
# Fetches/builds the AngelScript SDK (thirdparty/sdk/, fetched separately), compiles its
# 41 source files into a static lib, then links the bench harness that runs the same
# workload in both and reports the ratio. The categorical difference under test:
# ember compiles to native x86-64 (no dispatch-decode loop), AngelScript interprets
# bytecode. SSA-lite IR + linear-scan allocation remain a benchmark-gated future refactor.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/sdk/angelscript/source/as_atomic.cpp)
    file(GLOB AS_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/sdk/angelscript/source/*.cpp)
    add_library(angelscript STATIC ${AS_SOURCES})
    target_include_directories(angelscript PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/sdk/angelscript/include
        ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/sdk/angelscript/source)
    if(MSVC)
        target_compile_options(angelscript PRIVATE /wd4100 /wd4456 /wd4244 /wd4267)
    else()
        target_compile_options(angelscript PRIVATE -w)
    endif()

    add_executable(bench_ember_vs_as examples/bench_ember_vs_as.cpp)
    target_link_libraries(bench_ember_vs_as PRIVATE
        ember ember_frontend ember_import
        ember_ext_vec ember_ext_math
        angelscript)
    if(WIN32)
        target_link_libraries(bench_ember_vs_as PRIVATE kernel32)
    endif()
    add_test(NAME bench_ember_vs_as COMMAND bench_ember_vs_as)
    set_tests_properties(bench_ember_vs_as PROPERTIES TIMEOUT 300)

endif()

# ---- per-path codegen bench harness (docs/spec/BENCHMARK_SYSTEM_DESIGN.md) ----
# The evidence-gathering infra the codegen-optimization gate is gated on
# (docs/planning/DESIGN.md §9: "no speculative optimization before the bench
# proves it matters"; docs/spec/COMPILER_PIPELINE.md §5 defers SSA-lite IR +
# linear-scan regalloc until "stronger benchmarks show where Ember is slow").
# A CONSUMER of the existing JIT (no src/ changes), the v0_4_hardening_test /
# bench_ember_vs_as shape: compiles each path fn in BOTH safety-off and safety-on
# modes, times warmup+N-iters (fresh checkpoint per iter, min/median/p99 stats),
# and times a g++ -O2 baseline DLL doing the SAME work, reporting the ember/g++-O2
# ratio per path. The g++ -O2 baseline (bench/baseline_paths.cpp) is compiled to a
# DLL at BUILD time by the custom command below (ctest path) OR at runtime by
# bench/run_bench.sh (ad-hoc path) — either way, stale-probe discipline: from
# source each build/run, never a checked-in binary.
# PASS = ran + wrote results (the bench_ember_vs_as shape), NEVER an assertion on
# a ratio — fails only on a compile/run/IO error. TIMEOUT 600 covers the g++ -O2
# baseline compile + the 24 timed cells. See bench/run_bench.sh.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/bench/bench_codegen_paths.cpp)
    add_executable(bench_codegen_paths bench/bench_codegen_paths.cpp)
    target_link_libraries(bench_codegen_paths PRIVATE
        ember ember_frontend ember_import
        ember_ext_vec ember_ext_quat ember_ext_mat
        ember_ext_string ember_ext_array ember_ext_math)
    if(WIN32)
        target_link_libraries(bench_codegen_paths PRIVATE kernel32)
    endif()
    # Build the g++ -O2 baseline DLL from source at BUILD time (stale-probe
    # discipline: recompiled each build, never a checked-in binary). Uses the
    # same MinGW g++ CMake selected as CMAKE_CXX_COMPILER, with the anti-fold
    # flags (-fno-tree-vectorize / -fno-tree-loop-distribute-patterns) so the
    # baseline measures the scalar codegen path, not vectorization.
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/baseline_paths.dll
        COMMAND ${CMAKE_CXX_COMPILER} -O2 -std=c++17 -shared -fPIC
                -fno-tree-vectorize -fno-tree-loop-distribute-patterns
                ${CMAKE_CURRENT_SOURCE_DIR}/bench/baseline_paths.cpp
                -o ${CMAKE_CURRENT_BINARY_DIR}/baseline_paths.dll
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/bench/baseline_paths.cpp
        COMMENT "g++ -O2 baseline DLL for per-path bench"
        VERBATIM)
    add_custom_target(bench_baseline_dll
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/baseline_paths.dll)
    add_dependencies(bench_codegen_paths bench_baseline_dll)
    # PASS = ran + wrote results (the bench_ember_vs_as shape), NEVER an assertion
    # on a ratio — fails only on a compile/run/IO error. Run in the build dir so
    # the DLL path resolves + results_*.csv/.md land in the build dir (gitignored).
    add_test(NAME bench_codegen_paths COMMAND bench_codegen_paths
        "${CMAKE_CURRENT_BINARY_DIR}/baseline_paths.dll")
    set_tests_properties(bench_codegen_paths PROPERTIES TIMEOUT 600
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()

# These correctness/integration tests do not use AngelScript. Keep only the
# actual comparison benchmark conditional on SDK availability.
add_executable(v0_6_lifecycle_test examples/v0_6_lifecycle_test.cpp)
target_link_libraries(v0_6_lifecycle_test PRIVATE
    ember ember_frontend ember_import ember_ext_vec ember_ext_math)
if(WIN32)
    target_link_libraries(v0_6_lifecycle_test PRIVATE kernel32)
endif()
add_test(NAME v0_6_lifecycle COMMAND v0_6_lifecycle_test)

add_executable(v0_6_hot_reload_test examples/v0_6_hot_reload_test.cpp)
target_link_libraries(v0_6_hot_reload_test PRIVATE
    ember ember_frontend ember_import ember_ext_vec ember_ext_math)
if(WIN32)
    target_link_libraries(v0_6_hot_reload_test PRIVATE kernel32)
endif()
add_test(NAME v0_6_hot_reload COMMAND v0_6_hot_reload_test)

add_executable(game_host examples/game_host.cpp)
target_link_libraries(game_host PRIVATE ember ember_frontend ember_import)
if(WIN32)
    target_link_libraries(game_host PRIVATE kernel32)
endif()
add_test(NAME game_host_integration COMMAND game_host
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/scripts/game_logic.ember")

add_executable(float_global_regression examples/float_global_regression.cpp)
target_link_libraries(float_global_regression PRIVATE ember ember_frontend ember_import)
if(WIN32)
    target_link_libraries(float_global_regression PRIVATE kernel32)
endif()
add_test(NAME float_global_regression COMMAND float_global_regression)

# array_lit_test - pins array-literal expressions (chunk c2): fixed-array
# construction `let arr: i64[3] = [1,2,3];` and slice construction
# `let s: i64[] = [1,2,3];`. Compiles tiny .ember probes through the full
# pipeline and reads the i64 return directly via C (a non-circular direct-
# value probe - NOT an in-language comparison). A reverted LetStmt-init /
# eval() slice / sema type-bake fix turns this red. Pure language features
# (no native calls), so links only the core libs (independent of the
# extension refactor). Mirrors float_global_regression's link shape.
add_executable(array_lit_test examples/array_lit_test.cpp)
target_link_libraries(array_lit_test PRIVATE ember ember_frontend ember_import)
if(WIN32)
    target_link_libraries(array_lit_test PRIVATE kernel32)
endif()
add_test(NAME array_lit COMMAND array_lit_test)

# codegen_opt_test - pins the Stage 1 codegen-optimization peephole + local
# regalloc rewrites' VALUE EQUIVALENCE (docs/spec/CODEGEN_OPTIMIZATION_DESIGN.md §4.7).
# For each rewrite, compiles the SAME source with the Stage-1 flags OFF and ON,
# calls both, asserts identical return values. The rewritten bytes differ but
# the computed value is identical. Also pins flags-OFF = byte-identical (the
# gate's "no flag-day rewrite" discipline) and flags-ON = bytes differ where a
# rewrite applies (the flag is not a silent no-op). Links only the core libs
# (the probes are pure int arithmetic, no natives) — independent of the
# extension refactor and of F2's em-signed work.
add_executable(codegen_opt_test examples/codegen_opt_test.cpp)
target_link_libraries(codegen_opt_test PRIVATE ember ember_frontend ember_import)
if(WIN32)
    target_link_libraries(codegen_opt_test PRIVATE kernel32)
endif()
add_test(NAME codegen_opt COMMAND codegen_opt_test)

# aggregate_global_test - pins aggregate globals (chunk c3): struct globals
# `global cfg : Config = Config{...};`, fixed-array globals `global arr : i64[3]
# = [10,20,30];`, slice globals `global s : i64[] = [1,2,3];`. Compiles tiny
# .ember probes through the full pipeline and reads the i64 return directly
# via C (a non-circular direct-value probe - NOT an in-language comparison).
# A reverted fix (typed globals-block layout removed, StructLit/ArrayLit
# const-fold removed, FieldExpr/IndexExpr global-base path removed) -> a wrong
# exit code -> this test records FAIL. Also exercises the c1 interactions
# (struct global by-value arg + by-value return fall back to the global
# base) and the H12 .em round-trip for struct/array/slice globals (the slice
# global's relative-ptr makes its .em round-trip work without a loader change).
add_executable(aggregate_global_test examples/aggregate_global_test.cpp)
target_link_libraries(aggregate_global_test PRIVATE ember ember_frontend ember_import)
if(WIN32)
    target_link_libraries(aggregate_global_test PRIVATE kernel32)
endif()
add_test(NAME aggregate_global COMMAND aggregate_global_test)

set_tests_properties(em_roundtrip em_loader_hardening import_roundtrip binding_abi
    v0_6_lifecycle v0_6_hot_reload game_host_integration float_global_regression
    array_lit aggregate_global em_signed codegen_opt
    PROPERTIES TIMEOUT 30)
