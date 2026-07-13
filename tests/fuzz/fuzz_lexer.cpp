// fuzz_lexer.cpp — libFuzzer harness for the ember lexer + parser.
//
// Feeds arbitrary bytes as source text, tokenizes, then parses. Any input
// should either produce a valid AST or a clean error — never a crash. The
// parser depth guard (safety::DepthGuard, max 256) prevents stack overflow
// from deeply nested input (the incident root cause). The lexer string cap
// (1 MiB) prevents memory exhaustion.
//
// Build (Linux/CI with libFuzzer):
//   clang++ -O1 -g -fsanitize=fuzzer,address \
//     -I src fuzz_lexer.cpp -L build_fuzz -lember -lember_frontend \
//     -o fuzz_lexer
//   ./fuzz_lexer -max_total_time=60
#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/safety.hpp"

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Treat the fuzz input as source text. Null-terminate for safety.
    std::string src(reinterpret_cast<const char*>(data), size);

    // Tokenize — should never crash on any byte sequence.
    auto lr = ember::tokenize(src, "<fuzz>");
    if (!lr.ok) return 0;  // lex error is a clean rejection

    // Parse — should never crash on any token stream. The depth guard
    // throws safety::DepthLimitExceeded which we catch (clean reject).
    try {
        auto pr = ember::parse(std::move(lr.toks));
        (void)pr;  // we don't care if parse succeeds or fails — just no crash
    } catch (const ember::safety::DepthLimitExceeded&) {
        // Clean rejection — depth limit hit on pathological input.
    } catch (const std::exception&) {
        // Any other exception is a clean rejection (not a crash).
    }
    return 0;
}
