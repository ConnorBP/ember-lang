# Embedding Ember in a C++ application

This chapter builds an Ember host from the same public pieces used by
`examples/game_host.cpp` and `examples/ember_cli.cpp`. Ember deliberately does
not hide the compiler and runtime behind a single `engine_t`: the shipped API is
lexer → parser → sema → codegen, with host-owned native tables, dispatch,
globals, executable pages, contexts, and extension state. That ownership makes
capability selection and shutdown explicit.

> **Platform note.** The native JIT ABI is Win64 x64. The IR and extension code
> also builds elsewhere, but Win32-only extensions use fail-closed stubs.

## 1. Minimal source-string host

This complete program compiles one source string, registers the small standard
set used by the example, finalizes `main`, and calls it. It is intentionally
compact; production hosts should add the context boundary in the next section.

```cpp
#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"
#include "codegen.hpp"
#include "engine.hpp"
#include "dispatch_table.hpp"
#include "extensions/array/ext_array.hpp"
#include "extensions/math/ext_math.hpp"
#include <unordered_map>

int main() {
    using namespace ember;
    auto lex = tokenize("fn main() -> i64 { return 40 + 2; }", "<memory>");
    if (!lex.ok) return 1;
    auto parsed = parse(std::move(lex.toks));
    if (!parsed.ok) return 2;
    std::unordered_map<std::string, NativeSig> natives;
    ext_array::register_natives(natives); ext_math::register_natives(natives);
    std::unordered_map<std::string, int> slots{{"main", 0}};
    parsed.program.funcs[0].slot = 0;
    OpOverloadTable overloads; auto layouts = build_struct_layouts(parsed.program);
    if (!sema(parsed.program, natives, slots, 0, &overloads, &layouts).ok) return 3;
    DispatchTable dispatch(1); CodeGenCtx cg; cg.dispatch_base=(int64_t)dispatch.base();
    cg.natives=&natives; cg.script_slots=&slots; cg.structs=&layouts;
    CompiledFn fn=compile_func(parsed.program.funcs[0], cg);
    if (!finalize(fn)) return 4; dispatch.set(0, fn.entry);
    return int(call_i64_i64(fn.entry)); // process exit 42; CompiledFn owns the page
}
```

For CMake, link the core/frontend and exactly the extensions registered:

```cmake
add_subdirectory(path/to/ember ember-build)
add_executable(my_host main.cpp)
target_include_directories(my_host PRIVATE path/to/ember/src path/to/ember)
target_link_libraries(my_host PRIVATE ember ember_frontend
    ember_ext_array ember_ext_math)
```

The full CLI also computes a typed globals block, resolves imports and linked
modules, creates a function-target allowlist, and retains every object whose
address is baked into generated code. Copy those parts when the script uses the
corresponding feature. In particular, a `DispatchTable`, globals storage,
native table, struct layouts, linked-module registry, and every current
`CompiledFn` must outlive all calls that refer to them.

## 2. The recoverable call boundary: `context_t`

Compile safety checks with a context register and a trap stub, then enter JIT
code through `ember_call_*`. The raw helpers install `r14 = &ctx`; they do **not**
reset the context or establish a checkpoint.

```cpp
#include "context.hpp"
#include "engine.hpp"
#include <cstdio>
#include <cstdlib>

extern "C" void host_trap(ember::context_t* ctx,
                          ember::TrapReason reason, const char* detail) {
    if (ctx) {
        ctx->last_trap = reason;
        ctx->last_error = detail ? detail : "trap";
        if (ctx->has_checkpoint) EMBER_LONGJMP(ctx->checkpoint, 1);
    }
    std::abort();                         // trap without a host checkpoint
}

bool call_main(void* entry, int64_t& value) {
    ember::context_t ctx;
    ctx.budget_remaining = 1'000'000;    // INT64_MAX disables the budget
    ctx.max_call_depth = 256;             // combined script/native depth
    ctx.reset_for_call();
    const int jumped = EMBER_SETJMP(ctx.checkpoint);
    if (jumped == 0) {
        ctx.has_checkpoint = true;
        value = ember::ember_call_void(entry, &ctx);
        ctx.has_checkpoint = false;
        return true;
    }
    ctx.has_checkpoint = false;
    std::fprintf(stderr, "Ember trap: %s: %s\n",
        ember::trap_reason_str(ctx.last_trap), ctx.last_error.c_str());
    ctx.reset_for_call();                 // clears abandoned depth/catch/GC frames
    return false;
}
```

