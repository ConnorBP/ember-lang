// safety.cpp — process-wide failsafes against unbounded RAM + runaway execution.
// See safety.hpp for the incident background + design.
#include "safety.hpp"

#include <cstdio>
#include <cstdlib>
#include <atomic>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <psapi.h>
#else
#  include <fstream>
#  include <string>
#  include <unistd.h>
#endif

namespace ember::safety {

// ---- Process memory (RSS) failsafe -------------------------------------

size_t process_rss_kb() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    // K32GetProcessMemoryInfo is the modern name; GetProcessMemoryInfo is the
    // psapi forwarder. Either resolves to the same function on Win7+.
    if (GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        return static_cast<size_t>(pmc.WorkingSetSize) / 1024;
    }
    return 0;
#else
    // Linux: /proc/self/statm field 1 = resident pages.
    std::ifstream f("/proc/self/statm");
    if (!f) return 0;
    size_t size_pages = 0, resident_pages = 0;
    f >> size_pages >> resident_pages;
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) ps = 4096;
    return resident_pages * static_cast<size_t>(ps) / 1024;
#endif
}

static std::atomic<size_t> g_memory_limit_kb{ 2ull * 1024 * 1024 };  // 2 GiB default

void set_memory_limit_kb(size_t kb) { g_memory_limit_kb.store(kb, std::memory_order_relaxed); }
size_t memory_limit_kb()            { return g_memory_limit_kb.load(std::memory_order_relaxed); }

void check_memory_limit() {
    size_t limit = g_memory_limit_kb.load(std::memory_order_relaxed);
    if (limit == 0) return;  // disabled
    size_t rss = process_rss_kb();
    if (rss > limit) {
        std::fprintf(stderr,
            "\n*** SAFETY FAILSAFE: process RSS %zu KB exceeds limit %zu KB. "
            "Aborting to prevent host freeze (unbounded memory growth). ***\n",
            rss, limit);
        std::fflush(stderr);
        std::abort();
    }
}

// ---- Recursion depth failsafe (compiler) -------------------------------

DepthLimitExceeded::DepthLimitExceeded(const char* ctx)
    : std::runtime_error(std::string("compilation aborted: recursion depth exceeded in ") + ctx) {}

DepthGuard::DepthGuard(int& counter, int max_depth, const char* context)
    : counter_(counter) {
    int v = ++counter_;
    if (v > max_depth) {
        // Decrement first so a catch+retry sees a balanced counter.
        --counter_;
        throw DepthLimitExceeded(context);
    }
}

DepthGuard::~DepthGuard() { --counter_; }

// ---- Wall-clock deadline (harness loops) --------------------------------

bool deadline_expired(TimePoint start, uint64_t timeout_ms) {
    if (timeout_ms == 0) return false;
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now() - start).count();
    return static_cast<uint64_t>(elapsed) >= timeout_ms;
}

} // namespace ember::safety
