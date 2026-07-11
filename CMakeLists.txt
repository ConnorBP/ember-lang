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
    src/module_registry.cpp
    # Stage A c1 (IL-.em): the thin three-address IR data structures' debug
    # pretty-printer. Lives in the CORE lib (not ember_frontend) because it
    # touches Type ONLY through public data members + the Prim enum — it never
    # calls a Type method (those live in types.cpp / ember_frontend, and ember
    # must not depend on ember_frontend: the link direction is one-way). Same
    # self-contained pattern em_writer.cpp / em_loader.cpp already follow. The
    # struct test links only `ember` and so reaches dump() directly.
    #
    # Stage B c1a: the shared .em canonical-type codec (em_type_codec.{hpp,cpp})
    # deduplicates the Type/EmSignature on-disk encoding that em_writer.cpp
    # (emit_type/emit_signature/validate_canonical_type/validate_signature) and
    # em_loader.cpp (parse_type/canonical_type_same/parse_signature) previously
    # carried as private anon-namespace copies. Same self-contained pattern: it
    # touches Type ONLY through public data members + the Prim enum and never
    # calls a Type method, so it stays in the core lib with no ember_frontend
    # dependency. Both em_writer.cpp and em_loader.cpp now #include it.
    src/thin_ir.cpp
    src/em_type_codec.cpp
    # Stage B c1b: the ThinFunction serializer/deserializer (thin_ir_ser.{hpp,cpp}).
    # Turns a ThinFunction into an opaque byte blob (the ir_blob a v5 .em
    # per-function record carries) and back. Self-contained: touches Type only
    # via em_type_codec (emit_type/parse_type) + ThinFunction public members.
    src/thin_ir_ser.cpp
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
    src/thin_lower.cpp
    src/peephole.cpp
    src/thin_emit.cpp
    # Stage B c2: em_loader.cpp moved here from the core `ember` lib because the
    # v5 IR re-emit path (deserialize_thin_function + emit_x64) needs frontend
    # symbols (emit_x64 from thin_emit.cpp, CodeGenCtx from codegen.hpp). The
    # core lib must not depend on ember_frontend (one-way link direction), so
    # the loader — which now drives the IR re-emit — lives in the frontend.
    # em_loader.hpp stays in src/ (available to both libs); only the .cpp moved.
    # em_writer.cpp stays in the core lib (it has no frontend dependencies).
    src/em_loader.cpp
    # Stage C: the composable pass system infrastructure (ember_pass.{hpp,cpp}).
    # The pass manager, type-erased PassConcept/PassModel, PreservedAnalyses,
    # CRTP EmberPassInfoMixin, and instrumentation callbacks. Passes operate on
    # ThinFunction (core) but the pass system is a frontend concern (passes run
    # at compile time over the IR produced by lower_function).
    src/ember_pass.cpp
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
ember_add_extension(map      extensions/map/ext_map.cpp)
ember_add_extension(sync     extensions/sync/ext_sync.cpp)
ember_add_extension(lifecycle extensions/lifecycle/ext_lifecycle.cpp)
ember_add_extension(opt extensions/opt/ext_opt.cpp)         # Stage C: IR optimization passes
ember_add_extension(obf extensions/obf/ext_obf.cpp)           # Stage C Step 5: IR obfuscation passes
# ext_io: OS I/O (console + file + path), core subset (docs/planning/plan_OS_IO_EXTENSIONS.md).
# Links ext_array (file_read_bytes mints + file_write_bytes reads array<u8> handles via
# get_bytes/alloc_bytes) + ext_string (print/read_line/path_* read + mint string handles
# via slot/alloc). Both are core extensions the CLI already registers; the coupling is
# one-directional (ext_io calls into them; they do not know about ext_io).
ember_add_extension(io extensions/io/ext_io.cpp)
target_link_libraries(ember_ext_io PUBLIC ember_ext_array ember_ext_string)

# ---- tests: .em pre-compile round-trip + live import cross-module round-trip ----
# These link only ember + ember_frontend (no prism/prism_script_host) so they
# stay free of the prism panel-API link surface - the test functions use no
# natives/globals/structs, so an empty native table is a valid sema input.
enable_testing()

