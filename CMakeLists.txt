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
    src/platform.cpp         # platform abstraction (JIT memory, exec path) — groundwork for Linux/macOS/ARM ports
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
    # Stage 3: linear-scan register allocation over the ThinFunction. Runs
    # after the optimization passes, before emit_x64. Self-contained: touches
    # Type only via public data members + the Prim enum (same rule as
    # thin_ir.cpp / thin_ir_ser.cpp), so it stays in the core lib with no
    # ember_frontend dependency.
    src/regalloc.cpp
    # Stage B c1b: the ThinFunction serializer/deserializer (thin_ir_ser.{hpp,cpp}).
    # Turns a ThinFunction into an opaque byte blob (the ir_blob a v5 .em
    # per-function record carries) and back. Self-contained: touches Type only
    # via em_type_codec (emit_type/parse_type) + ThinFunction public members.
    src/thin_ir_ser.cpp
    # Phase 2 (plan_POLYMORPHIC_CODE_ENGINE.md §6.1/§6.2): the shared
    # IR-authoring foundation. thin_effects.{hpp,cpp} is the public exhaustive
    # operation-effect model (replaces the private side-effect switch in
    # ext_opt.cpp); thin_ir_mutation.{hpp,cpp} is the checked, transactional
    # mutation helper (central VReg enumeration, frame allocation, CFG edits,
    # regalloc invalidation). Both are self-contained: they touch Type only
    # via public data members + the Prim enum, so they stay in the core lib
    # with no ember_frontend dependency (one-way link direction).
    src/thin_effects.cpp
    src/thin_ir_mutation.cpp
    # Red 1 (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md §14.3): the versioned
    # reference affine dispatch permutation. Self-contained — only unsigned
    # integer arithmetic + the shared ExtensionResult/ExtensionError vocabulary
    # (extension_registry.hpp), no Type methods, no frontend symbols — so it
    # stays in the core lib with no ember_frontend dependency (one-way link
    # direction), matching thin_ir.cpp / thin_ir_ser.cpp / em_type_codec.cpp.
    src/keyed_dispatch.cpp
    src/key_provider.cpp          # Red 5: immutable provider boundary + dispatch key adapter (§6.1)
    src/gc.cpp              # GC: tracing mark-sweep core (foundational infra for lambdas/coroutines)
    src/safety.cpp          # process-wide failsafes: RSS memory cap, recursion depth guard, deadline
)
target_include_directories(ember PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ember PUBLIC ember_ed25519)  # F2 .em content authentication
# safety.cpp uses GetProcessMemoryInfo (psapi) on Windows for RSS measurement.
if(WIN32)
    target_link_libraries(ember PUBLIC psapi)
endif()

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
    # Phase 2 (plan_POLYMORPHIC_CODE_ENGINE.md §5, §9.3 Red 3): the shared,
    # versioned, stable seed-derivation service. SeedRequest + the immutable /
    # thread-safe SeedDeriver interface, the fixed-root implementation, the
    # u64→256-bit root adapter, and the pinned SplitMix64 StableRng helper.
    # Lives in ember_frontend alongside ember_pass.cpp (a compile-time pass
    # concern) and depends only on extension_registry.hpp (header-only) + the
    # C++ standard library.
    src/seed_derivation.cpp
    # Red 2 (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md §7, §14.3): the
    # stable canonical ABI fingerprint classifier. A concrete CallableDescriptor
    # + a pinned byte encoder + a non-std::hash 64-bit FNV-1a fingerprint.
    # Lives in ember_frontend (a compile-time/layout concern) and depends only
    # on the C++ standard library.
    src/dispatch_abi.cpp
    # Red 3 (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md §5, §9.2, §14.3): the
    # logical/physical module layout planner, identity layout, and the configured
    # KeyedDispatchRegistry for implicit-keyed-v1. A compile-time/layout concern
    # that depends on keyed_dispatch.* (core ember, the Red 1 helper — reached via
    # ember_frontend -> ember) and dispatch_abi.hpp (Red 2 header-only enum types).
    # No cycle: the core lib never depends on ember_frontend.
    src/module_layout.cpp
    # Red 6 (plan_POLYMORPHIC_CODE_ENGINE.md §4.2, §9.3): the immutable, shared
    # PolymorphicPassOptions record + its validation. A compile-time pass concern
    # that depends only on extension_registry.hpp + seed_derivation.hpp +
    # thin_ir_mutation.hpp (header-only types) + the C++ standard library. Lives
    # in ember_frontend alongside seed_derivation.cpp / ember_pass.cpp.
    src/polymorphic_options.cpp
    # The required host boundary for profile/pass compilation: the production
    # compile_func_checked + staged-ownership + atomic-publication primitive
    # shared by the JIT host (ember_cli --profile/--passes) and the integration
    # gate (polymorphic_pass_test PUB/LIFE). Depends on codegen.hpp
    # (compile_func_checked/CompileResult), engine.hpp (finalize),
    # dispatch_table.hpp (publish_batch), module_registry.hpp (preflight/register),
    # ember_pass.hpp (PassRunReport). Lives in ember_frontend (a compile-time /
    # host-boundary concern) and adds no new link direction.
    src/module_build.cpp
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
ember_add_extension(thread   extensions/thread/ext_thread.cpp)   # Tier 4: in-context threads (thread_spawn/join + call_mutex serialization)
# coroutine extension is Windows-only (uses CreateFiber/SwitchToFiber). On
# non-Windows we create an empty stub so link targets that reference
# ember_ext_coroutine still resolve. A ucontext port is a TODO (ROADMAP).
if(WIN32)
    ember_add_extension(coroutine extensions/coroutine/ext_coroutine.cpp) # #21 coroutines with yield (Windows fibers)
