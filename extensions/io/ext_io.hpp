// ext_io.hpp - ember extension: OS I/O (console + file + path), core subset.
//
// An ember *extension* (see ember/extensions/README.md): a reusable,
// non-cheat-specific addon. This is the CORE subset of the I/O extension
// (docs/planning/plan_OS_IO_EXTENSIONS.md §0/§2/§3/§4): console I/O
// (print/println/print_i64/print_f64/read_line), file I/O
// (file_read_bytes/file_write_bytes/file_exists), and path ops
// (path_exists/path_basename/path_dirname). The full plan adds directory
// listing and subprocess execution as separate sub-registration functions;
// this core ships the most immediately useful surface -- the gap the ROADMAP
// "Family B" trigger fires on: a script that could only signal via exit code
// can now print and read/write files.
//
// === SIGNATURE SHAPE ===
//
// The text natives take/return ember `string` handles (the ext_string
// host-store type), read via ext_string::slot() and minted via
// ext_string::alloc(). The byte natives (file_read_bytes/file_write_bytes)
// use ext_array array<u8> handles -- file_read_bytes mints one via
// ext_array::alloc_bytes, file_write_bytes reads one via
// ext_array::get_bytes(). This couples ext_io to ext_string + ext_array
// (both core extensions the CLI already registers); the coupling is
// one-directional (ext_io calls into them; they do not know about ext_io).
// This is the simplified CORE API the task specifies: string handles for
// text, array handles for bytes -- distinct from the plan's full design
// (which uses slice<u8> for paths and caller-supplied arrays for results to
// keep ext_string decoupled). The core subset optimizes for ergonomics: a
// script writes `print("hello")` and `let buf = file_read_bytes("data.txt")`
// without managing a dst array or converting a string handle to a slice.
//
// === SCOPE (docs/planning/plan_OS_IO_EXTENSIONS.md §0/§7 -- read first) ===
//
// ALL natives in this extension are PERM_FFI-gated: a module compiled without
// the FFI permission bit cannot call them (sema rejects the call site at
// compile time, before codegen -- zero runtime cost, no "check bypassed" path;
// see SAFETY_AND_SANDBOX.md §6). This is a deliberate choice for the core
// subset: console OUTPUT (print/println) is ungated in the full plan (§2.1:
// output-only, no security surface), but the core subset gates everything so
// a host that has not opted into I/O at all sees a uniform permission wall --
// no native in this extension is callable without PERM_FFI. A host that wants
// ungated print can register its own ungated print native (the extension is
// the menu, not the mandate -- §7.1).
//
// TWO LAYERS OF DEFENSE:
//   1. REGISTRATION: the host chooses whether to register this extension at
//      all. A host that does not call register_natives has NO I/O surface --
//      a script calling print gets "unknown function" (the strongest
//      isolation: the capability does not exist in the module's name
//      resolution).
//   2. PERMISSION: even when registered, PERM_FFI gating means a module
//      must be compiled with the FFI bit to call ANY I/O native. The host
//      grants PERM_FFI only to modules it trusts with I/O (the CLI does
//      this via its --ffi flag; without --ffi, sema is called with 0 and
//      every I/O call site is rejected at compile time).
//
// The extension provides RAW CAPABILITY, NOT POLICY. It does not sandbox
// paths (file_read_bytes opens any path the script supplies), does not
// restrict read_line (blocks the script thread on stdin), does not rate-limit.
// A host that wants policy wraps the natives or configures the process
// environment. See §7 of the plan.
//
// === STATE ===
//
// This extension is STATELESS: console I/O goes straight to stdout/stdin via
// C stdio, file I/O opens/closes a FILE* within the single native call (no
// persistent file handles in the core subset -- file_read_bytes/file_write_bytes
// are one-shot whole-file ops), and path ops are pure string manipulation
// (path_basename/path_dirname) or a portable stat (path_exists/file_exists via
// std::filesystem). There is no host-side slot vector; reset() is a no-op
// (provided for API symmetry with the other extensions -- a host that resets
// all extension stores on unload calls it uniformly). The full plan's
// file_open/file_close/dir_open natives DO carry host slot vectors; the core
// subset deliberately does not.
//
// BACKING-STORE ISOLATION (REDSHELL guard #8): no host slot vector here (the
// core subset is stateless), so the guard is satisfied trivially -- no native
// in this extension touches a host-heap slot store, none returns a pointer as
// an i64, and file_read_bytes/file_write_bytes reach into ext_array's
// bounds-checked 1-based handle store via the public get_bytes/alloc_bytes
// accessors (never through a script-supplied raw pointer). Mirrors
// ext_array/ext_string's posture.
#pragma once
#include "sema.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>

namespace ember::ext_io {

// Register the core I/O natives (console + file + path) into m. ALL are
// PERM_FFI-gated. Mirrors ext_array/ext_string's register_natives shape.
void register_natives(std::unordered_map<std::string, NativeSig>& m);

// Clear any host state. The core subset is stateless (no slot vector), so
// this is a no-op -- provided for API symmetry with the other extensions so
// a host that resets all extension stores on unload can call it uniformly.
void reset();

} // namespace ember::ext_io
