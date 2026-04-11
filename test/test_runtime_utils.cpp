#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/memory_mapped_file.hpp>
#include <pulp/runtime/temporary_file.hpp>
#include <pulp/runtime/dynamic_library.hpp>
#include <pulp/runtime/inter_process_lock.hpp>
#include <pulp/runtime/child_process.hpp>
#include <pulp/runtime/base64.hpp>
#include <pulp/runtime/range.hpp>
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

TEST_CASE("base64 binary round-trip", "[runtime][base64]") {
    std::vector<uint8_t> data = {0x00, 0xFF, 0x80, 0x7F, 0x01, 0xFE};
    auto encoded = base64_encode(data.data(), data.size());
    auto decoded = base64_decode(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(*decoded == data);
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

TEST_CASE("FloatRange", "[runtime][range]") {
    FloatRange r(0.0f, 1.0f);
    REQUIRE(r.contains(0.5f));
    REQUIRE_FALSE(r.contains(1.0f));
}
