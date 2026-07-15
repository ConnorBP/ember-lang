// Focused edge and error-path coverage for src/lexer.cpp.
#include "lexer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ember;

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

static bool has(const LexResult& r, Tk k) {
    return std::any_of(r.toks.begin(), r.toks.end(), [=](const Token& t) { return t.kind == k; });
}
static const Token* first(const LexResult& r, Tk k) {
    auto it = std::find_if(r.toks.begin(), r.toks.end(), [=](const Token& t) { return t.kind == k; });
    return it == r.toks.end() ? nullptr : &*it;
}
static void rejects(const std::string& source, const char* fragment) {
    auto r = tokenize(source, "<lexer-coverage>");
    CHECK(!r.ok);
    CHECK(r.err_line != 0 && r.err_col != 0);
    CHECK(r.error.find(fragment) != std::string::npos);
}

int main() {
    auto empty = tokenize("", "<empty>");
    CHECK(empty.ok && empty.toks.size() == 1 && empty.toks[0].kind == Tk::Eof);

    const std::string keywords =
        "fn struct global let mut const constexpr defer priv if else while do for switch case default "
        "break continue return as auto in try catch throw yield true false new delete sizeof offsetof "
        "link enum match static_assert namespace bool i8 i16 i32 i64 u8 u16 u32 u64 f32 f64 void";
    auto kw = tokenize(keywords);
    CHECK(kw.ok);
    CHECK(has(kw, Tk::Kw_fn) && has(kw, Tk::Kw_namespace) && has(kw, Tk::Kw_void));
    CHECK(std::count_if(kw.toks.begin(), kw.toks.end(), [](const Token& t) { return t.kind == Tk::BoolLit; }) == 2);

    const std::string operators =
        "(){}[],;: :: -> => . .. + - * / % = == != < <= > >= && || ! & | ^ ~ << >> ++ -- ? "
        "+= -= *= /= %= &= |= ^= <<= >>= @";
    auto op = tokenize(operators);
    CHECK(op.ok);
    for (Tk k : {Tk::LParen, Tk::RParen, Tk::DoubleColon, Tk::Arrow, Tk::FatArrow,
                 Tk::DotDot, Tk::AndAnd, Tk::OrOr, Tk::ShlAssign, Tk::ShrAssign, Tk::At})
        CHECK(has(op, k));

    auto nums = tokenize("0 18446744073709551615 0x2a 0XFF 1.25 2. 3e2 4E-2 5f 6.5F 7..9");
    CHECK(nums.ok);
    CHECK(nums.toks[1].ivalue == -1);
    CHECK(nums.toks[2].ivalue == 42 && nums.toks[3].ivalue == 255);
    CHECK(std::count_if(nums.toks.begin(), nums.toks.end(), [](const Token& t) { return t.kind == Tk::FloatLit; }) == 6);
    CHECK(has(nums, Tk::DotDot));
    const Token* f32 = first(nums, Tk::FloatLit);
    CHECK(f32 && std::fabs(f32->fvalue - 1.25) < 1e-12);

    auto strings = tokenize(
        "\"a\\n\\t\\r\\\\\\\"\\0b\" "
        "\"line one\\\nline two\" "
        "r\"\"\"raw\\path\nsecond line\"\"\" "
        "f\"escaped {{brace}} value={1 + {2}} nested={\"x\\n\"}\"");
    CHECK(strings.ok);
    CHECK(has(strings, Tk::StringLit) && has(strings, Tk::RawStringLit) && has(strings, Tk::FStringLit));
    CHECK(first(strings, Tk::RawStringLit)->text.find("raw\\path") != std::string::npos);
    CHECK(first(strings, Tk::FStringLit)->text.find("{1 + {2}}") != std::string::npos);

    std::string long_id(20000, 'a'); long_id[0] = '_';
    auto lid = tokenize(long_id);
    CHECK(lid.ok && lid.toks[0].kind == Tk::Ident && lid.toks[0].text == long_id);

    auto comments = tokenize("/* outer /* nested marker */ tail */ // line\nidentifier");
    // Block comments are deliberately non-nesting: the first close ends it.
    CHECK(comments.ok && has(comments, Tk::Ident));

    rejects("/* never closes", "unterminated block comment");
    rejects("\"never closes", "unterminated string literal");
    rejects("\"bad\\q\"", "unknown escape");
    rejects("\"first\nsecond\"", "unescaped newline");
    rejects("f\"never closes", "unterminated f-string");
    rejects("f\"x={\"never closes", "unterminated string literal inside f-string");
    rejects("r\"\"\"never closes", "unterminated raw string literal");
    rejects("0x", "requires at least one digit");
    rejects("0x12z", "malformed hex literal suffix");
    rejects("12u64", "width suffixes are unsupported");
    rejects("18446744073709551616", "out of range");
    rejects("#", "unexpected character");

    // Exercise every spelling switch arm, including keyword/default arms.
    for (unsigned i = 0; i <= static_cast<unsigned>(Tk::At); ++i)
        CHECK(tok_spelling(static_cast<Tk>(i)) != nullptr);

    std::puts(failures ? "lexer coverage: FAIL" : "lexer coverage: PASS");
    return failures != 0;
}
