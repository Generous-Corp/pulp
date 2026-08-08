#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/control_trace_session_executor.hpp>
#include <pulp/runtime/trace.hpp>

#include <chrono>
#include <memory>

using namespace pulp::inspect;
using namespace std::chrono_literals;

namespace {

std::shared_ptr<InspectorMainThreadRpc> inline_rpc() {
    return std::make_shared<InspectorMainThreadRpc>(
        InspectorMainThreadRpc::Config{1s, 4},
        [](std::function<void()> task) {
            task();
            return true;
        },
        [] { return false; });
}

ControlAdmissionPlan plan(std::string registration = "registration-a") {
    ControlAdmissionPlan value;
    value.registration_id = ControlRegistrationId{std::move(registration)};
    value.operation_id = "dev.pulp.trace/session-control@1";
    value.operation_version = 1;
    value.deadline_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 (std::chrono::system_clock::now() + 5s).time_since_epoch())
                                 .count();
    return value;
}

ControlRequestEnvelope request(std::string params = R"({"action":"start","ring_mb":8})") {
    return {
        .request_id = "route-a",
        .registration_id = "registration-a",
        .operation_id = "dev.pulp.trace/session-control@1",
        .operation_version = 1,
        .deadline_unix_ms = plan().deadline_unix_ms,
        .params_json = std::move(params),
    };
}

ControlExecutionOutcome invoke(const ControlOperationExecutor& executor,
                               ControlAdmissionPlan admission = plan(),
                               ControlRequestEnvelope envelope = request()) {
    return executor(admission, envelope,
                    ControlExecutionContext{
                        .checkpoint = [] { return ControlExecutionCheckpoint::Continue; },
                    });
}

} // namespace

TEST_CASE("canonical trace-session executor rejects requests outside its exact binding",
          "[inspect][control][trace][security]") {
    auto trace = std::make_shared<TraceInspector>();
    auto adapter = ControlTraceSessionExecutor::create({
        .main_thread_rpc = inline_rpc(),
        .trace_inspector = trace,
        .registration_id = ControlRegistrationId{"registration-a"},
    });
    REQUIRE(adapter);

    auto wrong_operation = request();
    wrong_operation.operation_id = "dev.pulp.state/read@1";
    auto wrong_operation_plan = plan();
    wrong_operation_plan.operation_id = wrong_operation.operation_id;
    const auto unsupported =
        invoke(adapter->executor(), std::move(wrong_operation_plan), std::move(wrong_operation));
    CHECK(unsupported.terminal_state == ControlReceiptState::Failed);
    CHECK(unsupported.result.result_code == ControlResultCode::NotImplemented);

    auto mismatched_plan = plan();
    mismatched_plan.operation_id = "dev.pulp.state/read@1";
    const auto mismatched = invoke(adapter->executor(), std::move(mismatched_plan), request());
    CHECK(mismatched.terminal_state == ControlReceiptState::Failed);
    CHECK(mismatched.result.result_code == ControlResultCode::InvalidRequest);

    auto wrong_registration = request();
    wrong_registration.registration_id = "registration-b";
    const auto stale =
        invoke(adapter->executor(), plan("registration-b"), std::move(wrong_registration));
    CHECK(stale.terminal_state == ControlReceiptState::Failed);
    CHECK(stale.result.result_code == ControlResultCode::SessionStale);

    const auto invalid =
        invoke(adapter->executor(), plan(), request(R"({"action":"start","out_path":"/tmp/x"})"));
    CHECK(invalid.terminal_state == ControlReceiptState::Failed);
    CHECK(invalid.result.result_code == ControlResultCode::InvalidRequest);
    CHECK_FALSE(pulp::runtime::Tracing::active());
}

TEST_CASE("canonical trace-session executor owns one registration for its full executor lifetime",
          "[inspect][control][trace][lifetime]") {
    auto trace = std::make_shared<TraceInspector>();
    auto adapter = ControlTraceSessionExecutor::create({
        .main_thread_rpc = inline_rpc(),
        .trace_inspector = trace,
        .registration_id = ControlRegistrationId{"registration-a"},
    });
    REQUIRE(adapter);
    CHECK_FALSE(ControlTraceSessionExecutor::create({
        .main_thread_rpc = inline_rpc(),
        .trace_inspector = trace,
        .registration_id = ControlRegistrationId{"registration-b"},
    }));

    auto executor = adapter->executor();
    adapter.reset();
    CHECK_FALSE(ControlTraceSessionExecutor::create({
        .main_thread_rpc = inline_rpc(),
        .trace_inspector = trace,
        .registration_id = ControlRegistrationId{"registration-b"},
    }));

    const auto started = invoke(executor);
#if defined(PULP_TRACING_ENABLED) && PULP_TRACING_ENABLED
    REQUIRE(started.terminal_state == ControlReceiptState::Completed);
    CHECK_FALSE(started.result.result_code.has_value());

    const auto legacy_stop = trace->handle(
        make_request(1, std::string(methods::kTraceStopSession), "{}"));
    CHECK(legacy_stop.is_error);
    CHECK(legacy_stop.error_code == "trace_owner_unbound");
    CHECK(pulp::runtime::Tracing::active());

    const auto stopped = invoke(executor, plan(), request(R"({"action":"stop"})"));
    REQUIRE(stopped.terminal_state == ControlReceiptState::Completed);
    CHECK_FALSE(stopped.result.result_code.has_value());
#else
    CHECK(started.terminal_state == ControlReceiptState::Failed);
    CHECK(started.result.result_code == ControlResultCode::NotBuilt);
#endif

    executor = {};
    CHECK(ControlTraceSessionExecutor::create({
        .main_thread_rpc = inline_rpc(),
        .trace_inspector = trace,
        .registration_id = ControlRegistrationId{"registration-b"},
    }));
}

TEST_CASE("trace ownership stays exclusive across publication and control bindings",
          "[inspect][control][trace][lifetime]") {
    auto trace = std::make_shared<TraceInspector>();
    InspectorDiscoveryRecord record;
    record.session_id = "session-a";
    record.instance_id = "instance-a";
    record.publication_id = "publication-a";

    auto publication = trace->bind_publication(record);
    REQUIRE(publication);
    CHECK_FALSE(ControlTraceSessionExecutor::create({
        .main_thread_rpc = inline_rpc(),
        .trace_inspector = trace,
        .registration_id = ControlRegistrationId{"registration-a"},
    }));

    publication.reset();
    auto control = ControlTraceSessionExecutor::create({
        .main_thread_rpc = inline_rpc(),
        .trace_inspector = trace,
        .registration_id = ControlRegistrationId{"registration-a"},
    });
    REQUIRE(control);
    CHECK_FALSE(trace->bind_publication(record));
}
