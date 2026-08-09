#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/capture_source.hpp>
#include <pulp/inspect/control_host_ui_executor.hpp>
#include <pulp/inspect/main_thread_rpc.hpp>
#include <pulp/inspect/runtime_evaluator.hpp>

#include <choc/text/choc_JSON.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using namespace pulp::inspect;

namespace {

struct PostedQueue {
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<std::function<void()>> tasks;

    bool post(std::function<void()> task) {
        {
            std::lock_guard lock(mutex);
            tasks.push_back(std::move(task));
        }
        ready.notify_all();
        return true;
    }

    std::function<void()> take() {
        std::unique_lock lock(mutex);
        if (!ready.wait_for(lock, 2s, [&] { return !tasks.empty(); }))
            return {};
        auto task = std::move(tasks.front());
        tasks.erase(tasks.begin());
        return task;
    }
};

class CaptureSource final : public InspectorCaptureSource {
  public:
    InspectorCapture capture_png() override {
        ++calls;
        return {.png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 1, 2, 3},
                .width = 64,
                .height = 32};
    }
    int calls = 0;
};

class Evaluator final : public RuntimeEvaluator {
  public:
    RuntimeEvaluatorCapabilities capabilities() const override {
        return {.engine = "fixture", .can_evaluate = true, .can_interrupt = true};
    }
    RuntimeEvaluationResult evaluate(std::string_view code, std::chrono::milliseconds timeout,
                                     std::size_t maximum_result_bytes) override {
        ++calls;
        last_source = code;
        last_timeout = timeout;
        last_maximum_result_bytes = maximum_result_bytes;
        while (block_until_interrupted && !interrupted.load())
            std::this_thread::sleep_for(1ms);
        if (interrupted.load())
            return {.timed_out = true};
        return {.ok = true, .json = R"({"secret":"bounded"})"};
    }
    bool interrupt() override {
        ++interrupt_calls;
        interrupted = true;
        return true;
    }
    std::string_view binary_marker() const noexcept override {
        return "PULP_INSPECT_RUNTIME_EVAL_HIGH_RISK_COMPONENT_V1";
    }
    int calls = 0;
    std::string last_source;
    std::chrono::milliseconds last_timeout{};
    std::size_t last_maximum_result_bytes = 0;
    bool block_until_interrupted = false;
    std::atomic<bool> interrupted{false};
    std::atomic<int> interrupt_calls{0};
};

ControlHostUiBinding binding(ControlBuildProfile profile = ControlBuildProfile::DeveloperLocal) {
    ControlManifest manifest;
    manifest.profile = profile;
    manifest.target = "ControlHostUiTest";
    manifest.product_name = "Control Host UI Test";
    manifest.bundle_id = "dev.pulp.test.control-host-ui";
    manifest.build_id = "build:0123456789abcdef0123456789abcdef";
    manifest.endpoint_included = true;
    manifest.unsafe_runtime_eval_acknowledged = profile == ControlBuildProfile::ResearchUnsafe;
    manifest.capabilities =
        profile == ControlBuildProfile::ResearchUnsafe
            ? std::vector{InspectorCapability::SessionControl, InspectorCapability::RuntimeEval}
            : std::vector{InspectorCapability::SessionControl, InspectorCapability::CaptureImage};
    const auto manifest_digest = control_manifest_digest(manifest);
    const auto artifact_digest = std::string(64, 'c');
    ControlRegistration registration;
    registration.registration_id = ControlRegistrationId{"registration-a"};
    registration.broker_id = ControlBrokerId{"broker-a"};
    registration.session_id = "session-a";
    registration.instance_id = "instance-a";
    registration.publication_id = "publication-a";
    registration.manifest_digest = manifest_digest;
    registration.artifact_digest = artifact_digest;
    registration.consent_identity = control_consent_identity(manifest_digest, artifact_digest);
    registration.profile = profile;
    registration.capabilities = manifest.capabilities;
    return {.registration = std::move(registration), .manifest = std::move(manifest)};
}

ControlAdmissionPlan plan(InspectorCapability capability, std::string operation) {
    ControlAdmissionPlan value;
    value.broker_id = ControlBrokerId{"broker-a"};
    value.client_principal = "principal-a";
    value.client_id = ControlClientId{"client-a"};
    value.registration_id = ControlRegistrationId{"registration-a"};
    value.grant_id = ControlGrantId{"grant-a"};
    value.session_id = "session-a";
    value.instance_id = "instance-a";
    value.publication_id = "publication-a";
    value.instance_generation = "publication-a";
    value.capability = capability;
    value.operation_id = std::move(operation);
    value.operation_version = 1;
    value.consent_decision_id = "single-use-consent-a";
    value.manifest_digest =
        control_manifest_digest(binding(capability == InspectorCapability::RuntimeEval
                                            ? ControlBuildProfile::ResearchUnsafe
                                            : ControlBuildProfile::DeveloperLocal)
                                    .manifest);
    value.producer_artifact_digest = std::string(64, 'c');
    value.deadline_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 (std::chrono::system_clock::now() + 5s).time_since_epoch())
                                 .count();
    value.receipt_id = ControlReceiptId{"receipt-a"};
    return value;
}

