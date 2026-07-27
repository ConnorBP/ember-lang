#!/usr/bin/env bash
# run_wasm_tests.sh — WASM W1 acceptance: run .ember scripts through the
# Emscripten ThinIR-interpreter CLI (ember_wasm.js) + assert correct results.
#
# Usage: bash tests/run_wasm_tests.sh <build_dir> [node_path]
#
# Curated to the interpreter's W0-validated subset (int arithmetic i64,
# control flow, calls + recursion, for-each, match, structs with i64 fields,
# simple try/catch, lambdas). Narrow-width (i8/i16/i32) arithmetic/casts/
# struct fields + nested try/catch are W2 interpreter coverage gaps (noted in
# docs/planning/WASM_PROGRESS.md W1) — they are EXCLUDED here. Coroutines /
# threads / call_raw are stubbed (not supported in WASM).
#
# Exit 0 = all pass, 1 = any fail.
set -u

BUILD="${1:?usage: run_wasm_tests.sh <build_dir> [node]}"
NODE="${2:-node}"
SRC="$(dirname "$0")/.."
PASS=0; FAIL=0

# each line: <relative script path> <expected RESULT or TRAP> [--fn NAME]
run_one() {
    local script="$1"; local expect="$2"; shift 2
    local path="${SRC}/${script}"
    local out
    out=$("$NODE" "${BUILD}/ember_wasm.js" "$path" "$@" 2>/dev/null | grep -E '^(RESULT|TRAP) ' | head -1)
    # `expect` is matched as a PREFIX of `out` so a TRAP line with a trailing
    # ": <detail>" still matches (the detail is run-specific, not asserted).
    case "$out" in
        "$expect"*)
            echo "PASS  $script -> $out"
            PASS=$((PASS+1)) ;;
        *)
            echo "FAIL  $script -> got '$out', expected prefix '$expect'"
            FAIL=$((FAIL+1)) ;;
    esac
}

echo "=== WASM W1: ember_wasm interpreter acceptance ==="

# 1. arithmetic + params (i64)
run_one tests/lang/valid_arith.ember            "RESULT 13"
# 2. control flow (if/else/while)
run_one tests/lang/valid_control.ember          "RESULT 132"
# 3. recursion: fib(20) = 6765
run_one tests/lang/wasm_fib.ember               "RESULT 6765"
# 4. for-each over a slice
run_one tests/lang/valid_for_each.ember         "RESULT 150"
# 5. match
run_one tests/lang/valid_match.ember            "RESULT 20"
# 6. structs (i64 fields) + destructuring
run_one tests/lang/valid_struct_destructure.ember "RESULT 142"
# 7. simple try/catch (single handler; throw caught)
run_one tests/lang/valid_throw_nested.ember     "RESULT 99"
# 8. lambdas (no capture)
run_one tests/lang/valid_lambda_no_capture.ember "RESULT 84"
# 9. lambdas (capture)
run_one tests/lang/valid_lambda.ember           "RESULT 42"
# 10. integer division/modulo forms (i64)
run_one tests/lang/runtime_division_forms.ember "RESULT 78"
# 11. constexpr recursive fn (fib(10)=55, const-folded at sema)
run_one tests/lang/valid_constexpr_recursive.ember "RESULT 55"
# 12. namespaced intra-module calls
run_one tests/lang/valid_namespaces_intra_call.ember "RESULT 30"
# 13. array push/pop (i64) via the array extension
run_one tests/lang/valid_array_push_pop_i64.ember "RESULT 1"
# 14. struct reassign (i64 fields) — runtime probe
run_one tests/lang/runtime_struct_reassign_single.ember "RESULT 42"
# 15. uncaught throw -> TRAP (the interpreter's recoverable-trap path)
run_one tests/lang/runtime_trap_throw_uncaught.ember "TRAP unhandled throw"

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
