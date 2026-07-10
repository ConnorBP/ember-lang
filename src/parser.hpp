// ember parser - recursive descent, full v1 grammar (docs/spec/COMPILER_PIPELINE.md Section 2).
#pragma once
#include "ast.hpp"
#include "lexer.hpp"
#include <stdexcept>
#include <string>
#include <vector>

namespace ember {

struct ParseError : std::runtime_error {
    uint32_t line, col;
    ParseError(const std::string& m, uint32_t l, uint32_t c)
        : std::runtime_error(m), line(l), col(c) {}
};

struct ParseErrorEntry { std::string msg; uint32_t line; uint32_t col; };

struct ParseResult {
    Program program;
    bool ok = true;
    std::string error;                     // all errors joined, one per line
    uint32_t err_line = 0, err_col = 0;     // first error's position
    std::vector<ParseErrorEntry> errors;    // every error, individually positioned
};

// Parse a full program. Throws on first unrecoverable error (caught
// internally, surfaced as ParseResult::ok=false). Best-effort
// synchronization to the next ';' / '}' (docs/spec/COMPILER_PIPELINE.md Section 7).
ParseResult parse(std::vector<Token> toks);

} // namespace ember
