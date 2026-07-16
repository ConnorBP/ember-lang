// ember_stub_main - standalone ember runtime stub (docs/planning/plan_STANDALONE_BUNDLER.md).
//
// The `ember bundle` command copies this exe to <output.exe> + appends a
// .em blob + a 12-byte footer. At runtime this main() reads its own file,
// finds the appended .em via the footer, loads it via `load_em_bytes`, and
// calls the entry function — the script runs as a self-contained exe with
// no separate ember install.
//
// This is the CLI's `--load-em` path (examples/ember_cli.cpp lines 912-922)
// with the .em source swapped from `load_em_file(path)` to
// `load_em_bytes(embedded_ptr, embedded_len)`. No lexer, no parser, no sema,
// no codegen-tree-walker at runtime — the .em is pre-compiled; load is
// memcpy + reloc patch + native binding resolution.
//
// Links: ember + ember_frontend + the standard extension libs (the same
// native allowlist the CLI registers, so any CLI-compiled .em loads). Does
// NOT link ember_import (no source imports at runtime). Does NOT parse/sema/
// codegen (the .em is pre-compiled — the v4 raw-x86 load path is memcpy +
// reloc patch, no re-emit).
//
// Footer format (12 bytes, little-endian, appended AFTER the .em at the very
// end of the file):
//   magic      : u32 = 0x454D4244  ("EMBD" — ember bundle, appended)
//   em_length  : u64              (byte length of the .em just appended)
//
// The stub reads the last 12 bytes of its own file, checks the magic, reads
// em_length, seeks back 12 + em_length bytes from the end, reads the .em.

#include "../src/engine.hpp"          // call_i64_i64
#include "../src/em_loader.hpp"       // load_em_bytes, LoadedModule
#include "../src/em_file.hpp"         // EM_MAGIC etc. (for reference)

#include "ext_vec.hpp"
#include "ext_quat.hpp"
#include "ext_mat.hpp"
#include "ext_string.hpp"
#include "ext_array.hpp"
#include "ext_math.hpp"
#include "ext_map.hpp"
#include "ext_sync.hpp"
#include "ext_lifecycle.hpp"
#include "ext_io.hpp"
#include "ext_call_raw.hpp"     // self-hosting Stage 4 gap: call_raw(fn_ptr,arg)->i64
#include "ext_coroutine.hpp"   // #21 coroutines (set_coroutine_dispatch native)
#include "ext_graphics.hpp"    // Win32 + D3D11 full-screen shader rendering
#include "ext_visualize.hpp"   // audio analysis and compact LLM exports
#include "ext_ui.hpp"          // ImGui bindings (no-op without an editor frame)

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#endif

// The appended-bundle footer magic ("EMBD" — ember bundle, appended).
// Distinct from EM_MAGIC ("EMBL" = 0x454D424C) and EM_SIG_MAGIC ("EMSG" =
// 0x454D5347) so the stub can tell "I have an embedded module" from "I'm a
// plain stub with no .em" and from a coincidental byte pattern.
constexpr uint32_t EM_BUNDLE_MAGIC      = 0x454D4244u;
constexpr uint32_t EM_BUNDLE_FOOTER_SIZE = 12u;  // u32 magic + u64 em_length

// Mirror the CLI's register_standard_bindings (same native allowlist). The
// stub must register every native the script uses so the loader can resolve
// the .em's symbolic native bindings by name. Registering all standard
// extensions means any script that compiles with the CLI loads with the stub.
static void register_standard_bindings(
        std::unordered_map<std::string, ember::NativeSig>& natives) {
    using namespace ember;
    ext_vec::register_natives(natives); ext_quat::register_natives(natives);
    ext_mat::register_natives(natives); ext_string::register_natives(natives);
    ext_array::register_natives(natives); ext_math::register_natives(natives);
    ext_map::register_natives(natives);
    ext_sync::register_natives(natives); ext_lifecycle::register_natives(natives);
    ext_io::register_natives(natives);
    ext_call_raw::register_natives(natives);
    ext_coroutine::register_natives(natives);
    ext_visualize::register_natives(natives);
    ext_ui::register_natives(natives);
    ext_graphics::register_natives(natives);
    // Publish overload names into the allowlist (same as the CLI: the .em
    // loader resolves overloads by their sema-resolved fn_name).
    OpOverloadTable overloads;
    ext_vec::register_overloads(overloads); ext_quat::register_overloads(overloads);
    ext_mat::register_overloads(overloads); ext_string::register_overloads(overloads);
    for (const auto& item : overloads.entries) {
        const OpOverload& o = item.second;
        NativeSig sig; sig.name = o.fn_name; sig.fn_ptr = o.fn_ptr;
        sig.ret = o.ret; sig.params = o.params;
        natives[o.fn_name] = std::move(sig);
    }
}

