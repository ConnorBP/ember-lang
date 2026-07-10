# ember - native binding API spec

Detail doc for DESIGN.md Section 4. The shipped `BindingBuilder`/`NativeSig`
API comes first; later fluent descriptor sketches are labeled deferred inline.
Calling-convention mapping and boundary error responsibilities are normative.

---

> **Shipped vs deferred.** The host-facing `TypeBuilder` / `StructBuilder` /
> `EnumBuilder` / `engine_t` / `NativeFn` / `NativeParam` / `TypeId`
> surface in Sections 1–7 below is a **deferred ergonomic target**, not
> the current implementation. v1.0 ships the **working binding API**:
> `src/binding_builder.hpp`'s `BindingBuilder` + the existing
> `NativeSig`/`OpOverload` map that `sema()` already consumes, used by
> the eight standard extensions in `extensions/` (vec/quat/mat/string/array/
> math/sync/lifecycle — `sync` + `lifecycle` were added in the v1.0 batch +
> its follow-on). The spec text below is
> the target design those extensions move toward; it is preserved
> unchanged. The call-ABI mapping in Section 4 **IS implemented and
> proven** in the v0.3 `binding_abi_test` suite (script→native
> struct-by-value arg/return, >4-arg spill, `f32` in xmm slot-parallel,
> slice as `ptr+len` two words — all pinned by tests, all matching the
> table within the supported limits). `PERM_FFI` is defined in
> `binding_builder.hpp` and **enforced by sema** at every native call site;
> denied calls never reach marshalling or codegen.

## v0.3 working binding API

What you actually include and call today (`src/binding_builder.hpp`):

```cpp
#include "binding_builder.hpp"

using namespace ember;

BindingBuilder b;
b.add("sqrt", bind_prim(Prim::F32), {bind_prim(Prim::F32)}, &my_sqrt);
b.add("vec3_new", bind_handle("vec3"),
      {bind_prim(Prim::F32), bind_prim(Prim::F32), bind_prim(Prim::F32)},
      &vec3_new);
b.add_overload("vec3", int(BinExpr::Op::Add),
               bind_handle("vec3"), &vec3_add);
NativeTable t = b.build();
SemaResult sr = sema(prog, t.natives, slots, 0, &t.overloads, &layouts);
```

- **`BindingBuilder::add(name, ret, {params...}, fn, permission=0)`** —
  one declarative call per native. This is the "bindings like
  AngelScript" floor: `RegisterGlobalFunction("sig", &fn)` shape over
  the direct `NativeSig` map. Replaces the per-extension I/H/add
  boilerplate the eight extensions each redefined (eight copies deduped).
- **`add_overload(type_name, int(BinExpr::Op::X), ret, fn)`** —
  registers an operator overload for a struct-tagged opaque-handle type.
- **`bind_prim(Prim)` / `bind_handle("struct")`** — convenience `Type`
  builders for a primitive and an opaque `i64` struct-name-tagged handle.
- **`PERM_FFI`** — permission bit constant. Pass it as the `permission`
  arg to `add()` for FFI-gated natives. Sema rejects a gated native call
  unless the module permission mask contains `PERM_FFI`; no call marshalling
  or runtime permission branch is emitted. See `SAFETY_AND_SANDBOX.md` §6.
- **`NativeTable b.build()`** — moves out the filled `natives` map +
  `overloads` table; call once after all `add()`s.

The fluent `TypeBuilder`/`StructBuilder`/`engine_t` surface below
(Sections 1–7) is deferred design. It does not exist in the v1.0 API.

---

## 1. Deferred `TypeId` design (not implemented)

```cpp
enum class TypeId : uint16_t {
    t_void = 0,
    t_bool,
    t_i8, t_i16, t_i32, t_i64,
    t_u8, t_u16, t_u32, t_u64,
    t_f32, t_f64,
    t_slice,      // {ptr,len} pair, element type carried out-of-band
                  // (see NativeParam.elem_type below) since TypeId alone
                  // can't express "slice of struct X" generically without
                  // a second field
    t_struct,     // script-declared or host-mapped struct, identified
                  // by name (NativeParam.type_name), not further TypeId
                  // subdivision
};
```

