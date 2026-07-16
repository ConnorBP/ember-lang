# Ember extensions

Ember currently ships **17 extension libraries**: 15 native/addon extensions and
2 ThinIR pass extensions. Extensions are host-side registration units; they do
not add grammar. Native extensions register `NativeSig` entries (and sometimes
operator overloads), while pass extensions register `ThinFunction` transforms.

## Complete inventory

| Directory / target | Kind | Current surface |
|---|---|---|
| `vec/` / `ember_ext_vec` | native + overloads | nominal `vec2`, `vec3`, and `vec4` handles; component access/mutation; `+`, `-`, `*`, `==` |
| `quat/` / `ember_ext_quat` | native + overloads | nominal `quat`; component access/mutation; add/subtract/Hamilton multiply/equality |
| `mat/` / `ember_ext_mat` | native + overloads | nominal `mat4`; construction, identity, indexed get/set, matrix multiply/equality |
| `string/` / `ember_ext_string` | native + overloads | owned `string` handles; conversion, identity, length, indexing, find, substring, `fmt1`-`fmt4`, concatenate/equality |
| `array/` / `ember_ext_array` | native | host-backed arrays; create/length/resize; typed u8/f32/i64 get/set/push/pop; clear/remove; host byte accessors |
| `math/` / `ember_ext_math` | native | f32/f64 trig and elementary functions, floor/ceil/round/trunc, exp/log variants, fmod, min/max/clamp, and integer abs/min/max/clamp |
| `map/` / `ember_ext_map` | native | host-backed i64-key/i64-value map: new/set/get/contains/length/remove/clear |
| `sync/` / `ember_ext_sync` | native | nominal atomics, swap buffers, SPSC/MPSC/MPMC queues, plus host-side accessors |
| `thread/` / `ember_ext_thread` | native | in-context concurrent workers: spawn/join/trap reason; legacy and keyed runtime initialization |
| `coroutine/` / `ember_ext_coroutine` | native | `coroutine_start`/`next`/`done` and `yield`; implemented with Windows fibers. Non-Windows builds link a fail-closed stub, not a coroutine implementation |
| `lifecycle/` / `ember_ext_lifecycle` | native | dynamic `register_routine(fn, data)` / `unregister_routine(id)` |
| `io/` / `ember_ext_io` | native | console, text/byte file, and path operations; all calls are `PERM_FFI`-gated |
| `call_raw/` / `ember_ext_call_raw` | native | raw executable-page creation/call/free plus the **EMBM v1/v2** executable-module loader/query/free API and context-layout queries. Raw/module operations are `PERM_FFI`-gated |
| `gc/` / `ember_ext_gc` | native/runtime | tracing GC, precise frame/global/extension roots, lambda environments, write barriers, `gc_*`, and the runtime substrate for language `new`/`delete` |
| `audio/` / `ember_ext_audio` | native | typed `AudioContext`, parameter/event access, f32/f64 sample I/O, and raw typed buffer helpers; all calls are `PERM_FFI`-gated |
| `opt/` / `ember_ext_opt` | pass | **18 optimization passes** (listed below) |
| `obf/` / `ember_ext_obf` | pass | **7 obfuscation passes** (listed below) |

The native/addon count is 15: vec, quat, mat, string, array, math, map, sync,
thread, coroutine, lifecycle, io, call_raw, gc, and audio. `opt` and `obf` are
extension libraries too, but they use `register_passes`, not `register_natives`.

## Pass inventory (25 registered names)

`ember_ext_opt` registers 18 value-preserving optimization passes:

- `constprop`, `dce`, `simplifycfg`, `cse`, `gvn`, `licm`, `lsr`
- `forward`, `copyprop`, `instcombine`, `dse`, `bounds-elim`, `sccp`
- `unroll`, `spill_elim`, `peephole`, `branch_folding`, `tailcall`

`ember_ext_obf` registers 7 required obfuscation passes:

- `subst`, `mba_expand`, `const_encode`, `opaque_pred`, `deadcode`,
  `str_encrypt`, `block_split`

See [`PASS_AUTHORING.md`](../docs/PASS_AUTHORING.md) for the pass contract and
[`PASS_SYSTEM_DESIGN.md`](../docs/spec/PASS_SYSTEM_DESIGN.md) for architecture.

## Registering extensions in a host

Link only the extension targets the host intends to expose, then register them
explicitly. Linking is not discovery.

```cpp
std::unordered_map<std::string, ember::NativeSig> natives;
ember::OpOverloadTable overloads;

ember::ext_vec::register_natives(natives);
ember::ext_vec::register_overloads(overloads);
ember::ext_string::register_natives(natives);
ember::ext_string::register_overloads(overloads);
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
```

Quat and mat follow the same native/overload pattern as vec. Host-backed stores
must be reset between independent runs. Thread, coroutine, GC, call_raw's EMBM
loader, and lifecycle also have host initialization/context APIs; read their
public `ext_*.hpp` files before exposing them. In particular:

- call `ext_thread::thread_init(...)` or its keyed counterpart before spawning;
- call `ext_coroutine::coroutine_init(...)` on Windows (or the keyed API where
  supported) before starting coroutines;
- initialize/attach GC when using GC environments or language `new`/`delete`;
- call `ext_call_raw::set_loader_context(...)` before EMBM images with native
  relocations are loaded.

A host can omit any capability simply by not linking/registering it. `io`,
`audio`, and raw/module operations in `call_raw` additionally require the
`PERM_FFI` permission at sema time.

Pass registration is separate:

```cpp
ember::EmberPassRegistry passes;
ember::ext_opt::register_passes(passes);
ember::ext_obf::register_passes(passes); // deterministic compatibility defaults
```

Configured obfuscation factories and named profiles are covered in the pass
authoring guide.

## Build integration

The root `CMakeLists.txt` defines every `ember_ext_*` static library. The stock
`ember_cli` links and registers all 15 native/addon extensions and both pass
extensions. The standalone bundle/stub intentionally expose a smaller runtime
surface than the CLI; do not infer host capabilities from the mere existence of
an extension library.

## Historical Prism relocation audit

[`AUDIT.md`](AUDIT.md) records the original six-extension Prism classification.
It is a historical provenance audit, not the current extension inventory. All
later generic extensions are listed above.
