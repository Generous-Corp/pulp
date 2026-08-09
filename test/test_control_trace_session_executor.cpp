#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/control_trace_session_executor.hpp>
#include <pulp/runtime/trace.hpp>

#include <choc/text/choc_JSON.h>

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
    CHECK_FALSE(decode_control_legacy_inspector_error(started.result.detail_json));
    const auto started_detail = choc::json::parse(started.result.detail_json);
    CHECK(started_detail["compiled_in"].getBool());
    CHECK(started_detail["active"].getBool());
    CHECK(started_detail["ok"].getBool());

    const auto already_active = invoke(executor);
    REQUIRE(already_active.terminal_state == ControlReceiptState::Failed);
    CHECK(already_active.result.result_code == ControlResultCode::LeaseConflict);
    CHECK(already_active.result.retry == ControlRetryClassification::AfterRefresh);
    const auto already_active_error =
        decode_control_legacy_inspector_error(already_active.result.detail_json);
    REQUIRE(already_active_error);
    CHECK(already_active_error->error_code == "trace_already_active");
    CHECK(already_active_error->error_message == already_active.result.explanation);
    CHECK(already_active_error->error_data_json == "{}");

    const auto legacy_stop =
        trace->handle(make_request(1, std::string(methods::kTraceStopSession), "{}"));
    CHECK(legacy_stop.is_error);
    CHECK(legacy_stop.error_code == "trace_owner_unbound");
    CHECK(pulp::runtime::Tracing::active());

    const auto stopped = invoke(executor, plan(), request(R"({"action":"stop"})"));
    REQUIRE(stopped.terminal_state == ControlReceiptState::Completed);
    CHECK_FALSE(stopped.result.result_code.has_value());
    CHECK_FALSE(decode_control_legacy_inspector_error(stopped.result.detail_json));

    auto external = pulp::runtime::Tracing::start_exclusive({}, {}, 1024);
    REQUIRE(external.status == pulp::runtime::TraceStartStatus::Started);
    REQUIRE(external.ownership);
    const auto owned_elsewhere = invoke(executor);
    const auto external_stop = pulp::runtime::Tracing::stop_owned(*external.ownership);
    REQUIRE(external_stop.ok);
    REQUIRE(owned_elsewhere.terminal_state == ControlReceiptState::Failed);
    CHECK(owned_elsewhere.result.result_code == ControlResultCode::LeaseConflict);
    CHECK(owned_elsewhere.result.retry == ControlRetryClassification::AfterRefresh);
    const auto owned_elsewhere_error =
        decode_control_legacy_inspector_error(owned_elsewhere.result.detail_json);
    REQUIRE(owned_elsewhere_error);
    CHECK(owned_elsewhere_error->error_code == "trace_owned_by_another_controller");
    CHECK(owned_elsewhere_error->error_code != already_active_error->error_code);
#else
    CHECK(started.terminal_state == ControlReceiptState::Failed);
    CHECK(started.result.result_code == ControlResultCode::NotBuilt);
    const auto detail = decode_control_legacy_inspector_error(started.result.detail_json);
    REQUIRE(detail);
    CHECK(detail->error_code == "tracing_unavailable");
    CHECK(detail->error_message == started.result.explanation);
    CHECK(detail->error_data_json == "{}");
#endif

    executor = {};
    CHECK(ControlTraceSessionExecutor::create({
        .main_thread_rpc = inline_rpc(),
        .trace_inspector = trace,
        .registration_id = ControlRegistrationId{"registration-b"},
    }));
}

