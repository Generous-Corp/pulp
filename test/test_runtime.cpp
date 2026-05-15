#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/dynamic_library.hpp>
#include <pulp/runtime/runtime.hpp>
#include <pulp/runtime/temporary_file.hpp>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <utility>

using namespace pulp::runtime;

TEST_CASE("SpscQueue basic operations", "[runtime][spsc]") {
    SpscQueue<int, 16> q;
    REQUIRE(SpscQueue<int, 16>::capacity() == 16);

    SECTION("Empty queue") {
        REQUIRE(q.empty());
        REQUIRE(q.size_approx() == 0);
        REQUIRE(q.try_pop() == std::nullopt);
    }

    SECTION("Push and pop") {
        REQUIRE(q.try_push(42));
        REQUIRE_FALSE(q.empty());
        REQUIRE(q.size_approx() == 1);

        auto val = q.try_pop();
        REQUIRE(val.has_value());
        REQUIRE(val.value() == 42);
        REQUIRE(q.empty());
    }

    SECTION("FIFO order") {
        q.try_push(1);
        q.try_push(2);
        q.try_push(3);
        REQUIRE(q.try_pop().value() == 1);
        REQUIRE(q.try_pop().value() == 2);
        REQUIRE(q.try_pop().value() == 3);
    }

    SECTION("Full queue rejects push") {
        for (int i = 0; i < 16; ++i) {  // capacity 16, all 16 slots usable
            REQUIRE(q.try_push(i));
        }
        REQUIRE_FALSE(q.try_push(99));
    }
}

