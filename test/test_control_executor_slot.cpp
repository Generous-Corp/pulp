#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/control_executor_slot.hpp>

#include "support/thread_progress.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <utility>
#include <vector>

using namespace pulp::inspect;
using namespace std::chrono_literals;

namespace {

ControlAdmissionPlan plan() {
    ControlAdmissionPlan value;
    value.registration_id = ControlRegistrationId{"registration-slot"};
    value.operation_id = "Trace.startSession";
    value.operation_version = 1;
    return value;
}

ControlRequestEnvelope request() {
    return {.request_id = "request-slot",
            .registration_id = "registration-slot",
            .operation_id = "Trace.startSession",
            .operation_version = 1,
            .params_json = R"({"session":"slot"})"};
}

ControlExecutionContext context() {
    return {.checkpoint = [] { return ControlExecutionCheckpoint::Continue; }};
}

ControlExecutionOutcome invoke(const ControlOperationExecutor& executor) {
    return executor(plan(), request(), context());
}

void check_unavailable(const ControlExecutionOutcome& result) {
    REQUIRE(result.result.result_code.has_value());
    CHECK(result.terminal_state == ControlReceiptState::Failed);
    CHECK(*result.result.result_code == ControlResultCode::HostUnavailable);
    CHECK(result.result.retry == ControlRetryClassification::AfterBackoff);
    CHECK(result.result.explanation == "host operation executor is not installed");
    CHECK(result.result.cancellation_reason.empty());
    CHECK_FALSE(result.deferred);
}

void check_cancelled(const ControlExecutionOutcome& result) {
    REQUIRE(result.result.result_code.has_value());
    CHECK(result.terminal_state == ControlReceiptState::Cancelled);
    CHECK(*result.result.result_code == ControlResultCode::Cancelled);
    CHECK(result.result.retry == ControlRetryClassification::Never);
    CHECK(result.result.explanation == "host operation executor slot is closed");
    CHECK(result.result.cancellation_reason == "executor-slot-closed");
    CHECK_FALSE(result.deferred);
}

ControlOperationExecutor completed_executor(std::atomic<unsigned>* calls = nullptr) {
    return [calls](const ControlAdmissionPlan&, const ControlRequestEnvelope&,
                   const ControlExecutionContext&) {
        if (calls)
            calls->fetch_add(1, std::memory_order_relaxed);
        return ControlExecutionOutcome{
            .terminal_state = ControlReceiptState::Completed,
            .result = {.detail_json = R"({"installed":true})"},
        };
    };
}

} // namespace

TEST_CASE("control executor slot fails closed before install and after close",
          "[inspect][control][executor][slot][security]") {
    ControlOperationExecutorSlot slot;
    const auto facade = slot.executor();

    check_unavailable(invoke(facade));
    CHECK_FALSE(slot.install({}));
    REQUIRE(slot.install(completed_executor()));
    CHECK_FALSE(slot.install(completed_executor()));

    const auto completed = invoke(facade);
    CHECK(completed.terminal_state == ControlReceiptState::Completed);
    CHECK(completed.result.detail_json == R"({"installed":true})");

    slot.close();
    slot.close();
    check_cancelled(invoke(facade));
    CHECK_FALSE(slot.install(completed_executor()));
}

TEST_CASE("control executor slot forwards exact inputs without holding its lock",
          "[inspect][control][executor][slot][reentrant]") {
    ControlOperationExecutorSlot slot;
    std::atomic<bool> saw_exact_inputs{false};
    std::atomic<bool> replacement_refused{false};
    REQUIRE(slot.install([&](const ControlAdmissionPlan& received_plan,
                             const ControlRequestEnvelope& received_request,
                             const ControlExecutionContext& received_context) {
        saw_exact_inputs.store(
            received_plan.registration_id == ControlRegistrationId{"registration-slot"} &&
                received_plan.operation_id == "Trace.startSession" &&
                received_request.request_id == "request-slot" &&
                received_request.params_json == R"({"session":"slot"})" &&
                received_context.checkpoint &&
                received_context.checkpoint() == ControlExecutionCheckpoint::Continue,
            std::memory_order_release);
        replacement_refused.store(!slot.install(completed_executor()), std::memory_order_release);
        slot.close();
        return ControlExecutionOutcome{.terminal_state = ControlReceiptState::Completed};
    }));

    auto call = std::async(std::launch::async, [&] { return invoke(slot.executor()); });
    REQUIRE(call.wait_for(2s) == std::future_status::ready);
    CHECK(call.get().terminal_state == ControlReceiptState::Completed);
    CHECK(saw_exact_inputs.load(std::memory_order_acquire));
    CHECK(replacement_refused.load(std::memory_order_acquire));
    check_cancelled(invoke(slot.executor()));
}

