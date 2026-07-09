# Research Notes - prior art survey

Sources examined before design:

## binprotect (`hyper_workspace/binprotect`)
- x64 PE bin2bin obfuscator, own `ext/binwrite` lib on top of Zydis (decode+encode).
- `binwrite::assembler_instruction_t` wraps one `ZydisEncoderRequest` + calls
  `ZydisEncoderEncodeInstruction` per instruction. No concept of labels, no
  relocation fixups, no executable-memory allocator. It's built for patching
  bytes inside an already-laid-out PE image, not for emitting fresh code at
  runtime.
- Verdict: **not directly reusable as a JIT backend.** Zydis itself (decoder)
  is useful for a disassembling debug/verify tool later, but for the actual
  JIT we need our own encoder with labels/patches/exec-mem, scoped to the
  small instruction subset we actually emit. Zero new dependency either way.

## Prior native-JIT scripting language (surveyed)

Two sources: the closed-source SDK headers (`.lib` only - no
implementation) and the public language docs. The docs reveal this
language is **far larger than `sdk.h` alone suggested** - it's a full
systems-level language, not a small scripting dialect. Surveyed surface:

**From the embedding-API headers (this is the part we mirror):**
- `engine_t` / `module_t` / `context_t` split (engine=registry, module=compiled
  unit, context=execution/call state) - same shape as AngelScript.
- `register_native(engine, name, fn_ptr, ret_type, params[], count, permission)`
  - flat native registration, permission bit for FFI gating.
- `type_builder` fluent API: `.method()`, `.field()`, `.subscript()`,
  `.iterable()`, operator overloads (`bin_add`, `bit_and`, `shl`...),
  `.convert()`, `.property()`, `.finish()`.
- Budget/GC/debug-hook knobs, hot reload, serialization, introspection,
  exceptions-as-data (`exception_pending/value/type/clear`).

**From the docs (the language itself - this is the part we deliberately
scope DOWN for v1):**
- Primitives incl. `aint8/16/32/64` (atomic), `char`/`wchar`, `null`.
- `const`/`constexpr`/`nullable`/`auto` inference.
- Operators incl. ternary `?:`, `++`/`--`, `cast<T>()`, `sizeof`, `offsetof`.
- Control flow: `if`/`else`, `while`, `do-while`, `for`, `for-each`,
  `switch`, `match`, `break`/`continue`, `goto`, `defer`.
- Functions: default args, reference (`&`), `out`, variadic, `extern`,
  const methods, function references, lambdas `[caps](p)->T{}` + arrow
  `(p)=>expr`.
- Dynamic arrays (push/pop/sort/slice/...), maps, strings with
  interpolation `f"v={x}"` + full method set.