ControlRequestEnvelope request(const ControlAdmissionPlan& admission, std::string params) {
    return {.request_id = "request-a",
            .client_id = admission.client_id.value,
            .registration_id = admission.registration_id.value,
            .grant_id = admission.grant_id.value,
            .instance_generation = admission.instance_generation,
            .operation_id = admission.operation_id,
            .operation_version = admission.operation_version,
            .deadline_unix_ms = admission.deadline_unix_ms,
            .params_json = std::move(params)};
}

ControlExecutionOutcome run_on_worker(const ControlOperationExecutor& executor,
                                      const ControlAdmissionPlan& admission,
                                      const ControlRequestEnvelope& envelope,
                                      ControlExecutionContext context, PostedQueue& queue) {
    ControlExecutionOutcome result;
    std::thread worker([&] { result = executor(admission, envelope, context); });
    auto task = queue.take();
    REQUIRE(task);
    task();
    worker.join();
    return result;
}

std::shared_ptr<InspectorMainThreadRpc> rpc(PostedQueue& queue) {
    return std::make_shared<InspectorMainThreadRpc>(
        InspectorMainThreadRpc::Config{2s, 4},
        [&](auto task) { return queue.post(std::move(task)); }, [] { return false; });
}

} // namespace

TEST_CASE("host UI capture publishes one exact-lineage sensitive PNG artifact",
          "[inspect][control][ui][capture][artifact][security]") {
    PostedQueue queue;
    auto source = std::make_shared<CaptureSource>();
    auto executor = ControlHostUiExecutor::create(
        {.binding = binding(), .main_thread_rpc = rpc(queue), .capture_source = source});
    REQUIRE(executor);
    const auto admission = plan(InspectorCapability::CaptureImage, "dev.pulp.ui/capture@1");
    const auto envelope = request(admission, R"({"target":"window","format":"png"})");
    bool published = false;
    const auto outcome = run_on_worker(
        executor->executor(), admission, envelope,
        {.checkpoint = [] { return ControlExecutionCheckpoint::Continue; },
         .maximum_artifact_bytes = 1024,
         .publish_artifact =
             [&](std::span<const std::uint8_t> bytes, ControlArtifactPublication publication) {
                 published = bytes.size() == 11 && publication.content_type == "image/png" &&
                             publication.sensitivity == ControlArtifactSensitivity::Sensitive &&
                             publication.redaction_state == ControlArtifactRedactionState::Original;
                 ControlArtifactMetadata metadata;
                 metadata.artifact_id = "artifact-0123456789abcdef0123456789abcdef";
                 metadata.sha256 = std::string(64, 'b');
                 metadata.byte_size = bytes.size();
                 metadata.content_type = publication.content_type;
                 return ControlArtifactStoreResult{ControlArtifactStatus::Stored,
                                                   std::move(metadata)};
             }},
        queue);
    REQUIRE(outcome.terminal_state == ControlReceiptState::Completed);
    REQUIRE(published);
    REQUIRE(source->calls == 1);
    REQUIRE(outcome.result.artifacts.size() == 1);
    const auto detail = choc::json::parse(outcome.result.detail_json);
    CHECK(detail["width"].getInt64() == 64);
    CHECK(detail["height"].getInt64() == 32);
    CHECK(detail["redaction_state"].getString() == "original");

    auto wrong_plan = admission;
    wrong_plan.session_id = "session-b";
    const auto denied = executor->executor()(
        wrong_plan, envelope, {.checkpoint = [] { return ControlExecutionCheckpoint::Continue; }});
    CHECK(denied.result.result_code == ControlResultCode::PolicyDenied);
    CHECK(source->calls == 1);
}

TEST_CASE("host UI executor refuses node capture and UI input without exact host seams",
          "[inspect][control][ui][input][disposition]") {
    PostedQueue queue;
    auto source = std::make_shared<CaptureSource>();
    auto executor = ControlHostUiExecutor::create(
        {.binding = binding(), .main_thread_rpc = rpc(queue), .capture_source = source});
    REQUIRE(executor);
    auto admission = plan(InspectorCapability::CaptureImage, "dev.pulp.ui/capture@1");
    auto outcome =
        run_on_worker(executor->executor(), admission,
                      request(admission, R"({"target":"node","format":"png","node_id":"gain"})"),
                      {.checkpoint = [] { return ControlExecutionCheckpoint::Continue; }}, queue);
    CHECK(outcome.result.result_code == ControlResultCode::NotImplemented);
    CHECK(source->calls == 0);

    outcome = run_on_worker(
        executor->executor(), admission,
        request(admission, R"({"target":"window","format":"png","raw_method":"capture"})"),
        {.checkpoint = [] { return ControlExecutionCheckpoint::Continue; }}, queue);
    CHECK(outcome.result.result_code == ControlResultCode::InvalidRequest);
    CHECK(source->calls == 0);

    admission = plan(InspectorCapability::UiInput, "dev.pulp.ui/input@1");
    outcome = run_on_worker(
        executor->executor(), admission,
        request(admission, R"({"kind":"focus","target_id":"gain","event":{"focused":true}})"),
        {.checkpoint = [] { return ControlExecutionCheckpoint::Continue; }}, queue);
    CHECK(outcome.result.result_code == ControlResultCode::NotImplemented);
    CHECK(control_ui_input_disposition() == "unsupported-no-exact-host-target-seam");
}