else()
    add_library(ember_ext_coroutine STATIC extensions/coroutine/ext_coroutine_stub.cpp)
    target_include_directories(ember_ext_coroutine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/extensions/coroutine)
    target_link_libraries(ember_ext_coroutine PUBLIC ember_frontend)
endif()
ember_add_extension(gc extensions/gc/ext_gc.cpp)            # tracing GC runtime: wires ember::gc::GcHeap into the engine for lambda env heap management (#20)
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
# ext_call_raw: raw x64 execution natives (the self-hosting Stage 4 gap).
# Three PERM_FFI-gated natives that bridge the ember-written codegen's output
# to actual execution: make_executable(array<u8>) -> fn_ptr (copies bytes to a
# W^X RX page via jit_memory), call_raw(fn_ptr, arg) -> i64 (invokes the entry),
# free_executable_ptr(ptr) (releases the page). Depends on ember_ext_array
# (make_executable reads the array<u8> handle via ext_array::get_bytes) + the
# ember core lib (jit_memory: alloc_executable / free_executable, pulled in
# transitively via ember_frontend -> ember). Stateless (no host slot vector;
# executable pages are caller-owned via free_executable_ptr). PERM_FFI-gated
# at the sema call site (minting + calling executable pages is a security
# surface — gated the same way ext_io is).
ember_add_extension(call_raw extensions/call_raw/ext_call_raw.cpp)
target_link_libraries(ember_ext_call_raw PUBLIC ember_ext_array)
# ext_audio: allocation-free raw access to host-owned f32/f64/i32 DSP buffers.
# The six bindings are PERM_FFI-gated because scripts supply raw pointers.
ember_add_extension(audio extensions/audio/ext_audio.cpp)

# ---- optional VST3 SDK + Phase 3 Ember gain effect ----
# The SDK is pinned as thirdparty/vst3sdk (git submodule, v3.8.0_build_66).
# Keep its samples/GUI/host utilities out of Ember's normal build; only the SDK
# libraries and our minimal combined IComponent/IAudioProcessor are built.
option(EMBER_BUILD_VST3 "Build the Ember stereo gain VST3 plug-in" ON)
if(EMBER_BUILD_VST3 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/vst3sdk/CMakeLists.txt")
    set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
    set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)
    set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "" FORCE)
    add_subdirectory(thirdparty/vst3sdk EXCLUDE_FROM_ALL)
    add_subdirectory(examples/vst3_wrapper)
endif()

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

# call_raw_test: runtime coverage for the call_raw native (extensions/call_raw,
# the self-hosting Stage 4 gap from plan_SELF_HOSTING.md). Exercises call_raw
# through the full lex->parse->sema->codegen->JIT->call path AND directly at
# the C++ level (compile a simple fn via the C++ codegen, get its entry pointer,
# call it via call_raw, verify the result), plus the PERM_FFI gating check.
# Links ember_ext_call_raw directly (no prism) to prove the extension is reusable.
add_executable(call_raw_test examples/call_raw_test.cpp)
target_link_libraries(call_raw_test PRIVATE ember ember_frontend ember_ext_call_raw)
add_test(NAME call_raw COMMAND call_raw_test)

# VST3 Phase 2 headless DSP gate: compile the @realtime gain/delay scripts,
# call them over host-owned stereo f32 buffers through ext_audio, compare to
# C++ references, and prove block-partition invariance (1024/256/64/1).
add_executable(vst_dsp_harness examples/vst_dsp_harness.cpp)
target_link_libraries(vst_dsp_harness PRIVATE ember ember_frontend ember_ext_audio)
add_test(NAME vst_dsp_harness COMMAND vst_dsp_harness
    ${CMAKE_CURRENT_SOURCE_DIR}/examples/scripts/gain_plugin.ember
    ${CMAKE_CURRENT_SOURCE_DIR}/examples/scripts/delay_plugin.ember)
set_tests_properties(vst_dsp_harness PROPERTIES TIMEOUT 60)

if(TARGET vst3_stress_tests)
    add_test(NAME vst3_stress COMMAND vst3_stress_tests stress)
    add_test(NAME vst3_realtime_contract COMMAND vst3_stress_tests realtime)
    add_test(NAME vst3_fuzz COMMAND vst3_stress_tests fuzz)
    add_test(NAME vst3_soak COMMAND vst3_stress_tests soak --seconds 30)
    set_tests_properties(vst3_stress vst3_realtime_contract vst3_fuzz PROPERTIES TIMEOUT 60)
    set_tests_properties(vst3_soak PROPERTIES TIMEOUT 60 LABELS soak)
endif()

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

# gc_test - pins the tracing mark-sweep GC core: alloc+collect, root scanning,
# reachable survives, unreachable collected, ref-graph tracing, cycles.
add_executable(gc_test examples/gc_test.cpp)
target_link_libraries(gc_test PRIVATE ember)
if(WIN32)
    target_link_libraries(gc_test PRIVATE kernel32)
