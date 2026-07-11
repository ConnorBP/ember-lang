// ext_io.cpp - ember extension: OS I/O (console + file + path), core subset.
//
// Implements the CORE I/O surface (docs/planning/plan_OS_IO_EXTENSIONS.md
// §2/§3/§4) as a Tier-0-shaped extension: BindingBuilder registration, string
// handles via ext_string::slot/alloc, byte arrays via ext_array::get_bytes/
// alloc_bytes, portable C stdio + std::filesystem. No host slot vector (the
// core subset is stateless -- one-shot whole-file ops, no persistent file
// handles); reset() is a no-op. See ext_io.hpp for the scope/safety framing.
//
// All natives are PERM_FFI-gated (the task's security-by-default choice for
// the core subset; the full plan ungates print/println as output-only §2.1).
// The CLI grants PERM_FFI via its --ffi flag; without it, sema rejects every
// I/O call site at compile time.
#include "ext_io.hpp"
#include "ast.hpp"                  // type_i64/void/f64, bind_handle, make_prim
#include "binding_builder.hpp"      // BindingBuilder, PERM_FFI
#include "../string/ext_string.hpp" // slot(), alloc()
#include "../array/ext_array.hpp"   // get_bytes(), alloc_bytes()

#include <cstdio>      // fopen/fread/fwrite/fclose/fseek/ftell/fgets/fputs/printf/snprintf
#include <cstring>     // memcpy
#include <filesystem>  // exists, is_regular_file (portable file/dir stat)
#include <string>
#include <vector>

using namespace ember;  // bind_handle, BindingBuilder, type_* singletons