Configure the matching code generation:

```cpp
CodeGenCtx cg;
cg.use_context_reg = true;
cg.trap_stub = reinterpret_cast<void*>(&host_trap);
cg.emit_budget_checks = true;
cg.emit_depth_checks = true;
cg.max_call_depth = 256;
```

`TrapReason` currently includes `BoundsCheck`, `BudgetExceeded`,
`StackOverflow`, `IllegalInstruction`, `DivByZero`, `BadCallTarget`,
`UnhandledThrow`, and `KeyedDispatchPadding`. A trap is a non-local unwind:
C++ destructors in abandoned JIT/native frames do not run. Keep RAII objects
outside the `setjmp` region, clear `has_checkpoint` on both paths, perform
host-owned cleanup after recovery, call `reset_for_call()`, and explicitly
replenish `budget_remaining` before the next call if desired. There is one
checkpoint per context, so use one outer call per context at a time.

Keyed `ModuleInstance` hosts should prefer `ember_call_keyed_*`; those APIs
return `CallResult`, derive the transient route, hold the generation guard,
establish the checkpoint, and clean up TLS/register state themselves.

## 3. Registering C++ natives

The implemented binding API is `BindingBuilder`, not the deferred `engine_t`
API shown in old design sketches.

```cpp
#include "binding_builder.hpp"
#include <cstdint>

static int64_t g_score = 0;
extern "C" int64_t score_add(int64_t amount, float scale) {
    g_score += static_cast<int64_t>(amount * scale);
    return g_score;
}

ember::BindingBuilder builder;
builder.add("score_add", ember::type_i64(),
            {ember::type_i64(), ember::type_f32()},
            reinterpret_cast<void*>(&score_add), 0);
ember::NativeTable table = builder.build();
```

Pass `table.natives` to `sema` and `CodeGenCtx::natives`; pass
`table.overloads` to sema. Registration erases the function-pointer type, so
matching the descriptor and real C++ signature is the host's responsibility.

| Ember value | Native C++ representation |
|---|---|
| `i8`…`i64`, `u8`…`u64` | matching fixed-width integer |
| `bool` | `bool`, ABI widened at the call boundary |
| `f32`, `f64` | `float`, `double` in positional XMM slots |
| `slice<T>` | two parameters: `T*`, `int64_t len` |
| extension `string` | opaque `int64_t` handle; use `ext_string::copy/slot` |
| `bind_handle("T")` | opaque `int64_t` nominal handle |
| registered struct | C++ POD by value, using the Win64 aggregate ABI |

Mark raw host access, files, graphics, or similar capabilities with
`PERM_FFI`:

```cpp
builder.add("dangerous_host_call", ember::type_i64(), {ember::type_i64()},
            reinterpret_cast<void*>(&dangerous_host_call), ember::PERM_FFI);
```

Then grant the bit to sema only for trusted source:

```cpp
const uint32_t permissions = trusted ? ember::PERM_FFI : 0;
auto result = sema(program, table.natives, slots, permissions,
                   &table.overloads, &layouts);
```

There is no runtime branch: denied source fails sema before codegen. Precompiled
modules are checked independently by `EmLoadPolicy` (section 6).

## 4. Registering the standard extensions

Linking is not discovery. A host explicitly chooses and registers capabilities:

```cpp
std::unordered_map<std::string, ember::NativeSig> natives;
ember::OpOverloadTable overloads;

ember::ext_vec::register_natives(natives);
ember::ext_quat::register_natives(natives);
ember::ext_mat::register_natives(natives);
ember::ext_string::register_natives(natives);
ember::ext_array::register_natives(natives);
ember::ext_math::register_natives(natives);
ember::ext_map::register_natives(natives);
ember::ext_sync::register_natives(natives);
ember::ext_thread::register_natives(natives);
ember::ext_coroutine::register_natives(natives);
ember::ext_lifecycle::register_natives(natives);
ember::ext_io::register_natives(natives);
ember::ext_call_raw::register_natives(natives);
ember::ext_gc::register_natives(natives);
ember::ext_audio::register_natives(natives);
ember::ext_graphics::register_natives(natives);
ember::ext_ui::register_natives(natives);
ember::ext_ui_widgets::register_natives(natives);
ember::ext_render::register_natives(natives);
ember::ext_visualize::register_natives(natives);

ember::ext_vec::register_overloads(overloads);
ember::ext_quat::register_overloads(overloads);
ember::ext_mat::register_overloads(overloads);
ember::ext_string::register_overloads(overloads);
```

The 20 native/addon extensions are `vec`, `quat`, `mat`, `string`, `array`,
`math`, `map`, `sync`, `thread`, `coroutine`, `lifecycle`, `io`, `call_raw`,
`gc`, `audio`, `graphics`, `ui`, `ui_widgets`, `render`, and `visualize`.
`opt` and `obf` are two additional pass extensions. Consult each public header
for required initialization. The CLI registration function is the canonical
complete example.

Operator overloads are separate from named natives during source compilation.
For a `.em` loader allowlist, also publish each overload's resolved
`fn_name`/signature in the native map, as `register_standard_bindings` in
`ember_cli.cpp` does. Reset stateful stores between independent runs and after
workers stop (`ext_string::reset`, `ext_array::reset`, `thread_reset`,
`gc_reset`, and the corresponding extension resets).

## 5. Binding structs by value

```cpp
struct Vec3 { float x, y, z; };
extern "C" Vec3 scale_vec(Vec3 v, float scale) {
    return {v.x * scale, v.y * scale, v.z * scale};
}

ember::StructLayoutTable layouts = ember::build_struct_layouts(program);
ember::register_struct(layouts, "Vec3", {
    {"x", ember::type_f32()},
    {"y", ember::type_f32()},
    {"z", ember::type_f32()},
});
ember::BindingBuilder b;
b.add("scale_vec", ember::bind_struct("Vec3"),
      {ember::bind_struct("Vec3"), ember::type_f32()},
      reinterpret_cast<void*>(&scale_vec));
```

`register_struct` computes C++-style field alignment and trailing padding;
`bind_struct` creates the first-class named type used in native signatures.
Pass the same `layouts` to sema and codegen. Add `static_assert(sizeof(Vec3) ==
12)` (and `offsetof` checks for nontrivial layouts) in the host.

On Win64, aggregates up to 8 bytes occupy a GP slot. Arguments larger than 8
bytes are copied and passed indirectly; aggregate returns use the hidden-return
pointer path. Ember accepts registered by-value native aggregates through 128
bytes. In particular, a struct larger than 16 bytes is **not** split into
ordinary scalar arguments: compile the native with the normal Win64 ABI and let
Ember marshal the hidden/indirect pointer. Never describe a pointer-taking C++
function as a by-value binding.

## 6. Loading precompiled `.em` modules

```cpp
#include "em_loader.hpp"

ember::LoadedModule module;
std::string error;
ember::EmLoadPolicy policy;
policy.module_permissions = 0;     // add PERM_FFI only for a trusted module
policy.allow_raw_x86 = false;      // secure default: all-IR modern module
if (!ember::load_em_file("plugin.em", module, &error, nullptr,
                         &natives, nullptr, &policy)) {
    throw std::runtime_error(error);
}
void* entry = module.entry_by_name("main");
if (!entry) throw std::runtime_error("main is not exported");
```