add_executable(em_roundtrip_test examples/em_roundtrip_test.cpp)
target_link_libraries(em_roundtrip_test PRIVATE ember ember_frontend)
add_test(NAME em_roundtrip COMMAND em_roundtrip_test)

# em_bytes_test: unit test for the in-memory .em round-trip (write_em_bytes /
# load_em_bytes — the APIs the standalone exe bundler + stub use). Covers the
# happy path (compile -> serialize -> load -> run -> compare to JIT) and the
# error paths (null data, short buffer, bad magic).
add_executable(em_bytes_test examples/em_bytes_test.cpp)
target_link_libraries(em_bytes_test PRIVATE ember ember_frontend)
add_test(NAME em_bytes COMMAND em_bytes_test)

add_executable(import_roundtrip_test examples/import_roundtrip_test.cpp)
target_link_libraries(import_roundtrip_test PRIVATE ember ember_frontend)
add_test(NAME import_roundtrip COMMAND import_roundtrip_test)

add_executable(em_loader_hardening_test examples/em_loader_hardening_test.cpp)
target_link_libraries(em_loader_hardening_test PRIVATE ember ember_frontend)
add_test(NAME em_loader_hardening COMMAND em_loader_hardening_test)

# ext_runtime_test: runtime coverage for the relocated extensions (RESTRUCTURE_PLAN.md Section 6).
# em_roundtrip/import_roundtrip call NO relocated native; this one calls the math
# extension's sqrt through the full pipeline, proving the extension relocation
# registers correctly at runtime (not just links cleanly). Links ember_ext_math
# directly (no prism_script_host) to prove the extensions are reusable outside prism.
add_executable(ext_runtime_test examples/ext_runtime_test.cpp)
target_link_libraries(ext_runtime_test PRIVATE ember ember_frontend ember_ext_math)
add_test(NAME ext_runtime COMMAND ext_runtime_test)

# for_each_array_test: pins the iterable() hook's array<T> case (Tier 1) -
# `for (x in array_handle)` lowered to array_length(h) + array_get_*(h, i).
# Each probe compiles a tiny .ember through the full pipeline with the array
# extension natives registered and reads the i64 return DIRECTLY via a C
# reinterpret of the JIT entry (a non-circular direct-value probe, including a
# >8-bit full-i64 storage pin that ember_cli's 8-bit exit code can't observe).
# Links ember_ext_array directly (no prism_script_host) to prove the array
# extension + the for-each lowering are reusable outside any specific host.
add_executable(for_each_array_test examples/for_each_array_test.cpp)
target_link_libraries(for_each_array_test PRIVATE ember ember_frontend ember_ext_array)
add_test(NAME for_each_array COMMAND for_each_array_test)

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

# ext_map_test: runtime coverage for the map<K,V> host-store extension (D1).
# The map extension ships 7 natives (map_new, map_set, map_get, map_contains,
# map_length, map_remove, map_clear) but had zero ctest coverage. This test
# calls the registered native fn_ptrs directly (the ext_lifecycle_test T4/T5
# shape — the map natives are pure host-side ops over opaque i64 handles, no
# JIT round-trip needed) and asserts the host-store contract: 1-indexed
# handles, set/get/contains/length/remove/clear semantics, missing-key
# behavior, invalid-handle safety, and reset(). Links ember_ext_map directly
# (no prism/prism_script_host) — proving the extension is reusable outside prism.
add_executable(ext_map_test examples/ext_map_test.cpp)
target_link_libraries(ext_map_test PRIVATE ember ember_frontend ember_ext_map)
if(WIN32)
    target_link_libraries(ext_map_test PRIVATE kernel32)
endif()
add_test(NAME ext_map COMMAND ext_map_test)