namespace ember::ext_io {

// ---------------------------------------------------------------------------
// path string helpers -- pure manipulation (find last '/' or '\\'), no FS access.
// Used by path_basename/path_dirname (the task specifies simple string ops,
// not std::filesystem::path, so the separator handling is explicit and the
// result is byte-identical on every platform for the same input).
// ---------------------------------------------------------------------------
static std::string path_basename_str(const std::string& p) {
    if (p.empty()) return std::string();
    // Strip trailing separators so "/foo/bar/" -> "bar" (not "").
    size_t end = p.size();
    while (end > 0 && (p[end - 1] == '/' || p[end - 1] == '\\')) --end;
    if (end == 0) return std::string();  // all separators
    // Find the last separator in [0, end).
    size_t pos = end;
    while (pos > 0 && p[pos - 1] != '/' && p[pos - 1] != '\\') --pos;
    return p.substr(pos, end - pos);
}

static std::string path_dirname_str(const std::string& p) {
    if (p.empty()) return std::string(".");
    // Strip trailing separators so "/foo/bar/" -> "/foo" (not "/foo/bar").
    size_t end = p.size();
    while (end > 0 && (p[end - 1] == '/' || p[end - 1] == '\\')) --end;
    if (end == 0) return std::string(".");  // all separators -> current dir
    // Find the last separator in [0, end).
    size_t pos = end;
    while (pos > 0 && p[pos - 1] != '/' && p[pos - 1] != '\\') --pos;
    if (pos == 0) return std::string(".");  // no separator -> current dir
    // Strip trailing separators from the directory part.
    while (pos > 0 && (p[pos - 1] == '/' || p[pos - 1] == '\\')) --pos;
    if (pos == 0) return std::string("/");  // was root ("/foo" -> "/")
    return p.substr(0, pos);
}

// ---------------------------------------------------------------------------
// CONSOLE I/O natives
// ---------------------------------------------------------------------------
extern "C" {

// print(text: string) -- write the string's bytes to stdout, no newline.
static void n_print(int64_t h) {
    const std::string* s = ext_string::slot(h);
    if (s && !s->empty()) std::fwrite(s->data(), 1, s->size(), stdout);
}

// println(text: string) -- write the string + '\n' to stdout.
static void n_println(int64_t h) {
    const std::string* s = ext_string::slot(h);
    if (s && !s->empty()) std::fwrite(s->data(), 1, s->size(), stdout);
    std::fputc('\n', stdout);
}

// print_i64(v: i64) -- write the decimal integer to stdout.
static void n_print_i64(int64_t v) {
    char buf[32];
    int n = std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    if (n > 0) std::fwrite(buf, 1, static_cast<size_t>(n), stdout);
}

// print_f64(v: f64) -- write the float to stdout.
static void n_print_f64(double v) {
    char buf[64];
    int n = std::snprintf(buf, sizeof(buf), "%g", v);
    if (n > 0) std::fwrite(buf, 1, static_cast<size_t>(n), stdout);
}

// read_line() -> string -- read a line from stdin (blocking). Strips the
// trailing newline. Returns a string handle (possibly empty for a blank
// line); returns 0 (null handle) on EOF with nothing read, so a script can
// distinguish end-of-input from a genuinely empty line.
static int64_t n_read_line() {
    std::string line;
    char buf[1024];
    while (true) {
        if (!std::fgets(buf, static_cast<int>(sizeof(buf)), stdin)) {
            if (line.empty()) return 0;  // EOF, nothing read yet -> null handle
            break;  // EOF but we have a partial line (no trailing newline)
        }
        line += buf;
        size_t nl = line.find('\n');
        if (nl != std::string::npos) {
            line.erase(nl);  // strip the newline (and anything after, which
                             // can't happen -- fgets stops at the first \n)
            break;
        }
    }
    return ext_string::alloc(std::move(line));
}

// ---------------------------------------------------------------------------
// FILE I/O natives (one-shot whole-file ops; no persistent file handle in the
// core subset -- the full plan's file_open/file_close carry a host slot vector)
// ---------------------------------------------------------------------------

// file_read_bytes(path: string) -> i64 -- read the entire file into a new
// array<u8>; returns the array handle, or 0 on failure (bad path handle,
// fopen error, read error). A 0-byte file returns a VALID empty array handle
// (not 0) -- only an actual failure returns 0.
static int64_t n_file_read_bytes(int64_t path_h) {
    const std::string* path = ext_string::slot(path_h);
    if (!path) return 0;
    FILE* fp = std::fopen(path->c_str(), "rb");
    if (!fp) return 0;
    // Determine size via seek-to-end + ftell. (Portable for regular files;
    // returns failure for pipes/special files, which is the right call here --
    // file_read_bytes is a whole-file op, not a stream op.)
    if (std::fseek(fp, 0, SEEK_END) != 0) { std::fclose(fp); return 0; }
    long sz = std::ftell(fp);
    if (sz < 0) { std::fclose(fp); return 0; }
    if (std::fseek(fp, 0, SEEK_SET) != 0) { std::fclose(fp); return 0; }
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    size_t got = 0;
    if (sz > 0) {
        got = std::fread(buf.data(), 1, static_cast<size_t>(sz), fp);
        buf.resize(got);
    }
    std::fclose(fp);
    return ext_array::alloc_bytes(buf.data(), static_cast<int64_t>(got));
}

// file_write_bytes(path: string, data: i64) -> i64 -- write the array<u8>'s
// bytes to path (truncate + create). Returns 1 on success, 0 on failure (bad
// path/data handle, fopen error, short write).
static int64_t n_file_write_bytes(int64_t path_h, int64_t data_h) {
    const std::string* path = ext_string::slot(path_h);
    if (!path) return 0;
    uint8_t* data = nullptr;
    int64_t len = 0;
    if (!ext_array::get_bytes(data_h, &data, &len)) return 0;
    FILE* fp = std::fopen(path->c_str(), "wb");
    if (!fp) return 0;
    if (len > 0 && data) {
        size_t wrote = std::fwrite(data, 1, static_cast<size_t>(len), fp);
        std::fclose(fp);
        return (wrote == static_cast<size_t>(len)) ? 1 : 0;
    }
    std::fclose(fp);
    return 1;  // wrote 0 bytes successfully (created/truncated an empty file)
}

// file_exists(path: string) -> i64 -- 1 if path exists and is a regular file,
// 0 otherwise (missing, directory, special file, bad handle).
static int64_t n_file_exists(int64_t path_h) {
    const std::string* path = ext_string::slot(path_h);
    if (!path) return 0;
    std::error_code ec;
    return std::filesystem::is_regular_file(*path, ec) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// PATH natives
// ---------------------------------------------------------------------------

// path_exists(path: string) -> i64 -- 1 if the path exists at all (file OR
// directory), 0 otherwise. Uses std::filesystem::exists (portable stat).
static int64_t n_path_exists(int64_t path_h) {
    const std::string* path = ext_string::slot(path_h);
    if (!path) return 0;
    std::error_code ec;
    return std::filesystem::exists(*path, ec) && !ec ? 1 : 0;
}

// path_basename(path: string) -> string -- the filename component (everything
// after the last '/' or '\\'). Pure string manipulation; no FS access.
static int64_t n_path_basename(int64_t path_h) {
    const std::string* path = ext_string::slot(path_h);
    if (!path) return ext_string::alloc(std::string());
    return ext_string::alloc(path_basename_str(*path));
}

// path_dirname(path: string) -> string -- the directory component (everything
// up to but not including the last separator). Returns "." if there is no
// separator (no directory part). Pure string manipulation; no FS access.
static int64_t n_path_dirname(int64_t path_h) {
    const std::string* path = ext_string::slot(path_h);
    if (!path) return ext_string::alloc(std::string("."));
    return ext_string::alloc(path_dirname_str(*path));
}

}  // extern "C"

// ---------------------------------------------------------------------------
// Registration -- all 10 natives, ALL PERM_FFI-gated (security by default:
// a host that has not granted the FFI bit sees a uniform permission wall).
// Mirrors ext_array/ext_string's BindingBuilder shape verbatim.
// ---------------------------------------------------------------------------
void register_natives(std::unordered_map<std::string, NativeSig>& m) {
    BindingBuilder b;
    const auto Str = bind_handle("string");  // the ext_string host-store type

    // --- console ---
    b.add("print",          type_void(), {Str},              (void*)&n_print,         PERM_FFI);
    b.add("println",        type_void(), {Str},              (void*)&n_println,       PERM_FFI);
    b.add("print_i64",      type_void(), {type_i64()},       (void*)&n_print_i64,     PERM_FFI);
    b.add("print_f64",      type_void(), {type_f64()},       (void*)&n_print_f64,     PERM_FFI);
    b.add("read_line",      Str,         {},                 (void*)&n_read_line,     PERM_FFI);

    // --- file ---
    b.add("file_read_bytes",  type_i64(), {Str},             (void*)&n_file_read_bytes,  PERM_FFI);
    b.add("file_write_bytes", type_i64(), {Str, type_i64()}, (void*)&n_file_write_bytes, PERM_FFI);
    b.add("file_exists",      type_i64(), {Str},             (void*)&n_file_exists,      PERM_FFI);

    // --- path ---
    b.add("path_exists",   type_i64(), {Str},                (void*)&n_path_exists,   PERM_FFI);
    b.add("path_basename", Str,        {Str},                (void*)&n_path_basename, PERM_FFI);
    b.add("path_dirname",  Str,        {Str},                (void*)&n_path_dirname,  PERM_FFI);

    NativeTable t = b.build();
    for (auto& kv : t.natives) m[kv.first] = std::move(kv.second);
}

// Stateless extension: no host slot vector to clear. Provided for API symmetry
// with the other extensions (a host that resets all stores on unload calls it
// uniformly). The string/array handles a script obtained via read_line/
// file_read_bytes are owned by ext_string/ext_array's stores and are cleared
// by THEIR reset() -- not here.
void reset() {}

}  // namespace ember::ext_io
