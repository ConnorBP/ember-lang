# Plan — standalone EXE bundler: compile an ember script + runtime into one .exe

> **Status: DONE.** Option A (prebuilt stub + appended `.em` + 12-byte EMBD
> footer) ships as both `ember bundle` and `ember_bundle`; the runtime is
> `ember_stub_main`. `load_em_bytes`, `write_em_bytes`, atomic temp-file
> publication, permission policy, portable globals initialization,
> native/coroutine registration, preview packaging, and
> `bundler_test`/self-host preview gates are implemented. The body below is the
> historical architecture decision; future-tense statements are superseded
> where production code now exists.
>
> This is the natural companion to the existing `.em` pre-compile feature
> (`docs/BUNDLING_AND_EM_MODULES.md`). The `.em` format already exists;
> this plan is about **wrapping a `.em` + the ember runtime into a single
> self-contained executable** so an ember script ships as one file a user
> can double-click / run from a prompt without installing the ember CLI.

---

## 0. The three options, and the LAZY MODE pick

The task names three approaches. Here they are, weighed against what the
codebase already provides:

### Option A — runtime stub exe that embeds the .em + a minimal ember runtime

A pre-built stub executable (`ember_stub.exe`) that contains the ember
runtime (the loader + jit_memory + engine + the standard extensions) and a
`main()` that: reads an embedded `.em` from within its own binary, loads
it via the loader, and calls the entry. The bundler tool produces
`output.exe` by taking the stub + embedding the `.em` (as an appended
binary section, a Windows resource, or a C++ byte array).

- *Pro:* **no C++ compiler needed at bundle time.** The stub is built once
  (part of the ember build); the bundler does a file copy + embed. This is
  the LAZY MODE the task asks for.
- *Pro:* the stub is exactly the CLI's `--load-em` path (`examples/ember_cli.cpp`'s
  `if (!load_em_path.empty()) { ... }` block) with the `.em` source swapped
  from "a file path argument" to "bytes embedded in this exe." The CLI
  already proves this path works.
- *Pro:* **the `.em` format is the natural intermediate** — the bundler
  doesn't invent a new container; it compiles `.ember` → `.em` (the
  existing `emit-em` flow) and embeds the `.em` in the stub.
- *Con:* the output exe is the stub's size (the ember runtime + extensions)
  even for a tiny script. Acceptable — the ember runtime is small (static
  libs, no runtime deps beyond the C runtime); a stub is a few hundred KB.
