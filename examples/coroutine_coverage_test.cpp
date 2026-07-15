// Direct coverage for Windows-fiber coroutine start/yield/resume/done paths.
#include "ext_coroutine.hpp"
#include "engine.hpp"
#include "dispatch_table.hpp"
#include "module_instance.hpp"
#include "runtime_extension_state.hpp"
#include "key_provider.hpp"
#include "ast.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>

using namespace ember;
#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); ext_coroutine::coroutine_reset(); return 1; } } while (0)

using YieldFn = void(*)(int64_t);
using StartFn = int64_t(*)(int64_t, int64_t);
static YieldFn g_yield = nullptr;
static StartFn g_start = nullptr;

extern "C" int64_t multi_yield(int64_t arg) {
    g_yield(arg); g_yield(arg + 1); return arg + 2;
}
extern "C" int64_t sequence_a(int64_t) {
    g_yield(1); g_yield(2); return 3;
}
extern "C" int64_t sequence_b(int64_t) {
    g_yield(10); g_yield(20); return 30;
}
extern "C" int64_t keyed_start_entry(int64_t arg) {
    return g_start(0, arg);
}

template <typename Fn>
static Fn native(const std::unordered_map<std::string, NativeSig>& n, const char* name) {
    const auto it = n.find(name); if (it == n.end() || !it->second.fn_ptr) std::abort();
    return reinterpret_cast<Fn>(it->second.fn_ptr);
}

int main() {
    std::unordered_map<std::string, NativeSig> n;
    ext_coroutine::register_natives(n);
    g_yield = native<YieldFn>(n, "__ember_coro_yield");
    g_start = native<StartFn>(n, "coroutine_start");
    const auto next = native<int64_t(*)(int64_t)>(n, "coroutine_next");
    const auto done = native<int64_t(*)(int64_t)>(n, "coroutine_done");
    const auto set_dispatch = native<void(*)(int64_t, int64_t)>(n, "set_coroutine_dispatch");

    context_t ctx{};
    ctx.budget_remaining = 1'000'000'000;
    ctx.max_call_depth = 128;
    std::array<std::atomic<void*>, 3> slots;
    slots[0].store(reinterpret_cast<void*>(&multi_yield));
    slots[1].store(reinterpret_cast<void*>(&sequence_a));
    slots[2].store(reinterpret_cast<void*>(&sequence_b));

    CHECK(!ext_coroutine::coroutine_init(nullptr, slots.data(), 3));
    CHECK(!ext_coroutine::coroutine_init(&ctx, nullptr, 3));
    CHECK(!ext_coroutine::coroutine_init(&ctx, slots.data(), 0));
    CHECK(ext_coroutine::coroutine_init(&ctx, slots.data(), 3));

    CHECK(g_start(-1, 0) == 0 && g_start(3, 0) == 0);
    slots[0].store(nullptr);
    CHECK(g_start(0, 0) == 0);
    slots[0].store(reinterpret_cast<void*>(&multi_yield));
    CHECK(next(0) == 0 && next(9999) == 0);
    CHECK(done(0) == 0 && done(9999) == 0);
    g_yield(55); // yielding outside a coroutine is a safe no-op

    // Basic argument, multiple yields, terminal value and repeated done/next.
    const int64_t basic = g_start(0, 40);
    CHECK(basic > 0 && done(basic) == 0);
    CHECK(next(basic) == 40 && done(basic) == 0);
    CHECK(next(basic) == 41 && done(basic) == 0);
    CHECK(next(basic) == 42 && done(basic) == 1);
    CHECK(done(basic) == 1);             // repeated done
    CHECK(next(basic) == 42);            // next after done returns terminal value

    // Two independent fibers retain their own suspended state.
    const int64_t a = g_start(1, 0), b = g_start(2, 0);
    CHECK(a > 0 && b > 0 && a != b);
    CHECK(next(a) == 1); CHECK(next(b) == 10);
    CHECK(next(a) == 2); CHECK(next(b) == 20);
    CHECK(next(a) == 3 && done(a) == 1);
    CHECK(next(b) == 30 && done(b) == 1);

    // EMBM-v2 plain dispatch path and its clear/error branch.
    void* plain[] = {reinterpret_cast<void*>(&multi_yield)};
    set_dispatch(reinterpret_cast<int64_t>(plain), 1);
    const int64_t plain_co = g_start(0, 7);
    CHECK(plain_co > 0 && next(plain_co) == 7 && next(plain_co) == 8 && next(plain_co) == 9);
    set_dispatch(0, 0);
    CHECK(g_start(0, 1) == 0);

    ext_coroutine::coroutine_reset();
    CHECK(g_start(0, 1) == 0 && next(basic) == 0 && done(basic) == 0);
    CHECK(ext_coroutine::coroutine_init(&ctx, slots.data(), 3));

    // Keyed execution must fail closed and expose a typed status.
    ModuleInstance no_state;
    CHECK(!ext_coroutine::coroutine_init_keyed(no_state));
    CHECK(!ext_coroutine::coroutine_last_start_status_keyed(no_state).ok);

    DispatchTable keyed_table(1);
    keyed_table.set(0, reinterpret_cast<void*>(&keyed_start_entry));
    ModuleInstance inst;
    inst.module_id = "coroutine.coverage";
    inst.logical_slot_count = 1; inst.physical_slot_count = 1;
    inst.entry_table = &keyed_table;
    inst.dispatch_base = reinterpret_cast<int64_t>(keyed_table.base());
    inst.named_entries["main"] = 0;
    inst.ext_state = std::make_shared<RuntimeExtensionState>();
    std::array<uint8_t, 32> material{}; material[0] = 0x42;
    inst.provider = std::make_shared<FixedMaterialProvider>(material);
    CHECK(assemble_identity_dispatch_record(inst));
    CHECK(ext_coroutine::coroutine_init_keyed(inst));
    auto initial = ext_coroutine::coroutine_last_start_status_keyed(inst);
    CHECK(!initial.ok && !initial.unsupported_mode && initial.reason.empty());
    DispatchKeyAdapter adapter(inst.provider);
    context_t keyed_ctx{}; keyed_ctx.budget_remaining = 1'000'000'000; keyed_ctx.max_call_depth = 64;
    auto result = ember_call_keyed_i64(inst, "main", keyed_ctx, 123, adapter);
    CHECK(result.ok && result.value == 0);
    auto status = ext_coroutine::coroutine_last_start_status_keyed(inst);
    CHECK(!status.ok && status.unsupported_mode && status.reason.find("unsupported") != std::string::npos);

    ext_coroutine::coroutine_reset();
    ext_coroutine::coroutine_reset();
    std::puts("coroutine coverage: PASS");
    return 0;
}
