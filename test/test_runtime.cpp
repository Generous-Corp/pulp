#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/budget_policy.hpp>
#include <pulp/runtime/dynamic_library.hpp>
#include <pulp/runtime/high_resolution_timer.hpp>
#include <pulp/runtime/range.hpp>
#include <pulp/runtime/runtime.hpp>
#include <pulp/runtime/temporary_file.hpp>
#include "harness/rt_allocation_probe.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace pulp::runtime;
using namespace std::chrono_literals;

namespace {

class ScopedEnvVar {
public:
    explicit ScopedEnvVar(const char* name)
        : name_(name)
        , previous_(get_env(name)) {}

    ~ScopedEnvVar() {
        if (previous_) {
#ifdef _WIN32
            static_cast<void>(_putenv_s(name_, previous_->c_str()));
#else
            static_cast<void>(setenv(name_, previous_->c_str(), 1));
#endif
        } else {
#ifdef _WIN32
            static_cast<void>(_putenv_s(name_, ""));
#else
            static_cast<void>(unsetenv(name_));
#endif
        }
    }

    void set(std::string_view value) const {
        std::string owned(value);
#ifdef _WIN32
        REQUIRE(_putenv_s(name_, owned.c_str()) == 0);
#else
        REQUIRE(setenv(name_, owned.c_str(), 1) == 0);
#endif
    }

    void unset() const {
#ifdef _WIN32
        REQUIRE(_putenv_s(name_, "") == 0);
#else
        REQUIRE(unsetenv(name_) == 0);
#endif
    }

private:
    const char* name_;
    std::optional<std::string> previous_;
};

} // namespace

TEST_CASE("RuntimeBudgetPolicy keeps critical audio running",
          "[runtime][budget-policy][phase4]") {
    RuntimeBudgetRequest request;
    request.lane = RuntimeWorkLane::CriticalAudio;
    request.estimated_cost = 100;
    request.remaining_budget = 0;
    request.overload_active = true;

    auto decision = evaluate_runtime_budget(request);

    REQUIRE(decision.action == RuntimeBudgetAction::Run);
    REQUIRE(decision.should_run());
    REQUIRE(std::string(to_string(decision.action)) == "run");
    REQUIRE(std::string(decision.reason) == "critical-audio");
}

TEST_CASE("RuntimeBudgetPolicy preserves reserve for interactive work",
          "[runtime][budget-policy][phase4]") {
    RuntimeBudgetPolicy policy;
    policy.critical_audio_reserve = 128;

    RuntimeBudgetRequest request;
    request.lane = RuntimeWorkLane::Interactive;
    request.estimated_cost = 64;
    request.remaining_budget = 256;

    auto decision = evaluate_runtime_budget(request, policy);
    REQUIRE(decision.action == RuntimeBudgetAction::Run);

    request.remaining_budget = 160;
    decision = evaluate_runtime_budget(request, policy);
    REQUIRE(decision.action == RuntimeBudgetAction::Defer);
    REQUIRE_FALSE(decision.should_run());
    REQUIRE(std::string(to_string(decision.action)) == "defer");
    REQUIRE(std::string(decision.reason) == "interactive-defer");
}

TEST_CASE("RuntimeBudgetPolicy sheds opportunistic work under pressure",
          "[runtime][budget-policy][phase4]") {
    RuntimeBudgetRequest request;
    request.lane = RuntimeWorkLane::Opportunistic;
    request.estimated_cost = 8;
    request.remaining_budget = 1024;
    request.overload_active = true;

    auto decision = evaluate_runtime_budget(request);

    REQUIRE(decision.action == RuntimeBudgetAction::Shed);
    REQUIRE_FALSE(decision.should_run());
    REQUIRE(std::string(to_string(decision.action)) == "shed");
    REQUIRE(std::string(decision.reason) == "overload-shed-opportunistic");
}