# ext_bounds_test: runtime bounds-checking coverage for the array<T> and string
# host-store extensions (D4+D5 from docs/audit/AUDIT_2026-07-11_DOCS_TESTS.md).
# D4: array_get_u8/array_set_u8/array_get_i64/array_set_i64 index bounds
# (negative index, OOB index, index==size boundary, index==size-1 last valid);
# an OOB/negative index is a no-op on set and returns 0 on get. D5:
# string_char_at (negative / >=length -> 0) and string_substr (negative start
# or start>=length -> empty; negative len -> to end; len past end -> clamped).
# Calls the registered native fn_ptrs directly (the ext_map_test shape — the
# indexing natives are pure host-side ops over opaque i64 handles, no JIT
# round-trip needed). Links ember_ext_array + ember_ext_string directly (no
# prism/prism_script_host) — proving the extensions are reusable outside prism.
add_executable(ext_bounds_test examples/ext_bounds_test.cpp)
target_link_libraries(ext_bounds_test PRIVATE ember ember_frontend ember_ext_array ember_ext_string)
if(WIN32)
    target_link_libraries(ext_bounds_test PRIVATE kernel32)
endif()
add_test(NAME ext_bounds COMMAND ext_bounds_test)

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
    ember_ext_sync ember_ext_lifecycle ember_ext_opt ember_ext_obf ember_ext_map
    ember_ext_io)

# ---- standalone exe bundler (docs/planning/plan_STANDALONE_BUNDLER.md) ----
# The stub: a pre-built runtime that links ember + ember_frontend + the
# standard extensions (the same load path as ember_cli's --load-em, minus
# ember_import — no source imports at runtime). Its main() reads its own
# file, finds a 12-byte footer (EMBD magic + .em length), loads the appended
# .em via load_em_bytes, and calls the entry. The bundler copies this exe
# to <output.exe> + appends the .em + footer.
add_executable(ember_stub_main examples/ember_stub_main.cpp)
target_link_libraries(ember_stub_main PRIVATE
    ember ember_frontend
    ember_ext_vec ember_ext_quat ember_ext_mat
    ember_ext_string ember_ext_array ember_ext_math
    ember_ext_sync ember_ext_lifecycle ember_ext_map
    ember_ext_io)

# The bundler tool: compiles a .ember -> .em (the emit-em pipeline), copies
# the stub to <output.exe>, appends the .em + footer. Links the same libs as
# ember_cli (it runs the full compile pipeline). The OUTPUT exe (the stub)
# links only the load path.
add_executable(ember_bundle examples/ember_bundle.cpp)
target_link_libraries(ember_bundle PRIVATE
    ember ember_frontend ember_import
    ember_ext_vec ember_ext_quat ember_ext_mat
    ember_ext_string ember_ext_array ember_ext_math
    ember_ext_sync ember_ext_lifecycle ember_ext_map
    ember_ext_io)

# End-to-end bundler test: bundle .ember scripts into standalone exes, run
# them, assert exit codes. Covers simple scripts, multi-function, imports,
# globals, natives, and malformed-bundle rejection. Receives the bundler +
# stub exe paths via $<TARGET_FILE:...>.
add_executable(bundler_test examples/bundler_test.cpp)
add_test(NAME bundler_test COMMAND bundler_test
    $<TARGET_FILE:ember_bundle> $<TARGET_FILE:ember_stub_main>)
set_tests_properties(bundler_test PROPERTIES TIMEOUT 60)

# Family A: `ember test` — native test runner (classify + compile + run/sema-check
# every .ember file in tests/lang/). The CLI's built-in test command replaces
# the bash-only runner for non-bash environments; run_lang_tests.sh stays as
# the fallback (lang_suite ctest still invokes it). Runs from the source dir
# so tests/lang/ resolves + import paths work.
add_test(NAME ember_test_cli COMMAND ember_cli test ${CMAKE_CURRENT_SOURCE_DIR}/tests/lang
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(ember_test_cli PROPERTIES TIMEOUT 120)

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
        ember_ext_string ember_ext_array ember_ext_math ember_ext_opt)
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

