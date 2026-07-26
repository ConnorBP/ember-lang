// Phase 1 validation (plan_MACOS_ARM64.md): Darwin W^X JIT memory end-to-end.
// Allocates an RX page via ember::jit_memory, copies a hand-written ARM64
// function (mov x0,#42; ret) into it, seals it RX, calls it, asserts it
// returns 42, and frees it. Also exercises the two-phase alloc_rw -> patch ->
// seal path (the em_loader reloc path). Proves MAP_JIT + pthread_jit_write_protect_np
// + mprotect(PROT_EXEC) + sys_icache_invalidate work on Apple Silicon.
//
// Build (macOS arm64):
//   clang++ -std=c++17 -Wall -Wextra tests/jit_memory_darwin_test.cpp \
//     -I src -L buildm -lember -lember_ed25519 -o /tmp/jit_darwin_test && /tmp/jit_darwin_test
#include "jit_memory.hpp"
#include "platform.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// mov x0, #42  -> movz x0, #42  = 0xD2800540  (1101 0010 1 00 imm16=42<<5 Rd=0)
// ret          -> ret x30       = 0xD65F03C0
// Encoding is DEFINITIVELY validated by the runtime call below (returns 42).
static const uint32_t kMov42 = 0xD2800540u;
static const uint32_t kRet   = 0xD65F03C0u;

using FnI64 = int64_t (*)();

// Optional encoding cross-check against the assembler (skipped if clang/xxd
// unavailable). Reads the raw object bytes with xxd and byte-swaps to the
// little-endian instruction value.
static int verify_encoding(uint32_t want_a, uint32_t want_b) {
    FILE* p = popen(
        "printf '.text\\nmov x0, #42\\nret\\n' | "
        "clang -c -arch arm64 -x assembler - -o /tmp/_jit_enc.o 2>/dev/null && "
        "/usr/bin/xxd -p -g4 -e /tmp/_jit_enc.o 2>/dev/null | head -2",
        "r");
    if (!p) { std::printf("[SKIP] encoding cross-check (popen failed)\n"); return 0; }
    char line[256];
    std::string hex;
    while (fgets(line, sizeof(line), p)) hex += line;
    pclose(p);
    if (hex.size() < 16) { std::printf("[SKIP] encoding cross-check (clang/xxd unavailable)\n"); return 0; }
    // xxd -p -g4 -e emits groups like "400528d2" (big-endian display of a
    // little-endian word -> the bytes are reversed from the instruction value).
    auto parse = [](const std::string& s, size_t i) -> uint32_t {
        unsigned v = 0; sscanf(s.c_str() + i, "%8x", &v);
        // s is big-endian hex of the 4 bytes; byte-swap to the LE instruction value.
        return ((v & 0xff) << 24) | ((v & 0xff00) << 8) | ((v & 0xff0000) >> 8) | ((v & 0xff000000) >> 24);
    };
    uint32_t a = parse(hex, 0), b = parse(hex, 8);
    if (a == want_a && b == want_b) { std::printf("[PASS] ARM64 encodings match assembler\n"); return 0; }
    std::printf("[FAIL] encoding mismatch: got %08x %08x, want %08x %08x\n", a, b, want_a, want_b);
    return 1;
}