TEST_CASE("RuntimeBudgetPolicy makes background degradation explicit",
          "[runtime][budget-policy][phase4]") {
    RuntimeBudgetRequest request;
    request.lane = RuntimeWorkLane::Background;
    request.estimated_cost = 200;
    request.remaining_budget = 100;

    auto decision = evaluate_runtime_budget(request);
    REQUIRE(decision.action == RuntimeBudgetAction::Bypass);
    REQUIRE_FALSE(decision.should_run());
    REQUIRE(std::string(to_string(decision.action)) == "bypass");
    REQUIRE(std::string(decision.reason) == "budget-bypass-background");

    request.required = true;
    decision = evaluate_runtime_budget(request);
    REQUIRE(decision.action == RuntimeBudgetAction::Defer);
    REQUIRE(std::string(decision.reason) == "required-defer");

    RuntimeBudgetPolicy policy;
    policy.shed_background_on_overload = true;
    request.required = false;
    request.overload_active = true;
    request.remaining_budget = 1000;
    decision = evaluate_runtime_budget(request, policy);
    REQUIRE(decision.action == RuntimeBudgetAction::Shed);
    REQUIRE(std::string(decision.reason) == "overload-shed-background");
}

TEST_CASE("RuntimeBudgetFrame consumes budget and records degradation",
          "[runtime][budget-policy][phase4][consumer]") {
    RuntimeBudgetPolicy policy;
    policy.critical_audio_reserve = 32;

    RuntimeBudgetFrame frame(128, policy);

    auto decision = frame.evaluate(RuntimeWorkLane::Interactive, 48);
    REQUIRE(decision.action == RuntimeBudgetAction::Run);
    REQUIRE(frame.remaining_budget() == 80);

    decision = frame.evaluate(RuntimeWorkLane::Interactive, 64);
    REQUIRE(decision.action == RuntimeBudgetAction::Defer);
    REQUIRE(frame.remaining_budget() == 80);

    decision = frame.evaluate(RuntimeWorkLane::Background, 96);
    REQUIRE(decision.action == RuntimeBudgetAction::Bypass);

    decision = frame.evaluate(RuntimeWorkLane::Opportunistic, 96);
    REQUIRE(decision.action == RuntimeBudgetAction::Shed);

    const auto& stats = frame.stats();
    REQUIRE(stats.run_count == 1);
    REQUIRE(stats.defer_count == 1);
    REQUIRE(stats.bypass_count == 1);
    REQUIRE(stats.shed_count == 1);
    REQUIRE(stats.consumed_budget == 48);
    REQUIRE(stats.remaining_budget == 80);
}

TEST_CASE("RuntimeBudgetFrame supports overload and refill hooks",
          "[runtime][budget-policy][phase4][consumer]") {
    RuntimeBudgetPolicy policy;
    policy.shed_background_on_overload = true;

    RuntimeBudgetFrame frame(16, policy);
    frame.set_overload_active(true);

    auto decision = frame.evaluate(RuntimeWorkLane::Background, 1);
    REQUIRE(decision.action == RuntimeBudgetAction::Shed);
    REQUIRE(std::string(decision.reason) == "overload-shed-background");

    frame.set_overload_active(false);
    frame.add_budget(32);
    decision = frame.evaluate(RuntimeWorkLane::Background, 40);
    REQUIRE(decision.action == RuntimeBudgetAction::Run);
    REQUIRE(frame.remaining_budget() == 8);

    frame.add_budget(std::numeric_limits<std::uint64_t>::max());
    REQUIRE(frame.remaining_budget() == std::numeric_limits<std::uint64_t>::max());

    RuntimeBudgetFrame saturating(0);
    REQUIRE(saturating.evaluate(RuntimeWorkLane::CriticalAudio,
                                std::numeric_limits<std::uint64_t>::max()).should_run());
    REQUIRE(saturating.evaluate(RuntimeWorkLane::CriticalAudio, 1).should_run());
    REQUIRE(saturating.stats().consumed_budget == std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE("RuntimeBudgetFrame hot path is allocation-free",
          "[runtime][budget-policy][phase4][consumer][rt]") {
    RuntimeBudgetPolicy policy;
    policy.critical_audio_reserve = 16;
    RuntimeBudgetFrame frame(512, policy);

    pulp::test::RtAllocationProbe probe;
    for (int i = 0; i < 64; ++i) {
        const auto decision = frame.evaluate(
            i % 2 == 0 ? RuntimeWorkLane::Interactive : RuntimeWorkLane::Opportunistic,
            4);
        (void)decision;
    }

    REQUIRE_FALSE(probe.saw_allocation());
    REQUIRE(probe.allocation_count() == 0);
    REQUIRE(probe.allocated_bytes() == 0);
}

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

TEST_CASE("BackgroundJobService runs higher priority queued work first",
          "[runtime][background-job][phase2]") {
    BackgroundJobService service;
    std::mutex mutex;
    std::condition_variable cv;
    bool first_started = false;
    bool release_first = false;
    std::vector<std::string> order;

    auto first = service.submit(
        {.name = "first", .priority = BackgroundJobPriority::normal},
        [&](BackgroundJobContext&) {
            std::unique_lock lock(mutex);
            first_started = true;
            cv.notify_all();
            cv.wait(lock, [&] { return release_first; });
            order.push_back("first");
        });

    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, 2s, [&] { return first_started; }));
    }

    auto low = service.submit(
        {.name = "low", .priority = BackgroundJobPriority::low},
        [&](BackgroundJobContext&) {
            std::lock_guard lock(mutex);
            order.push_back("low");
        });
    auto high = service.submit(
        {.name = "high", .priority = BackgroundJobPriority::high},
        [&](BackgroundJobContext&) {
            std::lock_guard lock(mutex);
            order.push_back("high");
        });

    REQUIRE(service.pending_count() == 2);

    {
        std::lock_guard lock(mutex);
        release_first = true;
    }
    cv.notify_all();

    first.wait();
    high.wait();
    low.wait();

    std::lock_guard lock(mutex);
    REQUIRE(order.size() == 3);
    REQUIRE(order[0] == "first");
    REQUIRE(order[1] == "high");
    REQUIRE(order[2] == "low");
}

