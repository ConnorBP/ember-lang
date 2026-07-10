#!/usr/bin/env bash
# ember lang regression suite - lex + parse + sema + import-resolver tests.
#
# Adapted from prism/tests/run_lang_tests.sh (the lang-suite portion only;
# the prism-specific spectator/obf/benchmark/loadscript/hot_reload/features/
# examples blocks are NOT lang-suite and stay in prism). This script is the
# canonical standalone runner: ctest invokes it as
#   bash <src>/tests/run_lang_tests.sh <build_dir>
# and it resolves the three exes out of <build_dir>:
#   PARSE = <build_dir>/ember_check.exe    (parse-only: lex + resolve_imports + parse)
#   SEMA  = <build_dir>/sema_check.exe     (parse + sema against the six standard extensions)
#   CLI   = <build_dir>/ember_cli.exe      (required for positive execution/trap coverage)
#
# Classification (verbatim from prism's runner):
#   valid_*.ember + sema_valid_*.ember + sema_invalid_*.ember  -> must PARSE ok
#       (sema_invalid_* only fails at sema, not parse)
#   invalid_*.ember                                           -> must parse ERROR
#   sema_valid_*.ember + import_test.ember
#       + valid_structs_slices.ember                          -> must SEMA ok
#   sema_invalid_*.ember                                      -> must sema ERROR
#   import_nested/diamond/double/dotdot/self                  -> parse ok AND
#       run end-to-end via CLI (soft-fail SKIP if ember_cli.exe not built yet)
set -u

BUILD="${1:-./build}"
cd "$(dirname "$0")/.." || exit 2

PARSE="${BUILD}/ember_check.exe"
SEMA="${BUILD}/sema_check.exe"
CLI="${BUILD}/ember_cli.exe"

pass=0; fail=0; skip=0

run() {
    local tool="$1" file="$2" expect="$3"
    local out rc
    out=$("$tool" "$file" 2>&1); rc=$?
    if [ "$expect" = "ok" ] && [ $rc -eq 0 ]; then
        printf "PASS  %s\n" "$file"; pass=$((pass+1))
    elif [ "$expect" = "err" ] && [ $rc -ne 0 ]; then
        printf "PASS  %s (errored as expected)\n" "$file"; pass=$((pass+1))
    else
        printf "FAIL  %s (expected %s, got rc=%d)\n" "$file" "$expect" "$rc"
        printf "  --- output ---\n%s\n  --------------\n" "$out"
        fail=$((fail+1))
    fi
}

# --- hard requirement: parse + sema checks ---
if [ ! -x "$PARSE" ]; then printf "FATAL: ember_check not found at %s\n" "$PARSE"; exit 2; fi
if [ ! -x "$SEMA" ];  then printf "FATAL: sema_check not found at %s\n" "$SEMA";  exit 2; fi

# parse-only regression (syntactic errors only - sema_invalid_* parse fine,
# failing at sema instead).
for f in tests/lang/valid_*.ember tests/lang/runtime_*.ember tests/lang/sema_valid_*.ember tests/lang/sema_invalid_*.ember; do
    run "$PARSE" "$f" ok
done
for f in tests/lang/invalid_*.ember; do
    run "$PARSE" "$f" err
done

# Sema regression. sema_valid_basics is intentionally standalone (no Prism
# ru64/rf32/clamp dependency), so the entire sema_valid set is a hard gate.
for f in tests/lang/sema_valid_*.ember tests/lang/runtime_audit_semantics.ember \
         tests/lang/runtime_cast_regressions.ember tests/lang/runtime_division_forms.ember tests/lang/runtime_integer_boundaries.ember \
         tests/lang/runtime_language_features.ember tests/lang/import_test.ember \
         tests/lang/valid_structs_slices.ember; do
    run "$SEMA" "$f" ok
done
for f in tests/lang/sema_invalid_*.ember; do
    run "$SEMA" "$f" err
done

# --- import resolver corner cases: parse + run end-to-end via CLI ---
# These parse via ember_check AND run end-to-end via ember_cli (each has a
# main() that returns the chained result). Positive execution is a release
# requirement, not a soft parse-only fallback.
#
# ember_cli's return convention (see examples/ember_cli.cpp): if the entry
# returns i64, that value IS the process exit code (the validation signal).
# So a successful run of import_nested (whose main returns 111) exits 111,
# NOT 0. The runner must assert against each script's documented expected
# value, not rc==0. import_diamond's main returns 2003, but process exit codes
# truncate to 8 bits -> 2003 & 0xFF = 211, so we expect 211 (the OS truncation
# is real and the CLI's comment documents the i64-as-exit-code convention).
if [ ! -x "$CLI" ]; then printf "FATAL: ember_cli not found at %s\n" "$CLI"; exit 2; fi

# expected exit codes per import_* script (from each script's header comment;
# import_diamond's 2003 truncates to 211 as an 8-bit exit code).
exp_nested=111; exp_diamond=211; exp_double=100; exp_dotdot=101; exp_self=42

run_import_cli() {
    local f="$1" exp="$2"
    local out rc
    out=$("$CLI" run "$f" 2>&1); rc=$?
    if [ $rc -eq "$exp" ]; then
        printf "PASS  %s (run main, rc=%d == expected %d)\n" "$f" "$rc" "$exp"; pass=$((pass+1))
    else
        printf "FAIL  %s (run main, rc=%d != expected %d)\n%s\n" "$f" "$rc" "$exp" "$out"; fail=$((fail+1))
    fi
}

# Executable language regressions use main's exit code as their assertion.
for spec in "runtime_audit_semantics.ember:77" "runtime_cast_regressions.ember:42" \
            "runtime_division_forms.ember:78" "runtime_integer_boundaries.ember:79" "runtime_language_features.ember:93" \
            "sema_valid_basics.ember:6" "sema_valid_defer_local_ref.ember:94"; do
    f=${spec%%:*}; exp=${spec##*:}; out=$("$CLI" run "tests/lang/$f" 2>&1); rc=$?
    if [ $rc -eq "$exp" ]; then printf "PASS  %s (explicit expected rc=%d)\n" "$f" "$rc"; pass=$((pass+1))
    else printf "FAIL  %s (rc=%d, expected %d)\n%s\n" "$f" "$rc" "$exp" "$out"; fail=$((fail+1)); fi
done
for f in tests/lang/runtime_trap_*.ember; do
    out=$("$CLI" run "$f" 2>&1); rc=$?
    if [ $rc -eq 70 ] && printf "%s" "$out" | grep -q "RUNTIME TRAP"; then
        printf "PASS  %s (recoverable trap)\n" "$f"; pass=$((pass+1))
    else
        printf "FAIL  %s (expected recoverable rc=70, got %d)\n%s\n" "$f" "$rc" "$out"; fail=$((fail+1))
    fi
done

for f in tests/lang/import_nested.ember tests/lang/import_diamond.ember \
         tests/lang/import_double.ember tests/lang/import_dotdot.ember \
         tests/lang/import_self.ember; do
    run "$PARSE" "$f" ok
    case "$f" in
        *import_nested.ember)  run_import_cli "$f" "$exp_nested";;
        *import_diamond.ember) run_import_cli "$f" "$exp_diamond";;
        *import_double.ember)  run_import_cli "$f" "$exp_double";;
        *import_dotdot.ember) run_import_cli "$f" "$exp_dotdot";;
        *import_self.ember)   run_import_cli "$f" "$exp_self";;
    esac
done

printf "\n%d passed, %d failed, %d skipped\n" "$pass" "$fail" "$skip"
[ $fail -eq 0 ]
