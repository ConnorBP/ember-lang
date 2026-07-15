# ember extensions

An **extension** is a reusable, non-cheat-specific set of
`ember::NativeSig` registrations + `ember::OpOverloadTable` entries +
the host C++ that backs them, shipped here so *any* ember consumer
(prism today, a future second consumer tomorrow) can link and register
the same addon set without re-implementing it.

## What an extension is - and is not

An extension is **NOT a language grammar or type-system change.**
Adding a new statement form, a new type-construction rule, or a new
sema pass is a `../docs/ROADMAP.md` **Tier 1+ language feature**, not an
extension. Extensions live entirely on the *host* side of the
`NativeSig`/`OpOverloadTable` seam that already exists in
`ember/src/sema.hpp` - they register native function pointers and
operator overloads against ember's stable v1 binding API, nothing more.
The language (parser/sema/codegen) is unchanged by an extension; an
extension only makes more *natives* resolvable at sema's
call-resolution step and more *operators* resolvable at sema's
overload-rewrite step.

This is the distinction the `../docs/ROADMAP.md` Tier 0 entry draws:
"these are not language features - they're `NativeFn` addons using
the stable v1 binding API." Extensions are where those addons live.

## What lives here (the audit)

`ember/extensions/` was populated by the restructure's Section 6 audit of
prism's `BuildScriptHostNatives` / `RegisterScriptHostOverloads`
(in `prism/src/prism/prism_script_host.cpp`). Every `NativeSig`
registration and `OpOverloadTable` entry was classified as either
**cheat-specific** (stays in prism) or **general-purpose** (relocated
here). The audit table is in `AUDIT.md` next to this file.

The general-purpose extensions relocated out of prism, matching the
`../docs/ROADMAP.md` Tier 0 standard addon set:

| Extension | Library | What it backs | Origin (prism) |
|---|---|---|---|
| `vec/` | `ember_ext_vec` | `vec2`/`vec3`/`vec4` host-store types + `Add`/`Sub`/`Mul`/`Eq` operator overloads | `prism_script_host.cpp` vec2/vec3/vec4 blocks |
| `quat/` | `ember_ext_quat` | `quat` host-store type + `Add`/`Sub`/`Mul`(Hamilton)/`Eq` overloads | `prism_script_host.cpp` quat block |
| `mat/` | `ember_ext_mat` | `mat4` host-store type + `Mul`(4×4 product)/`Eq` overloads | `prism_script_host.cpp` mat4 block |
| `string/` | `ember_ext_string` | mutable `string` host-store type + `Add`(concat)/`Eq` overloads + `from_slice`/`from_i64`/`from_f32`/`from_f64`/`from_bool`/`identity`/`length`/`char_at`/`find`/`substr` | `prism_script_host.cpp` string block |
| `array/` | `ember_ext_array` | `array<T>` host-store type + `new`/`length`/`resize`/`set_u8`/`get_u8`/`set_f32`/`get_f32`/`set_i64`/`get_i64`/`push_u8`/`push_f32`/`push_i64`/`pop_u8`/`pop_f32`/`pop_i64`/`clear`/`remove` + the `GetArrayBytes` accessor | `prism_script_host.cpp` array block |
| `math/` | `ember_ext_math` | `sqrt`/`sin`/`cos`/`tan` (f32) + `sqrt_f64`/`sin_f64`/`cos_f64`/`tan_f64`/`floor_f64`/`ceil_f64`/`abs_f64`/`pow_f64` (f64) + `abs_i64` - pure functions, no host store | `prism_script_host.cpp` math block |
| `sync/` | `ember_ext_sync` | cross-thread sync primitives: `aint8/16/32/64` atomics (load/store/fetch_add/cas/swap), swap buffer (double-buffer + atomic flip), SPSC/MPSC/MPMC queues — all behind opaque i64 handles, internally synchronized host storage (`std::atomic` / lock-free ring / host-internal `std::mutex` for MPMC). No operator overloads. | new (v1.0); no prism origin — added for the host↔script coordination pattern |
| `lifecycle/` | `ember_ext_lifecycle` | dynamic routine registration: `register_routine(fn h, i64 data) -> id` / `unregister_routine(id)` — the Tier 2 fn-refs feature's host-native half. The `fn` param is typed (`is_fn_handle`) so sema rejects a forged plain i64; the host calls a stored routine via the dispatch table (the SAME call mechanism as the static `@on_tick` path, just discovered by the script at runtime). No operator overloads. | new (v1.0 follow-on); no prism origin — added once Tier 2 fn-refs shipped (`../docs/LIFECYCLE.md` §2) |
| `map/` | `ember_ext_map` | `map<K,V>` host-store type — opaque i64 handle backed by a host-side `unordered_map<i64,i64>` + `map_new`/`map_set`/`map_get`/`map_contains`/`map_length`/`map_remove`/`map_clear` (K and V are i64 in v1; typed keys/values are v2). No operator overloads. | new (v1.0); no prism origin — added for the Tier 0 standard addon set (`../docs/ROADMAP.md` Tier 0) |
| `gc/` | `ember_ext_gc` | tracing GC runtime for lambda closure environments (#20) + script-visible `gc_new`/`gc_delete`/`gc_collect`/`gc_live`. Wraps the thread-local `ember::gc::GcHeap` core (`src/gc.{hpp,cpp}`) + the precise root-scanning integration: `gc_attach_context`/`gc_detach_context` register the frame-chain + global-roots trace callback; the c1 trace-callback + write-barrier facade (`gc_register_trace_callback`/`gc_unregister_trace_callback`/`gc_write_barrier`/`gc_register_barrier_observer`) lets other extensions root their owned GC-pointer storage + record edges. The `array`/`map` extensions register trace callbacks so an unpinned GC object stored in an `array<i64>` slot / `map` entry is rooted through the extension (survives collect) + reclaimed when the entry is removed / the extension resets. The `vec`/`string` extensions register no-op LEAF callbacks (no false roots). See `gc/ext_gc.hpp` + `../docs/spec/MEMORY_AND_GC.md` §8. No operator overloads. | new (v1.0); no prism origin — the GC integration for escaping lambda envs |
| `io/` | `ember_ext_io` | OS I/O core subset — console (`print`/`println`/`print_i64`/`print_f64`/`read_line`), file (`file_read_bytes`/`file_write_bytes`/`file_exists`), path (`path_exists`/`path_basename`/`path_dirname`). Stateless (no host slot vector); text natives take/return `string` handles, byte natives use `array<u8>` handles. **ALL `PERM_FFI`-gated** (sema rejects every call site without the FFI bit — zero runtime cost); two layers of defense (registration: host chooses whether to register at all; permission: `PERM_FFI` gating). See `../docs/planning/plan_OS_IO_EXTENSIONS.md`. No operator overloads. | new (v1.0); no prism origin — the ROADMAP "Family B" re-entry trigger fired (a script blocked on output beyond the exit code) |
| `opt/` | `ember_ext_opt` | **PASS EXTENSION** (not a `NativeSig` addon) — registers IR→IR transforms over `ThinFunction` via `register_passes(EmberPassRegistry&)` (not `register_natives`). Ships 16 optimization passes: `ConstPropPass` ("constprop"), `DeadCodeElimPass` ("dce"), `CSEPass` ("cse"), `LICMPass` ("licm"), `StoreToLoadForwardPass` ("forward"), `CopyPropPass` ("copyprop"), `InstCombinePass` ("instcombine"), `DeadStoreElimPass` ("dse"), `SimplifyCFGPass` ("simplifycfg"), `BoundsCheckElimPass` ("bounds-elim"), `SCCPPass` ("sccp"), `LoopStrengthReductionPass` ("lsr"), `LoopUnrollPass` ("unroll"), `DeadSpillElimPass` ("spill_elim"), `PeepholePass` ("peephole"), `BranchFoldingPass` ("branch_folding"). See `../docs/spec/PASS_SYSTEM_DESIGN.md` §8. | new (v1.0); no prism origin — the IR optimization pass set |
| `obf/` | `ember_ext_obf` | **PASS EXTENSION** (not a `NativeSig` addon) — registers IR→IR transforms over `ThinFunction` via `register_passes(EmberPassRegistry&)` (not `register_natives`). Ships 7 obfuscation passes (all `is_required = true`, bypassing skip gates): `SubstitutionPass` ("subst", MBA), `MBAExpansionPass` ("mba_expand"), `ConstantEncodingPass` ("const_encode"), `OpaquePredicatesPass` ("opaque_pred"), `DeadCodeInjectionPass` ("deadcode"), `StringEncryptionPass` ("str_encrypt"), `BlockSplittingPass` ("block_split"). See `../docs/spec/PASS_SYSTEM_DESIGN.md` §8. | new (v1.0); no prism origin — the obfuscation pass set |

**Pass extensions (a separate category).** `extensions/opt/` (`ember_ext_opt`) and
`extensions/obf/` (`ember_ext_obf`) are NOT `NativeSig` addons — they register
IR→IR transforms over `ThinFunction` via `register_passes(EmberPassRegistry&)`
(not `register_natives`). `opt` ships 16 optimization passes (`ConstPropPass`/
`DeadCodeElimPass`/`CSEPass`/`LICMPass`/`StoreToLoadForwardPass`/
`CopyPropPass`/`InstCombinePass`/`DeadStoreElimPass`/`SimplifyCFGPass`/
`BoundsCheckElimPass`/`SCCPPass`/`LoopStrengthReductionPass`/`LoopUnrollPass`/
`DeadSpillElimPass`/`PeepholePass`/`BranchFoldingPass`); `obf` ships 7
obfuscation passes (`SubstitutionPass`/`MBAExpansionPass`/
`ConstantEncodingPass`/`OpaquePredicatesPass`/`DeadCodeInjectionPass`/
`StringEncryptionPass`/`BlockSplittingPass`, all `is_required = true`).
See `../docs/spec/PASS_SYSTEM_DESIGN.md` §8. They do not fit the "what an
extension is" definition above (they are not host-side natives); they are
listed here for completeness.

Each is a self-contained C++ TU that depends only on ember's *public*
headers (`ast.hpp`, `sema.hpp` - for `Type`/`make_prim`/`make_slice`/
`NativeSig`/`OpOverloadTable`/`BinExpr::Op`/`Prim`) and the C++ stdlib.
No extension includes or links any prism header or prism target.

## What stayed in prism (cheat-specific, NOT here)

Per the audit, these `NativeSig` registrations are tied to reading a
game process or rendering an overlay and stay in prism:

- `proc.*` - `ru64`/`ru32`/`r32`/`rf32`/`r8`/`wu64`/`wf32`/`read_bulk`/
  `ref_process_native` (process memory read/write).
- `render_*` / view / shader - `get_view_*`, `draw_rect_filled`,
  `draw_text`, `create_shader`/`create_vertex_buffer`/...,
  `load_mesh`/`load_texture`/`get_font*` (overlay rendering + the
  `build_shader_natives` surface in `shader_api.cpp`).
- `gui_*` / panel - `register_gui_panel`, `begin_panel`, `gui_text`,
  `gui_slider_*`, `gui_button`, `create_subtab`, `panel_add`,
  `ui_checkbox`/`ui_slider_*`/`ui_*`/`widget_*` (GUI panels + the
  `build_panel_natives` surface in `prism_panel_api.cpp`).
- host-coupled IO / process - `print_i64`/`print_f32`/`print_str`/
  `print_string`/`assert_eq_*` (route through prism's host print sink
  + assert-failure counter), `get_tickcount64` (host-process timer
  used by cheat timing scripts), `aim_atan2` (cheat-named), sound /
  font / bitmap / http / file natives, `inject_mouse_delta`.
- numeric helpers not in the ROADMAP Tier 0 set - `clamp`/`min_max`
  (slice-pointer ABI), `fp_to_ieee`/`ieee_to_fp` (used by GPU shader
  scripts), `double_it`/`add_val` (test toys), `str_compare`/
  `str_length` (raw-pointer string ops, not the mutable `string`
  type).

These are not general-purpose language addons; relocating them would
drag cheat process/render/UI coupling into `ember/`, violating the
language-purity goal. They are documented in `AUDIT.md`.

## How a host registers an extension

A host (prism, or a future second consumer) links the extension
library and calls its registration entry points from its own native
table builder. The pattern, as prism now does in
`prism/src/prism/prism_script_host.cpp`:

```cpp
#include "ext_vec.hpp"      // from ember/extensions/vec/
#include "ext_quat.hpp"
// ... etc.

std::unordered_map<std::string, ember::NativeSig> BuildScriptHostNatives() {
    std::unordered_map<std::string, ember::NativeSig> m;
    // ... host's own cheat-specific natives registered here ...
    ember::ext_vec::register_natives(m);      // adds vec2_*/vec3_*/vec4_*
    ember::ext_quat::register_natives(m);
    ember::ext_mat::register_natives(m);
    ember::ext_string::register_natives(m);
    ember::ext_array::register_natives(m);
    ember::ext_math::register_natives(m);
    ember::ext_sync::register_natives(m);     // atomics, swap buffer, SPSC/MPSC/MPMC queues (v1.0)
    ember::ext_lifecycle::register_natives(m); // register_routine/unregister_routine (v1.0 follow-on)
    ember::ext_map::register_natives(m);       // map_new/map_set/map_get/map_contains/map_length/map_remove/map_clear (v1.0)
    ember::ext_io::register_natives(m);         // print/println/print_i64/print_f64/read_line/file_*/path_* — ALL PERM_FFI-gated (v1.0)
    return m;
}

void RegisterScriptHostOverloads(ember::OpOverloadTable& t) {
    ember::ext_vec::register_overloads(t);     // vec + - * ==
    ember::ext_quat::register_overloads(t);
    ember::ext_mat::register_overloads(t);
    ember::ext_string::register_overloads(t);  // string + ==
    // ext_sync + ext_lifecycle + ext_map + ext_io have no operator overloads (method-call natives, like ext_array).

// Host-store types keep per-run state (opaque i64 handles index into a
// host-owned vector). A host that wants each run independent calls the
// extension's reset() in its own per-run reset, as prism's
// ResetScriptHostState now does:
void ResetScriptHostState() {
    ember::ext_vec::reset();
    ember::ext_quat::reset();
    ember::ext_mat::reset();
    ember::ext_string::reset();
    ember::ext_array::reset();
    ember::ext_sync::reset();
    ember::ext_lifecycle::reset();
    ember::ext_map::reset();
    ember::ext_io::reset();   // stateless core subset (no-op, provided for symmetry)
    // math is stateless - no reset.
    // ... host's own state reset ...
}
```

The extension's `register_natives(map)` and `register_overloads(table)`
only insert their own keys; they do not touch keys the host already
registered (an `m[name] = ...` overwrites on collision, matching the
existing merge convention `BuildScriptHostNatives` uses for the
shader/panel native maps). A host that wants to shadow an extension
native with its own can simply register its own after calling the
extension's `register_natives`.

## Accessors a host may need

Some host-side cheat natives reach into an extension's host store by
handle (rather than going through a registered native). Each such
extension exposes a small accessor in its public header:

- `ember::ext_array::get_bytes(int64_t handle, uint8_t** out, int64_t* len)`
  - prism's `shader_api.cpp` (custom draw calls that receive
  `array<u8>` handles) and prism's `n_read_bulk` (process-memory read
  into an array) use this to read an array's backing bytes.