TEST_CASE("BackgroundJobService publishes progress and observes cancellation",
          "[runtime][background-job][cancel][phase2]") {
    BackgroundJobService service;
    std::atomic<bool> job_started{false};
    std::atomic<bool> saw_cancel{false};

    auto handle = service.submit(
        {.name = "cancel", .priority = BackgroundJobPriority::normal},
        [&](BackgroundJobContext& context) {
            job_started.store(true, std::memory_order_release);
            context.publish_progress({.completed = 1, .total = 4, .label = "loading"});
            while (!context.is_cancelled()) {
                std::this_thread::sleep_for(1ms);
            }
            saw_cancel.store(true, std::memory_order_release);
        });

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    std::optional<BackgroundJobProgress> progress;
    while (std::chrono::steady_clock::now() < deadline) {
        progress = handle.latest_progress();
        if (job_started.load(std::memory_order_acquire) && progress.has_value()) break;
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(job_started.load(std::memory_order_acquire));
    REQUIRE(progress.has_value());
    REQUIRE(progress->completed == 1);
    REQUIRE(progress->total == 4);
    REQUIRE(progress->label == "loading");

    handle.cancel();
    handle.wait();
    REQUIRE(handle.is_cancelled());
    REQUIRE(saw_cancel.load(std::memory_order_acquire));
}

TEST_CASE("BackgroundJobService cancels and drains queued work on teardown",
          "[runtime][background-job][cancel][lifetime][phase2]") {
    BackgroundJobService service;
    std::mutex mutex;
    std::condition_variable cv;
    bool first_started = false;
    bool release_first = false;
    std::atomic<bool> queued_ran{false};

    auto first = service.submit(
        {.name = "running", .priority = BackgroundJobPriority::normal},
        [&](BackgroundJobContext& context) {
            std::unique_lock lock(mutex);
            first_started = true;
            cv.notify_all();
            cv.wait(lock, [&] {
                return release_first || context.is_cancelled();
            });
        });

    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, 2s, [&] { return first_started; }));
    }

    auto queued_low = service.submit(
        {.name = "queued-low", .priority = BackgroundJobPriority::low},
        [&](BackgroundJobContext&) {
            queued_ran.store(true, std::memory_order_release);
        });
    auto queued_high = service.submit(
        {.name = "queued-high", .priority = BackgroundJobPriority::high},
        [&](BackgroundJobContext&) {
            queued_ran.store(true, std::memory_order_release);
        });

    REQUIRE(service.pending_count() == 2);

    service.cancel_all();
    {
        std::lock_guard lock(mutex);
        release_first = true;
    }
    cv.notify_all();
    service.wait_all();

    REQUIRE(first.is_finished());
    REQUIRE(first.is_cancelled());
    REQUIRE(queued_low.is_finished());
    REQUIRE(queued_low.is_cancelled());
    REQUIRE(queued_high.is_finished());
    REQUIRE(queued_high.is_cancelled());
    REQUIRE_FALSE(queued_ran.load(std::memory_order_acquire));
    REQUIRE(service.pending_count() == 0);
}