No `t_array`/`t_map`/`t_class`/`t_enum`/`t_lambda`/`t_pointer`/`t_auto`
/`t_null`/`t_custom_base` (present in the surveyed native-JIT language's
enum, RESEARCH_NOTES.md) - v1 deliberately has a much smaller type
surface: fixed arrays are a compile-time-sized variant of struct
layout (TYPE_SYSTEM.md Section 3) not a distinct *binding*-visible kind,
maps/classes/lambdas are non-goals (DESIGN.md Section 1), script-side
`enum` **shipped in v1.0** but as plain `i32`/`i64`-typed values (an enum
variant is an `IntLit` post-sema, no new `TypeId` kind — see
`COMPILER_PIPELINE.md` Section 2a), so no `t_enum` belongs in this table
either; pointers/null don't exist as a script-visible concept
(TYPE_SYSTEM.md Section 5). Extend this enum additively later if a real need
appears (YAGNI).

## 2. Deferred `NativeParam` / `NativeFn` descriptor design

```cpp
struct NativeParam {
    const char* name;       // for introspection/error messages only,
                             // not used for call-site matching (v1 has
                             // no named-argument call syntax)
    TypeId      type;
    const char* type_name;  // required iff type == t_struct; the
                             // registered struct name. required iff
                             // type == t_slice AND element type is a
                             // struct (see elem_type below); ignored
                             // otherwise.
    TypeId      elem_type;  // required iff type == t_slice; the
                             // slice's element TypeId (may itself be
                             // t_struct, in which case type_name names
                             // the element struct, not the slice).
};

struct NativeFn {
    const char*        name;
    void*              fn_ptr;
    TypeId             ret;
    const char*        ret_type_name;  // same rule as NativeParam.type_name,
                                        // applies to ret when ret == t_struct
    const NativeParam* params;
    uint32_t           param_count;
    uint32_t           permission;     // 0 or PERM_FFI (SAFETY_AND_SANDBOX.md Section 6)
};

inline constexpr uint32_t PERM_FFI = 0x01;

void register_native(engine_t* engine, const NativeFn& fn);
```

- **`fn_ptr` signature contract**: must be a real C++ function (or
  `extern "C"` function, either works - calling convention is what
  matters, not linkage) whose actual parameter list, in order, is:
  for each `NativeParam`, if `type == t_slice`, **two** C++ parameters
  (`ElemType*, int64_t`) consecutively; otherwise **one** C++
  parameter of the C++ type corresponding to the `TypeId` (Section 4 mapping
  table). The shipped path accepts native by-value aggregate parameters only
  through 8 bytes; larger aggregate arguments are rejected by sema. Large
  aggregate returns use the tested hidden-return path. The broader descriptor
  design must not be read as expanding those v1 ABI limits.
- **Arity/type mismatch between `NativeFn` descriptor and the actual
  `fn_ptr` signature is not detectable by ember** (C++ function
  pointers are type-erased to `void*` at the registration API) - this
  is a documented host responsibility, exactly like the surveyed
  native-JIT language's and AngelScript's registration APIs (RESEARCH_NOTES.md); a mismatch
  produces silently-wrong argument placement at the call site, not a
  crash-on-registration. Mitigation is host-side: wrap registration in
  a macro/helper that derives `NativeFn` from the real C++ function
  signature via templates so the descriptor can never drift from the
  pointer (recommended pattern, shown in Section 7, not enforced by the
  runtime API itself since the API must also support raw
  registration for non-C++ hosts / dynamically-generated bindings).
- **Duplicate registration** (same `name` registered twice on one
  `engine_t`): second registration overwrites the first; a warning is
  recorded (not a hard error - matches "engine is a mutable registry
  you build up incrementally" model, and overwriting-to-patch-a-
  binding is a legitimate workflow during iteration).
- **Native function returning a slice**: the returned `{ptr,len}`
  pair's lifetime is entirely the native function's contract to the
  caller (TYPE_SYSTEM.md Section 4) - ember does not track or validate it.

## 3. Deferred `TypeBuilder` design

