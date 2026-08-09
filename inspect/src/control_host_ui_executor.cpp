#include <pulp/inspect/control_host_ui_executor.hpp>

#include <pulp/inspect/capture_source.hpp>
#include <pulp/inspect/control_main_thread_executor.hpp>
#include <pulp/inspect/control_manifest.hpp>
#include <pulp/inspect/runtime_evaluator.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace pulp::inspect {
namespace {

constexpr std::string_view kCaptureOperation = "dev.pulp.ui/capture@1";
constexpr std::string_view kUiInputOperation = "dev.pulp.ui/input@1";
constexpr std::string_view kRuntimeEvalOperation = "dev.pulp.runtime/evaluate@1";
constexpr std::string_view kRuntimeEvalComponentMarker =
    "PULP_INSPECT_RUNTIME_EVAL_HIGH_RISK_COMPONENT_V1";

ControlExecutionOutcome
failure(ControlResultCode code, std::string explanation,
        ControlRetryClassification retry = ControlRetryClassification::Never) {
    return {.terminal_state = ControlReceiptState::Failed,
            .result = {.result_code = code, .retry = retry, .explanation = std::move(explanation)}};
}

ControlExecutionOutcome checkpoint_failure(ControlExecutionCheckpoint checkpoint) {
    if (checkpoint == ControlExecutionCheckpoint::DeadlineExceeded)
        return failure(ControlResultCode::DeadlineExceeded, "host UI operation deadline exceeded");
    return {.terminal_state = ControlReceiptState::Cancelled,
            .result = {.result_code = ControlResultCode::Cancelled,
                       .explanation = checkpoint == ControlExecutionCheckpoint::Cancelled
                                          ? "host UI operation was cancelled"
                                          : "host UI operation authority was revoked",
                       .cancellation_reason = checkpoint == ControlExecutionCheckpoint::Cancelled
                                                  ? "client-cancelled"
                                                  : "authority-revoked"}};
}

bool exact_authority(const ControlAdmissionPlan& plan, const ControlRequestEnvelope& request,
                     const ControlHostUiBinding& binding) {
    const auto& registration = binding.registration;
    return plan.broker_id == registration.broker_id &&
           plan.registration_id == registration.registration_id &&
           plan.session_id == registration.session_id &&
           plan.instance_id == registration.instance_id &&
           plan.publication_id == registration.publication_id &&
           plan.manifest_digest == registration.manifest_digest &&
           plan.producer_artifact_digest == registration.artifact_digest && plan.client_id &&
           plan.grant_id && !plan.consent_decision_id.empty() && !request.request_id.empty() &&
           request.client_id == plan.client_id.value &&
           request.registration_id == plan.registration_id.value &&
           request.grant_id == plan.grant_id.value &&
           request.instance_generation == plan.instance_generation &&
           request.operation_id == plan.operation_id &&
           request.operation_version == plan.operation_version &&
           request.deadline_unix_ms == plan.deadline_unix_ms;
}

bool valid_png(std::span<const std::uint8_t> bytes, std::uint32_t width, std::uint32_t height) {
    constexpr std::array<std::uint8_t, 8> signature{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    return width != 0 && height != 0 && bytes.size() >= signature.size() &&
           std::equal(signature.begin(), signature.end(), bytes.begin());
}

std::optional<std::int64_t> eval_timeout_ms(std::string_view params_json) {
    try {
        const auto params = choc::json::parse(params_json);
        if (!params.isObject() || !params["source"].isString() || !params["timeout_ms"].isInt() ||
            !params["idempotency_key"].isString())
            return std::nullopt;
        const std::string_view source = params["source"].getString();
        const auto timeout = params["timeout_ms"].getInt64();
        const std::string_view idempotency = params["idempotency_key"].getString();
        if (source.empty() || source.size() > kRuntimeEvalMaxCodeBytes ||
            source.find('\0') != std::string_view::npos || timeout <= 0 ||
            timeout > kRuntimeEvalDeadline.count() || idempotency.empty() ||
            idempotency.size() > 128)
            return std::nullopt;
        return timeout;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

struct ControlHostUiExecutor::State {
    mutable std::mutex mutex;
    ControlHostUiBinding binding;
    std::shared_ptr<InspectorMainThreadRpc> rpc;
    std::shared_ptr<InspectorCaptureSource> capture;
    std::shared_ptr<RuntimeEvaluator> evaluator;
    ControlRuntimeEvalRedactor redact_eval;
    std::size_t maximum_capture_bytes = 0;
    std::size_t maximum_eval_result_bytes = 0;
    std::chrono::milliseconds capture_lifetime{};
    ControlOperationExecutor main_thread;
    bool connected = true;

    ControlExecutionOutcome execute_main(const ControlAdmissionPlan& plan,
                                         const ControlRequestEnvelope& request,
                                         const ControlExecutionContext& context) {
        std::shared_ptr<InspectorCaptureSource> acquired_capture;
        std::shared_ptr<RuntimeEvaluator> acquired_evaluator;
        ControlHostUiBinding acquired_binding;
        {
            std::lock_guard lock(mutex);
            if (!connected)
                return failure(ControlResultCode::SessionStale, "host UI executor is disconnected",
                               ControlRetryClassification::AfterRefresh);
            acquired_capture = capture;
            acquired_evaluator = evaluator;
            acquired_binding = binding;
        }
        if (!exact_authority(plan, request, acquired_binding))
            return failure(ControlResultCode::PolicyDenied,
                           "host UI authority does not match the bound instance");
        if (!context.checkpoint)
            return failure(ControlResultCode::PolicyDenied,
                           "host UI execution requires broker revalidation");
        const auto checkpoint = context.checkpoint();
        if (checkpoint != ControlExecutionCheckpoint::Continue)
            return checkpoint_failure(checkpoint);

        const auto* descriptor =
            resolve_control_operation(request.operation_id, request.operation_version);
        ControlJsonSchemaDiagnostics diagnostics;
        if (!descriptor || descriptor->capability != plan.capability ||
            !validate_control_json_schema(request.params_json, descriptor->input_schema_json,
                                          &diagnostics)) {
            return failure(ControlResultCode::InvalidRequest,
                           diagnostics.explanation.empty()
                               ? "host UI request does not match its canonical operation schema"
                               : diagnostics.explanation);
        }

        if (request.operation_id == kUiInputOperation) {
            return failure(ControlResultCode::NotImplemented,
                           "UI input is unavailable without an exact host-owned target seam");
        }
        if (request.operation_id == kCaptureOperation) {
            if (plan.capability != InspectorCapability::CaptureImage)
                return failure(ControlResultCode::PolicyDenied,
                               "UI capture capability does not match the admitted plan");
            if (!acquired_capture)
                return failure(ControlResultCode::HostUnavailable,
                               "exact-instance UI capture is unavailable",
                               ControlRetryClassification::AfterRefresh);
            try {
                const auto params = choc::json::parse(request.params_json);
                if (!params.isObject() || !params["target"].isString() ||
                    !params["format"].isString() || params["format"].getString() != "png")
                    return failure(ControlResultCode::InvalidRequest,
                                   "UI capture request was not canonical");
                if (params["target"].getString() != "window")
                    return failure(
                        ControlResultCode::NotImplemented,
                        "node capture is unavailable without an exact node capture seam");
            } catch (...) {
                return failure(ControlResultCode::InvalidRequest,
                               "UI capture request could not be decoded");
            }
            if (!context.publish_artifact)
                return failure(ControlResultCode::HostUnavailable,
                               "broker artifact publication is unavailable",
                               ControlRetryClassification::AfterRefresh);
            auto captured = acquired_capture->capture_png();
            if (!captured.error.empty())
                return failure(ControlResultCode::InternalError, "UI capture failed");
            const auto maximum = std::min(maximum_capture_bytes, context.maximum_artifact_bytes);
            if (maximum == 0 || captured.png.size() > maximum)
                return failure(ControlResultCode::ResourceExhausted,
                               "UI capture exceeds the broker artifact capacity");
            if (captured.width > 1'048'576 || captured.height > 1'048'576 ||
                !valid_png(captured.png, captured.width, captured.height))
                return failure(ControlResultCode::InternalError,
                               "UI capture did not return a bounded PNG");
            const auto before_publish = context.checkpoint();
            if (before_publish != ControlExecutionCheckpoint::Continue)
                return checkpoint_failure(before_publish);
            const auto stored = context.publish_artifact(
                captured.png, {.content_type = "image/png",
                               .sensitivity = ControlArtifactSensitivity::Sensitive,
                               .redaction_state = ControlArtifactRedactionState::Original,
                               .lifetime = capture_lifetime});
            if (stored.status != ControlArtifactStatus::Stored || !stored.metadata) {
                if (stored.status == ControlArtifactStatus::Unauthorized) {
                    const auto after_denial = context.checkpoint();
                    if (after_denial != ControlExecutionCheckpoint::Continue)
                        return checkpoint_failure(after_denial);
                }
                return failure(stored.status == ControlArtifactStatus::ResourceExhausted
                                   ? ControlResultCode::ResourceExhausted
                                   : ControlResultCode::InternalError,
                               "UI capture artifact publication failed");
            }
            const auto& metadata = *stored.metadata;
            auto detail = choc::value::createObject("ControlUiCaptureResult");
            detail.addMember("artifact_id", metadata.artifact_id);
            detail.addMember("mime_type", metadata.content_type);
            detail.addMember("sha256", metadata.sha256);
            detail.addMember("byte_count", static_cast<std::int64_t>(metadata.byte_size));
            detail.addMember("width", static_cast<std::int64_t>(captured.width));
            detail.addMember("height", static_cast<std::int64_t>(captured.height));
            detail.addMember("redaction_state", "original");
            return {.terminal_state = ControlReceiptState::Completed,
                    .result = {.detail_json = choc::json::toString(detail, true),
                               .artifacts = {{.artifact_id = metadata.artifact_id,
                                              .media_type = metadata.content_type,
                                              .byte_size = metadata.byte_size}}}};
        }
        if (request.operation_id != kRuntimeEvalOperation ||
            plan.capability != InspectorCapability::RuntimeEval ||
            acquired_binding.registration.profile != ControlBuildProfile::ResearchUnsafe ||
            !acquired_binding.manifest.unsafe_runtime_eval_acknowledged || !acquired_evaluator ||
            acquired_evaluator->binary_marker() != kRuntimeEvalComponentMarker) {
            return failure(ControlResultCode::NotImplemented,
                           "host does not implement the requested UI operation");
        }

        std::string source;
        try {
            source = choc::json::parse(request.params_json)["source"].getString();
        } catch (...) {
            return failure(ControlResultCode::InvalidRequest,
                           "runtime evaluation request could not be decoded");
        }
        const auto capabilities = acquired_evaluator->capabilities();
        if (!capabilities.can_evaluate || !capabilities.can_interrupt)
            return failure(ControlResultCode::HostUnavailable, "runtime evaluator is unavailable",
                           ControlRetryClassification::AfterRefresh);

        const auto now = std::chrono::system_clock::now();
        const auto deadline =
            std::chrono::system_clock::time_point{std::chrono::milliseconds{plan.deadline_unix_ms}};
        if (deadline <= now)
            return failure(ControlResultCode::DeadlineExceeded,
                           "runtime evaluation exceeded its deadline");
        auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        timeout = std::max(timeout, std::chrono::milliseconds{1});

        std::jthread watchdog([&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
                if (stop.stop_requested())
                    return;
                const auto checkpoint = context.checkpoint();
                if (checkpoint != ControlExecutionCheckpoint::Continue) {
                    (void)acquired_evaluator->interrupt();
                    return;
                }
            }
        });
        auto evaluated = acquired_evaluator->evaluate(source, timeout, maximum_eval_result_bytes);
        watchdog.request_stop();
        watchdog.join();
        const auto after_eval = context.checkpoint();
        if (after_eval != ControlExecutionCheckpoint::Continue)
            return checkpoint_failure(after_eval);
        if (!evaluated.ok) {
            if (evaluated.timed_out)
                return failure(ControlResultCode::DeadlineExceeded,
                               "runtime evaluation exceeded its deadline");
            if (evaluated.busy)
                return failure(ControlResultCode::ResourceExhausted, "runtime evaluator is busy",
                               ControlRetryClassification::AfterBackoff);
            if (evaluated.detached)
                return failure(ControlResultCode::HostUnavailable, "runtime evaluator detached",
                               ControlRetryClassification::AfterRefresh);
            return failure(ControlResultCode::InternalError, "runtime evaluation failed");
        }
        if (evaluated.json.size() > maximum_eval_result_bytes || !redact_eval)
            return failure(ControlResultCode::ResourceExhausted,
                           "runtime evaluation result exceeds the control response bound");
        const auto redacted = redact_eval(evaluated.json);
        if (!redacted || redacted->size() > maximum_eval_result_bytes)
            return failure(ControlResultCode::PolicyDenied,
                           "runtime evaluation result could not be safely redacted");
        try {
            (void)choc::json::parse(*redacted);
        } catch (...) {
            return failure(ControlResultCode::InternalError,
                           "runtime evaluator returned invalid JSON");
        }
        auto detail = choc::value::createObject("ControlRuntimeEvalResult");
        detail.addMember("receipt_id", plan.receipt_id.value);
        detail.addMember("result_json", *redacted);
        detail.addMember("completed", true);
        return {.terminal_state = ControlReceiptState::Completed,
                .result = {.detail_json = choc::json::toString(detail, true)}};
    }

    ControlExecutionOutcome execute(const ControlAdmissionPlan& plan,
                                    const ControlRequestEnvelope& request,
                                    const ControlExecutionContext& context) {
        ControlOperationExecutor acquired;
        {
            std::lock_guard lock(mutex);
            if (!connected || !main_thread)
                return failure(ControlResultCode::SessionStale, "host UI executor is disconnected",
                               ControlRetryClassification::AfterRefresh);
            if (!exact_authority(plan, request, binding))
                return failure(ControlResultCode::PolicyDenied,
                               "host UI authority does not match the bound instance");
            acquired = main_thread;
        }
        auto bounded_plan = plan;
        auto bounded_request = request;
        if (request.operation_id == kRuntimeEvalOperation) {
            const auto timeout = eval_timeout_ms(request.params_json);
            if (!timeout)
                return failure(ControlResultCode::InvalidRequest,
                               "runtime evaluation request was not canonical");
            const auto requested_deadline =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    (std::chrono::system_clock::now() + std::chrono::milliseconds{*timeout})
                        .time_since_epoch())
                    .count();
            bounded_plan.deadline_unix_ms =
                std::min(bounded_plan.deadline_unix_ms, requested_deadline);
            bounded_request.deadline_unix_ms = bounded_plan.deadline_unix_ms;
        }
        return acquired(bounded_plan, bounded_request, context);
    }
};

ControlHostUiExecutor::ControlHostUiExecutor(std::shared_ptr<State> state)
    : state_(std::move(state)) {}

ControlHostUiExecutor::~ControlHostUiExecutor() {
    disconnect();
}

std::unique_ptr<ControlHostUiExecutor>
ControlHostUiExecutor::create(ControlHostUiExecutorConfig config) {
    const auto& registration = config.binding.registration;
    std::string manifest_error;
    if (!registration.registration_id || !registration.broker_id ||
        registration.session_id.empty() || registration.instance_id.empty() ||
        registration.publication_id.empty() || registration.manifest_digest.size() != 64 ||
        registration.artifact_digest.size() != 64 ||
        registration.profile != config.binding.manifest.profile ||
        registration.manifest_digest != control_manifest_digest(config.binding.manifest) ||
        registration.consent_identity !=
            control_consent_identity(registration.manifest_digest, registration.artifact_digest) ||
        !validate_control_manifest(config.binding.manifest, manifest_error) ||
        registration.capabilities != config.binding.manifest.capabilities ||
        !config.main_thread_rpc || config.maximum_capture_bytes == 0 ||
        config.maximum_capture_bytes > kControlCaptureMaximumBytes ||
        config.maximum_eval_result_bytes == 0 ||
        config.maximum_eval_result_bytes > kControlRuntimeEvalMaximumResultBytes ||
        config.capture_lifetime <= std::chrono::milliseconds::zero())
        return nullptr;
    if (config.runtime_evaluator &&
        (registration.profile != ControlBuildProfile::ResearchUnsafe ||
         !config.binding.manifest.unsafe_runtime_eval_acknowledged ||
         std::ranges::find(registration.capabilities, InspectorCapability::RuntimeEval) ==
             registration.capabilities.end() ||
         !config.redact_runtime_eval_result ||
         config.runtime_evaluator->binary_marker() != kRuntimeEvalComponentMarker))
        return nullptr;

    auto state = std::make_shared<State>();
    state->binding = std::move(config.binding);
    state->rpc = std::move(config.main_thread_rpc);
    state->capture = std::move(config.capture_source);
    state->evaluator = std::move(config.runtime_evaluator);
    state->redact_eval = std::move(config.redact_runtime_eval_result);
    state->maximum_capture_bytes = config.maximum_capture_bytes;
    state->maximum_eval_result_bytes = config.maximum_eval_result_bytes;
    state->capture_lifetime = config.capture_lifetime;
    std::weak_ptr<State> weak = state;
    ControlMainThreadExecutor main_thread{
        state->rpc, [weak](const ControlAdmissionPlan& plan, const ControlRequestEnvelope& request,
                           const ControlExecutionContext& context) {
            const auto locked = weak.lock();
            return locked
                       ? locked->execute_main(plan, request, context)
                       : failure(ControlResultCode::SessionStale, "host UI executor lifetime ended",
                                 ControlRetryClassification::AfterRefresh);
        }};
    state->main_thread = main_thread.executor();
    return std::unique_ptr<ControlHostUiExecutor>(new ControlHostUiExecutor(std::move(state)));
}

ControlOperationExecutor ControlHostUiExecutor::executor() const {
    const auto state = state_;
    return [state](const ControlAdmissionPlan& plan, const ControlRequestEnvelope& request,
                   const ControlExecutionContext& context) {
        return state->execute(plan, request, context);
    };
}

bool ControlHostUiExecutor::ready() const {
    std::lock_guard lock(state_->mutex);
    return state_->connected && static_cast<bool>(state_->main_thread) &&
           (state_->capture || state_->evaluator);
}

void ControlHostUiExecutor::disconnect() noexcept {
    if (!state_)
        return;
    std::lock_guard lock(state_->mutex);
    if (!state_->connected)
        return;
    state_->connected = false;
    state_->capture.reset();
    state_->evaluator.reset();
}

std::string_view control_ui_input_disposition() noexcept {
    return "unsupported-no-exact-host-target-seam";
}

} // namespace pulp::inspect
