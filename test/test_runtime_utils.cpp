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
#include <pulp/runtime/ip_address.hpp>
#include <pulp/runtime/primes.hpp>
#include <pulp/runtime/range.hpp>
#include <pulp/runtime/scope_guard.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>
#include <pulp/runtime/text_diff.hpp>
#include "../external/cpp-httplib/httplib.h"
#include <catch2/catch_approx.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <thread>

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

// ── ScopeGuard ─────────────────────────────────────────────────────────

TEST_CASE("ScopeGuard runs once on scope exit and can be dismissed",
          "[runtime][scope_guard][coverage][phase3]") {
    int calls = 0;
    {
        auto guard = make_scope_guard([&] { ++calls; });
        REQUIRE(calls == 0);
    }
    REQUIRE(calls == 1);

    {
        auto guard = make_scope_guard([&] { ++calls; });
        guard.dismiss();
    }
    REQUIRE(calls == 1);
}

TEST_CASE("ScopeGuard move transfers ownership and disables source",
          "[runtime][scope_guard][coverage]") {
    int calls = 0;
    {
        auto first = make_scope_guard([&] { ++calls; });
        {
            auto second = std::move(first);
            REQUIRE(calls == 0);
        }
        REQUIRE(calls == 1);
    }
    REQUIRE(calls == 1);
}

TEST_CASE("ScopeGuard move transfers the active cleanup",
          "[runtime][scope_guard][coverage][phase3]") {
    int calls = 0;
    {
        auto guard = make_scope_guard([&] { ++calls; });
        auto moved = std::move(guard);
        (void)moved;
    }
    REQUIRE(calls == 1);
}

TEST_CASE("PULP_ON_SCOPE_EXIT macro creates independent guards",
          "[runtime][scope_guard][coverage]") {
    int calls = 0;
    {
        PULP_ON_SCOPE_EXIT(++calls;);
        PULP_ON_SCOPE_EXIT(calls += 10;);
        REQUIRE(calls == 0);
    }
    REQUIRE(calls == 11);
}

TEST_CASE("PULP_ON_SCOPE_EXIT macro captures local state",
          "[runtime][scope_guard][coverage][phase3]") {
    int value = 0;
    {
        PULP_ON_SCOPE_EXIT(value += 7;);
        value = 3;
    }
    REQUIRE(value == 10);
}

// ── ScopedNoAlloc ───────────────────────────────────────────────────────

TEST_CASE("ScopedNoAlloc tracks nested scope depth on the current thread",
          "[runtime][scoped_no_alloc][coverage][phase3]") {
    REQUIRE(no_alloc_scope_depth() == 0);
    REQUIRE_FALSE(is_in_no_alloc_scope());

    {
        ScopedNoAlloc outer;
#ifndef NDEBUG
        REQUIRE(no_alloc_scope_depth() == 1);
        REQUIRE(is_in_no_alloc_scope());
#else
        REQUIRE(no_alloc_scope_depth() == 0);
        REQUIRE_FALSE(is_in_no_alloc_scope());
#endif

        {
            ScopedNoAlloc inner;
#ifndef NDEBUG
            REQUIRE(no_alloc_scope_depth() == 2);
            REQUIRE(is_in_no_alloc_scope());
#else
            REQUIRE(no_alloc_scope_depth() == 0);
            REQUIRE_FALSE(is_in_no_alloc_scope());
#endif
        }

#ifndef NDEBUG
        REQUIRE(no_alloc_scope_depth() == 1);
        REQUIRE(is_in_no_alloc_scope());
#else
        REQUIRE(no_alloc_scope_depth() == 0);
        REQUIRE_FALSE(is_in_no_alloc_scope());
#endif
    }

    REQUIRE(no_alloc_scope_depth() == 0);
    REQUIRE_FALSE(is_in_no_alloc_scope());
}

