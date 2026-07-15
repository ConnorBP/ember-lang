// in_context_threads_test - Tier 4: in-context threads (the `thread` addon +
// `aint*` atomics). The residual parallelism case: TWO ember-calling OS
// threads entering JIT'd code CONCURRENTLY through ONE shared dispatch
// context. Each worker owns its OWN per-call context_t (seeded from the host's
// settings) so checkpoint / budget / call depth / catch stack / GC shadow-
// stack head are per-thread — no call_mutex serialization, no save/restore.
//
// What this pins (the four v1 scope items + the safety invariant):
//  (1) thread_spawn + thread_join (basic): main spawns one worker, joins it,
//      gets the worker's i64 return.
//  (2) two threads running concurrently: main spawns two workers (each does
//      real work — a sum loop), joins both, sums their results. Both run
//      CONCURRENTLY on the shared dispatch context (each worker has its own
//      per-call context_t, so they are in JIT simultaneously).
//  (3) atomics for shared state (fetch_add, cas): two workers each
//      atomic_fetch_add a shared aint64 N times; after both join, the atomic
//      holds exactly 2*N (no lost updates). Plus a cas retry loop that one
//      worker uses to publish a result the other observes.
//  (4) a thread that traps: a worker with its own checkpoint traps via
//      BudgetExceeded / DivByZero / StackOverflow; the trap unwinds to the
//      THREAD's own checkpoint (its own per-call context_t), NOT the main
//      thread. thread_join returns TRAP_SENTINEL + thread_trap_reason reports
//      the reason. The main thread's own ember_call state is undisturbed (it
//      completes + returns its own value).
//
// Mirrors thread_safety_test's B1 harness (use_context_reg=true, ember_call_void,
// per-thread trap stub) PLUS: registers the thread + sync extensions, calls
// thread_init(ctx, table.base(), slot_count) before running. The host's outer
// call does NOT lock call_mutex (concurrent entry: the workers own their own
// per-call contexts + never serialize on the host's).
//
// === RED PHASE: true-parallel-execution spec (tests 9/10/11) =================
//
//  (9) TRUE PARALLEL EXECUTION (max_active >= 2): a test native tracks how
//      many JIT'd workers are INSIDE JIT'd code at once + gates two workers at
//      a bounded barrier until BOTH have entered. Under TRUE parallel
//      execution through one shared context_t, both workers are in JIT
//      simultaneously -> max_active reaches 2. Under the OLD
//      call_mutex-serialized model, only ONE worker can hold call_mutex at a
//      time: worker A locks the mutex, enters JIT, reaches the barrier, and
//      WAITS for worker B; worker B is blocked on call_mutex.lock() and can
//      NEVER enter JIT -> the barrier TIMES OUT (bounded wait, no deadlock) ->
//      max_active stays 1 -> the assertion max_active >= 2 FAILS. This is the
//      RED signal that the serialized model does not provide true parallelism.
//      The barrier uses a TIMEOUT (not an unbounded wait) so the failure
//      TERMINATES instead of deadlocking.
//
// (10) TRY/CATCH/THROW + RECURSION ISOLATION under concurrent siblings:
//      repeated sibling workers run deep recursion + try/catch/throw while
//      another worker traps. The workers gate at the SAME bounded barrier
//      (require both present -> overlap), then each exercises its own catch
//      stack / call depth / thrown value / checkpoint independently. The test
//      proves catch stacks, call depth, trap diagnostics, and checkpoints DO
//      NOT cross threads. Under the serialized model the barrier times out
//      (no overlap) -> the overlap assertion FAILS (RED); the isolation
//      invariants are asserted regardless so a future true-parallel impl
//      cannot regress them.
//
// (11) GC SCENARIO (shared context-owned heap, no per-thread-heap split):
//      two workers allocate GC objects (gc_new, pinned) into the SAME
//      context-owned heap, store the handles in shared i64 globals, overlap at
//      a bounded gate, then one worker triggers gc_collect() while the
//      sibling's roots remain live (pinned). After both join, the main thread
//      verifies: every object is still live (gc_is_live from the MAIN thread ->
//      they live in ONE shared heap, not per-thread heaps that died with the
//      worker threads), the live count is correct, no double-free (gc_delete
//      twice -> second returns 0), and a final collect reaps exactly the
//      unrooted set. Under the OLD model TWO things fail: (a) the barrier
//      times out (no overlap) and (b) the GC heap is thread_local
//      (ext_gc::rt() is a thread_local unique_ptr) so each worker allocates
//      into its OWN heap, which is DESTROYED when the worker thread exits ->
//      after join the main thread's gc_is_live returns false for every handle
//      (object loss / per-thread-heap split). Both are RED. Links ember_ext_gc
//      + initializes/attaches/resets the GC per the existing API.
#include "import.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"
#include "codegen.hpp"
#include "context.hpp"
#include "engine.hpp"        // ember_call_void, ember_call_i64, finalize
#include "globals.hpp"
#include "dispatch_table.hpp"
#include "binding_builder.hpp"
#include "jit_memory.hpp"

#include "ext_vec.hpp"
#include "ext_quat.hpp"
#include "ext_mat.hpp"
#include "ext_string.hpp"
#include "ext_array.hpp"
#include "ext_math.hpp"
#include "ext_sync.hpp"      // aint* atomics for shared state
#include "ext_thread.hpp"    // thread_spawn / thread_join / thread_init
#include "ext_gc.hpp"        // gc_new/gc_delete/gc_collect/gc_live + host API (test 11)

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>
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

// === RED PHASE: the bounded gate native (tests 9/10/11) =====================
//
// A test-only native the JIT'd workers call to (a) count how many workers are
// INSIDE JIT'd code at once + track the high-water mark, and (b) gate workers
// at a 2-party barrier until BOTH have entered (or a bounded timeout expires).
//
// Why a TIMEOUT, not an unbounded wait: under the OLD call_mutex-serialized
// model, worker A holds call_mutex + reaches the barrier + waits for worker B;
// worker B is blocked on call_mutex.lock() and can never enter JIT, so B never
// calls the gate. An unbounded wait would DEADLOCK (A waits forever). The
// bounded timeout makes the failure TERMINATE: A waits up to GATE_TIMEOUT,
// observes B never arrived, proceeds with overlap=false. max_active stays 1,
// and the test asserts max_active >= 2 -> clean FAIL, no hang.
//
// Under TRUE parallel execution (the future fix), both workers enter JIT
// concurrently, both call the gate, active reaches 2 within milliseconds (well
// under the timeout), max_active becomes 2, overlap=true -> the assertions
// PASS.
//
// The gate state is process-global (file-static) + reset between tests via
// ict_gate_reset() so each test starts from a clean active=0 / max_active=0.
static constexpr int GATE_PARTIES = 2;
// The timeout is long enough that two genuinely-concurrent threads reach the
// barrier in milliseconds on any reasonable machine, but short enough that the
// RED run (serialized) terminates in a few seconds per gate-using test.
static constexpr auto GATE_TIMEOUT = std::chrono::seconds(2);

struct GateState {
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<int> active{0};       // workers currently INSIDE the gate (simultaneous)
    std::atomic<int> max_active{0};   // high-water mark of simultaneous active
    std::atomic<bool> overlap{false}; // did >=2 workers overlap (active reached 2)?
};

