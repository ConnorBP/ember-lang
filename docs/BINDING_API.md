# ember - native binding API spec

Detail doc for DESIGN.md Section 4. Full descriptor structs, TypeBuilder,

> **Implementation status: v0.1** - this is the v1.0 design spec. The
> current repo implements the JIT codegen proof (encoder, label/patch,
> exec-mem, `.em` format). See `README.md` for what's shipped; see
> `CODEGEN_SPEC.md` Section 12 + Section 15 for the acceptance suite. This doc's
> content is the target design, not a claim of current implementation.
calling-convention mapping, error reporting across the boundary.

## 1. `TypeId` (all values, v1)

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
maps/classes/lambdas are non-goals (DESIGN.md Section 1), enums are deferred
(no script `enum` keyword in v1 grammar - a host wanting enum-like
behavior exposes named `i32` constants via `set_global`, DESIGN.md
Section 8), pointers/null don't exist as a script-visible concept
(TYPE_SYSTEM.md Section 5). Extend this enum additively later if a real need
appears (YAGNI).

## 2. `NativeParam` / `NativeFn`

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
  table); struct-by-value params follow whatever the *host's own C++
  compiler* does for that struct under Win64 (>8 bytes: host's
  compiler already passes by hidden pointer identically to
  CODEGEN_SPEC.md Section 8's rule, since both sides target the same ABI  - 
  this is exactly why zero marshalling is possible). Return follows
  the same correspondence.
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

## 3. `TypeBuilder`

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
| `struct` >8 bytes | the struct type, passed by value | hidden pointer to a copy, in the arg's GP slot (CODEGEN_SPEC.md Section 8) |
| `struct` return >8 bytes | the struct type, returned by value | hidden pointer as first arg (`rcx`), also returned in `rax` |

This table is authoritative for both directions (script calling
native, native calling into script via a dispatch-table slot looked
up by name for event callbacks) - one mapping, used everywhere,
exactly matching CODEGEN_SPEC.md Section 1/Section 8.

## 5. `StructBuilder` / `EnumBuilder`

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

`EnumBuilder`: **dropped from v1** - no `enum` keyword in the v1
grammar (COMPILER_PIPELINE.md Section 2's token/grammar list has no `enum`).
Host-side named constants use `set_global`. Revisit if script-side
enum syntax is added later (would need both a grammar addition and
this builder - YAGNI to add the builder alone first).

## 6. Slice argument convention edge cases

- **Empty slice arg** (`len == 0`, `ptr` possibly null): valid, must
  be handled by the native function itself (ember does not forbid
  passing a zero-length or null-ptr slice - indexing it from *script*
  would trap per SAFETY_AND_SANDBOX.md Section 5, but simply passing it to a
  native function that, say, just checks `len == 0` and returns early
  is fine and common).
- **Struct-by-value >8 bytes as a native param, with `PERM_FFI`
  unset**: permission gating (SAFETY_AND_SANDBOX.md Section 6) applies
  identically regardless of argument shape - the struct-passing
  mechanism doesn't interact with the permission check at all, it's
  checked purely on the function name/module-permission pair before
  any argument-marshalling codegen happens.
- **Native function with zero params**: `params = nullptr,
  param_count = 0` - valid, ordinary case, no special encoding.

## 7. Recommended host-side template wrapper (documented pattern, not
part of the runtime API surface)

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
