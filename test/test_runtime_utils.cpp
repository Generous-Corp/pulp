#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/runtime/memory_mapped_file.hpp>
#include <pulp/runtime/temporary_file.hpp>
#include <pulp/runtime/dynamic_library.hpp>
#include <pulp/runtime/inter_process_lock.hpp>
#include <pulp/runtime/child_process.hpp>
#include <pulp/runtime/base64.hpp>
#include <pulp/runtime/expression.hpp>
#include <pulp/runtime/http.hpp>
#include <pulp/runtime/range.hpp>
#include <pulp/runtime/scope_guard.hpp>
#include <pulp/runtime/text_diff.hpp>
#include <catch2/catch_approx.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <iterator>

using namespace pulp::runtime;

namespace {

const char* system_library_path() {
#if defined(_WIN32)
    return "kernel32.dll";
#elif defined(__APPLE__)
    return "/usr/lib/libSystem.B.dylib";
#else
    return "libc.so.6";
#endif
}

const char* system_library_symbol() {
#if defined(_WIN32)
    return "GetCurrentProcess";
#else
    return "malloc";
#endif
}

}  // namespace

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

TEST_CASE("TemporaryFile normalizes extensions without leading dots",
          "[runtime][temp_file][issue-641]") {
    TemporaryFile tmp("raw");
    REQUIRE(tmp.path().extension() == ".raw");
    REQUIRE(tmp.path_string() == tmp.path().string());
    REQUIRE(std::filesystem::exists(tmp.path()));
}

TEST_CASE("TemporaryFile move assignment removes the previous active file",
          "[runtime][temp_file][issue-641]") {
    std::filesystem::path old_path;
    std::filesystem::path new_path;
    {
        TemporaryFile old_file(".old");
        TemporaryFile new_file(".new");
        old_path = old_file.path();
        new_path = new_file.path();

        old_file = std::move(new_file);
        REQUIRE(old_file.path() == new_path);
        REQUIRE_FALSE(std::filesystem::exists(old_path));
        REQUIRE(std::filesystem::exists(new_path));
    }

    REQUIRE_FALSE(std::filesystem::exists(new_path));
}