For an embedded/network buffer, use the identical policy path:

```cpp
ember::LoadedModule module;
if (!ember::load_em_bytes(bytes.data(), bytes.size(), module, &error,
                          registry, &natives, verify_policy, &policy))
    return false;
```

`LoadedModule` owns executable pages, globals, dispatch storage, export names,
and signatures. Its move-only lifetime must cover every call. The loader stages
work transactionally, validates sizes/format/signatures, reserves stable
storage, applies dispatch/global/module-registry relocations, resolves symbolic
native relocations by name and exact signature, checks each native permission,
and seals pages before publication.

Legacy EMBM v1/v2 executable blobs are exposed by the `call_raw` extension;
initialize it with `ext_call_raw::set_loader_context(&natives, permissions)`.
Do not confuse that nested raw-module format with modern top-level `.em`
versions. Top-level v1–v4 and mixed/raw v5 contain raw x64 and require an
explicit trusted-development opt-in:

```cpp
ember::EmLoadPolicy legacy{0, true}; // accepts raw x64; never use for untrusted input
```

The null/default policy is safer: no FFI and no raw x64. Use `EmVerifyPolicy`
with trusted Ed25519 keys when artifact authenticity is required, and
`EmV6HostCaps` for v6 keyed/identity capability negotiation.

## 7. Hot reload

For identity dispatch, retain source `Program`, slots, native/layout/codegen
state, and one long-lived `HotReloadDomain`. Every outer call enrolls **before**
loading the current entry:

```cpp
auto call_tick = [&]() -> int64_t {
    auto guard = domain.guard();
    void* entry = table.get(tick_slot);
    return ember::ember_call_void(entry, &thread_context);
};
```

A file watcher can poll `last_write_time`, read one complete replacement
function, and publish it:

```cpp
auto result = ember::reload_function(replacement_source, program, table,
    domain, codegen, natives, &overloads, &layouts);
if (!result.ok) log(result.error);
else {
    // The domain owns the retired old page. Keep result.new_fn as current and
    // clear any duplicate old CompiledFn ownership before later quiescence.
    current_functions.push_back(std::move(result.new_fn));
    domain.reclaim();
}
```

The function must already exist and retain the exact parameter/return ABI.
Publication is atomic; callers already in the old page may finish, while later
calls resolve the replacement. Never cache an entry outside its guard. At
shutdown, stop watchers/calls, join workers, drain guards, call `quiesce()`,
then free pages still current.

State migration is a host protocol, not an automatic Ember feature. Keep stable
state in host-owned objects or a compatible globals block; otherwise expose
`save_state`/`load_state` functions and migrate around publication. For audio or
rendering, compile privately, publish at a safe boundary, retain both old/new
states during a host-controlled crossfade, evaluate both under valid guards,
and retire the old state after the ramp. Ember supplies safe code publication,
not the DSP crossfade policy.

## 8. GC integration

Register `ext_gc` before sema. Compile GC-aware functions with the GC environment
settings used by the CLI, build a `gc::GcGlobalRoots` descriptor for global
slots whose `Type` is GC-managed, and attach before entering JIT code:

```cpp
ember::context_t ctx;
ember::gc::GcGlobalRoots roots;
roots.base = reinterpret_cast<uint64_t>(globals.data()); // keep storage stable
roots.offs = gc_pointer_offsets;         // byte offsets of GC-pointer globals
if (!ember::ext_gc::gc_init()) return false;
if (!ember::ext_gc::gc_attach_context(&ctx, &roots)) return false;

// Calls may now allocate and collect. At a host safe point:
const int64_t freed = ember::ext_gc::gc_collect();

ember::ext_gc::gc_detach_context(&ctx);
ember::ext_gc::gc_reset();
```

