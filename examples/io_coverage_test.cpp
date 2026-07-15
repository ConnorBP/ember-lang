// Direct coverage for console, whole-file and path I/O natives.
#include "ext_io.hpp"
#include "ext_array.hpp"
#include "ext_string.hpp"
#include "ast.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace ember;
#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); cleanup(); return 1; } } while (0)

static const std::filesystem::path data_path = std::filesystem::current_path() / "io_coverage_data.bin";
static const std::filesystem::path input_path = std::filesystem::current_path() / "io_coverage_stdin.txt";
static void cleanup() {
    std::error_code ec;
    std::filesystem::remove(data_path, ec);
    std::filesystem::remove(input_path, ec);
    ext_array::reset(); ext_string::reset(); ext_io::reset();
}

template <typename Fn>
static Fn native(const std::unordered_map<std::string, NativeSig>& n, const char* name) {
    const auto it = n.find(name); if (it == n.end() || !it->second.fn_ptr) std::abort();
    return reinterpret_cast<Fn>(it->second.fn_ptr);
}

int main() {
    cleanup();
    std::unordered_map<std::string, NativeSig> n;
    ext_io::register_natives(n);

    const auto print = native<void(*)(int64_t)>(n, "print");
    const auto println = native<void(*)(int64_t)>(n, "println");
    const auto print_i64 = native<void(*)(int64_t)>(n, "print_i64");
    const auto print_f64 = native<void(*)(double)>(n, "print_f64");
    const auto read_line = native<int64_t(*)()>(n, "read_line");
    const auto read_bytes = native<int64_t(*)(int64_t)>(n, "file_read_bytes");
    const auto read_text = native<int64_t(*)(int64_t)>(n, "file_read_text");
    const auto write_bytes = native<int64_t(*)(int64_t, int64_t)>(n, "file_write_bytes");
    const auto file_exists = native<int64_t(*)(int64_t)>(n, "file_exists");
    const auto path_exists = native<int64_t(*)(int64_t)>(n, "path_exists");
    const auto basename = native<int64_t(*)(int64_t)>(n, "path_basename");
    const auto dirname = native<int64_t(*)(int64_t)>(n, "path_dirname");

    const int64_t output = ext_string::alloc("io-print");
    print(output); print(0); println(output); println(0); print_i64(-123456789); print_f64(3.25); std::fputc('\n', stdout);

    const std::string path_string = data_path.string();
    const int64_t path = ext_string::alloc(path_string);
    const int64_t missing = ext_string::alloc(path_string + ".missing");
    const std::vector<uint8_t> expected{0, 1, 2, 'E', 'm', 'b', 'e', 'r', 255};
    const int64_t bytes = ext_array::alloc_bytes(expected.data(), int64_t(expected.size()));
    CHECK(write_bytes(path, bytes) == 1);
    CHECK(file_exists(path) == 1 && path_exists(path) == 1);
    CHECK(file_exists(missing) == 0 && path_exists(missing) == 0);
    CHECK(file_exists(0) == 0 && path_exists(0) == 0);
    CHECK(read_text(missing) == 0 && read_text(0) == 0);
    CHECK(write_bytes(0, bytes) == 0 && write_bytes(path, 999999) == 0);

    const int64_t roundtrip = read_bytes(path);
    uint8_t* actual = nullptr; int64_t actual_len = -1;
    CHECK(roundtrip > 0 && ext_array::get_bytes(roundtrip, &actual, &actual_len));
    CHECK(actual_len == int64_t(expected.size()));
    CHECK(std::vector<uint8_t>(actual, actual + actual_len) == expected);
    CHECK(read_bytes(missing) == 0 && read_bytes(0) == 0);

    const int64_t empty = ext_array::alloc_bytes(nullptr, 0);
    CHECK(write_bytes(path, empty) == 1);
    const int64_t empty_read = read_bytes(path);
    CHECK(empty_read > 0 && ext_array::get_bytes(empty_read, &actual, &actual_len) && actual_len == 0);
    const int64_t empty_text = read_text(path);
    CHECK(empty_text > 0 && ext_string::slot(empty_text) && ext_string::slot(empty_text)->empty());

    const int64_t base_h = basename(path);
    const int64_t dir_h = dirname(path);
    CHECK(ext_string::slot(base_h) && *ext_string::slot(base_h) == data_path.filename().string());
    CHECK(ext_string::slot(dir_h) && *ext_string::slot(dir_h) == data_path.parent_path().string());
    CHECK(ext_string::slot(basename(0)) && ext_string::slot(dirname(0)));

    {
        std::ofstream in(input_path, std::ios::binary);
        in << "\ncontent\r\npartial";
    }
    CHECK(std::freopen(input_path.string().c_str(), "rb", stdin) != nullptr);
    const int64_t blank = read_line();
    const int64_t content = read_line();
    const int64_t partial = read_line();
    CHECK(blank > 0 && ext_string::slot(blank) && ext_string::slot(blank)->empty());
    CHECK(content > 0 && ext_string::slot(content) && *ext_string::slot(content) == "content");
    CHECK(partial > 0 && ext_string::slot(partial) && *ext_string::slot(partial) == "partial");
    CHECK(read_line() == 0);

    cleanup();
    std::puts("io coverage: PASS");
    return 0;
}