TEST_CASE("ScopedNoAlloc state is thread-local",
          "[runtime][scoped_no_alloc][coverage][phase3]") {
    ScopedNoAlloc main_scope;

    int worker_initial_depth = -1;
    bool worker_initial_in_scope = true;
    int worker_nested_depth = -1;
    bool worker_nested_in_scope = false;
    std::thread worker([&] {
        worker_initial_depth = no_alloc_scope_depth();
        worker_initial_in_scope = is_in_no_alloc_scope();
        ScopedNoAlloc worker_scope;
        worker_nested_depth = no_alloc_scope_depth();
        worker_nested_in_scope = is_in_no_alloc_scope();
    });
    worker.join();

    REQUIRE(worker_initial_depth == 0);
    REQUIRE_FALSE(worker_initial_in_scope);
#ifndef NDEBUG
    REQUIRE(no_alloc_scope_depth() == 1);
    REQUIRE(is_in_no_alloc_scope());
    REQUIRE(worker_nested_depth == 1);
    REQUIRE(worker_nested_in_scope);
#else
    REQUIRE(no_alloc_scope_depth() == 0);
    REQUIRE_FALSE(is_in_no_alloc_scope());
    REQUIRE(worker_nested_depth == 0);
    REQUIRE_FALSE(worker_nested_in_scope);
#endif
}

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

