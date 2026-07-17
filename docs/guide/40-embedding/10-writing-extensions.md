# Writing an Ember native extension

An extension is a C++ registration unit selected by the host. It can add native
functions, nominal handle types, operator overloads, host-side resource stores,
and GC trace callbacks. It does not add grammar, and linking it does not
register it. This tutorial follows `extensions/array`, `extensions/io`, and
`extensions/graphics` and finishes with the build-tested random extension in
`examples/ext_random.{hpp,cpp}`.

## 1. Directory and build structure

A conventional extension has this layout:

```text
extensions/myext/
├── ext_myext.hpp        # public registration, reset, host accessors
└── ext_myext.cpp        # store, native implementations, BindingBuilder calls
```

Public header:

```cpp
#pragma once
#include "sema.hpp"
#include <string>
#include <unordered_map>

namespace ember::ext_myext {
void register_natives(std::unordered_map<std::string, NativeSig>& natives);
void reset();
} // namespace ember::ext_myext
```

Add a library with the root helper:

```cmake
ember_add_extension(myext extensions/myext/ext_myext.cpp)
```

This creates `ember_ext_myext`, adds Ember include directories, links the
frontend, and applies the project's C++ settings. A host then links it:

```cmake
target_link_libraries(my_host PRIVATE ember_ext_myext)
```

For an out-of-tree extension, an equivalent target is:

```cmake
add_library(ember_ext_myext STATIC ext_myext.cpp)
target_include_directories(ember_ext_myext PUBLIC ${EMBER_ROOT}/src .)
target_link_libraries(ember_ext_myext PUBLIC ember_frontend)
target_compile_features(ember_ext_myext PUBLIC cxx_std_17)
```

## 2. Write ABI-correct native functions

Natives are ordinary Win64 C/C++ functions. `extern "C"` avoids C++ name
mangling when inspecting binaries, but calling-convention correctness—not
linkage spelling—is what Ember needs.

```cpp
extern "C" int64_t n_my_function(int64_t arg1, float arg2) {
    return arg1 + static_cast<int64_t>(arg2);
}
```

The descriptor must exactly match the actual function:

| Ember | C++ native ABI |
|---|---|
| `void` | `void` |
| signed/unsigned integers | matching fixed-width integer |
| `bool` | `bool` |
| `f32` / `f64` | `float` / `double`; return in `xmm0` |
| extension handle/string | `int64_t`; return in `rax` |
| `slice<T>` | two arguments, `T*` then `int64_t` |
| registered POD struct | POD by value using Win64 aggregate rules |

Scalar integers and handles return in `rax`; floats return in `xmm0`. A
`string` is not a `char*`: it is an opaque `i64` handle into `ext_string`.
Never cast a handle directly to a host pointer.

Binding registration type-erases a function to `void*`; Ember cannot diagnose
a descriptor/function mismatch. Add direct C++ unit tests for every boundary
shape, especially mixed integer/float arguments, more than four argument words,
slices, and aggregates.

## 3. Register with `BindingBuilder`

```cpp
#include "binding_builder.hpp"

void register_natives(std::unordered_map<std::string, ember::NativeSig>& m) {
    using namespace ember;
    BindingBuilder b;
    b.add("my_function", type_i64(), {type_i64(), type_f32()},
          reinterpret_cast<void*>(&n_my_function), 0);
    NativeTable t = b.build();
    for (auto& kv : t.natives)
        m[kv.first] = std::move(kv.second);
}
```

`add(name, return_type, parameter_types, pointer, permission, retains)` has two
optional policy arguments:

- `permission` defaults to zero; use `PERM_FFI` for sensitive host access.
- `retains` defaults to false. Set it only if a native stores a slice pointer
  after return; sema then rejects stack-backed slices that could escape.

`build()` moves the table out and should be called once. Duplicate native names
in the destination map overwrite earlier entries, so hosts should define and
test a collision policy when composing third-party extensions.

## 4. Host-owned resources and opaque handles

The simplest safe resource ABI is a 1-based index into a host store:

```cpp
struct Resource { /* host-only object */ };
static std::vector<Resource> g_resources;
static std::mutex g_mutex;

int64_t allocate(Resource value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_resources.size() >= 4096) return 0;
    g_resources.push_back(std::move(value));
    return static_cast<int64_t>(g_resources.size()); // 1-based; 0 invalid
}

Resource* lookup_locked(int64_t h) {
    if (h <= 0 || static_cast<size_t>(h) > g_resources.size()) return nullptr;
    return &g_resources[static_cast<size_t>(h - 1)];
}
```

Do not return a borrowed pointer after unlocking if vector growth can invalidate
it. Instead copy data under the lock or perform the entire operation while
locked. Bound object count, per-object bytes, dimensions, and arithmetic before
allocation. Catch `std::bad_alloc`/`std::length_error` at native boundaries and
return a documented failure value rather than allowing an exception through JIT
frames.

If resources can be individually destroyed and slots reused, encode a
generation in the handle:

```text
handle = (generation << 32) | (slot_index + 1)
```

Validate both fields on every operation and increment generation when freeing a
slot. This prevents stale handles from naming a new object. `graphics` uses
this family of approach for windows and shaders.

Expose `reset()` to free all host-owned resources between independent module
runs. Call it only after calls/workers using the store have stopped.

## 5. Strings

Use `ext_string`'s host API:

```cpp
#include "extensions/string/ext_string.hpp"

extern "C" int64_t n_uppercase(int64_t string_handle) {
    std::string input;
    if (!ember::ext_string::copy(string_handle, input)) return 0;
    for (char& c : input)
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    return ember::ext_string::alloc(std::move(input));
}
```

`slot(handle)` returns `const std::string*` and is convenient in a single-thread
host, but later allocation/reset can invalidate the borrowed pointer. Prefer
`copy(handle, out)` for concurrently callable extensions. `alloc(std::string)`
returns a new 1-based handle or zero on failure. Register the parameter and
return as `bind_handle("string")`, matching `ext_io`.

A literal is an immutable `slice<u8>`, not automatically an owned string. Ember
code uses `string_from_slice` when a native requires the owned string handle.

## 6. Arrays

Use the host accessors instead of reaching into the extension's store:

```cpp
#include "extensions/array/ext_array.hpp"

extern "C" int64_t n_checksum(int64_t array_handle) {
    uint8_t* data = nullptr;
    int64_t len = 0;
    if (!ember::ext_array::get_bytes(array_handle, &data, &len)) return -1;
    uint64_t sum = 0;
    for (int64_t i = 0; i < len; ++i) sum += data[i];
    return static_cast<int64_t>(sum);
}

int64_t return_packet(const uint8_t* data, int64_t len) {
    return ember::ext_array::alloc_bytes(data, len);
}
```

`get_bytes` is a borrowed view. Do not retain it across a call or concurrent
resize. For cross-thread consumers prefer the typed copying APIs
`copy_f32`, `copy_i32`, and `copy_i64`. `alloc_bytes` copies input into a new
`array<u8>`; `alloc_f32` creates a correctly typed `array<f32>`.

## 7. Permission gating

Mark every native that opens files, calls raw pointers, creates executable
memory, talks to devices, or otherwise crosses the sandbox boundary:

```cpp
b.add("device_open", type_i64(), {bind_handle("string")},
      reinterpret_cast<void*>(&n_device_open), PERM_FFI);
```

Source compilation checks `NativeSig::permission` at each call during sema. A
module compiled with permission mask zero cannot name that operation; no native
call or runtime permission branch is emitted. This is capability omission, not
an authorization check inside the native.

The `.em` loader repeats the check against `EmLoadPolicy::module_permissions`.
That closes the possibility of bypassing sema with a hand-built module. The
loader also requires the native's name and canonical signature to match the
host allowlist. Hosts must keep compile and load policies consistent.

## 8. Operator overloads

Nominal handle types can participate in binary operators:

```cpp
extern "C" int64_t n_vec3_add(int64_t left, int64_t right);

void register_overloads(ember::OpOverloadTable& overloads) {
    ember::BindingBuilder b;
    b.add_overload("vec3", int(ember::BinExpr::Op::Add),
                   ember::bind_handle("vec3"),
                   reinterpret_cast<void*>(&n_vec3_add));
    ember::NativeTable table = b.build();
    for (auto& item : table.overloads.entries)
        overloads.entries[item.first] = std::move(item.second);
}
```

`add_overload` creates the symbolic native name (`vec3_add`, `string_add`, and
so on) and assumes two `i64` handle arguments. Use overloads for a conventional,
unambiguous value operation such as vector addition, quaternion multiplication,
or string concatenation/equality. Prefer named functions for mutation,
allocation, fallible operations, non-handle by-value structs, or behavior whose
cost/side effects should be visible.

Pass the overload table to sema. If emitting/loading `.em`, also add each
resolved overload signature to the loader's ordinary native allowlist; see the
CLI's `register_standard_bindings`.

## 9. GC-aware extensions

A host store that can contain GC object pointers is an external root provider.
Register a trace callback only when the GC runtime exists:

```cpp
static ember::gc::GcTraceToken trace_token = 0;

static void trace(void*, ember::gc::GcTraceVisitor& visitor) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const Slot& slot : g_slots)
        for (int64_t child : slot.managed_children)
            visitor.report(reinterpret_cast<void*>(uintptr_t(child)));
}

void ensure_trace() {
    if (!trace_token && ember::ext_gc::gc_runtime_initialized())
        trace_token = ember::ext_gc::gc_register_trace_callback(nullptr, trace);
}
```

When replacing an edge from a managed owner to a managed child, call
`gc_write_barrier(owner, child)`. During `reset()`, unregister the token before
freeing callback user data. The visitor filters null, stale, and ordinary
integer candidates, but extensions should still trace only fields declared to
hold managed values. A leaf store such as `string` may register a callback that
reports nothing.