- `ember::ext_string::slot(int64_t handle)` - prism's `n_print_string`
  (the host-sink-coupled `print_string` native, which stays in prism)
  uses this to read a `string` handle's content.
- `ember::ext_sync::*_host(...)` (v1.0) - the sync primitives expose a full
  set of host-side accessors (`atomic_load_host`/`atomic_store_host`/
  `atomic_cas_host`, `swapbuf_back_ptr`/`swapbuf_front_index_host`/
  `swapbuf_front_write_host`/`swapbuf_swap_host`, `spsc/mpsc/mpmc_push_host`/
  `*_try_pop_host`) for the **U2 contract**: host producer/consumer threads
  that touch the queue/swap-buffer/atomic HOST storage WITHOUT calling ember
  (the script side stays single-threaded per context — see `ext_sync.hpp`'s
  S0 scope statement). These share the same underlying impl as the
  ember-facing natives; the `_host` suffix marks them as host-reach-in
  entries (the naming discipline that keeps `ext_array::get_bytes` clearly a
  host-reach-in vs an ember-callable `array_get_*`).
- `ember::ext_lifecycle::host_routines()` / `host_count()` (v1.0 follow-on) -
  the dynamic-registration extension exposes these so a host's tick/render
  loop iterates the currently-registered `(slot, data)` pairs and calls each
  via the dispatch table per frame (the SAME call mechanism as the static
  `@on_tick` path, just discovered by the script at runtime). The slot came
  from a `&fn` sema-validated at the take-handle site, so the host trusts it
  the way it trusts any sema-resolved slot. See `../docs/LIFECYCLE.md` §2.

