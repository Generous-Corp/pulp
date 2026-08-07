#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/control_main_thread_executor.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace pulp::inspect;
using namespace std::chrono_literals;

namespace {

ControlAdmissionPlan plan() {
    ControlAdmissionPlan value;
    value.deadline_unix_ms = 4'102'444'800'000;
    return value;
}

ControlRequestEnvelope request() {
    return {};
}

ControlExecutionOutcome invoke(const ControlOperationExecutor& executor) {
    return executor(plan(), request(),
                    ControlExecutionContext{
                        .checkpoint = [] { return ControlExecutionCheckpoint::Continue; },
                    });
}

struct PostedQueue {
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<std::function<void()>> tasks;

    bool post(std::function<void()> task) {
        {
            std::lock_guard lock(mutex);
            tasks.push_back(std::move(task));
        }
        condition.notify_all();
        return true;
    }

    bool wait_for_size(std::size_t size) {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, 2s, [&] { return tasks.size() >= size; });
    }

    std::function<void()> take(std::size_t index = 0) {
        std::lock_guard lock(mutex);
        auto task = std::move(tasks.at(index));
        tasks.erase(tasks.begin() + static_cast<std::ptrdiff_t>(index));
        return task;
    }

    void clear() {
        std::lock_guard lock(mutex);
        tasks.clear();
    }
};

} // namespace

TEST_CASE("control main-thread executor returns the legal-thread outcome",
          "[inspect][control][main-thread][executor]") {
    PostedQueue queue;
    auto rpc = std::make_shared<InspectorMainThreadRpc>(
        InspectorMainThreadRpc::Config{1s, 4},
        [&](auto task) { return queue.post(std::move(task)); }, [] { return false; });
    const auto main_thread = std::this_thread::get_id();
    std::thread::id handler_thread;
    ControlMainThreadExecutor adapter{
        rpc,
        [&](const auto&, const auto&, const auto&) {
            handler_thread = std::this_thread::get_id();
            return ControlExecutionOutcome{
                .terminal_state = ControlReceiptState::Completed,
                .result = {.detail_json = R"({"applied":true})"},
            };
        },
    };

    ControlExecutionOutcome result;
    std::thread caller([&] { result = invoke(adapter.executor()); });
    const bool posted = queue.wait_for_size(1);
    CHECK(posted);
    if (!posted) {
        rpc->cancel();
        caller.join();
        return;
    }
    queue.take()();
    caller.join();

    CHECK(handler_thread == main_thread);
    CHECK(result.terminal_state == ControlReceiptState::Completed);
    CHECK(result.result.detail_json == R"({"applied":true})");
}

TEST_CASE("control main-thread executor fails closed for a direct main-thread caller",
          "[inspect][control][main-thread][executor][direct][security]") {
    std::atomic<unsigned> calls{0};
    auto rpc = std::make_shared<InspectorMainThreadRpc>(
        InspectorMainThreadRpc::Config{1s, 1}, [](auto) { return true; }, [] { return true; });
    ControlMainThreadExecutor adapter{
        rpc,
        [&](const auto&, const auto&, const auto&) {
            ++calls;
            return ControlExecutionOutcome{};
        },
    };

    const auto result = invoke(adapter.executor());
    REQUIRE(result.result.result_code.has_value());
    CHECK(result.terminal_state == ControlReceiptState::Failed);
    CHECK(*result.result.result_code == ControlResultCode::HostUnavailable);
    CHECK(result.result.retry == ControlRetryClassification::AfterBackoff);
    CHECK(calls.load() == 0);
}

TEST_CASE("queued main-thread timeout is safely failed before apply",
          "[inspect][control][main-thread][executor][timeout]") {
    PostedQueue queue;
    auto rpc = std::make_shared<InspectorMainThreadRpc>(
        InspectorMainThreadRpc::Config{5ms, 1},
        [&](auto task) { return queue.post(std::move(task)); }, [] { return false; });
    std::atomic<unsigned> calls{0};
    ControlMainThreadExecutor adapter{
        rpc,
        [&](const auto&, const auto&, const auto&) {
            ++calls;
            return ControlExecutionOutcome{};
        },
    };

    const auto result = invoke(adapter.executor());
    REQUIRE(result.result.result_code.has_value());
    CHECK(result.terminal_state == ControlReceiptState::Failed);
    CHECK(*result.result.result_code == ControlResultCode::DeadlineExceeded);
    CHECK(calls.load() == 0);

    REQUIRE(queue.wait_for_size(1));
    queue.take()();
    CHECK(calls.load() == 0);
}

