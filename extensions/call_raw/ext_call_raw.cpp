// ext_call_raw.cpp - ember extension: raw x64 execution natives + EMBM v1
// module image loader.
//
// Implements six PERM_FFI-gated natives. The first three bridge the
// self-hosted ember codegen's output to actual execution:
//
//   make_executable(bytes: i64) -> i64
//       Takes an array<u8> handle (the codegen's x64 byte buffer), copies the
//       bytes to a W^X executable page (alloc_executable_rw -> seal_executable),
//       and returns the entry pointer as an i64 (0 on failure). The returned
//       pointer is RX (not writable) and lives until free_executable_ptr is
//       called on it. The entry is the FIRST byte of the buffer — i.e. the
//       first function's prologue (the self-hosted codegen lays functions out
//       in declaration order, so a single-fn program's entry is at offset 0).
//
//   call_raw(fn_ptr: i64, arg: i64) -> i64
//       Casts fn_ptr to int64_t(*)(int64_t) and calls it with arg. Returns the
//       callee's i64 return value. A null or garbage fn_ptr crashes the process
//       (the same posture as a C function-pointer dereference — see the
//       header's SECURITY POSTURE: raw capability, not policy).
//
//   free_executable_ptr(ptr: i64) -> void
//       Frees a page returned by make_executable (via free_executable). A
//       script that mints executable pages should free them when done; a
//       long-lived host may instead let them live until unload (the host's
//       reset path does not track them — they are raw VirtualAlloc pages, not
//       extension-slot objects).
//
// These three are the natives the self-hosting milestone
// (docs/planning/plan_SELF_HOSTING.md Stage 4) identified as the gap: they let
// the ember-written codegen's output be EXECUTED — codegen produces x64 bytes
// in array<u8> -> make_executable copies them to an RX page -> call_raw invokes
// the entry point -> free_executable_ptr releases the page. See
// ext_call_raw.hpp for the full framing.
//
// Stateless (no host slot vector); reset() is a no-op (the executable pages
// are owned by the caller via free_executable_ptr, not by this extension).
#include "ext_call_raw.hpp"
#include "ast.hpp"              // type_i64, type_void
#include "binding_builder.hpp"  // BindingBuilder, PERM_FFI
#include "ext_array.hpp"        // ext_array::get_bytes / alloc_bytes
#include "jit_memory.hpp"       // alloc_executable_rw, seal_executable, free_executable
#include "platform.hpp"         // platform::alloc_rw / protect_rx / free_page
#include "module_abi_fingerprint.hpp"  // abi_fingerprint, type_code (EMBM v1 §8)
#include "sema.hpp"             // NativeSig

#include <algorithm>            // std::sort
#include <cstdint>
#include <cstring>
#include <memory>               // std::unique_ptr
#include <mutex>
#include <vector>

using namespace ember;  // BindingBuilder, type_* singletons

