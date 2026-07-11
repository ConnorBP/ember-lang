# Host Struct ABI Research — Rust FFI, C++ Layout, Win64 Struct Passing

**Research question:** How are `#[repr(C)]` / C / C++ structs laid out in
memory and passed *by value* across the Rust↔C/C++ FFI boundary on x86-64
Windows? Where do Win64 and System V AMD64 diverge, and which divergences
silently corrupt a host↔JIT struct ABI?

**Method:** authoritative ABI text (Microsoft x64 ABI, Rust Reference, Itanium
C++ ABI, System V AMD64 ABI) **plus** empirical disassembly. Every rule below
that is tagged `[emp]` was verified by compiling a probe with the toolchain in
this workspace and disassembling it. Toolchain used for evidence:

| Toolchain | Target | Backend | Role |
|---|---|---|---|
| `gcc (Rev8, MSYS2) 15.2.0` | `x86_64-w64-mingw32` (Win64) | GCC | C / C++ probes, Win64 ABI |
| `rustc 1.95.0` | `x86_64-pc-windows-msvc` (Win64) | LLVM 22.1.2 | Rust `#[repr(C)]` probes |
| `objdump` (GNU) | — | — | disassembly of COFF `.o` from both |

The probes and raw disassembly are saved under `docs/abi_evidence/`
(`probe_c.asm`, `probe_caller.asm`, `probe_slots.asm`, `probe_ret16.asm`,
`probe_pod16.asm`, `probe_abi_attr.asm`, `rustabi.asm`, `rustabi2.asm`,
`probe_cpp.cpp`, `sizes.exe`).

**Frame for this workspace.** The `prism` host stores `vec3`/`vec4`/`quat`/`mat4`
as host-side types and hands them to ember scripts through natives; ember's
struct ABI is MSVC-compatible on Windows. A mismatch between how the host
lays out / passes a `Mat4` and how the JIT-compiled ember ABI expects it is
exactly the class of bug this document exists to prevent. The dual-use / ABI
research framing is the same one the rest of `docs/` uses.

---

## TL;DR — the rules that actually bite

1. **`#[repr(C)]` is mandatory for any Rust struct that crosses FFI by value
   or by pointer-into-C.** `#[repr(Rust)]` (the default) reorders fields and
   has no stable layout. Verified: `{u8, u64, u8}` is **16 bytes** under
   `repr(Rust)` (fields reordered, `u64` moved to offset 0) and **24 bytes**
   under `repr(C)` (declaration order, 15 bytes of padding). `[emp]`

