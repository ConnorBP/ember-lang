# Plan — OS IO extensions: file, console, process, path, directory

> **⚠ SHIPPED v1.0** — the `io` extension landed in `extensions/io/` (see
> `extensions/README.md` and `v1.0_INTEGRATION_NOTES.md`). The text below is
> the historical planning record, left unchanged.
>
> **Status: research / planning only.** This document reads the code
> firsthand (the extension pattern in `extensions/array/ext_array.cpp`,
> the binding API in `src/binding_builder.hpp`, the permission gate in
> `src/sema.cpp`, the `Family B` framing in `docs/ROADMAP.md`) and lays
> out the design. **No source is changed.** The user is making a scoping
> decision from this — be concrete about what these natives do, how they
> are gated, and how a host opts in per-capability.
>
> **This is the ROADMAP "Family B" entry, un-deferred.** The ROADMAP
> deferred Family B ("ember as a scripting language with real I/O") until
> "a demo or real use that is genuinely blocked on output beyond the exit
> code." This plan is the design that is ready when that trigger fires.
> It does not *fire* the trigger — it documents the surface so the
> decision to build it is informed.

---

## 0. The scope-honesty statement (read this first)

This is a **Tier-0-shaped extension** — exactly like the nine that already
ship (`extensions/{vec,quat,mat,string,array,math,map,sync,lifecycle}/`).
It is **not a language/grammar/sema change** (per
`extensions/README.md`'s "what an extension is — and is not" rule and
`ROADMAP.md` Tier 0's "not language features" framing). The pattern to
mirror, read firsthand:

- `extensions/array/ext_array.cpp` — the canonical i64-handle + host-vector +
  bounds-check pattern. `static std::vector<ArraySlot> g_arrays;` with
  `arr_slot(int64_t h)` doing `if (h<1 || h>size) return nullptr` (1-based
  handle, 0 = null/invalid). `extern "C" { static ... n_array_*(...) }`
  natives. `register_natives(map)` uses `BindingBuilder::add("name", ret,
  {params}, (void*)&fn)`. `reset()` clears the store. Public `get_bytes()`
  accessor for host-side natives that reach in by handle.
- `extensions/sync/ext_sync.hpp` — the modern header with a scope-honesty
  block, `register_natives`/`reset`, and `_host` accessors for host-side
  reach-in. No operator overloads (method-call natives, like `ext_array`).
- `src/binding_builder.hpp` — `BindingBuilder::add(name, ret, {params}, fn,
  permission=0)`. `PERM_FFI = 1u << 0` is the permission bit. `bind_prim(Prim)`
  / `bind_handle("struct")` are the convenience type builders.
- `docs/spec/BINDING_API.md` §"v0.3 working binding API" — the shipped API
  surface (the fluent `TypeBuilder`/`engine_t` is deferred design; this
  extension uses the working `BindingBuilder`).
- `docs/spec/SAFETY_AND_SANDBOX.md` §6 — the `PERM_FFI` enforcement model:
  compile-time, at the sema call-site check, zero runtime cost. A module
  that doesn't have the permission literally cannot produce code that calls
  the gated native.

**The one structural difference from `ext_array`/`ext_sync`: this extension
has a permission tier.** Most extensions register all their natives with
`permission = 0` (callable from any module). This extension registers the
I/O-capability natives with `permission = PERM_FFI` (callable only from a
module compiled with the FFI permission bit). The console-output natives
(`print`/`println`/`print_i64`) and the pure path-string natives
(`path_join`/`path_basename`/`path_dirname`) are ungated — they are output-
only or pure computation, with no filesystem/process reach. **Everything
that touches the filesystem, the process tree, or blocking input is
`PERM_FFI`-gated.**

**Stated precisely, the honest boundary:**

> These natives give an ember script real host I/O — file read/write,
> console I/O, directory listing, path manipulation, and subprocess
> execution. The filesystem/process/input natives are `PERM_FFI`-gated:
> a module compiled without the FFI permission bit cannot call them (sema
> rejects the call site at compile time, before codegen — zero runtime
> cost, no "check bypassed" path). The console-output and pure-path-string
> natives are ungated (output-only / pure computation, no security surface
> — matches `ROADMAP.md` Family B's "`print`/`println` ungated;
> `read_file`/`write_file` `PERM_FFI`-gated" framing). The extension
> provides **raw capability, not policy**: it does not sandbox paths, it
> does not restrict which commands `exec` can run, it does not impose a
> working-directory jail. The host that registers these natives is
> explicitly opting into raw I/O; sandboxing is a host wrapper
> (§7). The two layers of defense are: (1) the host chooses which
> sub-extensions to register at all (registration layer — a host that
> registers nothing has zero I/O surface), and (2) `PERM_FFI` gating means
> even a registered I/O native is only callable from a module the host
> granted the FFI permission (permission layer).

**Why this is the right cut:** ember is an embedded scripting language.
A game host embeds ember and registers exactly the natives it wants mods
to have — this is the whole extension model (`extensions/README.md` "How a
host registers an extension"). A host that does not want mods doing file
I/O simply does not call `register_file`/`register_dir`/`register_process`,
and those natives do not exist in the module's name resolution (a script
calling `file_open` gets "unknown function," not a permission error — the
strongest isolation). A host that wants mods to have file I/O but only for
trusted modules registers the natives AND grants those modules `PERM_FFI`
via the `module_permissions` arg to `sema()` (the permission layer). A host
that wants output-only scripting registers just `register_console` (print/
println, ungated) and nothing else. **The host is always in control of the
capability surface; this extension is the menu, not the mandate.**

---

## 1. The shape — one extension library, five sub-registration functions

This addon is one `extensions/io/` extension (one CMake target,
`ember_ext_io`, one TU) with **five granular registration entry points**
so a host opts in per-capability. This is the answer to the task's
"how to make these optional (the host chooses which I/O extensions to
register)" — the granularity is at the registration-function level, not
the library level (five separate libraries would add five CMake targets for
little benefit; the host already picks per-function-group here).

| Sub-registration | Natives | Permission | Coupling |
|---|---|---|---|
| `register_console(m)` | `print`, `println`, `print_i64`, `read_line`, `read_char` | print/println/print_i64 ungated; read_line/read_char `PERM_FFI` | links `ember_ext_array` (read_line_into fills an array); optional `ember_ext_string` for read_line-string variant |
| `register_file(m)` | `file_open`, `file_read`, `file_write`, `file_close`, `file_exists`, `file_size` | all `PERM_FFI` | links `ember_ext_array` (file_read/file_write go through array<u8> handles via `ext_array::get_bytes`) |
| `register_path(m)` | `path_join`, `path_basename`, `path_dirname`, `path_exists` | join/basename/dirname ungated (pure string ops); path_exists `PERM_FFI` (filesystem stat) | links `ember_ext_array` (result written into a caller-supplied array<u8>) |
| `register_dir(m)` | `dir_open`, `dir_next`, `dir_close`, `dir_create`, `dir_exists` | all `PERM_FFI` | links `ember_ext_string` (dir_next returns a string handle) |
| `register_process(m)` | `exec`, `proc_status`, `proc_wait`, `spawn`, `spawn_detach` | all `PERM_FFI` | links `ember_ext_array` (exec captures stdout into an array<u8>) |

Plus a convenience `register_all(m)` that calls all five (for hosts that
want the full surface, e.g. the standalone CLI in `--ffi` mode).

**Why `ember_ext_array` is a dependency.** The established convention for
a native that produces or consumes a byte buffer is the `ext_array` +
`get_bytes` pattern — `extensions/README.md` "Accessors a host may need"
documents `ext_array::get_bytes(handle, &ptr, &len)` as the reach-in for
host natives that fill an `array<u8>` handle (prism's `n_read_bulk` uses
exactly this). `file_read(file_h, dst_array_h, max_bytes)` calls
`ext_array::get_bytes(dst_array_h, &ptr, &len)` to get the backing store
and reads file bytes into it. This is the decoupled byte-buffer idiom; it
does not couple to `ext_string` (the script holds an `array<u8>` handle,
not a string handle, for raw bytes).

**Why `ember_ext_string` is a dependency for `dir_next`/the `read_line`
string variant.** A directory entry name is text, and the natural return
type is a persistent, owned string handle (not a raw slice, which would
dangle — §3.4). `dir_next` calls `ext_string::alloc(std::string)` to
allocate the handle. This couples `register_dir` (and the optional
`read_line`-string variant) to `ember_ext_string`. The host that registers
`register_dir` also registers `ext_string::register_natives` (the CLI
already does). This is a reasonable dependency — `string` is a core
extension, and the coupling is one-directional (`ext_io` calls
`ext_string::alloc`; `ext_string` does not know about `ext_io`).

**Handle types (the type-system tags).** Each handle-returning native
group gets its own `bind_handle` tag so the script (and sema) can tell the
handles apart, exactly as `array`/`string`/`sync`'s primitives are distinct
today:

| Handle | `bind_handle` tag | backing host type |
|---|---|---|
| file descriptor | `"file"` | `struct FileSlot { FILE* fp; int mode; bool valid; };` |
| directory iterator | `"dir"` | `struct DirSlot { void* handle; bool valid; };` (`DIR*` on POSIX, `HANDLE` + `WIN32_FIND_DATA` on Windows) |
| subprocess | `"proc"` | `struct ProcSlot { void* handle; int pid; bool finished; int exit_code; };` |

The script sees three distinct opaque handle types; sema gets them as `i64`
with a struct tag (the same way `array` vs `string` are both `i64`
underneath but tagged). No operator overloads (none of these have a `+` /
`==` that means anything); `register_overloads` is not implemented, mirroring
`ext_array`/`ext_sync`.

---

## 2. CONSOLE I/O — `print`, `println`, `print_i64`, `read_line`, `read_char`

### 2.1 The split: output ungated, input PERM_FFI

`ROADMAP.md` Family B: "`print`/`println` ungated; `read_file`/`write_file`
`PERM_FFI`-gated." The same logic applies to console: output is one-way
(the script writes to stdout; no host state is read, no security surface).
Input is a host capability — `read_line` reads from stdin (can block, can
read sensitive input), so it is `PERM_FFI`-gated. `print_i64` is a
convenience ungated native (so a script can print a number without
converting to string first); it is pure output.

### 2.2 API

```
// --- output (ungated) ---
print(s: slice<u8>)                      // write bytes to stdout, no newline
println(s: slice<u8>)                    // write bytes + '\n' to stdout
print_i64(v: i64)                        // write decimal integer to stdout

// --- input (PERM_FFI) ---
read_line(dst: array<u8>) -> i64         // read a line into dst, return byte length; -1 = EOF/error
read_char() -> i64                       // one byte (0-255), -1 = EOF/error
```

**Why `read_line` takes an `array<u8>` dst rather than returning a string.**
Two reasons:
1. **Decoupling.** The array-into variant links only `ember_ext_array`
   (already a console-io dependency for consistency). A string-returning
   `read_line() -> string` variant is offered as an *optional* addition
   (§2.3) that couples to `ember_ext_string`; a host that wants the
   decoupled surface uses the array-into form.
2. **No slice-lifetime hazard.** A native returning a `slice<u8>` would
   hand back a `{ptr, len}` pointing into host memory whose lifetime the
   script cannot manage (the slice-escape safety work, `ROADMAP.md`
   "Slice-of-stack-local escape safety," documents this as the C3/C5
   residual hole). Returning into a caller-supplied `array<u8>` handle
   makes the script own the buffer — the handle is persistent, the bytes
   are in the array's host-heap store, no dangling.

`read_line(dst)`: reads one line (up to and including `\n`, stripped) from
stdin into `dst` via `ext_array::get_bytes` + `resize`. Returns the byte
length, or -1 on EOF/error. The array is resized to fit the line (capped
at a sane maximum, e.g. 1 MiB — §6.1). `read_char()`: reads one byte from
stdin (a blocking `getchar`-equivalent), returns 0-255 or -1 on EOF.

### 2.3 Optional: `read_line_str() -> string` (couples to ext_string)

For hosts that register `ember_ext_string` and want the ergonomic form:
```
read_line_str() -> string                // read a line, return a string handle; empty string on EOF
```
This calls `ext_string::alloc(std::string)` with the line content. It is
gated `PERM_FFI` (input). A host that does not link `ember_ext_string`
does not register this native. **Recommendation: ship the array-into
`read_line` as the canonical form (decoupled); offer `read_line_str` as
an opt-in native the host registers only if it also registers `ext_string`.**
This keeps the console-io dependency surface minimal by default.

### 2.4 Host impl

```cpp
// ungated output natives
static void n_print(uint8_t* p, int64_t len) noexcept {
    if (p && len > 0) std::fwrite(p, 1, size_t(len), stdout);
}
static void n_println(uint8_t* p, int64_t len) noexcept {
    if (p && len > 0) std::fwrite(p, 1, size_t(len), stdout);
    std::fputc('\n', stdout);
}
static void n_print_i64(int64_t v) noexcept {
    char buf[32]; int n = std::snprintf(buf, sizeof(buf), "%lld", (long long)v);
    if (n > 0) std::fwrite(buf, 1, size_t(n), stdout);
}
// PERM_FFI input natives
static int64_t n_read_line(int64_t dst_h) noexcept;   // via ext_array::get_bytes + fgets
static int64_t n_read_char() noexcept;                // getchar()
```

`n_print`/`n_println` take the slice as `(uint8_t* p, int64_t len)` — the
two-consecutive-slot slice ABI (`BINDING_API.md` §4: `slice<T>` → `T*,
int64_t`). The slice bytes are read synchronously and written to stdout;
no escape (the native does not retain the pointer past the call). This is
the "synchronous use is fine" case for stack-backed slices (the encrypted
`StringLit` temp is live for the call's duration).

---

## 3. FILE I/O — `file_open`, `file_read`, `file_write`, `file_close`, `file_exists`, `file_size`

### 3.1 API (all PERM_FFI; opaque i64 handle tagged `"file"`)

```
file_open(path: slice<u8>, mode: i64) -> file       // mode: 0=read, 1=write(truncate), 2=append, 3=r+w; 0 = failed
file_read(file: file, dst: array<u8>, max_bytes: i64) -> i64  // bytes read; -1 = error
file_write(file: file, src: array<u8>, len: i64) -> i64       // bytes written; -1 = error
file_close(file: file)
file_exists(path: slice<u8>) -> i64                 // 1/0
file_size(path: slice<u8>) -> i64                   // bytes; -1 = no such file
```

**The path is a `slice<u8>`, not a string handle.** A script writes
`file_open("data.txt", 0)` — the string literal evaluates to a stack-backed
`slice<u8>` (the inline-XOR `StringLit` path). The native receives
`(uint8_t* ptr, int64_t len)`, copies the bytes into a host-local
`std::string` + null-terminates, and calls `fopen`/`open` with the C
string. The copy is immediate and synchronous (no slice escape). This is
the decoupled convention — no `ext_string` dependency for file paths. A
script holding a `string` handle for a path converts via
`string_to_slice` (if that native exists) or constructs the path as a
literal.

**`mode` is an i64, not an enum.** ember v1 enums are `i32`-typed values
(rewrite to `IntLit` at sema); the script can define its own `enum
FileMode { Read = 0, Write = 1, Append = 2, ReadWrite = 3 }` and pass
`FileMode::Read` — sema rewrites it to `0`. The native just switches on
the integer. This keeps file I/O decoupled from any host-side enum
builder (there is no `EnumBuilder` — `BINDING_API.md` §5 documents this).

### 3.2 Host impl

```cpp
struct FileSlot {
    FILE* fp = nullptr;        // std::FILE* (portable); or std::fstream
    int mode = 0;
    bool valid = false;
};
static std::vector<FileSlot> g_files;   // 1-based handles, mirror ext_array
static constexpr size_t MAX_READ_WRITE = size_t(1) << 30;  // 1 GiB cap per op

static int64_t n_file_open(uint8_t* p, int64_t len, int64_t mode) noexcept {
    if (!p || len <= 0 || len > MAX_PATH_LEN) return 0;
    std::string path((const char*)p, size_t(len));   // copy + null-terminate
    const char* m = (mode == 1) ? "wb" : (mode == 2) ? "ab" : (mode == 3) ? "r+b" : "rb";
    FILE* fp = std::fopen(path.c_str(), m);
    if (!fp) return 0;
    g_files.push_back({fp, int(mode), true});
    return int64_t(g_files.size());   // 1-based handle
}
static FileSlot* file_slot(int64_t h) {
    if (h < 1 || h > int64_t(g_files.size())) return nullptr;
    return &g_files[size_t(h - 1)];
}
static int64_t n_file_read(int64_t h, int64_t dst_h, int64_t maxb) noexcept {
    auto* s = file_slot(h); if (!s || !s->valid || maxb < 0) return -1;
    if (maxb > int64_t(MAX_READ_WRITE)) maxb = int64_t(MAX_READ_WRITE);
    uint8_t* dst = nullptr; int64_t dst_cap = 0;
    if (!ext_array::get_bytes(dst_h, &dst, &dst_cap)) return -1;   // bad array handle
    // resize the array to maxb (or dst_cap, whichever is smaller) via ext_array
    size_t to_read = size_t(std::min(maxb, dst_cap));
    size_t n = std::fread(dst, 1, to_read, s->fp);
    // (the array's logical length is updated by the host — see §3.3)
    return int64_t(n);
}
```

**The array-resize interaction.** `ext_array::get_bytes` returns the
backing pointer + current length. `file_read` needs the array sized to
`max_bytes` *before* reading into it. Two options:
- **(a) The script resizes first:** `array_resize(dst, max_bytes);
  file_read(f, dst, max_bytes);` — the script is explicit about the buffer
  size. `file_read` reads into the existing backing store, returns bytes
  read; the script then `array_resize(dst, bytes_read)` to trim. Clean,
  no new array API. **Recommend this** — it mirrors how prism's
  `read_bulk` works (the caller sizes the array).
- **(b) `file_read` resizes internally:** the native calls a new
  `ext_array::resize(handle, n)` host accessor. Adds a host-side accessor
  to `ext_array`. Slightly more coupling.

**Recommendation: (a) — the script sizes the array.** This keeps
`ext_array`'s accessor surface unchanged (`get_bytes` only) and matches
the existing `read_bulk` convention. The script pattern is:
```ember
let buf = array_new(1, 0);          // elem_size=1 (u8), count=0
array_resize(buf, 4096);            // make room
let n = file_read(f, buf, 4096);    // read up to 4096 bytes
array_resize(buf, n);               // trim to actual
// process buf[0..n)
```

### 3.3 `file_write` — the symmetric path

`file_write(f, src, len)`: calls `ext_array::get_bytes(src, &ptr, &cap)`,
writes `min(len, cap)` bytes from `ptr` to the file via `fwrite`, returns
bytes written. The script passes the array it filled and the length it
wants to write. No new array API.

### 3.4 Why no native returns a `slice<u8>` for file contents

A native returning `slice<u8>` would hand back `{ptr, len}` pointing into
host memory (a `FileSlot`-owned buffer or a static buffer). The slice's
lifetime is the native's contract (`BINDING_API.md` §6: "the returned
`{ptr,len}` pair's lifetime is entirely the native function's contract to
the caller — ember does not track or validate it"). For file contents,
there is no obvious ownership: the bytes would live in a host buffer the
script cannot free, and a second `file_read` would overwrite the first's
buffer. The read-into-array pattern (§3.2) makes the script own the buffer
via a persistent `array<u8>` handle — no dangling, no overwrite-surprise.
This is the same reason `ext_array` exists at all (owned, persistent byte
storage) rather than a "native returns a slice" model.

### 3.5 `file_exists` / `file_size` — path-taking, no handle

```
file_exists(path: slice<u8>) -> i64     // 1 if the path exists and is a regular file, 0 otherwise
file_size(path: slice<u8>) -> i64       // file size in bytes; -1 if no such file
```

These take a path slice, do a `stat`/`GetFileAttributesEx`, and return.
No handle, no state. `PERM_FFI` (filesystem access — even a stat leaks
filesystem structure). The host-local path copy is the same as
`file_open`'s.

---

## 4. PATH I/O — `path_join`, `path_basename`, `path_dirname`, `path_exists`

### 4.1 The split: pure string ops ungated, `path_exists` PERM_FFI

`path_join`/`path_basename`/`path_dirname` are **pure string manipulation** —
they operate on the path bytes as text (split on `/` and `\`, join with the
platform separator). No filesystem access. They are ungated (callable from
any module) — they are no more a security surface than `string_concat`.
`path_exists` does a filesystem `stat` — it is `PERM_FFI` (it leaks
filesystem structure, same as `file_exists`).

### 4.2 API

```
// --- pure string ops (ungated) ---
path_join(a: slice<u8>, b: slice<u8>, dst: array<u8>) -> i64     // join a + b with platform sep; write to dst; return length
path_basename(path: slice<u8>, dst: array<u8>) -> i64            // last component; write to dst; return length
path_dirname(path: slice<u8>, dst: array<u8>) -> i64             // directory component; write to dst; return length

// --- filesystem stat (PERM_FFI) ---
path_exists(path: slice<u8>) -> i64                             // 1 if path exists (file or dir), 0 otherwise
```

**The result is written into a caller-supplied `array<u8>`** (the same
decoupled pattern as `read_line`/`file_read`). The script creates an
`array<u8>`, passes it, and the native writes the result bytes + returns
the length. The script then reads the bytes or converts to a string. This
keeps `register_path` dependent only on `ember_ext_array` (not
`ember_ext_string`).

**Optional string-returning variants** (couple to `ember_ext_string`):
```
path_join_str(a: slice<u8>, b: slice<u8>) -> string       // if ext_string registered
path_basename_str(path: slice<u8>) -> string
path_dirname_str(path: slice<u8>) -> string
```
Same recommendation as `read_line`/`read_line_str`: ship the array-into
form as canonical (decoupled); offer the string-return form as opt-in.

### 4.3 Host impl

The pure ops use `std::filesystem::path` (`path(a) / path(b)`,
`.filename()`, `.parent_path()`) — portable, handles `/` and `\` on
Windows. The bytes are copied into the dst array via
`ext_array::get_bytes`. The platform separator is `std::filesystem::path::
preferred_separator` (so `path_join` produces `\`-separated paths on
Windows, `/` on POSIX — matching the host OS convention).

---

## 5. DIRECTORY I/O — `dir_open`, `dir_next`, `dir_close`, `dir_create`, `dir_exists`

### 5.1 API (all PERM_FFI; opaque i64 handle tagged `"dir"`)

```
dir_open(path: slice<u8>) -> dir                    // 0 = failed; iterator handle
dir_next(dir: dir) -> string                        // next entry name; empty string at end-of-list
dir_close(dir: dir)
dir_create(path: slice<u8>) -> i64                  // 1 = created, 0 = exists/error
dir_exists(path: slice<u8>) -> i64                  // 1/0
```

**`dir_next` returns a `string` handle** (couples to `ember_ext_string`).
Directory entry names are text, and the natural return is a persistent,
owned string. `dir_next` calls `ext_string::alloc(entry_name)`. An empty
string signals end-of-list (the script loops `while ((name = dir_next(d))
...)` — but it must distinguish "empty entry name" from "end"; a directory
can legitimately contain a file named `""`? No — filesystems do not allow
empty names. So empty string = end-of-list is safe. Document it loudly.)

**Alternative (decoupled): `dir_next_into(dir, dst: array<u8>) -> i64`**
returns the entry length into a caller array, -1 at end-of-list. This
couples to `ember_ext_array` only. Offer both; recommend the string
variant for `dir_next` (entry names are text, the string handle is the
natural type) and note the array-into variant as the decoupled option.

### 5.2 Host impl

```cpp
struct DirSlot {
#ifdef _WIN32
    HANDLE find_handle = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAW find_data;
    bool first_pending = true;
#else
    DIR* dp = nullptr;
#endif
    bool valid = false;
};
static std::vector<DirSlot> g_dirs;
```

`dir_open`: opens the directory (`opendir` on POSIX, `FindFirstFile` on
Windows with a `path/*` glob). `dir_next`: reads the next entry
(`readdir` / `FindNextFile`), skips `.` and `..`, converts the name to
UTF-8, allocates a string handle via `ext_string::alloc`. `dir_close`:
closes the handle (`closedir` / `FindClose`), marks the slot dead.
`dir_create`: `std::filesystem::create_directory`. `dir_exists`:
`std::filesystem::is_directory`.

**The Windows wide-char → UTF-8 conversion** is a host-side concern
(`WideCharToMultiByte`); the native returns UTF-8 bytes in the string
handle. Document that entry names are UTF-8 (matches ember's slice<u8>
string convention).

---

## 6. PROCESS I/O — `exec`, `proc_status`, `proc_wait`, `spawn`, `spawn_detach`

### 6.1 The highest-risk capability — all PERM_FFI

`exec`/`spawn` run a host command. This is the most dangerous capability
in the extension (arbitrary command execution). `PERM_FFI` is the gate; a
host that registers `register_process` is explicitly opting into command
execution from scripts. **Document the risk loudly in the header.** A
sandboxed host does not register this sub-extension at all.

### 6.2 `exec` — synchronous, capture stdout

```
exec(cmd: slice<u8>, stdout_dst: array<u8>) -> i64   // run cmd, capture stdout into dst, return exit code; -1 = spawn failed
```

`exec` runs a command synchronously and captures its stdout into the
caller-supplied `array<u8>` (via `ext_array::get_bytes`). It uses
`_popen` (Windows) / `popen` (POSIX) to run the command with captured
stdout, reads the output into the array (resizing via the script-sizing
convention — the script pre-sizes the array, or `exec` caps at the array
capacity), closes the pipe via `_pclose`/`pclose` (which returns the exit
code), and returns the exit code. `-1` means the spawn itself failed
(`popen` returned null).

**The command is a `slice<u8>`** (the same path-copy convention as
`file_open`). The native copies the bytes into a host-local null-terminated
`std::string` and passes it to `popen`. On Windows, `_popen` runs the
command via `cmd.exe /c`; on POSIX, `popen` runs via `/bin/sh -c`. This is
the standard popen contract — the command is a shell command string, not
an argv array. **Document this:** `exec("ls -la", buf)` runs `ls -la`
through the shell. A host that wants argv-based execution (no shell) wraps
this or uses `spawn` (§6.4).

**Buffer sizing.** The script pre-sizes `stdout_dst` (e.g.
`array_resize(buf, 65536)`); `exec` reads up to the array capacity. If the
output exceeds the buffer, `exec` reads only what fits (the rest is lost —
documented; a host that wants unlimited capture wraps `exec` with a
growing-buffer loop, or uses `spawn` + `proc_wait` + a pipe). For v1, the
fixed-buffer `exec` is the simple, correct, useful primitive. A
`exec_to_string() -> string` variant (captures all output into a string
handle, unbounded up to a cap) is a future addition if the fixed-buffer
form is too limiting.

### 6.3 `proc_status` / `proc_wait` — for `spawn` completion

```
proc_status(proc: proc) -> i64      // 0 = still running, 1 = finished (exit code cached); -1 = bad handle
proc_wait(proc: proc) -> i64        // block until finished, return exit code; -1 = bad handle
```

These operate on a `proc` handle from `spawn`. `proc_status` is a
non-blocking poll (`WaitForSingleObject(h, 0)` / `waitpid(pid, &status,
WNOHANG)`); `proc_wait` is a blocking wait (`WaitForSingleObject(h,
INFINITE)` / `waitpid(pid, &status, 0)`).

### 6.4 `spawn` / `spawn_detach` — async subprocess

```
spawn(cmd: slice<u8>) -> proc                   // start cmd async, return handle; 0 = failed
spawn_detach(cmd: slice<u8>) -> i64             // start cmd detached (fire-and-forget), 1/0
```

`spawn` starts a command asynchronously (no stdout capture — detached
stdout goes to the parent's stdout or /dev/null) and returns a `proc`
handle. The script polls with `proc_status` or blocks with `proc_wait`.
`spawn_detach` is fire-and-forget (start the process, don't track it,
return 1/0) — for "launch an editor" / "open a URL" style commands where
the script doesn't care about the exit code.

**Host impl:** `CreateProcess` (Windows) / `fork` + `execvp` (POSIX).
The `proc` handle stores the process handle / pid. This is a real
subsystem (~100 lines per platform) but well-understood. **For v1 LAZY
MODE, `exec` (synchronous popen) is the priority** — it covers the common
"run a command, get its output" case (data processing, code-gen, build
helpers — the ROADMAP's "arbitrary CLI tools" framing). `spawn`/
`proc_status`/`proc_wait` are the async refinement for scripts that need
non-blocking subprocess management. A host that only wants synchronous
`exec` registers only `register_process`'s `exec` subset (or the plan
splits `register_process` into `register_exec` + `register_spawn` — see
§6.5).

### 6.5 Optional split: `register_exec` vs `register_spawn`

If a host wants `exec` but not `spawn` (the common case — `exec` is the
useful one for scripting; `spawn` is the risky async one), split
`register_process` into:
- `register_exec(m)` — `exec` only (synchronous, stdout-captured)
- `register_spawn(m)` — `spawn`, `spawn_detach`, `proc_status`, `proc_wait`

A host calls `register_exec` for the safe synchronous subset and avoids
`register_spawn` if it doesn't want async subprocess tracking. **Recommend
this split** — it gives the host finer control over the riskiest
capability (a host that wants "run a command and get output" but not
"launch background processes" gets exactly that).

---

## 7. Security model — the two layers of defense, and what the extension does NOT do

### 7.1 Layer 1: registration (the host chooses which natives exist at all)

The host calls whichever sub-registration functions it wants. A host that
registers only `register_console` (print/println, ungated) has **no file,
directory, path-stat, or process surface at all** — a script calling
`file_open` gets "unknown function" from sema (the native is not in the
table). This is the strongest isolation: the capability does not exist in
the module's name resolution. A sandboxed game host embeds ember and
registers zero I/O; a trusted-tool host (the standalone CLI in `--ffi`
mode) registers all five.

### 7.2 Layer 2: permission (`PERM_FFI` gating, compile-time)

Even when a host registers an I/O native, a module must be compiled with
the FFI permission bit to call it. `sema()` takes
`module_permissions` as its 4th arg (`src/sema.hpp` line 115); the CLI
currently passes `0` (no FFI). The sema call-site check
(`src/sema.cpp` line 1023-1027):
```cpp
if ((nit->second.permission & PERM_FFI) && !(perms & PERM_FFI)) {
    err("function '" + c->name + "' requires PERM_FFI permission", ...);
}
```
rejects the call at compile time — zero runtime cost, no "check bypassed"
path. So the host grants `PERM_FFI` only to modules it trusts with I/O.
A host could compile two modules from the same source with different
permissions (one with `PERM_FFI`, one without) and the without-permission
one literally cannot produce code that calls `file_open`. This is the
model `SAFETY_AND_SANDBOX.md` §6 describes.

### 7.3 What the extension does NOT do (host policy, not extension policy)

- **No path sandboxing.** `file_open` opens any path the script supplies.
  The extension does not restrict paths to a working-directory jail. A
  host that wants sandboxing wraps the natives (registers its own
  `file_open` that checks the path against an allowlist before delegating)
  or configures the process's working directory / filesystem permissions.
  Document this: **the extension is raw capability; the host sets policy.**
- **No command allowlist for `exec`.** `exec` runs any command. A host
  that wants an allowlist wraps `exec` or doesn't register
  `register_process`. Document the risk.
- **No rate limiting.** A script can call `file_read` in a tight loop;
  the instruction budget (`SAFETY_AND_SANDBOX.md` §3) bounds the *ember
  instruction* count, but a native call is one instruction that does a
  lot of host work. A host that wants to bound I/O does so at the host
  wrapper level. Document.
- **No blocking-native timeout.** `read_line`, `exec`, `proc_wait` block
  the script thread until the operation completes. The trap/checkpoint
  machinery (`context_t` checkpoint + the trap stub) does not fire during
  a native block (the `longjmp` is set up in `ember_call`, and a native
  block doesn't hit a checkpoint). This is safe (the script thread blocks,
  not the host), but a script that blocks in a `@on_tick` stalls the tick.
  Document: **blocking natives are safe but stall the calling context;
  a host that needs timeout wraps the native or runs the script on a
  separate `context_t` it can cancel.**

### 7.4 Backing-store-isolation guard compliance

Mirrors `ext_array`/`ext_sync` verbatim (`plan_SYNC_QUEUES.md` §7):
1. **All stores are `std::vector<Slot>` on the host heap** — `FileSlot`,
   `DirSlot`, `ProcSlot` hold `FILE*`/`DIR*`/`HANDLE` + metadata, all in
   host-heap `std::vector`s. None are ever allocated from
   `jit_memory.cpp`'s `alloc_executable`. No IO native touches a
   `PAGE_EXECUTE_READWRITE` page.
2. **Handles are 1-based small integers, not pointers** — a script holding
   a `file` handle holds, e.g., `3`, not a `FILE*`. No native returns a
   raw pointer as an i64.
3. **Bounds checks on every handle-taking native** — `file_slot(h)` /
   `dir_slot(h)` / `proc_slot(h)` do `if (h < 1 || h > size) return
   nullptr`, mirroring `arr_slot`. Out-of-range handles are silent no-ops
   or error returns (-1), never a wild pointer.
4. **No native writes to a passed address** — `file_read` writes into an
   `array<u8>` handle's backing store (via `ext_array::get_bytes`, which
   is a bounds-checked 1-based index reach-in), not through a
   script-supplied raw pointer. The V5 finding ("no native writes to a
   passed address") is preserved.
5. **The one pointer accessor** — `ext_array::get_bytes` — returns a
   `uint8_t*` to the host-heap `std::vector`'s storage, to a *host C++
   caller* (the IO native), not to the script. The script cannot forge
   or supply a pointer to it (the handle is a bounds-checked index).

**Net:** the addon is backing-store-isolation-guard compliant by
construction — structurally identical to `ext_array`/`ext_sync` (host
`std::vector` of slots, 1-based handles, bounds-checked natives,
host-callable pointer accessors that never go through the script ABI).

---

## 8. File layout — `extensions/io/`

Mirrors `extensions/sync/` exactly (one TU, one header, one CMake line +
extra link deps).

```
ember/extensions/io/
├── ext_io.hpp        # public API: register_console/file/path/dir/process/exec/spawn, register_all, reset
└── ext_io.cpp        # all natives' host impl + BindingBuilder registration (one TU)
```

### `ext_io.hpp` (public surface — mirrors `ext_sync.hpp`)

```cpp
// ext_io.hpp - ember extension: OS I/O (file, console, process, path, directory).
// See docs/planning/plan_OS_IO_EXTENSIONS.md.
//
// An ember *extension* (see ember/extensions/README.md): reusable,
// non-cheat-specific. Host-owned storage behind opaque i64 handles
// (file/dir/proc); console/path natives are stateless or use caller arrays.
// reset() clears the handle stores. Mirrors ext_array/ext_sync's shape
// (1-based handle, slot(h) bounds check, register_*/reset, public accessor
// for host-side reach-in).
//
// === SCOPE (docs/planning/plan_OS_IO_EXTENSIONS.md §0/§7 -- read first) ===
//
// These natives give an ember script real host I/O. The filesystem/process/
// input natives are PERM_FFI-gated: a module compiled without the FFI
// permission bit cannot call them (sema rejects at compile time, zero
// runtime cost -- see SAFETY_AND_SANDBOX.md §6). The console-output
// (print/println/print_i64) and pure-path-string (path_join/basename/dirname)
// natives are ungated (output-only / pure computation, no security surface).
//
// TWO LAYERS OF DEFENSE:
//   1. REGISTRATION: the host chooses which sub-extensions to register at
//      all. A host that registers only register_console has NO file/dir/
//      process surface -- a script calling file_open gets "unknown function."
//   2. PERMISSION: even when registered, PERM_FFI gating means a module
//      must be compiled with the FFI bit to call filesystem/process/input
//      natives. The host grants PERM_FFI only to modules it trusts with I/O.
//
// The extension provides RAW CAPABILITY, NOT POLICY. It does not sandbox
// paths, does not allowlist exec commands, does not rate-limit. A host
// that wants policy wraps the natives or configures the process environment.
// See §7 of the plan.
//
// BACKING-STORE ISOLATION (REDSHELL guard #8): all handle stores are
// host-heap vectors of slots (FILE*/DIR*/HANDLE + metadata), NEVER
// co-located with exec JIT memory or the dispatch table. Handles are
// 1-based indices, never pointers. Bounds checks on every handle native.
// Mirrors ext_array/ext_sync's posture verbatim.
#pragma once
#include "sema.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>

namespace ember::ext_io {

// --- granular registration (the host opts in per-capability) ---
// Each registers its natives into m with the appropriate permission bits
// (PERM_FFI on filesystem/process/input; ungated on console-output/pure-path).

// Console: print/println/print_i64 (ungated) + read_line/read_char (PERM_FFI).
void register_console(std::unordered_map<std::string, NativeSig>& m);
// File: file_open/read/write/close/exists/size (all PERM_FFI). Links ext_array.
void register_file(std::unordered_map<std::string, NativeSig>& m);
// Path: path_join/basename/dirname (ungated) + path_exists (PERM_FFI). Links ext_array.
void register_path(std::unordered_map<std::string, NativeSig>& m);
// Directory: dir_open/next/close/create/exists (all PERM_FFI). Links ext_string.
void register_dir(std::unordered_map<std::string, NativeSig>& m);
// Process: exec (PERM_FFI). Links ext_array. The synchronous stdout-capture subset.
void register_exec(std::unordered_map<std::string, NativeSig>& m);
// Process async: spawn/spawn_detach/proc_status/proc_wait (all PERM_FFI).
void register_spawn(std::unordered_map<std::string, NativeSig>& m);
// Convenience: register_console + file + path + dir + exec + spawn.
void register_all(std::unordered_map<std::string, NativeSig>& m);

// Clear all handle stores (file/dir/proc). Mirrors ext_array/ext_sync reset.
// Console/path natives are stateless (no store to clear).
void reset();

} // namespace ember::ext_io
```

### `ext_io.cpp` (impl)

One namespace `ember::ext_io`. Layout, in order:
1. Includes: `ext_io.hpp`, `ast.hpp`, `binding_builder.hpp`,
   `../array/ext_array.hpp` (for `get_bytes`), `../string/ext_string.hpp`
   (for `alloc`, used by `dir_next`), `<cstdio>`, `<filesystem>`,
   `<string>`, `<vector>`, `<cstring>`.
2. `struct FileSlot`/`DirSlot`/`ProcSlot` definitions.
3. `static std::vector<FileSlot> g_files;` / `g_dirs` / `g_procs` +
   `file_slot(h)`/`dir_slot(h)`/`proc_slot(h)` bounds-check helpers.
4. `extern "C" { static ... n_* (...) noexcept }` blocks — one per group
   (console, file, path, dir, process), each containing that group's
   natives. All `noexcept` (mirrors `ext_array`).
5. `register_console(map)` — one `BindingBuilder` with print/println/
   print_i64 (ungated) + read_line/read_char (`PERM_FFI`), `build()`,
   loop-insert into `m`.
6. `register_file(map)` / `register_path(map)` / `register_dir(map)` /
   `register_exec(map)` / `register_spawn(map)` — same shape, each with
   its natives at the right permission.
7. `register_all(map)` — calls all six.
8. `reset()` — clears `g_files`/`g_dirs`/`g_procs`.

**The `PERM_FFI` constant** is in `binding_builder.hpp` (`inline constexpr
uint32_t PERM_FFI = 1u << 0;`). Pass it as the 5th arg to
`BindingBuilder::add`:
```cpp
b.add("file_open", bind_handle("file"), {U8S(), type_i64()},  // path slice + mode
      (void*)&n_file_open, PERM_FFI);
b.add("print", type_void(), {U8S()}, (void*)&n_print);  // ungated, no permission arg
```
The slice<u8> param type: `BindingBuilder::add` takes a `std::vector<Type>`
of param types. A `slice<u8>` has no convenience helper in
`binding_builder.hpp`; the existing extensions build it inline via
`make_slice` (`ast.hpp` line 59: `Type make_slice(std::shared_ptr<Type> elem)`).
The exact idiom, read firsthand in `extensions/string/ext_string.cpp` line
142:
```cpp
auto U8S = [](){ return make_slice(std::make_shared<Type>(make_prim(Prim::U8))); };
// then: b.add("string_from_slice", bind_handle("string"), {U8S()}, (void*)&n_string_from_slice);
```
`ext_io` uses the same `U8S` lambda. The native's C++ signature for a
`slice<u8>` arg is `void(uint8_t*, int64_t)` (the two-slot slice ABI,
`BINDING_API.md` §4: `slice<T>` → `T*, int64_t` as two consecutive slots).
This is the established convention — `ext_string`'s `string_from_slice`
works exactly this way. The other type helpers (`type_i64()` / `type_void()` /
`type_f32()` / `type_bool()` / `bind_handle("name")`) are the ones
`ext_string.cpp` uses throughout its `register_natives`.

### CMake — one `ember_add_extension(io, ...)` line + extra link deps

```cmake
ember_add_extension(io extensions/io/ext_io.cpp)
# ext_io links ext_array (file_read/file_write/exec read-into-array via
# get_bytes) + ext_string (dir_next/read_line_str allocate string handles).
# The ember_add_extension macro links ember_frontend PUBLIC; add the two
# extension deps the IO natives reach into:
target_link_libraries(ember_ext_io PUBLIC ember_ext_array ember_ext_string)
```

This produces `ember_ext_io`, a static lib linking `ember_frontend` (transitively
`ember`) + `ember_ext_array` + `ember_ext_string`. A consumer links it and
calls the granular `register_*` functions from its own host native-table
builder, exactly as it does for the other nine extensions
(`extensions/README.md` "How a host registers an extension").

---

## 9. CLI integration — the `--ffi` flag

The standalone CLI (`examples/ember_cli.cpp`) is the first host. Today it
passes `0` as `module_permissions` to `sema()` (line: `auto sr =
sema(pr.program, natives, slots, 0, &overloads, &struct_layouts,
&module_exports);`). To enable I/O:

1. **Register the console natives always** (ungated output — a CLI that
   can't print is useless). Add `ext_io::register_console(natives);` to
   `register_standard_bindings`.
2. **Add a `--ffi` flag** (or `--allow-io`) that:
   - Registers the file/path/dir/exec/spawn natives
     (`ext_io::register_file/path/dir/exec/spawn(natives)`).
   - Passes `PERM_FFI` as `module_permissions` to `sema()` (so the gated
     natives are callable).
3. **Without `--ffi`**: only console-output natives are registered
   (print/println/print_i64, ungated). `read_line`/`file_open`/etc. are
   not registered → a script calling them gets "unknown function" (the
   strongest isolation, §7.1). The module is compiled with
   `module_permissions = 0` → even if a gated native were registered, it
   couldn't be called.

This gives the CLI two modes: **safe mode** (default — output-only,
matches the current "exit-code-as-signal" contract) and **FFI mode**
(`--ffi` — full I/O, the scripting-language mode the ROADMAP's Family B
describes). The `--ffi` flag is the re-entry trigger's proof: a demo or
real use that needs I/O runs `ember run script.ember --ffi`.

**The `--ffi` flag does not weaken the existing safety model.** The
trap/checkpoint machinery (`context_t` + `ember_cli_trap`) still wraps
the entry call; the instruction budget still bounds the ember instruction
count; the budget/depth checks still emit (they're on by default unless
`--emit-em`). The `--ffi` flag only adds natives + grants the permission
bit. A trapped I/O script (e.g. `file_open` of a nonexistent file — though
that returns 0, not a trap) still longjmps to the checkpoint. A blocking
`read_line` blocks the script thread but the host (the CLI) is a single-
call process, so blocking is fine.

---

## 10. Test matrix

Two tiers, matching the existing test-shape discipline
(`ext_runtime_test` for single-thread functional, `ext_sync_test` for
stress):

### Tier 1 — single-thread functional (must pass; no I/O of real files)

These exercise the full lex→parse→sema→codegen→JIT→call path. They use
temp files / temp dirs (created + cleaned up by the test) so they're
hermetic and don't touch the user's filesystem:

1. **console print.** `print("hello"); println("world"); print_i64(42);`
   — assert stdout contains "hello\nworld\n42". (Capture stdout via
   redirect in the test.)
2. **file round-trip.** `file_open(tmp, 1)` (write) → `file_write(f, buf,
   n)` → `file_close(f)` → `file_open(tmp, 0)` (read) → `file_read(f, buf2,
   n)` → assert bytes match. `file_exists(tmp)` → 1; `file_size(tmp)` →
   n; `file_close`.
3. **file error paths.** `file_open("nonexistent_xyz", 0)` → 0 (failed);
   `file_exists("nonexistent_xyz")` → 0; `file_size("nonexistent_xyz")`
   → -1. `file_read(0, buf, 10)` → -1 (bad handle).
4. **path ops.** `path_join("a", "b", dst)` → `"a/b"` (or `a\b` on
   Windows); `path_basename("/foo/bar/baz.txt", dst)` → `"baz.txt"`;
   `path_dirname("/foo/bar/baz.txt", dst)` → `"/foo/bar"`. `path_exists`
   on a temp dir → 1.
5. **directory listing.** `dir_create(tmpdir)` → 1; `dir_open(tmpdir)` →
   handle; `dir_next` returns the entries created in the test;
   `dir_next` → empty string at end; `dir_close`; `dir_exists(tmpdir)` →
   1.
6. **exec synchronous.** `exec("echo hello", buf)` → exit 0; buf contains
   "hello\n". (On Windows: `exec("cmd /c echo hello", buf)`.) `exec` of a
   nonexistent command → nonzero exit or -1.
7. **handle lifetime.** `file_open` → h1; `file_close(h1)`; `file_read(h1,
   buf, 10)` → -1 (closed handle). `reset()` invalidates all handles.

### Tier 2 — permission gating (the security gate)

8. **PERM_FFI compile-time rejection.** A script calling `file_open`
   compiled with `module_permissions = 0` → sema error "function
   'file_open' requires PERM_FFI permission." The call site does not
   reach codegen. (This is the `src/sema.cpp` line 1023-1027 check; the
   test pins it for the IO natives.)
9. **ungated natives callable without permission.** `print("hi")` and
   `path_join(...)` compile + run with `module_permissions = 0` (no
   error). Pins the ungated/gated split.
10. **registration-layer isolation.** A host that registers only
    `register_console` → `file_open` is "unknown function" (sema error,
    distinct from the permission error). Pins the two-layer defense.

### Tier 3 — spawn (async, if `register_spawn` ships)

11. **spawn + proc_wait.** `spawn("sleep 1")` → handle; `proc_status` →
    0 (running); `proc_wait` → exit 0. (Or a Windows equivalent.)
12. **spawn_detach.** `spawn_detach("echo hi")` → 1 (launched); no handle
    to track.

---

## 11. Summary of the scoping decision this plan presents

**Ship a Tier-0-shaped `extensions/io/` extension with six granular
registration entry points (`register_console`/`file`/`path`/`dir`/`exec`/
`spawn`, plus `register_all`) covering console I/O, file I/O, path
manipulation, directory listing, and subprocess execution — all behind
opaque i64 handles (file/dir/proc) into host-owned storage, mirroring
`ext_array`/`ext_sync`. Console output (print/println/print_i64) and pure
path-string ops (path_join/basename/dirname) are ungated; everything that
touches the filesystem, the process tree, or blocking input is
`PERM_FFI`-gated (compile-time sema rejection, zero runtime cost — the
`SAFETY_AND_SANDBOX.md` §6 model). Two layers of defense: the host
chooses which sub-extensions to register at all (registration layer —
unregistered natives are "unknown function," the strongest isolation), and
`PERM_FFI` gating means even registered I/O natives are only callable from
modules the host granted the FFI permission (permission layer). The
extension provides raw capability, not policy — no path sandboxing, no
exec allowlist, no rate limiting; a host that wants policy wraps the
natives (§7). The CLI gets a `--ffi` flag that registers the I/O natives +
passes `PERM_FFI` to sema; without `--ffi` it's output-only (the current
safe default). The extension links `ember_ext_array` (read-into-array
byte-buffer convention via `get_bytes`) and `ember_ext_string` (dir_next/
read_line_str allocate string handles).**

The decision points the user makes from this plan:
1. **One library + six register functions, or six libraries?** Recommend
   one library + six register functions (the host opts in per-capability
   at the registration level; six CMake targets is overkill).
2. **`read_line` array-into vs string-return?** Recommend array-into as
   canonical (decoupled); string-return (`read_line_str`) as opt-in.
3. **`dir_next` string vs array-into?** Recommend string (entry names are
   text; `ext_string` is a reasonable dependency for `register_dir`).
4. **`register_process` split into `register_exec` + `register_spawn`?**
   Recommend yes (a host that wants synchronous `exec` but not async
   `spawn` gets exactly that; `spawn` is the riskiest capability).
5. **CLI `--ffi` flag name + behavior?** Recommend `--ffi` (registers
   file/path/dir/exec/spawn + grants `PERM_FFI` to sema); console-output
   always registered (ungated).
6. **Does `spawn`/`proc_status`/`proc_wait` ship in v1 or defer?**
   Recommend shipping `exec` first (synchronous, the common case);
   `spawn`/`proc_*` as the async refinement — implement if a concrete use
   needs non-blocking subprocess, otherwise defer to a follow-on.
