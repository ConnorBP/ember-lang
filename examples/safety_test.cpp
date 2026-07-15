// Focused unit coverage for process and compiler safety helpers.
#include "safety.hpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace {
int failures = 0;
void check(bool condition, const char* message) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++failures;
}
} // namespace

int main() {
    using namespace ember::safety;

    const size_t original_limit = memory_limit_kb();
    check(original_limit == 2ull * 1024 * 1024, "default memory limit is 2 GiB");
    set_memory_limit_kb(0);
    check(memory_limit_kb() == 0, "memory limit can be disabled");
    check_memory_limit();
    check(true, "disabled memory check returns normally");
    set_memory_limit_kb(original_limit);
    check(process_rss_kb() > 0, "process RSS is available on Windows");
    check_memory_limit();
    check(true, "normal memory check returns below the limit");

    int depth = 0;
    {
        DepthGuard outer(depth, 2, "safety_test");
        check(depth == 1, "depth guard increments its counter");
        {
            DepthGuard inner(depth, 2, "safety_test");
            check(depth == 2, "nested depth guard reaches the limit");
        }
        check(depth == 1, "nested depth guard balances on destruction");
    }
    check(depth == 0, "depth guard balances the outer scope");

    bool threw = false;
    try {
        DepthGuard too_deep(depth, 0, "edge context");
    } catch (const DepthLimitExceeded& e) {
        threw = std::string(e.what()).find("edge context") != std::string::npos;
    }
    check(threw, "depth overflow throws a contextual diagnostic");
    check(depth == 0, "throwing depth guard restores the counter");

    const auto start = now();
    check(!deadline_expired(start, 0), "zero timeout disables the deadline");
    check(!deadline_expired(start, 1000), "future deadline is not expired");
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    check(deadline_expired(start, 1), "elapsed deadline expires");

    std::printf("safety test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