TEST_CASE("operation deadline caps the main-thread RPC timeout",
          "[inspect][control][main-thread][executor][deadline]") {
    PostedQueue queue;
    auto rpc = std::make_shared<InspectorMainThreadRpc>(
        InspectorMainThreadRpc::Config{1s, 1},
        [&](auto task) { return queue.post(std::move(task)); }, [] { return false; });
    std::atomic<unsigned> calls{0};
    ControlMainThreadExecutor adapter{
        rpc,
        [&](const auto&, const auto&, const auto&) {
            ++calls;
            return ControlExecutionOutcome{};
        },
    };
    auto expiring_plan = plan();
    expiring_plan.deadline_unix_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            (std::chrono::system_clock::now() + 10ms).time_since_epoch())
            .count();

    const auto result =
        adapter.executor()(expiring_plan, request(),
                           ControlExecutionContext{
                               .checkpoint = [] { return ControlExecutionCheckpoint::Continue; },
                           });
    REQUIRE(result.result.result_code.has_value());
    CHECK(result.terminal_state == ControlReceiptState::Failed);
    CHECK(*result.result.result_code == ControlResultCode::DeadlineExceeded);
    CHECK(calls.load() == 0);

    REQUIRE(queue.wait_for_size(1));
    queue.take()();
    CHECK(calls.load() == 0);
}

TEST_CASE("started main-thread timeout becomes unknown until refresh",
          "[inspect][control][main-thread][executor][timeout][fence]") {
    PostedQueue queue;
    auto rpc = std::make_shared<InspectorMainThreadRpc>(
        InspectorMainThreadRpc::Config{10ms, 1},
        [&](auto task) { return queue.post(std::move(task)); }, [] { return false; });
    std::mutex mutex;
    std::condition_variable condition;
    bool started = false;
    bool release = false;
    std::atomic<unsigned> progress_deliveries{0};
    std::atomic<bool> late_progress_accepted{true};
    std::atomic<unsigned> deferred_completions{0};
    std::optional<ControlExecutionOutcome> deferred_outcome;
    ControlMainThreadExecutor adapter{
        rpc,
        [&](const auto&, const auto&, const auto& context) {
            std::unique_lock lock(mutex);
            started = true;
            condition.notify_all();
            condition.wait_for(lock, 10s, [&] { return release; });
            lock.unlock();
            late_progress_accepted.store(context.report_progress(1, 1, R"({"late":true})"),
                                         std::memory_order_release);
            return ControlExecutionOutcome{};
        },
    };

    ControlExecutionOutcome result;
    std::thread caller([&] {
        result = adapter.executor()(
            plan(), request(),
            ControlExecutionContext{
                .report_progress =
                    [&](std::uint64_t, std::uint64_t, std::string) {
                        ++progress_deliveries;
                        return true;
                    },
                .checkpoint = [] { return ControlExecutionCheckpoint::Continue; },
                .complete_deferred =
                    [&](ControlExecutionOutcome outcome) {
                        deferred_outcome = std::move(outcome);
                        ++deferred_completions;
                    },
            });
    });
    const bool posted = queue.wait_for_size(1);
    CHECK(posted);
    if (!posted) {
        rpc->cancel();
        caller.join();
        return;
    }
    auto task = queue.take();
    std::thread main_thread([task = std::move(task)]() mutable { task(); });
    bool operation_started = false;
    {
        std::unique_lock lock(mutex);
        operation_started = condition.wait_for(lock, 2s, [&] { return started; });
    }
    CHECK(operation_started);
    if (!operation_started) {
        {
            std::lock_guard lock(mutex);
            release = true;
        }
        condition.notify_all();
        main_thread.join();
        caller.join();
        return;
    }
    caller.join();

    REQUIRE(result.result.result_code.has_value());
    CHECK(result.terminal_state == ControlReceiptState::UnknownNeedsRefresh);
    CHECK(*result.result.result_code == ControlResultCode::UnknownNeedsRefresh);
    CHECK(result.result.retry == ControlRetryClassification::AfterRefresh);
    CHECK(result.deferred);
    CHECK(deferred_completions.load() == 0);

    {
        std::lock_guard lock(mutex);
        release = true;
    }
    condition.notify_all();
    main_thread.join();
    CHECK_FALSE(late_progress_accepted.load(std::memory_order_acquire));
    CHECK(progress_deliveries.load() == 0);
    REQUIRE(deferred_outcome.has_value());
    CHECK(deferred_outcome->terminal_state == ControlReceiptState::Completed);
    CHECK(deferred_completions.load() == 1);
}

