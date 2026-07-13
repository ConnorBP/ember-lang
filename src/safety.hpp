// safety.hpp — process-wide failsafes against unbounded RAM growth + runaway execution.
//
// BACKGROUND: a benchmark/verification harness caused an unbounded RAM leak +
// CPU runaway that hard-froze the host machine. The root cause was JIT/VM
// execution with no effective memory ceiling and no execution deadline. This
// module provides the hard failsafes that were missing:
//
//   1. process_rss_kb()         — cross-platform resident-set measurement.
//   2. check_memory_limit()     — instant-fail (abort) if RSS exceeds a safe max.
//                                  Called from GcHeap::alloc, harness loops, compiler.
//   3. DepthGuard               — RAII recursion-depth cap (throws on overflow).
//                                  Used by compiler recursive walks (codegen/sema/etc).
//   4. deadline_expired()       — wall-clock timeout check for harness loops.
//
// These are deliberately CHEAP and SAFE to call from hot paths. The memory
// check is one OS query (~1us); the depth guard is one int increment. Neither
// changes program semantics — they only stop runaway execution BEFORE it
// freezes the machine. A process that hits a limit fails fast with a clear
// diagnostic instead of consuming all RAM/CPU until a hard freeze.
//
// This is the "safe max failsafe which instant-fails the app" the incident
// post-mortem mandated, present in BOTH the harnesses AND the compiler/VM.
#pragma once
#include <cstdint>
#include <cstddef>
#include <chrono>
#include <stdexcept>

namespace ember::safety {

// ---- Process memory (RSS) failsafe -------------------------------------

// Returns the current process resident-set size in KB, or 0 if unavailable.
// Cross-platform: Windows uses GetProcessMemoryInfo (psapi); Linux reads
// /proc/self/statm; macOS uses mach_task_basic_info (TODO if ever ported).
size_t process_rss_kb();

// Set/get the global hard memory limit in KB. If process RSS exceeds this,
// check_memory_limit() prints a diagnostic to stderr and calls std::abort()
// (instant fail — no unwind, no freeze). 0 disables the check.
// Default: 2 GiB (2097152 KB) — generous for legitimate compilation/bench
// work, but catches unbounded growth before it exhausts host RAM.
void   set_memory_limit_kb(size_t kb);
size_t memory_limit_kb();

// Check RSS against the limit. If exceeded, prints + aborts. No-op if the
// limit is 0. Safe to call from any thread including JIT-native re-entry.
// This is the HARD failsafe: it never returns if the limit is breached.
void   check_memory_limit();

// ---- Recursion depth failsafe (compiler) -------------------------------

// Thrown by DepthGuard when a compile-time recursion depth limit is exceeded.
// The compiler top-level catches this and reports a fatal "compilation
// aborted: recursion depth exceeded" error instead of overflowing the C++
// stack. This prevents the deep-left-skewed-AST stack-overflow path the audit
// identified (flat binary/f-string chains bypass the parser depth guard and
// blow the stack in sema/codegen).
struct DepthLimitExceeded : std::runtime_error {
    explicit DepthLimitExceeded(const char* ctx);
};

// RAII recursion-depth guard. Increments `counter` on construction; if it
// then exceeds `max`, throws DepthLimitExceeded(context). Decrements on
// destruction. Usage:
//   int depth = 0;
//   void recurse() { safety::DepthGuard g(depth, 4000, "codegen::eval"); ... recurse(); }
struct DepthGuard {
    DepthGuard(int& counter, int max_depth, const char* context);
    ~DepthGuard();
    DepthGuard(const DepthGuard&) = delete;
    DepthGuard& operator=(const DepthGuard&) = delete;
private:
    int& counter_;
};

// ---- Wall-clock deadline (harness loops) --------------------------------

using Clock     = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

inline TimePoint now() { return Clock::now(); }

// Returns true if `timeout_ms` have elapsed since `start`. timeout_ms == 0
// means no deadline (always false). Used by harness loops to bail out of a
// runaway iteration instead of spinning until CTest's coarse process timeout.
bool deadline_expired(TimePoint start, uint64_t timeout_ms);

} // namespace ember::safety
