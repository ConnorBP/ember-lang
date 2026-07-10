#!/usr/bin/env bash
# Post-g7 verification — step-logged, per-test timeout, fully file-logged, detached.
# Defensive: sweeps hung ember test processes first so nothing locks output files.
set -u
cd /e/DEVELOPER/PROJECTS/sus/hyper_workspace/ember
LOG=/tmp/ember_verify_g7.log
: > "$LOG"
ts() { date '+%H:%M:%S'; }
log() { echo "[$(ts)] $*" >> "$LOG"; }

log "=== VERIFY START ==="
log "sweeping any hung ember test processes (so they don't lock output files)"
for exe in v0_4_hardening_test ember_cli ext_sync_test v0_6_hot_reload_test v0_6_lifecycle_test win64_abi_test v0_5_live_modules_test bench_ember_vs_as em_roundtrip_test em_cli_emit thread_safety function_refs_test ext_registration_test game_host binding_abi_test sema_check ember_check v0_4_hardening; do
  tasklist 2>/dev/null | grep -qi "$exe.exe" && taskkill //F //IM "$exe.exe" >/dev/null 2>&1 && log "  swept $exe"
done
log "sweep done"

CXX=/c/msys64/mingw64/bin/g++.exe
NINJA=/e/DEVELOPER/devtools/ninja
CTEST="/c/Program Files/CMake/bin/ctest.exe"

log "STEP 1: rebuild"
if timeout 120 $NINJA -C buildt >>"$LOG" 2>&1; then log "  build OK"; else log "  BUILD FAIL rc=$?"; log "=== VERIFY ABORT (build) ==="; exit 1; fi

log "STEP 2: full ctest (--timeout 30 per test)"
if timeout 240 $CTEST --timeout 30 >>"$LOG" 2>&1; then log "  ctest OK"; else log "  CTEST FAIL rc=$?"; fi
sum=$(grep -E "^[0-9]+% tests" "$LOG" | tail -1); log "  summary: $sum"

log "STEP 3: ext_sync determinism 20x"
p=0; for i in $(seq 1 20); do timeout 30 $CTEST -R ext_sync >/dev/null 2>&1 && p=$((p+1)); done
log "  ext_sync: $p/20"

log "STEP 4: lang suite"
if timeout 120 bash tests/run_lang_tests.sh buildt >>"$LOG" 2>&1; then
  log "  lang: $(grep -E 'passed' "$LOG" | tail -1)"
else log "  LANG FAIL rc=$?"; fi

log "STEP 5: relink loader probe + 5 malformed .em reproductions"
$CXX -std=c++17 -O2 -Isrc tmp_edit/audit_em_loader_probe.cpp buildt/libember.a -o tmp_edit/audit_em_loader_probe_g7.exe >>"$LOG" 2>&1
for f in em_reloc_wrap em_huge_fc em_huge_globals em_huge_slot em_huge_reloc; do
  out=$(timeout 10 ./tmp_edit/audit_em_loader_probe_g7.exe tmp_edit/$f.em 2>&1); log "  $f: rc=$? | $out"
done

log "STEP 6: v0_4_hardening (H-M4-1/2 regressions live here)"
if timeout 30 ./buildt/v0_4_hardening_test.exe >>"$LOG" 2>&1; then log "  v0_4 OK"; else log "  v0_4 FAIL rc=$?"; fi

log "STEP 7: H-§10-1 fold normalize (must be -2147483648 now)"
$CXX -std=c++17 -O2 -Isrc tmp_edit/c5/shift_enum_direct_probe.cpp buildt/libember.a buildt/libember_frontend.a -o tmp_edit/shift_enum_direct_probe_g7.exe >>"$LOG" 2>&1
log "  shift_enum: $(timeout 10 ./tmp_edit/shift_enum_direct_probe_g7.exe 2>&1)"

log "STEP 8: ext_sync_test (M-§10-2 CAS expected mask)"
if timeout 30 ./buildt/ext_sync_test.exe >>"$LOG" 2>&1; then log "  ext_sync_test OK"; else log "  ext_sync_test FAIL rc=$?"; fi

log "=== VERIFY DONE ==="