// Find this exe's own path. argv[0] is unreliable (may be a relative path,
// may differ from the actual loaded module path on some platforms).
// On Windows, GetModuleFileNameW gives the definitive answer.
static std::filesystem::path get_own_exe_path() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return std::filesystem::path();
    return std::filesystem::path(buf);
#else
    // POSIX: read /proc/self/exe (Linux) or use argv[0] as a fallback.
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec && !p.empty()) return p;
    return std::filesystem::path();
#endif
}

int main(int argc, char** argv) {
    // 1. Find this exe's own path.
    std::filesystem::path self = get_own_exe_path();
    if (self.empty()) {
        std::fprintf(stderr, "ember_stub: cannot determine own executable path\n");
        return 2;
    }

    // 2. Read the footer (last 12 bytes) + the appended .em.
    std::ifstream f(self, std::ios::binary | std::ios::ate);
    if (!f) {
        std::fprintf(stderr, "ember_stub: cannot open own executable: %s\n",
                     self.string().c_str());
        return 2;
    }
    std::streampos end_pos = f.tellg();
    if (end_pos < 0) {
        std::fprintf(stderr, "ember_stub: cannot determine file size\n");
        return 2;
    }
    uint64_t file_size = static_cast<uint64_t>(end_pos);
    if (file_size < EM_BUNDLE_FOOTER_SIZE) {
        std::fprintf(stderr, "ember_stub: no embedded module (file too small)\n");
        return 2;
    }

    // Read the footer (12 bytes: u32 magic + u64 em_length) in one read so
    // gcount() reflects the full 12 bytes.
    f.seekg(static_cast<std::streamoff>(file_size - EM_BUNDLE_FOOTER_SIZE));
    if (!f) {
        std::fprintf(stderr, "ember_stub: seek to footer failed\n");
        return 2;
    }
    uint8_t footer[EM_BUNDLE_FOOTER_SIZE] = {};
    f.read(reinterpret_cast<char*>(footer), EM_BUNDLE_FOOTER_SIZE);
    if (!f || f.gcount() < static_cast<std::streamsize>(EM_BUNDLE_FOOTER_SIZE)) {
        std::fprintf(stderr, "ember_stub: short footer read\n");
        return 2;
    }
    uint32_t magic = 0;
    uint64_t em_len = 0;
    for (int i = 0; i < 4; ++i)
        magic |= uint32_t(footer[i]) << (8 * i);
    for (int i = 0; i < 8; ++i)
        em_len |= uint64_t(footer[4 + i]) << (8 * i);
    if (magic != EM_BUNDLE_MAGIC) {
        std::fprintf(stderr, "ember_stub: no embedded module (bad footer magic)\n");
        return 2;
    }
    // Subtraction form is deliberate: `footer_size + em_len` can wrap for a
    // malicious all-ones u64 footer. file_size is already known >= footer.
    // The ember::MAX_FILE_SIZE cap is enforced BEFORE the allocation below:
    // em_len is attacker-controlled (read straight from the appended footer)
    // and the loader's own parse_file cap would only fire AFTER the stub had
    // already allocated em_len bytes. Without this gate a malicious footer
    // whose em_len is below size_t/streamsize max but far above the .em cap
    // (e.g. 1 GiB vs the 256 MiB cap) drives an allocation-amplification / OOM
    // DoS before load_em_bytes ever runs. Fail closed before the allocation.
    if (em_len == 0 || em_len > file_size - EM_BUNDLE_FOOTER_SIZE ||
        em_len > ember::MAX_FILE_SIZE ||
        em_len > uint64_t(std::numeric_limits<size_t>::max()) ||
        em_len > uint64_t(std::numeric_limits<std::streamsize>::max())) {
        std::fprintf(stderr, "ember_stub: bad em_length in footer\n");
        return 2;
    }

    // Read the .em (em_len bytes ending just before the footer). Position the
    // file cursor at the start of the em blob first, then contain the
    // allocation: even after the MAX_FILE_SIZE cap a hostile environment (low
    // virtual memory) can still throw std::bad_alloc from the vector ctor, and
    // std::length_error is not possible here (em_len <= size_t max is checked
    // above) but bad_alloc must surface as a clean exit, not terminate.
    f.seekg(static_cast<std::streamoff>(file_size - EM_BUNDLE_FOOTER_SIZE - em_len));
    if (!f) {
        std::fprintf(stderr, "ember_stub: seek to em blob failed\n");
        return 2;
    }
    std::vector<uint8_t> em_bytes;
    try {
        em_bytes.resize(static_cast<size_t>(em_len));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ember_stub: cannot allocate %llu bytes for em blob: %s\n",
                     static_cast<unsigned long long>(em_len), e.what());
        return 2;
    }
    f.read(reinterpret_cast<char*>(em_bytes.data()), static_cast<std::streamsize>(em_len));
    if (!f || static_cast<uint64_t>(f.gcount()) < em_len) {
        std::fprintf(stderr, "ember_stub: short em read\n");
        return 2;
    }
    f.close();

    // 3. Register the native allowlist (same as CLI --load-em).
    std::unordered_map<std::string, ember::NativeSig> natives;
    register_standard_bindings(natives);

    // The bundled program may itself invoke the self-hosted EMBM loader. Give
    // that loader the same allowlisted bindings and permissions as this .em.
    ember::ext_call_raw::set_loader_context(&natives, ember::PERM_FFI);

    // 4. Load the .em from memory.
    // The stub is a trusted host running its own bundled .em (appended at
    // build time, not loaded from an untrusted source), so it grants PERM_FFI
    // and opts in to raw x86 (the bundler emits v3). FIX 3 + Finding B
    // (EM_FORMAT_RED_TEAM 2026-07-11).
    ember::LoadedModule mod;
    std::string lerr;
    ember::EmLoadPolicy em_policy{ember::PERM_FFI, true};
    if (!ember::load_em_bytes(em_bytes.data(), em_bytes.size(), mod, &lerr,
                              nullptr, &natives, nullptr, &em_policy)) {
        std::fprintf(stderr, "ember_stub: load failed: %s\n", lerr.c_str());
        return 2;
    }

    // 5. Find the entry. The bundler persists --fn as mod.entry_slot. A
    // runtime --fn remains available for diagnostics/manual overrides.
    std::string fn_name;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--fn" && i + 1 < argc) {
            fn_name = argv[++i];
        }
    }
    void* entry = fn_name.empty() ? mod.entry() : mod.entry_by_name(fn_name.c_str());
    if (!entry) {
        std::fprintf(stderr, "ember_stub: entry '%s' not found\n",
                     fn_name.empty() ? "<bundled entry>" : fn_name.c_str());
        return 2;
    }

    // 6. Determine the return type (void -> exit 0, i64 -> exit code).
    //    Matches the CLI --load-em contract exactly.
    uint32_t selected_slot = mod.entry_slot;
    for (const auto& item : mod.name_table)
        if (!fn_name.empty() && item.first == fn_name) { selected_slot = item.second; break; }
    bool is_void = selected_slot < mod.signatures_by_slot.size() &&
                   mod.signatures_by_slot[selected_slot].ret.is_void();

    // 7. Initialize runtime services and process-local globals, then call.
    // String global values are handles into ext_string's process-local store,
    // so the bundler emits __globals_init to construct them in this process
    // instead of persisting the bundler process's stale handles in `.em`.
    ember::context_t ectx;
    ectx.budget_remaining = 20000000000;
    ectx.max_call_depth = 512;
    ember::ext_coroutine::coroutine_init(&ectx, mod.dispatch.data(),
                                         int64_t(mod.dispatch.size()));
    if (void* globals_init = mod.entry_by_name("__globals_init"))
        ember::call_i64_i64(globals_init);

    int64_t result = ember::call_i64_i64(entry);
    ember::ext_coroutine::coroutine_reset();
    ember::ext_call_raw::reset();
    ember::ext_graphics::reset();
    return is_void ? 0 : int(result);
}