# field_of_index_test - pins the C1 fix: `arr[i].field` on a struct array /
# slice (FieldExpr { base: IndexExpr { arr, i }, field }). Before the fix,
# both backends only handled a bare-Ident base for struct field reads; the
# tree-walker emitted nothing and the thin IR returned VReg 0, so
# `arr[0].a` silently returned 0/garbage. This probe compiles tiny .ember
# sources through the full tree-walker pipeline and reads the i64/f64
# return directly via C (non-circular direct-value probe, matching
# array_lit_test's shape). Pure language features (no native calls), so
# links only the core libs. Covers fixed-array + slice bases, zero and
# non-zero indices, zero and non-zero field offsets, and the f64 movsd
# load path.
add_executable(field_of_index_test examples/field_of_index_test.cpp)
target_link_libraries(field_of_index_test PRIVATE ember ember_frontend ember_import)
if(WIN32)
    target_link_libraries(field_of_index_test PRIVATE kernel32)
endif()
add_test(NAME field_of_index COMMAND field_of_index_test)

# constexpr_test - Tier 1 constexpr fn evaluation. A `constexpr fn` called
# with all-constant args is const-evaluated at sema time; the call site is
# rewritten to an IntLit before codegen. Covers basic fold, recursion (fib),
# constexpr in expressions / global inits, while/for loops + mutation inside
# constexpr fns, runtime fallback (non-const arg), nested constexpr calls,
# compound assignment, and `priv constexpr fn`. Pure language features (no
# natives), so links only the core libs — matching field_of_index_test's shape.
add_executable(constexpr_test examples/constexpr_test.cpp)
target_link_libraries(constexpr_test PRIVATE ember ember_frontend ember_import)
if(WIN32)
    target_link_libraries(constexpr_test PRIVATE kernel32)
endif()
add_test(NAME constexpr COMMAND constexpr_test)

# static_assert_test - Tier 1 compile-time assertion (static_assert(cond, msg)).
# A passing assertion (cond folds to true at sema time) is ELIDED (no codegen);
# a failing one (folds to false) is a sema compile error carrying msg; a
# non-constant condition is a sema compile error. Covers top-level + in-body
# positions, constexpr-fn conditions (the cross-feature fold), logical
# operators (&&/||/!) over compile-time comparisons, and non-bool / non-const
# rejections. Pure language features (no natives), so links only the core
# libs — matching constexpr_test's shape.
add_executable(static_assert_test examples/static_assert_test.cpp)
target_link_libraries(static_assert_test PRIVATE ember ember_frontend ember_import)
if(WIN32)
    target_link_libraries(static_assert_test PRIVATE kernel32)
endif()
add_test(NAME static_assert COMMAND static_assert_test)

# typed_enum_test - Tier 1 typed enums (enum E : T) + enum-from-constexpr-expr.
# `enum Color : i32 { ... }` makes Color a real type backed by i32: a
# Color-typed binding accepts Color::Variant values, rejects raw ints (int->
# enum), and widens to a backing int (enum->int, return c from an i64 fn). A
# variant's explicit value may be a constexpr fn call (resolve_enums folds it
# before the const-check). Covers typed-enum let/match/param/global, u8 + i64
# backings, enum->int implicit + explicit (as) widening, enum-from-constexpr
# (zero-arg + with-args), and the three sema rejections (int->enum, enum
# mismatch, non-integer backing). Pure language features (no natives), so
# links only the core libs — matching constexpr_test's shape.
add_executable(typed_enum_test examples/typed_enum_test.cpp)
target_link_libraries(typed_enum_test PRIVATE ember ember_frontend ember_import)
if(WIN32)
    target_link_libraries(typed_enum_test PRIVATE kernel32)
endif()
add_test(NAME typed_enum COMMAND typed_enum_test)

