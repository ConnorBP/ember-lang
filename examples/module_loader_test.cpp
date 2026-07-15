// module_loader_test - runtime coverage for the EMBM v1 module image loader
// (extensions/call_raw/ext_call_raw.cpp: load_executable_module /
// module_entry_ptr / free_executable_module; spec:
// self_hosted/MODULE_IMAGE_FORMAT.md §2/§7/§8/§9).
//
// This test hand-builds a minimal EMBM v1 image as a byte vector and exercises
// the loader through the native function pointers directly (the same shape as
// call_raw_test [1]) so it tests the LOADER LOGIC, not the JIT binding:
//
//   [A] Happy path: a tiny x64 function that (a) materializes a rodata string
//       slice {ptr,len} via an ABS64_RODATA reloc, (b) calls a real allowlisted
//       native via ABS64_NATIVE, (c) reads + writes a mutable global via
//       ABS64_DATA, (d) returns an i64. Loaded via load_executable_module,
//       entry via module_entry_ptr, invoked via call_raw. Two calls verify the
//       DATA write persisted across calls (mutable global). Asserts the return
//       matches the expected value computed from rodata byte + data global +
//       native result.
//   [B] Rejection paths (each must return 0 / be rejected, no crash):
//       - unknown native symbol
//       - ABI fingerprint mismatch (wrong addend)
//       - relocation patch_off out of bounds
//       - overlapping relocations
//       - truncated image (shorter than header)
//       - oversize section (code_len > 16 MiB cap)
//       - unknown magic / unknown version
//   [C] Handle lifetime: free then module_entry_ptr returns 0 (invalidated);
//       double-free is a no-op (no crash); module_entry_ptr out-of-code-bounds
//       returns 0; unknown handle returns 0.
//
// Links ember + ember_frontend + ember_ext_call_raw + ember_ext_array (the
// loader reads the image via ext_array::get_bytes / mint the image handle via
// ext_array::alloc_bytes), same as the existing call_raw_test wiring.

#include "../src/module_abi_fingerprint.hpp"   // abi_fingerprint (mirror the loader's §8)
#include "../src/binding_builder.hpp"          // BindingBuilder, PERM_FFI
#include "../src/sema.hpp"                     // NativeSig
#include "../extensions/call_raw/ext_call_raw.hpp"
#include "../extensions/array/ext_array.hpp"

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// ---- little-endian writers (the image is LE per §1) ----
void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x & 0xFF));
    v.push_back(uint8_t((x >> 8) & 0xFF));
    v.push_back(uint8_t((x >> 16) & 0xFF));
    v.push_back(uint8_t((x >> 24) & 0xFF));
}
void put_u64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(uint8_t((x >> (8 * i)) & 0xFF));
}

// A test native: add_one(x: i64) -> i64 { return x + 1; }. Registered into the
// natives map so ABS64_NATIVE relocs can resolve "add_one" against it. Its ABI
// fingerprint is computed by the shared abi_fingerprint() (the same function
// the loader uses), so the happy-path image's NATIVE reloc addend matches.
int64_t test_add_one(int64_t x) { return x + 1; }

