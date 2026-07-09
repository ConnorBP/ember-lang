// v0.4_hardening_test - regression tests pinning the red-team V5 (W^X JIT
// memory) and V6 (per-frame byte budget + int32 struct-sizing overflow)
// mitigations. Each red-team payload that previously crashed (SIGSEGV 139 /
// SIGILL 132 / silent 0-byte frame) must now be REJECTED at sema with a clear
// budget error, and the JIT page must be RX not RWX. A legit in-budget script
// must still sema-check OK (negative control).
//
// Self-contained: the malicious .ember payloads are inlined as string
// literals (no external test assets). The sema pipeline mirrors sema_check
// (resolve_imports -> tokenize -> parse -> slot-assign -> register six
// extensions -> build_struct_layouts -> string_xor_key=0 -> sema). The W^X
// probe allocates a JIT page via alloc_executable and queries its protection
// with VirtualQuery (must be PAGE_EXECUTE_READ, not PAGE_EXECUTE_READWRITE).
#include "import.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"
#include "jit_memory.hpp"

#include "ext_vec.hpp"
#include "ext_quat.hpp"
#include "ext_mat.hpp"
#include "ext_string.hpp"
#include "ext_array.hpp"
#include "ext_math.hpp"

#include <cstdio>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace ember;

static int g_fail = 0;
static void check(bool cond, const char* msg) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

// Run parse+sema on an inlined source. Returns true if sema OK, false if
// sema produced errors. Mirrors sema_check's pipeline (extensions registered,
// string_xor_key=0 so no encrypted-rodata path).
static bool sema_ok(const std::string& src) {
    std::unordered_set<std::string> seen;
    std::string resolved;
    try { resolved = resolve_imports(src, "./", seen); }
    catch (const std::exception&) { return false; }
    auto lr = tokenize(resolved, "<v0.4_test>");
    if (!lr.ok) return false;
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) return false;
    std::unordered_map<std::string, int> slots;
    int si = 0;
    for (auto& fn : pr.program.funcs) { slots[fn.name] = si++; fn.slot = slots[fn.name]; }
    std::unordered_map<std::string, NativeSig> natives;
    OpOverloadTable overloads;
    ext_vec::register_natives(natives);    ext_quat::register_natives(natives);
    ext_mat::register_natives(natives);     ext_string::register_natives(natives);
    ext_array::register_natives(natives);   ext_math::register_natives(natives);
    ext_vec::register_overloads(overloads); ext_quat::register_overloads(overloads);
    ext_mat::register_overloads(overloads); ext_string::register_overloads(overloads);
    auto layouts = build_struct_layouts(pr.program);
    pr.program.string_xor_key = 0;
    auto sr = sema(pr.program, natives, slots, 0, &overloads, &layouts);
    return sr.ok;
}

int main() {
    std::printf("=== v0.4 hardening regression (red-team V5 + V6) ===\n");

    // --- V6-DoS: large fixed-array local must be REJECTED at sema ---
    // Red-team payload: `fn main()->i64 { let a: u8[65536]; return 0; }`
    // Pre-fix: SIGSEGV (exit 139), no per-frame cap. Post-fix: sema budget error.
    check(!sema_ok("fn main() -> i64 { let a: u8[65536]; return 0; }\n"),
          "V6-DoS: u8[65536] local rejected at sema (was SIGSEGV)");

    // --- V6-DoS negative control: a small in-budget array must ACCEPT ---
    check(sema_ok("fn main() -> i64 { let a: u8[100]; return a[0]; }\n"),
          "V6-DoS negative control: u8[100] in-budget sema OK");

    // --- V6-overflow: struct field whose byte_size overflows int32 ---
    // Red-team payload: `struct S { big: i64[1073741824]; }` + `let s: S;`
    // Pre-fix: layout.size silently wraps to 0 -> 0-byte frame slot (latent
    // arbitrary write once field-of-struct array indexing ships). Post-fix:
    // any `let` of the struct is rejected by the per-frame budget.
    check(!sema_ok("struct S { big: i64[1073741824]; }\n"
                   "fn main() -> i64 { let s: S; return 0; }\n"),
          "V6-overflow: let of overflowing struct rejected (was silent 0-byte slot)");

    // --- V6-overflow negative control: a normal struct must ACCEPT ---
    check(sema_ok("struct P { x: i32; y: i32; }\n"
                  "fn main() -> i64 { let p: P; return 0; }\n"),
          "V6-overflow negative control: normal struct sema OK");

    // --- V5: W^X JIT memory - page must be RX, not RWX ---
    // Red-team finding: alloc_executable used PAGE_EXECUTE_READWRITE (RWX).
    // Post-fix: VirtualAlloc RW -> memcpy -> VirtualProtect RX. Verify the
    // published page is PAGE_EXECUTE_READ by querying its protection.
    std::vector<uint8_t> code(16, 0xC3); // rets
    void* page = alloc_executable(code);
    MEMORY_BASIC_INFORMATION mbi{};
    bool queried = page && VirtualQuery(page, &mbi, sizeof(mbi)) != 0;
    check(queried, "V5: alloc_executable + VirtualQuery succeeded");
    if (queried) {
        check(mbi.Protect == PAGE_EXECUTE_READ,
              "V5: JIT page is PAGE_EXECUTE_READ (W^X enforced, was RWX)");
        check(mbi.Protect != PAGE_EXECUTE_READWRITE,
              "V5: JIT page is NOT PAGE_EXECUTE_READWRITE (RWX eliminated)");
    }
    if (page) free_executable(page);

    std::printf("\nv0.4 hardening test: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