# type_stress_test - auto type-detection stress suite. A programmatic
# companion to tests/lang/valid_type_stress.ember + sema_invalid_type_narrow.ember
# / sema_invalid_type_edge.ember: compiles small probes through the full
# parse->sema->codegen->finalize->call pipeline and asserts either the i64
# return (positive probe) or sema REJECTION (negative probe). Covers every
# integer width/signedness, typed enums, floats, and the constexpr-fold
# interaction across the full usage chain (decl -> assign -> binop -> arg ->
# return -> cast -> index -> match -> native). Also pins the three bugs the
# suite was built to find: [B1] f32->f64 implicit widening (was rejected),
# [B2] typed-enum variant literal narrowing/signedness (was silently
# accepted), [B3] binary bitwise on floats (was silently accepted). Pure
# language features (no natives), so links only the core libs.
add_executable(type_stress_test examples/type_stress_test.cpp)
target_link_libraries(type_stress_test PRIVATE ember ember_frontend ember_import)
if(WIN32)
    target_link_libraries(type_stress_test PRIVATE kernel32)
endif()
add_test(NAME type_stress COMMAND type_stress_test)

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

# thin_ir_test - Stage A c5 (IL-.em): the IR-backend CORRECTNESS GATE. Models
# the Stage 1 gate (codegen_opt_test): a compile(src, ir_on, ...) helper that
# runs the full pipeline with CodeGenCtx::enable_ir_backend set, captures
# main's pre-finalize bytes, finalizes, installs the dispatch table; a
# call0_i64 / call_i64_i64_i64 caller; the ck() assertion macro. Three
# assertion classes: (1) flags-OFF = byte-identical (the flag is inert when
# off — the default tree-walker path is unchanged); (2) flags-ON =
# value-equivalent (compile each corpus source both ways, call main, assert
# identical i64 returns — covers int arith/overflow, comparisons, short-
# circuit, control flow, break/continue, recursion, script + native calls,
# slices, structs, strings, defer, cast, ternary); (3) the flag actually runs
# (ir-on bytes differ from off; lower_function produces a non-empty
# ThinFunction). Links the SIX standard extensions (vec quat mat string array
# math) so the corpus can use array/string/math natives, matching sema_check's
# link line. The flag is default-off everywhere (ember_cli + engine helpers do
# NOT set it), so the lang suite is unaffected and stays 268/0/0.
add_executable(thin_ir_test examples/thin_ir_test.cpp)
target_link_libraries(thin_ir_test PRIVATE
    ember ember_frontend ember_import
    ember_ext_vec ember_ext_quat ember_ext_mat ember_ext_string ember_ext_array ember_ext_math)
if(WIN32)
    target_link_libraries(thin_ir_test PRIVATE kernel32)