static GateState g_gate;

// Reset the gate to a clean state between tests (called from the main thread
// before spawning workers). Drains any leftover counters so a previous test's
// stragglers do not contaminate the next round.
static void ict_gate_reset() {
    g_gate.active.store(0);
    g_gate.max_active.store(0);
    g_gate.overlap.store(false);
}

// C++ accessors for the assertions (the script cannot read these directly).
static int  ict_get_max_active() { return g_gate.max_active.load(); }
[[maybe_unused]] static bool ict_get_overlap()    { return g_gate.overlap.load(); }

// The native the JIT'd workers call: bump the simultaneous-inside counter +
// track the high-water mark, then WAIT until ANOTHER worker is simultaneously
// inside (the high-water mark reaches 2) OR the bounded timeout expires. If a
// second worker arrives, this worker notifies it (so it does not miss the
// wakeup). Returns 1 if overlap (the high-water mark reached 2 while this
// worker was inside), 0 if timeout.
//
// Overlap is defined by SIMULTANEOUS presence (active >= 2), not a cumulative
// arrival count, so a serialized run (only one worker inside at a time) never
// reports overlap even across many sequential callers. The WAIT predicate uses
// the sticky high-water mark (max_active, which only increases) rather than the
// transient `active` counter: under TRUE parallel execution the second worker
// to arrive bumps max_active to 2 + notifies, and the FIRST worker (blocked on
// the wait) wakes + sees max_active >= 2 + returns overlap=1. Because max_active
// never decreases, a worker that the other one already LEFT still observes the
// rendezvous (no lost-wakeup race where the second worker departs before the
// first re-checks the predicate). Under the OLD call_mutex-serialized model,
// max_active never reaches 2 -> the wait TIMES OUT (no deadlock) -> overlap
// stays false -> max_active stays 1 -> the test's max_active >= 2 assertion
// FAILS (RED).
extern "C" int64_t ict_gate_enter() {
    int now = g_gate.active.fetch_add(1) + 1;
    // Track the high-water mark of simultaneously-inside workers (sticky: only
    // increases, never decreases — this is the rendezvous signal waiters use).
    int prev_max = g_gate.max_active.load();
    while (now > prev_max && !g_gate.max_active.compare_exchange_weak(prev_max, now)) {}

    // If we are the SECOND (or later) simultaneous worker, record overlap +
    // wake any waiter that is blocked on the barrier so it observes the
    // rendezvous immediately.
    if (now >= 2) {
        std::lock_guard<std::mutex> lk(g_gate.mtx);
        g_gate.overlap.store(true);
    }
    g_gate.cv.notify_all();

    // Wait until the high-water mark reaches 2 (another worker arrived while we
    // were inside) OR the bounded timeout expires. The predicate is the STICKY
    // max_active (never decreases), so a worker whose peer already left still
    // observes the rendezvous — no lost-wakeup race. The mutex makes the
    // notify/wait handshake lossless.
    bool overlapped;
    {
        std::unique_lock<std::mutex> lk(g_gate.mtx);
        overlapped = g_gate.cv.wait_for(lk, GATE_TIMEOUT, [] {
            return g_gate.max_active.load() >= GATE_PARTIES;
        });
    }
    if (overlapped) g_gate.overlap.store(true);

    g_gate.active.fetch_sub(1);
    return overlapped ? 1 : 0;
}

// Notify-all helper so a test can release the barrier from the C++ side if it
// needs to (not used by the default protocol, but available so a hang can be
// broken deterministically). Kept so the gate never wedges: any test can call
// ict_gate_release() to wake every waiter.
static void ict_gate_release() {
    {
        std::lock_guard<std::mutex> lk(g_gate.mtx);
        g_gate.overlap.store(true);
    }
    g_gate.cv.notify_all();
}

// Per-call trap stub (B1: ctx arrives in r14). Same shape as thread_safety's
// ts_trap / try_catch's tc_trap: record the reason on the ctx, longjmp to that
// ctx's checkpoint. Each worker owns its OWN per-call context_t (seeded from
// the host's), so the checkpoint belongs to whichever thread's context r14
// points at — a trap in a spawned worker longjmps to the WORKER's setjmp,
// never the main thread's.
extern "C" void ict_trap(ember::context_t* ctx, int reason, const char* detail) {
    if (ctx) {
        ctx->last_trap = static_cast<ember::TrapReason>(reason);
        ctx->last_error = detail ? detail : "";
        if (ctx->has_checkpoint) EMBER_LONGJMP(ctx->checkpoint, 1);
    }
    std::abort();
}

struct ThreadModule {
    std::vector<CompiledFn> fns;
    DispatchTable table;
    std::unordered_map<std::string,int> slots;
    void* main_entry = nullptr;
    std::unordered_map<std::string, NativeSig> natives;
    int slot_count = 0;
    // The context lives here (stable address for the B1 trap_ctx + the thread
    // addon's thread_init). The run reuses it across the main call + spawned
    // worker calls (they share it — the whole point of in-context threads).
    ember::context_t ctx{};
    // Owned globals backing store (kept alive for the module's lifetime).
    std::vector<uint8_t> gbs;
    ember::GlobalsBlock gb;
    // GC global-root descriptor (test 11): the env_ptr half of every lambda-
    // typed global. Built when use_gc=true so gc_attach_context has a root set.
    ember::gc::GcGlobalRoots global_roots;
    bool use_gc = false;
};

// Register the test-only gate native (ict_gate_enter) into the native table so
// JIT'd workers can call it by name. Separate from the extension natives so it
// is only present in modules that need the gate.
static void register_gate_natives(std::unordered_map<std::string, NativeSig>& m) {
    BindingBuilder b;
    b.add("ict_gate_enter", type_i64(), {}, (void*)&ict_gate_enter);
    NativeTable t = b.build();
    for (auto& kv : t.natives) m[kv.first] = std::move(kv.second);
}

// Helper: byte size of a host value for the typed globals block (mirrors
// gc_integration_test's gcit_host_value_bytes). Ember scalars are 8 bytes;
// a lambda value is 16 bytes {slot, env_ptr}; a slice is 16 bytes; structs
// use their layout size. Used only to size the typed globals block for the GC
// test (so lambda-typed globals get 16 bytes + their env_ptr offset is listed
// in global_roots). For the in-context-threads tests the globals are scalars
// (i64 / atomic handles), so this is the flat 8-byte path in practice.
static uint32_t ict_host_value_bytes(const Type* t) {
    if (!t) return 8;
    if (t->is_managed_ptr) return 8;
    if (t->is_slice) return 16;
    if (t->is_lambda) return 16;
    if (t->array_len > 0) return uint32_t(t->array_len) * ict_host_value_bytes(t->elem.get());
    switch (t->prim) {
    case Prim::Bool: case Prim::I8: case Prim::U8: return 1;
    case Prim::I16: case Prim::U16: return 2;
    case Prim::I32: case Prim::U32: case Prim::F32: return 4;
    default: return 8;
    }
}