namespace ember::ext_call_raw {

// call_raw(fn_ptr: i64, arg: i64) -> i64
// Cast fn_ptr to int64_t(*)(int64_t) and call it with arg. Returns the
// callee's i64 return value. A null or garbage fn_ptr crashes the process
// (the same posture as a C function-pointer dereference — see the header's
// SECURITY POSTURE: raw capability, not policy).
//
// The cast + call is a single C-level reinterpret_cast + indirect call. The
// Win64 / SysV calling convention for an int64_t(*)(int64_t) matches the
// ember i64->i64 call shape exactly (first int arg in rcx/rdi, return in
// rax), so no trampoline is needed — the native IS the call.
static int64_t n_call_raw(int64_t fn_ptr, int64_t arg) {
    using Fn = int64_t(*)(int64_t);
    // Security guard: reject null/garbage fn_ptr instead of crashing the process
    // (audit MEDIUM finding — the original design deliberately crashed, but a
    // recoverable error is safer for a scripting language). Returns INT64_MIN
    // as a sentinel error value (same convention as thread_join trap signal).
    if (fn_ptr == 0) {
        return INT64_MIN;  // null function pointer — recoverable error
    }
    Fn f = reinterpret_cast<Fn>(fn_ptr);
    return f(arg);
}

// make_executable(bytes: i64) -> i64
// Takes an array<u8> handle (the codegen's byte buffer), copies the bytes to a
// W^X executable page, and returns the entry pointer as an i64 (0 on failure).
// The returned page is RX (sealed, not writable) and owned by the caller until
// free_executable_ptr is called on it.
//
// The entry is the first byte of the buffer — i.e. the first function's
// prologue. The self-hosted codegen lays functions out in declaration order,
// so for a single-fn program the entry is at offset 0. (Multi-fn programs with
// calls use a placeholder fn-address that the host would patch at JIT time;
// make_executable does NOT patch placeholders, so a program that calls other
// fns would branch to address 0. The execution demo uses a single self-
// contained fn — pure arithmetic, no calls — whose bytes are directly
// executable.)
static int64_t n_make_executable(int64_t bytes_handle) {
    uint8_t* data = nullptr;
    int64_t  len  = 0;
    if (!ext_array::get_bytes(bytes_handle, &data, &len) || len <= 0) {
        return 0;  // invalid handle or empty buffer -> no page
    }
    std::vector<uint8_t> code(data, data + size_t(len));
    void* page = alloc_executable(code);  // RW -> memcpy -> seal RX (one-shot)
    return int64_t(page);                 // 0 (nullptr) on alloc/seal failure
}

// free_executable_ptr(ptr: i64) -> void
// Frees a page returned by make_executable. Safe to call with 0 (free_executable
// is a no-op on nullptr). Calling with a non-page i64 is UB (same posture as
// free() on a garbage pointer) — the caller is expected to pass exactly what
// make_executable returned.
static void n_free_executable_ptr(int64_t ptr) {
    free_executable(reinterpret_cast<void*>(ptr));
}

// =====================================================================
// EMBM v1 module image loader (self_hosted/MODULE_IMAGE_FORMAT.md).
//
// Three PERM_FFI-gated natives that replace the bare code-blob +
// make_executable model so the self-hosted compiler can emit real string
// literals (rodata), mutable globals (data section), and native/extension
// calls (relocated call sites) as one self-contained array<u8> image:
//
//   load_executable_module(image: array<u8>) -> i64
//       Parse + validate an EMBM v1 image, allocate stable code(RX)/
//       rodata(R)/data(RW) regions (reusing jit_memory/platform helpers — no
//       second allocator), copy sections, apply ABS64 relocations (rodata /
//       data / native with ABI-fingerprint validation), protect permissions,
//       and return an opaque owning handle (0 on failure). On ANY validation
//       failure every allocated region is freed and 0 is returned (no partial
//       state, no partial patching).
//
//   module_entry_ptr(handle: i64, code_offset: i64) -> i64
//       code_base + code_offset for a function in the loaded module. Validates
//       the handle is live and 0 <= code_offset < code_len; returns 0 otherwise.
//       Entry pointers are invalidated when the handle is freed.
//
//   free_executable_module(handle: i64) -> void
//       Frees all regions owned by a loaded module handle and removes it.
//       Rejects an unknown / already-freed handle (double-free) as a no-op.
//
// Native-symbol resolution: ABS64_NATIVE relocations reference a symbol by
// index into the image's null-terminated symbol-name table; the loader
// resolves the name against the CURRENT host native table. The host native
// table is a compile-time construct (CodeGenCtx::natives, baked fn_ptrs live
// in JIT'd code) — no existing native reaches it at runtime — so this loader
// reads it through a process-wide pointer the host sets before running JIT'd
// code that loads modules (set_loader_context, mirroring g_globals_for_codegen).
// The host also sets the calling module's granted permission mask so the
// loader can validate that a relocated native's required permission is granted.
// =====================================================================

// Host wiring: the host sets these before running JIT'd code that calls
// load_executable_module. `g_loader_natives` is the table ABS64_NATIVE relocs
// resolve against; `g_loader_permissions` is the calling module's granted
// permission mask (a relocated native's required permission must be a subset).
// Null natives / zero perms is the unset state: an image with NATIVE relocs is
// rejected in that state (cannot resolve), which is the safe default.
static const std::unordered_map<std::string, NativeSig>* g_loader_natives = nullptr;
static uint32_t g_loader_permissions = 0;

// A loaded module: the three stable regions + their lengths. The handle is an
// index+1 into g_modules (the same 1-based opaque-handle convention ext_array
// uses). code is RX (sealed), rodata is RX (read-only — platform exposes no
// PAGE_READONLY helper, so the closest read-only-non-writable protection is
// RX; W^X is preserved: rodata is never writable), data is RW (mutable
// globals, a fresh per-load copy).
struct LoadedModule {
    void*  code_base   = nullptr;  // RX after seal_executable
    size_t code_len    = 0;
    void*  rodata_base = nullptr;  // R (via protect_rx: read-only, not writable)
    size_t rodata_len  = 0;
    void*  data_base   = nullptr;  // RW (mutable globals, fresh per load)
    size_t data_len    = 0;
};
static std::vector<std::unique_ptr<LoadedModule>> g_modules;
static std::mutex g_module_mutex;

void set_loader_context(const std::unordered_map<std::string, NativeSig>* natives,
                        uint32_t permissions) {
    std::lock_guard<std::mutex> lock(g_module_mutex);
    g_loader_natives = natives;
    g_loader_permissions = permissions;
}

// ---- little-endian readers (the image is LE per §1) ----
static uint32_t rd_u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
static uint64_t rd_u64(const uint8_t* p) {
    return uint64_t(p[0]) | (uint64_t(p[1]) << 8) | (uint64_t(p[2]) << 16) |
           (uint64_t(p[3]) << 24) | (uint64_t(p[4]) << 32) | (uint64_t(p[5]) << 40) |
           (uint64_t(p[6]) << 48) | (uint64_t(p[7]) << 56);
}

// Validation caps (§2).
static constexpr uint64_t CAP_SECTION = 16ull * 1024 * 1024;  // 16 MiB
static constexpr uint64_t CAP_SYMS    =  1ull * 1024 * 1024;  //  1 MiB
static constexpr uint64_t CAP_RELOCS  = 65536;
static constexpr uint64_t CAP_TOTAL   = 64ull * 1024 * 1024;  // 64 MiB
static constexpr uint64_t EMBM_HEADER = 80;
static constexpr uint64_t RELOC_SIZE  = 32;

// Relocation types (§7).
static constexpr uint64_t RELOC_RODATA = 1;
static constexpr uint64_t RELOC_DATA   = 2;
static constexpr uint64_t RELOC_NATIVE = 3;

// A parsed relocation record (32 bytes in the image).
struct ParsedReloc {
    uint64_t type;
    uint64_t patch_off;
    uint64_t sym_idx;
    uint64_t addend;
    uint64_t value;     // resolved value to write at code[patch_off] (apply phase)
};

// load_executable_module(image: array<u8>) -> i64
// Full EMBM v1 parse + validate + allocate + copy + relocate + protect.
// Returns an opaque owning handle (1-based), or 0 on ANY failure (every
// allocated region freed; no partial state).
static int64_t n_load_executable_module(int64_t bytes_handle) {
    // --- read the image bytes from the array<u8> handle (same pattern as
    //     n_make_executable) ---
    uint8_t* img = nullptr;
    int64_t  img_len = 0;
    if (!ext_array::get_bytes(bytes_handle, &img, &img_len) || img_len <= 0) {
        return 0;  // invalid handle / empty buffer
    }
    uint64_t total = uint64_t(img_len);

    // ===== STEP 1: validate header + section bounds + caps BEFORE allocating
    //         (§9 lifecycle: reject malformed images without any allocation). =====
    if (total < EMBM_HEADER) return 0;                 // truncated (no header)
    if (total > CAP_TOTAL)   return 0;                 // oversized image

    // magic + version
    if (!(img[0] == 0x45 && img[1] == 0x4D &&
          img[2] == 0x42 && img[3] == 0x4D)) return 0; // unknown magic
    if (rd_u32(img + 4) != 1) return 0;                // unknown version

    uint64_t code_off   = rd_u64(img + 8);
    uint64_t code_len   = rd_u64(img + 16);
    uint64_t rodata_off = rd_u64(img + 24);
    uint64_t rodata_len = rd_u64(img + 32);
    uint64_t data_off   = rd_u64(img + 40);
    uint64_t data_len   = rd_u64(img + 48);
    uint64_t syms_off   = rd_u64(img + 56);
    uint64_t syms_len   = rd_u64(img + 64);
    uint64_t reloc_count = rd_u64(img + 72);

    // caps
    if (code_len   > CAP_SECTION) return 0;
    if (rodata_len > CAP_SECTION) return 0;
    if (data_len   > CAP_SECTION) return 0;
    if (syms_len   > CAP_SYMS)    return 0;
    if (reloc_count > CAP_RELOCS) return 0;

    // section bounds + non-overlapping + in section order (header < code <
    // rodata < data < symbols < relocations). A zero-length section may have
    // off = 0 (absent); it does not advance the cursor.
    uint64_t cursor = EMBM_HEADER;  // end of header
    auto check_section = [&](uint64_t off, uint64_t len) -> bool {
        if (len == 0) return true;            // absent section: off ignored
        if (off < cursor) return false;       // out of order / overlap
        if (off + len < off) return false;    // offset+length overflow
        if (off + len > total) return false;  // runs past image end
        cursor = off + len;
        return true;
    };
    if (!check_section(code_off,   code_len))   return 0;
    if (!check_section(rodata_off, rodata_len)) return 0;
    if (!check_section(data_off,   data_len))   return 0;
    if (!check_section(syms_off,   syms_len))   return 0;
    // relocations immediately follow the symbol table (or the last present
    // section); cursor is now the end of the last present section.
    uint64_t reloc_start = cursor;              // == syms_off+syms_len if syms present
    uint64_t reloc_bytes = reloc_count * RELOC_SIZE;
    if (reloc_count > 0) {
        if (reloc_start + reloc_bytes < reloc_start) return 0;  // overflow
        if (reloc_start + reloc_bytes > total)        return 0;  // runs past end
    }

    // ===== STEP 2: allocate stable regions + copy sections (writable) =====
    auto mod = std::make_unique<LoadedModule>();

    // code: RW page with code bytes copied in (patched next, then sealed RX).
    if (code_len > 0) {
        std::vector<uint8_t> code_copy(img + code_off, img + code_off + code_len);
        void* codep = alloc_executable_rw(code_copy);  // RW, bytes copied
        if (!codep) return 0;                          // allocation failure
        mod->code_base = codep;
        mod->code_len  = size_t(code_len);
    }

    // rodata: RW page, copy bytes, then protect_rx (read-only, not writable).
    if (rodata_len > 0) {
        void* rp = ember::platform::alloc_rw(size_t(rodata_len));
        if (!rp) { free_executable(mod->code_base); return 0; }
        std::memcpy(rp, img + rodata_off, size_t(rodata_len));
        mod->rodata_base = rp;
        mod->rodata_len  = size_t(rodata_len);
    }

    // data: RW page, fresh per-load copy of the initialized mutable globals.
    if (data_len > 0) {
        void* dp = ember::platform::alloc_rw(size_t(data_len));
        if (!dp) {
            free_executable(mod->code_base);
            ember::platform::free_page(mod->rodata_base, mod->rodata_len);
            return 0;
        }
        std::memcpy(dp, img + data_off, size_t(data_len));
        mod->data_base = dp;
        mod->data_len  = size_t(data_len);
    }

    // Helper to tear down everything allocated so far (failure path).
    auto teardown = [&]() {
        free_executable(mod->code_base);
        ember::platform::free_page(mod->rodata_base, mod->rodata_len);
        ember::platform::free_page(mod->data_base, mod->data_len);
    };

    // ===== STEP 3: parse the symbol-name table (null-terminated strings) =====
    std::vector<std::string> symbols;
    if (syms_len > 0) {
        const uint8_t* sp = img + syms_off;
        const uint8_t* send = img + syms_off + syms_len;
        while (sp < send) {
            const uint8_t* nul = (const uint8_t*)std::memchr(sp, 0, size_t(send - sp));
            if (!nul) { teardown(); return 0; }   // malformed: no terminator
            symbols.emplace_back(reinterpret_cast<const char*>(sp), size_t(nul - sp));
            sp = nul + 1;
        }
    }

    // ===== STEP 4: read + validate all relocations BEFORE patching (no
    //         partial patching on failure). Cache the resolved value per reloc. =====
    std::vector<ParsedReloc> relocs;
    relocs.reserve(size_t(reloc_count));
    for (uint64_t i = 0; i < reloc_count; ++i) {
        const uint8_t* r = img + reloc_start + i * RELOC_SIZE;
        ParsedReloc pr;
        pr.type      = rd_u64(r + 0);
        pr.patch_off = rd_u64(r + 8);
        pr.sym_idx   = rd_u64(r + 16);
        pr.addend    = rd_u64(r + 24);
        pr.value     = 0;
        relocs.push_back(pr);
    }

    // patch_off must be in-bounds for the 8-byte imm64 slot (§7).
    for (auto& pr : relocs) {
        if (pr.patch_off + 8 < pr.patch_off) { teardown(); return 0; }  // overflow
        if (pr.patch_off + 8 > code_len)     { teardown(); return 0; }  // OOB
    }

    // No two relocations may overlap: sort by patch_off, reject if a slot's
    // 8 bytes run into the next slot (§7).
    std::sort(relocs.begin(), relocs.end(),
              [](const ParsedReloc& a, const ParsedReloc& b) {
                  return a.patch_off < b.patch_off;
              });
    for (size_t i = 1; i < relocs.size(); ++i) {
        if (relocs[i - 1].patch_off + 8 > relocs[i].patch_off) {
            teardown(); return 0;  // overlapping patches
        }
    }

    // Resolve each relocation's value (full validation; reject the whole image
    // on any failure — unknown type, OOB addend, unknown native, ABI mismatch,
    // missing permission, nonzero reserved fingerprint bits).
    const std::unordered_map<std::string, NativeSig>* natives = g_loader_natives;
    for (auto& pr : relocs) {
        if (pr.type == RELOC_RODATA) {
            if (rodata_len == 0) { teardown(); return 0; }       // no rodata to ref
            if (pr.addend >= rodata_len) { teardown(); return 0; } // addend OOB
            uintptr_t base = reinterpret_cast<uintptr_t>(mod->rodata_base);
            if (pr.addend > UINT64_MAX - base) { teardown(); return 0; } // wrap
            pr.value = uint64_t(base + pr.addend);
        } else if (pr.type == RELOC_DATA) {
            if (data_len == 0) { teardown(); return 0; }         // no data to ref
            if (pr.addend >= data_len) { teardown(); return 0; } // addend OOB
            uintptr_t base = reinterpret_cast<uintptr_t>(mod->data_base);
            if (pr.addend > UINT64_MAX - base) { teardown(); return 0; } // wrap
            pr.value = uint64_t(base + pr.addend);
        } else if (pr.type == RELOC_NATIVE) {
            // sym_idx must index a valid symbol.
            if (pr.sym_idx >= symbols.size()) { teardown(); return 0; }
            const std::string& name = symbols[size_t(pr.sym_idx)];
            if (!natives) { teardown(); return 0; }               // no table to resolve against
            auto it = natives->find(name);
            if (it == natives->end()) { teardown(); return 0; }   // unknown native
            const NativeSig& sig = it->second;
            // ABI fingerprint (§8): reserved bits 38..63 must be zero, then the
            // whole u64 must match the registered signature's fingerprint.
            if (pr.addend & ~((1ull << 38) - 1ull)) { teardown(); return 0; } // nonzero reserved
            uint64_t fp = abi_fingerprint(sig);
            if (pr.addend != fp) { teardown(); return 0; }        // ABI mismatch
            // The calling module must have been granted the native's required
            // permission (every required bit present in g_loader_permissions).
            if ((sig.permission & g_loader_permissions) != sig.permission) {
                teardown(); return 0;                             // missing permission
            }
            if (!sig.fn_ptr) { teardown(); return 0; }            // no fn pointer
            pr.value = uint64_t(reinterpret_cast<uintptr_t>(sig.fn_ptr));
        } else {
            teardown(); return 0;  // unknown relocation type
        }
    }

    // ===== STEP 5: apply — write each resolved u64 LE into the code page
    //         (still RW), then protect. All validation passed, so patching is
    //         total (no partial state). =====
    uint8_t* code_bytes = reinterpret_cast<uint8_t*>(mod->code_base);
    for (auto& pr : relocs) {
        uint64_t v = pr.value;
        std::memcpy(code_bytes + pr.patch_off, &v, 8);
    }

    // Protect: code RX (seal), rodata R (protect_rx — read-only, not writable).
    // data stays RW (mutable globals). Failures roll back the whole load.
    if (mod->code_len > 0) {
        if (!seal_executable(mod->code_base, mod->code_len)) { teardown(); return 0; }
    }
    if (mod->rodata_len > 0) {
        if (!ember::platform::protect_rx(mod->rodata_base, mod->rodata_len)) {
            // rodata couldn't be sealed read-only; code already RX. Roll back.
            teardown(); return 0;
        }
    }

    // ===== STEP 6: publish the handle (1-based index into g_modules). =====
    std::lock_guard<std::mutex> lock(g_module_mutex);
    int64_t handle = int64_t(g_modules.size()) + 1;
    g_modules.push_back(std::move(mod));
    return handle;
}

// module_entry_ptr(handle: i64, code_offset: i64) -> i64
// Returns code_base + code_offset for a function in a loaded module. Validates
// the handle is live and 0 <= code_offset < code_len; returns 0 otherwise.
// Entry pointers are invalidated when the handle is freed.
static int64_t n_module_entry_ptr(int64_t handle, int64_t code_offset) {
    std::lock_guard<std::mutex> lock(g_module_mutex);
    if (handle < 1 || handle > int64_t(g_modules.size())) return 0;
    const auto& mod = g_modules[size_t(handle - 1)];
    if (!mod) return 0;                                  // freed handle
    if (code_offset < 0) return 0;                       // negative offset
    if (uint64_t(code_offset) >= mod->code_len) return 0; // out of bounds
    return int64_t(reinterpret_cast<uintptr_t>(mod->code_base) + uintptr_t(code_offset));
}

// free_executable_module(handle: i64) -> void
// Frees all regions owned by a loaded module handle and removes it. Rejects
// an unknown / already-freed handle (double-free) as a no-op — it does NOT
// crash on a bad handle (the handle is a 1-based index into a null-able slot
// vector, so an out-of-range or freed handle is detected, not dereferenced).
static void n_free_executable_module(int64_t handle) {
    std::unique_ptr<LoadedModule> mod;
    {
        std::lock_guard<std::mutex> lock(g_module_mutex);
        if (handle < 1 || handle > int64_t(g_modules.size())) return;  // unknown
        mod = std::move(g_modules[size_t(handle - 1)]);                // nulls the slot
        if (!mod) return;                                              // already freed
    }
    // Freed outside the lock (no allocator work under the mutex).
    free_executable(mod->code_base);
    ember::platform::free_page(mod->rodata_base, mod->rodata_len);
    ember::platform::free_page(mod->data_base, mod->data_len);
}

// Register the six natives, all PERM_FFI-gated (raw execution is a security
// surface — a script that can mint + call executable pages can branch to
// arbitrary host code). Mirrors ext_io/ext_array's BindingBuilder shape.
// The three module-loader natives (load_executable_module / module_entry_ptr /
// free_executable_module) are the EMBM v1 image loader
// (self_hosted/MODULE_IMAGE_FORMAT.md §9).
void register_natives(std::unordered_map<std::string, NativeSig>& m) {
    BindingBuilder b;
    b.add("call_raw",            type_i64(), {type_i64(), type_i64()},
          (void*)&n_call_raw,            PERM_FFI);
    b.add("make_executable",     type_i64(), {type_i64()},
          (void*)&n_make_executable,     PERM_FFI);
    b.add("free_executable_ptr", type_void(), {type_i64()},
          (void*)&n_free_executable_ptr, PERM_FFI);
    b.add("load_executable_module",  type_i64(), {type_i64()},
          (void*)&n_load_executable_module,  PERM_FFI);
    b.add("module_entry_ptr",    type_i64(), {type_i64(), type_i64()},
          (void*)&n_module_entry_ptr,    PERM_FFI);
    b.add("free_executable_module", type_void(), {type_i64()},
          (void*)&n_free_executable_module, PERM_FFI);
    NativeTable t = b.build();
    for (auto& kv : t.natives) m[kv.first] = std::move(kv.second);
}

// Stateless for the original three natives (executable pages are caller-owned
// via free_executable_ptr). The module-loader natives DO own state (g_modules,
// the loaded-module slot vector), so reset() frees every live module handle so
// a host that resets all extension stores on unload reclaims the loaded code /
// rodata / data regions uniformly.
void reset() {
    std::vector<std::unique_ptr<LoadedModule>> to_free;
    {
        std::lock_guard<std::mutex> lock(g_module_mutex);
        to_free.swap(g_modules);
        g_loader_natives = nullptr;
        g_loader_permissions = 0;
    }
    for (auto& mod : to_free) {
        if (!mod) continue;
        free_executable(mod->code_base);
        ember::platform::free_page(mod->rodata_base, mod->rodata_len);
        ember::platform::free_page(mod->data_base, mod->data_len);
    }
}

} // namespace ember::ext_call_raw