// ---- the hand-assembled x64 code section ----
// entry(arg: i64 in rcx) -> i64 in rax, Win64 ABI.
//
//   rodata = "AB" (2 bytes); rodata[0] = 'A' = 65.
//   data   = one i64 global, init 100.
//   return = add_one(arg) + data_global_read + rodata[0]
//          = (arg + 1) + 100 + 65 = arg + 166        (first call)
//   also writes (arg + 1000) to data_global (exercises the DATA write through
//   the relocated address); the second call reads the persisted 1007.
//
// Three ABS64 relocations patch imm64 slots:
//   patch_off=18  ABS64_RODATA  -> rodata_base + 0   (the slice ptr; len=2 imm)
//   patch_off=37  ABS64_DATA    -> data_base  + 0   (the global address)
//   patch_off=65  ABS64_NATIVE  -> add_one fn ptr   (fingerprint in addend)
//
// Returns the code bytes AND, via out params, the three reloc patch offsets so
// the caller can build the reloc records without hardcoding magic numbers.
std::vector<uint8_t> build_entry_code(uint64_t& rodata_patch,
                                      uint64_t& data_patch,
                                      uint64_t& native_patch) {
    std::vector<uint8_t> c;
    auto b = [&](uint8_t x) { c.push_back(x); };
    // 0:  push rbp
    b(0x55);
    // 1:  mov rbp, rsp
    b(0x48); b(0x89); b(0xE5);
    // 4:  push rbx
    b(0x53);
    // 5:  push r12
    b(0x41); b(0x54);
    // 7:  push r13
    b(0x41); b(0x55);
    // 9:  sub rsp, 40          (32 shadow + 8 pad -> 16-aligned for the call)
    b(0x48); b(0x83); b(0xEC); b(0x28);
    // 13: mov rbx, rcx         (rbx = arg, callee-saved across the native call)
    b(0x48); b(0x89); b(0xCB);
    // 16: mov rax, imm64       <ABS64_RODATA>  (rodata ptr)
    b(0x48); b(0xB8);
    rodata_patch = c.size();                 // imm64 starts here = 18
    for (int i = 0; i < 8; ++i) b(0x00);
    // 26: mov edx, 2           (len = 2; zero-extends to rdx)
    b(0xBA); b(0x02); b(0x00); b(0x00); b(0x00);
    // 31: movzx r12, byte [rax]  (r12 = rodata[0] = 'A' = 65; callee-saved)
    b(0x4C); b(0x0F); b(0xB6); b(0x20);
    // 35: mov r13, imm64       <ABS64_DATA>  (data global address)
    b(0x49); b(0xBD);
    data_patch = c.size();                   // = 37
    for (int i = 0; i < 8; ++i) b(0x00);
    // 45: mov r8, [r13]        (r8 = data_global read; volatile -> save before call)
    b(0x4D); b(0x8B); b(0x45); b(0x00);
    // 49: lea r9, [rbx + 1000] (r9 = arg + 1000)
    b(0x4C); b(0x8D); b(0x8B); b(0xE8); b(0x03); b(0x00); b(0x00);
    // 56: mov [r13], r9        (data_global = arg + 1000; exercises DATA write)
    b(0x4D); b(0x89); b(0x4D); b(0x00);
    // 60: mov r13, r8          (r13 = data_global read; callee-saved across call)
    b(0x4D); b(0x89); b(0xC5);
    // 63: mov rax, imm64       <ABS64_NATIVE>  (add_one fn ptr)
    b(0x48); b(0xB8);
    native_patch = c.size();                 // = 65
    for (int i = 0; i < 8; ++i) b(0x00);
    // 73: call rax             (add_one(arg) -> rax = arg + 1; rcx still = arg)
    b(0xFF); b(0xD0);
    // 75: add rax, r13         (+ data_global read)
    b(0x4C); b(0x01); b(0xE8);
    // 78: add rax, r12         (+ rodata[0])
    b(0x4C); b(0x01); b(0xE0);
    // 81: add rsp, 40
    b(0x48); b(0x83); b(0xC4); b(0x28);
    // 85: pop r13
    b(0x41); b(0x5D);
    // 87: pop r12
    b(0x41); b(0x5C);
    // 89: pop rbx
    b(0x5B);
    // 90: pop rbp
    b(0x5D);
    // 91: ret
    b(0xC3);
    return c;                                // length 92
}

// Build a full EMBM v1 image from sections + symbols + relocs.
// relocs: vector of (type, patch_off, sym_idx, addend), 32 bytes each.
std::vector<uint8_t> build_image(const std::vector<uint8_t>& code,
                                 const std::vector<uint8_t>& rodata,
                                 const std::vector<uint8_t>& data,
                                 const std::string& syms,        // concatenated null-terminated names
                                 const std::vector<std::array<uint64_t,4>>& relocs) {
    std::vector<uint8_t> img;
    const uint64_t header = 80;
    uint64_t code_off   = header;
    uint64_t rodata_off = code_off + code.size();
    uint64_t data_off   = rodata_off + rodata.size();
    uint64_t syms_off   = data_off + data.size();
    uint64_t syms_len   = syms.size();
    uint64_t reloc_count = relocs.size();

    // header
    put_u32(img, 0x4D424D45u);                 // magic "EMBM" LE -> 45 4D 42 4D
    put_u32(img, 1);                            // version
    put_u64(img, code_off);   put_u64(img, code.size());
    put_u64(img, rodata_off); put_u64(img, rodata.size());
    put_u64(img, data_off);   put_u64(img, data.size());
    put_u64(img, syms_off);   put_u64(img, syms_len);
    put_u64(img, reloc_count);
    // sections
    img.insert(img.end(), code.begin(), code.end());
    img.insert(img.end(), rodata.begin(), rodata.end());
    img.insert(img.end(), data.begin(), data.end());
    img.insert(img.end(), syms.begin(), syms.end());
    // relocations (32 bytes each)
    for (auto& r : relocs) {
        put_u64(img, r[0]);   // type
        put_u64(img, r[1]);   // patch_off
        put_u64(img, r[2]);   // sym_idx
        put_u64(img, r[3]);   // addend
    }
    return img;
}

} // namespace

