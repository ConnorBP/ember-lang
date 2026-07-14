// bench_output_names.hpp — deterministic results-artifact naming for the
// per-path codegen benchmark harness (bench/bench_codegen_paths.cpp).
//
// PROBLEM (the artifact-clobber bug): the harness writes its CSV + markdown
// to CWD-relative `results_codegen_paths.csv` / `.md` regardless of whether
// `--passes` was passed. The CTest-registered run (no `--passes`) and a manual
// `--passes` run both land in the SAME working directory (the build dir, or
// `bench/` for run_bench.sh), so the second run overwrites the first's
// results. A no-passes run after a `--passes` run silently DESTROYS the
// `cfg=passes` rows — the only place the pass-impact data lived.
//
// FIX: when `passes_flag` is true, write to distinct `_passes`-suffixed names;
// when false, preserve the existing names (the CTest-registered run's output is
// unchanged, so nothing that greps `results_codegen_paths.csv` breaks). The
// selection is a pure function of `passes_flag` (deterministic, side-effect
// free) so it is unit-testable without running the harness.
//
// This header is the single source of truth for the names; the harness and the
// bench_output_names_test both include it so the behavior is not merely
// manually observed (the test asserts the no-clobber invariant: the two names
// are distinct for every artifact kind).
#ifndef EMBER_BENCH_OUTPUT_NAMES_HPP
#define EMBER_BENCH_OUTPUT_NAMES_HPP

#include <string>

namespace ember::bench {

// The results base name for a run. `passes_flag=false` preserves the original
// name (the CTest-registered no-passes run); `passes_flag=true` selects the
// distinct `_passes`-suffixed name (a manual `--passes` run) so the two never
// share an output file.
//
//   passes_flag=false -> "results_codegen_paths"
//   passes_flag=true  -> "results_codegen_paths_passes"
inline std::string results_base_name(bool passes_flag) {
    return passes_flag ? std::string("results_codegen_paths_passes")
                       : std::string("results_codegen_paths");
}

// The full CSV path. The CSV is the machine-readable matrix
// (path,safety,engine,config,...); `--passes` adds `cfg=passes` rows that the
// no-passes run does not have, so keeping them in a separate file preserves
// those rows against a later no-passes clobber.
inline std::string results_csv_path(bool passes_flag) {
    return results_base_name(passes_flag) + ".csv";
}

// The full markdown path. The MD is the human-readable tables (per-path ratios,
// passes-impact section, safety-on overhead); the `_passes` variant keeps the
// passes-impact section intact.
inline std::string results_md_path(bool passes_flag) {
    return results_base_name(passes_flag) + ".md";
}

} // namespace ember::bench

#endif // EMBER_BENCH_OUTPUT_NAMES_HPP
