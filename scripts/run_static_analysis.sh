#!/usr/bin/env bash
# scripts/run_static_analysis.sh — Phase 1 audit tooling runner.
#
# Runs cppcheck + clang-tidy on the ember source. No build required (cppcheck
# works on source; clang-tidy needs buildt/compile_commands.json). Produces
# findings in analysis/cppcheck.txt + analysis/clang-tidy.txt.
#
# Usage: bash scripts/run_static_analysis.sh [build_dir]
#   build_dir defaults to buildt
#
# Tools must be installed (MSYS2):
#   pacman -S mingw-w64-x86_64-cppcheck mingw-w64-x86_64-clang-tools-extra
set -u

BUILD="${1:-buildt}"
cd "$(dirname "$0")/.." || exit 2
mkdir -p analysis

CPPCHECK="${CPPCHECK:-cppcheck}"
CLANG_TIDY="${CLANG_TIDY:-clang-tidy}"

echo "=== cppcheck ==="
if command -v "$CPPCHECK" >/dev/null 2>&1; then
    "$CPPCHECK" --enable=warning,performance,portability,style \
        --suppress=missingIncludeSystem --suppress=unusedFunction \
        --suppress=duplInheritedMember \
        --inline-suppr --check-level=exhaustive \
        -I src -I include --force -j 8 \
        src/ extensions/ 2>&1 \
        | grep -E 'warning:|error:|style:|performance:|portability:' \
        | grep -v 'normalCheckLevelMaxBranches\|information:' \
        > analysis/cppcheck.txt 2>&1
    echo "cppcheck: $(wc -l < analysis/cppcheck.txt) findings -> analysis/cppcheck.txt"
    echo "  by severity:"
    grep -oE '(warning|error|style|performance|portability):' analysis/cppcheck.txt | sort | uniq -c | sort -rn | sed 's/^/    /'
else
    echo "cppcheck not found — install: pacman -S mingw-w64-x86_64-cppcheck"
fi

echo ""
echo "=== clang-tidy ==="
if [ ! -f "$BUILD/compile_commands.json" ]; then
    echo "compile_commands.json not found in $BUILD — reconfigure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
else
    if command -v "$CLANG_TIDY" >/dev/null 2>&1; then
        # Focus on bug-finding checks, not style noise.
        "$CLANG_TIDY" -p "$BUILD" \
            --checks='-*,bugprone-*,clang-analyzer-*,cert-err33-c,cert-err34-c,cert-err52-cpp' \
            $(find src extensions -name '*.cpp') 2>&1 \
            | grep -E 'warning:|error:' \
            | grep -v 'pointer arithmetic\|multi-level-implicit\|easily-swappable' \
            > analysis/clang-tidy.txt 2>&1
        echo "clang-tidy: $(wc -l < analysis/clang-tidy.txt) findings -> analysis/clang-tidy.txt"
    else
        echo "clang-tidy not found — install: pacman -S mingw-w64-x86_64-clang-tools-extra"
    fi
fi