Generated prologues link precise `GcFrameRecord`s into
`ctx.gc_frame_head`; the collector scans those shadow-stack records, typed
global roots, extension trace callbacks, and explicit pins. A trap-recovery
`reset_for_call()` clears abandoned shadow frames. `gc_alloc_env` and
`gc_alloc_object` return unpinned managed allocations; `gc_root_env` pins a
host-retained object, and `gc_unroot_env` releases it. Use
`gc_delete_object` only for deterministic destruction.

A worker that shares the heap needs its **own** context:

```cpp
ember::context_t worker;
worker.budget_remaining = ctx.budget_remaining;
worker.max_call_depth = ctx.max_call_depth;
if (ember::ext_gc::gc_thread_enter(ctx.gc_runtime, &worker)) {
    // establish worker checkpoint and enter JIT
    ember::ext_gc::gc_thread_exit(&worker); // every normal or trapped exit
}
```

Extensions that store managed pointers register a `GcTraceFn` with
`gc_register_trace_callback`, report child candidates through the visitor, and
invoke `gc_write_barrier(owner, child)` when installing edges.

## 9. Threading

Compiled code and a finalized dispatch table may be shared after publication;
mutable call state may not. Allocate one `context_t` per concurrently entering
OS thread. Each context owns its checkpoint, budget, call depth, catch stack,
and shadow-stack head. Globals and extension stores must also be safe for the
application's access pattern.

The thread extension exposes script `thread_spawn`/`thread_join`. Initialize
legacy identity dispatch before scripts spawn:

```cpp
ember::ext_thread::register_natives(natives);
if (!ember::ext_thread::thread_init(&host_ctx, table.base(), slot_count))
    return false;
// run scripts; workers receive private contexts seeded from host_ctx
ember::ext_thread::thread_reset(); // only after all workers are joined
```

For keyed modules call `thread_init_keyed(instance)`. Workers resolve logical
slots at execution time, retain module/generation lifetime, and join the shared
GC runtime when attached. `thread_join` waits on worker synchronization; it does
not serialize all calls through `context_t::call_mutex` (that field is retained
only for source compatibility).

For ordinary host-created threads, apply the same rules manually: private
context/checkpoint, a reload `ExecutionGuard` spanning resolution through
return, `gc_thread_enter/exit` when sharing GC, and no module destruction until
all threads join.

## 10. Standalone executable bundles

Build the three stock targets, emit a portable module, then append it to the
stub:

```bash
cmake --build build --target ember_cli ember_bundle ember_stub_main
build/ember_cli emit-em app.ember app.em
build/ember_bundle app.ember app.exe --stub build/ember_stub_main
```

`ember_bundle` compiles source to the `.em` module representation, copies
`ember_stub_main`, appends the module bytes, then writes a 12-byte footer with
the module length and magic. At startup the stub finds its own executable,
validates the footer and length before allocation, reads the embedded bytes,
registers its linked extension/native allowlist, calls `load_em_bytes`, resolves
the selected export/entry, installs a context/checkpoint, and runs it. The stub
contains no lexer, parser, or sema.

The `.em` format contains function code or validated ThinIR, globals,
relocations, native signatures, exports, and entry metadata. It does not embed
arbitrary source annotations or source for future recompilation. The stub's
capability set is exactly what it links/registers; if a bundle uses a custom
extension, add that extension to **both** bundler compile-time registration and
stub load-time registration, link both targets, and preserve permission parity.
Use `--permissions ffi` only for a bundle intentionally granted those
capabilities.

## Production checklist

1. Register only required natives/extensions and choose permission bits.
2. Keep all baked-address storage and executable owners stable and alive.
3. Compile with context checks; establish a checkpoint for every outer call.
4. Use one context per concurrent caller and one reload guard per outer call.
5. Attach GC before GC-capable execution and detach after workers stop.
6. Treat raw-x64 `.em` compatibility mode as trusted-input-only.
7. Stop entry, watcher, tick, coroutine, and worker activity before teardown.
8. Quiesce reload domains, detach/reset extensions, then free current pages and
   module backing in dependency order.