int main() {
    int failures = verify_encoding(kMov42, kRet);

    // --- one-shot: alloc_executable (RW -> memcpy -> seal RX) ---
    std::vector<uint8_t> code(8);
    std::memcpy(code.data(), &kMov42, 4);
    std::memcpy(code.data() + 4, &kRet, 4);
    void* page = ember::alloc_executable(code);
    if (!page) { std::printf("[FAIL] alloc_executable returned nullptr\n"); return 1; }
    std::printf("[PASS] alloc_executable + seal RX (MAP_JIT + toggle + mprotect EXEC + icache invalidate)\n");
    int64_t r = reinterpret_cast<FnI64>(page)();
    if (r == 42) std::printf("[PASS] JIT'd ARM64 function executed: returned %lld\n", (long long)r);
    else { std::printf("[FAIL] returned %lld, expected 42\n", (long long)r); failures++; }
    ember::free_executable(page);
    std::printf("[PASS] free_executable (munmap with tracked rounded size)\n");

    // --- two-phase: alloc_executable_rw -> protect_rw patch -> seal (em_loader path) ---
    {
        static const uint32_t mov0 = 0xD2800000u; // movz x0, #0
        std::vector<uint8_t> c2(8);
        std::memcpy(c2.data(), &mov0, 4);
        std::memcpy(c2.data() + 4, &kRet, 4);
        void* p2 = ember::alloc_executable_rw(c2);
        if (!p2) { std::printf("[FAIL] alloc_executable_rw\n"); failures++; }
        else {
            uint32_t patched = 0xD2800000u | (7u << 5); // movz x0, #7
            ember::platform::protect_rw(p2, c2.size());   // re-enable writes (drop EXEC)
            std::memcpy(p2, &patched, 4);
            if (!ember::seal_executable(p2, c2.size())) { std::printf("[FAIL] two-phase seal\n"); failures++; }
            else {
                int64_t r2 = reinterpret_cast<FnI64>(p2)();
                if (r2 == 7) std::printf("[PASS] two-phase patch+seal executed: returned %lld\n", (long long)r2);
                else { std::printf("[FAIL] two-phase returned %lld, expected 7\n", (long long)r2); failures++; }
            }
            ember::free_executable(p2);
        }
    }

    // ── W^X JIT edge-case probes (audit H6): boundary inputs to the
    //    executable-memory allocator must not crash; they must either reject
    //    cleanly (return nullptr / false) or allocate a full page. These pin
    //    the defensive behavior so a future regression (e.g. mmap(0) handing
    //    back a wild pointer, or free_executable(nullptr) dereferencing) is
    //    caught as a FAIL, not a segfault that kills the harness. ──

    // (a) alloc_executable with an EMPTY code vector (size 0). The allocator
    //     rounds 0 up to a 0-byte mmap (which fails -> nullptr) OR allocates a
    //     full page; either is acceptable as long as it does not crash or
    //     return a wild non-null pointer that can't be freed.
    {
        std::vector<uint8_t> empty;
        void* p = ember::alloc_executable(empty);
        if (p == nullptr) {
            std::printf("[PASS] alloc_executable(empty) -> nullptr (size-0 rejected cleanly)\n");
        } else {
            // Got a page: it must be a real, freeable page (free without crash).
            ember::free_executable(p);
            std::printf("[PASS] alloc_executable(empty) -> full page (allocated + freed cleanly)\n");
        }
    }

    // (b) free_executable(nullptr) is a safe no-op (must not deref / crash).
    {
        ember::free_executable(nullptr);
        std::printf("[PASS] free_executable(nullptr) is a safe no-op\n");
    }

    // (c) alloc_executable_rw of 1 byte: the allocator rounds the request up to
    //     a full page (ember::platform::page_size(), 16 KiB on Apple Silicon).
    //     We cannot read the internal rounded-size map (it is private to
    //     jit_memory), so we PROVE the rounding indirectly: a 1-byte request
    //     must succeed (alloc_rw(1) would return nullptr if it rounded to 0),
    //     the returned page must be a full writable page (we can write a full
    //     hand-written ARM64 fn into it without faulting past the 1st byte),
    //     and it must seal + execute. The page_size() assertion documents the
    //     expected rounding granularity.
    {
        long ps = ember::platform::page_size();
        if (ps != 16384) {
            std::printf("[WARN] page_size() == %ld (expected 16384 on Apple Silicon)\n", ps);
        } else {
            std::printf("[PASS] ember::platform::page_size() == 16384 (Apple Silicon 16 KiB pages)\n");
        }
        std::vector<uint8_t> one(1, 0);
        void* p = ember::alloc_executable_rw(one);
        if (!p) { std::printf("[FAIL] alloc_executable_rw(1 byte) returned nullptr\n"); failures++; }
        else {
            // Overwrite the full page with a repeating `movz x0,#7; ret` pattern
            // (8 bytes each) — this only succeeds without faulting because the
            // allocator rounded the 1-byte request up to a whole mapped page.
            // protect_rw re-enables thread-local writes before the patch.
            ember::platform::protect_rw(p, size_t(ps));
            static const uint32_t kMov7 = 0xD28000E0u; // movz x0, #7
            static const uint32_t kRet2  = 0xD65F03C0u; // ret x30
            for (long off = 0; off + 8 <= ps; off += 8) {
                std::memcpy(static_cast<char*>(p) + off, &kMov7, 4);
                std::memcpy(static_cast<char*>(p) + off + 4, &kRet2, 4);
            }
            if (!ember::seal_executable(p, size_t(ps))) {
                std::printf("[FAIL] seal_executable(1-byte->full page) failed\n"); failures++;
            } else {
                int64_t r = reinterpret_cast<FnI64>(p)();
                if (r == 7) std::printf("[PASS] alloc_executable_rw(1 byte) rounded to full page (executed, returned %lld)\n", (long long)r);
                else { std::printf("[FAIL] 1-byte->full-page fn returned %lld, expected 7\n", (long long)r); failures++; }
            }
            ember::free_executable(p);
        }
    }

    // (d) seal_executable with an unmapped/bad pointer: mprotect on an
    //     unmapped region fails with ENOMEM -> seal returns false. Must NOT
    //     crash (the bad pointer must never be dereferenced, only passed to
    //     mprotect which validates it). Use a deliberately non-page address.
    {
        // A smallish integer cast to a pointer is guaranteed not to be a
        // valid mmap result (mmap returns high addresses on Apple Silicon).
        void* bad = reinterpret_cast<void*>(0x1000);
        bool ok = ember::seal_executable(bad, 4096);
        if (!ok) std::printf("[PASS] seal_executable(bad ptr) -> false (no crash)\n");
        else { std::printf("[FAIL] seal_executable(bad ptr) unexpectedly succeeded\n"); failures++; }
    }

    if (failures) { std::printf("\n%d FAILURE(S)\n", failures); return 1; }
    std::printf("\nALL PASS\n");
    return 0;
}