TEST_CASE("control executor slot keeps an acquired executor alive across close",
          "[inspect][control][executor][slot][lifetime]") {
    ControlOperationExecutorSlot slot;
    std::promise<void> entered;
    std::promise<void> release;
    auto release_ready = release.get_future().share();
    REQUIRE(slot.install([&](const auto&, const auto&, const auto&) {
        entered.set_value();
        if (release_ready.wait_for(pulp::test::kProgressDeadline) != std::future_status::ready) {
            return ControlExecutionOutcome{
                .terminal_state = ControlReceiptState::Failed,
                .result = {.result_code = ControlResultCode::InternalError,
                           .explanation = "test release deadline elapsed"},
            };
        }
        return ControlExecutionOutcome{.terminal_state = ControlReceiptState::Completed};
    }));
    const auto facade = slot.executor();
    auto in_flight = std::async(std::launch::async, [&] { return invoke(facade); });
    REQUIRE(entered.get_future().wait_for(2s) == std::future_status::ready);

    slot.close();
    check_cancelled(invoke(facade));
    release.set_value();
    REQUIRE(in_flight.wait_for(2s) == std::future_status::ready);
    CHECK(in_flight.get().terminal_state == ControlReceiptState::Completed);
}

TEST_CASE("control executor slot releases callback captures outside its lock",
          "[inspect][control][executor][slot][reentrant][lifetime]") {
    struct ReentrantDestruction {
        ControlOperationExecutorSlot* slot = nullptr;
        std::atomic<bool>* destroyed = nullptr;
        ~ReentrantDestruction() {
            slot->close();
            destroyed->store(true, std::memory_order_release);
        }
    };

    ControlOperationExecutorSlot slot;
    std::atomic<bool> destroyed{false};
    auto capture = std::make_shared<ReentrantDestruction>();
    capture->slot = &slot;
    capture->destroyed = &destroyed;
    REQUIRE(slot.install([capture](const auto&, const auto&, const auto&) {
        return ControlExecutionOutcome{.terminal_state = ControlReceiptState::Completed};
    }));
    capture.reset();

    auto close = std::async(std::launch::async, [&] { slot.close(); });
    REQUIRE(close.wait_for(2s) == std::future_status::ready);
    close.get();
    CHECK(destroyed.load(std::memory_order_acquire));
    check_cancelled(invoke(slot.executor()));
}

TEST_CASE("control executor slot facades fail closed after owner destruction",
          "[inspect][control][executor][slot][lifetime]") {
    ControlOperationExecutor facade;
    {
        ControlOperationExecutorSlot slot;
        REQUIRE(slot.install(completed_executor()));
        facade = slot.executor();
        CHECK(invoke(facade).terminal_state == ControlReceiptState::Completed);
    }
    check_cancelled(invoke(facade));
}

TEST_CASE("control executor slot moves ownership without reviving replaced state",
          "[inspect][control][executor][slot][move]") {
    ControlOperationExecutorSlot source;
    REQUIRE(source.install(completed_executor()));
    const auto source_facade = source.executor();

    ControlOperationExecutorSlot moved{std::move(source)};
    CHECK(invoke(source_facade).terminal_state == ControlReceiptState::Completed);
    check_cancelled(invoke(source.executor()));

    ControlOperationExecutorSlot destination;
    REQUIRE(destination.install(completed_executor()));
    const auto replaced_facade = destination.executor();
    destination = std::move(moved);
    check_cancelled(invoke(replaced_facade));
    CHECK(invoke(destination.executor()).terminal_state == ControlReceiptState::Completed);
    check_cancelled(invoke(moved.executor()));
}

TEST_CASE("control executor slot linearizes concurrent execute install and close",
          "[inspect][control][executor][slot][stress]") {
    ControlOperationExecutorSlot slot;
    const auto facade = slot.executor();
    std::atomic<bool> go{false};
    std::atomic<unsigned> completed{0};
    std::atomic<unsigned> unavailable_count{0};
    std::atomic<unsigned> cancelled_count{0};
    std::atomic<unsigned> invalid{0};
    std::atomic<unsigned> forwarded{0};

    std::vector<std::thread> callers;
    for (unsigned thread = 0; thread < 8; ++thread) {
        callers.emplace_back([&] {
            if (!pulp::test::wait_for_condition(
                    [&] { return go.load(std::memory_order_acquire); })) {
                invalid.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            for (unsigned call = 0; call < 500; ++call) {
                const auto result = invoke(facade);
                if (result.terminal_state == ControlReceiptState::Completed &&
                    !result.result.result_code) {
                    completed.fetch_add(1, std::memory_order_relaxed);
                } else if (result.terminal_state == ControlReceiptState::Failed &&
                           result.result.result_code == ControlResultCode::HostUnavailable) {
                    unavailable_count.fetch_add(1, std::memory_order_relaxed);
                } else if (result.terminal_state == ControlReceiptState::Cancelled &&
                           result.result.result_code == ControlResultCode::Cancelled) {
                    cancelled_count.fetch_add(1, std::memory_order_relaxed);
                } else {
                    invalid.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    std::thread installer([&] {
        if (!pulp::test::wait_for_condition([&] { return go.load(std::memory_order_acquire); }))
            return;
        (void)slot.install(completed_executor(&forwarded));
    });
    std::thread closer([&] {
        if (!pulp::test::wait_for_condition([&] { return go.load(std::memory_order_acquire); }))
            return;
        slot.close();
    });

    go.store(true, std::memory_order_release);
    for (auto& caller : callers)
        caller.join();
    installer.join();
    closer.join();

    CHECK(invalid.load() == 0);
    CHECK(completed.load() + unavailable_count.load() + cancelled_count.load() == 4'000);
    CHECK(completed.load() == forwarded.load());
    check_cancelled(invoke(facade));
    CHECK_FALSE(slot.install(completed_executor()));
}
