#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/memory_mapped_file.hpp>
#include <pulp/runtime/temporary_file.hpp>
#include <pulp/runtime/dynamic_library.hpp>
#include <pulp/runtime/inter_process_lock.hpp>
#include <pulp/runtime/child_process.hpp>
#include <pulp/runtime/base64.hpp>
#include <pulp/runtime/http.hpp>
#include <pulp/runtime/range.hpp>
#include <pulp/runtime/text_diff.hpp>
#include <fstream>
#include <filesystem>

using namespace pulp::runtime;

// ── TemporaryFile ───────────────────────────────────────────────────────

TEST_CASE("TemporaryFile creates and deletes", "[runtime][temp_file]") {
    std::filesystem::path path;
    {
        TemporaryFile tmp(".txt");
        path = tmp.path();
        REQUIRE(std::filesystem::exists(path));
        REQUIRE(path.extension() == ".txt");
    }
    REQUIRE_FALSE(std::filesystem::exists(path));
}

TEST_CASE("TemporaryFile release prevents deletion", "[runtime][temp_file]") {
    std::filesystem::path path;
    {
        TemporaryFile tmp(".dat");
        path = tmp.path();
        tmp.release();
    }
    REQUIRE(std::filesystem::exists(path));
    std::filesystem::remove(path);  // Clean up manually
}

TEST_CASE("TemporaryFile move semantics", "[runtime][temp_file]") {
    TemporaryFile a(".tmp");
    auto path = a.path();

    TemporaryFile b = std::move(a);
    REQUIRE(b.path() == path);
    REQUIRE(std::filesystem::exists(path));
}

// ── MemoryMappedFile ────────────────────────────────────────────────────

TEST_CASE("MemoryMappedFile maps a file", "[runtime][mmap]") {
    // Create a test file
    TemporaryFile tmp(".bin");
    {
        std::ofstream f(tmp.path(), std::ios::binary);
        f << "Hello, mmap!";
    }

    MemoryMappedFile mmap;
    REQUIRE(mmap.open(tmp.path_string()));
    REQUIRE(mmap.is_open());
    REQUIRE(mmap.size() == 12);
    REQUIRE(std::string(reinterpret_cast<const char*>(mmap.data()), mmap.size()) == "Hello, mmap!");

    mmap.close();
    REQUIRE_FALSE(mmap.is_open());
}

TEST_CASE("MemoryMappedFile fails on nonexistent file", "[runtime][mmap]") {
    MemoryMappedFile mmap;
    REQUIRE_FALSE(mmap.open("/tmp/pulp_nonexistent_file_12345.bin"));
}

TEST_CASE("MemoryMappedFile move semantics", "[runtime][mmap]") {
    TemporaryFile tmp(".bin");
    {
        std::ofstream f(tmp.path(), std::ios::binary);
        f << "test";
    }

    MemoryMappedFile a;
    a.open(tmp.path_string());
    REQUIRE(a.is_open());

    MemoryMappedFile b = std::move(a);
    REQUIRE(b.is_open());
    REQUIRE_FALSE(a.is_open());
}

// ── DynamicLibrary ──────────────────────────────────────────────────────

TEST_CASE("DynamicLibrary loads system library", "[runtime][dynlib]") {
#ifdef __APPLE__
    DynamicLibrary lib;
    REQUIRE(lib.open("/usr/lib/libSystem.B.dylib"));
    REQUIRE(lib.is_open());

    void* sym = lib.find_symbol("malloc");
    REQUIRE(sym != nullptr);

    lib.close();
    REQUIRE_FALSE(lib.is_open());
#elif defined(__linux__)
    DynamicLibrary lib;
    REQUIRE(lib.open("libc.so.6"));
    void* sym = lib.find_symbol("malloc");
    REQUIRE(sym != nullptr);
#endif
}

TEST_CASE("DynamicLibrary fails on nonexistent", "[runtime][dynlib]") {
    DynamicLibrary lib;
    REQUIRE_FALSE(lib.open("/tmp/nonexistent_lib_12345.dylib"));
    REQUIRE_FALSE(lib.error().empty());
}