- Structs (ctors/dtors/methods/operator overloading/bitfields/packed) AND
  classes (reference types, multi-inheritance w/ vtable thunks, RAII),
  interfaces, mixins, properties, templates (monomorphization), enums,
  typedefs, delegates, namespaces, coroutines + `yield`, exceptions
  try/catch/throw w/ unwinding, heap `new`/`delete`, inline asm intrinsics,
  `[[packed]]`/`[[align]]`/`[[reflect]]`/`[[serialize]]`/`[[noopt]]`/
  `[[inline]]`/`[[dll(...)]]` annotations, modules w/ import, preprocessor
  (#define/#ifdef/#include/#pragma), `static_assert`.
- Lifecycle: `main()` entry returning int64 (>0 stay loaded, <=0 unload),
  `register_routine(cast(fn), data)` for per-frame callbacks, `unregister`.

**Takeaway / scoping decision.** The surveyed language's *embedding API
shape* (`engine/module/context`, `type_builder`, `register_native`, budget,
annotations, hot reload, exceptions-as-data) is our north star for ergonomics
and is mirrored in `BINDING_API.md` / `SAFETY_AND_SANDBOX.md` /
`HOT_RELOAD.md`. Its *language feature surface* (templates, classes, vtables,
coroutines, exceptions, heap, modules, preprocessor) is **deliberately not
v1** - each is a large subsystem that would make v1 unshippable and none is
required by the original request (which emphasized speed + bindings, not
language completeness). The YAGNI stance holds; see `ROADMAP.md` for the
per-feature deferral rationale and re-entry triggers. Several small C-style
conveniences the surveyed language has that *are* cheap and expected in a
C-style language (ternary, `++`/`--`, `switch`, `do-while`, `const`/`constexpr`,
`defer`, `sizeof`/`offsetof`, string interpolation, compound-assign
semantics) **are** added to v1 in this spec pass - see `GAP_ANALYSIS.md` for
the audit.

The surveyed language's implementation is closed, so ember's internals
(codegen, regalloc, IR) are our own design, informed by AngelScript (source
available) and Mun (source available, AOT/ABI shape).

## AngelScript (`vacnetme_workspace/fvc/thirdparty/angelscript_sdk`)
- `RegisterGlobalFunction(decl_string, funcptr, callConv, aux)`,
  `RegisterObjectType/Method/Property(decl_string, ...)` - declaration
  strings are parsed at registration time. Flexible but adds a mini-parser
  and runtime string matching cost to every bind. We will bind by explicit
  C++ descriptor structs instead (like the surveyed language's
  `native_desc`), skip the
  string-declaration parser - it's the single biggest source of AngelScript
  registration-time complexity (`as_callfunc.cpp`, calling-convention
  dispatch per-platform, ~thousands of lines) and we don't need it.
- Execution model: bytecode VM (`as_context.cpp`), not JIT by default - this
  is *why* AngelScript is slow relative to native code. JIT is an optional
  add-on hook in AS, bytecode is the baseline. Confirms our differentiator:
  make JIT the *only* mode, no bytecode interpreter fallback needed for v1.
- `as_callfunc_x64_msvc.cpp` / `as_callfunc_x64_gcc.cpp`: hand-written
  per-ABI native trampolines for calling arbitrary native function
  signatures from the VM. We need the equivalent (native call thunk
  generation) but can emit it as JIT'd code directly, i.e. the call site to
  a native fn is just a `mov`+`call` with correctly placed args - no separate
  trampoline layer needed since we're already emitting real x86-64.

## Mun (`E:/DEVELOPER/PROJECTS/mun`)
- Pipeline: `mun_syntax` (rowan-based parser) → `mun_hir` (name resolution,
  type inference, salsa incremental queries) → `mun_codegen` (inkwell/LLVM IR)
  → `mun_abi` (stable C ABI struct layout: `FunctionInfo`, `TypeInfo`,
  `DispatchTable`) → `mun_runtime` (loads compiled `.munlib` shared object,
  hot-swaps via dispatch table indirection on file change).
- Key idea worth stealing: **dispatch table indirection**. Every external
  call (script→host or script→script-function-that-may-be-hot-reloaded)
  goes through a function-pointer slot, not a direct call, so reload just
  overwrites the slot. Cheap enough (one extra indirect load) and gives us
  hot-reload without invalidating/relinking all callers.
- Mun's cost: full LLVM (inkwell) dependency, AOT-compiles to a `.dll`/`.so`
  on disk, then dlopen's it. That's compile-latency-heavy (LLVM opt
  pipeline) - wrong tradeoff for us. We want compile time in
  microseconds-to-low-milliseconds for small scripts, in-process, no temp
  files. So: borrow the *pipeline shape* (syntax→HIR→codegen→ABI) and the
  *dispatch-table hot-reload trick*, skip LLVM entirely.
- `mun_abi::type_id`/`TypeInfo`/`StructInfo` layout is a good template for
  our own stable ABI struct definitions (size/alignment/field offsets
  computed once, shared between host and script).
- **Now implemented (v0.1):** the dispatch-table indirection is proven by
  acceptance test 5 (recursive fib via `mov r11, [r11 + slot*8]; call r11`)
  and test 7 (the `.em` round-trip - `load_em_file` repoints the
  dispatch-table-base reloc at the loaded module's own table). The `.em`
  reloc system (`AbsFixup` → `EmReloc`, `em_file.hpp`) is ember's own design
  - informed by Mun's ABI shape but not copied; see `BUNDLING_AND_EM_MODULES.md`
  Section 2 and `CODEGEN_SPEC.md` Section 15.

## Conclusion driving the design doc
No new external dependency is needed. Own minimal x86-64 encoder (subset of
ISA actually emitted - moves, arithmetic, compares, jumps, calls, SSE scalar
ops), own label/patch/exec-mem layer, own tiny recursive-descent parser (no
parser generator needed for a C-subset grammar), own dispatch-table-based
native+hot-reload binding. AngelScript's decl-string parsing and Mun's LLVM
backend are both explicitly skipped as overkill for the performance and
compile-latency goals here. The v0.1 implementation (encoder, label/patch,
exec-mem, IR data model, `.em` serializer/loader) is the first concrete
realization of this conclusion; see `CODEGEN_SPEC.md` Section 12 + Section 15 for the
acceptance suite that proves it.
