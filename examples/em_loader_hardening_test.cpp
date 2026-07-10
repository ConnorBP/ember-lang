// Malformed `.em` boundary regressions for C1/C2/D8.
#include "em_file.hpp"
#include "em_loader.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace ember;

static void u16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8));
}
static void u32(std::vector<uint8_t>& b, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
}
static void header(std::vector<uint8_t>& b, uint32_t fns, uint32_t globals,
                   uint32_t rodata, uint32_t entry) {
    u32(b, EM_MAGIC); u32(b, EM_VERSION); u32(b, 0); u32(b, fns);
    u32(b, globals); u32(b, rodata); u32(b, entry);
    u32(b, 0); u32(b, 0); u32(b, 0);
}
static void fn(std::vector<uint8_t>& b, uint32_t slot, uint32_t code_size = 1,
               uint32_t reloc_offset = 0, int reloc_kind = -1,
               uint32_t rodata_size = 0) {
    u16(b, 1); b.push_back('f'); u32(b, slot); u32(b, code_size); u32(b, rodata_size);
    for (uint32_t i = 0; i < code_size; ++i) b.push_back(i + 1 == code_size ? 0xc3 : 0x90);
    for (uint32_t i = 0; i < rodata_size; ++i) b.push_back(0);
    u32(b, reloc_kind < 0 ? 0 : 1);
    if (reloc_kind >= 0) { u32(b, reloc_offset); b.push_back(uint8_t(reloc_kind)); }
}
static void names(std::vector<uint8_t>& b, uint32_t slot) {
    u32(b, 1); u16(b, 1); b.push_back('f'); u32(b, slot);
}

static int failures = 0;
static std::filesystem::path temp_path(const char* tag) {
    return std::filesystem::temp_directory_path() /
           (std::string("ember_em_hardening_") + tag + ".em");
}
static void expect_reject(const char* tag, const std::vector<uint8_t>& bytes) {
    const auto path = temp_path(tag);
    { std::ofstream os(path, std::ios::binary); os.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); }
    LoadedModule out;
    out.dispatch.push_back(reinterpret_cast<void*>(uintptr_t(1)));
    out.globals.push_back(42);
    out.name_table.push_back({"old", 0});
    out.entry_slot = 0;
    std::string err;
    bool ok = false;
    bool threw = false;
    const auto started = std::chrono::steady_clock::now();
    try { ok = load_em_file(path.string().c_str(), out, &err); }
    catch (...) { threw = true; }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    std::filesystem::remove(path);
    bool unchanged = out.pages.empty() && out.dispatch.size() == 1 &&
                     out.dispatch[0] == reinterpret_cast<void*>(uintptr_t(1)) &&
                     out.globals == std::vector<uint8_t>{42} &&
                     out.name_table.size() == 1 && out.name_table[0].first == "old" &&
                     out.entry_slot == 0;
    if (ok || threw || err.empty() || err.rfind("em_loader:", 0) != 0 || !unchanged ||
        elapsed > std::chrono::seconds(2)) {
        std::printf("FAIL %s: ok=%d threw=%d err=%s unchanged=%d bounded=%d\n", tag,
                    int(ok), int(threw), err.c_str(), int(unchanged),
                    int(elapsed <= std::chrono::seconds(2)));
        ++failures;
    }
}