```cpp
class TypeBuilder {
public:
    TypeBuilder(engine_t* engine, const char* name, uint32_t size, uint32_t align);
    ~TypeBuilder(); // calls finish() if not already called, so a builder
                     // that goes out of scope without an explicit finish()
                     // still registers (RAII convenience) - matches a typical
                     // native-JIT scripting language's constructor/destructor pattern (RESEARCH_NOTES.md)

    TypeBuilder& field(const char* name, uint32_t offset, TypeId type,
                        const char* type_name = nullptr);
    TypeBuilder& method(const char* name, void* fn, TypeId ret,
                         const NativeParam* params, uint32_t param_count);
    TypeBuilder& property(const char* name, void* getter, void* setter, TypeId type);

    // operator overloads - each takes a NativeFn-shaped signature
    // (lhs, rhs) -> result for binary ops, (self) -> result for unary.
    // registering these is what makes TYPE_SYSTEM.md Section 7's operator
    // resolution find a candidate at sema time.
    TypeBuilder& bin_add(void* fn); TypeBuilder& bin_sub(void* fn);
    TypeBuilder& bin_mul(void* fn); TypeBuilder& bin_div(void* fn);
    TypeBuilder& bin_mod(void* fn);
    TypeBuilder& bin_eq(void* fn);  TypeBuilder& bin_lt(void* fn);
    TypeBuilder& bin_gt(void* fn);  TypeBuilder& bin_le(void* fn);
    TypeBuilder& bin_ge(void* fn);
    TypeBuilder& bit_and(void* fn); TypeBuilder& bit_or(void* fn);
    TypeBuilder& bit_xor(void* fn); TypeBuilder& shl(void* fn);
    TypeBuilder& shr(void* fn);
    TypeBuilder& unary_neg(void* fn); TypeBuilder& unary_bit_not(void* fn);

    void finish();
};
```

Trimmed from the surveyed native-JIT language's surface (RESEARCH_NOTES.md)  - 
dropped: `subscript`/`iterable`/`kv_iterable`/`factory`/`init_push`/
`hash`/`convert`/`destructor`. Rationale per item:
- `subscript`/`iterable`/`kv_iterable`: these back script-visible
  `for-each`/`[]`-on-a-custom-type semantics beyond plain slices  - 
  no grammar support for custom-type indexing/iteration exists in v1
  (TYPE_SYSTEM.md has only slice/fixed-array indexing). Add the
  builder method *when* the grammar grows the feature, not before
  (YAGNI - a builder method with no corresponding language feature to
  drive it is dead API surface).
- `factory`/`init_push`: constructor-call syntax for host types isn't
  in the v1 grammar (script can only get a struct instance by value
  from a native call return or by constructing a script-declared
  struct via field-literal syntax) - same YAGNI reasoning.
- `hash`: no script-visible map/set type exists (DESIGN.md non-goal).
- `convert`: implicit/explicit struct-to-struct conversion isn't a
  language feature (TYPE_SYSTEM.md Section 6 - struct types never convert).
  An explicit `as`-cast hook for host types is a plausible future
  addition but not needed until a concrete binding wants it.
- `destructor`: v1 has no script-owned heap objects needing cleanup
  (MEMORY_AND_GC.md Section 1/Section 4) - a host-mapped struct's lifetime is
  entirely host-managed; script never destroys one.
Each of the above is additive if/when needed - dropping them now
keeps the builder's surface matched 1:1 to actually-reachable
language features, which is the whole point of writing the spec
before the code.

