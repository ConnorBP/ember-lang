// ember lexer - full v1 token set per docs/spec/COMPILER_PIPELINE.md Section 1.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ember {

enum class Tk : uint16_t {
    Eof,
    // literals
    IntLit, FloatLit, StringLit, FStringLit, RawStringLit, BoolLit,
    Ident,
    // keywords
    Kw_fn, Kw_struct, Kw_global, Kw_let, Kw_mut, Kw_const, Kw_constexpr, Kw_defer, Kw_priv,
    Kw_if, Kw_else, Kw_while, Kw_do, Kw_for, Kw_switch, Kw_case, Kw_default,
    Kw_break, Kw_continue, Kw_return, Kw_as, Kw_auto, Kw_in,
    Kw_try, Kw_catch, Kw_throw,
    Kw_true, Kw_false,
    Kw_sizeof, Kw_offsetof,
    Kw_link, Kw_enum, Kw_match, Kw_static_assert, Kw_namespace,
    // primitive types
    Kw_bool, Kw_i8, Kw_i16, Kw_i32, Kw_i64,
    Kw_u8, Kw_u16, Kw_u32, Kw_u64, Kw_f32, Kw_f64, Kw_void,
    // punctuation
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Comma, Semicolon, Colon, DoubleColon, Arrow, FatArrow, Dot, DotDot,
    // operators
    Plus, Minus, Star, Slash, Percent, Assign,
    Eq, Neq, Lt, Le, Gt, Ge, AndAnd, OrOr, Not,
    Amp, Pipe, Caret, Tilde, Shl, Shr,
    Inc, Dec, Question,
    PlusAssign, MinusAssign, StarAssign, SlashAssign, PercentAssign,
    // bitwise / shift compound assignments (so @obf("mba") is testable on
    // the compound-assignment path for every MBA-supported op).
    AmpAssign, PipeAssign, CaretAssign, ShlAssign, ShrAssign,
    // annotation
    At,
};

struct Token {
    Tk       kind;
    std::string text;       // identifier name / raw literal text / keyword spelling
    uint32_t line;
    uint32_t col;
    // decoded literal values (only meaningful for the matching Lit kind)
    int64_t  ivalue = 0;
    double   fvalue = 0;
    bool     bvalue = false;
    bool     f32_suffix = false;  // float literal had `f`/`F` suffix -> f32
    // Integer width suffixes are intentionally unsupported in v1; the lexer
    // rejects them rather than consuming syntax without semantic metadata.
};

struct LexResult {
    std::vector<Token> toks;
    bool ok = true;
    std::string error;
    // Position of `error` (1-based, same scheme as Token::line/col) - 0/0 if
    // ok is true. Lets a caller (e.g. the code editor's inline error display)
    // show a lex failure at the exact source line, not just as flat text.
    uint32_t err_line = 0;
    uint32_t err_col = 0;
};

// Tokenize `src`. On error, returns ok=false with an error message and
// the tokens parsed so far (best-effort continuation, docs/spec/COMPILER_PIPELINE.md Section 1).
LexResult tokenize(std::string_view src, const char* filename = "<script>");

const char* tok_spelling(Tk k);

} // namespace ember