int main() {
    using namespace ember;
    int failures = 0;
    auto passfail = [&](bool ok) { return ok ? "PASS" : "FAIL"; };

    // ---- register the natives the loader resolves against ----
    // call_raw extension natives (call_raw / make_executable / free_executable_ptr
    // + the three loader natives) + the test's add_one.
    std::unordered_map<std::string, NativeSig> natives;
    ext_call_raw::register_natives(natives);
    {
        BindingBuilder b;
        b.add("add_one", type_i64(), {type_i64()}, (void*)&test_add_one, /*perm=*/0);
        NativeTable t = b.build();
        for (auto& kv : t.natives) natives[kv.first] = std::move(kv.second);
    }
    // Host wiring: the loader resolves ABS64_NATIVE relocs against this table,
    // and the calling module is granted PERM_FFI (so it may call the FFI-gated
    // loader native itself; add_one is permission 0 so always allowed).
    ext_call_raw::set_loader_context(&natives, PERM_FFI);

    // Native function pointers (call the loader logic directly, like
    // call_raw_test [1]).
    using LoadFn   = int64_t(*)(int64_t);
    using EntryFn  = int64_t(*)(int64_t, int64_t);
    using FreeFn   = void(*)(int64_t);
    using CallRaw  = int64_t(*)(int64_t, int64_t);
    auto load_fn  = reinterpret_cast<LoadFn>(natives["load_executable_module"].fn_ptr);
    auto entry_fn = reinterpret_cast<EntryFn>(natives["module_entry_ptr"].fn_ptr);
    auto free_fn  = reinterpret_cast<FreeFn>(natives["free_executable_module"].fn_ptr);
    auto callraw  = reinterpret_cast<CallRaw>(natives["call_raw"].fn_ptr);

    // ---- build the happy-path image ----
    uint64_t rp = 0, dp = 0, np = 0;
    std::vector<uint8_t> code = build_entry_code(rp, dp, np);
    std::vector<uint8_t> rodata = {0x41, 0x42};          // "AB"
    std::vector<uint8_t> data(8, 0);
    { int64_t g = 100; std::memcpy(data.data(), &g, 8); } // global init = 100
    std::string syms = std::string("add_one") + '\0';    // symbol 0 = "add_one"
    uint64_t add_one_fp = abi_fingerprint(natives["add_one"]);  // mirror the loader's §8
    std::vector<std::array<uint64_t,4>> relocs = {
        {1, rp, 0, 0},           // ABS64_RODATA, addend 0
        {2, dp, 0, 0},           // ABS64_DATA,   addend 0
        {3, np, 0, add_one_fp},  // ABS64_NATIVE, sym 0, fingerprint addend
    };
    std::vector<uint8_t> img = build_image(code, rodata, data, syms, relocs);

    // =====================================================================
    // [A] Happy path: load, entry, call (twice for DATA persistence), free.
    // =====================================================================
    int64_t img_h = ext_array::alloc_bytes(img.data(), int64_t(img.size()));
    if (img_h == 0) { std::printf("[A] FAIL: alloc image handle\n"); failures++; }
    else {
        int64_t h = load_fn(img_h);
        if (h == 0) {
            std::printf("[A] FAIL: load_executable_module returned 0\n"); failures++;
        } else {
            int64_t entry = entry_fn(h, 0);          // entry at code offset 0
            if (entry == 0) { std::printf("[A] FAIL: module_entry_ptr(0) == 0\n"); failures++; }
            else {
                int64_t r1 = callraw(entry, 7);
                // add_one(7)=8 + data_read(100) + rodata[0](65) = 173
                bool ok1 = (r1 == 173);
                std::printf("[A] call#1 entry(7) == 173 : %s (got %lld)\n",
                            passfail(ok1), (long long)r1);
                if (!ok1) failures++;
                int64_t r2 = callraw(entry, 7);
                // second call: data_read persisted = 1007; 8 + 1007 + 65 = 1080
                bool ok2 = (r2 == 1080);
                std::printf("[A] call#2 entry(7) == 1080 (DATA persisted) : %s (got %lld)\n",
                            passfail(ok2), (long long)r2);
                if (!ok2) failures++;
            }
            // [C] handle lifetime: entry out of bounds / unknown
            int64_t oob = entry_fn(h, int64_t(code.size()));     // == code_len -> reject
            int64_t neg = entry_fn(h, -1);                       // negative -> reject
            int64_t bad = entry_fn(999, 0);                      // unknown handle -> 0
            bool okoob = (oob == 0), okneg = (neg == 0), okbad = (bad == 0);
            std::printf("[C] entry_ptr(oob==code_len)==0 : %s\n", passfail(okoob)); if (!okoob) failures++;
            std::printf("[C] entry_ptr(-1)==0           : %s\n", passfail(okneg)); if (!okneg) failures++;
            std::printf("[C] entry_ptr(unknown)==0      : %s\n", passfail(okbad)); if (!okbad) failures++;

            free_fn(h);
            // entry invalidated after free
            int64_t after_free = entry_fn(h, 0);
            bool okinv = (after_free == 0);
            std::printf("[C] entry_ptr after free == 0 : %s\n", passfail(okinv)); if (!okinv) failures++;
            // double-free is a no-op (must not crash)
            free_fn(h);
            free_fn(0);                 // freeing the null handle is a no-op
            free_fn(12345);             // freeing an unknown handle is a no-op
            std::printf("[C] double-free / unknown-free no crash : PASS\n");
        }
    }

    // =====================================================================
    // [B] Rejection paths: each variant image must be rejected (load -> 0),
    //     with no crash and no partial state.
    // =====================================================================
    auto expect_reject = [&](const char* label, const std::vector<uint8_t>& image) {
        int64_t ah = ext_array::alloc_bytes(image.data(), int64_t(image.size()));
        int64_t h = (ah != 0) ? load_fn(ah) : 0;
        bool ok = (h == 0);
        std::printf("[B] reject %-28s : %s\n", label, passfail(ok));
        if (!ok) failures++;
        if (h != 0) free_fn(h);   // clean up if the loader wrongly accepted
    };

    // (1) unknown native symbol
    {
        std::string s = std::string("does_not_exist") + '\0';
        std::vector<std::array<uint64_t,4>> rs = {
            {3, np, 0, add_one_fp},  // NATIVE referencing the unknown symbol
        };
        expect_reject("unknown native symbol", build_image(code, rodata, data, s, rs));
    }
    // (2) ABI fingerprint mismatch (wrong addend)
    {
        std::vector<std::array<uint64_t,4>> rs = {
            {3, np, 0, add_one_fp + 1},  // wrong fingerprint
        };
        expect_reject("ABI fingerprint mismatch", build_image(code, rodata, data, syms, rs));
    }
    // (3) relocation patch_off out of bounds (== code_len)
    {
        std::vector<std::array<uint64_t,4>> rs = {
            {1, uint64_t(code.size()), 0, 0},  // patch_off == code_len -> +8 > code_len
        };
        expect_reject("reloc patch_off OOB", build_image(code, rodata, data, syms, rs));
    }
    // (4) overlapping relocations (two 8-byte slots that overlap)
    {
        std::vector<std::array<uint64_t,4>> rs = {
            {1, rp, 0, 0},       // patch_off 18..25
            {2, rp + 2, 0, 0},   // patch_off 20..27 -> overlaps 18+8=26 > 20
        };
        expect_reject("overlapping relocations", build_image(code, rodata, data, syms, rs));
    }
    // (5) truncated image (shorter than the 80-byte header)
    {
        std::vector<uint8_t> short_img(40, 0);
        expect_reject("truncated image (< header)", short_img);
    }
    // (6) oversize section (code_len claims 32 MiB > 16 MiB cap)
    {
        std::vector<uint8_t> bad = img;   // start from a valid image
        // overwrite code_len (offset 16) with 32 MiB
        uint64_t big = 32ull * 1024 * 1024;
        for (int i = 0; i < 8; ++i) bad[16 + i] = uint8_t((big >> (8 * i)) & 0xFF);
        expect_reject("oversize section (>16MiB)", bad);
    }
    // (7) unknown magic
    {
        std::vector<uint8_t> bad = img;
        bad[0] = 'X';   // break the magic
        expect_reject("unknown magic", bad);
    }
    // (8) unknown version
    {
        std::vector<uint8_t> bad = img;
        bad[4] = 2;     // version 2
        expect_reject("unknown version", bad);
    }
    // (9) overlapping section offsets (rodata_off inside the code section)
    {
        std::vector<uint8_t> bad = img;
        // rodata_off is at offset 24; set it to code_off (80) so rodata overlaps code
        uint64_t overlap = 80;
        for (int i = 0; i < 8; ++i) bad[24 + i] = uint8_t((overlap >> (8 * i)) & 0xFF);
        expect_reject("overlapping section offsets", bad);
    }
    // (10) addend out of bounds (RODATA addend >= rodata_len)
    {
        std::vector<std::array<uint64_t,4>> rs = {
            {1, rp, 0, 100},  // addend 100 >= rodata_len 2
        };
        expect_reject("RODATA addend OOB", build_image(code, rodata, data, syms, rs));
    }

    ext_call_raw::reset();   // frees any live modules + clears loader context

    if (failures == 0) { std::printf("\nmodule_loader: PASS\n"); return 0; }
    std::printf("\nmodule_loader: FAIL (%d)\n", failures);
    return 1;
}
