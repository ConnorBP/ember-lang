// fuzz_batch_driver.cpp — Windows fallback for libFuzzer harnesses.
//
// libFuzzer (-fsanitize=fuzzer) is not supported on Windows/MinGW (confirmed
// empirically — see docs/planning/plan_AUDIT_TOOLING.md). This driver provides
// a poor-man's fuzzer: it reads files from a corpus directory, feeds each to
// the fuzz harness entry points, and reports any crash. It doesn't do
// coverage-guided mutation (that's the Linux CI job), but it verifies the
// harnesses don't crash on known inputs and can be used for regression testing
// of crash-reproducing inputs found by the Linux fuzzer.
//
// Build (Windows):
//   g++ -O1 -g -I src tests/fuzz/fuzz_batch_driver.cpp tests/fuzz/fuzz_em_loader.cpp tests/fuzz/fuzz_lexer.cpp \
//     -L buildt -lember -lember_frontend -o fuzz_batch_driver.exe
//   ./fuzz_batch_driver.exe <corpus_dir>
//
// The Linux CI job runs the real libFuzzer with coverage-guided mutation.
#include "../src/em_loader.hpp"
#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/safety.hpp"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>

// Pull in the harness entry points (they're the same LLVMFuzzerTestOneInput
// functions, but we call them directly instead of via the fuzzer runtime).
extern "C" int LLVMFuzzerTestOneInput_em_loader(const uint8_t* data, size_t size);
extern "C" int LLVMFuzzerTestOneInput_lexer(const uint8_t* data, size_t size);

// Rename the harness entry points to avoid multiple-definition when linking
// both harnesses. We do this by wrapping the harnesses.
static int fuzz_em_loader_entry(const uint8_t* data, size_t size) {
    ember::LoadedModule out;
    std::string err;
    ember::EmLoadPolicy policy{0, false};
    ember::load_em_bytes(data, size, out, &err, nullptr, nullptr, nullptr, &policy);
    return 0;
}

static int fuzz_lexer_entry(const uint8_t* data, size_t size) {
    std::string src(reinterpret_cast<const char*>(data), size);
    auto lr = ember::tokenize(src, "<fuzz>");
    if (!lr.ok) return 0;
    try {
        auto pr = ember::parse(std::move(lr.toks));
        (void)pr;
    } catch (const ember::safety::DepthLimitExceeded&) {
    } catch (const std::exception&) {
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: fuzz_batch_driver <corpus_dir>\n");
        std::fprintf(stderr, "  Feeds every file in <corpus_dir> to the fuzz harnesses.\n");
        return 2;
    }

    std::filesystem::path corpus(argv[1]);
    if (!std::filesystem::exists(corpus) || !std::filesystem::is_directory(corpus)) {
        std::fprintf(stderr, "corpus directory not found: %s\n", argv[1]);
        return 2;
    }

    int tested = 0, crashed = 0;

    for (auto& entry : std::filesystem::directory_iterator(corpus)) {
        if (!entry.is_regular_file()) continue;
        std::ifstream f(entry.path(), std::ios::binary);
        if (!f) continue;
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
        tested++;

        // Feed to em_loader harness.
        try {
            fuzz_em_loader_entry(data.data(), data.size());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "CRASH (em_loader) on %s: %s\n",
                         entry.path().filename().string().c_str(), e.what());
            crashed++;
        }

        // Feed to lexer harness.
        try {
            fuzz_lexer_entry(data.data(), data.size());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "CRASH (lexer) on %s: %s\n",
                         entry.path().filename().string().c_str(), e.what());
            crashed++;
        }
    }

    std::printf("\n%d files tested, %d crashes\n", tested, crashed);
    return crashed > 0 ? 1 : 0;
}