// ── InterProcessLock ────────────────────────────────────────────────────

TEST_CASE("InterProcessLock acquires and releases", "[runtime][ipc_lock]") {
    InterProcessLock lock("test_lock_pulp");
    REQUIRE(lock.try_lock());
    REQUIRE(lock.is_locked());
    lock.unlock();
    REQUIRE_FALSE(lock.is_locked());
}

TEST_CASE("InterProcessLock double lock succeeds", "[runtime][ipc_lock]") {
    InterProcessLock lock("test_lock_double");
    REQUIRE(lock.try_lock());
    REQUIRE(lock.try_lock());  // Already locked, should still return true
}

// ── ChildProcess ────────────────────────────────────────────────────────

TEST_CASE("run_process captures stdout", "[runtime][child_process]") {
#ifdef _WIN32
    // run_process double-quotes every arg, which breaks cmd.exe's /c
    // quoting rules. Use powershell -NoProfile -Command instead.
    auto result = run_process("powershell", {"-NoProfile", "-Command", "Write-Output 'hello world'"});
#else
    auto result = run_process("/bin/echo", {"hello", "world"});
#endif
    REQUIRE(result.has_value());
    REQUIRE(result->exit_code == 0);
    REQUIRE(result->stdout_output.find("hello") != std::string::npos);
}

TEST_CASE("run_process captures exit code", "[runtime][child_process]") {
#ifdef _WIN32
    auto result = run_process("powershell", {"-NoProfile", "-Command", "exit 42"});
#else
    auto result = run_process("/bin/sh", {"-c", "exit 42"});
#endif
    REQUIRE(result.has_value());
    REQUIRE(result->exit_code == 42);
}

TEST_CASE("run_process fails on nonexistent", "[runtime][child_process]") {
#ifdef _WIN32
    auto result = run_process("C:\\nonexistent_binary_12345.exe");
#else
    auto result = run_process("/tmp/nonexistent_binary_12345");
#endif
    REQUIRE_FALSE(result.has_value());
}

// ── Base64 ──────────────────────────────────────────────────────────────

TEST_CASE("base64 encode/decode round-trip", "[runtime][base64]") {
    std::string input = "Hello, Pulp!";
    auto encoded = base64_encode(input);
    REQUIRE(encoded == "SGVsbG8sIFB1bHAh");

    auto decoded = base64_decode(encoded);
    REQUIRE(decoded.has_value());
    std::string result(decoded->begin(), decoded->end());
    REQUIRE(result == input);
}

TEST_CASE("base64 encode empty", "[runtime][base64]") {
    REQUIRE(base64_encode("") == "");
}

TEST_CASE("base64 encode padding", "[runtime][base64]") {
    REQUIRE(base64_encode("a") == "YQ==");
    REQUIRE(base64_encode("ab") == "YWI=");
    REQUIRE(base64_encode("abc") == "YWJj");
}