TEST_CASE("ScopeGuard dismiss and move control destructor execution",
          "[runtime][scope_guard][coverage][phase3-github]") {
    int calls = 0;
    {
        auto guard = make_scope_guard([&] { ++calls; });
        guard.dismiss();
    }
    REQUIRE(calls == 0);

    {
        auto guard = make_scope_guard([&] { ++calls; });
        auto moved = std::move(guard);
        (void)moved;
    }
    REQUIRE(calls == 1);
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

TEST_CASE("MemoryMappedFile self move assignment preserves mapping",
          "[runtime][mmap][coverage][phase3]") {
    TemporaryFile tmp(".bin");
    {
        std::ofstream f(tmp.path(), std::ios::binary);
        f << "self-move";
    }

    MemoryMappedFile mmap;
    REQUIRE(mmap.open(tmp.path_string()));
    auto& ref = mmap;
    mmap = std::move(ref);

    REQUIRE(mmap.is_open());
    REQUIRE(mmap.size() == 9);
    REQUIRE(std::string(reinterpret_cast<const char*>(mmap.data()), mmap.size()) == "self-move");
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

TEST_CASE("DynamicLibrary missing symbol lookup records an error",
          "[runtime][dynlib][coverage][phase3]") {
    DynamicLibrary lib;
    REQUIRE(lib.open(system_library_path()));

    REQUIRE(lib.find_symbol("pulp_definitely_missing_symbol_12345") == nullptr);
    REQUIRE_FALSE(lib.error().empty());
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

TEST_CASE("DynamicLibrary missing symbol records an error while staying open",
          "[runtime][dynlib][coverage][phase3]") {
    DynamicLibrary lib;
    REQUIRE(lib.open(system_library_path()));
    REQUIRE(lib.is_open());

    REQUIRE(lib.find_symbol("pulp_missing_symbol_for_phase3_coverage") == nullptr);
    REQUIRE_FALSE(lib.error().empty());
    REQUIRE(lib.is_open());

    REQUIRE(lib.find_symbol(system_library_symbol()) != nullptr);
    REQUIRE(lib.is_open());
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

TEST_CASE("run_process honors working directory and preserves spaced arguments",
          "[runtime][child_process][coverage][phase3]") {
    const auto dir = std::filesystem::temp_directory_path()
        / "pulp-runtime-run-process-working-dir";
    std::filesystem::create_directories(dir);

#ifdef _WIN32
    auto result = run_process(
        "powershell",
        {"-NoProfile", "-Command",
         "Set-Content -Path marker.txt -Value ok; Write-Output 'value with spaces'; exit 0"},
        dir.string());
#else
    auto result = run_process(
        "/bin/sh",
        {"-c", "printf ok > marker.txt; printf '%s\\n' \"$1\"", "sh",
         "value with spaces"},
        dir.string());
#endif

    REQUIRE(result.has_value());
    REQUIRE(result->exit_code == 0);
    REQUIRE(std::filesystem::exists(dir / "marker.txt"));
    REQUIRE(result->stdout_output.find("value with spaces") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove(dir, ec);
}

TEST_CASE("run_process fails on nonexistent", "[runtime][child_process]") {
#ifdef _WIN32
    auto result = run_process("C:\\nonexistent_binary_12345.exe");
#else
    auto result = run_process("/tmp/nonexistent_binary_12345");
#endif
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("launch_process reports failed starts and missing pids",
          "[runtime][child_process][coverage][phase3]") {
#ifdef _WIN32
    REQUIRE(launch_process("C:\\nonexistent_binary_12345.exe") == -1);
#else
    REQUIRE(launch_process("/tmp/nonexistent_binary_12345") == -1);
#endif
    REQUIRE_FALSE(is_process_running(99999999));
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
    REQUIRE(base64_encode(nullptr, 3) == "");

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

TEST_CASE("base64 rejects URL-safe alphabet and misplaced padding across whitespace",
          "[runtime][base64][coverage][phase3-large]") {
    REQUIRE_FALSE(base64_decode("SGVsbG8-").has_value());
    REQUIRE_FALSE(base64_decode("SGVsbG8_").has_value());
    REQUIRE_FALSE(base64_decode("YQ=\n=Z").has_value());
    REQUIRE_FALSE(base64_decode("Y Q= =Z").has_value());

    auto spaced = base64_decode("\tA B A g M P 8 =\r\n");
    REQUIRE(spaced.has_value());
    REQUIRE(*spaced == std::vector<uint8_t>{0x00, 0x10, 0x20, 0x30, 0xff});
}

// ── IP address helpers ─────────────────────────────────────────────────

TEST_CASE("IPv4 validation accepts boundary octets",
          "[runtime][ip][coverage][phase3-batch742]") {
    REQUIRE(is_valid_ipv4("0.0.0.0"));
    REQUIRE(is_valid_ipv4("127.0.0.1"));
    REQUIRE(is_valid_ipv4("192.168.1.20"));
    REQUIRE(is_valid_ipv4("255.255.255.255"));
}

TEST_CASE("IPv4 validation rejects malformed octet counts",
          "[runtime][ip][coverage][phase3-batch742]") {
    REQUIRE_FALSE(is_valid_ipv4(""));
    REQUIRE_FALSE(is_valid_ipv4("127.0.0"));
    REQUIRE_FALSE(is_valid_ipv4("127.0.0.1.2"));
    REQUIRE_FALSE(is_valid_ipv4("127..0.1"));
}

TEST_CASE("IPv4 validation rejects out-of-range and signed octets",
          "[runtime][ip][coverage][phase3-batch742]") {
    REQUIRE_FALSE(is_valid_ipv4("256.0.0.1"));
    REQUIRE_FALSE(is_valid_ipv4("1.2.3.-4"));
    REQUIRE_FALSE(is_valid_ipv4("+1.2.3.4"));
    REQUIRE_FALSE(is_valid_ipv4("1.2.3.999"));
}

TEST_CASE("IPv4 validation rejects whitespace-padded addresses",
          "[runtime][ip][coverage][phase3-batch742]") {
    REQUIRE_FALSE(is_valid_ipv4(" 127.0.0.1"));
    REQUIRE_FALSE(is_valid_ipv4("127.0.0.1 "));
    REQUIRE_FALSE(is_valid_ipv4("127.0.0.1\n"));
    REQUIRE_FALSE(is_valid_ipv4("\t127.0.0.1"));
}

TEST_CASE("base64 decodes final quantum without padding",
          "[runtime][base64][coverage][phase3-github]") {
    auto one = base64_decode("TQ");
    REQUIRE(one.has_value());
    REQUIRE(*one == std::vector<uint8_t>{'M'});

    auto two = base64_decode("TWE");
    REQUIRE(two.has_value());
    REQUIRE(*two == std::vector<uint8_t>{'M', 'a'});
}

TEST_CASE("base64 decodes mixed full quartets and unpadded tails",
          "[runtime][base64][coverage][phase3]") {
    auto one_tail = base64_decode("TWFuYQ");
    REQUIRE(one_tail.has_value());
    REQUIRE(std::string(one_tail->begin(), one_tail->end()) == "Mana");

    auto two_tail = base64_decode("TWFuYWI");
    REQUIRE(two_tail.has_value());
    REQUIRE(std::string(two_tail->begin(), two_tail->end()) == "Manab");
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

TEST_CASE("Expression evaluator covers remaining built-in math functions",
          "[runtime][expression][codecov]") {
    auto builtins = evaluate("tan(0) + sqrt(9) + abs(-4) + log(e) + log10(100) + exp(0) + floor(2.9) + ceil(2.1) + round(2.5)");
    REQUIRE(builtins.has_value());
    REQUIRE(*builtins == Catch::Approx(19.0));

    auto minmax = evaluate("min(3, max(4, 9))");
    REQUIRE(minmax.has_value());
    REQUIRE(*minmax == Catch::Approx(3.0));

    auto powered = evaluate("pow(2, 5)");
    REQUIRE(powered.has_value());
    REQUIRE(*powered == Catch::Approx(32.0));

    REQUIRE(evaluate("log(e)").value_or(0.0) == Catch::Approx(1.0));
    REQUIRE(evaluate("log10(1000)").value_or(0.0) == Catch::Approx(3.0));
    REQUIRE(evaluate("exp(1)").value_or(0.0) == Catch::Approx(2.71828182845904523536));
    REQUIRE(evaluate("floor(2.9) + ceil(2.1) + round(2.5)").value_or(0.0)
            == Catch::Approx(8.0));
    REQUIRE(evaluate("max(2, 5) + pow(2, 3)").value_or(0.0) == Catch::Approx(13.0));
    REQUIRE(evaluate("tan(0)").value_or(1.0) == Catch::Approx(0.0));
}

TEST_CASE("Expression evaluator covers built-in math function variants",
          "[runtime][expression][coverage][phase3]") {
    auto value = evaluate("abs(-3) + floor(1.9) + ceil(2.1) + round(2.6)");
    REQUIRE(value.has_value());
    REQUIRE(*value == Catch::Approx(10.0));

    auto extrema = evaluate("min(7, 2) + max(7, 2) + pow(2, 3)");
    REQUIRE(extrema.has_value());
    REQUIRE(*extrema == Catch::Approx(17.0));

    auto logs = evaluate("log(e) + log10(100) + exp(0)");
    REQUIRE(logs.has_value());
    REQUIRE(*logs == Catch::Approx(4.0));
}

TEST_CASE("Expression evaluator treats division by zero as silent zero",
          "[runtime][expression][coverage][phase3]") {
    auto direct = evaluate("12 / 0");
    REQUIRE(direct.has_value());
    REQUIRE(*direct == Catch::Approx(0.0));

    auto nested = evaluate("5 + 12 / (3 - 3)");
    REQUIRE(nested.has_value());
    REQUIRE(*nested == Catch::Approx(5.0));
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
    REQUIRE_FALSE(evaluate("pow(2)").has_value());
    REQUIRE_FALSE(evaluate("max(1, 2, 3)").has_value());
    REQUIRE_FALSE(evaluate("2 + @").has_value());
}

TEST_CASE("Expression evaluator rejects malformed decimal tokens",
          "[runtime][expression][coverage][phase3]") {
    REQUIRE_FALSE(evaluate(".").has_value());
    REQUIRE_FALSE(evaluate("1..2").has_value());
    REQUIRE_FALSE(evaluate("1.2.3").has_value());
    REQUIRE_FALSE(evaluate("..5").has_value());
}

TEST_CASE("Expression evaluator handles unary plus division by zero and identifiers",
          "[runtime][expression][coverage][phase3]") {
    auto value = evaluate("+gain_1 / zero", {{"gain_1", 12.0}, {"zero", 0.0}});
    REQUIRE(value.has_value());
    REQUIRE(*value == Catch::Approx(0.0));

    REQUIRE(evaluate("_offset2 + 1", {{"_offset2", 4.5}}).value_or(0.0)
            == Catch::Approx(5.5));
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

TEST_CASE("ExpressionEvaluator rejects extra arguments for custom unary functions",
          "[runtime][expression][coverage][phase3]") {
    ExpressionEvaluator evaluator;
    evaluator.register_function("double_it", [](double value) {
        return value * 2.0;
    });

    auto valid = evaluator.evaluate("double_it(3)");
    REQUIRE(valid.has_value());
    REQUIRE(*valid == Catch::Approx(6.0));

    REQUIRE_FALSE(evaluator.evaluate("double_it(3, 99)").has_value());
}

TEST_CASE("Expression evaluator handles binary math functions",
          "[runtime][expression][coverage][phase3]") {
    auto minimum = evaluate("min(9, 4)");
    REQUIRE(minimum.has_value());
    REQUIRE(*minimum == Catch::Approx(4.0));

    auto maximum = evaluate("max(-2, 3)");
    REQUIRE(maximum.has_value());
    REQUIRE(*maximum == Catch::Approx(3.0));

    auto power = evaluate("pow(3, 3)");
    REQUIRE(power.has_value());
    REQUIRE(*power == Catch::Approx(27.0));
}

TEST_CASE("Expression evaluator tolerates whitespace around tokens",
          "[runtime][expression][coverage][phase3]") {
    auto value = evaluate("  ( 1 + 2 ) *\t3  ");
    REQUIRE(value.has_value());
    REQUIRE(*value == Catch::Approx(9.0));
}

TEST_CASE("ExpressionEvaluator replaces registered unary functions",
          "[runtime][expression][coverage][phase3]") {
    ExpressionEvaluator evaluator;
    evaluator.register_function("shape", [](double x) { return x + 1.0; });
    REQUIRE(*evaluator.evaluate("shape(4)") == Catch::Approx(5.0));

    evaluator.register_function("shape", [](double x) { return x * 2.0; });
    REQUIRE(*evaluator.evaluate("shape(4)") == Catch::Approx(8.0));
}

TEST_CASE("ExpressionEvaluator variables can be overwritten",
          "[runtime][expression][coverage][phase3]") {
    ExpressionEvaluator evaluator;
    evaluator.set("amount", 1.5);
    REQUIRE(*evaluator.evaluate("amount + 1") == Catch::Approx(2.5));

    evaluator.set("amount", -2.0);
    REQUIRE(*evaluator.get("amount") == Catch::Approx(-2.0));
    REQUIRE(*evaluator.evaluate("amount * amount") == Catch::Approx(4.0));
}

TEST_CASE("Expression evaluator rejects trailing operators",
          "[runtime][expression][coverage][phase3]") {
    REQUIRE_FALSE(evaluate("1 +").has_value());
    REQUIRE_FALSE(evaluate("2 *").has_value());
    REQUIRE_FALSE(evaluate("pow(2,)").has_value());
}

TEST_CASE("Expression evaluator handles nested function calls",
          "[runtime][expression][coverage][phase3]") {
    auto value = evaluate("max(min(10, 4), abs(-7))");
    REQUIRE(value.has_value());
    REQUIRE(*value == Catch::Approx(7.0));
}

TEST_CASE("Expression evaluator covers multi-argument builtins",
          "[runtime][expression][coverage][phase3-large]") {
    REQUIRE(evaluate("min(5, 3)") == Catch::Approx(3.0));
    REQUIRE(evaluate("max(5, 3)") == Catch::Approx(5.0));
    REQUIRE(evaluate("pow(2, 5)") == Catch::Approx(32.0));
    REQUIRE(evaluate("clamp(8, 5)") == Catch::Approx(5.0));
    REQUIRE(evaluate("clamp(-2, 5)") == Catch::Approx(0.0));
}

TEST_CASE("Expression evaluator rejects incomplete multi-argument calls",
          "[runtime][expression][coverage][phase3-large]") {
    REQUIRE_FALSE(evaluate("min(1)").has_value());
    REQUIRE_FALSE(evaluate("max(1)").has_value());
    REQUIRE_FALSE(evaluate("pow(2)").has_value());
    REQUIRE_FALSE(evaluate("clamp(2)").has_value());
}

// ── ScopeGuard ─────────────────────────────────────────────────────────

TEST_CASE("ScopeGuard dismiss and move transfer ownership",
          "[runtime][scope_guard][coverage][phase3-large]") {
    int calls = 0;
    {
        auto guard = make_scope_guard([&] { ++calls; });
        guard.dismiss();
    }
    REQUIRE(calls == 0);

    {
        auto guard = make_scope_guard([&] { ++calls; });
        auto moved = std::move(guard);
        REQUIRE(calls == 0);
    }
    REQUIRE(calls == 1);
}

TEST_CASE("PULP_ON_SCOPE_EXIT runs at block exit",
          "[runtime][scope_guard][coverage][phase3-large]") {
    int value = 0;
    {
        PULP_ON_SCOPE_EXIT(value = 42);
        REQUIRE(value == 0);
    }
    REQUIRE(value == 42);
}

TEST_CASE("Expression evaluator treats chained powers as right associative",
          "[runtime][expression][coverage]") {
    auto chained = evaluate("2 ^ 3 ^ 2");
    REQUIRE(chained.has_value());
    REQUIRE(*chained == Catch::Approx(512.0));

    auto grouped_left = evaluate("(2 ^ 3) ^ 2");
    REQUIRE(grouped_left.has_value());
    REQUIRE(*grouped_left == Catch::Approx(64.0));

    auto negative_exponent = evaluate("4 ^ 1 ^ -1");
    REQUIRE(negative_exponent.has_value());
    REQUIRE(*negative_exponent == Catch::Approx(4.0));
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

TEST_CASE("HTTP helpers accept case-insensitive URL schemes during parsing",
          "[runtime][http][url][coverage][phase3]") {
    const auto response = http_get("HTTP://127.0.0.1:1/path", 1);
    REQUIRE(response.status_code == 0);
    REQUIRE(response.error != "Invalid URL");
}

TEST_CASE("HTTP helpers round-trip against a loopback server",
          "[runtime][http][url][coverage][phase3]") {
    httplib::Server server;
    server.Get("/hello", [](const httplib::Request& request, httplib::Response& response) {
        response.set_header("X-Pulp-Test",
                            request.has_param("name") ? request.get_param_value("name") : "missing");
        response.set_content("hello", "text/plain");
    });
    server.Post("/echo", [](const httplib::Request& request, httplib::Response& response) {
        response.set_header("X-Content-Type", request.get_header_value("Content-Type"));
        response.set_content(request.body, "text/plain");
    });
    server.Get("/download", [](const httplib::Request&, httplib::Response& response) {
        response.set_content(std::string("payload\0bytes", 13), "application/octet-stream");
    });

    const int port = server.bind_to_any_port("127.0.0.1");
    REQUIRE(port > 0);

    std::thread thread([&] { server.listen_after_bind(); });
    auto stop_server = make_scope_guard([&] {
        server.stop();
        if (thread.joinable())
            thread.join();
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!server.is_running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(server.is_running());

    const auto base = std::string("http://127.0.0.1:") + std::to_string(port);
    auto get_response = http_get(base + "/hello?name=agent", 2);
    const auto get_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!get_response.ok() && std::chrono::steady_clock::now() < get_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        get_response = http_get(base + "/hello?name=agent", 2);
    }
    REQUIRE(get_response.ok());
    REQUIRE(get_response.status_code == 200);
    REQUIRE(get_response.body == "hello");
    REQUIRE(get_response.headers.at("X-Pulp-Test") == "agent");

    const auto post_response = http_post(base + "/echo", "body=42", "text/custom", 2);
    REQUIRE(post_response.ok());
    REQUIRE(post_response.body == "body=42");
    REQUIRE(post_response.headers.at("X-Content-Type").find("text/custom") != std::string::npos);

    TemporaryFile downloaded(".bin");
    REQUIRE(http_download(base + "/download", downloaded.path_string(), 2));
    std::ifstream file(downloaded.path(), std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    REQUIRE(bytes == std::string("payload\0bytes", 13));
}

TEST_CASE("local IPv4 helpers return usable fallback values",
          "[runtime][ip][coverage]") {
    const auto primary = local_ipv4_address();
    REQUIRE(is_valid_ipv4(primary));

    for (const auto& address : local_ipv4_addresses()) {
        REQUIRE(is_valid_ipv4(address));
    }

    REQUIRE_FALSE(hostname().empty());
}

// ── Primes ──────────────────────────────────────────────────────────────

TEST_CASE("prime helpers cover small values composites and sieve output",
          "[runtime][primes][coverage]") {
    REQUIRE_FALSE(is_prime(0));
    REQUIRE_FALSE(is_prime(1));
    REQUIRE(is_prime(2));
    REQUIRE(is_prime(3));
    REQUIRE_FALSE(is_prime(4));
    REQUIRE_FALSE(is_prime(21));
    REQUIRE(is_prime(97, 4));

    REQUIRE(sieve_primes(1).empty());
    REQUIRE(sieve_primes(2) == std::vector<uint32_t>{2});
    REQUIRE(sieve_primes(20) == std::vector<uint32_t>{2, 3, 5, 7, 11, 13, 17, 19});
}

TEST_CASE("generate_prime rejects impossible bit sizes and returns bounded primes",
          "[runtime][primes][coverage]") {
    REQUIRE(generate_prime(0) == 0);
    REQUIRE(generate_prime(1) == 0);
    REQUIRE(generate_prime(63) == 0);

    const auto prime = generate_prime(8);
    REQUIRE(prime >= 128);
    REQUIRE(prime <= 255);
    REQUIRE(is_prime(prime, 4));
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
    REQUIRE(format_diff(diff) ==
            "  alpha\n"
            "  beta\n");
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

TEST_CASE("text_diff treats identical multiline inputs as equal entries",
          "[runtime][text-diff][coverage][phase3]") {
    auto diff = text_diff("one\ntwo\nthree", "one\ntwo\nthree");
    REQUIRE(diff.size() == 3);
    for (const auto& entry : diff) {
        REQUIRE(entry.op == DiffOp::Equal);
    }
    REQUIRE(format_diff(diff) == "  one\n  two\n  three\n");
}

TEST_CASE("text_diff preserves leading and trailing empty lines",
          "[runtime][text-diff][coverage][phase3]") {
    auto diff = text_diff("\nbody\n", "\nbody\nnext\n");
    REQUIRE(diff.size() == 3);
    REQUIRE(diff[0].op == DiffOp::Equal);
    REQUIRE(diff[0].text.empty());
    REQUIRE(diff[1].op == DiffOp::Equal);
    REQUIRE(diff[1].text == "body");
    REQUIRE(diff[2].op == DiffOp::Insert);
    REQUIRE(diff[2].text == "next");
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

TEST_CASE("text_diff treats missing terminal newlines equivalently",
          "[runtime][text-diff][coverage][phase3]") {
    auto diff = text_diff("one\ntwo", "one\ntwo\n");

    REQUIRE(diff.size() == 2);
    REQUIRE(diff[0].op == DiffOp::Equal);
    REQUIRE(diff[0].text == "one");
    REQUIRE(diff[1].op == DiffOp::Equal);
    REQUIRE(diff[1].text == "two");
    REQUIRE(format_diff(diff) ==
            "  one\n"
            "  two\n");
}

TEST_CASE("format_diff handles manually constructed operation order",
          "[runtime][text-diff][coverage][phase3-github]") {
    std::vector<DiffEntry> diff{
        {DiffOp::Equal, "context"},
        {DiffOp::Insert, "added"},
        {DiffOp::Delete, "removed"},
    };

    REQUIRE(format_diff(diff) ==
            "  context\n"
            "+ added\n"
            "- removed\n");
}

TEST_CASE("text_diff treats trailing newline as no synthetic blank line",
          "[runtime][text-diff][coverage][phase3-large]") {
    auto diff = text_diff("alpha\nbeta\n", "alpha\nbeta");

    REQUIRE(diff.size() == 2);
    REQUIRE(diff[0].op == DiffOp::Equal);
    REQUIRE(diff[0].text == "alpha");
    REQUIRE(diff[1].op == DiffOp::Equal);
    REQUIRE(diff[1].text == "beta");
    REQUIRE(format_diff(diff) ==
            "  alpha\n"
            "  beta\n");
}

TEST_CASE("text_diff chooses deletions first for single-line replacements",
          "[runtime][text-diff][coverage]") {
    auto diff = text_diff("left", "right");

    REQUIRE(diff.size() == 2);
    REQUIRE(diff[0].op == DiffOp::Delete);
    REQUIRE(diff[0].text == "left");
    REQUIRE(diff[1].op == DiffOp::Insert);
    REQUIRE(diff[1].text == "right");
    REQUIRE(format_diff(diff) == "- left\n+ right\n");
}

TEST_CASE("text_diff ignores trailing empty line fragments",
          "[runtime][text-diff][coverage]") {
    auto diff = text_diff("same\n", "same");

    REQUIRE(diff.size() == 1);
    REQUIRE(diff[0].op == DiffOp::Equal);
    REQUIRE(diff[0].text == "same");
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

TEST_CASE("Range constrain preserves floating half-open upper bound",
          "[runtime][range][coverage][phase3]") {
    FloatRange floats(0.0f, 1.0f);
    auto constrained_float = floats.constrain(2.0f);
    REQUIRE(constrained_float < 1.0f);
    REQUIRE(constrained_float > 0.0f);
    REQUIRE(floats.contains(constrained_float));

    DoubleRange doubles(-2.0, 2.0);
    auto constrained_double = doubles.constrain(10.0);
    REQUIRE(constrained_double < 2.0);
    REQUIRE(constrained_double > 1.0);
    REQUIRE(doubles.contains(constrained_double));
    REQUIRE_THAT(doubles.constrain(-10.0), Catch::Matchers::WithinAbs(-2.0, 1e-12));
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

TEST_CASE("Floating Range constrain clamps to contained upper bound",
          "[runtime][range][coverage][phase3]") {
    FloatRange unit(0.0f, 1.0f);
    REQUIRE_THAT(unit.constrain(-2.0f), Catch::Matchers::WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(unit.constrain(0.25f), Catch::Matchers::WithinAbs(0.25f, 1e-6f));
    REQUIRE(unit.constrain(2.0f) < 1.0f);
    REQUIRE(unit.contains(unit.constrain(2.0f)));

    DoubleRange bipolar(-1.0, 1.0);
    REQUIRE_THAT(bipolar.constrain(-2.0), Catch::Matchers::WithinAbs(-1.0, 1e-12));
    REQUIRE(bipolar.constrain(2.0) < 1.0);
    REQUIRE(bipolar.contains(bipolar.constrain(2.0)));
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

TEST_CASE("Range intersection is commutative for overlapping integer ranges",
          "[runtime][range][coverage][phase3]") {
    IntRange a(-4, 8);
    IntRange b(3, 12);
    REQUIRE(a.intersection(b) == IntRange(3, 8));
    REQUIRE(b.intersection(a) == IntRange(3, 8));
}

TEST_CASE("Range enclosing union keeps reversed ranges isolated",
          "[runtime][range][coverage][phase3]") {
    IntRange reversed(8, 3);
    IntRange normal(1, 4);
    REQUIRE(reversed.empty());
    REQUIRE(reversed.enclosing_union(normal) == normal);
    REQUIRE(normal.enclosing_union(reversed) == normal);
}

TEST_CASE("Range expansion handles negative domains",
          "[runtime][range][coverage][phase3]") {
    IntRange range(-10, -4);
    REQUIRE(range.expanded(-12) == IntRange(-12, -4));
    REQUIRE(range.expanded(-1) == IntRange(-10, 0));
    REQUIRE(range.expanded(-7) == range);
}

TEST_CASE("FloatRange constrain clamps fractional values",
          "[runtime][range][coverage][phase3]") {
    FloatRange range(-1.0f, 3.0f);
    REQUIRE_THAT(range.constrain(-2.5f), Catch::Matchers::WithinAbs(-1.0f, 1e-6f));
    REQUIRE_THAT(range.constrain(0.25f), Catch::Matchers::WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(range.constrain(2.5f), Catch::Matchers::WithinAbs(2.5f, 1e-6f));
    REQUIRE_THAT(range.constrain(3.5f), Catch::Matchers::WithinAbs(3.0f, 1e-6f));
}

TEST_CASE("DoubleRange empty constrain returns start",
          "[runtime][range][coverage][phase3]") {
    DoubleRange empty(4.5, 4.5);
    REQUIRE(empty.empty());
    REQUIRE_THAT(empty.constrain(-100.0), Catch::Matchers::WithinAbs(4.5, 1e-12));
    REQUIRE_THAT(empty.constrain(100.0), Catch::Matchers::WithinAbs(4.5, 1e-12));
}

TEST_CASE("Range expanded covers negative fractional values",
          "[runtime][range][coverage][phase3-github]") {
    DoubleRange empty(2.0, 2.0);
    auto seeded = empty.expanded(-1.5);
    REQUIRE_THAT(seeded.start, Catch::Matchers::WithinAbs(-1.5, 1e-12));
    REQUIRE_THAT(seeded.end, Catch::Matchers::WithinAbs(-0.5, 1e-12));

    auto expanded = DoubleRange(-2.0, -1.0).expanded(-3.25);
    REQUIRE_THAT(expanded.start, Catch::Matchers::WithinAbs(-3.25, 1e-12));
    REQUIRE_THAT(expanded.end, Catch::Matchers::WithinAbs(-1.0, 1e-12));
}
