// demo/concurrency/concurrency_demo.cpp — shape (A) host-driven concurrency harness.
//
// Exercises the ember sync extension (atomics, swapbuf, SPSC/MPSC/MPMC queues)
// under REAL multi-thread contention. Each HOST thread owns its own context_t
// (the single-context contract: a context_t is NOT entered concurrently by
// multiple threads) and calls a compiled ember fn via ember_call_i64. The sync
// queues are process-global (the extension's g_store), so threads genuinely
// race through the SAME shared rings + atomics. This is the real test of the
// sync surface under contention.
//
// Compile model (mirrors examples/thread_safety_test):
//   - resolve_imports -> tokenize -> parse -> slot-assign -> register sync
//     natives -> sema -> compile ONCE with use_context_reg=true (B1 mode).
//   - The SAME compiled entry serves N per-thread contexts (r14 = ctx per call).
//   - Each thread: own context_t (private budget/checkpoint), call via ember_call_i64.
//
// Work discipline (bounded batch per call + host re-call loop):
//   - Each ember fn does a BOUNDED batch (BATCH items, RETRIES per item) then
//     returns to the host, which yields + re-calls. This keeps each ember call
//     bounded (no budget exhaustion on a legit busy-wait) and gives the host the
//     deadline + yield (ember has no script-visible yield).
//   - Progress persists in per-role globals (shared backing store, single writer
//     per global -> no race).
//
// Determinism + assertions:
//   - SPSC: producer pushes 1..N; consumer verifies FIFO (v == count+1) + no-loss
//     (count == N). Bounded deadline; both threads join.
//   - MPSC: 2 producers (base 0 + base 100000); 1 consumer. No-loss = each
//     producer's full stream drained (consumer count == 2N).
//   - MPMC: 2 producers + 2 consumers. No-loss = total consumed == 2*MPMC_N.
//   - Atomic: N threads each fetch_add 50000; final value == N*50000.
//
// Link: g++ -std=c++17 -O2 -pthread -Isrc demo/concurrency/concurrency_demo.cpp
//       buildt/libember_frontend.a buildt/libember.a buildt/libember_ext_sync.a
//       -o demo/concurrency/concurrency_demo.exe
#include "import.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"
#include "codegen.hpp"
#include "context.hpp"
#include "engine.hpp"        // ember_call_i64, finalize, free_executable
#include "globals.hpp"        // eval_global_initializers
#include "dispatch_table.hpp"
#include "binding_builder.hpp"
#include "jit_memory.hpp"
#include "lifecycle.hpp"     // get_annotated_functions

#include "ext_sync.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace ember;

static int g_fail = 0;
static void check(bool cond, const char* msg) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

using Clock = std::chrono::steady_clock;
static constexpr auto TIMEOUT = std::chrono::seconds(30);

// Pull a native's C fn-ptr out of the registered table (the natives are static
// in the ext_sync TU, but NativeSig::fn_ptr is the same address the JIT would
// call). Lets the harness create the shared queues on the host side (mirrors
// examples/ext_sync_test::native_ptr).
template <typename Sig>
static Sig native_ptr(const std::unordered_map<std::string,NativeSig>& m, const char* name) {
    auto it = m.find(name);
    return it == m.end() ? nullptr : reinterpret_cast<Sig>(it->second.fn_ptr);
}

// Per-thread trap stub (mirrors thread_safety_test::ts_trap): record reason on
// the ctx that arrived in r14 + longjmp to that ctx's checkpoint.
extern "C" void demo_trap(ember::context_t* ctx, int reason, const char* detail) {
    if (ctx) {
        ctx->last_trap = static_cast<ember::TrapReason>(reason);
        ctx->last_error = detail ? detail : "";
        if (ctx->has_checkpoint) __builtin_longjmp(ctx->checkpoint, 1);
    }
    std::abort();
}

// A compiled module: fns (for cleanup), dispatch table, slot map, globals
// backing store (shared across all per-thread contexts), native table.
struct DemoModule {
    std::vector<CompiledFn> fns;
    DispatchTable table;
    std::unordered_map<std::string,int> slots;
    std::unordered_map<std::string, NativeSig> natives;
    std::vector<uint8_t> globals_store;   // shared backing store for globals
    GlobalsBlock gb;
    Program program;                       // kept for re-seeding globals between tests
    bool ok = false;