2. **`repr(C)` does NOT recurse.** A `#[repr(C)]` struct containing a field
   of a `#[repr(Rust)]` type does **not** impose C layout on that inner type.
   Every nested struct that participates in the ABI must itself be `repr(C)`.
   (Rust Reference, type-layout.html#representations.)

3. **Win64 passes/returns structs ≤ 8 bytes in a single integer register.**
   Structs **> 8 bytes** are passed/returned by **hidden pointer** (caller
   allocates, passes pointer, callee fills / returns pointer in RAX). This is
   true **regardless of float content** — an 8-byte `{float, float}` is passed
   in **RCX**, not XMM0. `[emp]` (see §3)

4. **Win64 argument registers are positional, not independently counted.**
   Argument position *i* (1-based, with a hidden return pointer counted as
   position 1) maps to a fixed pair `(GP_i, XMM_{i-1})`:
   `pos1=(RCX,XMM0), pos2=(RDX,XMM1), pos3=(R8,XMM2), pos4=(R9,XMM3)`. An
   integer/pointer/≤8B-struct uses the GP half; a scalar float/double uses the
   XMM half. `f(int,float,int,float)` → `a=RCX, b=XMM1, c=R8, d=XMM3`. `[emp]`
   (see §3.4). This is the single most commonly mis-stated Win64 rule.

5. **`Mat4` of 16 `float`s is `align 4` by default, NOT 16.** You must
   `#[repr(C, align(16))]` / `alignas(16)` to get 16-byte alignment for
   SIMD. Verified in C, C++, and Rust. `[emp]` (see §4)

6. **Win64 ≠ System V for mixed structs.** A 16-byte `{int, double}` is
   passed **by hidden pointer** on Win64 but **split across EDI + XMM0** on
   System V (eightbyte classification). Same source, two ABIs, different
   registers — verified side-by-side with `__attribute__((sysv_abi))`. `[emp]`
   (see §5)

7. **`offsetof` on non-standard-layout C++ is conditionally-supported** and
   emits a warning. Do not FFI a C++ type with virtual functions / base
   classes / non-trivial ctor by value. Pass it opaquely. `[emp]` (see §2.3)

8. **There is a real MSVC-vs-LLVM/GCC divergence on 16-byte POD returns.**
   MSVC's documented ABI permits a qualifying ≤16-byte POD to be returned in
   `RAX`(+`RDX`). Both LLVM (rustc/MSVC target) and GCC/mingw, as observed
   here, return **all** >8-byte structs (including 16-byte all-int POD) by
   hidden pointer. Treat "return a >8-byte struct by value" as a
   cross-toolchain hazard; prefer an explicit out-pointer. `[emp]` (see §3.3)

---

## 1. Rust FFI ABI — `#[repr(C)]` and struct-by-value

### 1.1 `repr(Rust)` (default) has no stable layout

The Rust Reference (type-layout.html) defines the default representation:

> "The Rust representation is the default representation for nominal types
> without a repr attribute. … For structs, it is further guaranteed that the
> fields do not overlap. That is, the fields can be ordered such that the
> offset plus the size of any field is less than or equal to the offset of the
> next field in the ordering. **The ordering does not have to be the same as
> the order in which the fields are specified in the declaration.**"

Concretely, the compiler **reorders fields** to minimize padding. Empirical
proof (`docs/abi_evidence/`, `reorder` probe):

```
struct Reorder { a: u8, b: u64, c: u8 }   // repr(Rust)
  size = 16, align = 8
  memory: [0]=0x2(b) ... [8]=0x1(a) [9]=0x3(c)   // b moved to offset 0

#[repr(C)] struct COrder { a: u8, b: u64, c: u8 }
  size = 24, align = 8                          // declaration order, 15B padding
  a@0, pad@1-7, b@8-15, c@16, pad@17-23
```

The C struct `struct COrder { char a; uint64_t b; char c; }` is 24 bytes —
it matches `repr(C)`, **not** `repr(Rust)`. Passing a `repr(Rust)` struct
into C by pointer is therefore a layout/size mismatch: the C side reads 24
bytes where Rust wrote 16, at different field offsets. **This is undefined
behavior and a classic FFI soundness bug.**

> Rust Reference: "even types with the same layout can still differ in how
> they are passed across function boundaries. For function call ABI
> compatibility of types, see [ABI]."

So layout equality is necessary but not sufficient — the *calling convention*
must also match (see §1.4).

### 1.2 `#[repr(C)]` struct layout algorithm

Rust Reference (type-layout.html#repr-c-structs), verbatim algorithm:

> - "The alignment of the struct is the alignment of the most-aligned field
>   in it."
> - "Start with a current offset of 0 bytes. For each field **in declaration
>   order**, first determine the size and alignment of the field. If the
>   current offset is not a multiple of the field's alignment, then add
>   padding bytes … The offset for the field is what the current offset is
>   now. Then increase the current offset by the size of the field."
> - "Finally, the size of the struct is the current offset rounded up to the
>   nearest multiple of the struct's alignment." (trailing padding)

This is **exactly** the C struct layout rule (see §2.1). A `#[repr(C)]`
struct has byte-for-byte the same layout as the equivalent C struct on the
same target. Verified for `Vec3`, `Vec4`, `Mat4`, `{i32, f64}`, `{f64, f64}`
in Rust vs C — identical `size`/`align`/offsets (§4).

### 1.3 `repr` modifiers and non-recursion

Modifiers (Rust Reference, type-layout.html#representations):

| Attribute | Effect |
|---|---|
| `#[repr(C)]` | C layout: declaration order, C padding rules. |
| `#[repr(C, packed)]` / `#[repr(C, packed(N))]` | Remove inter-field padding (UB to take references to fields; read via `addr_of!`). |
| `#[repr(C, align(N))]` | Raise alignment to N (and thus trailing padding). |
| `#[repr(align(N))]` on a field | Raises that field's alignment, which raises the containing struct's alignment. |

**Non-recursion (critical):**

> "The representation of a type can change the padding between fields, but
> does not change the layout of the fields themselves. For example, a struct
> with a C representation that contains a struct `Inner` with the Rust
> representation **will not change the layout of `Inner`**."

Rule of thumb: **every** struct in an FFI graph must be `#[repr(C)]`, or you
must hold the non-`C` inner type opaquely (pointer / byte array). A
`#[repr(C)]` wrapper around a `#[repr(Rust)]` inner does not give you a C
layout for the inner bytes.

### 1.4 Calling convention: `extern "C"` / `"system"` / `"win64"`

Rust function ABI is chosen by the `extern "abi"` string (Rust Reference,
items/external-blocks.html#abi):

| ABI string | Meaning on x86-64 Windows |
|---|---|
| `extern "Rust"` (default) | Rust's native, **unstable** ABI. No FFI guarantees. |
| `extern "C"` | The default ABI of the dominant C compiler for the target → **Microsoft x64 ABI** on Win64. |
| `extern "system"` | `= "C"` on x86_64 Windows (=`stdcall` on x86_32). Recommended for WinAPI linkage. |
| `extern "win64"` | Explicit Windows x64 ABI. |
| `extern "sysv64"` | System V AMD64 ABI (Linux/macOS x86-64). |
| `extern "C-unwind"` / `"system-unwind"` | Same as above + unwind across the boundary. |

> Rust Reference: "The Rust compiler automatically translates between the
> Rust ABI and the foreign ABI."

For FFI: declare the Rust entrypoint `pub extern "C" fn ...` (or `"system"`
for WinAPI) and the struct `#[repr(C)]`. That pair is the only guaranteed-
stable combination. **`#[repr(C)]` on the struct without `extern "C"` on the
function is not enough** — the *argument/return* marshalling still follows the
Rust ABI, which may differ from C for the same layout (esp. for return-by-
value of larger structs).

### 1.5 Empirical: Rust `#[repr(C)]` struct-by-value on Win64 (MSVC target)

`rustc 1.95.0` → `x86_64-pc-windows-msvc`, LLVM 22.1.2. Probe:
`docs/abi_evidence/rustabi.asm`. Every function is `pub unsafe extern "C" fn`,
struct is `#[repr(C)]`.

**Passing by value (callee view — which register holds the struct):**

| Function | Struct | Size | Observed | Verdict |
|---|---|---|---|---|
| `take_II({i32,i32})` | 8B int | 8 | `mov rax,rcx; shr rax,0x20; add eax,ecx` | **RCX** (packed int) |
| `take_FF({f32,f32})` | 8B pure float | 8 | `movd xmm1,ecx; shufps …` | **RCX** (NOT XMM0) |
| `take_IF({i32,f32})` | 8B mixed | 8 | `cvtsi2ss xmm0,ecx; movq xmm2,rcx; shufps …` | **RCX** |
| `take_ID({i32,f64})` | 16B mixed | 16 | `cvtsi2sd xmm0,[rcx]; addsd xmm0,[rcx+0x8]` | **hidden ptr in RCX** |
| `take_DD({f64,f64})` | 16B pure double | 16 | `movsd xmm0,[rcx]; addsd xmm0,[rcx+0x8]` | **hidden ptr in RCX** |
| `take_Vec3({f32,f32,f32})` | 12B | 12 | `movss xmm0,[rcx]; addss [rcx+4]; [rcx+8]` | **hidden ptr in RCX** |
| `take_Vec4({f32,f32,f32,f32})` | 16B | 16 | `movss xmm0,[rcx] … [rcx+0xc]` | **hidden ptr in RCX** |
| `take_Mat4({f32×16})` | 64B | 64 | `movss xmm0,[rcx]; addss [rcx+0x3c]` | **hidden ptr in RCX** |

**Returning by value (callee view):**

| Function | Struct | Size | Observed | Verdict |
|---|---|---|---|---|
| `ret_II()` | 8B int | 8 | `movabs rax,0x200000001; ret` | **RAX** (packed) |
| `ret_FF()` | 8B pure float | 8 | `movabs rax,0x400000003f800000; ret` | **RAX** (NOT XMM0) |
| `ret_Vec3()` | 12B | 12 | `mov rax,rcx; movsd [rcx],…; mov [rcx+8],…; ret` | **hidden ptr RCX → RAX** |
| `ret_Vec4()` | 16B | 16 | `mov rax,rcx; movups [rcx],xmm0; ret` | **hidden ptr RCX → RAX** |
| `ret_DD()` | 16B pure double | 16 | `mov rax,rcx; movups [rcx],xmm0; ret` | **hidden ptr RCX → RAX** |
| `ret_Mat4()` | 64B | 64 | `mov rax,rcx; movups [rcx],xmm0 ×4; ret` | **hidden ptr RCX → RAX** |

**Positional register allocation (Rust, mixed scalar+struct):**

```
mix(f32 s, Vec3 v):     s in XMM0 (pos1 float),  v ptr in RDX (pos2 int)
                       addss xmm0,[rdx]; addss [rdx+4]; [rdx+8]     [emp]
mix2(Vec3 v, f32 s):    v ptr in RCX (pos1 int),  s in XMM1 (pos2 float)
                       movss xmm0,[rcx]; …; addss xmm0,xmm1         [emp]
```

**Conclusion (Rust/Win64):** `#[repr(C)]` + `extern "C"` makes Rust pass/return
structs with byte-identical semantics to MSVC C: ≤8B → one integer register
(RCX-slot / RAX), >8B → hidden pointer in RCX (return: ptr also returned in
RAX). Pure-float content does **not** move a struct into XMM. This matches the
C probe exactly (§3), so a Rust↔C FFI on Win64 is ABI-faithful **as long as**
the >8B-by-value cases agree with what the C side compiles to (see §3.3 for
the one caveat).

---

## 2. C++ struct layout on x86-64

### 2.1 Standard-layout (POD) = C layout, on every ABI

For a *standard-layout* class (no virtuals, no base classes, no non-static
access control, no reference members, no non-standard-layout non-static data
members — roughly "looks like a C struct"), **both** the Itanium C++ ABI
(GCC/Clang on Linux/macOS/mingw) and the MSVC C++ ABI produce the **same**
layout as the base C ABI.

Itanium C++ ABI §2.2 (itanium-cxx-abi.github.io):

> "The size and alignment of a type which is a POD for the purpose of layout
> is as specified by the base C ABI … If the base ABI does not specify rules
> for empty classes, then an empty class has size and alignment 1."

The C layout rule (Microsoft x64 type/storage layout, verbatim):

> - "The alignment of the beginning of a structure or a union is the
>   **maximum alignment of any individual member**. Each member within the
>   structure or union must be placed at its proper alignment … which may
>   require implicit internal padding, depending on the previous member."
> - "**Structure size must be an integral multiple of its alignment**, which
>   may require padding after the last member. Since structures and unions
>   can be grouped in arrays, each array element of a structure or union must
>   begin and end at the proper alignment."
> - "The alignment of an array is the same as the alignment of one of the
>   elements of the array."

Algorithm (identical to Rust `repr(C)`, §1.2):
1. `align(S) = max(align(member_i))`.
2. Walk members in declaration order; before placing member *i*, pad `offset`
   up to a multiple of `align(member_i)`.
3. `sizeof(S) = round_up(offset_after_last_member, align(S))` (trailing
   padding so arrays of `S` stay aligned).

### 2.2 `offsetof` and `sizeof`

- `sizeof(T)` = the rounded-up size **including trailing padding**. This is
  the stride of `T` in an array. Two adjacent `Vec3` in `Vec3[2]` are 12 bytes
  apart (no extra padding, because 12 is already a multiple of `align=4`).
- `offsetof(T, m)` = the byte offset of `m` from the start of `T`. Valid for
  standard-layout types. For non-standard-layout it is
  *conditionally-supported* (see §2.3).
- `alignof(T)` / `_Alignof(T)` = `align(T)`.

### 2.3 Non-standard-layout: Itanium vs MSVC diverge; do not FFI by value

The moment a C++ type has any of: a virtual function, a base class, a
non-trivial copy/move constructor or destructor, private/protected non-static
data members, or reference members — it is **non-standard-layout**, and:

- **Layout diverges between Itanium and MSVC.** Itanium puts the vtable
  pointer at offset 0, lays out non-virtual bases, then members, then virtual
  bases, with `dsize`/`nvsize` tail-padding reuse for non-POD bases. MSVC uses
  a different vtable / base layout scheme. The same source `struct` can have
  different `sizeof`/offsets under the two ABIs.
- **`offsetof` is conditionally-supported** — the compiler warns:
  `warning: 'offsetof' within non-standard-layout type 'WithVtable' is
  conditionally-supported [-Winvalid-offsetof]`. `[emp]`
- **Itanium: non-trivial-for-calls types are passed by invisible reference**
  (Itanium C++ ABI: "A type is considered non-trivial for the purposes of
  calls if: it has a non-trivial copy constructor, move constructor, or
  destructor, or all of its copy and move constructors are deleted … passed
  and returned according to the rules of the base C ABI [only if trivial]").
  Win64 has the analogous "no user-defined ctor/dtor/copy-assign" requirement
  for return-by-value-in-RAX (see §3.3).

Empirical (mingw g++ 15.2, Itanium ABI), `docs/abi_evidence/probe_cpp.cpp`:

```
POD / standard-layout (== C):
  Vec3 {f32,f32,f32}        size=12 align=4   off(x,y,z)=(0,4,8)
  Mat4 {f32[16]}            size=64 align=4
  MixID {i32,f64}           size=16 align=8   off(a,b)=(0,8)      // 4B pad after a
  Nest {Vec3; f32 w}        size=16 align=4   off(v,w)=(0,12)
  ArrIn {f32[4]; i32}       size=20 align=4   off(arr,tag)=(0,16)
  Empty {}                  size=1  align=1                       // C++ empty = 1 byte

Non-standard-layout (Itanium; MSVC differs):
  Base {i32 bi}             size=4  align=4   off(bi)=0
  Derived : Base {virt f; i32 di}  size=16 align=8               // vptr@0, bi@8, di@12, pad->16
  WithVtable {virt g; i32 a; f32 b} size=16 align=8 off(a,b)=(8,12)  // vptr@0
  (offsetof on these -> compiler warning, conditionally-supported)

alignas:
  alignas(16) Mat4A {f32[16]}    size=64 align=16
  Mat4Inner16 {alignas(16) f32 m[16]} size=64 align=16 off(m)=0
```

**Rule for FFI:** expose only standard-layout (POD-equivalent) types across
the boundary, and expose them as `#[repr(C)]` on the Rust side. Hold
non-standard-layout C++ types opaquely (as `*const c_void` / byte buffer) and
never pass them by value.

### 2.4 Itanium `dsize`/`nvsize` (why tail padding matters for non-POD)

Itanium C++ ABI defines (internal to the spec):
- `dsize(O)` — data size, "the size of O **without tail padding**."
- `nvsize(O)` — non-virtual size, "size of O without virtual bases."
- `nvalign(O)` — non-virtual alignment.

For POD, `dsize == sizeof`. For **non-POD** base classes, the derived class
may **reuse the base's tail padding** for its own members (because the
standard forbids `memcpy` of a base subobject, so the bytes are reusable).
This is why a non-POD base + derived member can produce a `sizeof` smaller
than the naive "base sizeof + member" sum, and why Itanium and MSVC layouts
can differ by a few bytes. This never affects standard-layout FFI types, but
it is the reason "C++ struct layout" is only trivially "C layout" for POD.

---

## 3. Win64 (Microsoft x64) calling convention for structs

Source: Microsoft, "x64 calling convention" + "x64 software conventions → x64
type and storage layout" (learn.microsoft.com), verified by disassembly.

### 3.1 The core struct rule

Microsoft x64 ABI, verbatim:

> "`__m128` types, arrays, and strings are never passed by immediate value.
> Instead, a pointer is passed to memory allocated by the caller. **Structs
> and unions of size 8, 16, 32, or 64 bits, and `__m64` types, are passed as
> if they were integers of the same size.** Structs or unions of other sizes
> are passed as a pointer to memory allocated by the caller. For these
> aggregate types passed as a pointer, including `__m128`, the
> caller-allocated temporary memory must be 16-byte aligned."

Decode (the "8, 16, 32, or 64 bits" list = 1, 2, 4, 8 **bytes**):

| Struct size | Passed as | Where |
|---|---|---|
| 1, 2, 4, 8 bytes | a single integer of that size | GP register of its argument position (RCX/RDX/R8/R9) |
| > 8 bytes | hidden pointer to caller-allocated memory | GP register of its argument position; callee dereferences |

**Float content does not change this.** A `{float, float}` (8B) is "passed as
if it were an 8-byte integer" → it goes in **RCX**, packed, not in XMM0. This
is the single biggest difference from System V (§5) and the most common
mis-statement about Win64. `[emp]` (`take_FF`: `movd xmm0,ecx` — callee reads
the floats out of the integer register RCX.)

Scalar (non-aggregate) `float`/`double` arguments are the only things that use
XMM0–XMM3. Aggregates never use XMM for their *carrier*, even when full of
floats.

### 3.2 Return values

Microsoft x64 ABI, verbatim:

> "A scalar return value that can fit into 64 bits, including the `__m64`
> type, is returned through **RAX**. Nonscalar types including floats,
> doubles, and vector types such as `__m128`, `__m128i`, `__m128d` are
> returned in **XMM0**. The state of unused bits in the value returned in RAX
> or XMM0 is undefined."

> "User-defined types can be returned by value … To return a user-defined
> type by value in **RAX**, it must have a length of 1, 2, 4, 8, 16, 32, or 64
> bits. It must also have **no user-defined constructor, destructor, or copy
> assignment operator**. It can have no private or protected nonstatic data
> members, and no nonstatic data members of reference type. It can't have base
> classes or virtual functions. … This definition is essentially the same as
> a **C++03 POD** type. … Otherwise, the caller must allocate memory for the
> return value and pass a pointer to it as the **first argument**. The
> remaining arguments are then **shifted one argument to the right**. The same
> pointer must be returned by the callee in **RAX**."

Decode:

| Return type | Returned via |
|---|---|
| scalar ≤ 64 bits (int/ptr/`__m64`) | RAX |
| scalar `float`/`double`/`__m128*` | XMM0 |
| user-defined ≤ 8 bytes AND C++03-POD | RAX (packed) |
| user-defined > 8 bytes, OR non-POD | hidden pointer: caller allocates, passes ptr in **RCX** (arg 1), args shift right, callee returns ptr in RAX |
| (MSVC-only, see §3.3) 16-byte POD | may use RAX(+RDX) |

Empirical (gcc/mingw and rustc/LLVM-MSVC): `ret_FF` (8B pure float) returns in
**RAX** as `0x400000003f800000` (the two floats packed), **not** XMM0. All
>8-byte structs returned via hidden RCX pointer. `[emp]`

The MS examples, verbatim, are the authoritative reference for arg placement
with a hidden return pointer:

```
struct Struct1 { int j, k, l; };              // exceeds 64 bits
Struct1 func3(int a, double b, int c, float d);
// Caller allocates for Struct1, passes pointer in RCX,
//   a in RDX, b in XMM2, c in R9, d passed on the stack;
// callee returns pointer to Struct1 in RAX.

struct Struct2 { int j, k; };                  // fits 64 bits, POD
Struct2 func4(int a, double b, int c, float d);
// a in RCX, b in XMM1, c in R8, d in XMM3;
// callee returns Struct2 by value in RAX.
```

Note `func3`: with the hidden return pointer occupying **position 1 (RCX)**,
`a` moves to **RDX**, `b` to **XMM2**, `c` to **R9**, and `d` (position 5)
**spills to the stack**. This is the positional rule (§3.4) in action.

### 3.3 The 16-byte POD return divergence (read this before you FFI)

The MS doc says a ≤16-byte *C++03-POD* user type **may** be returned in
registers (`RAX`, plus `RDX` for the second 8 bytes). This is an **MSVC
optimization**. Empirically, **neither toolchain available in this workspace
emits it**:

```
ret_pod16i()  -> Pod16i {i32,i32,i32,i32}  (16B, all-int POD)
  rustc/LLVM-MSVC:  mov rax,rcx; movaps xmm0,[rip]; movups [rcx],xmm0; ret   // hidden ptr
  gcc/mingw:        mov rax,rcx; mov [rcx],1; [rcx+4]=2; [rcx+8]=3; [rcx+0xc]=4 // hidden ptr
ret_pod16ptr() -> Pod16ptr {void*,void*}   (16B, 2 pointers)
  both:             mov rax,rcx; ...; movups [rcx],xmm0                   // hidden ptr
ret_Vec4()     -> Vec4 {f32×4}              (16B)
  both:             mov rax,rcx; movups [rcx],xmm0                        // hidden ptr
ret_DD()       -> DD {f64,f64}              (16B pure double)
  both:             mov rax,rcx; movups [rcx],xmm0                        // hidden ptr
```

So the **observed, uniform rule across GCC/mingw and rustc/LLVM-MSVC** is:
**>8 bytes → hidden pointer, no exceptions.** MSVC itself *can* return a
qualifying 16-byte POD in `RAX`+`RDX`; LLVM's MSVC target and GCC's mingw
target, in the versions tested here, do not for the cases above.

**Practical consequence:** a function that returns a >8-byte struct by value
can be a **cross-toolchain ABI mismatch** if one side is MSVC and the other is
LLVM/GCC. This workspace's host is MSVC-ABI; the JIT emits its own code. The
safe, portable rule:

> **For any struct > 8 bytes, do not return it by value across the FFI/JIT
> boundary.** Have the caller pass an explicit out-pointer (`T* out` /
> `*mut T`) and have the callee write through it. This is identical to what
> the ABI does anyway (hidden pointer), but it makes the contract explicit
> and toolchain-independent.

For ≤8-byte POD, return-by-value in RAX is uniform and safe across all three
toolchains (verified: `ret_II`, `ret_FF`, `ret_pod8i` all use RAX in C, C++,
and Rust).

### 3.4 Argument registers are positional (the rule everyone gets wrong)

Microsoft x64 ABI:

> "By default, the x64 calling convention passes the first four arguments to
> a function in registers. The registers used for these arguments **depend on
> the position and type** of the argument. Remaining arguments are passed on
> the stack in right-to-left order."

> "Integer valued arguments in the leftmost four positions are passed in
> left-to-right order in RCX, RDX, R8, and R9 … Any floating-point and
> double-precision arguments in the first four parameters are passed in
> XMM0–XMM3, depending on position."

The decisive MS example (and the thing that distinguishes "positional" from
"independent counting"):

```
func3(int a, double b, int c, float d);
// a in RCX, b in XMM1, c in R8, d in XMM3
```

If register slots were *independently counted* (Nth integer → Nth GP, Mth
float → Mth XMM), `b` would be XMM0, `c` would be RDX, `d` would be XMM1.
They are not. `b` is **XMM1**, `c` is **R8**, `d` is **XMM3** — i.e. the
**argument position** picks a fixed `(GP, XMM)` pair and the argument uses the
half matching its type:

| arg position | GP register | XMM register |
|---|---|---|
| 1 | RCX | XMM0 |
| 2 | RDX | XMM1 |
| 3 | R8  | XMM2 |
| 4 | R9  | XMM3 |
| ≥5 | — (stack, right-to-left, 8-byte aligned) | — |

A hidden return pointer, when present, occupies **position 1's GP slot
(RCX)** and shifts every subsequent argument up by one position (so `a`→RDX,
`b`→XMM2, `c`→R9, `d`→stack — exactly the `func3` example above).

Empirical proof (`docs/abi_evidence/probe_slots.asm`), caller setup:

```
alt(int a, float b, int c, float d):
    mov ecx,1        ; a  -> RCX  (pos1 GP)
    movss xmm1,...   ; b  -> XMM1 (pos2 XMM)   <-- NOT XMM0
    mov r8d,3        ; c  -> R8   (pos3 GP)    <-- NOT RDX
    movss xmm3,...   ; d  -> XMM3 (pos4 XMM)   <-- NOT XMM1

altf(float a, int b, float c, int d):
    movss xmm0,...   ; a -> XMM0 (pos1 XMM)
    mov edx,2        ; b -> RDX  (pos2 GP)
    movss xmm2,...   ; c -> XMM2 (pos3 XMM)
    mov r9d,4        ; d -> R9   (pos4 GP)

five(int a, float b, int c, float d, int e):
    ecx, xmm1, r8d, xmm3  ; e (pos5) -> [rsp+0x20]  (stack)

two_II(II, II):  rcx = 0x200000001 ; rdx = 0x400000003   (two 8B structs, pos1+pos2 GP)
```

All four calls match the positional model and contradict independent-counting.
This is confirmed verbatim by the MS `func3` example. **Cite the positional
rule, not the independent-counting one.**

### 3.5 Shadow space, register spill, stack alignment

Supporting details that matter when the JIT must build a call frame:

- **Shadow space:** the caller **always** reserves 32 bytes (4 × 8) on the
  stack above the return address, even if fewer than 4 register args are used.
  The callee may spill RCX/RDX/R8/R9 into this space (e.g. to take addresses).
  A JIT calling a Win64 function must allocate this 32 bytes regardless of
  arity.
- **Stack alignment:** at the point of `call`, `(RSP+8)` (return address
  pushed) must be 16-byte aligned — i.e. RSP must be 16-aligned *before* the
  `call` (the `call` pushes 8, making RSP ≡ 8 mod 16 on entry, which the
  prologue then re-aligns). Leaf functions are exempt.
- **5th+ args:** pushed right-to-left, each 8-byte aligned, **after** the
  32-byte shadow store.
- **`__m128`/arrays/strings:** always by pointer (caller-allocated, 16-byte
  aligned), even if "≤8 bytes" by some reading — arrays are never passed by
  immediate value.
- **Varargs / unprototyped:** floats must be duplicated into the
  corresponding GP register too (the callee may expect the value in the
  integer register). A JIT that emits variadic calls must honor this.

### 3.6 Full empirical summary (C, mingw gcc 15.2)

`docs/abi_evidence/probe_c.asm`, `probe_caller.asm`. Callee view (how the
struct arrives) and caller view (how the caller sets up):

| Case | Size | Passing | Returning |
|---|---|---|---|
| `{i32,i32}` | 8 | RCX (packed int) | RAX (packed) |
| `{f32,f32}` | 8 | RCX (packed int, **not** XMM) | RAX (packed, **not** XMM0) |
| `{i32,f32}` | 8 | RCX (packed int) | RAX |
| `{i32,f64}` | 16 | hidden ptr in RCX | hidden ptr RCX → RAX |
| `{f64,f64}` | 16 | hidden ptr in RCX | hidden ptr RCX → RAX |
| `{f32,f32,f32}` (Vec3) | 12 | hidden ptr in RCX | hidden ptr RCX → RAX |
| `{f32[16]}` (Mat4) | 64 | hidden ptr in RCX | hidden ptr RCX → RAX |
| `{i32,i32,i32,i32}` | 16 | hidden ptr in RCX | hidden ptr RCX → RAX |
| mixed `f(int,float,int,float)` | — | RCX,XMM1,R8,XMM3 | — |
| ret-ptr shifts args `f()->{i32,i32,i32}(int,double,int,float)` | — | ret ptr RCX; a=RDX, b=XMM2, c=R9, d=stack | ptr in RAX |

---

## 4. Practical examples — sizes, alignments, layouts

All values verified on this workspace (`sizes.exe`, `probe_cpp.cpp`,
`rustabi sz` probe). C, C++, and Rust agree to the byte.

### 4.1 `Vec3 { float x, y, z; }`

```
size  = 12    align = 4
x@0  y@4  z@8   (no internal padding; 12 is already a multiple of 4)
no trailing padding (12 % 4 == 0)
```
Passed/returned by **hidden pointer** on Win64 (12 > 8). On System V, one SSE
eightbyte + one partial → XMM0 + RAX (≤16B, in registers).

### 4.2 `Vec4 { float x, y, z, w; }`

```
size  = 16    align = 4      <-- align is 4, NOT 16
x@0 y@4 z@8 w@12   no padding at all
```
`align=4` because the most-aligned member is `float` (align 4). **16 bytes
does not imply 16-byte alignment.** For aligned SIMD loads (`movaps`) you must
force `alignas(16)` / `#[repr(C, align(16))]`. Passed by hidden pointer on
Win64 (16 > 8).

### 4.3 `Mat4 { float m[16]; }`

```
size  = 64    align = 4      <-- align is 4, NOT 16
m@0 .. m@60   (array of 16 floats, element align 4 => array align 4)
no internal or trailing padding
```
**This is the one that bites.** `Mat4` is 64 bytes of `float`s; naïve
assumption is "it's 16-byte aligned because it's a matrix." It is not —
`align=4`. To store it in a 16-byte-aligned buffer for `movaps`/shader
upload, declare:

```c
struct alignas(16) Mat4A { float m[16]; };   // C/C++:  size=64 align=16
```
```rust
#[repr(C, align(16))] struct Mat4A { m: [f32; 16] }   // Rust: size=64 align=16
```
Verified: both give `size=64, align=16`. Passed by hidden pointer on Win64.

### 4.4 `struct { int32_t a; double b; }` (the padding classic)

```
size  = 16    align = 8
a@0  pad@4-7   b@8-15   (4 bytes of internal padding after a)
no trailing padding (16 % 8 == 0)
```
`a` is 4 bytes but `double` needs 8-byte alignment → 4 bytes of padding
between `a` and `b`. This matches the Microsoft x64 ABI example
`{int a; double b; short c;}` (24B, align 8: a@0, pad@4-7, b@8-15, c@16-17,
pad@18-23) exactly. Passed by hidden pointer on Win64 (16 > 8); on System V
this exact struct is **split**: `a` in EDI (INTEGER eightbyte), `b` in XMM0
(SSE eightbyte) — see §5.

### 4.5 Nested, arrays-in-struct, empty

```
Nest   { Vec3 v; float w; }   size=16 align=4  v@0 w@12          // w@12, 0 trailing pad
ArrIn  { float[4]; int tag; } size=20 align=4  arr@0 tag@16      // 20%4==0, no trailing pad
Empty  {}                     size=1  align=1                    // C++: empty struct = 1 byte
```
`Nest`: `v` occupies 0–11, `w` at 12 (its align 4, 12 is a multiple of 4),
total 16. `ArrIn`: `float[4]` is 16 bytes (align 4), `tag` at 16, total 20.
C++ `Empty` is size 1 (so distinct objects have distinct addresses); C does
not allow empty structs; Rust ZSTs are size 0.

### 4.6 The repr(Rust) vs repr(C) object lesson

```
{ u8 a; u64 b; u8 c }
  repr(Rust): size=16 align=8   b@0, a@8, c@9   (reordered, 6B tail pad)
  repr(C):    size=24 align=8   a@0, pad, b@8, c@16, pad->24  (15B padding)
```
Same field set, two different sizes and three different field offsets. This
is why `#[repr(C)]` is not optional for FFI.

---

## 5. Win64 vs System V AMD64 — the divergence that corrupts JIT ABIs

If the host compiles to MSVC ABI (this workspace) but a JIT/codegen path ever
targets, or interops with, System V (Linux/macOS x86-64, or a `sysv_abi`
function on Windows), the struct-passing rules are **not** the same. Verified
side-by-side with `__attribute__((sysv_abi))` on the *same* source
(`docs/abi_evidence/probe_abi_attr.asm`):

### 5.1 Same struct, two ABIs — `{int32, double}` (16B mixed)

```
take_ID_ms    (Win64):   cvtsi2sd xmm0,[rcx];  addsd xmm0,[rcx+8]
                          -> struct by HIDDEN POINTER in RCX            [emp]

take_ID_sysv  (SysV):    cvtsi2sd xmm0,edi;   movq rax,xmm0;  addsd xmm0,xmm1
                          -> a in EDI (INTEGER eightbyte), b in XMM0      [emp]
                             (struct SPLIT across GP + XMM, no pointer)
```

### 5.2 Same struct, two ABIs — `{float, float}` (8B pure float)

```
take_FF_ms    (Win64):   movd xmm0,ecx;  movq xmm2,rcx;  shufps ...
                          -> struct packed in RCX (integer reg)          [emp]

take_FF_sysv  (SysV):    movdqa xmm2,xmm0;  shufps xmm2,...;  addss xmm0,xmm2
                          -> struct in XMM0 (SSE eightbyte)              [emp]
```

### 5.3 The System V rules (for contrast)

System V AMD64 ABI (refspecs.linuxbase.org/elf/x86_64-abi-0.99.pdf):

- Aggregates are classified in **8-byte "eightbyte"** chunks; each eightbyte
  is independently INTEGER / SSE / SSEUP / X87 / MEMORY.
- "If the size of an object is larger than **four eightbytes**, or it contains
  unaligned fields, it has class MEMORY." Post-merger: if any eightbyte is
  MEMORY, the whole thing is in memory; if size > **two eightbytes** (>16B)
  and not a homogeneous vector aggregate, it's in memory.
- "**≤ 16 bytes** (≤2 eightbytes) → passed in registers," one register per
  eightbyte: INTEGER→GP (`rdi,rsi,rdx,rcx,r8,r9`), SSE→XMM (`xmm0..xmm7`).
  A mixed struct uses **one GP and one XMM**.
- **Homogeneous float aggregates** (up to 4 floats / 2 doubles, all same
  float class) → packed into consecutive XMM registers.
- Integer args: **6** GP registers (RDI, RSI, RDX, RCX, R8, R9) + 8 XMM.
  (Win64 has only 4 GP + 4 XMM.)
- Return: INTEGER→`rax,rdx`; SSE→`xmm0,xmm1`; MEMORY→hidden pointer passed in
  **RDI** (not RCX), returned in RAX.
- C++ non-trivial-for-calls type → invisible reference (Itanium rule, §2.3).
- Stack 16-byte aligned at call; no shadow space; 5th+ arg on stack.

### 5.4 Summary of differences

| Aspect | Win64 (MS x64) | System V AMD64 |
|---|---|---|
| Struct ≤ 8B | one **integer** register (RCX-slot), even if all floats | eightbyte class: ints→GP, floats→XMM; ≤16B in regs |
| Struct 9–16B | **hidden pointer** (RCX) | **in registers**, split per eightbyte (GP+XMM if mixed) |
| Struct > 16B | hidden pointer | hidden pointer (in RDI) |
| Pure-float 8B struct `{f32,f32}` | RCX (integer!) | XMM0 |
| Mixed 16B `{i32,f64}` | hidden pointer | EDI (int) + XMM0 (double) |
| Arg GP regs | RCX,RDX,R8,R9 (4) | RDI,RSI,RDX,RCX,R8,R9 (6) |
| Arg XMM regs | XMM0–XMM3 (4) | XMM0–XMM7 (8) |
| Reg assignment | **positional** (pos→GP/XMM pair) | per-eightbyte class, in order |
| Hidden return ptr | RCX (arg 1, shifts rest) | RDI (arg 1) |
| 16B POD return | MSVC: RAX(+RDX); LLVM/GCC: hidden ptr `[emp]` | RAX+RDX or XMM0+XMM1 per class |
| Stack at call | 16B aligned, **+32B shadow space** | 16B aligned, no shadow space |
| 5th+ arg | stack, right-to-left | stack, right-to-left |
| C++ non-trivial type | by hidden ptr (not in RAX) | invisible reference |

**Implication for the host/JIT ABI:** the host emits MSVC-ABI call sites.
Any JIT-compiled call that targets a SysV-compiled function (or vice versa)
for a struct > 8 bytes, or any struct with mixed/float content, will read
the wrong registers. The host must either (a) always emit Win64 call frames
and only ever link Win64 functions, or (b) explicitly marshal at the
boundary (out-pointers for >8B, never rely on struct-in-register across ABI
families). Option (a) is what this workspace does.

---

## 6. Recommendations for the host↔ember struct ABI

1. **Every FFI struct is `#[repr(C)]`** on the Rust side and a plain
   standard-layout struct on the C/C++ side. No `repr(Rust)` types in the ABI
   graph; no non-standard-layout C++ types in the ABI graph.
2. **`repr(C)` must be applied recursively** to every nested struct that is
   part of the ABI. Inner `repr(Rust)` types keep Rust layout (§1.3).
3. **Pass structs > 8 bytes by explicit out-pointer** (`*const T` in /
   `*mut T` out), not by value, across host↔JIT. This matches what Win64
   does anyway and is immune to the MSVC-vs-LLVM 16B-return divergence (§3.3).
   Structs ≤ 8 bytes may be passed/returned by value (one integer register /
   RAX) safely.
4. **`Mat4` (and any 16-byte-SIMD type) must be `alignas(16)` /
   `#[repr(C, align(16))]`** if the host or JIT issues aligned 128-bit
   loads/stores (`movaps`) or uploads to shaders expecting 16-byte alignment.
   Default `Mat4` is `align 4` (§4.3).
5. **The JIT must emit Win64 call frames**: positional `(RCX,RDX,R8,R9)` +
   `(XMM0–XMM3)` register allocation (§3.4), 32-byte shadow space, 16-byte
   stack alignment at `call`, hidden return pointer in RCX for >8B returns,
   5th+ args on the stack right-to-left.
6. **Never pass a C++ type with virtuals / bases / non-trivial ctor-dtor by
   value.** `offsetof` is conditionally-supported on such types and their
   layout differs between Itanium and MSVC (§2.3). Hold them opaquely.
7. **Do not assume "8 bytes of floats → XMM."** On Win64, an 8-byte
   `{float,float}` struct goes in an **integer** register (RCX), not XMM0.
   Only scalar `float`/`double` arguments use XMM0–XMM3 (§3.1).
8. **If the JIT ever targets System V** (cross-platform codegen, or a
   `sysv_abi` interop), re-derive every struct call: ≤16B structs go in
   registers with eightbyte classification, mixed structs split GP+XMM, pure-
   float structs use XMM, hidden return pointer is in RDI, there are 6 GP + 8
   XMM arg registers, and there is no shadow space (§5). A Win64 frame and a
   SysV frame are not interchangeable.

---

## 7. Sources (all fetched and verified during this research)

- Microsoft, "x64 calling convention" — learn.microsoft.com/en-us/cpp/build/x64-calling-convention
- Microsoft, "x64 software conventions → x64 type and storage layout" — learn.microsoft.com/en-us/cpp/build/x64-software-conventions
- Microsoft, "/Zp (Struct Member Alignment)" — learn.microsoft.com/en-us/cpp/build/reference/zp-struct-member-alignment
- Rust Reference, "Type layout" (repr, repr(C) algorithm, modifiers, non-recursion) — doc.rust-lang.org/reference/type-layout.html
- Rust Reference, "External blocks → ABI" (extern "C"/"system"/"win64"/"sysv64") — doc.rust-lang.org/reference/items/external-blocks.html
- Rustnomicon, "FFI" — doc.rust-lang.org/nomicon/ffi.html
- Itanium C++ ABI (§2.2 POD, §2.4 Non-POD, dsize/nvsize, non-trivial-for-calls) — itanium-cxx-abi.github.io/cxx-abi/abi.html
- System V AMD64 ABI 0.99 (eightbyte classification, passing, returning) — refspecs.linuxbase.org/elf/x86_64-abi-0.99.pdf

Empirical artifacts: `docs/abi_evidence/` — `probe_c.asm`, `probe_caller.asm`,
`probe_slots.asm`, `probe_ret16.asm`, `probe_pod16.asm`, `probe_abi_attr.asm`,
`probe_cpp.cpp`, `rustabi.asm`, `rustabi2.asm`, `sizes.exe`.

Toolchain: `gcc 15.2.0 (mingw, x86_64-w64-mingw32)`, `rustc 1.95.0
(x86_64-pc-windows-msvc, LLVM 22.1.2)`, `GNU objdump`. All disassembly is of
real compiled object files, not hand-written.