TEST_CASE("RealtimeResourceSlot publishes prepared resources with deferred reclaim",
          "[runtime][background-job][rt-handoff][phase2]") {
    struct PreparedResource {
        int value = 0;
    };

    RealtimeResourceSlot<PreparedResource, 4> slot;
    REQUIRE(slot.get() == nullptr);

    REQUIRE(slot.publish(std::make_unique<PreparedResource>(PreparedResource{1})));
    REQUIRE(slot.get() != nullptr);
    REQUIRE(slot.get()->value == 1);

    REQUIRE(slot.publish(std::make_unique<PreparedResource>(PreparedResource{2})));
    REQUIRE(slot.get() != nullptr);
    REQUIRE(slot.get()->value == 2);
    REQUIRE(slot.retired_count_approx() == 1);
    REQUIRE(slot.reclaim_retired() == 1);
    REQUIRE(slot.retired_count_approx() == 0);
}

TEST_CASE("RealtimeResourceSlot reports retire queue pressure",
          "[runtime][background-job][rt-handoff][telemetry][phase2]") {
    struct PreparedResource {
        int value = 0;
    };

    RealtimeResourceSlot<PreparedResource, 1> slot;

    REQUIRE(slot.publish(std::make_unique<PreparedResource>(PreparedResource{1})));
    REQUIRE(slot.publish(std::make_unique<PreparedResource>(PreparedResource{2})));
    REQUIRE(slot.retired_count_approx() == 1);
    REQUIRE(slot.retire_overflow_count() == 0);

    REQUIRE_FALSE(slot.publish(std::make_unique<PreparedResource>(PreparedResource{3})));
    REQUIRE(slot.get() != nullptr);
    REQUIRE(slot.get()->value == 2);
    REQUIRE(slot.retired_count_approx() == 1);
    REQUIRE(slot.retire_overflow_count() == 1);

    REQUIRE(slot.reclaim_retired() == 1);
    REQUIRE(slot.publish(std::make_unique<PreparedResource>(PreparedResource{3})));
    REQUIRE(slot.get() != nullptr);
    REQUIRE(slot.get()->value == 3);
    REQUIRE(slot.retire_overflow_count() == 1);
}

TEST_CASE("RealtimeResourceSlot audio read path allocates zero times",
          "[runtime][background-job][rt-handoff][rt-safety][phase2]") {
    struct PreparedResource {
        int value = 0;
    };

    RealtimeResourceSlot<PreparedResource, 2> slot;
    REQUIRE(slot.publish(std::make_unique<PreparedResource>(PreparedResource{42})));

    pulp::test::RtAllocationProbe probe;
    const PreparedResource* resource = nullptr;
    for (int i = 0; i < 256; ++i) {
        resource = slot.get();
        REQUIRE(resource != nullptr);
        REQUIRE(resource->value == 42);
    }

    REQUIRE_FALSE(probe.saw_allocation());
}

TEST_CASE("Background resource job publishes immutable audio resource",
          "[runtime][background-job][resource-recipe][phase2]") {
    struct PreparedResource {
        std::array<float, 4> values{};
        std::string source_id;
    };

    BackgroundJobService service;
    RealtimeResourceSlot<PreparedResource, 2> slot;
    std::atomic<bool> publish_result{false};

    auto handle = service.submit(
        {.name = "prepare-ir", .priority = BackgroundJobPriority::normal},
        [&](BackgroundJobContext& context) {
            context.publish_progress({.completed = 1, .total = 2, .label = "decode"});

            auto prepared = std::make_unique<PreparedResource>();
            prepared->values = {0.25f, 0.5f, 0.125f, 0.0625f};
            prepared->source_id = "factory-short-room";

            context.publish_progress({.completed = 2, .total = 2, .label = "publish"});
            if (!context.is_cancelled()) {
                publish_result.store(slot.publish(std::move(prepared)),
                                     std::memory_order_release);
            }
        });

    handle.wait();
    REQUIRE(publish_result.load(std::memory_order_acquire));

    auto progress = handle.latest_progress();
    REQUIRE(progress.has_value());
    REQUIRE(progress->completed == 2);
    REQUIRE(progress->total == 2);
    REQUIRE(progress->label == "publish");

    const PreparedResource* resource = nullptr;
    {
        pulp::test::RtAllocationProbe probe;
        resource = slot.get();
        REQUIRE(resource != nullptr);
        REQUIRE(resource->values[0] == 0.25f);
        REQUIRE(resource->values[1] == 0.5f);
        REQUIRE_FALSE(probe.saw_allocation());
    }

    REQUIRE(resource->source_id == "factory-short-room");
}

