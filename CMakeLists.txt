# ember - canonical language build (RESTRUCTURE_PLAN.md Section 2).
#
# Defines the three ember libraries (ember, ember_frontend, ember_import) and
# the two .em/import round-trip test executables. Consumed by prism via
# add_subdirectory() - prism links ember/ember_frontend/ember_import and gets
# their PUBLIC include dirs (src/) transitively. The obfuscation features
# (string encryption, MBA, CPUID gate) are part of the language codegen and
# ship here; prism wires the host-side natives (__str_decrypt etc.).

cmake_minimum_required(VERSION 3.16)
project(ember VERSION 0.2.0 LANGUAGES CXX)

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

# ---- core runtime lib: JIT memory, engine, .em serialize/load, module registry ----
add_library(ember STATIC
    src/jit_memory.cpp
    src/engine.cpp
    src/em_writer.cpp
    src/em_loader.cpp
    src/module_registry.cpp
)
target_include_directories(ember PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ---- frontend lib: lexer, types, parser, sema, codegen ----
add_library(ember_frontend STATIC
    src/lexer.cpp
    src/types.cpp
    src/parser.cpp
    src/sema.cpp
    src/codegen.cpp
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
# audit (see ember/extensions/AUDIT.md): vec2/vec3/vec4, quat, mat4, string,
# array<T>, math. Each is a self-contained TU depending only on ember's
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

# ext_runtime_test: runtime coverage for the relocated extensions (RESTRUCTURE_PLAN.md Section 6).
# em_roundtrip/import_roundtrip call NO relocated native; this one calls the math
# extension's sqrt through the full pipeline, proving the extension relocation
# registers correctly at runtime (not just links cleanly). Links ember_ext_math
# directly (no prism_script_host) to prove the extensions are reusable outside prism.
add_executable(ext_runtime_test examples/ext_runtime_test.cpp)
target_link_libraries(ext_runtime_test PRIVATE ember ember_frontend ember_ext_math)
add_test(NAME ext_runtime COMMAND ext_runtime_test)

# Extension registration smoke test (RESTRUCTURE_PLAN.md Section 6): validates the
# relocated non-cheat extensions (vec/quat/mat/string/array/math) register
# their NativeSig + OpOverloadTable entries via the extension entry points.
# Links ONLY the extension libs + ember_frontend (no prism/prism_script_host)
# - proving the extensions are pure ember and self-contained. The end-to-end
# JIT exercise of the same natives is covered by prism's ember_cli path
# (vector_math_demo.ember).
add_executable(ext_registration_test examples/ext_registration_test.cpp)
target_link_libraries(ext_registration_test PRIVATE
    ember_ext_vec ember_ext_quat ember_ext_mat ember_ext_string ember_ext_array ember_ext_math
    ember_frontend)
add_test(NAME ext_registration COMMAND ext_registration_test)

# ---- lang regression suite: ember_check (parse-only) + sema_check (parse+sema) +
# the run_lang_tests.sh runner that classifies tests/lang/{valid,invalid,sema_valid,
# sema_invalid,import_*}.ember against the two check exes. Ported from prism's
# tests/run_lang_tests.sh (lang-suite portion only); the .ember corpus under
# tests/lang/ is ported verbatim from prism/tests/lang/. The CLI run-via-CLI
# block for the import_* cases soft-fails (SKIP) if ember_cli.exe isn't built
# yet - parse/sema are the hard requirement. See tests/run_lang_tests.sh.
add_executable(ember_check examples/ember_check.cpp)
target_link_libraries(ember_check PRIVATE ember ember_frontend ember_import)

# sema_check links all six extension libs so scripts that call array/string
# (or any standard-addon) natives sema-check against the registered set -
# EXACTLY the ext_registration_test registration shape (register_natives for
# all six, register_overloads for vec/quat/mat/string), not prism's process/
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

# ---- standalone ember CLI: prism-decoupled runner ----
# A minimal host that exercises the full parse->sema->codegen->finalize->call
# pipeline (the em_roundtrip_test shape) with the full six-extension addon
# surface registered (the ext_registration_test shape). Reads a .ember file,
# resolves `import "path";` inlining, compiles, calls the entry fn (default
# main), and exits with the i64 return code (or 0 for void). No prism linkage:
# no VFS, no backends, no proc/shader api, no pak/assets, no print natives.
add_executable(ember_cli examples/ember_cli.cpp)
target_link_libraries(ember_cli PRIVATE
    ember ember_frontend ember_import
    ember_ext_vec ember_ext_quat ember_ext_mat
    ember_ext_string ember_ext_array ember_ext_math)

# ---- binding ABI correctness suite (BINDING_API.md Sec 4) ----
# Pins the script->native Win64 call ABI (struct-by-value arg <=8B,
# struct-by-value return >8B hidden-pointer, >4-arg stack spill, f32 in xmm
# slot-parallel, slice as ptr+len two words, mixed int+float slot-parallel)
# so a future tree-walker->SSA-IR refactor cannot silently break it. The
# codegen is already correct; this is the regression net. Registers FRESH
# host fns via BindingBuilder (src/binding_builder.hpp) + links all six
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