TEST_CASE("started main-thread throw settles exactly once after the timeout fence",
          "[inspect][control][main-thread][executor][timeout][fence][exception]") {
    PostedQueue queue;
    auto rpc = std::make_shared<InspectorMainThreadRpc>(
        InspectorMainThreadRpc::Config{10ms, 1},
        [&](auto task) { return queue.post(std::move(task)); }, [] { return false; });
    std::mutex mutex;
    std::condition_variable condition;
    bool started = false;
    bool release = false;
    std::atomic<unsigned> deferred_completions{0};
    std::optional<ControlExecutionOutcome> deferred_outcome;
    ControlMainThreadExecutor adapter{
        rpc,
        [&](const auto&, const auto&, const auto&) -> ControlExecutionOutcome {
            std::unique_lock lock(mutex);
            started = true;
            condition.notify_all();
            condition.wait_for(lock, 10s, [&] { return release; });
            throw std::runtime_error("late handler failure");
        },
    };

    ControlExecutionOutcome result;
    std::thread caller([&] {
        result = adapter.executor()(
            plan(), request(),
            ControlExecutionContext{
                .checkpoint = [] { return ControlExecutionCheckpoint::Continue; },
                .complete_deferred =
                    [&](ControlExecutionOutcome outcome) {
                        deferred_outcome = std::move(outcome);
                        ++deferred_completions;
                    },
            });
    });
    REQUIRE(queue.wait_for_size(1));
    auto task = queue.take();
    std::thread main_thread([task = std::move(task)]() mutable { task(); });
    {
        std::unique_lock lock(mutex);
        REQUIRE(condition.wait_for(lock, 2s, [&] { return started; }));
    }
    caller.join();
    CHECK(result.deferred);
    CHECK(result.terminal_state == ControlReceiptState::UnknownNeedsRefresh);
    CHECK(deferred_completions.load() == 0);

    {
        std::lock_guard lock(mutex);
        release = true;
    }
    condition.notify_all();
    main_thread.join();
    REQUIRE(deferred_outcome.has_value());
    CHECK(deferred_outcome->terminal_state == ControlReceiptState::Failed);
    CHECK(deferred_outcome->result.result_code == ControlResultCode::InternalError);
    CHECK(deferred_outcome->result.explanation == "late handler failure");
    CHECK(deferred_completions.load() == 1);
}

TEST_CASE("main-thread handler cannot create an unowned nested deferral",
          "[inspect][control][main-thread][executor][deferred]") {
    PostedQueue queue;
    auto rpc = std::make_shared<InspectorMainThreadRpc>(
        InspectorMainThreadRpc::Config{1s, 1},
        [&](auto task) { return queue.post(std::move(task)); }, [] { return false; });
    ControlMainThreadExecutor adapter{
        rpc,
        [](const auto&, const auto&, const auto&) {
            return ControlExecutionOutcome{.deferred = true};
        },
    };

    ControlExecutionOutcome result;
    std::thread caller([&] { result = invoke(adapter.executor()); });
    REQUIRE(queue.wait_for_size(1));
    queue.take()();
    caller.join();

    CHECK_FALSE(result.deferred);
    CHECK(result.terminal_state == ControlReceiptState::Failed);
    CHECK(result.result.result_code == ControlResultCode::InternalError);
}

TEST_CASE("main-thread queue pressure and teardown retain distinct outcomes",
          "[inspect][control][main-thread][executor][capacity][teardown]") {
    PostedQueue queue;
    auto rpc = std::make_shared<InspectorMainThreadRpc>(
        InspectorMainThreadRpc::Config{1s, 1},
        [&](auto task) { return queue.post(std::move(task)); }, [] { return false; });
    ControlMainThreadExecutor adapter{
        rpc,
        [](const auto&, const auto&, const auto&) { return ControlExecutionOutcome{}; },
    };

    ControlExecutionOutcome first;
    std::thread caller([&] { first = invoke(adapter.executor()); });
    const bool posted = queue.wait_for_size(1);
    CHECK(posted);
    if (!posted) {
        rpc->cancel();
        caller.join();
        return;
    }

    const auto full = invoke(adapter.executor());
    REQUIRE(full.result.result_code.has_value());
    CHECK(full.terminal_state == ControlReceiptState::Failed);
    CHECK(*full.result.result_code == ControlResultCode::ResourceExhausted);
    CHECK(full.result.retry == ControlRetryClassification::AfterBackoff);

    rpc->cancel();
    caller.join();
    REQUIRE(first.result.result_code.has_value());
    CHECK(first.terminal_state == ControlReceiptState::Cancelled);
    CHECK(*first.result.result_code == ControlResultCode::Cancelled);

    const auto teardown = invoke(adapter.executor());
    REQUIRE(teardown.result.result_code.has_value());
    CHECK(teardown.terminal_state == ControlReceiptState::Cancelled);
    CHECK(*teardown.result.result_code == ControlResultCode::Cancelled);
    queue.clear();
}