TEST_CASE("TemporaryFile self move assignment preserves ownership",
          "[runtime][temp_file][issue-641]") {
    std::filesystem::path path;
    {
        TemporaryFile tmp(".self");
        path = tmp.path();
        auto& ref = tmp;
        tmp = std::move(ref);
        REQUIRE(tmp.path() == path);
        REQUIRE(std::filesystem::exists(path));
    }

    REQUIRE_FALSE(std::filesystem::exists(path));
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

TEST_CASE("MemoryMappedFile reopen closes the previous mapping",
          "[runtime][mmap][issue-641]") {
    TemporaryFile first(".bin");
    TemporaryFile second(".bin");
    {
        std::ofstream f(first.path(), std::ios::binary);
        f << "first";
    }
    {
        std::ofstream f(second.path(), std::ios::binary);
        f << "second-file";
    }

    MemoryMappedFile mmap;
    REQUIRE(mmap.open(first.path_string()));
    REQUIRE(mmap.size() == 5);
    REQUIRE(mmap.open(second.path_string()));
    REQUIRE(mmap.is_open());
    REQUIRE(mmap.size() == 11);
    REQUIRE(std::string(reinterpret_cast<const char*>(mmap.data()), mmap.size()) == "second-file");
}

TEST_CASE("MemoryMappedFile ReadWrite mode persists byte edits",
          "[runtime][mmap][coverage][issue-641]") {
    TemporaryFile tmp(".bin");
    {
        std::ofstream f(tmp.path(), std::ios::binary);
        f << "abcde";
    }

    MemoryMappedFile mmap;
    REQUIRE(mmap.open(tmp.path_string(), MapMode::ReadWrite));
    REQUIRE(mmap.is_open());
    REQUIRE(mmap.size() == 5);
    REQUIRE(mmap.mutable_data() != nullptr);

    mmap.mutable_data()[1] = static_cast<uint8_t>('A');
    mmap.mutable_data()[3] = static_cast<uint8_t>('D');
    mmap.close();
    REQUIRE_FALSE(mmap.is_open());

    std::ifstream f(tmp.path(), std::ios::binary);
    std::string contents((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    REQUIRE(contents == "aAcDe");
}

TEST_CASE("MemoryMappedFile open failure clears an existing mapping",
          "[runtime][mmap][coverage][issue-641]") {
    TemporaryFile mapped(".bin");
    {
        std::ofstream f(mapped.path(), std::ios::binary);
        f << "mapped";
    }

    TemporaryFile empty(".bin");

    MemoryMappedFile mmap;
    REQUIRE(mmap.open(mapped.path_string()));
    REQUIRE(mmap.is_open());
    REQUIRE(mmap.size() == 6);

    REQUIRE_FALSE(mmap.open(empty.path_string()));
    REQUIRE_FALSE(mmap.is_open());
    REQUIRE(mmap.data() == nullptr);
    REQUIRE(mmap.size() == 0);
}

TEST_CASE("MemoryMappedFile read-write maps persist and move assignment closes old map",
          "[runtime][mmap][coverage][issue-656]") {
    TemporaryFile first(".bin");
    TemporaryFile second(".bin");
    {
        std::ofstream f(first.path(), std::ios::binary);
        f << "abcd";
    }
    {
        std::ofstream f(second.path(), std::ios::binary);
        f << "wxyz";
    }

    MemoryMappedFile old_map;
    REQUIRE(old_map.open(first.path_string(), MapMode::ReadOnly));
    REQUIRE(old_map.is_open());

    MemoryMappedFile writable;
    REQUIRE(writable.open(second.path_string(), MapMode::ReadWrite));
    REQUIRE(writable.is_open());
    REQUIRE(writable.size() == 4);
    writable.mutable_data()[1] = static_cast<uint8_t>('!');

    old_map = std::move(writable);
    REQUIRE(old_map.is_open());
    REQUIRE_FALSE(writable.is_open());
    REQUIRE(old_map.size() == 4);
    REQUIRE(std::string(reinterpret_cast<const char*>(old_map.data()), old_map.size()) == "w!yz");

    old_map.close();
    std::ifstream f(second.path(), std::ios::binary);
    std::string saved((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    REQUIRE(saved == "w!yz");
}

TEST_CASE("MemoryMappedFile rejects empty files and close is idempotent",
          "[runtime][mmap][coverage][issue-656]") {
    TemporaryFile tmp(".bin");

    MemoryMappedFile mmap;
    REQUIRE_FALSE(mmap.open(tmp.path_string()));
    REQUIRE_FALSE(mmap.is_open());
    REQUIRE(mmap.size() == 0);
    REQUIRE(mmap.data() == nullptr);
    mmap.close();
    mmap.close();
    REQUIRE_FALSE(mmap.is_open());
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

TEST_CASE("DynamicLibrary closed lookup is null and close is idempotent",
          "[runtime][dynlib][coverage][phase3]") {
    DynamicLibrary lib;
    REQUIRE_FALSE(lib.is_open());
    REQUIRE(lib.find_symbol(system_library_symbol()) == nullptr);
    lib.close();
    REQUIRE_FALSE(lib.is_open());
}

TEST_CASE("DynamicLibrary move construction transfers the handle",
          "[runtime][dynlib][coverage][phase3]") {
    DynamicLibrary lib;
    REQUIRE(lib.open(system_library_path()));
    REQUIRE(lib.find_symbol(system_library_symbol()) != nullptr);

    DynamicLibrary moved(std::move(lib));
    REQUIRE(moved.is_open());
    REQUIRE_FALSE(lib.is_open());
    REQUIRE(moved.find_symbol(system_library_symbol()) != nullptr);
}

TEST_CASE("DynamicLibrary move assignment closes target and transfers source",
          "[runtime][dynlib][coverage][phase3]") {
    DynamicLibrary target;
    DynamicLibrary source;
    REQUIRE(target.open(system_library_path()));
    REQUIRE(source.open(system_library_path()));

    target = std::move(source);

    REQUIRE(target.is_open());
    REQUIRE_FALSE(source.is_open());
    REQUIRE(target.find_symbol(system_library_symbol()) != nullptr);
}

TEST_CASE("DynamicLibrary failed reopen clears a previously loaded handle",
          "[runtime][dynlib][coverage][phase3]") {
    DynamicLibrary lib;
    REQUIRE(lib.open(system_library_path()));
    REQUIRE(lib.is_open());

    REQUIRE_FALSE(lib.open("/tmp/nonexistent_lib_after_success_12345.dylib"));
    REQUIRE_FALSE(lib.is_open());
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

TEST_CASE("InterProcessLock unlock is idempotent and permits reacquire",
          "[runtime][ipc_lock][coverage][issue-641]") {
    InterProcessLock lock("test_lock_reacquire");
    REQUIRE_FALSE(lock.is_locked());

    lock.unlock();
    REQUIRE_FALSE(lock.is_locked());

    REQUIRE(lock.try_lock());
    REQUIRE(lock.is_locked());
    lock.unlock();
    REQUIRE_FALSE(lock.is_locked());
    lock.unlock();
    REQUIRE_FALSE(lock.is_locked());

    REQUIRE(lock.try_lock());
    REQUIRE(lock.is_locked());
}

TEST_CASE("InterProcessLock competing instance waits for release",
          "[runtime][ipc_lock][coverage][phase3]") {
    const auto name = "test_lock_competing_phase3";
    InterProcessLock first(name);
    InterProcessLock second(name);

    REQUIRE(first.try_lock());
    REQUIRE_FALSE(second.try_lock());

    first.unlock();
    REQUIRE(second.try_lock());
    REQUIRE(second.is_locked());
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

TEST_CASE("run_process captures stderr separately",
          "[runtime][child_process][coverage][phase3]") {
#ifdef _WIN32
    auto result = run_process("powershell", {"-NoProfile", "-Command", "[Console]::Error.WriteLine('bad-news'); exit 7"});
#else
    auto result = run_process("/bin/sh", {"-c", "echo bad-news >&2; exit 7"});
#endif
    REQUIRE(result.has_value());
    REQUIRE(result->exit_code == 7);
    REQUIRE(result->stderr_output.find("bad-news") != std::string::npos);
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

TEST_CASE("base64 handles explicit byte pointers and exact quartet decoding",
          "[runtime][base64][coverage][issue-641]") {
    REQUIRE(base64_encode(nullptr, 0) == "");

    const uint8_t bytes[] = {0x00, 0x10, 0x20, 0x30, 0xff};
    auto encoded = base64_encode(bytes, sizeof(bytes));
    REQUIRE(encoded == "ABAgMP8=");

    auto decoded = base64_decode("ABAgMP8=");
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->size() == sizeof(bytes));
    REQUIRE(std::equal(decoded->begin(), decoded->end(), std::begin(bytes)));

    auto quartet = base64_decode("////");
    REQUIRE(quartet.has_value());
    REQUIRE(*quartet == std::vector<uint8_t>{0xff, 0xff, 0xff});
}

TEST_CASE("base64 covers full alphabet and padded binary tails",
          "[runtime][base64][coverage][phase3-large]") {
    const uint8_t alphabet_bytes[] = {
        0x00, 0x10, 0x83, 0x10, 0x51, 0x87, 0x20, 0x92,
        0x8b, 0x30, 0xd3, 0x8f, 0x41, 0x14, 0x93, 0x51,
        0x55, 0x97, 0x61, 0x96, 0x9b, 0x71, 0xd7, 0x9f,
        0x82, 0x18, 0xa3, 0x92, 0x59, 0xa7, 0xa2, 0x9a,
        0xab, 0xb2, 0xdb, 0xaf, 0xc3, 0x1c, 0xb3, 0xd3,
        0x5d, 0xb7, 0xe3, 0x9e, 0xbb, 0xf3, 0xdf, 0xbf,
    };

    auto encoded = base64_encode(alphabet_bytes, sizeof(alphabet_bytes));
    REQUIRE(encoded == "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/");

    auto decoded = base64_decode(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(std::equal(decoded->begin(), decoded->end(), std::begin(alphabet_bytes)));

    const uint8_t one_tail[] = {0xfb};
    const uint8_t two_tail[] = {0xfb, 0xff};
    REQUIRE(base64_encode(one_tail, sizeof(one_tail)) == "+w==");
    REQUIRE(base64_encode(two_tail, sizeof(two_tail)) == "+/8=");
}

TEST_CASE("base64 decode tolerates whitespace inside unpadded input",
          "[runtime][base64][coverage][phase3-large]") {
    auto decoded = base64_decode("\n Y W J \t j Z A \r");
    REQUIRE(decoded.has_value());
    REQUIRE(std::string(decoded->begin(), decoded->end()) == "abcd");
}

// ── Expression ─────────────────────────────────────────────────────────

TEST_CASE("Expression evaluator handles precedence and exponent edge cases",
          "[runtime][expression][coverage][issue-641]") {
    auto value = evaluate("2 + 3 * 4 ^ 2");
    REQUIRE(value.has_value());
    REQUIRE(*value == Catch::Approx(50.0));

    auto signed_power = evaluate("-2^2");
    REQUIRE(signed_power.has_value());
    REQUIRE(*signed_power == Catch::Approx(4.0));

    auto grouped = evaluate("(-2)^2");
    REQUIRE(grouped.has_value());
    REQUIRE(*grouped == Catch::Approx(4.0));
}

TEST_CASE("Expression evaluator handles constants, functions, and scientific notation",
          "[runtime][expression][coverage][issue-641]") {
    auto trig = evaluate("sin(pi / 2) + cos(0)");
    REQUIRE(trig.has_value());
    REQUIRE(*trig == Catch::Approx(2.0));

    auto clamped = evaluate("clamp(12, 10)");
    REQUIRE(clamped.has_value());
    REQUIRE(*clamped == Catch::Approx(10.0));

    auto scientific = evaluate("1.5e2 + 2E-1");
    REQUIRE(scientific.has_value());
    REQUIRE(*scientific == Catch::Approx(150.2));
}

TEST_CASE("Expression evaluator resolves variables and rejects malformed inputs",
          "[runtime][expression][coverage][issue-641]") {
    auto variable = evaluate("gain * 2 + offset", {{"gain", 0.75}, {"offset", 0.25}});
    REQUIRE(variable.has_value());
    REQUIRE(*variable == Catch::Approx(1.75));

    REQUIRE_FALSE(evaluate("missing + 1").has_value());
    REQUIRE_FALSE(evaluate("min(1)").has_value());
    REQUIRE_FALSE(evaluate("1e").has_value());
    REQUIRE_FALSE(evaluate("1e+").has_value());
    REQUIRE_FALSE(evaluate("1e-").has_value());
    REQUIRE_FALSE(evaluate("(1 + 2").has_value());
    REQUIRE_FALSE(evaluate("").has_value());
}

TEST_CASE("ExpressionEvaluator stores variables and clears them",
          "[runtime][expression][coverage][issue-641]") {
    ExpressionEvaluator evaluator;
    evaluator.set("x", 4.0);
    evaluator.set("y", 2.5);

    REQUIRE(evaluator.get("x").has_value());
    REQUIRE(*evaluator.get("x") == Catch::Approx(4.0));

    auto value = evaluator.evaluate("x * y");
    REQUIRE(value.has_value());
    REQUIRE(*value == Catch::Approx(10.0));

    evaluator.clear_variables();
    REQUIRE_FALSE(evaluator.get("x").has_value());
    REQUIRE_FALSE(evaluator.evaluate("x").has_value());
}

TEST_CASE("ExpressionEvaluator dispatches registered unary functions",
          "[runtime][expression][coverage][issue-641]") {
    ExpressionEvaluator evaluator;
    evaluator.register_function("db_to_gain", [](double db) {
        return std::pow(10.0, db / 20.0);
    });

    auto unity = evaluator.evaluate("db_to_gain(0)");
    REQUIRE(unity.has_value());
    REQUIRE(*unity == Catch::Approx(1.0));

    REQUIRE_FALSE(evaluator.evaluate("missing_fn(1)").has_value());
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

TEST_CASE("HttpResponse ok covers status code boundaries", "[runtime][http][url][coverage][issue-656]") {
    HttpResponse response;
    response.status_code = 199;
    REQUIRE_FALSE(response.ok());
    response.status_code = 200;
    REQUIRE(response.ok());
    response.status_code = 204;
    REQUIRE(response.ok());
    response.status_code = 299;
    REQUIRE(response.ok());
    response.status_code = 300;
    REQUIRE_FALSE(response.ok());
}

TEST_CASE("HTTP helpers reject malformed URL hosts and schemes",
          "[runtime][http][url][coverage][issue-656]") {
    REQUIRE(http_get("https://:443/path", 1).error == "Invalid URL");
    REQUIRE(http_get("http://example.com:not-a-port/path", 1).error == "Invalid URL");
    REQUIRE(http_post("file://example.com/path", "body", "text/plain", 1).error == "Invalid URL");
    REQUIRE_FALSE(http_download("http://", "/tmp/pulp-url-invalid-download-656", 1));
}

// ── Text Diff ────────────────────────────────────────────────────────────

TEST_CASE("text_diff handles empty inputs", "[runtime][text-diff][issue-641]") {
    auto diff = text_diff("", "");
    REQUIRE(diff.empty());
    REQUIRE(format_diff(diff).empty());
}

TEST_CASE("text_diff formats unchanged lines as context",
          "[runtime][text-diff][coverage][phase3]") {
    auto diff = text_diff("alpha\nbeta", "alpha\nbeta");

    REQUIRE(diff.size() == 2);
    REQUIRE(diff[0].op == DiffOp::Equal);
    REQUIRE(diff[0].text == "alpha");
    REQUIRE(diff[1].op == DiffOp::Equal);
    REQUIRE(diff[1].text == "beta");
    REQUIRE(format_diff(diff) == "  alpha\n  beta\n");
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

TEST_CASE("text_diff keeps repeated-line matches stable",
          "[runtime][text-diff][coverage][issue-641]") {
    auto diff = text_diff("same\nkeep\nsame\nremove\nsame",
                          "same\nkeep\nsame\ninsert\nsame");

    REQUIRE(diff.size() == 6);
    REQUIRE(diff[0].op == DiffOp::Equal);
    REQUIRE(diff[0].text == "same");
    REQUIRE(diff[1].op == DiffOp::Equal);
    REQUIRE(diff[1].text == "keep");
    REQUIRE(diff[2].op == DiffOp::Equal);
    REQUIRE(diff[2].text == "same");
    REQUIRE(diff[3].op == DiffOp::Delete);
    REQUIRE(diff[3].text == "remove");
    REQUIRE(diff[4].op == DiffOp::Insert);
    REQUIRE(diff[4].text == "insert");
    REQUIRE(diff[5].op == DiffOp::Equal);
    REQUIRE(diff[5].text == "same");
}

TEST_CASE("text_diff handles replacement ties and empty line formatting",
          "[runtime][text-diff][coverage][phase3]") {
    auto diff = text_diff("old-a\n\nold-b", "new-a\nnew-b");

    REQUIRE(diff.size() == 5);
    REQUIRE(diff[0].op == DiffOp::Delete);
    REQUIRE(diff[0].text == "old-a");
    REQUIRE(diff[1].op == DiffOp::Delete);
    REQUIRE(diff[1].text.empty());
    REQUIRE(diff[2].op == DiffOp::Delete);
    REQUIRE(diff[2].text == "old-b");
    REQUIRE(diff[3].op == DiffOp::Insert);
    REQUIRE(diff[3].text == "new-a");
    REQUIRE(diff[4].op == DiffOp::Insert);
    REQUIRE(diff[4].text == "new-b");

    REQUIRE(format_diff(diff) ==
            "- old-a\n"
            "- \n"
            "- old-b\n"
            "+ new-a\n"
            "+ new-b\n");
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

TEST_CASE("Range constrain handles empty and reversed integer ranges",
          "[runtime][range][coverage][issue-641]") {
    REQUIRE(IntRange(5, 5).constrain(100) == 5);
    REQUIRE(IntRange(10, 5).constrain(-100) == 10);
    REQUIRE(IntRange(-3, -1).constrain(9) == -2);
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


TEST_CASE("DoubleRange intersections and unions preserve fractional bounds",
          "[runtime][range][coverage][issue-641]") {
    DoubleRange a(0.25, 2.75);
    DoubleRange b(1.5, 4.0);

    auto intersection = a.intersection(b);
    REQUIRE_THAT(intersection.start, Catch::Matchers::WithinAbs(1.5, 1e-12));
    REQUIRE_THAT(intersection.end, Catch::Matchers::WithinAbs(2.75, 1e-12));

    auto combined = a.enclosing_union(b);
    REQUIRE_THAT(combined.start, Catch::Matchers::WithinAbs(0.25, 1e-12));
    REQUIRE_THAT(combined.end, Catch::Matchers::WithinAbs(4.0, 1e-12));
}

TEST_CASE("SizeRange and FloatRange cover containment and expansion helpers",
          "[runtime][range][coverage][phase3]") {
    SizeRange bytes = SizeRange::from_start_length(4u, 8u);
    REQUIRE(bytes.start == 4u);
    REQUIRE(bytes.end == 12u);
    REQUIRE(bytes.length() == 8u);
    REQUIRE(bytes.contains(4u));
    REQUIRE_FALSE(bytes.contains(12u));
    REQUIRE(bytes.contains(SizeRange(6u, 10u)));

    auto expanded = bytes.expanded(16u);
    REQUIRE(expanded == SizeRange(4u, 17u));

    FloatRange unit(0.0f, 1.0f);
    REQUIRE(unit.contains(0.25f));
    REQUIRE_FALSE(unit.contains(1.0f));

    auto overlap = unit.intersection(FloatRange(0.5f, 2.0f));
    REQUIRE_THAT(overlap.start, Catch::Matchers::WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(overlap.end, Catch::Matchers::WithinAbs(1.0f, 1e-6f));
}

TEST_CASE("Range boundary touch points remain non-intersections",
          "[runtime][range][coverage][issue-641]") {
    REQUIRE_FALSE(IntRange(0, 10).intersects(IntRange(10, 20)));
    REQUIRE(IntRange(0, 10).intersection(IntRange(10, 20)).empty());
    REQUIRE(IntRange(10, 20).intersection(IntRange(0, 10)).empty());

    REQUIRE(IntRange(5, 5).empty());
    REQUIRE(IntRange(5, 4).empty());
    REQUIRE(IntRange(5, 5).constrain(100) == 5);
}
