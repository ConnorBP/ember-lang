#!/usr/bin/env bash
# scripts/run_coverage.sh — gcov code coverage report (Phase 1).
#
# Builds a coverage-instrumented ember_cli, runs the lang test suite + the
# optimization validation, and generates a text + HTML coverage report.
#
# Usage: bash scripts/run_coverage.sh
# Requires: gcov (ships with MinGW g++), gcovr (pip install gcovr)
set -u
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
    -DCMAKE_CXX_COMPILER=/c/msys64/mingw64/bin/g++.exe \
    -DCMAKE_CXX_FLAGS="-O0 -g -fprofile-arcs -ftest-coverage -fPIC" \
    -DCMAKE_EXE_LINKER_FLAGS="-lgcov --coverage" \
    . 2>&1 | tail -3

echo "=== Building ember_cli (coverage) ==="
cmake --build "$BUILD_COV" --target ember_cli -j 8 2>&1 | grep -iE 'error' | head -5

echo "=== Running tests under coverage ==="
cd "$BUILD_COV"
./ember_cli.exe run ../tests/lang/optimization_validation.ember --fn main --passes constprop,forward,copyprop,instcombine,dce,licm,dse,simplifycfg,bounds-elim,sccp,unroll,spill_elim,peephole,lsr 2>/dev/null
echo "  validation: $?"
./ember_cli.exe test ../tests/lang 2>&1 | grep '#.*passed' | tail -1
cd ..

echo "=== Generating coverage report ==="
mkdir -p "$BUILD_COV/coverage_html"
"$GCOVR" --root "$BUILD_COV" --gcov-executable "$GCOV" \
    --exclude "$BUILD_COV/CMakeFiles/4*" --exclude 'thirdparty/*' \
    --txt-metric=line \
    > analysis/coverage.txt 2>&1
echo "  text report -> analysis/coverage.txt"
"$GCOVR" --root "$BUILD_COV" --gcov-executable "$GCOV" \
    --exclude "$BUILD_COV/CMakeFiles/4*" --exclude 'thirdparty/*' \
    --html-details "$BUILD_COV/coverage_html/index.html" 2>&1 | tail -2
echo "  html report -> $BUILD_COV/coverage_html/index.html"
echo ""
echo "=== Coverage summary ==="
grep 'TOTAL' analysis/coverage.txt
