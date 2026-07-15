// Focused diagnostic-path coverage for src/sema.cpp.
#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"
#include "codegen.hpp"

#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

using namespace ember;

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

struct Result {
    bool parsed = false;
    SemaResult sema_result;
};

static Result check_source(const std::string& src) {
    Result out;
    auto lr = tokenize(src, "<sema-coverage>");
    if (!lr.ok) { std::fprintf(stderr, "unexpected lex failure: %s\n", lr.error.c_str()); return out; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::fprintf(stderr, "unexpected parse failure: %s\n", pr.error.c_str()); return out; }
    out.parsed = true;
    std::unordered_map<std::string, int> slots;
    int slot = 0;
    for (auto& fn : pr.program.funcs) { fn.slot = slot; slots[fn.name] = slot++; }
    std::unordered_map<std::string, NativeSig> natives;
    OpOverloadTable overloads;
    auto layouts = build_struct_layouts(pr.program);
    out.sema_result = sema(pr.program, natives, slots, 0, &overloads, &layouts);
    return out;
}

static void invalid(const char* name, const std::string& src,
                    std::initializer_list<const char*> fragments) {
    Result r = check_source(src);
    CHECK(r.parsed);
    CHECK(!r.sema_result.ok);
    std::string all;
    for (const auto& e : r.sema_result.errors) all += e.msg + "\n";
    for (const char* f : fragments) {
        if (all.find(f) == std::string::npos)
            std::fprintf(stderr, "%s missing diagnostic '%s'; got:\n%s", name, f, all.c_str());
        CHECK(all.find(f) != std::string::npos);
    }
}

int main() {
    invalid("type mismatch",
        "fn bad(x: i64, y: f32) -> i64 { return x + y; }",
        {"operator requires same-type operands"});
    invalid("undefined and scope",
        "fn bad() -> i64 { { let inner: i64 = 1; } return inner + missing; }",
        {"undefined name 'inner'", "undefined name 'missing'"});
    invalid("same scope redeclaration",
        "fn bad() -> i64 { let x: i64 = 1; let x: i64 = 2; return x; }",
        {"redeclaration"});
    invalid("invalid casts",
        "fn bad() -> i64 { let x: i64 = 1; let s: u8[] = x as u8[]; return 0; }",
        {"invalid cast"});
    invalid("typed enum mismatch",
        "enum Color : i32 { Red, Green } enum Hue : i32 { Red, Green } "
        "fn bad() -> i64 { let c: Color = Hue::Red; return c as i64; }",
        {"let type mismatch"});
    invalid("enum edges",
        "enum E { A, A } fn bad() -> i64 { return E::Missing + Ghost::A; }",
        {"duplicate variant", "has no variant", "unknown enum"});
    invalid("struct fields",
        "struct P { x: i64; y: i64; } fn bad() -> i64 { "
        "let a: P = P { x: 1 }; let b: P = P { x: 1.5f, y: 2 }; return a.z; }",
        {"missing field 'y'", "field 'x' type mismatch", "has no field 'z'"});
    invalid("field on scalar",
        "fn bad() -> i64 { let x: i64 = 1; return x.nope; }",
        {"field access requires a struct type"});
    invalid("match subject and pattern",
        "fn bad() -> i64 { let f: f32 = 1.0f; match (f) { 1 => { return 1; }, _ => { return 0; } } return 0; }",
        {"match subject must be an integer or bool"});
    invalid("lambda arity and argument",
        "fn bad() -> i64 { let f = fn(x: i64) -> i64 { return x; }; "
        "let a = f(1, 2); let b = f(true); return a + b; }",
        {"lambda call has 2 arg", "lambda argument type mismatch"});
    invalid("lambda declared signature",
        "fn add(a: i64, b: i64) -> i64 { return a + b; } "
        "fn bad() -> i64 { let f: fn(i64) -> i64 = &add; return 0; }",
        {"let type mismatch"});
    invalid("coroutine yield mismatch",
        "fn bad() -> i64 { yield 1; yield true; return 0; }",
        {"yield type mismatch"});
    invalid("loop conditions",
        "fn bad() -> i64 { while (1) { break; } for (let i: i64 = 0; i; i = i + 1) { break; } return 0; }",
        {"while condition must be bool", "for cond must be bool"});
    invalid("control scope",
        "fn bad() -> i64 { break; continue; return 0; }",
        {"break is only valid", "continue is only valid"});
    invalid("return coverage",
        "fn bad(x: bool) -> i64 { if (x) { return 1; } }",
        {"not all paths return"});
    invalid("multiple recovery",
        "fn bad() -> i64 { const x: i64 = 5; x = 6; if (x) { return 1; } return x + 1.5f; }",
        {"const", "condition must be bool", "operator requires same-type operands"});

    Result good = check_source(
        "enum E : i32 { A, B } struct P { x: i64; y: i64; } "
        "fn generator() -> i64 { yield 1; yield 2; return 3; } "
        "fn main() -> i64 { let p: P = P { x: 1, y: 2 }; let e: E = E::A; "
        "let base: i64 = p.x; let f = fn(x: i64) -> i64 { return x + base; }; "
        "match (e) { E::A => { return f(41); }, _ => { return 0; } } return 0; }");
    CHECK(good.parsed && good.sema_result.ok);

    std::puts(failures ? "sema coverage: FAIL" : "sema coverage: PASS");
    return failures != 0;
}