TEST_CASE("SpscQueue cross-thread", "[runtime][spsc]") {
    SpscQueue<int, 1024> q;
    constexpr int count = 10000;
    std::atomic<int> sum{0};

    std::thread producer([&] {
        for (int i = 0; i < count; ++i) {
            while (!q.try_push(i)) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&] {
        int received = 0;
        while (received < count) {
            auto val = q.try_pop();
            if (val) {
                sum.fetch_add(val.value(), std::memory_order_relaxed);
                ++received;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    int expected = (count - 1) * count / 2;
    REQUIRE(sum.load() == expected);
}

TEST_CASE("SpscQueue accepts rvalue pushes", "[runtime][spsc][coverage][phase3]") {
    SpscQueue<std::string, 2> q;

    REQUIRE(q.try_push(std::string("alpha")));
    REQUIRE(q.size_approx() == 1);

    auto value = q.try_pop();
    REQUIRE(value.has_value());
    REQUIRE(*value == "alpha");
    REQUIRE(q.empty());
}

TEST_CASE("ScopeGuard executes on exit", "[runtime][scope_guard]") {
    int x = 0;
    {
        auto guard = make_scope_guard([&] { x = 42; });
        REQUIRE(x == 0);
    }
    REQUIRE(x == 42);
}

TEST_CASE("ScopeGuard dismiss prevents execution", "[runtime][scope_guard]") {
    int x = 0;
    {
        auto guard = make_scope_guard([&] { x = 42; });
        guard.dismiss();
    }
    REQUIRE(x == 0);
}

TEST_CASE("ScopeGuard move transfers cleanup ownership", "[runtime][scope_guard]") {
    int calls = 0;
    {
        auto guard = make_scope_guard([&] { ++calls; });
        auto moved = std::move(guard);
        static_cast<void>(moved);
    }
    REQUIRE(calls == 1);
}

TEST_CASE("Logging does not crash", "[runtime][log]") {
    // Just verify these don't crash — output goes to stderr
    REQUIRE_NOTHROW(log_info("test message: {}", 42));
    REQUIRE_NOTHROW(log_warn("warning: {}", "something"));
    REQUIRE_NOTHROW(log_error("error: {}", 3.14));
    REQUIRE_NOTHROW(log_debug("debug: {}", true));
}

TEST_CASE("Runtime environment helpers", "[runtime][system]") {
#ifdef _WIN32
    REQUIRE(_putenv_s("PULP_RUNTIME_TEST_ENV", "value") == 0);
#else
    REQUIRE(setenv("PULP_RUNTIME_TEST_ENV", "value", 1) == 0);
#endif

    REQUIRE(get_env("PULP_RUNTIME_TEST_ENV") == std::optional<std::string>{"value"});
    REQUIRE_FALSE(get_env("PULP_RUNTIME_TEST_ENV_DOES_NOT_EXIST"));
}

TEST_CASE("Runtime GMT helper returns UTC tm", "[runtime][system]") {
    auto tm = gmtime_utc(static_cast<std::time_t>(0));
    REQUIRE(tm.tm_year == 70);
    REQUIRE(tm.tm_mon == 0);
    REQUIRE(tm.tm_mday == 1);
    REQUIRE(tm.tm_hour == 0);
    REQUIRE(tm.tm_min == 0);
    REQUIRE(tm.tm_sec == 0);
}

TEST_CASE("Runtime localtime helper returns a normalized tm", "[runtime][system][coverage][phase3]") {
    auto tm = localtime_local(static_cast<std::time_t>(0));
    REQUIRE(tm.tm_mon >= 0);
    REQUIRE(tm.tm_mon <= 11);
    REQUIRE(tm.tm_mday >= 1);
    REQUIRE(tm.tm_mday <= 31);
    REQUIRE(tm.tm_hour >= 0);
    REQUIRE(tm.tm_hour <= 23);
    REQUIRE(tm.tm_min >= 0);
    REQUIRE(tm.tm_min <= 59);
    REQUIRE(tm.tm_sec >= 0);
    REQUIRE(tm.tm_sec <= 60);
}

TEST_CASE("Runtime C string copy truncates safely", "[runtime][system]") {
    char buffer[5]{};
    copy_c_string(buffer, "abcdef");
    REQUIRE(std::string(buffer) == "abcd");
}

TEST_CASE("Runtime C string copy handles degenerate buffers", "[runtime][system]") {
    char one_byte[1]{'x'};
    copy_c_string(one_byte, sizeof(one_byte), "abc");
    REQUIRE(one_byte[0] == '\0');

    char untouched = 'x';
    copy_c_string(&untouched, 0, "abc");
    REQUIRE(untouched == 'x');

    REQUIRE_NOTHROW(copy_c_string(nullptr, 4, "abc"));
}

TEST_CASE("Runtime system info convenience helpers mirror cached info",
          "[runtime][system][coverage][phase3]") {
    const auto& info = get_system_info();
    REQUIRE_FALSE(info.os_name.empty());
    REQUIRE_FALSE(info.arch.empty());
    REQUIRE(info.cpu_threads >= 0);
    REQUIRE(cpu_thread_count() == info.cpu_threads);
    REQUIRE(total_memory_mb() == info.total_memory_mb);
}

TEST_CASE("TemporaryFile creates extensions and release preserves path", "[runtime][temporary_file]") {
    std::filesystem::path kept_path;
    {
        TemporaryFile wav("wav");
        REQUIRE(std::filesystem::exists(wav.path()));
        REQUIRE(wav.path().extension() == ".wav");
        REQUIRE(wav.path_string() == wav.path().string());

        kept_path = wav.path();
        wav.release();
    }

    REQUIRE(std::filesystem::exists(kept_path));
    std::error_code ec;
    std::filesystem::remove(kept_path, ec);
    REQUIRE_FALSE(std::filesystem::exists(kept_path));

    {
        TemporaryFile json(".json");
        REQUIRE(std::filesystem::exists(json.path()));
        REQUIRE(json.path().extension() == ".json");
    }
}

TEST_CASE("TemporaryFile move transfers cleanup ownership", "[runtime][temporary_file]") {
    std::filesystem::path moved_path;
    {
        TemporaryFile original(".tmp");
        moved_path = original.path();

        TemporaryFile moved(std::move(original));
        REQUIRE(moved.path() == moved_path);
        REQUIRE(std::filesystem::exists(moved.path()));
    }
    REQUIRE_FALSE(std::filesystem::exists(moved_path));
}

TEST_CASE("TemporaryFile move assignment cleans previous file", "[runtime][temporary_file]") {
    std::filesystem::path old_path;
    std::filesystem::path new_path;
    {
        TemporaryFile old_file(".old");
        TemporaryFile new_file(".new");
        old_path = old_file.path();
        new_path = new_file.path();

        old_file = std::move(new_file);
        REQUIRE_FALSE(std::filesystem::exists(old_path));
        REQUIRE(old_file.path() == new_path);
        REQUIRE(std::filesystem::exists(new_path));
    }
    REQUIRE_FALSE(std::filesystem::exists(new_path));
}

TEST_CASE("DynamicLibrary reports closed and failed lookup paths", "[runtime][dynamic_library]") {
    DynamicLibrary library;
    REQUIRE_FALSE(library.is_open());
    REQUIRE(library.find_symbol("definitely_missing") == nullptr);
    REQUIRE(library.error().empty());

    auto missing_path = std::filesystem::temp_directory_path() / "pulp_missing_library_for_test";
#ifdef _WIN32
    missing_path += ".dll";
#elif defined(__APPLE__)
    missing_path += ".dylib";
#else
    missing_path += ".so";
#endif

    REQUIRE_FALSE(std::filesystem::exists(missing_path));
    REQUIRE_FALSE(library.open(missing_path.string()));
    REQUIRE_FALSE(library.is_open());
    REQUIRE_FALSE(library.error().empty());

    library.close();
    REQUIRE_FALSE(library.is_open());
}

TEST_CASE("DynamicLibrary move keeps failed handles closed", "[runtime][dynamic_library]") {
    DynamicLibrary first;
    DynamicLibrary second;

    REQUIRE_FALSE(first.open("/definitely/not/a/real/library"));
    second = std::move(first);

    REQUIRE_FALSE(first.is_open());
    REQUIRE_FALSE(second.is_open());
    REQUIRE_FALSE(second.error().empty());
}

TEST_CASE("DynamicLibrary move transfers an open handle", "[runtime][dynamic_library][coverage][phase3]") {
    DynamicLibrary original;
#ifdef __APPLE__
    REQUIRE(original.open("/usr/lib/libSystem.B.dylib"));
    const char* symbol = "malloc";
#elif defined(__linux__)
    REQUIRE(original.open("libc.so.6"));
    const char* symbol = "malloc";
#else
    const char* symbol = nullptr;
    SUCCEED("No stable system library fixture on this platform.");
    return;
#endif

    DynamicLibrary moved(std::move(original));
    REQUIRE_FALSE(original.is_open());
    REQUIRE(moved.is_open());
    REQUIRE(moved.find_symbol(symbol) != nullptr);

    DynamicLibrary assigned;
    assigned = std::move(moved);
    REQUIRE_FALSE(moved.is_open());
    REQUIRE(assigned.is_open());
    REQUIRE(assigned.find_symbol(symbol) != nullptr);
}
