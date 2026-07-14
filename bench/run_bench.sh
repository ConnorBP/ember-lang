#!/usr/bin/env bash
# run_bench.sh — build the g++ -O2 baseline DLL (stale-probe: from source each
# run) + run the per-path codegen bench harness. Writes (no --passes):
#   bench/results_codegen_paths.csv   (machine-readable matrix)
#   bench/results_codegen_paths.md   (human-readable table + safety overhead)
# A manual `--passes` run instead writes results_codegen_paths_passes.csv/.md
# (distinct names so the two runs never clobber each other; see
# bench/bench_output_names.hpp).
#
# Gate: this is a "ran + wrote results = pass" bench (the bench_ember_vs_as
# shape), NOT an assertion bench. It never fails on a ratio; only on a
# compile/run/IO error. See docs/spec/BENCHMARK_SYSTEM_DESIGN.md.
#
# Usage: bash bench/run_bench.sh   (from the ember root)
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BENCH="$ROOT/bench"
GXX="${GXX:-/c/msys64/mingw64/bin/g++.exe}"

# 1. compile the g++ -O2 baseline from source (stale-probe discipline).
echo "[1/3] g++ -O2 baseline -> $BENCH/baseline_paths.dll"
"$GXX" -O2 -std=c++17 -shared -fPIC \
  -fno-tree-vectorize -fno-tree-loop-distribute-patterns \
  "$BENCH/baseline_paths.cpp" -o "$BENCH/baseline_paths.dll" || {
  echo "FAIL: baseline compile failed ($GXX)"; exit 2; }

# 2. locate the harness (built via CMake into buildt/, or build it ad-hoc).
HARNESS="$ROOT/buildt/bench_codegen_paths.exe"
if [ ! -x "$HARNESS" ]; then
  echo "[2/3] harness not at $HARNESS — building ad-hoc vs buildt libs"
  # ad-hoc build: link the harness against the fresh buildt static libs.
  "$GXX" -std=c++17 -O2 -I"$ROOT/src" -I"$ROOT/extensions/vec" -I"$ROOT/extensions/quat" \
    -I"$ROOT/extensions/mat" -I"$ROOT/extensions/string" -I"$ROOT/extensions/array" \
    -I"$ROOT/extensions/math" \
    "$BENCH/bench_codegen_paths.cpp" \
    "$ROOT/buildt/libember.a" "$ROOT/buildt/libember_frontend.a" "$ROOT/buildt/libember_import.a" \
    "$ROOT/buildt/libember_ext_vec.a" "$ROOT/buildt/libember_ext_quat.a" "$ROOT/buildt/libember_ext_mat.a" \
    "$ROOT/buildt/libember_ext_string.a" "$ROOT/buildt/libember_ext_array.a" "$ROOT/buildt/libember_ext_math.a" \
    -lkernel32 -o "$HARNESS" || { echo "FAIL: harness build failed"; exit 2; }
else
  echo "[2/3] harness found at $HARNESS"
fi

# 3. run (cd into bench/ so the DLL path resolves + results land in bench/).
echo "[3/3] running bench"
cd "$BENCH"
"$HARNESS" "$BENCH/baseline_paths.dll"