endif()
add_test(NAME gc_core COMMAND gc_test)

# gc_integration_test - the tracing GC WIRED INTO THE ENGINE: ext_gc host API
# (alloc/pin/unroot/collect, heap-stays-bounded loop, auto-collect threshold)
# + the codegen use_gc_env path (compile+run capturing/no-capture/nested/
# lambda-as-arg lambdas + a 2000-lambda loop that triggers auto-collect
# mid-run, proving pinned envs survive). Links the standard extensions +
# ember_ext_gc so the __ember_gc_alloc_env native resolves at codegen time.
add_executable(gc_integration_test examples/gc_integration_test.cpp)
target_link_libraries(gc_integration_test PRIVATE
    ember ember_frontend ember_import
    ember_ext_vec ember_ext_quat ember_ext_mat ember_ext_string
    ember_ext_array ember_ext_math ember_ext_gc)
if(WIN32)
    target_link_libraries(gc_integration_test PRIVATE kernel32)
endif()
add_test(NAME gc_integration COMMAND gc_integration_test)
set_tests_properties(gc_integration PROPERTIES TIMEOUT 60)

# gc_full_test - the FULL GC integration end-to-end proof: by-reference
# lambda capture (fn[&x] capture-list syntax: read post-capture mutation +
# write-through + nested/transitive) + script-visible new/delete (gc_new/
# gc_delete/gc_collect/gc_live: heap stays bounded, unreachable reaped,
# reachable survive). Same harness shape as gc_integration_test (compile+run
# with use_gc_env=true + the gc extension registered).
add_executable(gc_full_test examples/gc_full_test.cpp)
target_link_libraries(gc_full_test PRIVATE
    ember ember_frontend ember_import
    ember_ext_vec ember_ext_quat ember_ext_mat ember_ext_string
    ember_ext_array ember_ext_math ember_ext_gc)
if(WIN32)
    target_link_libraries(gc_full_test PRIVATE kernel32)
endif()
add_test(NAME gc_full COMMAND gc_full_test)
set_tests_properties(gc_full PROPERTIES TIMEOUT 60)

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

# in_context_threads_test: Tier 4 in-context threads (the `thread` addon +
# aint* atomics). Two ember-calling OS threads on ONE context_t, serialized by
# context_t::call_mutex. Links ember_ext_thread (thread_spawn/join) +
# ember_ext_sync (aint* atomics for shared state). Mirrors thread_safety's
# B1 harness + the lock-around-main-call contract.
add_executable(in_context_threads_test examples/in_context_threads_test.cpp)
target_link_libraries(in_context_threads_test PRIVATE
    ember ember_frontend ember_import
    ember_ext_vec ember_ext_quat ember_ext_mat
    ember_ext_string ember_ext_array ember_ext_math
    ember_ext_sync ember_ext_thread)
if(WIN32)
    target_link_libraries(in_context_threads_test PRIVATE kernel32)
endif()
add_test(NAME in_context_threads COMMAND in_context_threads_test)
set_tests_properties(in_context_threads PROPERTIES TIMEOUT 60)

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

# Tier 4: in-language exceptions (try/catch/throw). try { ... } catch (name) { ... }
# + throw expr; with JIT-emitted setjmp/longjmp over a per-context catch stack
# (context_t::catch_bufs). A throw with no enclosing try/catch unwinds to the
# host checkpoint as a TrapReason::UnhandledThrow (a recoverable runtime trap).
# Mirrors function_refs_test's harness (B1 context-register model, six extensions).
add_executable(try_catch_test examples/try_catch_test.cpp)
target_link_libraries(try_catch_test PRIVATE
    ember ember_frontend ember_import
    ember_ext_vec ember_ext_quat ember_ext_mat
    ember_ext_string ember_ext_array ember_ext_math)
if(WIN32)
    target_link_libraries(try_catch_test PRIVATE kernel32)
endif()
add_test(NAME try_catch COMMAND try_catch_test)

# v1.0 Tier 2 parameterized function types: `fn(Args)->Ret` as a type, closing
# the bare-fn signature hole. A fn-typed param/variable WITH a signature
# type-checks its args at the call site; a bare `fn` stays the unsound
# backward-compat fallback (runtime guard still validates the handle). Proves
# Type::same equality (fn(i64)->i64 == fn(i64)->i64, != fn(i64,i64)->i64),
# sema accept/reject of matching/mismatched &fn handles, arg type-checking
# through a typed fn param, and runtime dispatch through typed fn params
# (higher-order, fn-as-arg, struct-field fn). Same harness as function_refs.
add_executable(fn_types_test examples/fn_types_test.cpp)
target_link_libraries(fn_types_test PRIVATE
    ember ember_frontend ember_import
    ember_ext_vec ember_ext_quat ember_ext_mat
    ember_ext_string ember_ext_array ember_ext_math)
if(WIN32)
    target_link_libraries(fn_types_test PRIVATE kernel32)
endif()
add_test(NAME fn_types COMMAND fn_types_test)

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