- **`field()`**: registers a named field at a fixed byte offset for
  read/write access from script (`obj.x`). Offsets must not overlap
  in a way inconsistent with `size`/`align` given to the constructor  - 
  checked at `finish()` time (iterate registered fields, verify each
  `offset + type_size(field.type) <= size`; overlap between two
  fields is *allowed* (host might legitimately union two views over
  the same bytes) - not ember's business to forbid, it's the host's
  struct layout.
- **`method()`**: a native function taking the instance as an
  implicit first argument - codegen-wise this is **not** special: it
  desugars at sema time to an ordinary native call where the first
  `NativeParam` is synthesized as `{type: t_slice-of-1 or direct
  pointer depending on struct size, elem_type/type_name: this
  struct}` - i.e. `obj.method(a)` compiles to exactly the same call
  sequence as a free function `method(obj_ref, a)` would
  (CODEGEN_SPEC.md Section 8), just found via member-call syntax at parse
  time. No vtable, no virtual dispatch - matches "no OOP inheritance"
  (not in the v1 grammar; struct methods are static dispatch only, one
  registered native fn per method name per type).
- **`property()`**: sugar for a getter/setter pair invoked by
  `obj.name`/`obj.name = value` syntax instead of `obj.name()`/
  `obj.set_name(value)` - same desugar-to-native-call mechanism as
  `method()`, purely a parser/sema convenience, zero codegen
  difference from calling the getter/setter as ordinary methods.
- **Missing `finish()` call and builder destroyed early via
  exception/early-return**: covered by the destructor-calls-finish
  RAII rule above - cannot happen in v1 usage since ember doesn't
  throw C++ exceptions internally (SAFETY_AND_SANDBOX.md Section 2), so the
  only way `finish()` is skipped is normal scope exit, which the
  destructor covers.

## 4. Calling-convention mapping table (script type -> Win64 slot)

| Script type | C++ type on native side | Win64 slot |
|---|---|---|
| `bool` | `bool` (1 byte, but ABI-widened) | GP reg/stack, zero-extended to 32/64 bits per standard ABI rule for sub-word args |
| `i8..i64` | `int8_t..int64_t` | GP reg/stack |
| `u8..u64` | `uint8_t..uint64_t` | GP reg/stack |
| `f32` | `float` | xmm reg/stack |
| `f64` | `double` | xmm reg/stack |
| `slice<T>` | `T*, int64_t` (two consecutive slots) | GP+GP (or GP+stack / stack+stack once slot index exceeds 4, per CODEGEN_SPEC.md Section 1's slot-parallel rule) |
| `struct` <=8 bytes, POD | the struct type, passed by value | packed into one GP reg (bitcast, matches MSVC's small-POD-by-value-in-register rule) |
| `struct` >8 bytes argument | deferred/unsupported | v1 sema rejects native by-value aggregate arguments over 8 bytes |
| `struct` return >8 bytes | the struct type, returned by value | hidden pointer as first arg (`rcx`), also returned in `rax` |

This table is authoritative for both directions (script calling
native, native calling into script via a dispatch-table slot looked
up by name for event callbacks) - one mapping, used everywhere,
exactly matching CODEGEN_SPEC.md Section 1/Section 8.

## 5. Deferred `StructBuilder` / `EnumBuilder` design

`StructBuilder` (host declares a struct's shape without a full
`TypeBuilder` - i.e. no methods/operators, just fields, for simple
data-only host structs):
```cpp
class StructBuilder {
public:
    StructBuilder(engine_t*, const char* name, uint32_t size, uint32_t align);
    StructBuilder& field(const char* name, uint32_t offset, TypeId type, const char* type_name = nullptr);
    void finish();
};
```
Strictly a convenience subset of `TypeBuilder` (no operator/method
support) - kept as a separate class rather than folded into
`TypeBuilder` with optional method calls because "this is a plain
data struct, no behavior" is a useful declared intent, matching
a typical native-JIT scripting language's data-vs-behavior split (RESEARCH_NOTES.md).

`EnumBuilder`: **not needed / still dropped.** Script-side `enum E { A, B, C }`
**shipped in v1.0** (`ROADMAP.md` Tier 1 ✓, `COMPILER_PIPELINE.md` Section 2a),
but it is a pure source-text feature — an enum variant is rewritten to an
`IntLit` at sema (no new `TypeId`, no host-side state, no binding surface),
so there is nothing for an `EnumBuilder` to build. Host-side named constants
still use `set_global`. A host wanting enum-like behavior with reflection
(a `name_from_value` lookup, iteration over variants) would be a separate
future addition; that is the only case a builder would earn, and it is
YAGNI until a concrete binding asks for it.

## 6. Slice argument convention edge cases

- **Empty slice arg** (`len == 0`, `ptr` possibly null): valid, must
  be handled by the native function itself (ember does not forbid
  passing a zero-length or null-ptr slice - indexing it from *script*
  would trap per SAFETY_AND_SANDBOX.md Section 5, but simply passing it to a
  native function that, say, just checks `len == 0` and returns early
  is fine and common).
- **Struct-by-value >8 bytes as a native param**: rejected in v1 regardless
  of permissions. `PERM_FFI` gating is orthogonal and is checked before
  codegen for otherwise supported calls.
- **Native function with zero params**: `params = nullptr,
  param_count = 0` - valid, ordinary case, no special encoding.

## 7. Deferred host-side template wrapper (not runtime API)

To avoid the "descriptor can drift from the real function pointer"
risk noted in Section 2, hosts are expected to use a small variadic template
helper (implemented in a header the host includes, not inside ember's
core - this keeps ember's actual registration API dependency-free of
any template-metaprogramming complexity, matching the "own the ABI,
keep it boring" stance) that derives `TypeId` per argument from the
C++ parameter types via `if constexpr`/type-trait dispatch and builds
the `NativeFn`/`NativeParam` array automatically from a real function
pointer's signature, e.g. usage looks like:
```cpp
register_native_fn<&host_set_health>(engine, "set_health");
```
This wrapper is a **v1.0-milestone-adjacent nicety**, not required for
the core binding mechanism to work (DESIGN.md Section 9's milestones don't
block on it) - the raw descriptor-struct API is what actually needs
to exist and be correct first; the template sugar is additive.