TEST_CASE("base64 decode invalid", "[runtime][base64]") {
    auto result = base64_decode("!!!invalid!!!");
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("base64 decode rejects malformed padding", "[runtime][base64][issue-641]") {
    REQUIRE_FALSE(base64_decode("A").has_value());
    REQUIRE_FALSE(base64_decode("YQ=").has_value());
    REQUIRE_FALSE(base64_decode("Y=Q=").has_value());
    REQUIRE_FALSE(base64_decode("YQ==Z").has_value());
    REQUIRE_FALSE(base64_decode("YQ===").has_value());
    REQUIRE_FALSE(base64_decode("====").has_value());
}

TEST_CASE("base64 decode permits whitespace around terminal padding", "[runtime][base64][issue-641]") {
    auto decoded = base64_decode(" YQ = = \n");
    REQUIRE(decoded.has_value());
    REQUIRE(std::string(decoded->begin(), decoded->end()) == "a");
}

TEST_CASE("base64 decode handles whitespace and unpadded tail groups", "[runtime][base64][issue-641]") {
    auto empty = base64_decode(" \n\r\t");
    REQUIRE(empty.has_value());
    REQUIRE(empty->empty());

    auto one_byte = base64_decode("YQ");
    REQUIRE(one_byte.has_value());
    REQUIRE(std::string(one_byte->begin(), one_byte->end()) == "a");

    auto two_bytes = base64_decode("YWI");
    REQUIRE(two_bytes.has_value());
    REQUIRE(std::string(two_bytes->begin(), two_bytes->end()) == "ab");

    auto with_whitespace = base64_decode(" SGV sbG8=\n");
    REQUIRE(with_whitespace.has_value());
    REQUIRE(std::string(with_whitespace->begin(), with_whitespace->end()) == "Hello");
}

TEST_CASE("base64 decode rejects non-ascii bytes", "[runtime][base64][issue-641]") {
    std::string encoded = "YQ";
    encoded.push_back(static_cast<char>(0x80));
    REQUIRE_FALSE(base64_decode(encoded).has_value());
}

TEST_CASE("base64 binary round-trip", "[runtime][base64]") {
    std::vector<uint8_t> data = {0x00, 0xFF, 0x80, 0x7F, 0x01, 0xFE};
    auto encoded = base64_encode(data.data(), data.size());
    auto decoded = base64_decode(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(*decoded == data);
}

// ── HTTP URL parsing ───────────────────────────────────────────────────

TEST_CASE("HTTP helpers reject malformed URLs without transport work",
          "[runtime][http][url][issue-641]") {
    const auto get_response = http_get("ftp://example.com/file", 1);
    REQUIRE(get_response.status_code == 0);
    REQUIRE(get_response.error == "Invalid URL");

    const auto post_response = http_post("http:///missing-host", "{}", "application/json", 1);
    REQUIRE(post_response.status_code == 0);
    REQUIRE(post_response.error == "Invalid URL");

    REQUIRE_FALSE(http_download("example.com/no-scheme", "/tmp/pulp-url-invalid-download", 1));
}

TEST_CASE("HTTP helpers reject invalid numeric URL ports",
          "[runtime][http][url][issue-641]") {
    const auto zero_port = http_get("http://example.com:0/path", 1);
    REQUIRE(zero_port.status_code == 0);
    REQUIRE(zero_port.error == "Invalid URL");

    const auto overflow_port = http_get("http://example.com:999999999999999999999/path", 1);
    REQUIRE(overflow_port.status_code == 0);
    REQUIRE(overflow_port.error == "Invalid URL");

    const auto too_large_port = http_post("http://example.com:65536/path", "body", "text/plain", 1);
    REQUIRE(too_large_port.status_code == 0);
    REQUIRE(too_large_port.error == "Invalid URL");
}

// ── Text Diff ────────────────────────────────────────────────────────────

TEST_CASE("text_diff handles empty inputs", "[runtime][text-diff][issue-641]") {
    auto diff = text_diff("", "");
    REQUIRE(diff.empty());
    REQUIRE(format_diff(diff).empty());
}

TEST_CASE("text_diff reports pure inserts and deletes",
          "[runtime][text-diff][issue-641]") {
    auto inserted = text_diff("", "one\ntwo");
    REQUIRE(inserted.size() == 2);
    REQUIRE(inserted[0].op == DiffOp::Insert);
    REQUIRE(inserted[0].text == "one");
    REQUIRE(inserted[1].op == DiffOp::Insert);
    REQUIRE(inserted[1].text == "two");

    auto deleted = text_diff("one\ntwo", "");
    REQUIRE(deleted.size() == 2);
    REQUIRE(deleted[0].op == DiffOp::Delete);
    REQUIRE(deleted[0].text == "one");
    REQUIRE(deleted[1].op == DiffOp::Delete);
    REQUIRE(deleted[1].text == "two");
}

TEST_CASE("text_diff preserves equal lines across replacements and appends",
          "[runtime][text-diff][issue-641]") {
    auto diff = text_diff("alpha\nbeta\ngamma\n",
                          "alpha\ndelta\ngamma\nomega");

    REQUIRE(diff.size() == 5);
    REQUIRE(diff[0].op == DiffOp::Equal);
    REQUIRE(diff[0].text == "alpha");
    REQUIRE(diff[1].op == DiffOp::Delete);
    REQUIRE(diff[1].text == "beta");
    REQUIRE(diff[2].op == DiffOp::Insert);
    REQUIRE(diff[2].text == "delta");
    REQUIRE(diff[3].op == DiffOp::Equal);
    REQUIRE(diff[3].text == "gamma");
    REQUIRE(diff[4].op == DiffOp::Insert);
    REQUIRE(diff[4].text == "omega");

    REQUIRE(format_diff(diff) ==
            "  alpha\n"
            "- beta\n"
            "+ delta\n"
            "  gamma\n"
            "+ omega\n");
}

// ── Range ───────────────────────────────────────────────────────────────

TEST_CASE("Range basic operations", "[runtime][range]") {
    IntRange r(10, 20);
    REQUIRE(r.length() == 10);
    REQUIRE_FALSE(r.empty());
    REQUIRE(r.contains(10));
    REQUIRE(r.contains(15));
    REQUIRE_FALSE(r.contains(20));
    REQUIRE_FALSE(r.contains(9));
}

TEST_CASE("Range intersection", "[runtime][range]") {
    IntRange a(0, 10);
    IntRange b(5, 15);
    auto i = a.intersection(b);
    REQUIRE(i.start == 5);
    REQUIRE(i.end == 10);
}

TEST_CASE("Range no intersection", "[runtime][range]") {
    IntRange a(0, 5);
    IntRange b(10, 20);
    REQUIRE(a.intersection(b).empty());
    REQUIRE_FALSE(a.intersects(b));
}

TEST_CASE("Range union", "[runtime][range]") {
    IntRange a(0, 5);
    IntRange b(10, 20);
    auto u = a.enclosing_union(b);
    REQUIRE(u.start == 0);
    REQUIRE(u.end == 20);
}

TEST_CASE("Range constrain", "[runtime][range]") {
    IntRange r(0, 100);
    REQUIRE(r.constrain(-5) == 0);
    REQUIRE(r.constrain(50) == 50);
    REQUIRE(r.constrain(200) == 99);
}

TEST_CASE("Range from_start_length", "[runtime][range]") {
    auto r = IntRange::from_start_length(5, 10);
    REQUIRE(r.start == 5);
    REQUIRE(r.end == 15);
    REQUIRE(r.length() == 10);
}

TEST_CASE("Range covers containment and equality edge paths",
          "[runtime][range][coverage][issue-641]") {
    IntRange outer(0, 10);

    REQUIRE(outer.contains(IntRange(0, 10)));
    REQUIRE(outer.contains(IntRange(2, 8)));
    REQUIRE(outer.contains(IntRange(5, 5)));
    REQUIRE_FALSE(outer.contains(IntRange(-1, 5)));
    REQUIRE_FALSE(outer.contains(IntRange(5, 11)));

    REQUIRE(outer == IntRange(0, 10));
    REQUIRE(outer != IntRange(0, 9));
}

TEST_CASE("Range expands empty and non-empty intervals",
          "[runtime][range][coverage][issue-641]") {
    REQUIRE(IntRange(5, 5).expanded(8) == IntRange(8, 9));
    REQUIRE(IntRange(3, 7).expanded(10) == IntRange(3, 11));
    REQUIRE(IntRange(3, 7).expanded(-2) == IntRange(-2, 7));
    REQUIRE(IntRange(3, 7).expanded(5) == IntRange(3, 7));
}

TEST_CASE("Range union handles empty operands",
          "[runtime][range][coverage][issue-641]") {
    REQUIRE(IntRange().enclosing_union(IntRange(4, 9)) == IntRange(4, 9));
    REQUIRE(IntRange(4, 9).enclosing_union(IntRange()) == IntRange(4, 9));
    REQUIRE(IntRange().enclosing_union(IntRange()) == IntRange());
}

TEST_CASE("FloatRange", "[runtime][range]") {
    FloatRange r(0.0f, 1.0f);
    REQUIRE(r.contains(0.5f));
    REQUIRE_FALSE(r.contains(1.0f));
}