    // Re-seed ALL globals (const + mutable) to their declared initial values.
    // Called before each test so the shared globals store starts clean (the
    // const globals like BATCH/RETRIES must NOT be zeroed, and the mutable
    // progress globals must reset to their initial 1/0). Mirrors how a fresh
    // module load would seed globals.
    void reseed_globals() {
        std::memset(globals_store.data(), 0, globals_store.size());
        eval_global_initializers(program, GlobalInitCtx{globals_store, gb.index, gb.types});
    }
};

// Compile the demo ember source with use_context_reg=true (B1 mode). Returns
// a module whose entries can be called from N threads with N contexts.
static bool compile_module(const std::string& src, DemoModule& m) {
    std::unordered_set<std::string> seen; std::string resolved;
    try { resolved = resolve_imports(src, "./demo/concurrency/", seen); }
    catch (const std::exception& e) { std::printf("FAIL: resolve_imports: %s\n", e.what()); return false; }
    auto lr = tokenize(resolved, "<demo>");
    if (!lr.ok) { std::printf("FAIL: lex: %s\n", lr.error.c_str()); return false; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::printf("FAIL: parse: %s\n", pr.error.c_str()); return false; }
    int si=0; for(auto&fn:pr.program.funcs){m.slots[fn.name]=si++;fn.slot=m.slots[fn.name];}
    ember::ext_sync::register_natives(m.natives);
    ember::OpOverloadTable ov;   // sync has no overloads, but sema needs the table ref
    auto layouts = build_struct_layouts(pr.program); pr.program.string_xor_key = 0;
    auto sr = sema(pr.program, m.natives, m.slots, 0, &ov, &layouts);
    if (!sr.ok) {
        std::printf("FAIL: sema (%zu errors):\n", sr.errors.size());
        for (auto& e : sr.errors) std::printf("  line %u: %s\n", e.line, e.msg.c_str());
        return false;
    }
    // Globals: shared backing store (one block for all per-thread contexts).
    m.globals_store.assign(pr.program.globals.size() * 8, 0);
    m.gb.base = int64_t(m.globals_store.data());
    { uint32_t gi=0; for (auto& g : pr.program.globals) { m.gb.index[g.name]=gi++; m.gb.types[g.name]=g.ty.get(); } }
    eval_global_initializers(pr.program, GlobalInitCtx{m.globals_store, m.gb.index, m.gb.types});
    ember::g_globals_for_codegen = nullptr;   // MUST stay null (B1 emit-time fix)
    m.table = DispatchTable(pr.program.funcs.size());
    ember::CodeGenCtx ctx;
    ctx.globals_base = m.gb.base; ctx.dispatch_base = int64_t(m.table.base());
    ctx.natives = &m.natives; ctx.script_slots = &m.slots; ctx.structs = &layouts;
    ctx.globals_index = &m.gb.index; ctx.globals_types = &m.gb.types;
    ctx.use_context_reg = true;                 // B1: r14 = per-call ctx
    ctx.trap_stub = (void*)&demo_trap;
    ctx.emit_budget_checks = true;
    ctx.emit_depth_checks = true;
    ctx.max_call_depth = 64;
    for(auto&fn:pr.program.funcs){
        auto cf = compile_func(fn, ctx); finalize(cf); m.table.set(fn.slot, cf.entry);
        m.fns.push_back(std::move(cf));
    }
    m.program = std::move(pr.program);   // keep for reseed_globals()
    m.ok = true;
    return true;
}

// Look up a fn's compiled entry by name.
static void* entry_of(DemoModule& m, const char* name) {
    auto it = m.slots.find(name);
    if (it == m.slots.end()) return nullptr;
    return m.table.get(it->second);
}

// Call a compiled fn(h: i64)->i64 under a GIVEN context_t (per-thread). Sets
// *trapped if the trap fired (longjmp). Returns the i64 result. On trap,
// *reason is set so the harness can diagnose (budget vs bounds vs ...).
static int64_t call_i64_ctx(void* entry, context_t* ectx, int64_t h, bool* trapped,
                            TrapReason* reason = nullptr) {
    *trapped = false;
    ectx->has_checkpoint = true;
    if (__builtin_setjmp(ectx->checkpoint)) {
        *trapped = true; ectx->has_checkpoint = false;
        if (reason) *reason = ectx->last_trap;
        return 0;
    }
    int64_t r = ember_call_i64(entry, ectx, h);
    ectx->has_checkpoint = false;
    return r;
}