TEST_CASE("runtime evaluation requires research acknowledgement and returns bounded JSON",
          "[inspect][control][runtime-eval][consent][security]") {
    PostedQueue queue;
    auto evaluator = std::make_shared<Evaluator>();
    auto unsafe = binding(ControlBuildProfile::ResearchUnsafe);
    auto executor = ControlHostUiExecutor::create(
        {.binding = unsafe,
         .main_thread_rpc = rpc(queue),
         .runtime_evaluator = evaluator,
         .redact_runtime_eval_result = [](std::string_view) -> std::optional<std::string> {
             return R"({"secret":"[redacted]","nested":{"token":"[redacted]"}})";
         }});
    REQUIRE(executor);
    const auto admission = plan(InspectorCapability::RuntimeEval, "dev.pulp.runtime/evaluate@1");
    const auto envelope = request(
        admission,
        R"json({"source":"({ answer: 42 })","timeout_ms":1000,"idempotency_key":"eval-a"})json");
    const auto outcome =
        run_on_worker(executor->executor(), admission, envelope,
                      {.checkpoint = [] { return ControlExecutionCheckpoint::Continue; }}, queue);
    REQUIRE(outcome.terminal_state == ControlReceiptState::Completed);
    REQUIRE(evaluator->calls == 1);
    CHECK(evaluator->last_source == "({ answer: 42 })");
    CHECK(evaluator->last_timeout > 0ms);
    CHECK(evaluator->last_timeout <= 1000ms);
    CHECK(evaluator->last_maximum_result_bytes == kControlRuntimeEvalMaximumResultBytes);
    const auto detail = choc::json::parse(outcome.result.detail_json);
    CHECK(detail["receipt_id"].getString() == "receipt-a");
    const auto redacted = choc::json::parse(detail["result_json"].getString());
    CHECK(redacted["secret"].getString() == "[redacted]");
    CHECK(redacted["nested"]["token"].getString() == "[redacted]");

    unsafe.manifest.unsafe_runtime_eval_acknowledged = false;
    CHECK_FALSE(
        ControlHostUiExecutor::create({.binding = unsafe,
                                       .main_thread_rpc = rpc(queue),
                                       .runtime_evaluator = evaluator,
                                       .redact_runtime_eval_result = [](std::string_view value) {
                                           return std::optional<std::string>{std::string(value)};
                                       }}));
}

TEST_CASE("runtime evaluation interrupts on broker cancellation",
          "[inspect][control][runtime-eval][cancellation][security]") {
    PostedQueue queue;
    auto evaluator = std::make_shared<Evaluator>();
    evaluator->block_until_interrupted = true;
    auto executor =
        ControlHostUiExecutor::create({.binding = binding(ControlBuildProfile::ResearchUnsafe),
                                       .main_thread_rpc = rpc(queue),
                                       .runtime_evaluator = evaluator,
                                       .redact_runtime_eval_result = [](std::string_view value) {
                                           return std::optional<std::string>{std::string(value)};
                                       }});
    REQUIRE(executor);
    const auto admission = plan(InspectorCapability::RuntimeEval, "dev.pulp.runtime/evaluate@1");
    std::atomic<int> checkpoints{0};
    const auto outcome = run_on_worker(
        executor->executor(), admission,
        request(admission,
                R"({"source":"while(true){}","timeout_ms":1000,"idempotency_key":"eval-cancel"})"),
        {.checkpoint =
             [&] {
                 return ++checkpoints >= 3 ? ControlExecutionCheckpoint::Cancelled
                                           : ControlExecutionCheckpoint::Continue;
             }},
        queue);
    CHECK(outcome.terminal_state == ControlReceiptState::Cancelled);
    CHECK(evaluator->interrupt_calls == 1);
}

TEST_CASE("host UI work is cancelled before main-thread capture applies",
          "[inspect][control][ui][cancellation]") {
    PostedQueue queue;
    auto source = std::make_shared<CaptureSource>();
    auto executor = ControlHostUiExecutor::create(
        {.binding = binding(), .main_thread_rpc = rpc(queue), .capture_source = source});
    REQUIRE(executor);
    const auto admission = plan(InspectorCapability::CaptureImage, "dev.pulp.ui/capture@1");
    const auto outcome =
        run_on_worker(executor->executor(), admission,
                      request(admission, R"({"target":"window","format":"png"})"),
                      {.checkpoint = [] { return ControlExecutionCheckpoint::Cancelled; }}, queue);
    CHECK(outcome.terminal_state == ControlReceiptState::Cancelled);
    CHECK(source->calls == 0);
}