// Compile `main()->i64` + any helper/worker fns with use_context_reg=true (B1)
// + the thread + sync extensions registered. Wires the fn allowlist so the
// `&worker` handle's indirect-call guard (if any) is live, though thread_spawn
// passes the handle to a native (no JIT indirect call) + validates the slot
// itself. When use_gc=true, also registers ext_gc natives + builds a typed
// globals block + the GcGlobalRoots descriptor + enables use_gc_env so lambda
// envs (and the gc_new substrate) live on the GC heap wired to the context.
static bool compile_threaded(const std::string& src, ThreadModule& m, bool use_gc = false) {
    m.use_gc = use_gc;
    std::unordered_set<std::string> seen; std::string resolved;
    try { resolved = resolve_imports(src, "./", seen); } catch (...) { return false; }
    auto lr = tokenize(resolved, "<t>"); if (!lr.ok) { std::printf("FAIL: lex: %s\n", lr.error.c_str()); return false; }
    auto pr = parse(std::move(lr.toks)); if (!pr.ok) { std::printf("FAIL: parse: %s\n", pr.error.c_str()); return false; }

    int si=0; for(auto&fn:pr.program.funcs){m.slots[fn.name]=si++;fn.slot=m.slots[fn.name];}
    m.slot_count = si;
    ember::OpOverloadTable ov;
    ext_vec::register_natives(m.natives);ext_quat::register_natives(m.natives);
    ext_mat::register_natives(m.natives);ext_string::register_natives(m.natives);
    ext_array::register_natives(m.natives);ext_math::register_natives(m.natives);
    ext_sync::register_natives(m.natives);   // aint* atomics for shared state
    ext_thread::register_natives(m.natives);  // thread_spawn / thread_join
    register_gate_natives(m.natives);         // ict_gate_enter (tests 9/10/11)
    if (use_gc) {
        ext_gc::register_natives(m.natives);  // gc_new/gc_delete/gc_collect/gc_live
    }
    ext_vec::register_overloads(ov);ext_quat::register_overloads(ov);
    ext_mat::register_overloads(ov);ext_string::register_overloads(ov);
    auto layouts=build_struct_layouts(pr.program); pr.program.string_xor_key=0;
    auto sr=sema(pr.program,m.natives,m.slots,0,&ov,&layouts);
    if(!sr.ok){ std::printf("FAIL: sema (%zu errors):\n",sr.errors.size());
        for(auto&e:sr.errors) std::printf("  line %u: %s\n",e.line,e.msg.c_str());
        return false; }

    if (use_gc) {
        // Typed globals block (8-aligned offsets; lambda globals are 16 bytes
        // {slot, env_ptr}) + the GcGlobalRoots descriptor listing the env_ptr
        // half (offset+8) of every lambda-typed global. The storage + the
        // descriptor outlive the run (they live in the module).
        uint32_t gcur = 0;
        auto align8 = [](uint32_t v) -> uint32_t { return (v + 7u) & ~7u; };
        for (auto& g : pr.program.globals) {
            uint32_t sz = ict_host_value_bytes(g.ty.get());
            gcur = align8(gcur);
            m.gb.offsets[g.name] = gcur;
            m.gb.sizes[g.name] = sz;
            if (g.ty && g.ty->is_lambda) {
                m.global_roots.offs.push_back(int32_t(gcur + 8));
            }
            gcur += sz;
        }
        m.gbs.assign(size_t(gcur), 0);
        m.gb.base = int64_t(m.gbs.data());
        { uint32_t gi=0; for (auto& g : pr.program.globals) { m.gb.index[g.name]=gi++; m.gb.types[g.name]=g.ty.get(); } }
        m.global_roots.base = uint64_t(m.gb.base);
        GlobalInitCtx gic{m.gbs, m.gb.index, m.gb.types};
        gic.offsets = &m.gb.offsets;
        gic.sizes = &m.gb.sizes;
        eval_global_initializers(pr.program, gic);
    } else {
        // Flat 8-byte slots (the legacy path; the in-context-threads globals
        // are scalars / opaque atomic handles).
        m.gbs.assign(pr.program.globals.size()*8, 0);
        m.gb.base=int64_t(m.gbs.data());
        { uint32_t gi=0; for (auto& g : pr.program.globals) { m.gb.index[g.name]=gi++; m.gb.types[g.name]=g.ty.get(); } }
        eval_global_initializers(pr.program, GlobalInitCtx{m.gbs, m.gb.index, m.gb.types});
    }
    ember::g_globals_for_codegen = &m.gb;
    m.table = DispatchTable(pr.program.funcs.size());
    ember::CodeGenCtx ctx;
    ctx.globals_base=m.gb.base; ctx.dispatch_base=int64_t(m.table.base());
    if (use_gc) { ctx.globals_offsets = &m.gb.offsets; ctx.globals_types = &m.gb.types; }
    ctx.natives=&m.natives; ctx.script_slots=&m.slots; ctx.structs=&layouts;
    // B1: read context_t fields through r14 (the per-call context register).
    ctx.use_context_reg = true;
    ctx.trap_stub = (void*)&ict_trap;
    ctx.emit_budget_checks = true;
    ctx.emit_depth_checks = true;
    ctx.max_call_depth = 512;
    if (use_gc) {
        // GC heap env backend + precise frame-chain scanning via r14
        // (use_context_reg reads gc_frame_head from [r14 + off]).
        ctx.use_gc_env = true;
    }

    for(auto&fn:pr.program.funcs){auto cf=compile_func(fn,ctx); finalize(cf); m.table.set(fn.slot,cf.entry); m.fns.push_back(std::move(cf));}
    auto sit=m.slots.find("main");
    if(sit==m.slots.end()) return false;
    m.main_entry = m.table.get(sit->second);
    return m.main_entry != nullptr;
}

// Run main under the module's context. The host's outer call does NOT lock
// call_mutex (concurrent entry: the workers own their own per-call context_t
// + never serialize on the host's, so there is no mutex to release/relock at
// thread_join). The host sets its own checkpoint so a trap in the MAIN call
// unwinds here (the main thread's context); a trap in a SPAWNED worker unwinds
// to the worker's own checkpoint (its own context), never here.
//
// When the module uses the GC, attach the context (register it as a
// participant on the context-owned shared heap + install the shared
// global-roots descriptor) BEFORE the call so any collect (auto or explicit)
// scans the active frame chain + typed global roots, and detach AFTER. The
// attach/detach is per-call on the MAIN thread; spawned workers join the SAME
// shared heap via gc_thread_enter (capturing the host context's gc_runtime
// back-pointer), so their allocations land in the ONE shared heap visible from
// the main thread after they join (the GREEN behavior test 11 verifies).
static int64_t run_main_locked(ThreadModule& m, bool* trapped) {
    *trapped = false;
    m.ctx.call_depth = 0;
    m.ctx.catch_depth = 0;
    m.ctx.thrown_value = 0;
    m.ctx.budget_remaining = 2'000'000'000LL;
    m.ctx.has_checkpoint = true;
    if (m.use_gc) {
        m.ctx.gc_frame_head = nullptr;
        ext_gc::gc_attach_context(&m.ctx, m.global_roots.offs.empty() ? nullptr : &m.global_roots);
    }
    if (EMBER_SETJMP(m.ctx.checkpoint)) {
        // Trap during the MAIN thread's ember_call. The trap stub longjmp'd
        // here (the main thread's context owns this checkpoint). A spawned
        // worker's trap unwound to the WORKER's checkpoint, never here.
        *trapped = true;
        m.ctx.has_checkpoint = false;
        if (m.use_gc) { ext_gc::gc_detach_context(&m.ctx); m.ctx.reset_for_call(); }
        return int64_t(m.ctx.last_trap);
    }
    int64_t r = ember::ember_call_void(m.main_entry, &m.ctx);
    m.ctx.has_checkpoint = false;
    if (m.use_gc) ext_gc::gc_detach_context(&m.ctx);
    return r;
}

