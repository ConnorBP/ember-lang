// dbg_lambda_test.cpp - TEMPORARY debug harness: dump thin IR + ARM64 bytes
// for valid_lambda.ember to diagnose the by-value capture crash.
#include "../src/engine.hpp"
#include "../src/dispatch_table.hpp"
#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"
#include "../src/codegen.hpp"
#include "../src/globals.hpp"
#include "../src/binding_builder.hpp"
#include "../src/thin_lower.hpp"
#include "../src/thin_emit.hpp"
#include "../src/thin_ir.hpp"

#include "../extensions/string/ext_string.hpp"
#include "../extensions/array/ext_array.hpp"
#include "../extensions/gc/ext_gc.hpp"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using namespace ember;

static void build_natives(NativeTable& nt) {
    ext_string::register_natives(nt.natives);
    ext_array::register_natives(nt.natives);
    ext_string::register_overloads(nt.overloads);
    for (const auto& item : nt.overloads.entries) {
        const OpOverload& o = item.second;
        NativeSig sig; sig.name = o.fn_name; sig.fn_ptr = o.fn_ptr;
        sig.ret = o.ret; sig.params = o.params;
        nt.natives[o.fn_name] = std::move(sig);
    }
}

int main() {
    const char* src =
        "fn main() -> i64 {\n"
        "    let captured_var: i64 = 40;\n"
        "    let f = fn(x: i64) -> i64 { return x + captured_var; };\n"
        "    return f(2);\n"
        "}\n";

    auto lr = tokenize(src, "<dbg>");
    if (!lr.ok) { std::fprintf(stderr, "lex fail: %s\n", lr.error.c_str()); return 1; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::fprintf(stderr, "parse fail: %s\n", pr.error.c_str()); return 1; }
    Program prog = std::move(pr.program);
    int si = 0;
    std::unordered_map<std::string, int> slots;
    for (auto& fn : prog.funcs) { slots[fn.name] = si++; fn.slot = si - 1; }
    NativeTable nt; build_natives(nt);
    auto layouts = build_struct_layouts(prog);
    prog.string_xor_key = 0xA5;
    auto sr = sema(prog, nt.natives, slots, 0, &nt.overloads, &layouts);
    if (!sr.ok) {
        std::fprintf(stderr, "sema fail:\n");
        for (auto& e : sr.errors) std::fprintf(stderr, "  %u: %s\n", e.line, e.msg.c_str());
        return 1;
    }

    CodeGenCtx ctx;
    ctx.natives = &nt.natives;
    ctx.script_slots = &slots;
    ctx.structs = &layouts;
    ctx.enable_ir_backend = true;
    // match ember_cli run defaults: use_context_reg=true, use_gc_env=false
    ctx.use_context_reg = true;
    ctx.use_gc_env = false;

    for (auto& fn : prog.funcs) {
        std::printf("===== fn %s (slot=%u, is_lambda=%d) =====\n",
                    fn.name.c_str(), fn.slot, int(fn.is_lambda));
        if (fn.is_lambda) {
            std::printf("  captures: ");
            for (size_t i = 0; i < fn.lambda_captures.size(); ++i) {
                std::printf("[%s off=%d by_ref=%d] ",
                            fn.lambda_captures[i].c_str(),
                            fn.lambda_capture_offsets[i],
                            int(i < fn.lambda_capture_by_ref.size() && fn.lambda_capture_by_ref[i]));
            }
            std::printf("\n");
        }
        ThinFunction thf = lower_function(fn, ctx);
        std::printf("---- thin IR ----\n");
        std::printf("%s\n", dump(thf).c_str());
        std::printf("---- emit arm64 ----\n");
        try {
            CompiledFn cf = emit_arm64(thf, ctx);
            std::printf("  bytes (%zu):", cf.bytes.size());
            for (size_t b = 0; b < cf.bytes.size(); ++b) {
                if (b % 16 == 0) std::printf("\n  %04zx:", b);
                std::printf(" %02x", cf.bytes[b]);
            }
            std::printf("\n");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "emit failed: %s\n", e.what());
        }
        std::printf("\n");
    }
    return 0;
}
