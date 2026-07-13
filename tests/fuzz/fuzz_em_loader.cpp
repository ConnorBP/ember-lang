// fuzz_em_loader.cpp — libFuzzer harness for the .em binary module loader.
//
// The .em loader (em_loader.cpp load_em_bytes) deserializes untrusted binary
// input — the #1 attack surface for ember. This harness feeds arbitrary bytes
// and asserts the loader either cleanly loads or cleanly rejects (no crash,
// no abort, no uncaught exception). The safety::DepthGuard + RSS cap protect
// against unbounded recursion/memory; the fuzzer catches any input that slips
// past them.
//
// Build (Linux/CI with libFuzzer):
//   clang++ -O1 -g -fsanitize=fuzzer,address \
//     -I src fuzz_em_loader.cpp -L build_fuzz -lember -lember_frontend \
//     -o fuzz_em_loader
//   ./fuzz_em_loader -max_total_time=60 -max_len=65536
//
// Build (Windows — no libFuzzer; use the batch harness driver instead):
//   See tests/fuzz/fuzz_batch_driver.cpp which links the same harnesses.
#include "../src/em_loader.hpp"
#include "../src/safety.hpp"

#include <cstdint>
#include <cstddef>
#include <cstring>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    ember::LoadedModule out;
    std::string err;
    // Try to load the arbitrary bytes as a .em module. We pass no registry
    // and no native bindings (the most restrictive policy). The loader should
    // return false with a categorized error message for any malformed input,
    // or true for a valid .em. Either way: no crash.
    ember::EmLoadPolicy policy{0, false};
    ember::load_em_bytes(data, size, out, &err, nullptr, nullptr, nullptr, &policy);
    return 0;
}