The vec/quat/mat stores are entirely internal to their extension; no
prism native reaches into them by handle outside the registered
accessor natives, so they expose no accessor.

## Build

`ember/CMakeLists.txt` defines one static library per extension
(`ember_ext_vec`, `ember_ext_quat`, `ember_ext_mat`, `ember_ext_string`,
`ember_ext_array`, `ember_ext_math`, `ember_ext_map`, `ember_ext_sync`,
`ember_ext_thread`, `ember_ext_coroutine`, `ember_ext_lifecycle`,
`ember_ext_io`, `ember_ext_call_raw`, `ember_ext_audio`, `ember_ext_gc`, plus the `ember_ext_opt`/`ember_ext_obf`
pass-extension libs). Each links `ember_frontend` PUBLIC (for `make_prim`/`make_slice`/`Type` symbols,
defined in `ember_frontend`'s `types.cpp`) and exposes its `ext_*.hpp` header via
a PUBLIC include directory. A consumer links whichever extensions it wants;
prism links the original six (sync + lifecycle + map + io are host-arranged per consumer
— see `examples/ext_sync_test.cpp` / `examples/ext_lifecycle_test.cpp` for
the wiring; the standalone `ember` CLI links and registers all fifteen addon
extensions
(vec/quat/mat/string/array/math/map/sync/thread/coroutine/lifecycle/io/call_raw/audio/gc),
plus the `opt`/`obf` pass extensions).

## Purity

`ember/extensions/` follows the same language-purity rule as
`ember/src/`: no references to specific cheat products, hosts, or
research objects by name in code, comments, or docs. The forbidden
vocabulary is the named-product list scoped to the whole `ember/` tree;
an extension is a generic, reusable addon, so a grep for that vocabulary
against `ember/extensions/` returns zero hits.