TEST_CASE("Cancelled background resource job does not publish stale resource",
          "[runtime][background-job][resource-recipe][cancel][phase2]") {
    struct PreparedResource {
        int revision = 0;
    };

    BackgroundJobService service;
    RealtimeResourceSlot<PreparedResource, 2> slot;
    std::atomic<bool> job_started{false};
    std::atomic<bool> publish_attempted{false};

    auto handle = service.submit(
        {.name = "cancelled-sample-import", .priority = BackgroundJobPriority::normal},
        [&](BackgroundJobContext& context) {
            job_started.store(true, std::memory_order_release);
            while (!context.is_cancelled()) {
                std::this_thread::sleep_for(1ms);
            }

            auto prepared = std::make_unique<PreparedResource>();
            prepared->revision = 7;
            if (!context.is_cancelled()) {
                publish_attempted.store(slot.publish(std::move(prepared)),
                                        std::memory_order_release);
            }
        });

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!job_started.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }

    REQUIRE(job_started.load(std::memory_order_acquire));
    handle.cancel();
    handle.wait();

    REQUIRE(handle.is_cancelled());
    REQUIRE_FALSE(publish_attempted.load(std::memory_order_acquire));
    REQUIRE(slot.get() == nullptr);
}

TEST_CASE("SpscQueue reuses slots after wrap-around",
          "[runtime][spsc][coverage][issue-641]") {
    SpscQueue<int, 4> q;
    REQUIRE(q.capacity() == 4);

    for (int cycle = 0; cycle < 3; ++cycle) {
        REQUIRE(q.empty());
        for (int i = 0; i < 4; ++i) {
            REQUIRE(q.try_push(cycle * 10 + i));
        }
        REQUIRE_FALSE(q.try_push(999));
        REQUIRE(q.size_approx() == 4);
        for (int i = 0; i < 4; ++i) {
            auto value = q.try_pop();
            REQUIRE(value.has_value());
            REQUIRE(*value == cycle * 10 + i);
        }
    }

    REQUIRE(q.empty());
    REQUIRE_FALSE(q.try_pop().has_value());
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

TEST_CASE("SpscQueue accepts rvalue pushes", "[runtime][spsc][coverage]") {
    SpscQueue<std::string, 2> q;

    REQUIRE(q.try_push(std::string("alpha")));
    REQUIRE(q.size_approx() == 1);

    auto value = q.try_pop();
    REQUIRE(value.has_value());
    REQUIRE(*value == "alpha");
    REQUIRE(q.empty());
}

TEST_CASE("SpscQueue preserves moved string payload order",
          "[runtime][spsc][coverage]") {
    SpscQueue<std::string, 3> q;

    std::string alpha = "alpha";
    std::string beta = "beta";
    REQUIRE(q.try_push(std::move(alpha)));
    REQUIRE(q.try_push(std::move(beta)));
    REQUIRE(q.try_push(std::string("gamma")));
    REQUIRE_FALSE(q.try_push(std::string("overflow")));

    REQUIRE(q.try_pop().value() == "alpha");
    REQUIRE(q.try_pop().value() == "beta");
    REQUIRE(q.try_pop().value() == "gamma");
    REQUIRE_FALSE(q.try_pop().has_value());
    REQUIRE(q.empty());
}

TEST_CASE("SpscQueue exposes producer overflow telemetry",
          "[runtime][spsc][telemetry][phase2]") {
    SpscQueue<int, 2> q;
    REQUIRE(q.overflow_count() == 0);

    REQUIRE(q.try_push(1));
    REQUIRE(q.try_push(2));
    REQUIRE_FALSE(q.try_push(3));
    REQUIRE_FALSE(q.try_push(4));

    REQUIRE(q.overflow_count() == 2);
    const auto full = q.telemetry();
    REQUIRE(full.size_approx == 2);
    REQUIRE(full.capacity == 2);
    REQUIRE(full.overflow_count == 2);

    REQUIRE(q.try_pop().value() == 1);
    REQUIRE(q.try_push(5));
    REQUIRE(q.overflow_count() == 2);

    q.reset_overflow_count();
    REQUIRE(q.overflow_count() == 0);
    const auto reset = q.telemetry();
    REQUIRE(reset.size_approx == 2);
    REQUIRE(reset.capacity == 2);
    REQUIRE(reset.overflow_count == 0);
}

TEST_CASE("SpscQueue telemetry hot path allocates zero times",
          "[runtime][spsc][telemetry][rt-safety][phase2]") {
    SpscQueue<int, 2> q;

    pulp::test::RtAllocationProbe probe;

    REQUIRE(q.try_push(1));
    REQUIRE(q.try_push(2));
    REQUIRE_FALSE(q.try_push(3));
    (void)q.telemetry();
    q.reset_overflow_count();
    REQUIRE(q.try_pop().value() == 1);
    REQUIRE(q.try_push(4));

    REQUIRE_FALSE(probe.saw_allocation());
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

TEST_CASE("ScopeGuard dismiss after move suppresses transferred cleanup",
          "[runtime][scope_guard][coverage]") {
    int calls = 0;
    {
        auto guard = make_scope_guard([&] { ++calls; });
        auto moved = std::move(guard);
        moved.dismiss();
    }
    REQUIRE(calls == 0);
}

TEST_CASE("PULP_ON_SCOPE_EXIT runs at block exit",
          "[runtime][scope_guard][coverage][issue-641]") {
    int calls = 0;
    {
        PULP_ON_SCOPE_EXIT(++calls);
        REQUIRE(calls == 0);
    }
    REQUIRE(calls == 1);
}

TEST_CASE("ScopeGuard move preserves dismissed state",
          "[runtime][scope_guard][coverage]") {
    int calls = 0;
    {
        auto guard = make_scope_guard([&] { ++calls; });
        guard.dismiss();
        auto moved = std::move(guard);
        static_cast<void>(moved);
    }

    REQUIRE(calls == 0);
}

TEST_CASE("Range reports containment intersections and empty spans",
          "[runtime][range][codecov]") {
    const IntRange range{10, 20};

    REQUIRE(range.length() == 10);
    REQUIRE_FALSE(range.empty());
    REQUIRE(range.contains(10));
    REQUIRE(range.contains(19));
    REQUIRE_FALSE(range.contains(20));
    REQUIRE(range.contains(IntRange{12, 18}));
    REQUIRE_FALSE(range.contains(IntRange{9, 18}));

    REQUIRE(range.intersects(IntRange{0, 11}));
    REQUIRE(range.intersects(IntRange{19, 30}));
    REQUIRE_FALSE(range.intersects(IntRange{20, 30}));

    REQUIRE(range.intersection(IntRange{15, 25}) == IntRange{15, 20});
    REQUIRE(range.intersection(IntRange{20, 25}).empty());
    REQUIRE(IntRange{5, 5}.empty());
    REQUIRE(IntRange{7, 3}.empty());
}

TEST_CASE("Range union constrain and expansion handle edge inputs",
          "[runtime][range][codecov]") {
    REQUIRE(IntRange{5, 5}.enclosing_union(IntRange{2, 4}) == IntRange{2, 4});
    REQUIRE(IntRange{2, 4}.enclosing_union(IntRange{8, 10}) == IntRange{2, 10});

    const IntRange range{3, 7};
    REQUIRE(range.constrain(1) == 3);
    REQUIRE(range.constrain(5) == 5);
    REQUIRE(range.constrain(99) == 6);
    REQUIRE(IntRange{4, 4}.constrain(99) == 4);

    REQUIRE(IntRange{4, 4}.expanded(9) == IntRange{9, 10});
    REQUIRE(range.expanded(1) == IntRange{1, 7});
    REQUIRE(range.expanded(9) == IntRange{3, 10});
    REQUIRE(IntRange::from_start_length(8, 3) == IntRange{8, 11});
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

TEST_CASE("Runtime environment helpers treat unset and empty as missing",
          "[runtime][system][codecov]") {
    ScopedEnvVar env("PULP_RUNTIME_TEST_EMPTY_ENV");

    env.unset();
    REQUIRE_FALSE(get_env("PULP_RUNTIME_TEST_EMPTY_ENV").has_value());

    env.set("");
    REQUIRE_FALSE(get_env("PULP_RUNTIME_TEST_EMPTY_ENV").has_value());
}

TEST_CASE("Runtime environment helpers preserve non-empty values verbatim",
          "[runtime][system][codecov]") {
    ScopedEnvVar env("PULP_RUNTIME_TEST_VERBATIM_ENV");

    env.set(" value with spaces = yes ");
    auto value = get_env("PULP_RUNTIME_TEST_VERBATIM_ENV");
    REQUIRE(value.has_value());
    REQUIRE(*value == " value with spaces = yes ");

    env.set("0");
    REQUIRE(get_env("PULP_RUNTIME_TEST_VERBATIM_ENV") == std::optional<std::string>{"0"});
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

TEST_CASE("Runtime GMT helper handles leap-day timestamps",
          "[runtime][system][codecov]") {
    auto tm = gmtime_utc(static_cast<std::time_t>(951827696)); // 2000-02-29 12:34:56 UTC
    REQUIRE(tm.tm_year == 100);
    REQUIRE(tm.tm_mon == 1);
    REQUIRE(tm.tm_mday == 29);
    REQUIRE(tm.tm_hour == 12);
    REQUIRE(tm.tm_min == 34);
    REQUIRE(tm.tm_sec == 56);
}

TEST_CASE("Runtime localtime helper returns a normalized tm", "[runtime][system][coverage]") {
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

TEST_CASE("Runtime C string copy handles exact fits and leaves tail bytes alone",
          "[runtime][system][codecov]") {
    char exact[4]{};
    copy_c_string(exact, "abc");
    REQUIRE(exact[0] == 'a');
    REQUIRE(exact[1] == 'b');
    REQUIRE(exact[2] == 'c');
    REQUIRE(exact[3] == '\0');

    std::array<char, 6> padded{'?', '?', '?', '?', '?', '?'};
    copy_c_string(padded.data(), padded.size(), "xy");
    REQUIRE(padded[0] == 'x');
    REQUIRE(padded[1] == 'y');
    REQUIRE(padded[2] == '\0');
    REQUIRE(padded[3] == '?');
    REQUIRE(padded[4] == '?');
    REQUIRE(padded[5] == '?');
}

TEST_CASE("Runtime C string copy writes terminator for empty sources",
          "[runtime][system][coverage]") {
    std::array<char, 4> buffer{'x', 'y', 'z', 'w'};

    copy_c_string(buffer.data(), buffer.size(), "");

    REQUIRE(buffer[0] == '\0');
    REQUIRE(buffer[1] == 'y');
    REQUIRE(buffer[2] == 'z');
    REQUIRE(buffer[3] == 'w');
}

TEST_CASE("Runtime C string copy respects string_view length with embedded nulls",
          "[runtime][system][codecov]") {
    std::array<char, 5> buffer{'?', '?', '?', '?', '?'};
    constexpr char source[] = {'a', '\0', 'b'};

    copy_c_string(buffer.data(), buffer.size(), std::string_view(source, sizeof(source)));

    REQUIRE(buffer[0] == 'a');
    REQUIRE(buffer[1] == '\0');
    REQUIRE(buffer[2] == 'b');
    REQUIRE(buffer[3] == '\0');
    REQUIRE(buffer[4] == '?');
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
          "[runtime][system][coverage]") {
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

TEST_CASE("TemporaryFile self move assignment leaves file ownership intact",
          "[runtime][temporary_file][coverage]") {
    std::filesystem::path path;
    {
        TemporaryFile file(".self");
        path = file.path();
        REQUIRE(std::filesystem::exists(path));

        TemporaryFile* same_file = &file;
        file = std::move(*same_file);

        REQUIRE(file.path() == path);
        REQUIRE(std::filesystem::exists(path));
    }
    REQUIRE_FALSE(std::filesystem::exists(path));
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

TEST_CASE("DynamicLibrary move transfers an open handle", "[runtime][dynamic_library][coverage]") {
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

TEST_CASE("DynamicLibrary failed reopen closes the previous handle",
          "[runtime][dynamic_library][coverage]") {
    DynamicLibrary library;
#ifdef __APPLE__
    REQUIRE(library.open("/usr/lib/libSystem.B.dylib"));
    const char* symbol = "malloc";
#elif defined(__linux__)
    REQUIRE(library.open("libc.so.6"));
    const char* symbol = "malloc";
#elif defined(_WIN32)
    REQUIRE(library.open("kernel32.dll"));
    const char* symbol = "GetCurrentProcess";
#else
    const char* symbol = nullptr;
    SUCCEED("No stable system library fixture on this platform.");
    return;
#endif

    REQUIRE(library.is_open());
    REQUIRE(library.find_symbol(symbol) != nullptr);

    auto missing_path = std::filesystem::temp_directory_path() /
                        "pulp_missing_reopen_library_for_test";
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

TEST_CASE("HighResolutionTimer starts stops and reports running state",
          "[runtime][timer][coverage]") {
    HighResolutionTimer timer;
    std::atomic<int> calls{0};

    REQUIRE_FALSE(timer.is_running());
    timer.start(5ms, [&] { calls.fetch_add(1, std::memory_order_relaxed); });
    REQUIRE(timer.is_running());

    const auto deadline = std::chrono::steady_clock::now() + 250ms;
    while (calls.load(std::memory_order_relaxed) < 2 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(2ms);
    }

    REQUIRE(calls.load(std::memory_order_relaxed) >= 2);
    timer.stop();
    REQUIRE_FALSE(timer.is_running());

    const auto stopped_count = calls.load(std::memory_order_relaxed);
    std::this_thread::sleep_for(20ms);
    REQUIRE(calls.load(std::memory_order_relaxed) == stopped_count);

    timer.stop();
    REQUIRE_FALSE(timer.is_running());
}

TEST_CASE("HighResolutionTimer restart replaces callback",
          "[runtime][timer][coverage]") {
    HighResolutionTimer timer;
    std::atomic<int> first{0};
    std::atomic<int> second{0};

    timer.start(20ms, [&] { first.fetch_add(1, std::memory_order_relaxed); });
    std::this_thread::sleep_for(30ms);
    timer.start(5ms, [&] { second.fetch_add(1, std::memory_order_relaxed); });

    const auto first_after_restart = first.load(std::memory_order_relaxed);
    const auto deadline = std::chrono::steady_clock::now() + 250ms;
    while (second.load(std::memory_order_relaxed) < 2 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(2ms);
    }

    timer.stop();
    REQUIRE(second.load(std::memory_order_relaxed) >= 2);
    REQUIRE(first.load(std::memory_order_relaxed) == first_after_restart);

    timer.start(5ms, {});
    REQUIRE(timer.is_running());
    timer.stop();
    REQUIRE_FALSE(timer.is_running());
}

TEST_CASE("HighResolutionTimer can stop from its own callback",
          "[runtime][timer][coverage]") {
    HighResolutionTimer timer;
    std::atomic<int> calls{0};

    timer.start(1ms, [&] {
        calls.fetch_add(1, std::memory_order_relaxed);
        timer.stop();
    });

    const auto deadline = std::chrono::steady_clock::now() + 250ms;
    while (timer.is_running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }

    REQUIRE_FALSE(timer.is_running());
    REQUIRE(calls.load(std::memory_order_relaxed) == 1);
    timer.stop();
    REQUIRE_FALSE(timer.is_running());
}

TEST_CASE("runtime logging wrappers accept formatted payloads",
          "[runtime][log][coverage]") {
    REQUIRE_NOTHROW(log_info("info {} {}", "message", 1));
    REQUIRE_NOTHROW(log_warn("warn {}", 2));
    REQUIRE_NOTHROW(log_error("error {}", 3));
    REQUIRE_NOTHROW(log(LogLevel::Debug, "debug {}", 4));
    REQUIRE_NOTHROW(log_debug("debug-wrapper {}", 5));
}

// ─── pulp_build_info.hpp (Tier A Slice 7) ───────────────────────────────────

#include <pulp/runtime/build_info.hpp>

TEST_CASE("pulp_build_info constants are populated at configure time",
          "[runtime][build-info]") {
    // CMake's CMAKE_BUILD_TYPE is empty for multi-config generators
    // (Xcode, Visual Studio) but populated for Ninja/Make. We assert
    // the field is *available*, not that it's specifically non-empty.
    static_assert(!pulp::runtime::kSdkVersion.empty(),
                  "SDK version must be set from PROJECT_VERSION");

    // ISO 8601 timestamp uses the year-T-time-Z shape; check the T.
    REQUIRE(pulp::runtime::kBuildIso8601.find('T') != std::string_view::npos);
    REQUIRE(pulp::runtime::kBuildIso8601.find('Z') != std::string_view::npos);

    // Stamp label always begins with the SDK version.
    REQUIRE(pulp::runtime::kStampLabel.starts_with(
        pulp::runtime::kSdkVersion));

    // Git SHA is optional (empty when not a checkout). When present
    // it should be the short form: 7+ hex chars.
    if (!pulp::runtime::kGitSha.empty()) {
        REQUIRE(pulp::runtime::kGitSha.size() >= 7);
    }
}