- *Con:* the embedded `.em` is either appended (a footer the stub finds at
  runtime), a Windows resource (needs the resource compiler / `UpdateResource`
  at bundle time), or a compiled-in byte array (needs a C++ compiler at
  bundle time — that's Option B). The appended-section variant keeps the
  "no compiler at bundle time" property.

### Option B — compile the .ember to .em, link the .em + ember libs + a main() stub into a single exe via the C++ linker

The bundler generates a small C++ `main()` that contains the `.em` as a
byte array (`const unsigned char em_data[] = { ... };`), calls
`load_em_bytes(em_data, sizeof(em_data), ...)`, and runs the entry. Then
the bundler invokes the C++ compiler/linker (`cl.exe` / `g++`) to compile
this generated `.cpp` + link the ember static libs into a single exe.

- *Pro:* truly standalone exe, no stub overhead beyond what's linked, the
  `.em` is a compiled-in constant (no runtime self-extraction).
- *Con:* **requires a C++ toolchain at bundle time.** The bundler must
  find `cl.exe`/`g++` + the ember headers + the ember static libs. This is
  a heavy dependency — a user who just built the ember runtime has the
  toolchain, but a user distributing the bundler tool may not. This
  violates LAZY MODE (the task's explicit "which approach is simplest"
  framing).
- *Con:* the generated `.cpp` embeds the `.em` as a byte array — a large
  script produces a large `.cpp` (hundreds of KB of `0x..,` literals),
  which is slow to compile. The appended-section variant of Option A
  avoids this entirely (the `.em` is binary-appended, not compiled in).

### Option C — self-extracting exe that writes the .em to a temp file and runs it with a bundled ember_cli

The output exe contains the `.em` + a copy of `ember_cli.exe` (or links
its functionality). At runtime, it extracts the `.em` to a temp file,
then invokes `ember_cli run --load-em <tempfile>` (or calls the loader
directly on the temp file).

- *Pro:* conceptually simple — reuses the CLI verbatim.
- *Con:* **embeds a whole CLI in every output exe** — the output is
  `ember_cli.exe`'s size + the `.em`, duplicated per script. Wasteful.
- *Con:* temp-file friction (write the `.em` to `%TEMP%`, clean up after,
  handle write failure, handle the temp path having spaces/unicode). The
  appended-section variant of Option A avoids temp files entirely — the
  stub loads the `.em` straight from its own memory.

### Decision: Option A, appended-binary-section variant — LAZY MODE

**Option A with an appended binary section is the simplest correct design.**
The stub is built once (a CMake target alongside `ember_cli`); the bundler
tool (`ember bundle`) compiles `.ember` → `.em` (reusing the existing
`emit-em` pipeline) and produces `output.exe` by `copy ember_stub.exe
output.exe` + appending the `.em` bytes + a small footer. The stub's
`main()` reads its own file, finds the footer, loads the `.em` from the
appended bytes, and runs the entry. **No C++ compiler at bundle time. No
temp files at runtime. No whole-CLI duplication.** The stub links exactly
the libraries the CLI's `--load-em` path links (§3).

The key enabler, read firsthand in `src/em_loader.cpp` line 544-585:
`load_em_file_impl` reads the file into a `std::vector<uint8_t> file`
buffer, then calls `parse_file(file, parsed, registry, natives, err)` —
**the parsing is already buffer-based.** The file-reading is a thin
wrapper. So a `load_em_bytes(const uint8_t* data, size_t len, ...)` API —
which the stub needs to load the embedded `.em` without a temp file — is a
trivial refactor: skip the `ifstream`, take the buffer directly, call
`parse_file`. §4.1 details this.

---

## 1. What the stub is — and how the .em v5 IR format helps (or doesn't, for LAZY MODE)

### 1.1 The stub is the CLI's --load-em path, with the .em source swapped

The CLI's `--load-em` path (`examples/ember_cli.cpp`):
```cpp
if (!load_em_path.empty()) {
    std::unordered_map<std::string, NativeSig> load_natives;
    register_standard_bindings(load_natives);            // 1. register natives
    LoadedModule loaded; std::string lerr;
    if(!load_em_file(load_em_path.c_str(),loaded,&lerr,nullptr,&load_natives)){...}  // 2. load .em
    void* entry=loaded.entry_by_name(fn_name.c_str()); if(!entry)entry=loaded.entry();  // 3. find entry
    bool is_void = ...;                                   // 4. read return type
    int64_t result=call_i64_i64(entry);                   // 5. call entry
    return is_void?0:int(result);                         // 6. exit code
}
```

The stub's `main()` is this exact sequence with one change: step 2 calls
`load_em_bytes(embedded_ptr, embedded_len, ...)` instead of
`load_em_file(path, ...)`. The stub does not parse or sema anything — the
`.em` is already compiled (that's what `emit-em` did at bundle time). The
stub links the loader + the extensions (for the native allowlist the
loader resolves against) + the engine (for `call_i64_i64`). **No lexer,
no parser, no sema, no codegen-tree-walker at runtime** — the stub loads
pre-compiled code and calls it. This is the "load is `memcpy` + a couple
pointer fixups per function" payoff `BUNDLING_AND_EM_MODULES.md` §2.5
describes.

### 1.2 v4 (raw x86) vs v5 (IR) — which does the stub want?

The `.em` format has two on-disk shapes (`src/em_file.hpp`):

- **v4 (the current writer version, `EM_VERSION = 4`):** per-function raw
  x86 bytes + relocations. The loader `memcpy`s the code into an exec
  page, patches the relocs (dispatch table base, globals base,
  function-rodata base), resolves native bindings by name, publishes the
  dispatch table. **No re-emit.** The stub needs: `em_loader` +
  `jit_memory` + `engine` + the extensions (for the native allowlist).
- **v5 (Stage B, `EM_VERSION_V5 = 5`, inert but the writer/loader exist —
  `examples/em_v5_ir_test.cpp` proves the round-trip):** per-function IR
  blob (`serialize_thin_function` output) OR raw-x86 fallback. The loader
  deserializes the IR (`deserialize_thin_function`), validates it
  (`validate_thin_function`), re-emits x86 (`emit_x64` from
  `src/thin_emit.hpp`), then publishes the exec page. **The stub needs
  the re-emit backend (`thin_emit`) at runtime.** The v5 security model
  (`em_file.hpp` v5 SECURITY MODEL): the loader validates the IR before
  `alloc_executable_rw`, so a tampered v5 `.em` is rejected at IR
  validation with no exec page allocated.

**Historical recommendation:** for LAZY MODE, this plan selected v4. The
shipped bundler writes the production byte module through `write_em_bytes`, and
the trusted standalone stub explicitly opts into raw execution; untrusted
external loaders retain secure-default validated-IR policy. **For LAZY MODE,
the original design uses v4.** v4 is simpler: the stub links
`em_loader` (which is in `ember_frontend`, §3) and does not exercise the
IR re-emit path. The `.em` is raw x86 + relocs; load is `memcpy` + patch.
The stub is smaller (no `thin_emit`/`thin_ir_ser`/`thin_lower` symbols
pulled at runtime — though they're linked into `ember_frontend` anyway,
the v4 path doesn't call them). v4 is also the default writer version
(`em_writer.hpp`: `write_em_file` emits v3/v4; `write_em_file_signed`
emits v4 signed).

**How v5 helps (the task's framing):** v5's advantage is that the `.em`
carries IR, not raw x86 — so the `.em` is *target-agnostic at the IR
level* (the stub re-emits x86 for its own platform at load time). In
principle, a v5 `.em` built on one compiler/ABI could be loaded by a stub
built on a different compiler/ABI (the IR is re-emitted by the stub's
`emit_x64`). This is the "the stub just needs the loader + emit_x64"
framing. **For a standalone exe on one platform (the LAZY MODE use case),
this portability doesn't matter** — the bundler and the stub are built
from the same ember source, same compiler, same ABI. v4's raw x86 is
already correct for that stub. v5's re-emit is extra runtime work (and
extra link surface) for no benefit on a same-platform bundle.

**Where v5 would earn its keep:** if the bundler wanted to produce a
*signed, tamper-resistant* standalone exe. v5 is currently unsigned
(`em_file.hpp`: "v5 is UNSIGNED for Stage B; a v5-signed variant is
FUTURE work"). v4 has the Ed25519 signature block (F2 content
authentication — the loader verifies the signature before
`alloc_executable_rw`). So for a *signed* standalone exe, **v4 signed
(`write_em_file_signed`) is the path today**, and the stub verifies the
embedded `.em`'s signature against a compiled-in public key before
loading — a tampered exe (someone modified the appended `.em`) is rejected
rather than executed. §5 covers the signed variant.

**Recommendation: ship v4 (raw x86, unsigned) as the LAZY MODE default;
offer v4-signed as the tamper-resistant variant; note v5 as a future
smaller-output option once v5-signed exists.** The stub links
`em_loader` either way; the difference is whether the load path exercises
the re-emit (v5) or the memcpy (v4).

---

## 2. The end-to-end bundler flow

### 2.1 `ember bundle input.ember output.exe` — the CLI command

A new CLI action in `examples/ember_cli.cpp` (or a small separate tool,
but adding it to the CLI is simpler — the CLI already has the
compile-to-`.em` pipeline):

```
ember bundle <input.ember> <output.exe> [--fn NAME] [--signed <keyfile>]
```

What it does:
1. **Compile `.ember` → `.em`** — the exact `emit-em` flow the CLI already
   has (`ember_cli.cpp`'s `if (!emit_em_path.empty()) { ... }` block):
   read file → resolve imports → lex → parse → slot assignment →
   register standard bindings → struct layouts + sema → globals block →
   codegen + finalize each fn → build `EmModule` → `write_em_file` (or
   `write_em_file_signed` if `--signed`). The `.em` is written to a temp
   file (or held in memory — §4.2).
2. **Copy the stub to `output.exe`** — `std::filesystem::copy(
   ember_stub_exe_path, output.exe)` where `ember_stub_exe_path` is the
   stub built alongside the CLI (§3.1). The bundler knows the stub path
   (configured at build time, or found relative to the CLI's own exe).
3. **Append the `.em` + footer** — open `output.exe` in append mode,
   write the `.em` bytes, then write a 12-byte footer:
   ```
   footer (12 bytes, little-endian):
     magic      : u32 = 0x454D4244  ("EMBD" — ember bundle, appended)
     em_length  : u64              (byte length of the .em just appended)
   ```
   The footer lets the stub find the `.em` at runtime: read the last 12
   bytes of its own file, check the magic, read `em_length`, seek back
   `12 + em_length` bytes from the end, read the `.em`.

That's it. The bundler is: compile (existing pipeline) + copy stub +
append `.em` + append footer. **No C++ compiler invoked. No resource
compiler. No temp `.em` left on disk (the bundler can hold the `.em` in
memory and write it straight into `output.exe`).**

### 2.2 The footer format — why a footer, not a header

A header (prepend the `.em` before the stub's PE) would break the PE — a
Windows `.exe` starts with the PE header at offset 0; prepending bytes
makes the OS fail to load it. Appending is safe: the stub's PE is
unchanged, the OS loads it normally, and the stub's `main()` reads the
appended bytes from its own file after the PE end.

The footer (at the very end of the file) lets the stub find the `.em`
without scanning: read the last 12 bytes, check the magic, get the
length, seek. A scan-for-`EMBL`-magic approach would also work (the `.em`
header starts with `EMBL = 0x454D424C`, `em_file.hpp`) but risks a false
positive if the stub's PE coincidentally contains those bytes (unlikely
but the footer is robust by construction — `EMBD` is distinct from `EMBL`
and the footer is always the last 12 bytes).

**Footer magic choice:** `0x454D4244` (`"EMBD"` — ember bundle, appended).
Distinct from `EM_MAGIC` (`0x454D424C`, `"EMBL"`) and `EM_SIG_MAGIC`
(`0x454D5347`, `"EMSG"`). The stub checks `magic == EM_BUNDLE_MAGIC` and
rejects (clear error) if it doesn't match — a plain `ember_stub.exe` with
no `.em` appended reads a non-matching footer and prints "this stub has
no embedded module; use `ember bundle` to create one."

### 2.3 Finding the stub at bundle time

The bundler needs the stub's path. Two options:
- **(a) Build-time configured:** the CMake build writes the stub's path
  into a config header (`#define EMBER_STUB_PATH "..."`) or the bundler
  finds the stub next to the CLI exe (`ember_cli.exe`'s directory +
  `ember_stub.exe`). Simple, works for a from-source build.
- **(b) Bundled in the CLI:** the stub is embedded as a Windows resource
  in `ember_cli.exe` itself; the bundler extracts it to `output.exe` +
  appends the `.em`. This makes `ember_cli.exe` self-contained (the
  `bundle` command works without a separate stub file on disk) but
  bloats the CLI by the stub's size. 

**Recommend (a) — the stub is a sibling exe, found next to the CLI.** This
keeps the CLI lean and the stub is a normal CMake target. A user who
distributes the bundler ships `ember_cli.exe` + `ember_stub.exe` together.
The bundler's "stub not found" error tells the user to ship the stub
alongside the CLI.

---

## 3. What the stub links — the library map

The ember build produces three libraries (`CMakeLists.txt` lines 66-128):

| Library | Contains | Links |
|---|---|---|
| `ember` (core) | `jit_memory.cpp`, `engine.cpp`, `em_writer.cpp`, `module_registry.cpp`, `thin_ir.cpp`, `em_type_codec.cpp`, `thin_ir_ser.cpp` | `ember_ed25519` |
| `ember_frontend` | `lexer.cpp`, `types.cpp`, `parser.cpp`, `sema.cpp`, `codegen.cpp`, `thin_lower.cpp`, `peephole.cpp`, `thin_emit.cpp`, **`em_loader.cpp`**, `ember_pass.cpp` | `ember` (PUBLIC) |
| `ember_import` | `import.cpp` | `ember`, `ember_frontend` |
| `ember_ext_*` (per extension) | one extension `.cpp` | `ember_frontend` (PUBLIC) |

**The stub needs at runtime:**
- `load_em_file` / `load_em_bytes` → `em_loader.cpp` → **`ember_frontend`**
- `call_i64_i64` → `engine.cpp` → **`ember`** (transitively via `ember_frontend`)
- `alloc_executable` / `free_executable` → `jit_memory.cpp` → **`ember`**
- `LoadedModule` destructor → `free_executable` → **`ember`**
- the native allowlist (the loader resolves native bindings by name
  against a host-supplied `unordered_map<string, NativeSig>`) → the
  extension libs' `register_natives` → **`ember_ext_*`** (transitively
  `ember_frontend` + `ember`)

**The stub does NOT need at runtime (for v4):**
- `lexer`/`parser`/`sema`/`codegen`/`thin_lower`/`peephole`/`thin_emit` —
  no source is compiled at runtime; the `.em` is pre-compiled. These are
  linked (they're in `ember_frontend` alongside `em_loader.cpp`) but the
  v4 load path doesn't call them. Static linking pulls only referenced
  symbols, so the stub's actual code footprint is the loader +
  jit_memory + engine + the extensions' native fn pointers.
- `ember_import` — no source imports at runtime (the `.em` is already
  link-resolved at bundle time). The stub does not link `ember_import`.
- `em_writer` — the stub loads `.em`, it doesn't write one. (`em_writer`
  is in `ember` core, linked transitively, but unreferenced.)

**So the stub links: `ember ember_frontend` + the `ember_ext_*` libs it
wants to support.** This is *exactly* the CLI's `--load-em` link line
minus `ember_import`:
```cmake
# the CLI's --load-em path links (CMakeLists.txt line 353-360):
ember ember_frontend ember_import
ember_ext_vec ember_ext_quat ember_ext_mat
ember_ext_string ember_ext_array ember_ext_math
ember_ext_sync ember_ext_lifecycle ember_ext_opt ember_ext_obf ember_ext_map

# the stub links the same MINUS ember_import (no source imports at runtime):
ember ember_frontend
ember_ext_vec ember_ext_quat ember_ext_mat
ember_ext_string ember_ext_array ember_ext_math
ember_ext_sync ember_ext_lifecycle ember_ext_map
# (opt/obf are pass extensions, not natives — not needed for loading)
```

**Which extensions?** The stub must register the same native allowlist
the bundler compiled against. If the script uses `array`/`string`/`math`/
`vec` etc., the stub must register those extensions' natives (the loader
resolves the `.em`'s native bindings by name against the stub's table; a
missing native is a load error — `em_loader.cpp` rejects unknown names).
**The stub registers all the standard extensions** (the same
`register_standard_bindings` the CLI uses), so any script that compiles
with the CLI loads with the stub. If the OS IO extensions (see
`plan_OS_IO_EXTENSIONS.md`) ship, a stub that supports IO scripts links
`ember_ext_io` too + registers `ext_io::register_all` and the script must
have been bundled with `--ffi` (the `.em` carries the native binding
names; the stub just needs them in its allowlist).

### 3.1 The stub as a CMake target

```cmake
# standalone stub exe: the runtime that loads an appended .em + runs it.
# Built alongside ember_cli; the `ember bundle` command copies this to
# output.exe + appends the .em. Links the load path (ember + ember_frontend
# + the standard extensions) but NOT ember_import (no source imports at
# runtime) and NOT the tree-walker/IR-emit beyond what em_loader pulls.
add_executable(ember_stub examples/ember_stub.cpp)
target_link_libraries(ember_stub PRIVATE
    ember ember_frontend
    ember_ext_vec ember_ext_quat ember_ext_mat
    ember_ext_string ember_ext_array ember_ext_math
    ember_ext_sync ember_ext_lifecycle ember_ext_map)
# Same native-registration + call shape as ember_cli's --load-em path.
```

The stub is a new `examples/ember_stub.cpp` (~80 lines, §4.3). It's the
minimal host: register bindings → find the appended `.em` → load → call
entry → exit code. No arg parsing beyond an optional `--fn` (to call a
non-`main`/non-`@entry` function from the embedded module).

---

## 4. The new runtime API the stub needs — `load_em_bytes`

### 4.1 The refactor: `load_em_file` already reads-then-parses; split it

`src/em_loader.cpp` line 544-585, `load_em_file_impl`:
```cpp
bool load_em_file_impl(const char* path, LoadedModule& out, std::string* err,
                       ModuleRegistry* registry,
                       const std::unordered_map<std::string, NativeSig>* natives,
                       const EmVerifyPolicy* verify) {
    // ... open the file, read it into a std::vector<uint8_t> file ...
    std::ifstream ifs(path, ...);
    std::vector<uint8_t> file(file_size);
    ifs.read(file.data(), file.size());
    // ... then parse from the buffer ...
    ParsedModule parsed;
    if (!parse_file(file, parsed, registry, natives, err)) return false;
    // ... signature verify + publish (stages the exec pages, patches relocs) ...
}
```

`parse_file(const std::vector<uint8_t>& file, ...)` at line 207 is the
buffer-based parser. The file-reading is a thin prefix. So a
`load_em_bytes` API is a trivial split:

```cpp
// NEW: load a .em from an in-memory byte buffer (the stub's path — the
// .em is appended to the stub's own exe, read into memory, loaded here).
// Same semantics as load_em_file: returns true + fills out on success,
// false + *err on failure. registry/natives/verify are the same optional
// args. This is the load_em_file_impl body AFTER the ifstream read —
// factored out so both the file path and the byte buffer share one impl.
bool load_em_bytes(const uint8_t* data, size_t len, LoadedModule& out,
                   std::string* err,
                   ModuleRegistry* registry = nullptr,
                   const std::unordered_map<std::string, NativeSig>* native_bindings = nullptr,
                   const EmVerifyPolicy* verify = nullptr);
```

The implementation:
```cpp
bool load_em_bytes(const uint8_t* data, size_t len, LoadedModule& out,
                   std::string* err, ModuleRegistry* registry,
                   const std::unordered_map<std::string, NativeSig>* natives,
                   const EmVerifyPolicy* verify) {
    if (!data || len < EM_HEADER_SIZE) { set_error(err, "em_loader: ..."); return false; }
    if (len > MAX_FILE_SIZE) { set_error(err, "em_loader: ..."); return false; }
    std::vector<uint8_t> file(data, data + len);   // copy into the vector parse_file expects
    // ... or: refactor parse_file to take (const uint8_t*, size_t) directly,
    //     avoiding the copy. The vector copy is O(n) but n is the .em size
    //     (small); the copy is negligible vs the exec page allocation. Keep
    //     the vector for now (parse_file's signature stays stable); optimize
    //     the copy away later if a profile says it matters.
    ParsedModule parsed;
    if (!parse_file(file, parsed, registry, natives, err)) return false;
    // ... the EXACT same signature-verify + publish tail as load_em_file_impl ...
}
```

Then `load_em_file_impl` becomes:
```cpp
bool load_em_file_impl(const char* path, ...) {
    // ... ifstream read into std::vector<uint8_t> file ...
    return load_em_bytes_impl(file.data(), file.size(), out, err, registry, natives, verify);
}
```

**This is a small, mechanical refactor.** The public `load_em_file` API
is unchanged (it still takes a path). The new `load_em_bytes` is additive
(default args preserve source compat). The stub calls `load_em_bytes`;
the CLI's `--load-em` path and all existing tests keep calling
`load_em_file`. The `em_loader.hpp` header gains the `load_em_bytes`
declaration (additive, with the same optional args as `load_em_file`).

### 4.2 The bundler holds the .em in memory (no temp file)

With `load_em_bytes` available, the bundler itself can also use it: the
`ember bundle` command compiles `.ember` → an in-memory `EmModule` →
serializes to an in-memory byte buffer (a small `write_em_bytes` companion
to `write_em_file`, or just `write_em_file` to a temp `.em` + read it
back). The simplest path: the bundler writes the `.em` to a temp file
(`write_em_file` already exists), reads it back into memory, appends it to
the stub copy, deletes the temp file. Or: add a `write_em_bytes(const
EmModule&, std::vector<uint8_t>&, std::string* err)` that serializes to a
buffer (the same refactor as the loader — `write_em_file`'s body writes
to an `ofstream`; split the serialize-to-buffer from the file-write). This
avoids the temp file entirely. **Recommend the `write_em_bytes` companion**
— it's the symmetric refactor to `load_em_bytes` and keeps bundling
temp-file-free.

### 4.3 The stub's main() — the full shape

```cpp
// examples/ember_stub.cpp - standalone ember runtime stub.
// The `ember bundle` command copies this exe to output.exe + appends a
// .em + a 12-byte footer. At runtime this main() reads its own file,
// finds the appended .em, loads it, and calls the entry.
//
// Links: ember + ember_frontend + the standard extension libs (the same
// native allowlist the CLI registers, so any CLI-compiled .em loads).
// Does NOT link ember_import (no source imports at runtime). Does NOT
// parse/sema/codegen (the .em is pre-compiled — load is memcpy + reloc patch).

#include "../src/engine.hpp"          // call_i64_i64
#include "../src/em_loader.hpp"       // load_em_bytes, LoadedModule
#include "../src/em_file.hpp"         // EM_MAGIC etc.
#include "ext_vec.hpp" ... // all standard extensions
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

// The appended-bundle footer magic ("EMBD" - distinct from EMBL/EMSG).
constexpr uint32_t EM_BUNDLE_MAGIC = 0x454D4244u;
constexpr uint32_t EM_BUNDLE_FOOTER_SIZE = 12u;  // u32 magic + u64 em_length

// Mirror the CLI's register_standard_bindings (same native allowlist).
static void register_standard_bindings(std::unordered_map<ember::NativeSig>& n) {
    ember::ext_vec::register_natives(n); ... // all extensions
    // + overloads published into the allowlist, same as the CLI
}

int main(int argc, char** argv) {
    // 1. Find this exe's own path (argv[0] is unreliable; GetModuleFileName on Windows).
    std::filesystem::path self = get_own_exe_path();  // GetModuleFileNameW / /proc/self/exe

    // 2. Read the whole file (or just the footer + the .em — read the footer
    //    first, then seek + read the .em; reading the whole file is simpler
    //    and the stub is small).
    std::ifstream f(self, std::ios::binary | std::ios::ate);
    uint64_t file_size = f.tellg();
    if (file_size < EM_BUNDLE_FOOTER_SIZE) { fprintf(stderr, "stub: no embedded module\n"); return 2; }
    // read the footer (last 12 bytes)
    f.seekg(file_size - EM_BUNDLE_FOOTER_SIZE);
    uint32_t magic = 0; uint64_t em_len = 0;
    f.read((char*)&magic, 4); f.read((char*)&em_len, 8);
    if (magic != EM_BUNDLE_MAGIC) { fprintf(stderr, "stub: no embedded module (bad footer magic)\n"); return 2; }
    if (em_len == 0 || file_size < EM_BUNDLE_FOOTER_SIZE + em_len) { fprintf(stderr, "stub: bad em_length\n"); return 2; }
    // read the .em (em_len bytes ending at file_size - FOOTER_SIZE)
    f.seekg(file_size - EM_BUNDLE_FOOTER_SIZE - em_len);
    std::vector<uint8_t> em_bytes(em_len);
    f.read((char*)em_bytes.data(), em_len);

    // 3. Register the native allowlist (same as CLI --load-em).
    std::unordered_map<std::string, ember::NativeSig> natives;
    register_standard_bindings(natives);

    // 4. Load the .em from memory.
    ember::LoadedModule mod; std::string lerr;
    if (!ember::load_em_bytes(em_bytes.data(), em_bytes.size(), mod, &lerr, nullptr, &natives)) {
        fprintf(stderr, "stub: load failed: %s\n", lerr.c_str()); return 2;
    }

    // 5. Find the entry (--fn overrides; else @entry slot; else "main").
    std::string fn_name = "main";
    if (argc >= 2 && std::string(argv[1]).substr(0,5) == "--fn") { /* parse --fn NAME */ }
    void* entry = mod.entry_by_name(fn_name.c_str()); if (!entry) entry = mod.entry();
    if (!entry) { fprintf(stderr, "stub: entry '%s' not found\n", fn_name.c_str()); return 2; }

    // 6. Call + exit code (the CLI --load-em contract: i64 return -> exit code; void -> 0).
    uint32_t slot = mod.entry_slot;
    for (auto& kv : mod.name_table) if (kv.first == fn_name) { slot = kv.second; break; }
    bool is_void = slot < mod.signatures_by_slot.size() && mod.signatures_by_slot[slot].ret.is_void();
    int64_t result = ember::call_i64_i64(entry);
    return is_void ? 0 : int(result);
}
```

**That's the whole stub.** ~80 lines. It's the CLI's `--load-em` block
with the `.em` source swapped from `load_em_file(path)` to
`load_em_bytes(embedded)`. No trap/checkpoint (the stub runs pre-compiled
trusted code — the bundler compiled it; if the host wants trap recovery,
it wraps the call in a `setjmp` + `context_t` like the CLI's run path —
§6.2).

---

## 5. The signed variant — tamper-resistant standalone exe

A standalone exe that runs an embedded `.em` is a code-injection vector if
someone can modify the appended `.em` (replace the bytes between the stub
PE and the footer, keep the footer valid). The `.em` v4 format already has
Ed25519 content authentication (`em_file.hpp` F2, `EM_SIG_MAGIC` /
`EM_SIG_BLOCK_SIZE`): `write_em_file_signed` emits a v4 module with a
signature block; `load_em_file` / `load_em_bytes` verifies the signature
**before `alloc_executable_rw`** (the loader takes an `EmVerifyPolicy`
with a keyring of trusted public keys).

**The signed-bundle flow:**
1. The bundler compiles `.ember` → `.em` with `write_em_file_signed` (the
   `--signed <keyfile>` flag: the keyfile holds the 32-byte Ed25519 seed
   or expanded private key; the bundler derives the keypair + signs).
2. The stub is built with a **compiled-in verification public key** (the
   matching public key, baked into `ember_stub.cpp` as a `constexpr
   uint8_t[32]`). The stub constructs an `EmVerifyPolicy{ {trusted_pub} }`
   (signed-only mode — the loader rejects unsigned modules + accepts a v4
   module only if its signature verifies against the trusted key).
3. At runtime, the stub calls `load_em_bytes(..., &verify)` with the
   policy. The loader verifies the Ed25519 signature over the `.em`
   content **before allocating any exec page** — a tampered `.em` (someone
   modified the appended bytes) is rejected with "signature verification
   failed," no code runs.

**The signing key stays off the stub** (the secure-boot model,
`em_file.hpp` KEY MANAGEMENT): the build tool that signs has the private
key; the stub has only the public key. A user who wants signed standalone
exes generates a keypair, builds the stub with their public key baked in,
and keeps the private key for the bundler. This is the F2 model applied to
the bundler.

**Recommendation: offer `--signed` as an opt-in flag.** The default
`ember bundle` produces an unsigned v4 stub (dev convenience — the stub
loads it in dev mode, no keyring). A user who distributes standalone exes
to untrusted environments uses `--signed` + a stub built with their
public key. The signed path is the tamper-resistant variant; the unsigned
path is the LAZY MODE default.

---

## 6. Design decisions and edge cases

### 6.1 Why not a Windows resource (RT_RCDATA) for the embed?

A Windows resource (`FindResource` + `LoadResource` + `LockResource`) is
the "native" way to embed data in a PE. It's clean (the OS loads it, no
file-read at runtime) but it requires the resource compiler (`rc.exe`) or
`UpdateResource` at bundle time to inject the `.em` into the stub's PE
resource section. This is a tool dependency the appended-section variant
avoids (the bundler is pure file I/O — `copy` + `append`). The
appended-section approach is also portable (works the same on POSIX, if
ember ever ships a Linux stub — the footer-find logic is OS-agnostic;
only `get_own_exe_path` differs). **Recommend appended-section for LAZY
MODE; note Windows-resource as a future "cleaner load, no self-read"
variant if a profile shows the self-read is a cost (it isn't — reading
one's own file once at startup is negligible).**

### 6.2 Trap recovery — does the stub wrap the call in a checkpoint?

The CLI's `run` path wraps the entry call in `__builtin_setjmp(ectx.checkpoint)`
+ a trap stub (`ember_cli_trap`) so a trap (bounds, budget, stack overflow)
longjmps back instead of crashing. The CLI's `--load-em` path does **not**
do this — it calls `call_i64_i64(entry)` directly (no context, no
checkpoint), treating the loaded `.em` as trusted pre-compiled code.

**The stub matches the `--load-em` path: no trap recovery by default.**
The bundled `.em` was compiled by the bundler (trusted source); a trap in
it is a bug in the script or the compiler, and a crash is an honest
signal. If a host wants trap recovery (e.g. a signed-bundle distributor
who wants graceful failure on a corrupted `.em` that passes signature but
traps at runtime), the stub wraps the call in a `context_t` +
`__builtin_setjmp` + a trap stub — the same pattern as the CLI's `run`
path. This is an opt-in complexity (the stub gains a `context_t` + a trap
stub + the `use_context_reg` compile flag, which the `.em` must have been
compiled with — the bundler would need to set `ctx.use_context_reg = true`
+ `ctx.trap_stub` at compile time, matching the CLI's `run` path). **For
LAZY MODE, the stub does no trap recovery (matches `--load-em`); the
trap-recovery stub is a future variant.**

### 6.3 Globals — the .em carries them, the loader patches them

The `.em` includes the globals block (`EmModule::globals`, the
initialized byte block). The loader `memcpy`s it into the allocated
globals block + patches the `GlobalsBase` relocations. The stub does
nothing globals-specific — `load_em_bytes` handles it (the same as
`load_em_file`). A script with `global cfg = Config{...};` works because
the bundler's compile step evaluated the initializer + baked the bytes
into the `.em` (the CLI's `emit-em` path does this: `mod.globals =
globals_bytes`).

### 6.4 Native bindings — the stub's allowlist must match the bundler's

The `.em` carries symbolic native bindings (`EmNativeBinding`: name +
signature, per `em_file.hpp`). The loader resolves each by name against
the stub's `unordered_map<string, NativeSig>` allowlist (`load_em_bytes`'s
`native_bindings` arg). A missing native is a load error ("unknown native
name -> rejected, no exec page" — the v5 security gate, but v4 also
rejects unknown names). **The stub must register every native the script
uses.** The stub registers all standard extensions (the same
`register_standard_bindings` as the CLI), so any script that compiles
with the CLI's standard bindings loads with the stub. If the script uses
OS IO natives (`plan_OS_IO_EXTENSIONS.md`), the stub must link
`ember_ext_io` + register `ext_io::register_all` AND the bundler must
have compiled with `--ffi` (so the natives are in the `.em`'s binding
table). A stub that doesn't register IO would fail to load an IO `.em`
with "unknown native `file_open`" — the right failure (the stub doesn't
have the capability).

### 6.5 Cross-module `link` — does the stub support it?

A `.em` with `link "foo.em" as foo;` directives has cross-module call
sites (kind-2 `ModuleRegistryBase` relocations). The loader patches these
with a supplied `ModuleRegistry` (`load_em_bytes`'s `registry` arg). The
stub would need to pre-load + register the linked `.em` modules. **For
LAZY MODE, the stub does not support `link`** (passes `registry = nullptr`);
a `.em` with a kind-2 reloc is a load error ("kind-2 reloc with no
registry"). A bundle is a single `.ember` → single `.em` (the bundler
compiles one file). If a script uses `link`, the bundler would need to
bundle multiple `.em`s (a multi-module bundle format — a future addition:
the footer could list N `.em`s + the stub loads + registers them all).
**Defer multi-module bundles; LAZY MODE is one script → one `.em` → one
exe.**

### 6.6 The stub's own exit code

The stub returns the entry function's i64 return as the process exit code
(clamped to 0-255 for POSIX), or 0 if the entry returns void — the same
contract as the CLI's `run` / `--load-em` paths. A load failure returns 2
(matching the CLI's parse/sema/load exit code). This makes a bundled exe
scriptable: `output.exe && echo ok` works, a test harness can check the
exit code.

---

## 7. Test matrix

### Tier 1 — bundler round-trip (the keystone)

1. **bundle + run.** `ember bundle hello.ember hello.exe` (where
   `hello.ember` is `fn main() -> i64 { return 42; }`) → `hello.exe`
   exists. Run `hello.exe` → exit code 42. Proves the full flow:
   compile → `.em` → append to stub → stub self-read → load → call →
   exit.
2. **bundle with natives.** A script using `array_new`/`array_push_i64`/
   `array_get_i64` → bundled → run → exit code reflects the array result.
   Proves the stub's native allowlist resolves the `.em`'s native bindings
   (the loader doesn't reject "unknown native `array_new`").
3. **bundle with globals.** `global cfg : i64 = 7; fn main() -> i64 {
   return cfg; }` → bundled → run → exit 7. Proves the globals block
   round-trips through the stub.
4. **bundle with string literal.** A script with an encrypted string
   literal (the default `0xA5` key) used in a native call → bundled → run
   → the rodata relocation patches correctly in the stub's load. Proves
   function-rodata relocations survive the append + self-read + load.
5. **bundle --fn.** `ember bundle lib.ember out.exe --fn compute` → the
   stub calls `compute` instead of `main`. Proves the `--fn` override
   flows through the bundler (stores the selected entry slot / name) to
   the stub.

### Tier 2 — footer robustness

6. **bad footer.** A plain `ember_stub.exe` (no `.em` appended) run
   directly → "no embedded module (bad footer magic)" + exit 2. Proves
   the stub detects the no-embed case cleanly.
7. **truncated .em.** Manually truncate the appended `.em` (keep the
   footer's `em_length` lying) → the stub reads `em_length` bytes but
   they're short → `load_em_bytes` rejects (the loader's header/size
   checks). Exit 2, no crash.
8. **footer magic mismatch.** Append bytes with a wrong footer magic →
   "no embedded module (bad footer magic)" + exit 2.

### Tier 3 — signed bundle (if `--signed` ships)

9. **signed bundle + valid stub key.** Generate a keypair; build the stub
   with the public key; `ember bundle script.ember out.exe --signed
   keyfile` → run `out.exe` → the stub verifies the signature → loads →
   runs → exit code correct.
10. **tampered .em.** Take the signed `out.exe`, flip one byte in the
    appended `.em` (not the footer) → run → the stub's
    `load_em_bytes(..., &verify)` rejects with "signature verification
    failed" → exit 2, **no exec page allocated** (the F2 property —
    verification before `alloc_executable_rw`). Proves the tamper-
    resistance.
11. **wrong key.** Build the stub with public key A; sign the bundle with
    private key B → run → "signed by an untrusted key" → exit 2. Proves
    the keyring distinguishes "wrong key" from "tampered content."

---

## 8. Summary of the architecture decision this plan presents

**Option A with an appended binary section is LAZY MODE.** The stub
(`ember_stub.exe`) is a pre-built runtime that links `ember` +
`ember_frontend` + the standard extensions (the same load path as the
CLI's `--load-em`, minus `ember_import`) and a `main()` that reads its own
file, finds a 12-byte footer (`EMBD` magic + `.em` length), loads the
appended `.em` via a new `load_em_bytes` API (a trivial refactor of
`load_em_file` — the parsing is already buffer-based in
`em_loader.cpp`'s `parse_file`), and calls the entry. The bundler (`ember
bundle input.ember output.exe`) compiles the `.ember` → `.em` (the
existing `emit-em` pipeline) + copies the stub to `output.exe` + appends
the `.em` + the footer. **No C++ compiler at bundle time. No temp files
at runtime. No whole-CLI duplication.** The stub is ~80 lines (the CLI's
`--load-em` block with `load_em_file(path)` swapped for
`load_em_bytes(embedded)`). Use v4 (raw x86) `.em` for LAZY MODE — the
stub links `em_loader` and does the memcpy+patch load; no IR re-emit. The
v5 IR format's portability advantage (target-agnostic `.em`, re-emitted by
the stub's `emit_x64`) doesn't help a same-platform bundle. The signed
variant (`--signed` + a stub built with a compiled-in public key) gives
tamper-resistant standalone exes via the existing v4 Ed25519 content
authentication (the loader verifies before `alloc_executable_rw`).**

The decision points the user makes from this plan:
1. **Option A (appended section) vs B (C++ linker) vs C (self-extracting)?**
   Recommend A (appended section) — LAZY MODE, no compiler at bundle time,
   no temp files, reuses the CLI's `--load-em` path verbatim.
2. **v4 (raw x86) vs v5 (IR) for the embedded .em?** Recommend v4 for LAZY
   MODE (simpler load, no re-emit; same-platform bundle doesn't need v5's
   portability). v5 is a future smaller-output option once v5-signed exists.
3. **Appended section vs Windows resource (RT_RCDATA)?** Recommend
   appended section (no `rc.exe`/`UpdateResource` at bundle time;
   portable; pure file I/O). Resource is a future cleaner-load variant.
4. **`load_em_bytes` API: refactor `load_em_file` to share the tail, or
   stub reads footer + writes temp + calls `load_em_file`?** Recommend the
   refactor (clean, no temp file; `parse_file` is already buffer-based so
   it's mechanical). The temp-file fallback is the no-refactor alternative.
5. **Signed bundle as default or opt-in?** Recommend opt-in (`--signed`
   flag + a stub built with the user's public key). Unsigned is the LAZY
   MODE default (dev convenience; the stub loads in dev mode).
6. **Trap recovery in the stub?** Recommend no (matches the CLI's
   `--load-em` path — pre-compiled trusted code, a crash is an honest
   signal). A trap-recovery stub is a future variant (wraps the call in a
   `context_t` + checkpoint, requires the bundler to compile with
   `use_context_reg` + a trap stub).
7. **Multi-module bundles (`link`)?** Defer — LAZY MODE is one script →
   one `.em` → one exe. A `.em` with kind-2 relocs is a load error in the
   stub (no registry). Multi-module bundles are a future footer extension.
