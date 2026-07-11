// sema_check - parse + sema check exe for the lang regression suite.
//
// Reads a .ember file from argv[1], resolves imports, tokenizes, parses,
// assigns per-function dispatch slots, registers the SIX standard ember
// extensions (vec/quat/mat/string/array/math) into a NativeSig map + the four
// overload-bearing extensions' OpOverloadTable entries (vec/quat/mat/string),
// builds struct layouts, sets string_xor_key=0xA5, and runs sema. Exits 0 if
// sema OK, nonzero with the errors on stderr.
//
// The extension registration is EXACTLY ext_registration_test.cpp's lines
// 1-120 shape: ext_*::register_natives for all six, ext_*::register_overloads
// for the four that have operator overloads. A script that uses array/string
// natives (sema_valid_recovery_mix's default-param + struct-by-value, etc.)
// sema-checks against THIS registered set, not prism's process/render natives.
// Links ember + ember_frontend + ember_import + all six ember_ext_* so the
// registered natives resolve at link time.
#include "import.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"

#include "ext_vec.hpp"
#include "ext_quat.hpp"
#include "ext_mat.hpp"
#include "ext_string.hpp"
#include "ext_array.hpp"
#include "ext_math.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: sema_check <file.ember>\n");
        return 2;
    }

    std::ifstream f(argv[1]);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string src = ss.str();

    std::string base_dir = std::filesystem::path(argv[1]).parent_path().string();
    std::unordered_set<std::string> seen;
    try {
        src = ember::resolve_imports(src, base_dir, seen);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "IMPORT_ERROR: %s\n", e.what());
        return 1;
    }

    auto lr = ember::tokenize(src, argv[1]);
    if (!lr.ok) {
        std::fprintf(stderr, "LEX_ERROR: %s\n", lr.error.c_str());
        return 1;
    }

    auto pr = ember::parse(std::move(lr.toks));
    if (!pr.ok) {
        std::fprintf(stderr, "PARSE_ERROR:\n%s", pr.error.c_str());
        return 1;
    }

    // per-function dispatch slots (mirror em_roundtrip_test / a host builder).
    std::unordered_map<std::string, int> slots;
    int slot = 0;
    for (auto& fn : pr.program.funcs) {
        slots[fn.name] = slot++;
    }

    // register the SIX standard extensions into one native table, exactly as
    // ext_registration_test.cpp does (lines 1-120: register_natives for all
    // six, register_overloads for the four with operator overloads).
    std::unordered_map<std::string, ember::NativeSig> natives;
    ember::ext_vec::register_natives(natives);
    ember::ext_quat::register_natives(natives);
    ember::ext_mat::register_natives(natives);
    ember::ext_string::register_natives(natives);
    ember::ext_array::register_natives(natives);
    ember::ext_math::register_natives(natives);

    // Slice-escape safety Stage 2 (C3) test surface: a RETAINING native. No
    // shipped native retains a slice ptr (string_from_slice copies), so C3 is
    // "accidentally safe" in production. To exercise the C3 guard's reject
    // path (a stack-backed slice passed to a retains=true native -> sema
    // error), register a test-only retaining native here. It takes a u8[]
    // slice and is flagged retains=true; sema then rejects a stack-backed
    // slice arg to it (sema_invalid_local_slice_native_retain.ember). The fn
    // ptr is never called (sema_check runs sema only, no execution), so a
    // stub null ptr is fine — only the signature + retains flag matter.
    {
        ember::NativeSig s;
        s.name = "test_retain_slice";
        s.fn_ptr = nullptr;
        s.ret = ember::type_i64();
        s.params.push_back(ember::make_slice(std::make_shared<ember::Type>(ember::make_prim(ember::Prim::U8))));
        s.permission = 0;
        s.retains = true;
        natives["test_retain_slice"] = std::move(s);
    }

    ember::OpOverloadTable overloads;
    ember::ext_vec::register_overloads(overloads);
    ember::ext_quat::register_overloads(overloads);
    ember::ext_mat::register_overloads(overloads);
    ember::ext_string::register_overloads(overloads);

    auto struct_layouts = ember::build_struct_layouts(pr.program);
    pr.program.string_xor_key = 0xA5;

    auto sr = ember::sema(pr.program, natives, slots, 0, &overloads, &struct_layouts);
    if (!sr.ok) {
        std::fprintf(stderr, "SEMA_ERROR (%zu):\n", sr.errors.size());
        for (auto& e : sr.errors) {
            std::fprintf(stderr, "  line %u:%u  %s\n", e.line, e.col, e.msg.c_str());
        }
        return 1;
    }

    std::printf("OK: %zu funcs type-checked\n", pr.program.funcs.size());
    return 0;
}