# v1.0 Tier 2 cross-module function handles (`&mod::fn` / `mod::fn` returning a
# handle that works across module boundaries). Proves cross-module handle
# creation + call, handle-passed-to-fn + called, out-of-range / unregistered
# forged handles trapping via BadCallTarget, and the intra/cross distinct-
# spaces type rule. Mirrors v0_5's registry/export wiring + function_refs'
# allowlist + trap-checkpoint harness.
add_executable(cross_module_handles_test examples/cross_module_handles_test.cpp)
target_link_libraries(cross_module_handles_test PRIVATE
    ember ember_frontend ember_import
    ember_ext_vec ember_ext_quat ember_ext_mat
    ember_ext_string ember_ext_array ember_ext_math)
if(WIN32)
    target_link_libraries(cross_module_handles_test PRIVATE kernel32)
endif()
add_test(NAME cross_module_handles COMMAND cross_module_handles_test)

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
    ember_ext_vec ember_ext_quat ember_ext_mat ember_ext_string ember_ext_array ember_ext_math
    ember_ext_io ember_ext_gc)

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
    ember_ext_io ember_ext_coroutine ember_ext_call_raw ember_ext_audio ember_ext_thread ember_ext_gc)

# Self-hosted compiler stages. Each ember test returns 0 on success and a
# stage-specific nonzero code on failure. Use absolute source paths so ctest
# works from any build directory; --ffi grants the array/string/call_raw
# natives used by the compiler implementation and full-pipeline execution.
foreach(stage IN ITEMS lex parse sema codegen full_pipeline)
    add_test(NAME self_hosted_${stage}
        COMMAND $<TARGET_FILE:ember_cli> run
                ${CMAKE_CURRENT_SOURCE_DIR}/self_hosted/${stage}_test.ember
                --fn main --ffi)
    set_tests_properties(self_hosted_${stage} PROPERTIES TIMEOUT 60)
endforeach()

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
    ember_ext_io ember_ext_coroutine ember_ext_call_raw ember_ext_gc)

# Shared bundler implementation. Both `ember bundle` and ember_bundle.exe call
# this one implementation, so compile/validation/footer policy cannot drift.
add_library(ember_bundler STATIC examples/ember_bundle.cpp)
target_compile_definitions(ember_bundler PRIVATE EMBER_BUNDLE_LIBRARY_ONLY)
target_link_libraries(ember_bundler PUBLIC
    ember ember_frontend ember_import
    ember_ext_vec ember_ext_quat ember_ext_mat
    ember_ext_string ember_ext_array ember_ext_math
    ember_ext_sync ember_ext_lifecycle ember_ext_map
    ember_ext_io ember_ext_coroutine ember_ext_call_raw ember_ext_gc)
target_link_libraries(ember_cli PRIVATE ember_bundler)

# Thin compatibility front-end for scripts that invoke ember_bundle directly.
add_executable(ember_bundle examples/ember_bundle.cpp)
target_compile_definitions(ember_bundle PRIVATE EMBER_BUNDLE_MAIN_ONLY)
target_link_libraries(ember_bundle PRIVATE ember_bundler)

# Bundle the Ember-written compiler as a runnable preview artifact. The custom
# command uses the same production bundler/stub as `ember bundle`; FFI is
# explicit because emberc reads a source file and allocates executable memory.
set(EMBER_SELFHOST_PREVIEW ${CMAKE_CURRENT_BINARY_DIR}/ember_selfhost_preview.exe)
add_custom_command(
    OUTPUT ${EMBER_SELFHOST_PREVIEW}
    COMMAND $<TARGET_FILE:ember_bundle>
            ${CMAKE_CURRENT_SOURCE_DIR}/self_hosted/emberc.ember
            ${EMBER_SELFHOST_PREVIEW}
            --stub $<TARGET_FILE:ember_stub_main>
            --fn main --permissions ffi
    DEPENDS ember_bundle ember_stub_main
            ${CMAKE_CURRENT_SOURCE_DIR}/self_hosted/emberc.ember
            ${CMAKE_CURRENT_SOURCE_DIR}/self_hosted/full_pipeline.ember
            ${CMAKE_CURRENT_SOURCE_DIR}/self_hosted/lex.ember
            ${CMAKE_CURRENT_SOURCE_DIR}/self_hosted/parse.ember
            ${CMAKE_CURRENT_SOURCE_DIR}/self_hosted/sema.ember
            ${CMAKE_CURRENT_SOURCE_DIR}/self_hosted/codegen.ember
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/self_hosted
    COMMENT "Bundling ember_selfhost_preview.exe")
add_custom_target(ember_selfhost_preview ALL DEPENDS ${EMBER_SELFHOST_PREVIEW})

# Feed a zero-return subset program to the bundled preview through stdin. This
# exercises bundle loading, io file read, all four self-hosted stages, W^X
# executable creation, and execution of the generated main function.
add_test(NAME self_hosted_preview_smoke
    COMMAND ${CMAKE_COMMAND}
            -DPREVIEW_EXE=${EMBER_SELFHOST_PREVIEW}
            -DINPUT_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/self_hosted/preview_smoke_input.ember
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/selfhost_preview_smoke.cmake)
set_tests_properties(self_hosted_preview_smoke PROPERTIES TIMEOUT 60)

# End-to-end bundler test: bundle .ember scripts into standalone exes, run
# them, assert exit codes. Covers simple scripts, multi-function, imports,
# globals, natives, and malformed-bundle rejection. Receives the bundler +
# stub exe paths via $<TARGET_FILE:...>.
add_executable(bundler_test examples/bundler_test.cpp)
add_test(NAME bundler_test COMMAND bundler_test
    $<TARGET_FILE:ember_bundle> $<TARGET_FILE:ember_stub_main> $<TARGET_FILE:ember_cli>)
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