// A worker thread that repeatedly calls `entry(handle)` until the fn returns
// -1 (done) or the deadline passes. Each call gets a fresh checkpoint. The
// thread owns its own context_t. out_last holds the last return value; trapped
// is set if any call trapped.
struct WorkerResult { int64_t last = 0; bool trapped = false; bool deadline = false; TrapReason reason = TrapReason::None; };
static WorkerResult worker_loop(void* entry, int64_t handle, Clock::time_point deadline) {
    WorkerResult r;
    context_t ectx; ectx.budget_remaining = 500'000'000; ectx.max_call_depth = 64;
    while (Clock::now() < deadline) {
        ectx.call_depth = 0;            // reset per call (budget NOT reset)
        bool trapped = false;
        TrapReason reason = TrapReason::None;
        int64_t v = call_i64_ctx(entry, &ectx, handle, &trapped, &reason);
        if (trapped) { r.trapped = true; r.reason = reason; return r; }
        r.last = v;
        if (v == -1) { return r; }       // fn signalled done
        if (v == -2) { r.trapped = true; r.reason = reason; return r; }   // FIFO/range violation -> fail
        std::this_thread::yield();       // host yields between bounded batches
    }
    r.deadline = true;
    return r;
}

// Specialized MPMC consumer worker: the consumer fn is STATELESS (pops a
// bounded batch, returns the count popped this call, no shared mutable global
// -> no race between the two consumer threads). The HOST accumulates the
// per-call counts into a thread-local total and decides when to stop: when the
// producers are done AND the ring is empty (mpmc_size == 0). `producers_done` is
// a C++ atomic the producer threads set on exit. Returns the total this consumer
// popped + status. This is the no-shared-mutable-global design: the two consumer
// threads share ONLY the mpmc ring (internally synchronized).
struct MpmcConsumerResult { int64_t total = 0; bool trapped = false; bool deadline = false; TrapReason reason = TrapReason::None; };
static MpmcConsumerResult mpmc_consumer_worker(void* entry, int64_t handle,
        const std::atomic<bool>& producers_done,
        int64_t (*mpmc_size_fn)(int64_t), Clock::time_point deadline) {
    MpmcConsumerResult r;
    context_t ectx; ectx.budget_remaining = 500'000'000; ectx.max_call_depth = 64;
    while (Clock::now() < deadline) {
        ectx.call_depth = 0;
        bool trapped = false;
        TrapReason reason = TrapReason::None;
        int64_t v = call_i64_ctx(entry, &ectx, handle, &trapped, &reason);
        if (trapped) { r.trapped = true; r.reason = reason; return r; }
        r.total += v;   // v is the count popped this call (>= 0)
        // Stop when producers finished AND the ring is drained.
        if (producers_done.load(std::memory_order_acquire) && mpmc_size_fn(handle) == 0) {
            return r;
        }
        std::this_thread::yield();
    }
    r.deadline = true;
    return r;
}