Allocation choices:

- `gc_alloc_env(size)`: unpinned lambda/environment storage;
- `gc_alloc_object(size)`: unpinned generic managed object;
- `gc_root_env(ptr)` / `gc_unroot_env(ptr)`: explicit host pin lifetime;
- `gc_delete_object(ptr)`: immediate deterministic destruction.

If the extension's handle is merely an `i64`, the compiler does not infer that
it is a managed pointer. Either keep the managed object reachable through a
precisely typed script root, trace it from your store, or explicitly pin it.

## 10. Wire every host and artifact path

A new native extension is complete only when every intended host has parity:

1. Include `ext_myext.hpp` and call `ext_myext::register_natives(natives)` in
   `examples/ember_cli.cpp`.
2. Link `ember_ext_myext` to `ember_cli` in the root `CMakeLists.txt`.
3. If standalone source may use it, register/link it in
   `examples/ember_bundle.cpp` so sema recognizes calls.
4. Register/link it in `examples/ember_stub_main.cpp` so `.em` native
   relocations resolve at runtime.
5. Add overload registration and loader-allowlist publication if applicable.
6. Initialize context-dependent state before calls and invoke `reset()` during
   cleanup after all worker/coroutine activity stops.
7. Add a registration test, direct native tests, source-to-native execution
   test, `.em` round-trip test, permission-denial test, limit tests, and
   concurrent/reset tests as appropriate.

Keeping a native in the bundler but not the stub creates an artifact that
compiles and fails to load. Keeping it only in the stub does not let source
compile. Treat the pair as one deployment contract.

## 11. Complete extension: deterministic random numbers

The complete files are:

- [`examples/ext_random.hpp`](../../../examples/ext_random.hpp)
- [`examples/ext_random.cpp`](../../../examples/ext_random.cpp)
- [`examples/ext_random_test.cpp`](../../../examples/ext_random_test.cpp)

Public API (`ext_random.hpp`):

```cpp
#pragma once
#include "sema.hpp"
#include <string>
#include <unordered_map>

namespace ember::ext_random {
void register_natives(std::unordered_map<std::string, NativeSig>& natives);
void reset();
}
```

Implementation core (`ext_random.cpp`):

```cpp
#include "ext_random.hpp"
#include "binding_builder.hpp"
#include <cstdint>
#include <mutex>

namespace ember::ext_random {
namespace {
std::mutex g_mutex;
uint64_t g_state = 0x4d595df4d0f33173ULL;

uint64_t next_u64() {
    uint64_t x = g_state;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    g_state = x;
    return x * 2685821657736338717ULL;
}
extern "C" int64_t n_random_next(int64_t max) {
    if (max <= 0) return 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    return int64_t(next_u64() % uint64_t(max));
}
extern "C" void n_random_seed(int64_t seed) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_state = seed ? uint64_t(seed) : 0x4d595df4d0f33173ULL;
}
extern "C" float n_random_float() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return float(next_u64() >> 40) * (1.0f / 16777216.0f);
}
}

void register_natives(std::unordered_map<std::string, NativeSig>& m) {
    BindingBuilder b;
    b.add("random_next", type_i64(), {type_i64()}, (void*)&n_random_next);
    b.add("random_seed", type_void(), {type_i64()}, (void*)&n_random_seed);
    b.add("random_float", type_f32(), {}, (void*)&n_random_float);
    NativeTable t = b.build();
    for (auto& kv : t.natives) m[kv.first] = std::move(kv.second);
}
void reset() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_state = 0x4d595df4d0f33173ULL;
}
}
```

CMake integration and test registration:

```cmake
ember_add_extension(random examples/ext_random.cpp)
add_executable(ext_random_test examples/ext_random_test.cpp)
target_link_libraries(ext_random_test PRIVATE ember_ext_random ember_frontend)
add_test(NAME ext_random COMMAND ext_random_test)
```

Example Ember program:

```rs
fn main() -> i64 {
    random_seed(12345);
    let die: i64 = random_next(6) + 1;
    let unit: f32 = random_float();
    if (unit >= 0.0 && unit < 1.0) { return die; }
    return 0;
}
```

The extension is process-global but mutex-protected, deterministic after
`random_seed`, rejects nonpositive bounds, returns `random_float` in `[0, 1)`,
and restores a known state in `reset`. `examples/ext_random_test.cpp` directly
smoke-tests the same API; the project's existing `ext_registration` CTest also
compiles and exercises the extension, satisfying the in-tree CMake test gate
without changing the root build script. For independent modules or deterministic
parallel streams, replace the global state with generation-checked RNG handles:
`random_new(seed) -> rng`, `random_next(rng, max)`, and `random_destroy(rng)`.

Build the checked-in smoke test directly through the project target once wired,
or compile it out of tree:

```bash
g++ -std=c++17 -Isrc -Iexamples \
  examples/ext_random.cpp examples/ext_random_test.cpp \
  -Lbuild -lember_frontend -o build/ext_random_test
./build/ext_random_test
```