endif()
add_test(NAME thin_ir COMMAND thin_ir_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# thin_ir_struct_test - Stage A c1 (IL-.em): pins the thin three-address IR
# data-structure CONTRACT that the lowering (c2) and the emit (c3) will
# independently implement against. Builds a small fib-shaped ThinFunction BY
# HAND (no parser/sema/lowering) and asserts the invariants c2 produces and
# c3 consumes: blocks non-empty + blocks[0] entry + every block terminated +
# VReg 0 sentinel + dump() non-empty and names the fn. Links only the core
# `ember` lib (thin_ir.cpp / dump() live there; no frontend symbols used) so
# it is independent of the extension refactor and of F2 — a pure data-struct
# pin, disjoint from c2/c3's new files (thin_lower / thin_emit).
add_executable(thin_ir_struct_test examples/thin_ir_struct_test.cpp)
target_link_libraries(thin_ir_struct_test PRIVATE ember)
add_test(NAME thin_ir_struct COMMAND thin_ir_struct_test)

# thin_ir_ser_test - Stage B c1b (IL-.em): the IR SERIALIZER round-trip gate.
# Hand-builds a ThinFunction + uses the real lower_function path (fib) to
# prove serialize->deserialize->validate->re-emit->execute works end-to-end,
# then malformed-blob rejection (bad magic, truncated, invalid ThinOp, OOB
# target, no terminator, empty blob). Links ember (thin_ir_ser/em_type_codec/
# thin_ir) + ember_frontend (lower_function/emit_x64/sema).
add_executable(thin_ir_ser_test examples/thin_ir_ser_test.cpp)
target_link_libraries(thin_ir_ser_test PRIVATE ember ember_frontend)
if(WIN32)
    target_link_libraries(thin_ir_ser_test PRIVATE kernel32)
endif()
add_test(NAME thin_ir_ser COMMAND thin_ir_ser_test)

# em_v5_ir_test - Stage B c3 (IL-.em): the v5 IR .em end-to-end integration
# test. Compiles a function via the IR path (lower_function), serializes to a
# v5 .em (write_em_file_v5), loads it (load_em_file deserializes + validates +
# re-emits + exec page), and asserts value-equivalence. Then malformed-blob
# rejection: bad magic, truncated, empty, unknown native name -> load returns
# false with NO exec page. Links ember + ember_frontend (the loader's v5 IR
# re-emit path is in ember_frontend).
add_executable(em_v5_ir_test examples/em_v5_ir_test.cpp)
target_link_libraries(em_v5_ir_test PRIVATE ember ember_frontend)
if(WIN32)
    target_link_libraries(em_v5_ir_test PRIVATE kernel32)
endif()
add_test(NAME em_v5_ir COMMAND em_v5_ir_test)

# ember_pass_test - Stage C: the composable pass system infrastructure test.
# Pins the pass manager, registry, pipeline parser, instrumentation,
# PreservedAnalyses, is_required, and run_to_fixpoint — no real passes yet.
# Links ember + ember_frontend (ember_pass.cpp lives in ember_frontend).
add_executable(ember_pass_test examples/ember_pass_test.cpp)
target_link_libraries(ember_pass_test PRIVATE ember ember_frontend)
add_test(NAME ember_pass COMMAND ember_pass_test)

# host_struct_test - proof-of-concept: host C++ struct types as first-class
# by-value script arguments (register_struct + C++-compatible layout).
add_executable(host_struct_test examples/host_struct_test.cpp)
target_link_libraries(host_struct_test PRIVATE ember ember_frontend)
if(WIN32)
    target_link_libraries(host_struct_test PRIVATE kernel32)
endif()
add_test(NAME host_struct COMMAND host_struct_test)

# ir_passes_test - Stage C: the IR optimization passes value-preservation +
# instr-count-reduction gate. Compiles the four IR-pass workloads, lowers each
# to ThinFunction, runs each pass (constprop/dce/cse) via the registry,
# emit_x64's, calls main, and asserts (a) value-preserving + (b) instr-count
# reduction on the pass's target workload. Links ember_ext_opt + six std exts.
add_executable(ir_passes_test examples/ir_passes_test.cpp)
target_link_libraries(ir_passes_test PRIVATE
    ember ember_frontend ember_import
    ember_ext_vec ember_ext_quat ember_ext_mat
    ember_ext_string ember_ext_array ember_ext_math
    ember_ext_opt ember_ext_obf)
if(WIN32)
    target_link_libraries(ir_passes_test PRIVATE kernel32)
endif()
add_test(NAME ir_passes COMMAND ir_passes_test)

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

# em_v5_mixed_test - D8: v5 mixed-mode (.em with IR + raw-x86 functions in one
# module). Builds a 2-function v5 module: `add` (is_ir=1, IR blob) + `double`
# (is_ir=0, raw-x86). Writes v5, loads, calls BOTH, asserts value-equivalence.
# Covers the mixed is_ir format (audit finding D8).
add_executable(em_v5_mixed_test examples/em_v5_mixed_test.cpp)
target_link_libraries(em_v5_mixed_test PRIVATE ember ember_frontend)
if(WIN32)
    target_link_libraries(em_v5_mixed_test PRIVATE kernel32)
endif()
add_test(NAME em_v5_mixed COMMAND em_v5_mixed_test)

set_tests_properties(em_roundtrip em_loader_hardening import_roundtrip binding_abi
    v0_6_lifecycle v0_6_hot_reload game_host_integration float_global_regression
    array_lit aggregate_global em_signed codegen_opt thin_ir_struct thin_ir
    thin_ir_ser em_v5_ir em_v5_mixed ember_pass ir_passes ext_map ext_bounds
    PROPERTIES TIMEOUT 30)
