// bench_output_names_test — focused coverage for bench/bench_output_names.hpp.
//
// The per-path codegen harness (bench/bench_codegen_paths.cpp) writes its CSV +
// markdown to CWD-relative files. Before the fix, a manual `--passes` run and
// the CTest-registered no-passes run both wrote `results_codegen_paths.csv` /
// `.md`, so the second clobbered the first — a no-passes run after a `--passes`
// run silently destroyed the `cfg=passes` rows.
//
// This test pins the deterministic filename-selection helper that fixes that:
//   - passes_flag=false preserves the original names (CTest run unchanged).
//   - passes_flag=true selects distinct `_passes`-suffixed names.
//   - the no-clobber invariant: for each artifact kind the two names differ, so
//     a `--passes` run and a no-passes run can never share an output file.
//
// This is the "not merely manually observed" coverage: the helper is a pure
// function of `passes_flag`, so the behavior is asserted here without running
// the 60-second benchmark harness. It links nothing (header-only), keeping it
// cheap and independent of the ember libs.

#include "../bench/bench_output_names.hpp"

#include <cstdio>
#include <string>

static int failures = 0;
static void check(bool ok, const char* msg) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", msg);
    if (!ok) ++failures;
}

int main() {
    using namespace ember::bench;

    std::printf("=== bench_output_names_test ===\n");

    // ---- base name selection ----
    check(results_base_name(false) == "results_codegen_paths",
           "no-passes base name is the original results_codegen_paths (CTest run unchanged)");
    check(results_base_name(true) == "results_codegen_paths_passes",
           "passes base name is the distinct results_codegen_paths_passes");

    // ---- full CSV path ----
    check(results_csv_path(false) == "results_codegen_paths.csv",
           "no-passes CSV path preserves the original .csv name");
    check(results_csv_path(true) == "results_codegen_paths_passes.csv",
           "passes CSV path is the distinct _passes.csv name");

    // ---- full markdown path ----
    check(results_md_path(false) == "results_codegen_paths.md",
           "no-passes MD path preserves the original .md name");
    check(results_md_path(true) == "results_codegen_paths_passes.md",
           "passes MD path is the distinct _passes.md name");

    // ---- the no-clobber invariant (the actual fix): the two runs never share
    //      an output file for either artifact kind. This is the assertion that
    //      would have caught the artifact-clobber bug. ----
    check(results_csv_path(false) != results_csv_path(true),
           "no-clobber: no-passes CSV != passes CSV (distinct files)");
    check(results_md_path(false) != results_md_path(true),
           "no-clobber: no-passes MD != passes MD (distinct files)");
    check(results_base_name(false) != results_base_name(true),
           "no-clobber: base names differ (the root of the distinct paths)");

    // ---- determinism: the same flag always yields the same name (the helper
    //      is a pure function of passes_flag; no hidden state / env / CWD). ----
    check(results_csv_path(true) == results_csv_path(true),
           "deterministic: repeated passes CSV path is stable");
    check(results_md_path(false) == results_md_path(false),
           "deterministic: repeated no-passes MD path is stable");

    // ---- the suffix is exactly the pass-specific marker (no accidental
    //      collision with a hypothetical extra extension). ----
    const std::string passes_csv = results_csv_path(true);
    check(passes_csv.find("results_codegen_paths_passes.csv") != std::string::npos,
           "passes CSV contains the _passes.csv suffix");
    check(passes_csv.rfind(".csv") == passes_csv.size() - 4,
           "passes CSV ends in .csv (extension intact)");

    if (failures == 0) {
        std::printf("\nbench_output_names_test: PASS (all filename-selection invariants hold)\n");
        return 0;
    }
    std::printf("\nbench_output_names_test: FAIL (%d invariant(s) broken)\n", failures);
    return 1;
}
