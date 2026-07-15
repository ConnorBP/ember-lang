#!/usr/bin/env bash
# scripts/run_coverage.sh — gcov code coverage report (Phase 1).
#
# Builds all coverage-instrumented test targets, runs the CTest/lang suites,
# the standalone IR/pass gates, and several optimization/obfuscation pipelines,
# then generates a text + HTML coverage report.
#
# Usage: bash scripts/run_coverage.sh
# Requires: gcov (ships with MinGW g++), gcovr (pip install gcovr)
set -euo pipefail
cd "$(dirname "$0")/.." || exit 2

BUILD_COV="build_cov"
GCOVR="${GCOVR:-gcovr}"
GCOV="${GCOV:-gcov}"

if ! command -v "$GCOVR" >/dev/null 2>&1; then
    echo "gcovr not found — install: pip install gcovr"
    exit 1
fi

echo "=== Configuring coverage build ($BUILD_COV) ==="
cmake -G Ninja -B "$BUILD_COV" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_COMPILER=/c/msys64/mingw64/bin/g++.exe \
    -DCMAKE_CXX_FLAGS="-O0 -g -fprofile-arcs -ftest-coverage -fPIC" \
    -DCMAKE_EXE_LINKER_FLAGS="-lgcov --coverage" \
    . 2>&1 | tail -3

# Remove stale counters before building too: a few ALL/custom targets execute
# instrumented binaries as part of their build and would otherwise merge data
# from objects with a different checksum after a reconfiguration.
find "$BUILD_COV" -name '*.gcda' -delete

echo "=== Building coverage test targets ==="
# CTest and the standalone gates below need more than ember_cli, so build the
# complete default target set rather than only the CLI.
cmake --build "$BUILD_COV" -j 8

# Do not mix this run's counters with data from an older coverage invocation.
find "$BUILD_COV" -name '*.gcda' -delete

echo "=== Running tests under coverage ==="
cd "$BUILD_COV"

# Run the regular suite, omitting benchmarks, the long-running thread test, and
# anything explicitly labelled as a soak test.
# Coverage collection is deliberately best-effort: this checkout can contain
# RED-phase/experimental tests, but their exercised lines are still useful.
# Preserve CTest's status in the log without aborting the remaining workloads.
if ! ctest -E 'bench|in_context_threads' -LE soak --timeout 120; then
    echo "WARNING: CTest reported failures; continuing coverage collection" >&2
fi

# Some of the newest IR/pass gates intentionally are not registered with CTest.
# Keep the requested test names in the log; several keyed-dispatch CMake targets
# omit the source file's trailing `_test` from the produced executable name.
run_test_executable() {
    local test_name="$1"
    local executable="$2"
    shift 2
    echo "--- $test_name ---"
    if "./${executable}.exe" "$@"; then
        return 0
    else
        local status=$?
        echo "WARNING: $test_name exited $status; continuing coverage collection" >&2
        return 0
    fi
}

run_test_executable ember_pass_test ember_pass_test
run_test_executable ir_passes_test ir_passes_test
run_test_executable ember_passes_exec_test ember_passes_exec_test \
    ./ember_cli.exe ../tests/lang/valid_unroll.ember 56 dce
run_test_executable thin_ir_test thin_ir_test
run_test_executable thin_ir_ser_test thin_ir_ser_test
run_test_executable thin_ir_struct_test thin_ir_struct_test
run_test_executable em_v5_ir_test em_v5_ir_test
run_test_executable polymorphic_pass_test polymorphic_pass_test
run_test_executable keyed_dispatch_codegen_test keyed_dispatch_codegen
run_test_executable keyed_dispatch_math_test keyed_dispatch_math
run_test_executable keyed_dispatch_runtime_test keyed_dispatch_runtime
run_test_executable keyed_dispatch_modules_test keyed_dispatch_modules
run_test_executable keyed_dispatch_outer_thunk_test keyed_dispatch_outer_thunk
run_test_executable keyed_dispatch_extensions_test keyed_dispatch_extensions
run_test_executable keyed_dispatch_hot_reload_test keyed_dispatch_hot_reload

# optimization_validation.ember deliberately returns 177 on success. Exercise
# each pass family separately so a full-pipeline run cannot hide an unvisited
# parser/registry/analysis path.
run_validation() {
    local description="$1"
    local passes="$2"
    local status

    echo "--- optimization validation: $description ---"
    if ./ember_cli.exe run ../tests/lang/optimization_validation.ember \
        --fn main --passes "$passes"; then
        status=0
    else
        status=$?
    fi

    if [[ "$status" -ne 177 ]]; then
        echo "  FAIL: expected exit 177, got $status" >&2
        return 1
    fi
    echo "  PASS: exit $status"
}

run_validation "minimal" \
    "constprop,dce"
run_validation "standard optimizations" \
    "constprop,forward,copyprop,instcombine,dce"
run_validation "SCCP + unroll" \
    "sccp,unroll"
run_validation "loop optimizations" \
    "constprop,licm,dse,simplifycfg"
run_validation "peephole + LSR" \
    "spill_elim,peephole,lsr"
run_validation "full optimization pipeline" \
    "constprop,forward,copyprop,instcombine,dce,licm,dse,simplifycfg,bounds-elim,sccp,unroll,spill_elim,peephole,lsr"
run_validation "obfuscation pipeline" \
    "subst,mba_expand,const_encode,opaque_pred,deadcode,str_encrypt,block_split"

echo "--- Ember language suite (direct CLI path) ---"
if ! ./ember_cli.exe test ../tests/lang; then
    echo "WARNING: direct Ember language suite reported failures; coverage was retained" >&2
fi
cd ..

echo "=== Generating coverage report ==="
mkdir -p analysis "$BUILD_COV/coverage_html"
"$GCOVR" --root . "$BUILD_COV" --gcov-executable "$GCOV" \
    --exclude "$BUILD_COV/CMakeFiles/4*" --exclude 'thirdparty/*' \
    --gcov-ignore-parse-errors negative_hits.warn_once_per_file \
    --txt-metric=line \
    > analysis/coverage.txt 2>&1
echo "  text report -> analysis/coverage.txt"
"$GCOVR" --root . "$BUILD_COV" --gcov-executable "$GCOV" \
    --exclude "$BUILD_COV/CMakeFiles/4*" --exclude 'thirdparty/*' \
    --gcov-ignore-parse-errors negative_hits.warn_once_per_file \
    --html-details "$BUILD_COV/coverage_html/index.html" 2>&1 | tail -2
echo "  html report -> $BUILD_COV/coverage_html/index.html"
echo ""
echo "=== Coverage summary ==="
grep 'TOTAL' analysis/coverage.txt
