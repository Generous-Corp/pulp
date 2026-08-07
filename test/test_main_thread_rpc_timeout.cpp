#include <pulp/inspect/main_thread_rpc.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;
using pulp::inspect::InspectorMainThreadRpc;
using pulp::inspect::InspectorMessage;
using pulp::inspect::make_response;

namespace {

bool may_have_applied(const InspectorMessage& message, bool expected) {
    const auto token = std::string("\"mayHaveApplied\":") + (expected ? "true" : "false");
    return message.error_data_json.find(token) != std::string::npos;
}

} // namespace

TEST_CASE("per-call timeout cancels queued work before it can apply",
          "[inspect][main-thread-rpc][timeout]") {
    std::function<void()> posted;
    std::atomic<int> operations{0};
    std::atomic<int> completions{0};
    InspectorMainThreadRpc rpc(
        {500ms, 1},
        [&](auto task) {
            posted = std::move(task);
            return true;
        },
        [] { return false; });

    const auto started_at = std::chrono::steady_clock::now();
    const auto response = rpc.call(
        101,
        [&] {
            ++operations;
            return make_response(101, "{}");
        },
        [&] { ++completions; }, 5ms);
    const auto elapsed = std::chrono::steady_clock::now() - started_at;

    REQUIRE(response.error_code == "main_thread_timeout");
    CHECK(may_have_applied(response, false));
    CHECK(elapsed < 250ms);
    CHECK(operations.load() == 0);
    CHECK(completions.load() == 1);
    REQUIRE(posted);
    posted();
    CHECK(operations.load() == 0);
    CHECK(completions.load() == 1);
}

TEST_CASE("per-call started timeout remains fenced until operation completion",
          "[inspect][main-thread-rpc][timeout]") {
    std::mutex mutex;
    std::condition_variable cv;
    std::function<void()> posted;
    bool operation_started = false;
    bool release_operation = false;
    std::atomic<int> completions{0};
    InspectorMainThreadRpc rpc(
        {500ms, 1},
        [&](auto task) {
            {
                std::lock_guard lock(mutex);
                posted = std::move(task);
            }
            cv.notify_all();
            return true;
        },
        [] { return false; });

    InspectorMessage response;
    std::thread caller([&] {
        response = rpc.call(
            102,
            [&] {
                std::unique_lock lock(mutex);
                operation_started = true;
                cv.notify_all();
                cv.wait_for(lock, 10s, [&] { return release_operation; });
                return make_response(102, "{}");
            },
            [&] { ++completions; }, 10ms);
    });
    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, 1s, [&] { return static_cast<bool>(posted); }));
    }
    std::thread executor([&] { posted(); });
    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, 1s, [&] { return operation_started; }));
    }
    caller.join();

    REQUIRE(response.error_code == "main_thread_timeout");
    CHECK(may_have_applied(response, true));
    CHECK(completions.load() == 0);
    {
        std::lock_guard lock(mutex);
        release_operation = true;
    }
    cv.notify_all();
    executor.join();
    CHECK(completions.load() == 1);
}

TEST_CASE("non-positive per-call timeout clamps without changing default config",
          "[inspect][main-thread-rpc][timeout]") {
    std::function<void()> posted;
    InspectorMainThreadRpc rpc(
        {250ms, 1},
        [&](auto task) {
            posted = std::move(task);
            return true;
        },
        [] { return false; });

    const auto started_at = std::chrono::steady_clock::now();
    const auto response = rpc.call(103, [] { return make_response(103, "{}"); }, {}, 0ms);
    const auto elapsed = std::chrono::steady_clock::now() - started_at;
    CHECK(response.error_code == "main_thread_timeout");
    CHECK(may_have_applied(response, false));
    CHECK(elapsed < 100ms);

    // The per-call clamp must not mutate the configured default timeout.
    const auto default_started = std::chrono::steady_clock::now();
    const auto default_response = rpc.call(104, [] { return make_response(104, "{}"); });
    const auto default_elapsed = std::chrono::steady_clock::now() - default_started;
    CHECK(default_response.error_code == "main_thread_timeout");
    CHECK(default_elapsed >= 100ms);
}

TEST_CASE("direct main-thread timeout is reported only after legal-thread work returns",
          "[inspect][main-thread-rpc][timeout][direct]") {
    std::atomic<int> operations{0};
    std::atomic<int> completions{0};
    InspectorMainThreadRpc rpc({500ms, 1}, {}, [] { return true; });

    const auto started_at = std::chrono::steady_clock::now();
    const auto response = rpc.call(
        105,
        [&] {
            ++operations;
            std::this_thread::sleep_for(20ms);
            return make_response(105, R"({"applied":true})");
        },
        [&] { ++completions; }, 5ms);
    const auto elapsed = std::chrono::steady_clock::now() - started_at;

    REQUIRE(response.error_code == "main_thread_timeout");
    CHECK(may_have_applied(response, true));
    CHECK(elapsed >= 20ms);
    CHECK(operations.load() == 1);
    CHECK(completions.load() == 1);
}

TEST_CASE("direct serialized timeout expires before the operation can apply",
          "[inspect][main-thread-rpc][timeout][direct]") {
    std::mutex mutex;
    std::condition_variable cv;
    bool first_started = false;
    bool release_first = false;
    std::atomic<int> second_operations{0};
    std::atomic<int> second_completions{0};
    InspectorMainThreadRpc rpc({500ms, 2}, {}, [] { return true; });

    InspectorMessage first_response;
    std::thread first([&] {
        first_response = rpc.call(
            106,
            [&] {
                std::unique_lock lock(mutex);
                first_started = true;
                cv.notify_all();
                cv.wait_for(lock, 10s, [&] { return release_first; });
                return make_response(106, "{}");
            },
            {}, 1s);
    });
    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, 1s, [&] { return first_started; }));
    }
    std::thread release([&] {
        std::this_thread::sleep_for(20ms);
        {
            std::lock_guard lock(mutex);
            release_first = true;
        }
        cv.notify_all();
    });

    const auto response = rpc.call(
        107,
        [&] {
            ++second_operations;
            return make_response(107, "{}");
        },
        [&] { ++second_completions; }, 5ms);
    release.join();
    first.join();

    CHECK_FALSE(first_response.is_error);
    REQUIRE(response.error_code == "main_thread_timeout");
    CHECK(may_have_applied(response, false));
    CHECK(second_operations.load() == 0);
    CHECK(second_completions.load() == 1);
}

TEST_CASE("queued-only RPC rejects a direct main-thread caller before apply",
          "[inspect][main-thread-rpc][timeout][direct][queued-only]") {
    std::atomic<int> operations{0};
    std::atomic<int> completions{0};
    InspectorMainThreadRpc rpc({500ms, 1}, {}, [] { return true; });

    const auto response = rpc.call_queued_only(
        108,
        [&] {
            ++operations;
            return make_response(108, "{}");
        },
        [&] { ++completions; }, 5ms);

    REQUIRE(response.error_code == "direct_main_thread_forbidden");
    CHECK(may_have_applied(response, false));
    CHECK(operations.load() == 0);
    CHECK(completions.load() == 1);
}
