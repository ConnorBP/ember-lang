#!/usr/bin/env bash
# scripts/run_drmemory.sh — DrMemory dynamic memory checker (Phase 1, Windows).
#
# DrMemory works on UNMODIFIED MinGW g++ binaries — no toolchain change needed.
# It catches UAF, OOB, uninitialized reads, invalid heap args, and leaks at
# runtime. This is the Windows-native alternative to ASAN (which is unreliable
# on MinGW — see docs/planning/plan_AUDIT_TOOLING.md).
#
# Usage: bash scripts/run_drmemory.sh [build_dir] [test_file]
#   build_dir defaults to buildt
#   test_file defaults to tests/lang/optimization_validation.ember
#
# Requires: DrMemory installed (download from drmemory.org/page_download.html)
#   default path: /c/tools/DrMemory-Windows-2.6.0/bin/drmemory.exe
set -u

BUILD="${1:-buildt}"
TEST="${2:-tests/lang/optimization_validation.ember}"
cd "$(dirname "$0")/.." || exit 2

DRMEMORY="${DRMEMORY:-/c/tools/DrMemory-Windows-2.6.0/bin/drmemory.exe}"

if [ ! -x "$DRMEMORY" ]; then
    echo "DrMemory not found at $DRMEMORY"
    echo "Download from: https://drmemory.org/page_download.html"
    echo "Extract to /c/tools/DrMemory-Windows-2.6.0/ (or set DRMEMORY env var)"
    exit 1
fi

if [ ! -f "$BUILD/ember_cli.exe" ]; then
    echo "ember_cli.exe not found in $BUILD — build first"
    exit 1
fi

mkdir -p analysis

echo "=== Running ember_cli under DrMemory ==="
echo "Test: $TEST"
"$DRMEMORY" -batch -- "$BUILD/ember_cli.exe" run "$TEST" --fn main 2>&1 \
    | grep -E 'ERRORS|unique|total|Error #' \
    > analysis/drmemory.txt 2>&1

echo ""
echo "=== Summary ==="
grep -E 'ERRORS|unique|total' analysis/drmemory.txt
echo ""
echo "Full results: analysis/drmemory.txt"
echo "DrMemory log: see the path printed at the end of the run"
echo ""
echo "NOTE: many findings are false positives from the JIT execution model"
echo "(unaddressable 'beyond top of stack' = JIT pages DrMemory doesn't track)"
echo "and libstdc++ std::filesystem (malloc/delete mismatch = runtime quirk)."
echo "The value is catching NEW findings beyond this baseline."