int main() {
    static_assert(!std::is_copy_constructible_v<LoadedModule> &&
                  !std::is_copy_assignable_v<LoadedModule> &&
                  std::is_nothrow_move_constructible_v<LoadedModule> &&
                  std::is_nothrow_move_assignable_v<LoadedModule>);
    { std::vector<uint8_t> b; header(b, MAX_FUNCTIONS + 1, 0, 0, EM_NO_ENTRY); expect_reject("huge_functions", b); }
    { std::vector<uint8_t> b; header(b, 0, MAX_GLOBALS + 1, 0, EM_NO_ENTRY); expect_reject("huge_globals", b); }
    { std::vector<uint8_t> b; header(b, 0, 0, uint32_t(MAX_FILE_SIZE + 1), EM_NO_ENTRY); expect_reject("huge_rodata_total", b); }
    { std::vector<uint8_t> b; header(b, 1, 0, 0, 0); u16(b, 1); b.push_back('f'); u32(b, 0); u32(b, MAX_CODE_PER_FN + 1); u32(b, 0); expect_reject("huge_code", b); }
    { std::vector<uint8_t> b; header(b, 1, 0, 0, 0); u16(b, 1); b.push_back('f'); u32(b, 0); u32(b, 1); u32(b, MAX_RODATA_PER_FN + 1); expect_reject("huge_rodata", b); }
    { std::vector<uint8_t> b; header(b, 1, 0, 0, 0); fn(b, MAX_SLOTS); names(b, 0); expect_reject("huge_slot", b); }
    { std::vector<uint8_t> b; header(b, 1, 0, 0, 0); u16(b, MAX_NAME_SIZE + 1); expect_reject("huge_function_name", b); }
    { std::vector<uint8_t> b; header(b, 1, 0, 0, 0); u16(b, 1); b.push_back('f'); u32(b, 0); u32(b, 8); u32(b, 0); for (int i=0;i<8;++i)b.push_back(0); u32(b, MAX_RELOCS_PER_FN + 1); expect_reject("huge_reloc_count", b); }
    { std::vector<uint8_t> b; header(b, 1, 0, 0, 0); fn(b, 0); u32(b, MAX_NAMES + 1); expect_reject("huge_name_count", b); }
    { std::vector<uint8_t> b; header(b, 1, 0, 0, 0); fn(b, 0, 8, 0xfffffffc, EmReloc::DispatchTableBase); names(b, 0); expect_reject("reloc_wrap", b); }
    { std::vector<uint8_t> b; header(b, 1, 0, 0, 0); fn(b, 0, 8, 1, EmReloc::DispatchTableBase); names(b, 0); expect_reject("reloc_short_slot", b); }
    { std::vector<uint8_t> b; header(b, 1, 0, 0, 0); fn(b, 0, 7, 0, EmReloc::DispatchTableBase); names(b, 0); expect_reject("reloc_code_too_short", b); }
    { std::vector<uint8_t> b; header(b, 1, 0, 0, 0); fn(b, 0, 8, 0, 99); names(b, 0); expect_reject("reloc_kind", b); }
    { std::vector<uint8_t> b; header(b, 2, 0, 0, 0); fn(b, 0); fn(b, 0); names(b, 0); expect_reject("duplicate_slot", b); }
    { std::vector<uint8_t> b; header(b, 1, 0, 0, 1); fn(b, 0); names(b, 0); expect_reject("bad_entry", b); }
    { std::vector<uint8_t> b; header(b, 1, 0, 0, 0); fn(b, 0); names(b, 1); expect_reject("bad_name_slot", b); }
    { std::vector<uint8_t> b; header(b, 1, 0, 0, 0); fn(b, 0); u32(b, 1); u16(b, MAX_NAME_SIZE + 1); expect_reject("huge_directory_name", b); }
    { std::vector<uint8_t> b; header(b, 1, 0, 0, 0); fn(b, 0); u32(b, 1); u16(b, 2); b.push_back('f'); expect_reject("truncated_directory", b); }
    { std::vector<uint8_t> b; header(b, 1, 0, 0, 0); fn(b, 0); expect_reject("missing_directory", b); }
    { std::vector<uint8_t> b; header(b, 1, 0, 0, 0); fn(b, 0); names(b, 0); b.push_back(0); expect_reject("trailing", b); }
    { std::vector<uint8_t> b(EM_HEADER_SIZE - 1, 0); expect_reject("truncated_header", b); }
    { std::vector<uint8_t> b; header(b, 1, 0, 0, 0); u16(b, 1); b.push_back('f'); expect_reject("truncated_function", b); }
    { std::vector<uint8_t> b; header(b, 1, 2, 0, 0); fn(b, 0); b.push_back(1); expect_reject("truncated_globals", b); }

    // Friendly record also proves serialized initialized global bytes survive.
    {
        std::vector<uint8_t> b; header(b, 1, 3, 0, 0); fn(b, 0);
        b.push_back(7); b.push_back(8); b.push_back(9); names(b, 0);
        const auto path = temp_path("globals");
        { std::ofstream os(path, std::ios::binary); os.write(reinterpret_cast<const char*>(b.data()), b.size()); }
        LoadedModule out; std::string err;
        bool ok = load_em_file(path.string().c_str(), out, &err);
        std::filesystem::remove(path);
        if (!ok || out.globals != std::vector<uint8_t>({7, 8, 9}) || !out.entry()) {
            std::printf("FAIL globals: ok=%d err=%s\n", int(ok), err.c_str());
            ++failures;
        } else {
            void* entry = out.entry();
            LoadedModule moved(std::move(out));
            LoadedModule destination;
            destination = std::move(moved);
            if (out.entry() || moved.entry() || destination.entry() != entry ||
                destination.pages.size() != 1) {
                std::printf("FAIL move ownership\n");
                ++failures;
            }
        }
    }

    std::printf("em loader hardening: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