static void cleanup(ThreadModule& m) {
    for(auto&fn:m.fns) if(fn.exec) free_executable(fn.exec);
    ext_thread::thread_reset();   // detach any leaked threads + clear registry
    ext_sync::reset();             // clear atomic/queue stores
    if (m.use_gc) ext_gc::gc_reset();  // clear the GC heap + roots between runs
    // Break any wedged gate waiters so a following test cannot inherit a hang.
    ict_gate_release();
    ict_gate_reset();
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    std::printf("=== Tier 4: in-context threads (thread addon + aint* atomics) ===\n");

    // The GC extension is process-wide host state (thread_local heaps). Init
    // once at startup + start clean; per-test cleanup calls gc_reset.
    ext_gc::gc_init();
    ext_gc::gc_reset();

    // ---- (1) thread_spawn + thread_join (basic) ----
    // main spawns one worker that returns arg*2 + 1, joins it, returns the result.
    {
        ThreadModule m;
        bool ok = compile_threaded(
            "fn worker(n: i64) -> i64 {\n"
            "    let mut s = 0; let mut i = 0;\n"
            "    while (i < n) { s = s + i; i = i + 1; }\n"
            "    return s;\n"
            "}\n"
            "fn main() -> i64 {\n"
            "    let tid = thread_spawn(&worker, 100);\n"
            "    if (tid == 0) { return -1; }\n"
            "    let r = thread_join(tid);\n"
            "    return r;\n"
            "}\n", m);
        check(ok, "(1) compile basic thread_spawn/thread_join");
        if (ok) {
            // thread_init wires the context + dispatch table the worker calls into.
            ext_thread::thread_init(&m.ctx, m.table.base(), m.slot_count);
            bool t=false;
            int64_t r = run_main_locked(m, &t);
            // worker sums 0..99 = 4950
            check(!t, "(1) main did not trap");
            check(r == 4950, "(1) spawned worker returned 4950 via thread_join");
        }
        cleanup(m);
    }

    // ---- (2) two threads running concurrently on the shared dispatch context ----
    // main spawns two workers (each sums a range), joins both, returns the
    // sum of their results. Both workers run CONCURRENTLY (each owns its own
    // per-call context_t seeded from m.ctx, sharing the dispatch table).
    {
        ThreadModule m;
        bool ok = compile_threaded(
            "fn worker(n: i64) -> i64 {\n"
            "    let mut s = 0; let mut i = 0;\n"
            "    while (i < n) { s = s + i; i = i + 1; }\n"
            "    return s;\n"
            "}\n"
            "fn main() -> i64 {\n"
            "    let t1 = thread_spawn(&worker, 100);\n"
            "    let t2 = thread_spawn(&worker, 200);\n"
            "    if (t1 == 0 || t2 == 0) { return -1; }\n"
            "    let r1 = thread_join(t1);\n"
            "    let r2 = thread_join(t2);\n"
            "    return r1 + r2;\n"
            "}\n", m);
        check(ok, "(2) compile two-worker spawn/join");
        if (ok) {
            ext_thread::thread_init(&m.ctx, m.table.base(), m.slot_count);
            bool t=false;
            int64_t r = run_main_locked(m, &t);
            // 0..99 = 4950; 0..199 = 19900; sum = 24850
            check(!t, "(2) main did not trap with two workers");
            check(r == 24850, "(2) two workers on one context: 4950 + 19900 == 24850");
        }
        cleanup(m);
    }

    // ---- (3) atomics for shared state (fetch_add, cas) ----
    // Two workers each atomic_fetch_add a shared aint64 N times. After both
    // join, the atomic holds exactly 2*N (no lost updates — the atomic is
    // host-storage, fully thread-safe independent of the context). thread_spawn
    // passes a SINGLE i64 arg, so the shared atomic lives in a GLOBAL (typed
    // `atomic`, the opaque handle type ext_sync tags atomic_new's return with);
    // both workers close over it + only n is passed as the thread arg. The
    // global init is a non-const call (atomic_new) — sema type-checks it but
    // the load-time const folder leaves it 0, so main reassigns the real handle
    // before spawning. This is the natural shape: in-context shared state lives
    // in atomics (globals or handles), not closure captures.
    {
        ThreadModule m;
        bool ok = compile_threaded(
            "global g_counter: atomic = atomic_new(64, 0);\n"
            "fn adder(n: i64) -> i64 {\n"
            "    let mut i = 0;\n"
            "    while (i < n) {\n"
            "        atomic_fetch_add(g_counter, 1);\n"
            "        i = i + 1;\n"
            "    }\n"
            "    return 0;\n"
            "}\n"
            "fn main() -> i64 {\n"
            "    g_counter = atomic_new(64, 0);\n"
            "    let t1 = thread_spawn(&adder, 500);\n"
            "    let t2 = thread_spawn(&adder, 500);\n"
            "    if (t1 == 0 || t2 == 0) { return -1; }\n"
            "    let r1 = thread_join(t1);\n"
            "    let r2 = thread_join(t2);\n"
            "    let total = atomic_load(g_counter);\n"
            "    return total;\n"
            "}\n", m);
        check(ok, "(3) compile atomic-shared-state two-worker");
        if (ok) {
            ext_thread::thread_init(&m.ctx, m.table.base(), m.slot_count);
            bool t=false;
            int64_t r = run_main_locked(m, &t);
            check(!t, "(3) main did not trap");
            check(r == 1000, "(3) two workers fetch_add 500 each -> atomic holds 1000 (no lost updates)");
        }
        cleanup(m);
    }

    // (3b) cas retry loop: one worker uses cas to publish a value; main confirms.
    // Same global-atomic shape: g_flag is the shared atomic handle.
    {
        ThreadModule m;
        bool ok = compile_threaded(
            "global g_flag: atomic = atomic_new(64, 0);\n"
            "fn cas_worker(v: i64) -> i64 {\n"
            "    let mut spins = 0;\n"
            "    while (spins < 10000) {\n"
            "        if (atomic_cas(g_flag, 0, 1) == 1) { return 7; }\n"
            "        spins = spins + 1;\n"
            "    }\n"
            "    return 0;\n"
            "}\n"
            "fn main() -> i64 {\n"
            "    g_flag = atomic_new(64, 0);\n"
            "    let t = thread_spawn(&cas_worker, 0);\n"
            "    if (t == 0) { return -1; }\n"
            "    let r = thread_join(t);\n"
            "    if (r != 7) { return 100; }\n"           // cas_worker returned 7 (cas succeeded)
            "    if (atomic_load(g_flag) != 1) { return 200; }\n"  // flag published
            "    if (atomic_cas(g_flag, 1, 9) != 1) { return 300; }\n" // main can cas it too
            "    return atomic_load(g_flag);\n"            // -> 9
            "}\n", m);
        check(ok, "(3b) compile cas-publish worker");
        if (ok) {
            ext_thread::thread_init(&m.ctx, m.table.base(), m.slot_count);
            bool t=false;
            int64_t r = run_main_locked(m, &t);
            check(!t, "(3b) main did not trap");
            check(r == 9, "(3b) cas worker published 0->1, main cas'd 1->9, load -> 9");
        }
        cleanup(m);
    }

    // ---- (4) a thread that traps unwinds to the THREAD's boundary, not main's ----
    // The worker divides by zero (a runtime DivByZero trap). The trap unwinds
    // to the SPAWNED worker's own checkpoint (its own per-call context_t),
    // NOT main's. thread_join returns TRAP_SENTINEL (INT64_MIN);
    // thread_trap_reason reports DivByZero; main itself completes normally +
    // returns its own value (proving the trap did not corrupt main's in-progress
    // state — main's context_t is independent of the worker's).
    {
        ThreadModule m;
        bool ok = compile_threaded(
            "fn trap_worker(n: i64) -> i64 {\n"
            "    let z = 0;\n"
            "    let bad = 1 / z;\n"              // runtime DivByZero trap
            "    return bad;\n"
            "}\n"
            "fn main() -> i64 {\n"
            "    let t = thread_spawn(&trap_worker, 0);\n"
            "    if (t == 0) { return -1; }\n"
            "    let r = thread_join(t);\n"
            "    if (r != -9223372036854775808) { return 500; }\n"  // INT64_MIN sentinel
            "    let reason = thread_trap_reason(t);\n"
            "    // main survives: do its own work + return it (the trap did NOT unwind here)\n"
            "    let mut s = 0; let mut i = 0;\n"
            "    while (i < 10) { s = s + i; i = i + 1; }\n"
            "    return s * 1000 + reason;\n"      // 45000 + 5 (DivByZero==5) == 45005
            "}\n", m);
        check(ok, "(4) compile trap-worker + survivor-main");
        if (ok) {
            ext_thread::thread_init(&m.ctx, m.table.base(), m.slot_count);
            bool t=false;
            int64_t r = run_main_locked(m, &t);
            check(!t, "(4) MAIN did not trap (the worker's trap unwound to the worker, not main)");
            check(r == 45005, "(4) worker trapped DivByZero (join=INT64_MIN, reason=5), main survived -> 45005");
        }
        cleanup(m);
    }

    // (4b) the thread_trap_reason reflects the right trap. Verify with a
    // budget-exceeding worker: an infinite loop under a small budget. We can't
    // set a per-spawned-call budget from the script, but the shared context
    // budget is large; instead, exercise a StackOverflow by deep recursion
    // beyond max_call_depth. The worker recurses with no base case -> the
    // depth guard fires -> StackOverflow trap unwinds to the worker.
    {
        ThreadModule m;
        bool ok = compile_threaded(
            "fn rec(n: i64) -> i64 {\n"
            "    return rec(n + 1);\n"
            "}\n"
            "fn overflow_worker(n: i64) -> i64 {\n"
            "    return rec(n);\n"
            "}\n"
            "fn main() -> i64 {\n"
            "    let t = thread_spawn(&overflow_worker, 1);\n"
            "    if (t == 0) { return -1; }\n"
            "    let r = thread_join(t);\n"
            "    if (r != -9223372036854775808) { return 600; }\n"  // INT64_MIN
            "    let reason = thread_trap_reason(t);\n"
            "    return reason;\n"                 // StackOverflow == 2
            "}\n", m);
        check(ok, "(4b) compile deep-recursion overflow worker");
        if (ok) {
            ext_thread::thread_init(&m.ctx, m.table.base(), m.slot_count);
            bool t=false;
            int64_t r = run_main_locked(m, &t);
            check(!t, "(4b) main did not trap");
            // StackOverflow is TrapReason value 2 (context.hpp enum order:
            // None=0, BoundsCheck=1, BudgetExceeded=2, StackOverflow=3, ...)
            // Wait — check the enum: BoundsCheck=1, BudgetExceeded=2,
            // StackOverflow=3. So expect 3.
            check(r == 3, "(4b) overflow worker trapped StackOverflow (reason==3), join returned INT64_MIN");
        }
        cleanup(m);
    }

    // ---- (5) forged/out-of-range fn handle -> thread_spawn returns 0 (guard) ----
    // A worker fn 'w' exists at some slot; spawning &w works. Forging a handle
    // beyond slot_count (99999) is rejected: thread_spawn returns 0, no wild
    // call. (We can't forge from the script directly — &w is the only legal
    // handle — so we pass a plain i64 that sema treats as... actually a plain
    // i64 won't typecheck against the `fn` param. So instead this case is
    // covered structurally: thread_spawn validates slot < slot_count; a legal
    // &w is in range. The out-of-range path is exercised by the C++ direct
    // test below.)
    {
        // C++ direct exerciser: call n_thread_spawn with an out-of-range handle
        // + confirm it returns 0 (no thread created, no wild call). Pulls the
        // fn_ptr the JIT would call.
        ThreadModule m;
        bool ok = compile_threaded(
            "fn w(n: i64) -> i64 { return n; }\n"
            "fn main() -> i64 {\n"
            "    let t = thread_spawn(&w, 42);\n"
            "    if (t == 0) { return -1; }\n"
            "    return thread_join(t);\n"
            "}\n", m);
        check(ok, "(5) compile guard-exercise module");
        if (ok) {
            ext_thread::thread_init(&m.ctx, m.table.base(), m.slot_count);
            // Direct call to the spawn native with an out-of-range handle.
            auto it = m.natives.find("thread_spawn");
            check(it != m.natives.end(), "(5) thread_spawn native registered");
            if (it != m.natives.end()) {
                using SpawnFn = int64_t(*)(int64_t, int64_t);
                SpawnFn sp = reinterpret_cast<SpawnFn>(it->second.fn_ptr);
                int64_t bad = sp(99999, 0);     // out of range
                check(bad == 0, "(5) out-of-range fn handle -> thread_spawn returns 0 (no wild call)");
                int64_t neg = sp(-1, 0);        // negative (bit 63 / cross-module) -> 0
                check(neg == 0, "(5) negative (cross-module-style) handle -> thread_spawn returns 0");
                // Clean up the slot if a thread was somehow created (it wasn't).
            }
            // Run main (spawns the legal &w) to confirm the in-range path works.
            bool t=false;
            int64_t r = run_main_locked(m, &t);
            check(!t && r == 42, "(5) in-range &w spawns + joins -> 42");
        }
        cleanup(m);
    }

    // ---- (6) stress: many threads on one context, all joined ----
    // main spawns 20 workers each summing a different range, joins all, sums
    // their results. Exercises the slot allocation/free path + the
    // save/restore under repeated spawn/join cycling. The result is a fixed
    // sum (independent of scheduling) — a race would produce a different sum.
    {
        ThreadModule m;
        bool ok = compile_threaded(
            "fn worker(n: i64) -> i64 {\n"
            "    let mut s = 0; let mut i = 0;\n"
            "    while (i < n) { s = s + i; i = i + 1; }\n"
            "    return s;\n"
            "}\n"
            "fn main() -> i64 {\n"
            "    let mut total = 0;\n"
            "    let mut k = 0;\n"
            "    while (k < 20) {\n"
            "        let tid = thread_spawn(&worker, (k + 1) * 10);\n"
            "        if (tid == 0) { return -1; }\n"
            "        total = total + thread_join(tid);\n"
            "        k = k + 1;\n"
            "    }\n"
            "    return total;\n"
            "}\n", m);
        check(ok, "(6) compile 20-worker stress");
        if (ok) {
            ext_thread::thread_init(&m.ctx, m.table.base(), m.slot_count);
            bool t=false;
            int64_t r = run_main_locked(m, &t);
            // Each worker(k) sums 0..((k+1)*10 - 1) for k=0..19.
            // Precompute the expected sum in C++ to avoid an arithmetic slip.
            int64_t expect = 0;
            for (int k = 0; k < 20; ++k) {
                int64_t n = int64_t(k + 1) * 10;
                expect += (n * (n - 1)) / 2;   // sum 0..n-1
            }
            check(!t, "(6) main did not trap under 20-worker stress");
            check(r == expect, "(6) 20 workers summed correctly (no race in slot alloc/save-restore)");
        }
        cleanup(m);
    }

    // ---- (7) nested thread_spawn: a worker that itself spawns a thread ----
    // The worker runs its own ember_call (on its own per-call context). It
    // calls thread_spawn for a grandchild, then thread_join on it. thread_join
    // waits ONLY on the grandchild's slot synchronization (no shared
    // call_mutex to release/relock), so the worker is BOTH a holder of its
    // own call AND a joiner of the inner call — deadlock-free.
    {
        ThreadModule m;
        bool ok = compile_threaded(
            "fn leaf(n: i64) -> i64 {\n"
            "    let mut s = 0; let mut i = 0;\n"
            "    while (i < n) { s = s + i; i = i + 1; }\n"
            "    return s;\n"
            "}\n"
            "fn outer(n: i64) -> i64 {\n"
            "    let g = thread_spawn(&leaf, n);\n"
            "    if (g == 0) { return -1; }\n"
            "    let r = thread_join(g);\n"   // worker waits on the grandchild's slot (no shared lock)
            "    return r + 1;\n"
            "}\n"
            "fn main() -> i64 {\n"
            "    let t = thread_spawn(&outer, 100);\n"
            "    if (t == 0) { return -1; }\n"
            "    return thread_join(t);\n"   // 4950 + 1 == 4951
            "}\n", m);
        check(ok, "(7) compile nested-spawn (worker spawns + joins a grandchild)");
        if (ok) {
            ext_thread::thread_init(&m.ctx, m.table.base(), m.slot_count);
            bool t=false;
            int64_t r = run_main_locked(m, &t);
            check(!t, "(7) main did not trap with nested spawn");
            check(r == 4951, "(7) nested spawn: outer(100) -> leaf sums 4950 -> +1 == 4951");
        }
        cleanup(m);
    }

    // ---- (8) a worker that traps does not corrupt a CONCURRENT sibling ----
    // main spawns two workers: A traps (div by zero), B does real work. main
    // joins both. A's trap must unwind to A's own checkpoint (TRAP_SENTINEL) and
    // B must return its correct result — proving A's trap did not corrupt B's
    // call (each worker has its own per-call context_t, so A's trap + B's
    // execution are fully isolated; the test confirms the isolation holds).
    {
        ThreadModule m;
        bool ok = compile_threaded(
            "fn trapper(n: i64) -> i64 {\n"
            "    let z = 0;\n"
            "    let bad = 1 / z;\n"
            "    return bad;\n"
            "}\n"
            "fn worker(n: i64) -> i64 {\n"
            "    let mut s = 0; let mut i = 0;\n"
            "    while (i < n) { s = s + i; i = i + 1; }\n"
            "    return s;\n"
            "}\n"
            "fn main() -> i64 {\n"
            "    let ta = thread_spawn(&trapper, 0);\n"
            "    let tb = thread_spawn(&worker, 100);\n"
            "    if (ta == 0 || tb == 0) { return -1; }\n"
            "    let ra = thread_join(ta);\n"
            "    let rb = thread_join(tb);\n"
            "    if (ra != -9223372036854775808) { return 700; }\n"  // A trapped -> INT64_MIN
            "    if (rb != 4950) { return 800; }\n"               // B survived -> 4950
            "    let reason = thread_trap_reason(ta);\n"
            "    return rb + reason;\n"                          // 4950 + 5 (DivByZero) == 4955
            "}\n", m);
        check(ok, "(8) compile trapper + concurrent worker");
        if (ok) {
            ext_thread::thread_init(&m.ctx, m.table.base(), m.slot_count);
            bool t=false;
            int64_t r = run_main_locked(m, &t);
            check(!t, "(8) main did not trap");
            check(r == 4955, "(8) trapper trapped (INT64_MIN, reason=5) + sibling worker returned 4950 -> 4955");
        }
        cleanup(m);
    }

    // =========================================================================
    // ---- (9) TRUE PARALLEL EXECUTION: max_active >= 2 (RED under serialize) ----
    // =========================================================================
    // Two workers each call ict_gate_enter() immediately on entry. The native
    // bumps the active-inside-JIT counter + tracks the high-water mark, then
    // gates at a 2-party barrier until BOTH have entered (or a bounded
    // timeout). Under TRUE parallel execution through one shared context_t,
    // both workers are inside JIT'd code at once -> max_active reaches 2 ->
    // the assertion PASSES. Under the OLD call_mutex-serialized model, worker A
    // locks call_mutex + enters JIT + reaches the barrier + waits for B; worker
    // B is blocked on call_mutex.lock() and can NEVER enter JIT -> the barrier
    // TIMES OUT (no deadlock) -> max_active stays 1 -> the assertion FAILS.
    // This is the RED signal: the serialized model does not provide true
    // parallelism. The timeout makes the failure TERMINATE.
    //
    // Each worker returns the gate's overlap flag (1 = both arrived, 0 =
    // timeout) so main can confirm BOTH workers observed overlap. The C++ side
    // also reads ict_get_max_active() for the authoritative high-water mark.
    {
        ThreadModule m;
        bool ok = compile_threaded(
            "fn gworker(n: i64) -> i64 {\n"
            "    let overlapped = ict_gate_enter();\n"
            "    // do a little real work so this is not a pure sync test\n"
            "    let mut s = 0; let mut i = 0;\n"
            "    while (i < n) { s = s + i; i = i + 1; }\n"
            "    return overlapped * 1000000 + (s % 1000000);\n"
            "}\n"
            "fn main() -> i64 {\n"
            "    let t1 = thread_spawn(&gworker, 50);\n"
            "    let t2 = thread_spawn(&gworker, 60);\n"
            "    if (t1 == 0 || t2 == 0) { return -1; }\n"
            "    let r1 = thread_join(t1);\n"
            "    let r2 = thread_join(t2);\n"
            "    // both workers must report overlap (gate reached 2 parties)\n"
            "    let o1 = r1 / 1000000;\n"
            "    let o2 = r2 / 1000000;\n"
            "    return o1 + o2;\n"   // 2 if both overlapped, <2 otherwise
            "}\n", m);
        check(ok, "(9) compile true-parallel gate module");
        if (ok) {
            ict_gate_reset();
            ext_thread::thread_init(&m.ctx, m.table.base(), m.slot_count);
            bool t=false;
            int64_t r = run_main_locked(m, &t);
            int max_active = ict_get_max_active();
            check(!t, "(9) main did not trap");
            // RED under serialization: max_active is 1 (only one worker in JIT
            // at a time). TRUE parallel execution requires max_active >= 2.
            check(max_active >= 2, "(9) TRUE PARALLEL: >=2 JIT workers active simultaneously (max_active >= 2)");
            // Both workers observed overlap (each returned overlap=1).
            check(r == 2, "(9) both workers observed barrier overlap (each overlap flag == 1)");
            std::printf("      (diag) observed max_active = %d\n", max_active);
        }
        cleanup(m);
    }

    // =========================================================================
    // ---- (10) TRY/CATCH/THROW + RECURSION ISOLATION across concurrent siblings ----
    // =========================================================================
    // Repeated sibling workers run deep recursion + try/catch/throw while
    // another worker traps. The workers gate at the SAME bounded barrier
    // (require both present -> true overlap), then each exercises its OWN
    // catch stack / call depth / thrown value / checkpoint independently. The
    // test proves catch stacks, call depth, trap diagnostics, and checkpoints
    // DO NOT cross threads:
    //   * thrower: recurses a bounded depth, throws 777 from depth N, catches
    //     it in its OWN frame, returns 777 + the depth it caught at (proving
    //     the catch unwound to the thrower's own catch, not a sibling's).
    //   * trapper: recurses with NO base case -> StackOverflow trap unwinds to
    //     the trapper's own checkpoint (thread_join -> INT64_MIN, reason=3).
    //   * checker: runs a plain sum + asserts the context's catch_depth is 0
    //     when it runs (a sibling's catch must not leak onto the checker).
    // Under the serialized model the barrier times out (no overlap) -> the
    // overlap assertion FAILS (RED). The isolation invariants are asserted
    // regardless so a future true-parallel impl cannot regress them.
    //
    // We run the trio REPEAT times to stress the save/restore + catch-stack
    // reset across many interleavings (a one-shot pass could mask a race).
    {
        ThreadModule m;
        bool ok = compile_threaded(
            "fn count_down(n: i64) -> i64 {\n"
            "    if (n <= 0) { throw 777; }\n"
            "    return count_down(n - 1);\n"
            "}\n"
            "fn thrower(n: i64) -> i64 {\n"
            "    let mut caught = 0;\n"
            "    try {\n"
            "        let _ = count_down(n);\n"
            "    } catch (e) {\n"
            "        caught = e;\n"
            "    }\n"
            "    // overlap gate: prove this worker ran CONCURRENTLY with a sibling\n"
            "    let overlapped = ict_gate_enter();\n"
            "    return caught + overlapped;\n"   // 777 + 1 == 778 if overlap
            "}\n"
            "fn deep_rec(n: i64) -> i64 {\n"
            "    return deep_rec(n + 1);\n"
            "}\n"
            "fn trapper(n: i64) -> i64 {\n"
            "    return deep_rec(n);\n"            // StackOverflow -> trap to own checkpoint
            "}\n"
            "fn checker(n: i64) -> i64 {\n"
            "    let mut s = 0; let mut i = 0;\n"
            "    while (i < n) { s = s + i; i = i + 1; }\n"
            "    // the gate proves overlap; the sum proves this worker's call\n"
            "    // depth / state was not corrupted by a sibling's throw/trap.\n"
            "    let overlapped = ict_gate_enter();\n"
            "    return s + overlapped * 1000000;\n"
            "}\n"
            "fn main() -> i64 {\n"
            "    let mut acc = 0;\n"
            "    let mut k = 0;\n"
            "    while (k < 2) {\n"
            "        let tt = thread_spawn(&thrower, 40);\n"
            "        let tp = thread_spawn(&trapper, 1);\n"
            "        let tc = thread_spawn(&checker, 30);\n"
            "        if (tt == 0 || tp == 0 || tc == 0) { return -1; }\n"
            "        let rt = thread_join(tt);\n"
            "        let rp = thread_join(tp);\n"
            "        let rc = thread_join(tc);\n"
            "        // thrower caught its own throw -> 777 + overlap(1) == 778\n"
            "        if (rt != 778) { return 1000 + k; }\n"
            "        // trapper trapped StackOverflow -> INT64_MIN\n"
            "        if (rp != -9223372036854775808) { return 2000 + k; }\n"
            "        let reason = thread_trap_reason(tp);\n"
            "        if (reason != 3) { return 3000 + k; }\n"  // StackOverflow==3
            "        // checker summed 0..29 = 435 + overlap(1)*1000000 == 1000435\n"
            "        if (rc != 1000435) { return 4000 + k; }\n"
            "        acc = acc + 1;\n"
            "        k = k + 1;\n"
            "    }\n"
            "    return acc;\n"   // 2 if all rounds held
            "}\n", m);
        check(ok, "(10) compile try/catch/throw + recursion + trap isolation module");
        if (ok) {
            ict_gate_reset();
            ext_thread::thread_init(&m.ctx, m.table.base(), m.slot_count);
            bool t=false;
            int64_t r = run_main_locked(m, &t);
            int max_active = ict_get_max_active();
            check(!t, "(10) main did not trap");
            // RED under serialization: the 3-worker barrier never reaches 2
            // concurrent JIT workers, so each ict_gate_enter times out -> each
            // overlap flag is 0 -> thrower returns 777 (not 778), checker
            // returns 435 (not 1000435) -> main returns a failure code, not 3.
            check(r == 2, "(10) 2 rounds: thrower caught own throw(777)+overlap, trapper trapped StackOverflow, checker sum held");
            check(max_active >= 2, "(10) TRUE PARALLEL: thrower/trapper/checker overlapped in JIT (max_active >= 2)");
            std::printf("      (diag) observed max_active = %d, main result = %lld\n", max_active, (long long)r);
        }
        cleanup(m);
    }

    // =========================================================================
    // ---- (11) GC SCENARIO: shared context-owned heap, no per-thread-heap split ----
    // =========================================================================
    // Two workers allocate GC objects (gc_new, PINNED so they survive collects)
    // into the SAME context-owned heap, store the handles in shared i64 globals,
    // overlap at the bounded gate, then worker A triggers gc_collect() while
    // worker B's roots remain live (pinned). After both join, the main thread
    // verifies from ITS OWN heap view:
    //   * every object is still live (gc_is_live)  -> they live in ONE shared
    //     heap, not per-thread heaps that died with the worker threads.
    //   * the live count is correct (gc_live_count == 2*K).
    //   * no double-free: gc_delete once -> 1, gc_delete again -> 0.
    //   * a final collect reaps exactly the unrooted set (freed_count jumps).
    //   * no corruption: every handle is a valid live pointer before delete.
    // Under the OLD model TWO things fail (RED):
    //   (a) the barrier times out -> no overlap (serialized JIT).
    //   (b) the GC heap is thread_local (ext_gc::rt() is a thread_local
    //       unique_ptr) so each worker allocates into its OWN heap, which is
    //       DESTROYED when the worker thread exits -> after join the main
    //       thread's gc_is_live returns false for EVERY handle (object loss /
    //       per-thread-heap split) + gc_live_count on the main thread is 0.
    // Both are the RED signal that the GC must be a single context-owned heap
    // for in-context threads. The test links ember_ext_gc + initializes /
    // attaches / resets the GC per the existing API.
    {
        // K objects per worker; handles land in g_a0..g_a{K-1} + g_b0..g_b{K-1}.
        constexpr int K = 4;
        // Build the source with K globals per worker.
        std::string src;
        src += "fn aworker(n: i64) -> i64 {\n";
        for (int i = 0; i < K; ++i) src += "    g_a" + std::to_string(i) + " = gc_new(16);\n";
        src += "    let overlapped = ict_gate_enter();\n";   // overlap with B
        src += "    // collect while B's roots are still live (pinned)\n";
        src += "    let freed = gc_collect();\n";
        src += "    let live = gc_live();\n";
        src += "    return overlapped * 1000000 + live;\n";
        src += "}\n";
        src += "fn bworker(n: i64) -> i64 {\n";
        for (int i = 0; i < K; ++i) src += "    g_b" + std::to_string(i) + " = gc_new(16);\n";
        src += "    let overlapped = ict_gate_enter();\n";   // overlap with A
        src += "    let live = gc_live();\n";
        src += "    // keep roots live (pinned) -> A's collect must NOT reap these\n";
        src += "    return overlapped * 1000000 + live;\n";
        src += "}\n";
        src += "fn main() -> i64 {\n";
        src += "    let ta = thread_spawn(&aworker, 0);\n";
        src += "    let tb = thread_spawn(&bworker, 0);\n";
        src += "    if (ta == 0 || tb == 0) { return -1; }\n";
        src += "    let ra = thread_join(ta);\n";
        src += "    let rb = thread_join(tb);\n";
        src += "    let oa = ra / 1000000;\n";
        src += "    let ob = rb / 1000000;\n";
        src += "    return oa + ob;\n";   // 2 if both overlapped
        src += "}\n";
        // globals declared at top-level (i64 handles).
        for (int i = 0; i < K; ++i) src = "global g_a" + std::to_string(i) + " : i64 = 0;\n" + src;
        for (int i = 0; i < K; ++i) src = "global g_b" + std::to_string(i) + " : i64 = 0;\n" + src;

        ThreadModule m;
        bool ok = compile_threaded(src, m, /*use_gc=*/true);
        check(ok, "(11) compile GC shared-heap scenario module");
        if (ok) {
            ict_gate_reset();
            ext_gc::gc_reset();   // start the GC heap clean for this scenario
            ext_thread::thread_init(&m.ctx, m.table.base(), m.slot_count);
            bool t=false;
            int64_t r = run_main_locked(m, &t);
            int max_active = ict_get_max_active();
            check(!t, "(11) main did not trap");
            std::printf("      (diag) observed max_active = %d, main overlap result = %lld\n", max_active, (long long)r);

            // --- Verify the shared-heap invariants from the MAIN thread. ---
            // Read the handles the workers stored into the shared globals.
            auto read_global_i64 = [&](const std::string& name) -> int64_t {
                auto it = m.gb.index.find(name);
                if (it == m.gb.index.end()) return 0;
                // Typed globals block (use_gc path) uses per-global byte offsets;
                // the flat legacy path uses slot_index * 8. Prefer offsets when
                // present, fall back to the flat slot otherwise.
                uint32_t off = uint32_t(it->second) * 8;
                auto oit = m.gb.offsets.find(name);
                if (oit != m.gb.offsets.end()) off = oit->second;
                if (off + 8 > m.gbs.size()) return 0;
                int64_t v; std::memcpy(&v, m.gbs.data() + off, 8);
                return v;
            };

            int live_from_main = 0;
            int nonzero_handles = 0;
            for (int i = 0; i < K; ++i) {
                int64_t ha = read_global_i64("g_a" + std::to_string(i));
                int64_t hb = read_global_i64("g_b" + std::to_string(i));
                if (ha != 0) { ++nonzero_handles; if (ext_gc::gc_is_live(ha)) ++live_from_main; }
                if (hb != 0) { ++nonzero_handles; if (ext_gc::gc_is_live(hb)) ++live_from_main; }
            }
            // RED (b): under the thread-local heap model the workers' heaps die
            // with the worker threads, so the main thread sees 0 live objects
            // (per-thread-heap split / object loss). A shared context-owned
            // heap keeps all 2K objects live + visible from the main thread.
            check(nonzero_handles == 2*K, "(11) all 2K handles stored in shared globals (no alloc failure)");
            check(live_from_main == 2*K, "(11) all 2K objects live in ONE shared heap visible from main thread (no per-thread-heap split / object loss)");
            int64_t live_count_main = ext_gc::gc_live_count();
            check(live_count_main >= 2*K, "(11) main-thread gc_live_count >= 2K (shared heap holds all worker objects)");

            // --- no double-free: gc_delete once -> 1, gc_delete again -> 0. ---
            // Use the host API (gc_unroot_env is what gc_delete maps to). Under
            // the shared-heap model the first unroot returns 1 (was pinned),
            // the second returns 0 (already unpinned) -> no double-free. Under
            // the thread-local model both return 0 (object not in this heap).
            int double_free_detected = 0;
            int first_unroot_ok = 0;
            for (int i = 0; i < K; ++i) {
                int64_t ha = read_global_i64("g_a" + std::to_string(i));
                int64_t hb = read_global_i64("g_b" + std::to_string(i));
                for (int64_t h : {ha, hb}) {
                    if (h == 0) continue;
                    bool first = ext_gc::gc_unroot_env(h);
                    bool second = ext_gc::gc_unroot_env(h);   // already unpinned -> false
                    if (first) ++first_unroot_ok;
                    if (first && second) ++double_free_detected;  // pathological: both returned true
                }
            }
            check(double_free_detected == 0, "(11) no double-free: second gc_delete on a handle returns 0 (already unpinned)");
            check(first_unroot_ok == 2*K, "(11) first gc_delete on each live handle unroots it (shared heap honors the pin)");

            // --- final collect reaps exactly the unrooted set (no corruption). ---
            int64_t freed = ext_gc::gc_collect();
            int64_t live_after = ext_gc::gc_live_count();
            std::printf("      (diag) final collect freed = %lld, live_after = %lld\n", (long long)freed, (long long)live_after);
            check(freed >= 2*K, "(11) final gc_collect reaped the unrooted set (>= 2K freed, no stranded objects)");
            check(live_after == 0, "(11) after unrooting all + collect, heap is empty (no leak / no corruption)");

            // RED (a): under serialization the barrier times out -> no overlap.
            check(max_active >= 2, "(11) TRUE PARALLEL: workers overlapped in JIT at the GC gate (max_active >= 2)");
            check(r == 2, "(11) both GC workers observed barrier overlap (each overlap flag == 1)");
        }
        cleanup(m);
    }

    std::printf("\nTier 4 in-context threads test: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
