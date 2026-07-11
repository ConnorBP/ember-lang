#include "lexer.hpp"
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace ember {

static const std::unordered_map<std::string, Tk>& keywords() {
    static const std::unordered_map<std::string, Tk> m = {
        {"fn",Tk::Kw_fn},{"struct",Tk::Kw_struct},{"global",Tk::Kw_global},
        {"let",Tk::Kw_let},{"mut",Tk::Kw_mut},
        {"const",Tk::Kw_const},{"constexpr",Tk::Kw_constexpr},{"defer",Tk::Kw_defer},
        {"priv",Tk::Kw_priv},{"static_assert",Tk::Kw_static_assert},{"namespace",Tk::Kw_namespace},
        {"if",Tk::Kw_if},{"else",Tk::Kw_else},{"while",Tk::Kw_while},{"do",Tk::Kw_do},
        {"for",Tk::Kw_for},{"switch",Tk::Kw_switch},{"case",Tk::Kw_case},{"default",Tk::Kw_default},
        {"break",Tk::Kw_break},{"continue",Tk::Kw_continue},{"return",Tk::Kw_return},
        {"as",Tk::Kw_as},{"auto",Tk::Kw_auto},{"in",Tk::Kw_in},
        {"try",Tk::Kw_try},{"catch",Tk::Kw_catch},{"throw",Tk::Kw_throw},
        {"yield",Tk::Kw_yield},
        {"true",Tk::Kw_true},{"false",Tk::Kw_false},
        {"sizeof",Tk::Kw_sizeof},{"offsetof",Tk::Kw_offsetof},
        {"link",Tk::Kw_link},
        {"enum",Tk::Kw_enum},
        {"match",Tk::Kw_match},
        {"bool",Tk::Kw_bool},
        {"i8",Tk::Kw_i8},{"i16",Tk::Kw_i16},{"i32",Tk::Kw_i32},{"i64",Tk::Kw_i64},
        {"u8",Tk::Kw_u8},{"u16",Tk::Kw_u16},{"u32",Tk::Kw_u32},{"u64",Tk::Kw_u64},
        {"f32",Tk::Kw_f32},{"f64",Tk::Kw_f64},{"void",Tk::Kw_void},
    };
    return m;
}

const char* tok_spelling(Tk k) {
    switch (k) {
    case Tk::Eof: return "eof";
    case Tk::IntLit: return "int"; case Tk::FloatLit: return "float";
    case Tk::StringLit: return "string"; case Tk::FStringLit: return "fstring";
    case Tk::RawStringLit: return "rawstring";
    case Tk::BoolLit: return "bool"; case Tk::Ident: return "ident";
    case Tk::LParen: return "("; case Tk::RParen: return ")";
    case Tk::LBrace: return "{"; case Tk::RBrace: return "}";
    case Tk::LBracket: return "["; case Tk::RBracket: return "]";
    case Tk::Comma: return ","; case Tk::Semicolon: return ";";
    case Tk::Colon: return ":"; case Tk::DoubleColon: return "::"; case Tk::Arrow: return "->"; case Tk::FatArrow: return "=>";
    case Tk::Dot: return "."; case Tk::DotDot: return "..";
    case Tk::Plus: return "+"; case Tk::Minus: return "-";
    case Tk::Star: return "*"; case Tk::Slash: return "/";
    case Tk::Percent: return "%"; case Tk::Assign: return "=";
    case Tk::Eq: return "=="; case Tk::Neq: return "!=";
    case Tk::Lt: return "<"; case Tk::Le: return "<=";
    case Tk::Gt: return ">"; case Tk::Ge: return ">=";
    case Tk::AndAnd: return "&&"; case Tk::OrOr: return "||"; case Tk::Not: return "!";
    case Tk::Amp: return "&"; case Tk::Pipe: return "|";
    case Tk::Caret: return "^"; case Tk::Tilde: return "~";
    case Tk::Shl: return "<<"; case Tk::Shr: return ">>";
    case Tk::Inc: return "++"; case Tk::Dec: return "--";
    case Tk::Question: return "?";
    case Tk::PlusAssign: return "+="; case Tk::MinusAssign: return "-=";
    case Tk::StarAssign: return "*="; case Tk::SlashAssign: return "/=";
    case Tk::PercentAssign: return "%="; case Tk::At: return "@";
    case Tk::AmpAssign: return "&="; case Tk::PipeAssign: return "|=";
    case Tk::CaretAssign: return "^="; case Tk::ShlAssign: return "<<="; case Tk::ShrAssign: return ">>=";
    case Tk::Kw_enum: return "enum";
    case Tk::Kw_match: return "match";
    case Tk::Kw_priv: return "priv";
    case Tk::Kw_static_assert: return "static_assert";
    case Tk::Kw_namespace: return "namespace";
    case Tk::Kw_try: return "try";
    case Tk::Kw_catch: return "catch";
    case Tk::Kw_throw: return "throw";
    case Tk::Kw_yield: return "yield";
    default: return "kw";
    }
}

LexResult tokenize(std::string_view src, const char*) {
    LexResult r;
    size_t i = 0;
    uint32_t line = 1, col = 1;
    auto adv = [&](size_t n){ i += n; col += uint32_t(n); };
    auto push = [&](Tk k, std::string t){ r.toks.push_back({k, std::move(t), line, col}); };

    while (i < src.size()) {
        char c = src[i];
        // whitespace
        if (c == ' ' || c == '\t' || c == '\r') { adv(1); continue; }
        if (c == '\n') { ++line; col = 1; ++i; continue; }
        // line comment
        if (c == '/' && i + 1 < src.size() && src[i+1] == '/') {
            while (i < src.size() && src[i] != '\n') { ++i; ++col; }
            continue;
        }
        // block comment
        if (c == '/' && i + 1 < src.size() && src[i+1] == '*') {
            adv(2);
            while (i + 1 < src.size() && !(src[i] == '*' && src[i+1] == '/')) {
                if (src[i] == '\n') { ++line; col = 1; } else { ++col; }
                ++i;
            }
            if (i + 1 < src.size()) adv(2); else { r.ok = false; r.error = "unterminated block comment"; r.err_line = line; r.err_col = col; return r; }
            continue;
        }
        // annotation @
        if (c == '@') { push(Tk::At, "@"); adv(1); continue; }
        // f-string: brace-depth + quote-aware body scan (Item D). Checked
        // BEFORE the identifier branch below - 'f' is alphabetic, so without
        // this ordering the identifier loop would greedily consume just the
        // bare letter 'f' as its own Ident token and leave the following
        // '"..."' as a completely separate (and un-triggered) plain string
        // literal, meaning f-strings would never actually reach this branch
        // at all (this exact bug existed until Item D first exercised it -
        // f-strings were lexed-but-dead-code before, so nothing had ever hit
        // this ordering issue). At depth 0 (scanning literal text) a bare
        // '"' is the real terminator and a doubled '{{'/'}}' collapses to
        // one literal brace. At depth > 0 (inside an interpolation) a '"'
        // starts a NESTED plain string literal that must be scanned to its
        // own matching close quote (mirroring the plain-StringLit branch
        // further below) before resuming the outer scan - otherwise a nested
        // string's own quote would wrongly terminate the whole f-string
        // early. The token's `.text` stays raw/un-decoded throughout
        // (including nested-string escapes) - the parser's split step
        // re-lexes each interpolation span from scratch, so decoding here
        // too would double-unescape.
        if (c == 'f' && i + 1 < src.size() && src[i+1] == '"') {
            uint32_t sl = line, sc = col; adv(2);
            std::string s;
            int brace_depth = 0;
            for (;;) {
                if (i >= src.size()) { r.ok = false; r.error = "unterminated f-string"; r.err_line = sl; r.err_col = sc; return r; }
                char ch = src[i];
                if (brace_depth == 0) {
                    if (ch == '"') break; // real terminator
                    if (ch == '{') {
                        if (i + 1 < src.size() && src[i+1] == '{') { s.push_back('{'); adv(2); continue; }
                        brace_depth = 1; s.push_back(ch); adv(1); continue;
                    }
                    if (ch == '}') {
                        if (i + 1 < src.size() && src[i+1] == '}') { s.push_back('}'); adv(2); continue; }
                        s.push_back(ch); adv(1); continue; // unmatched '}' at depth 0 - parser's split step reports this
                    }
                    s.push_back(ch); (ch == '\n' ? (++line, col=1) : ++col); ++i; continue;
                }
                // brace_depth > 0: inside an interpolated expression
                if (ch == '"') {
                    s.push_back(ch); adv(1);
                    while (i < src.size() && src[i] != '"') {
                        if (src[i] == '\\' && i + 1 < src.size()) { s.push_back(src[i]); s.push_back(src[i+1]); adv(2); continue; }
                        s.push_back(src[i]); (src[i] == '\n' ? (++line, col=1) : ++col); ++i;
                    }
                    if (i >= src.size()) { r.ok = false; r.error = "unterminated string literal inside f-string"; r.err_line = sl; r.err_col = sc; return r; }
                    s.push_back('"'); adv(1);
                    continue;
                }
                if (ch == '{') { ++brace_depth; s.push_back(ch); adv(1); continue; }
                if (ch == '}') { --brace_depth; s.push_back(ch); adv(1); continue; }
                s.push_back(ch); (ch == '\n' ? (++line, col=1) : ++col); ++i;
            }
            adv(1); // closing outer "
            r.toks.push_back({Tk::FStringLit, s, sl, sc});
            continue;
        }
        // raw triple-quoted string: r"""...""" - every byte between the
        // delimiters is taken completely literally (no escape processing at
        // all, not even for a backslash), closing only at the next literal
        // '"""'. This is the opt-in for real embedded newlines/backslashes:
        // a PLAIN string below now requires an explicit '\' line-
        // continuation to span more than one physical line, so anything that
        // actually wants free-form multi-line text (or a run of literal
        // backslashes, e.g. a Windows path) reaches for this instead. Checked
        // before the identifier branch below - 'r' is alphabetic, so without
        // this ordering the identifier loop would just consume 'r' as its
        // own Ident token and leave the '"""...' behind as unrelated tokens.
        if (c == 'r' && i + 3 < src.size() && src[i+1] == '"' && src[i+2] == '"' && src[i+3] == '"') {
            uint32_t sl = line, sc = col; adv(4);
            std::string s;
            while (i < src.size() &&
                   !(src[i] == '"' && i + 2 < src.size() && src[i+1] == '"' && src[i+2] == '"')) {
                s.push_back(src[i]); (src[i] == '\n' ? (++line, col=1) : ++col); ++i;
            }
            if (i + 2 >= src.size() || src[i] != '"' || src[i+1] != '"' || src[i+2] != '"') {
                r.ok = false; r.error = "unterminated raw string literal"; r.err_line = sl; r.err_col = sc; return r;
            }
            adv(3); // closing """
            r.toks.push_back({Tk::RawStringLit, s, sl, sc});
            continue;
        }
        // identifier / keyword
        if (std::isalpha((unsigned char)c) || c == '_') {
            uint32_t sl = line, sc = col; size_t s = i;
            while (i < src.size() && (std::isalnum((unsigned char)src[i]) || src[i] == '_')) adv(1);
            std::string id(src.substr(s, i - s));
            auto it = keywords().find(id);
            Tk k = (it != keywords().end()) ? it->second : Tk::Ident;
            Token t{k, id, sl, sc};
            if (k == Tk::Kw_true)  { t.bvalue = true;  t.kind = Tk::BoolLit; }
            if (k == Tk::Kw_false) { t.bvalue = false; t.kind = Tk::BoolLit; }
            r.toks.push_back(std::move(t));
            continue;
        }
        // number (int or float, with optional type suffix; hex with 0x prefix)
        if (std::isdigit((unsigned char)c)) {
            uint32_t sl = line, sc = col; size_t s = i;
            bool isfloat = false;
            // hex prefix 0x / 0X
            if (c == '0' && i + 1 < src.size() && (src[i+1] == 'x' || src[i+1] == 'X')) {
                adv(2);
                size_t digits = i;
                while (i < src.size() && std::isxdigit((unsigned char)src[i])) adv(1);
                if (i == digits) {
                    r.ok = false; r.error = "hex literal requires at least one digit"; r.err_line = sl; r.err_col = sc; return r;
                }
                if (i < src.size() && (std::isalnum((unsigned char)src[i]) || src[i] == '_')) {
                    r.ok = false; r.error = "unsupported or malformed hex literal suffix"; r.err_line = sl; r.err_col = sc; return r;
                }
                std::string num(src.substr(s, i - s));
                Token t{Tk::IntLit, num, sl, sc};
                try {
                    size_t used = 0;
                    unsigned long long value = std::stoull(num.substr(2), &used, 16);
                    if (used != num.size() - 2) throw std::invalid_argument("trailing hex input");
                    t.ivalue = int64_t(value);
                } catch (const std::exception&) {
                    r.ok = false; r.error = "integer literal out of range or malformed: " + num; r.err_line = sl; r.err_col = sc; return r;
                }
                r.toks.push_back(std::move(t));
                continue;
            }
            while (i < src.size() && std::isdigit((unsigned char)src[i])) adv(1);
            if (i < src.size() && src[i] == '.') {
                // distinguish '.' (float) from '..' (slice view) - only consume if not '..'
                if (i + 1 < src.size() && src[i+1] == '.') {
                    // '..' belongs to the next token (slice view); do not consume here
                } else {
                    isfloat = true; adv(1);
                    while (i < src.size() && std::isdigit((unsigned char)src[i])) adv(1);
                }
            }
            // exponent e[+-]?digits
            if (!isfloat && i < src.size() && (src[i] == 'e' || src[i] == 'E')) {
                size_t j = i + 1;
                if (j < src.size() && (src[j] == '+' || src[j] == '-')) ++j;
                if (j < src.size() && std::isdigit((unsigned char)src[j])) {
                    isfloat = true; i = j; col += uint32_t(j - s);
                    while (i < src.size() && std::isdigit((unsigned char)src[i])) adv(1);
                }
            }
            std::string num(src.substr(s, i - s));
            // Integer-width suffixes are not represented in the AST/type
            // system yet. Reject them consistently for decimal and hex rather
            // than silently consuming and discarding their meaning.
            if (!isfloat && i < src.size() && (src[i] == 'u' || src[i] == 'i')) {
                r.ok = false; r.error = "integer literal width suffixes are unsupported; use an explicit `as` cast";
                r.err_line = sl; r.err_col = sc; return r;
            }
            bool f32_suf = false;
            // 'f' suffix forces f32
            if (i < src.size() && (src[i] == 'f' || src[i] == 'F')) { isfloat = true; f32_suf = true; adv(1); }
            Token t{isfloat ? Tk::FloatLit : Tk::IntLit, num, sl, sc};
            t.f32_suffix = f32_suf;
            if (isfloat) {
                // strip any 'f' already consumed into num
                std::string clean = num;
                try {
                    t.fvalue = std::stod(clean);
                } catch (const std::exception&) {
                    r.ok = false; r.error = "floating-point literal out of range or malformed: " + num; r.err_line = sl; r.err_col = sc; return r;
                }
            } else {
                try {
                    // Unsigned parse for the full 64-bit range, same as the hex
                    // path above: a decimal literal has no sign of its own here
                    // (unary minus is a separate token), so any digit run up to
                    // u64::MAX (18446744073709551615) is a legal IntLit - e.g. a
                    // `let x: u64 = 18446744073709551615;` literal. stoll alone
                    // only covers up to i64::MAX and throws std::out_of_range,
                    // uncaught, past that - which used to crash the whole
                    // process (std::terminate) on this exact kind of literal.
                    t.ivalue = int64_t(std::stoull(num, nullptr, 10));
                } catch (const std::exception&) {
                    r.ok = false; r.error = "integer literal out of range or malformed: " + num; r.err_line = sl; r.err_col = sc; return r;
                }
            }
            r.toks.push_back(std::move(t));
            continue;
        }
        // string literal - single physical line only, unless a line ends in
        // '\' (line continuation: shorthand for writing an actual '\n' -
        // inserts a real newline into the decoded value, just spelled as a
        // trailing backslash + a real source line break instead of the two
        // characters '\' 'n'). A bare, un-escaped newline with no preceding
        // backslash is still a lex error rather than being silently absorbed
        // into the string: previously a plain string could span any number
        // of physical lines just by containing a real newline byte, which
        // made a single missing/misplaced closing quote silently swallow the
        // rest of the file as "string content" instead of failing right
        // where the mistake was. Something that wants free-form embedded
        // newlines without an explicit '\' on every line should reach for a
        // raw string (r"""...""" above) instead.
        if (c == '"') {
            uint32_t sl = line, sc = col; adv(1);
            std::string s;
            while (i < src.size() && src[i] != '"') {
                if (src[i] == '\n') {
                    r.ok = false;
                    r.error = "unescaped newline in string literal - end the line with '\\' to continue it, or use a raw string r\"\"\"...\"\"\" for literal embedded newlines";
                    r.err_line = sl; r.err_col = sc; return r;
                }
                if (src[i] == '\\') {
                    adv(1);
                    if (i >= src.size()) break;
                    char e = src[i];
                    if (e == '\n') { s.push_back('\n'); ++line; col = 1; ++i; continue; }  // line continuation: same as an explicit \n
                    // Reject unknown escape sequences rather than silently
                    // dropping the backslash and keeping the raw character
                    // (previously `"\q"` silently became `"q"`, masking a
                    // likely typo such as `\t` misspelled as `\q`). Only the
                    // six documented escapes (n t r \ " 0) plus the line-
                    // continuation form above are valid in a plain string. A
                    // raw string r"""...""" takes every byte literally if a
                    // literal backslash+letter is actually wanted.
                    uint32_t esc_col = col;
                    adv(1);
                    switch (e) {
                    case 'n': s.push_back('\n'); break;
                    case 't': s.push_back('\t'); break;
                    case 'r': s.push_back('\r'); break;
                    case '\\': s.push_back('\\'); break;
                    case '"': s.push_back('"'); break;
                    case '0': s.push_back('\0'); break;
                    default:
                        r.ok = false;
                        r.error = std::string("unknown escape sequence '\\") + e + "' in string literal (valid: \\n \\t \\r \\\\ \\\" \\0; use a raw string for a literal backslash)";
                        r.err_line = line; r.err_col = esc_col;
                        return r;
                    }
                } else { s.push_back(src[i]); ++col; ++i; }
            }
            if (i >= src.size()) { r.ok = false; r.error = "unterminated string literal"; r.err_line = sl; r.err_col = sc; return r; }
            adv(1); // closing "
            r.toks.push_back({Tk::StringLit, s, sl, sc});
            continue;
        }
        // punctuation / operators
        auto two = [&](char a, char b, Tk k){ if (i+1 < src.size() && src[i]==a && src[i+1]==b) { push(k, std::string{a,b}); adv(2); return true; } return false; };
        auto three = [&](char a, char b, char c, Tk k){ if (i+2 < src.size() && src[i]==a && src[i+1]==b && src[i+2]==c) { push(k, std::string{a,b,c}); adv(3); return true; } return false; };
        switch (c) {
        case '(': push(Tk::LParen,"("); adv(1); continue;
        case ')': push(Tk::RParen,")"); adv(1); continue;
        case '{': push(Tk::LBrace,"{"); adv(1); continue;
        case '}': push(Tk::RBrace,"}"); adv(1); continue;
        case '[': push(Tk::LBracket,"["); adv(1); continue;
        case ']': push(Tk::RBracket,"]"); adv(1); continue;
        case ',': push(Tk::Comma,","); adv(1); continue;
        case ';': push(Tk::Semicolon,";"); adv(1); continue;
        case ':': if (two(':',':',Tk::DoubleColon)) continue; push(Tk::Colon,":"); adv(1); continue;
        case '?': push(Tk::Question,"?"); adv(1); continue;
        case '~': push(Tk::Tilde,"~"); adv(1); continue;
        case '.': if (two('.','.',Tk::DotDot)) continue; push(Tk::Dot,"."); adv(1); continue;
        case '-': if (two('-','-',Tk::Dec)) continue;
                  if (two('-','>',Tk::Arrow)) continue;
                  if (two('-','=',Tk::MinusAssign)) continue;
                  push(Tk::Minus,"-"); adv(1); continue;
        case '+': if (two('+','+',Tk::Inc)) continue;
                  if (two('+','=',Tk::PlusAssign)) continue;
                  push(Tk::Plus,"+"); adv(1); continue;
        case '*': if (two('*','=',Tk::StarAssign)) continue; push(Tk::Star,"*"); adv(1); continue;
        case '/': if (two('/','=',Tk::SlashAssign)) continue; push(Tk::Slash,"/"); adv(1); continue;
        case '%': if (two('%','=',Tk::PercentAssign)) continue; push(Tk::Percent,"%"); adv(1); continue;
        case '=': if (two('=','=',Tk::Eq)) continue; if (two('=','>',Tk::FatArrow)) continue; push(Tk::Assign,"="); adv(1); continue;
        case '!': if (two('!','=',Tk::Neq)) continue; push(Tk::Not,"!"); adv(1); continue;
        case '<': if (three('<','<','=',Tk::ShlAssign)) continue;
                  if (two('<','<',Tk::Shl)) continue;
                  if (two('<','=',Tk::Le)) continue;
                  push(Tk::Lt,"<"); adv(1); continue;
        case '>': if (three('>','>', '=',Tk::ShrAssign)) continue;
                  if (two('>','>',Tk::Shr)) continue;
                  if (two('>','=',Tk::Ge)) continue;
                  push(Tk::Gt,">"); adv(1); continue;
        case '&': if (two('&','&',Tk::AndAnd)) continue;
                  if (two('&','=',Tk::AmpAssign)) continue;
                  push(Tk::Amp,"&"); adv(1); continue;
        case '|': if (two('|','|',Tk::OrOr)) continue;
                  if (two('|','=',Tk::PipeAssign)) continue;
                  push(Tk::Pipe,"|"); adv(1); continue;
        case '^': if (two('^','=',Tk::CaretAssign)) continue;
                  push(Tk::Caret,"^"); adv(1); continue;
        }
        r.ok = false; r.error = std::string("unexpected character '") + c + "'";
        r.err_line = line; r.err_col = col;
        return r;
    }
    r.toks.push_back({Tk::Eof, "", line, col});
    return r;
}

} // namespace ember