TEST_CASE("legacy inspector adapter error detail is canonical and lossless",
          "[inspect][control][adapter][error]") {
    const ControlLegacyInspectorError original{
        .error_code = "future_trace_failure",
        .error_message = "future trace failure with no typed specialization",
        .error_data_json = "  { \"opaque\" : [1, {\"z\":true}], \"order\" : \"kept\" }  ",
    };
    const auto encoded = encode_control_legacy_inspector_error(original);
    REQUIRE(encoded);
    CHECK(canonicalize_control_json(*encoded) == encoded);
    const auto decoded = decode_control_legacy_inspector_error(*encoded);
    REQUIRE(decoded);
    CHECK(*decoded == original);

    const ControlLegacyInspectorError without_data{
        .error_code = "future_trace_failure",
        .error_message = "no structured detail",
        .error_data_json = "",
    };
    const auto encoded_without_data = encode_control_legacy_inspector_error(without_data);
    REQUIRE(encoded_without_data);
    CHECK(decode_control_legacy_inspector_error(*encoded_without_data) == without_data);
}

TEST_CASE("legacy inspector adapter error detail rejects malformed compatibility data",
          "[inspect][control][adapter][error][security]") {
    for (
        const std::string_view malformed : {
            "{}",
            R"({"error_code":"x","error_message":"message"})",
            R"({"error_code":"x","error_message":"message","error_data_json":"{}","extra":true})",
            R"({"error_code":1,"error_message":"message","error_data_json":"{}"})",
            R"({"error_code":"x","error_code":"y","error_message":"message","error_data_json":"{}"})",
            R"({"error_code":"x","error_message":"message","error_data_json":"{"})",
            "{",
        }) {
        INFO(malformed);
        CHECK_FALSE(decode_control_legacy_inspector_error(malformed));
    }
    CHECK_FALSE(encode_control_legacy_inspector_error({
        .error_code = "future_trace_failure",
        .error_message = "message",
        .error_data_json = "{",
    }));
}

TEST_CASE("trace ownership stays exclusive across control bindings",
          "[inspect][control][trace][lifetime]") {
    auto trace = std::make_shared<TraceInspector>();
    auto first = ControlTraceSessionExecutor::create({
        .main_thread_rpc = inline_rpc(),
        .trace_inspector = trace,
        .registration_id = ControlRegistrationId{"registration-a"},
    });
    REQUIRE(first);
    CHECK_FALSE(ControlTraceSessionExecutor::create({
        .main_thread_rpc = inline_rpc(),
        .trace_inspector = trace,
        .registration_id = ControlRegistrationId{"registration-b"},
    }));

    first.reset();
    auto second = ControlTraceSessionExecutor::create({
        .main_thread_rpc = inline_rpc(),
        .trace_inspector = trace,
        .registration_id = ControlRegistrationId{"registration-b"},
    });
    REQUIRE(second);
}

TEST_CASE("projected authority end releases its process-global trace lease",
          "[inspect][control][trace][authority][lifetime]") {
    auto trace = std::make_shared<TraceInspector>();
    auto first = ControlTraceSessionExecutor::create({
        .main_thread_rpc = inline_rpc(),
        .trace_inspector = trace,
        .registration_id = ControlRegistrationId{"registration-a"},
    });
    REQUIRE(first);
    auto authority_plan = plan();
    authority_plan.client_id = ControlClientId{"authority-a"};
    auto authority_request = request();
    authority_request.client_id = "authority-a";
    const auto started = invoke(first->executor(), authority_plan, authority_request);
#if defined(PULP_TRACING_ENABLED) && PULP_TRACING_ENABLED
    REQUIRE(started.terminal_state == ControlReceiptState::Completed);
    first->end_authority("authority-b");
    CHECK_FALSE(ControlTraceSessionExecutor::create({
        .main_thread_rpc = inline_rpc(),
        .trace_inspector = trace,
        .registration_id = ControlRegistrationId{"registration-b"},
    }));
    first->end_authority("authority-a");
#else
    CHECK(started.terminal_state == ControlReceiptState::Failed);
    first->disconnect();
#endif
    auto second = ControlTraceSessionExecutor::create({
        .main_thread_rpc = inline_rpc(),
        .trace_inspector = trace,
        .registration_id = ControlRegistrationId{"registration-b"},
    });
    REQUIRE(second);
}