# ---- IR-backend (--passes) end-to-end execution gate ----
# Reusable driver that shells out to the real ember_cli.exe `run --passes` path
# and compares the process exit code to an expected value. Pins the
# loop-carried accumulator fix in src/thin_emit.cpp (load_int_vreg must reload a
# promoted VReg from its promoted callee-saved register, not its stale frame
# slot) so a regression (e.g. valid_unroll --passes dce returning 26 instead of
# 56, or a runtime-trap exit 70 from an infinite for-loop) is a CTest failure.
# Driver args: <ember_cli> <source.ember> <expected_exit> <passes_spec>.
add_executable(ember_passes_exec_test examples/ember_passes_exec_test.cpp)
target_link_libraries(ember_passes_exec_test PRIVATE ember)
add_test(NAME ember_passes_unroll
    COMMAND ember_passes_exec_test $<TARGET_FILE:ember_cli>
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/lang/valid_unroll.ember 56 dce)
add_test(NAME ember_passes_lsr
    COMMAND ember_passes_exec_test $<TARGET_FILE:ember_cli>
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/lang/valid_lsr.ember 60 lsr)
add_test(NAME ember_passes_sccp
    COMMAND ember_passes_exec_test $<TARGET_FILE:ember_cli>
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/lang/valid_sccp.ember 42 sccp)
set_tests_properties(ember_passes_unroll ember_passes_lsr ember_passes_sccp
    PROPERTIES TIMEOUT 30)