int main() {
    std::printf("=== ember concurrency demo (shape A: host-driven, real threads) ===\n");

    // Reset the sync extension's host stores (in case a prior run leaked).
    ember::ext_sync::reset();

    // Compile the race module from disk.
    DemoModule m;
    {
        // Read the ember source from disk.
        FILE* f = std::fopen("demo/concurrency/race.ember", "rb");
        if (!f) { std::printf("FAIL: cannot open demo/concurrency/race.ember\n"); return 1; }
        std::fseek(f, 0, SEEK_END); long sz = std::ftell(f); std::fseek(f, 0, SEEK_SET);
        std::string src(sz, '\0'); std::fread(&src[0], 1, sz, f); std::fclose(f);
        if (!compile_module(src, m)) { check(false, "compile race.ember"); return g_fail; }
        check(true, "compile race.ember (B1, use_context_reg=true)");
    }

    // ========================================================================
    // TEST 1: SPSC — 1 producer (push 1..100000) + 1 consumer (verify FIFO +
    //         no-loss). Small cap (256, rounded to 256) forces real contention.
    // ========================================================================
    std::printf("--- SPSC: producer/consumer race (cap 256, N 100000) ---\n");
    {
        // Create the shared SPSC on the host (process-global slot). Pass the
        // handle to both ember fns via ember_call_i64.
        auto spsc_new = native_ptr<int64_t(*)(int64_t)>(m.natives, "spsc_new");
        auto spsc_free = native_ptr<void(*)(int64_t)>(m.natives, "spsc_free");
        int64_t h = spsc_new(256);          // rounded up to 256
        check(h != 0, "spsc_new(256) -> valid handle");

        void* prod = entry_of(m, "spsc_producer_step");
        void* cons = entry_of(m, "spsc_consumer_step");
        check(prod && cons, "SPSC producer/consumer entries resolved");

        // The consumer verifies FIFO (v == count+1) inside ember; the producer
        // pushes 1..100000. Reset the progress globals in the shared store so a
        // re-run is clean (the ember source initializes them to 1/0, but we
        // re-seed here for safety since the store persists across tests).
        // globals_store indices: spsc_prod_next=0, spsc_cons_count=1, ...
        // (set in compile order; we re-zero the whole store for a clean slate.)
        m.reseed_globals();

        const auto dl = Clock::now() + TIMEOUT;
        std::atomic<bool> prod_done{false}, cons_done{false};
        WorkerResult pr, cr;
        std::thread tp([&]{ pr = worker_loop(prod, h, dl); prod_done.store(true); });
        std::thread tc([&]{ cr = worker_loop(cons, h, dl); cons_done.store(true); });
        tp.join(); tc.join();

        bool ok = !pr.trapped && !pr.deadline && pr.last == -1   // producer finished
               && !cr.trapped && !cr.deadline && cr.last == -1;  // consumer finished
        // no-loss: the consumer's progress global (spsc_cons_count) reached N.
        // Read it back from the shared globals store.
        int64_t cons_count = 0;
        {
            auto it = m.gb.index.find("spsc_cons_count");
            if (it != m.gb.index.end()) {
                int64_t* p = reinterpret_cast<int64_t*>(m.gb.base) + it->second;
                cons_count = *p;
            }
        }
        ok &= (cons_count == 100000);
        check(ok, "SPSC no-loss + FIFO (producer done, consumer drained 100000 in order)");
        if (!ok) {
            std::printf("  diag: prod trapped=%d reason=%s last=%lld deadline=%d; cons trapped=%d reason=%s last=%lld deadline=%d; cons_count=%lld\n",
                        pr.trapped, trap_reason_str(pr.reason), (long long)pr.last, pr.deadline,
                        cr.trapped, trap_reason_str(cr.reason), (long long)cr.last, cr.deadline, (long long)cons_count);
        }
        spsc_free(h);
    }

    // ========================================================================
    // TEST 2: MPSC — 2 producers (base 0 + base 100000, 50000 each) + 1 consumer.
    //         cap 256 per producer ring. No-loss = consumer drained 100000.
    // ========================================================================
    std::printf("--- MPSC: 2 producers / 1 consumer race (cap 256, 50000 each) ---\n");
    {
        auto mpsc_new = native_ptr<int64_t(*)(int64_t,int64_t)>(m.natives, "mpsc_new");
        auto mpsc_ph  = native_ptr<int64_t(*)(int64_t,int64_t)>(m.natives, "mpsc_producer_handle");
        auto mpsc_free = native_ptr<void(*)(int64_t)>(m.natives, "mpsc_free");
        int64_t h = mpsc_new(256, 2);     // 2 producers, cap 256 each ring
        check(h != 0, "mpsc_new(256, 2) -> valid container handle");
        int64_t ph0 = mpsc_ph(h, 0), ph1 = mpsc_ph(h, 1);
        check(ph0 != 0 && ph1 != 0, "mpsc producer sub-handles resolved");

        void* pa = entry_of(m, "mpsc_producer_a_step");
        void* pb = entry_of(m, "mpsc_producer_b_step");
        void* cc = entry_of(m, "mpsc_consumer_step");
        check(pa && pb && cc, "MPSC producer A/B + consumer entries resolved");

        m.reseed_globals();

        const auto dl = Clock::now() + TIMEOUT;
        WorkerResult ar, br, cr;
        std::thread ta([&]{ ar = worker_loop(pa, ph0, dl); });   // A gets producer sub-handle 0
        std::thread tb([&]{ br = worker_loop(pb, ph1, dl); });   // B gets producer sub-handle 1
        std::thread tc([&]{ cr = worker_loop(cc, h,  dl); });    // consumer gets the container
        ta.join(); tb.join(); tc.join();

        int64_t cons_count = 0;
        { auto it = m.gb.index.find("mpsc_cons_count");
          if (it != m.gb.index.end()) cons_count = *(reinterpret_cast<int64_t*>(m.gb.base) + it->second); }
        // producer A progress + producer B progress should both be at N+1 (done).
        int64_t a_next = 0, b_next = 0;
        { auto it = m.gb.index.find("mpsc_a_next"); if (it != m.gb.index.end()) a_next = *(reinterpret_cast<int64_t*>(m.gb.base) + it->second); }
        { auto it = m.gb.index.find("mpsc_b_next"); if (it != m.gb.index.end()) b_next = *(reinterpret_cast<int64_t*>(m.gb.base) + it->second); }

        bool ok = !ar.trapped && !ar.deadline && ar.last == -1
               && !br.trapped && !br.deadline && br.last == -1
               && !cr.trapped && !cr.deadline && cr.last == -1
               && cons_count == 100000          // 2 * 50000 drained
               && a_next == 50001 && b_next == 50001;   // both producers pushed all 50000
        check(ok, "MPSC no-loss (2 producers x 50000, consumer drained 100000, both producers done)");
        if (!ok) {
            std::printf("  diag: A trapped=%d last=%lld; B trapped=%d last=%lld; C trapped=%d last=%lld; cons=%lld a_next=%lld b_next=%lld\n",
                        ar.trapped,(long long)ar.last, br.trapped,(long long)br.last, cr.trapped,(long long)cr.last,
                        (long long)cons_count,(long long)a_next,(long long)b_next);
        }
        mpsc_free(h);
    }

    // ========================================================================
    // TEST 3: MPMC — 2 producers + 2 consumers, shared container, cap 2048.
    //         No-loss = total consumed == 2*MPMC_N == 40000.
    // ========================================================================
    std::printf("--- MPMC: 2 producers / 2 consumers race (cap 2048, 20000 each) ---\n");
    {
        auto mpmc_new   = native_ptr<int64_t(*)(int64_t)>(m.natives, "mpmc_new");
        auto mpmc_free  = native_ptr<void(*)(int64_t)>(m.natives, "mpmc_free");
        auto mpmc_size  = native_ptr<int64_t(*)(int64_t)>(m.natives, "mpmc_size");
        int64_t h = mpmc_new(2048);
        check(h != 0, "mpmc_new(2048) -> valid handle");

        void* pa = entry_of(m, "mpmc_producer_a_step");
        void* pb = entry_of(m, "mpmc_producer_b_step");
        void* cc = entry_of(m, "mpmc_consumer_step");
        check(pa && pb && cc, "MPMC producer A/B + consumer entries resolved");

        m.reseed_globals();   // resets progress globals + const BATCH/RETRIES

        const auto dl = Clock::now() + TIMEOUT;
        // producers signal done via a C++ atomic (the consumer worker stops when
        // both producers finished AND the ring is empty).
        std::atomic<bool> producers_done{false};
        std::atomic<int> producers_failed{0};
        WorkerResult ar, br;
        std::thread ta([&]{ ar = worker_loop(pa, h, dl); });
        std::thread tb([&]{ br = worker_loop(pb, h, dl); });
        // Two consumer threads calling the SAME stateless consumer fn; each
        // accumulates its own thread-local total (no shared mutable global -> no
        // globals-backing-store race). They share ONLY the mpmc ring.
        MpmcConsumerResult c0r, c1r;
        std::thread c0([&]{ c0r = mpmc_consumer_worker(cc, h, producers_done, mpmc_size, dl); });
        std::thread c1([&]{ c1r = mpmc_consumer_worker(cc, h, producers_done, mpmc_size, dl); });
        // Wait for both producers to finish, then signal consumers they can stop
        // once the ring is empty.
        ta.join(); tb.join();
        producers_done.store(true, std::memory_order_release);
        c0.join(); c1.join();

        int64_t pa_next = 0, pb_next = 0;
        { auto it = m.gb.index.find("mpmc_pa_next"); if (it != m.gb.index.end()) pa_next = *(reinterpret_cast<int64_t*>(m.gb.base) + it->second); }
        { auto it = m.gb.index.find("mpmc_pb_next"); if (it != m.gb.index.end()) pb_next = *(reinterpret_cast<int64_t*>(m.gb.base) + it->second); }

        bool producers_ok = !ar.trapped && !ar.deadline && ar.last == -1
                         && !br.trapped && !br.deadline && br.last == -1
                         && pa_next == 20001 && pb_next == 20001;
        bool consumers_ok = !c0r.trapped && !c0r.deadline && !c1r.trapped && !c1r.deadline;
        int64_t consumed = c0r.total + c1r.total;
        bool noloss = (consumed == 40000);   // 2 * MPMC_N
        check(producers_ok && consumers_ok && noloss,
              "MPMC no-loss (2 producers x 20000, 2 stateless consumers drained 40000 total)");
        if (!(producers_ok && consumers_ok && noloss)) {
            std::printf("  diag: A t=%d r=%s l=%lld; B t=%d r=%s l=%lld; C0 t=%d r=%s tot=%lld dl=%d; C1 t=%d r=%s tot=%lld dl=%d; consumed=%lld pa=%lld pb=%lld size=%lld\n",
                        ar.trapped,trap_reason_str(ar.reason),(long long)ar.last, br.trapped,trap_reason_str(br.reason),(long long)br.last,
                        c0r.trapped,trap_reason_str(c0r.reason),(long long)c0r.total,c0r.deadline,
                        c1r.trapped,trap_reason_str(c1r.reason),(long long)c1r.total,c1r.deadline,
                        (long long)consumed,(long long)pa_next,(long long)pb_next,(long long)mpmc_size(h));
        }
        mpmc_free(h);
    }

    // ========================================================================
    // TEST 4: ATOMIC — 4 threads each fetch_add 50000 on ONE shared atomic.
    //         final value == 4 * 50000 == 200000. This is the M1 race-safety fix:
    //         concurrent fetch_add on one atomic must not lose increments.
    // ========================================================================
    std::printf("--- ATOMIC: 4 threads x 50000 fetch_add on one atomic ---\n");
    {
        auto atomic_new  = native_ptr<int64_t(*)(int64_t,int64_t)>(m.natives, "atomic_new");
        auto atomic_load = native_ptr<int64_t(*)(int64_t)>(m.natives, "atomic_load");
        auto atomic_free = native_ptr<void(*)(int64_t)>(m.natives, "atomic_free");
        int64_t h = atomic_new(64, 0);
        check(h != 0, "atomic_new(64, 0) -> valid handle");

        void* bump = entry_of(m, "atomic_bumper");
        check(bump, "atomic_bumper entry resolved");

        const auto dl = Clock::now() + TIMEOUT;
        constexpr int NTHREADS = 4;
        std::vector<WorkerResult> rs(NTHREADS);
        std::vector<std::thread> ts;
        for (int i = 0; i < NTHREADS; ++i)
            ts.emplace_back([&, i]{ rs[i] = worker_loop(bump, h, dl); });
        for (auto& t : ts) t.join();

        // atomic_bumper returns -1 (done) after its 50000-fetch_add loop, so each
        // thread runs it EXACTLY ONCE. worker_loop sees -1 and stops. The final
        // atomic value must be NTHREADS * 50000 == 200000 -- no lost increments.
        bool all_done = true;
        for (int i = 0; i < NTHREADS; ++i) {
            if (rs[i].trapped || rs[i].deadline || rs[i].last != -1) all_done = false;
        }
        int64_t val = atomic_load(h);
        check(all_done && val == NTHREADS * 50000,
              "ATOMIC no lost increments (4 x 50000 fetch_add == 200000)");
        if (!(all_done && val == NTHREADS * 50000)) {
            std::printf("  diag: val=%lld; threads:", (long long)val);
            for (int i = 0; i < NTHREADS; ++i)
                std::printf(" [%d t=%d l=%lld dl=%d]", i, rs[i].trapped, (long long)rs[i].last, rs[i].deadline);
            std::printf("\n");
        }
        atomic_free(h);
    }

    std::printf("\nember concurrency demo (shape A): %s\n", g_fail ? "FAIL" : "PASS");
    for(auto&fn:m.fns) if(fn.exec) free_executable(fn.exec);
    ember::ext_sync::reset();
    return g_fail;
}