# ---- Family C: ember pipe + ember live CLI regression ----
# End-to-end test harness for the two Family C CLI subcommands (docs/ROADMAP.md
# Family C) added in examples/ember_cli.cpp. Invokes the real ember_cli.exe as
# a subprocess and verifies the exit code + stdout of: pipe 2-stage (exit 60),
# pipe 3-stage (exit 810), pipe empty stream (exit 0), pipe missing module
# module (exit 2), pipe through a .em-bundled stage (exercises the bundler),
# live tick loop (tick 1:11/2:12/3:13), and live file-change (v1->overwrite v2
# mid-run -> stdout has both + a reload marker). The test .ember scripts +
# pipe configs live in tests/features/ (not tests/lang/, so `ember test` does
# not scan them). The harness takes <ember_cli> <source_dir> so it can resolve
# tests/features/ paths.
add_executable(ember_cli_pipe_live_test examples/ember_cli_pipe_live_test.cpp)
add_test(NAME ember_cli_pipe_live COMMAND ember_cli_pipe_live_test
    $<TARGET_FILE:ember_cli> ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(ember_cli_pipe_live PROPERTIES TIMEOUT 60)

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
    target_include_directories(bench_codegen_paths PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/bench)  # bench_output_names.hpp (deterministic results naming)
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

# bench_codegen_paths_selftest — focused CTest for the F-irsafe-contradiction
# contract: make_paths() must mark the 3 Stage A IR-backend gap paths
# (slice_bounds, string_decrypt, struct_by_value) ir_safe=false and the 7
# scalar/CF paths ir_safe=true. Runs bench_codegen_paths --selftest, which
# calls make_paths() and asserts the flags WITHOUT loading the baseline DLL
# or timing anything — sub-second, independent of the 60-second harness and
# the g++ -O2 baseline DLL. Reuses the same executable (no new target).
add_test(NAME bench_codegen_paths_selftest COMMAND bench_codegen_paths --selftest)
set_tests_properties(bench_codegen_paths_selftest PROPERTIES TIMEOUT 30)

# bench_output_names_test — focused coverage for the benchmark results-artifact
# naming fix (bench/bench_output_names.hpp). A manual --passes run and the
# CTest-registered no-passes run previously clobbered each other's CWD-relative
# results_codegen_paths.csv/.md. The helper makes the names deterministic and
# distinct on passes_flag; this test pins that invariant without running the
# 60-second harness (header-only, links no ember libs).
add_executable(bench_output_names_test examples/bench_output_names_test.cpp)
add_test(NAME bench_output_names COMMAND bench_output_names_test)
set_tests_properties(bench_output_names PROPERTIES TIMEOUT 30)

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

# keyed_dispatch_math - Red 1 (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md
# §14.3): the versioned reference affine dispatch permutation's property/
# bijection/bounds/domain-separation/wrong-key gate. Pins determinism,
# exhaustive bijection+bounds for every domain size 2..KEYED_DISPATCH_MAX_DOMAIN_SIZE,
# route words 0 and UINT64_MAX, first/last ordinals, multiple keys/salts/
# strategy versions, domain-separated pinned vectors, many wrong keys,
# hard-coded golden vectors, distinct build/runtime wrapper parity, and
# structured ExtensionResult/ExtensionError rejection of invalid sizes/
# ordinals. Cross-checks the production helper against an INDEPENDENT
# reimplementation of the spec written in the test (two implementations of
# one spec, exhaustively cross-checked). Links only the core `ember` lib
# (keyed_dispatch.* lives there, same self-contained discipline as thin_ir.cpp
# / thin_ir_ser.cpp). NOT a CTest entry: the filtered suite count must stay
# 67 (see docs/planning/plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md §14.1);
# the target building cleanly + the executable passing IS the gate.
add_executable(keyed_dispatch_math examples/keyed_dispatch_math_test.cpp)
target_link_libraries(keyed_dispatch_math PRIVATE ember)

# module_layout_test - Red 3 (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md
# §5, §9.2, §14.3): the logical/physical module-layout planner gate. Pins
# canonical-classifier fingerprints (Red 2), domain grouping by module/
# visibility/calling-mode/ABI-fingerprint/label, golden logical-to-physical
# placements under two pinned target route words (cross-checked against an
# independent salt/route-word oracle + the Red 1 permute helper), deterministic
# repeat builds, different keyed layouts, dense logical identity, exact
# ABI/visibility/label isolation, one ABI-compatible padding entry per keyed
# domain, singleton-real-function domains with physical count two, wrong-key
# per-domain in-range + same-metadata safety (NOT a global-range assertion),
# strict negatives (duplicate/non-dense slots, missing routes, overlapping/OOB
# placements, missing/duplicate padding, inconsistent counts, cross-ABI/
# cross-visibility membership, oversized domains, empty modules), disabled
# identity layout (slot i -> physical i, counts match, no padding), and the
# configured KeyedDispatchRegistry (implicit-keyed-v1) with structured
# diagnostics. Links ember (keyed_dispatch.* — Red 1 helper) + ember_frontend
# (dispatch_abi.* — Red 2 classifier, module_layout.* — Red 3 planner). NOT a
# CTest entry: the filtered suite count must stay 67 (§14.1); the target
# building cleanly + the executable passing IS the gate.
add_executable(module_layout_test examples/module_layout_test.cpp)
target_link_libraries(module_layout_test PRIVATE ember ember_frontend)

# keyed_dispatch_runtime - Red 4 (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md
# §2.4, §5.5, §7.3, §10, §14.2, §14.3, §14.5, §14.6): the runtime resolver +
# immutable module dispatch record gate. Pins correct-key pinned physical-index
# vectors, many wrong keys (including 0 and UINT64_MAX) with per-domain in-range
# + same-ABI/visibility/mode/label membership (NOT a global-range assertion),
# alternate/padding selection, malformed-record null/OOB safety with no OOB
# read, null/unfinalized entry rejection + strict publish_keyed, strict
# whole-record validation, identity-mode legacy behavior preservation,
# TrapReason::KeyedDispatchPadding + its stable string, and the §14.6 no-key-
# storage schema assertion (no expected key / fingerprint / digest / verifier /
# predecoded permutation in the record). Cross-checks the production resolver
# against an INDEPENDENT resolution oracle. Links ember (keyed_dispatch.* —
# Red 1 helper, context.hpp — TrapReason, dispatch_table.hpp —
# KeyedDispatchStorage) + ember_frontend (module_layout.* — Red 3 planner, Red
# 4 record/resolver/C-ABI helper). NOT a CTest entry: the filtered suite count
# must stay 67 (§14.1); the target building cleanly + the executable passing IS
# the gate.
add_executable(keyed_dispatch_runtime examples/keyed_dispatch_runtime_test.cpp)
target_link_libraries(keyed_dispatch_runtime PRIVATE ember ember_frontend)

# keyed_dispatch_outer_thunk - Red 5 (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md
# §6, §9.8, §10.3, §14.2, §14.3): the keyed outer thunk / provider boundary /
# ModuleInstance / safe keyed host-call API gate. Pins the §6 key lifecycle and
# the §9.8 / §14.2 mandatory buckets: the provider is invoked exactly once per
# outer call and never for recursive/nested script edges; provider failure
# prevents entry; r14 contains the supplied context_t*; r15 contains the
# transient route word throughout direct recursion and native re-entry; the
# caller's original r14/r15 are restored; transient r15 is cleared before
# restoring the caller value on normal and trapped exits; route words 0 and
# UINT64_MAX work; try/throw/catch preserves the keyed r15 invariant (slot 5
# restore); and two concurrent callers using different providers do not share
# route state. Links ember (keyed_dispatch.* — Red 1, context.hpp, engine.* —
# Red 5 thunks, dispatch_table.hpp, key_provider.* — Red 5 provider/adapter)
# + ember_frontend (module_layout.* — Red 3/4, module_instance.hpp — Red 5,
# codegen.* — the keyed CodeGenCtx, regalloc.* — r15 exclusion, the standard
# six-extension harness for native registration). NOT a CTest entry: the
# filtered suite count must stay 67 (§14.1); the target building cleanly + the
# executable passing IS the gate.
add_executable(keyed_dispatch_outer_thunk examples/keyed_dispatch_outer_thunk_test.cpp)
target_link_libraries(keyed_dispatch_outer_thunk PRIVATE
    ember ember_frontend ember_import
    ember_ext_vec ember_ext_quat ember_ext_mat
    ember_ext_string ember_ext_array ember_ext_math)
if(WIN32)
    target_link_libraries(keyed_dispatch_outer_thunk PRIVATE kernel32)
endif()

# keyed_dispatch_codegen - Red 6 (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md
# §9.4, §14.3, §14.2): the same-module keyed call lowering gate for BOTH the
# tree-walker backend and the Thin IR backend. Pins direct calls, recursive
# calls, indirect function handles, and lambda calls through both backends;
# mixed GP/XMM argument cases; depth accounting (balanced, no leak); correct-
# key routing; safe wrong-key alternate/padding routing (no crash, in-domain,
# KeyedDispatchPadding trap); logical handle stability (handle is a logical
# slot, never a physical slot); edge route words 0 and UINT64_MAX; and
# identity-layout behavior/bytes when CodeGenCtx::keyed_dispatch is null (the
# disabled path is byte-identical to legacy, no resolver address in the
# bytes). Links ember (keyed_dispatch.* — Red 1, context.hpp, engine.* — Red 5
# thunks, dispatch_table.hpp) + ember_frontend (module_layout.* — Red 3/4,
# codegen.* — the keyed CodeGenCtx, thin_lower.*, thin_emit.*, dispatch_abi.*).
# NOT a CTest entry: the filtered suite count must stay 67 (§14.1); the target
# building cleanly + the executable passing IS the gate.
add_executable(keyed_dispatch_codegen examples/keyed_dispatch_codegen_test.cpp)
target_link_libraries(keyed_dispatch_codegen PRIVATE ember ember_frontend ember_import)
if(WIN32)
    target_link_libraries(keyed_dispatch_codegen PRIVATE kernel32)
endif()

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

# em_redteam_audit_test - regression tests for the .em format red-team audit
# (docs/audit/EM_FORMAT_RED_TEAM_2026-07-11.md). Tests FIX 2 (PERM_FFI
# load-side enforcement, both v3 raw-x86 + v5 IR paths) and FIX 3 (raw-x86
# v1-v4 rejected by default, v5 accepted). FIX 1 (frame_size=0) is tested in
# thin_ir_ser_test.cpp. Links ember + ember_frontend (loader + writer + v5
# IR re-emit path + lowering).
add_executable(em_redteam_audit_test examples/em_redteam_audit_test.cpp)
target_link_libraries(em_redteam_audit_test PRIVATE ember ember_frontend)
if(WIN32)
    target_link_libraries(em_redteam_audit_test PRIVATE kernel32)
endif()
add_test(NAME em_redteam_audit COMMAND em_redteam_audit_test)

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
# PreservedAnalyses, is_required, run_to_fixpoint, and the Red 5 checked
# execution path (snapshots, validation, rollback, structured reports, and
# the compile_func_checked boundary). Links ember + ember_frontend + the opt
# extension so the checked-compile vectors can drive the real pass pipeline.
add_executable(ember_pass_test examples/ember_pass_test.cpp)
target_link_libraries(ember_pass_test PRIVATE ember ember_frontend ember_ext_opt ember_ext_obf)
add_test(NAME ember_pass COMMAND ember_pass_test)

# polymorphic_pass_test - Red 9 (plan_POLYMORPHIC_CODE_ENGINE.md §9.3
# Red 9): the configured polymorphic obfuscation pass test + the non-tautological
# polymorphic integration gate. Pins options validation, configured factory
# registration, no-op/changed preservation, same-seed serialized Thin IR equality,
# two-pinned-seed structural variation, baseline differential execution,
# serialize/deserialize round-trip, stale-regalloc invalidation, and exact growth
# boundaries + stop-before-site atomicity for the six migrated transforms
# (subst, mba_expand, const_encode, opaque_pred, deadcode, block_split) plus the
# full str_encrypt matrix (the Red 9 overlap gate: repeated + partial-overlap
# rodata ranges get PRIVATE non-overlapping encrypted regions with the original
# plaintext scrubbed). PLUS the Red 9 integration gate: a genuine multi-function
# module (real lex/parse/sema/lower) driven through compile_func_checked with an
# optimization prefix + all seven configured obfuscators, source/reverse order,
# serialize/deserialize/execute (regalloc off + on) of the deserialized Thin
# functions, byte-identical serialized Thin IR across order changes + repeated
# fresh construction, seed-0 vs UINT64_MAX variation with observable
# preservation, all seven legacy names EXECUTED through configured factories,
# structured lifecycle evidence (stale-regalloc, regalloc-once, corrupting-pass
# rollback, validation-failure-prevents-publication), and the real compile /
# publication boundary (compile_func_checked, reject TreeWalker, private staged
# ownership, rollback on failure, no partial record). Built as a standalone
# executable WITHOUT add_test so the unfiltered CTest baseline is unchanged
# (the add_test source-line count is 69; the filtered CI ctest total stays 67);
# run explicitly: ./buildt/polymorphic_pass_test. Links ember + ember_frontend +
# ember_ext_obf (the configured obf factories) + ember_ext_opt (the optimization
# prefix) + ember_ext_string (string_length for the module's string site).
add_executable(polymorphic_pass_test examples/polymorphic_pass_test.cpp)
target_link_libraries(polymorphic_pass_test PRIVATE ember ember_frontend ember_ext_obf ember_ext_opt ember_ext_string)
if(WIN32)
    target_link_libraries(polymorphic_pass_test PRIVATE kernel32)
endif()

# polymorphic_cli_test - Red 8 (plan_POLYMORPHIC_CODE_ENGINE.md §9.3 Red 8):
# the ordinary pipeline profile CLI regression harness. Shells out to the real
# ember_cli.exe as a subprocess and verifies --profile light|balanced|heavy,
# --pass-seed (decimal/hex/0/UINT64_MAX + malformed/overflow rejection),
# --passes-replaces-profile, and no-profile behavior preservation. Built as a
# standalone executable WITHOUT add_test so the filtered CTest total is
# unchanged (run explicitly: ./buildt/polymorphic_cli_test <ember_cli> <src>).
# Does NOT link ember — it only invokes the CLI as a subprocess, like
# ember_cli_pipe_live_test / em_cli_emit_test.
add_executable(polymorphic_cli_test examples/polymorphic_cli_test.cpp)
if(WIN32)
    # No ember linkage: the harness only invokes ember_cli as a subprocess
    # (CreateProcess + std::filesystem). It needs only the C++ stdlib + the
    # Windows API, both available by default on MinGW/MSVC.
endif()

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

# regalloc_test - Stage 3: the linear-scan register allocation value-preservation
# + register-assignment gate. Compiles register-heavy workloads (recursion,
# loops, register pressure) WITH and WITHOUT run_regalloc, asserts identical
# i64 returns, and verifies the regalloc actually assigned VRegs to callee-saved
# registers. Links ember + ember_frontend (lower_function/emit_x64/run_regalloc).
add_executable(regalloc_test examples/regalloc_test.cpp)
target_link_libraries(regalloc_test PRIVATE
    ember ember_frontend ember_import)
if(WIN32)
    target_link_libraries(regalloc_test PRIVATE kernel32)
endif()
add_test(NAME regalloc COMMAND regalloc_test)

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
# module) + the v5 secure-default raw-x86 rejection. Builds a 2-function v5
# module: `add` (is_ir=1, IR blob) + `double` (is_ir=0, raw-x86). Asserts the
# SECURE DEFAULT (no EmLoadPolicy) rejects the mixed module (raw-x86-bearing)
# before any exec allocation with a clear 'allow_raw_x86' error, that the
# secure default accepts an all-IR v5 module, and that explicit
# EmLoadPolicy{allow_raw_x86=true} accepts the mixed module with both
# functions value-equivalent. Covers the mixed is_ir format + the secure-
# default gate (audit finding D8 + MAINTENANCE_LOG 2026-07-12 candidate #1).
add_executable(em_v5_mixed_test examples/em_v5_mixed_test.cpp)
target_link_libraries(em_v5_mixed_test PRIVATE ember ember_frontend)
if(WIN32)
    target_link_libraries(em_v5_mixed_test PRIVATE kernel32)
endif()
add_test(NAME em_v5_mixed COMMAND em_v5_mixed_test)

set_tests_properties(em_roundtrip em_loader_hardening import_roundtrip binding_abi
    v0_6_lifecycle v0_6_hot_reload game_host_integration float_global_regression
    array_lit aggregate_global em_signed codegen_opt thin_ir_struct thin_ir
    thin_ir_ser em_redteam_audit em_v5_ir em_v5_mixed ember_pass ir_passes ext_map ext_bounds
    regalloc
    PROPERTIES TIMEOUT 30)

# ---- Fuzz harnesses (Phase 4 audit tooling) ----
# libFuzzer harnesses for the untrusted-input surfaces: .em loader (#1 security
# surface), lexer, parser. On Linux/CI these build with -fsanitize=fuzzer for
# coverage-guided fuzzing. On Windows (where libFuzzer is unsupported) the
# fuzz_batch_driver reads corpus files and calls the same entry points.
# See docs/planning/plan_AUDIT_TOOLING.md + tests/fuzz/.

# Batch driver (Windows + Linux): reads a corpus dir, feeds each file to the
# fuzz entry points, reports crashes. No coverage guidance — a regression /
# crash-repro verifier.
add_executable(fuzz_batch_driver tests/fuzz/fuzz_batch_driver.cpp)
target_link_libraries(fuzz_batch_driver PRIVATE ember_frontend ember_import ember ember_ed25519)
if(WIN32)
    target_link_libraries(fuzz_batch_driver PRIVATE psapi kernel32)
endif()

# libFuzzer harnesses (Linux only — -fsanitize=fuzzer is unsupported on
# Windows/MinGW). Enabled with -DENABLE_FUZZERS=ON. The CI sanitizer job
# builds + runs these on Linux with coverage-guided mutation.
option(ENABLE_FUZZERS "Build libFuzzer harnesses (Linux, requires -fsanitize=fuzzer support)" OFF)
if(ENABLE_FUZZERS)
    add_executable(fuzz_em_loader tests/fuzz/fuzz_em_loader.cpp)
    target_compile_options(fuzz_em_loader PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
    target_link_options(fuzz_em_loader PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_libraries(fuzz_em_loader PRIVATE ember_frontend ember_import ember ember_ed25519)

    add_executable(fuzz_lexer tests/fuzz/fuzz_lexer.cpp)
    target_compile_options(fuzz_lexer PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
    target_link_options(fuzz_lexer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_libraries(fuzz_lexer PRIVATE ember_frontend ember_import ember ember_ed25519)
endif()

# ctest: fuzz batch driver — verify no crash on the lang test corpus.
add_test(NAME fuzz_batch COMMAND fuzz_batch_driver ${CMAKE_CURRENT_SOURCE_DIR}/tests/lang)
set_tests_properties(fuzz_batch PROPERTIES TIMEOUT 30)
