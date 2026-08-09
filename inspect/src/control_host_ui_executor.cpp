#include <pulp/inspect/control_host_ui_executor.hpp>

#include <pulp/inspect/capture_source.hpp>
#include <pulp/inspect/control_main_thread_executor.hpp>
#include <pulp/inspect/control_manifest.hpp>
#include <pulp/inspect/runtime_evaluator.hpp>
#include <pulp/runtime/crypto.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>

namespace pulp::inspect {
namespace {

constexpr std::string_view kCaptureOperation = "dev.pulp.ui/capture@1";
constexpr std::string_view kUiInputOperation = "dev.pulp.ui/input@1";
constexpr std::string_view kRuntimeEvalOperation = "dev.pulp.runtime/evaluate@1";
constexpr std::string_view kRuntimeEvalComponentMarkerDigest =
    "08cf24197eb22433641d33d4a3445f685dcfe024de53d7bbd2b533c607db7832";

bool exact_runtime_eval_component(const std::shared_ptr<RuntimeEvaluator>& evaluator) {
    return evaluator &&
           runtime::sha256_hex(evaluator->binary_marker()) == kRuntimeEvalComponentMarkerDigest;
}

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
           plan.instance_generation == registration.publication_id &&
           plan.manifest_digest == registration.manifest_digest &&
           plan.producer_artifact_digest == registration.artifact_digest && plan.client_id &&
           plan.grant_id &&
           (!plan.consent_decision_id.empty() ||
            (plan.client_id.value == plan.grant_id.value &&
             !plan.client_principal.empty())) &&
           !request.request_id.empty() &&
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

struct ParsedCaptureRequest {
    bool node = false;
    std::string node_id;
    std::string view_generation;
};

std::optional<ParsedCaptureRequest> parse_capture_request(std::string_view params_json) {
    try {
        const auto params = choc::json::parse(params_json);
        if (!params.isObject() || !params["target"].isString() ||
            !params["format"].isString() || params["format"].getString() != "png")
            return std::nullopt;
        if (params["target"].getString() == "window")
            return ParsedCaptureRequest{};
        if (params["target"].getString() != "node" || !params["node_id"].isString() ||
            !params["view_generation"].isString())
            return std::nullopt;
        ParsedCaptureRequest parsed{.node = true,
                                    .node_id = std::string(params["node_id"].getString()),
                                    .view_generation =
                                        std::string(params["view_generation"].getString())};
        if (parsed.node_id.empty() || parsed.node_id.size() > kControlUiMaximumTargetBytes ||
            parsed.view_generation.empty() ||
            parsed.view_generation.size() > kControlUiMaximumViewGenerationBytes)
            return std::nullopt;
        return parsed;
    } catch (...) {
        return std::nullopt;
    }
}

struct ParsedInputRequest {
    std::string target_id;
    std::string view_generation;
    ControlUiInput input;
};

std::optional<ParsedInputRequest> parse_input_request(std::string_view params_json) {
    try {
        const auto params = choc::json::parse(params_json);
        if (!params.isObject() || !params["kind"].isString() ||
            !params["target_id"].isString() || !params["view_generation"].isString() ||
            !params["event"].isObject())
            return std::nullopt;
        ParsedInputRequest parsed{.target_id = std::string(params["target_id"].getString()),
                                  .view_generation =
                                      std::string(params["view_generation"].getString())};
        if (parsed.target_id.empty() || parsed.target_id.size() > kControlUiMaximumTargetBytes ||
            parsed.view_generation.empty() ||
            parsed.view_generation.size() > kControlUiMaximumViewGenerationBytes)
            return std::nullopt;
        const auto event = params["event"];
        const std::string_view kind = params["kind"].getString();
        if (kind == "pointer") {
            if (!event["phase"].isString() ||
                (!event["x"].isInt() && !event["x"].isFloat()) ||
                (!event["y"].isInt() && !event["y"].isFloat()))
                return std::nullopt;
            const auto x = event["x"].getWithDefault<double>(
                std::numeric_limits<double>::quiet_NaN());
            const auto y = event["y"].getWithDefault<double>(
                std::numeric_limits<double>::quiet_NaN());
            const auto button = event.hasObjectMember("button") ? event["button"].getInt64() : 0;
            if (!std::isfinite(x) || !std::isfinite(y) ||
                std::abs(x) > kControlUiMaximumCoordinate ||
                std::abs(y) > kControlUiMaximumCoordinate || button < 0 || button > 3)
                return std::nullopt;
            ControlUiPointerInput pointer{.x = x, .y = y,
                                          .button = static_cast<std::uint8_t>(button)};
            const std::string_view phase = event["phase"].getString();
            if (phase == "down")
                pointer.phase = ControlUiPointerInput::Phase::Down;
            else if (phase == "move")
                pointer.phase = ControlUiPointerInput::Phase::Move;
            else if (phase == "up")
                pointer.phase = ControlUiPointerInput::Phase::Up;
            else
                return std::nullopt;
            parsed.input = std::move(pointer);
        } else if (kind == "keyboard") {
            if (!event["phase"].isString() || !event["key"].isString() ||
                !event["repeat"].isBool())
                return std::nullopt;
            ControlUiKeyboardInput keyboard{.key = std::string(event["key"].getString()),
                                            .repeat = event["repeat"].getBool()};
            if (keyboard.key.empty() || keyboard.key.size() > kControlUiMaximumKeyBytes)
                return std::nullopt;
            const std::string_view phase = event["phase"].getString();
            if (phase == "down")
                keyboard.phase = ControlUiKeyboardInput::Phase::Down;
            else if (phase == "up")
                keyboard.phase = ControlUiKeyboardInput::Phase::Up;
            else
                return std::nullopt;
            parsed.input = std::move(keyboard);
        } else if (kind == "focus") {
            if (!event["focused"].isBool())
                return std::nullopt;
            parsed.input = ControlUiFocusInput{event["focused"].getBool()};
        } else if (kind == "text") {
            if (!event["text"].isString())
                return std::nullopt;
            ControlUiTextInput text_input{std::string(event["text"].getString())};
            if (text_input.text.empty() || text_input.text.size() > kControlUiMaximumTextBytes ||
                text_input.text.find('\0') != std::string::npos)
                return std::nullopt;
            parsed.input = std::move(text_input);
        } else {
            return std::nullopt;
        }
        return parsed;
    } catch (...) {
        return std::nullopt;
    }
}

ControlExecutionOutcome input_failure(ControlUiApplyStatus status) {
    switch (status) {
    case ControlUiApplyStatus::TargetUnavailable:
        return failure(ControlResultCode::InvalidRequest, "exact UI target is unavailable");
    case ControlUiApplyStatus::StaleGeneration:
        return failure(ControlResultCode::SessionStale, "UI view generation is stale",
                       ControlRetryClassification::AfterRefresh);
    case ControlUiApplyStatus::InvalidEvent:
        return failure(ControlResultCode::InvalidRequest, "UI event was rejected by the host");
    case ControlUiApplyStatus::Applied:
        break;
    }
    return failure(ControlResultCode::InternalError, "UI input returned an invalid host status");
}

} // namespace

struct ControlHostUiExecutor::State : std::enable_shared_from_this<State> {
    mutable std::mutex mutex;
    ControlHostUiBinding binding;
    std::shared_ptr<InspectorMainThreadRpc> rpc;
    std::shared_ptr<InspectorCaptureSource> capture;
    std::shared_ptr<ControlHostUiTargetAdapter> target_adapter;
    ControlUiAuthorityResolver resolve_authority;
    std::unordered_map<std::string, std::shared_ptr<void>> authority_subscriptions;
    std::string view_generation;
    std::shared_ptr<RuntimeEvaluator> evaluator;
    ControlRuntimeEvalRedactor redact_eval;
    std::size_t maximum_capture_bytes = 0;
    std::size_t maximum_eval_result_bytes = 0;
    std::chrono::milliseconds capture_lifetime{};
    ControlOperationExecutor main_thread;
    bool connected = true;

    bool retain_authority(const ControlUiProjectedAuthority& authority) {
        if (authority.owner.authority_id.empty() || !authority.authority_live ||
            !authority.authority_live() || !authority.subscribe_authority_end)
            return false;
        {
            std::lock_guard lock(mutex);
            if (!connected)
                return false;
            if (authority_subscriptions.contains(authority.owner.authority_id))
                return true;
        }
        const auto weak = weak_from_this();
        auto subscription = authority.subscribe_authority_end([weak, owner = authority.owner] {
            if (const auto state = weak.lock())
                state->release_authority(owner);
        });
        if (!subscription || !authority.authority_live())
            return false;
        std::lock_guard lock(mutex);
        if (!connected)
            return false;
        authority_subscriptions.try_emplace(authority.owner.authority_id,
                                            std::move(subscription));
        return true;
    }

    void release_authority(const ControlUiAuthorityOwner& owner) noexcept {
        std::shared_ptr<ControlHostUiTargetAdapter> adapter;
        std::shared_ptr<InspectorMainThreadRpc> dispatcher;
        {
            std::lock_guard lock(mutex);
            adapter = target_adapter;
            dispatcher = rpc;
        }
        if (adapter && dispatcher) {
            static std::atomic<std::int64_t> next_release_request{
                std::numeric_limits<std::int64_t>::min() + 1};
            const auto request_id = next_release_request.fetch_add(1, std::memory_order_relaxed);
            try {
                (void)dispatcher->call(request_id, [adapter, owner, request_id] {
                    adapter->release_controller(owner);
                    return make_response(request_id, R"({"released":true})");
                });
            } catch (...) {
            }
        }
        std::lock_guard lock(mutex);
        authority_subscriptions.erase(owner.authority_id);
    }

    ControlExecutionOutcome execute_main(const ControlAdmissionPlan& plan,
                                         const ControlRequestEnvelope& request,
                                         const ControlExecutionContext& context) {
        std::shared_ptr<InspectorCaptureSource> acquired_capture;
        std::shared_ptr<ControlHostUiTargetAdapter> acquired_target_adapter;
        std::shared_ptr<RuntimeEvaluator> acquired_evaluator;
        ControlUiAuthorityResolver acquired_authority_resolver;
        ControlHostUiBinding acquired_binding;
        {
            std::lock_guard lock(mutex);
            if (!connected)
                return failure(ControlResultCode::SessionStale, "host UI executor is disconnected",
                               ControlRetryClassification::AfterRefresh);
            acquired_capture = capture;
            acquired_target_adapter = target_adapter;
            acquired_evaluator = evaluator;
            acquired_authority_resolver = resolve_authority;
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
            if (plan.capability != InspectorCapability::UiInput)
                return failure(ControlResultCode::PolicyDenied,
                               "UI input capability does not match the admitted plan");
            if (!acquired_target_adapter)
                return failure(ControlResultCode::HostUnavailable,
                               "exact-target UI input is unavailable",
                               ControlRetryClassification::AfterRefresh);
            const auto parsed = parse_input_request(request.params_json);
            if (!parsed)
                return failure(ControlResultCode::InvalidRequest,
                               "UI input request was not canonical");
            if (parsed->view_generation != view_generation)
                return failure(ControlResultCode::SessionStale, "UI view generation is stale",
                               ControlRetryClassification::AfterRefresh);
            const ControlUiExactTarget target{.instance_id = acquired_binding.registration.instance_id,
                                              .instance_generation = plan.instance_generation,
                                              .view_generation = parsed->view_generation,
                                              .node_id = parsed->target_id};
            if (!acquired_authority_resolver)
                return failure(ControlResultCode::HostUnavailable,
                               "opaque UI authority resolver is unavailable",
                               ControlRetryClassification::AfterRefresh);
            const auto authority = acquired_authority_resolver(plan);
            if (!authority || !retain_authority(*authority))
                return checkpoint_failure(ControlExecutionCheckpoint::AuthorityRevoked);
            const auto& owner = authority->owner;
            const auto applied =
                acquired_target_adapter->dispatch_input(target, owner, parsed->input);
            if (applied != ControlUiApplyStatus::Applied)
                return input_failure(applied);
            const auto after_dispatch = context.checkpoint();
            if (after_dispatch != ControlExecutionCheckpoint::Continue) {
                acquired_target_adapter->release_controller(owner);
                return checkpoint_failure(after_dispatch);
            }
            auto detail = choc::value::createObject("ControlUiInputResult");
            detail.addMember("receipt_id", plan.receipt_id.value);
            detail.addMember("applied", true);
            return {.terminal_state = ControlReceiptState::Completed,
                    .result = {.detail_json = choc::json::toString(detail, true)}};
        }
        if (request.operation_id == kCaptureOperation) {
            if (plan.capability != InspectorCapability::CaptureImage)
                return failure(ControlResultCode::PolicyDenied,
                               "UI capture capability does not match the admitted plan");
            const auto parsed = parse_capture_request(request.params_json);
            if (!parsed) {
                return failure(ControlResultCode::InvalidRequest,
                               "UI capture request could not be decoded");
            }
            if (!context.publish_artifact)
                return failure(ControlResultCode::HostUnavailable,
                               "broker artifact publication is unavailable",
                               ControlRetryClassification::AfterRefresh);
            InspectorCapture captured;
            if (parsed->node) {
                if (!acquired_target_adapter)
                    return failure(ControlResultCode::HostUnavailable,
                                   "exact-target node capture is unavailable",
                                   ControlRetryClassification::AfterRefresh);
                if (parsed->view_generation != view_generation)
                    return failure(ControlResultCode::SessionStale, "UI view generation is stale",
                                   ControlRetryClassification::AfterRefresh);
                captured = acquired_target_adapter->capture_node_png(
                    {.instance_id = acquired_binding.registration.instance_id,
                     .instance_generation = plan.instance_generation,
                     .view_generation = parsed->view_generation,
                     .node_id = parsed->node_id});
            } else {
                if (!acquired_capture)
                    return failure(ControlResultCode::HostUnavailable,
                                   "exact-instance window capture is unavailable",
                                   ControlRetryClassification::AfterRefresh);
                captured = acquired_capture->capture_png();
            }
            if (!captured.error.empty()) {
                if (captured.error_code == "session_stale")
                    return failure(ControlResultCode::SessionStale, captured.error,
                                   ControlRetryClassification::AfterRefresh);
                if (captured.error_code == "target_unavailable")
                    return failure(ControlResultCode::InvalidRequest, captured.error);
                return failure(ControlResultCode::InternalError, "UI capture failed");
            }
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
            !acquired_binding.manifest.unsafe_runtime_eval_acknowledged ||
            !exact_runtime_eval_component(acquired_evaluator)) {
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
    (void)disconnect();
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
         !exact_runtime_eval_component(config.runtime_evaluator)))
        return nullptr;
    if (config.target_adapter &&
        (!config.resolve_authority || config.view_generation.empty() ||
         config.view_generation.size() > kControlUiMaximumViewGenerationBytes ||
         (std::ranges::find(registration.capabilities, InspectorCapability::CaptureImage) ==
              registration.capabilities.end() &&
          std::ranges::find(registration.capabilities, InspectorCapability::UiInput) ==
              registration.capabilities.end())))
        return nullptr;
    if (!config.target_adapter && (!config.view_generation.empty() || config.resolve_authority))
        return nullptr;

    auto state = std::make_shared<State>();
    state->binding = std::move(config.binding);
    state->rpc = std::move(config.main_thread_rpc);
    state->capture = std::move(config.capture_source);
    state->target_adapter = std::move(config.target_adapter);
    state->resolve_authority = std::move(config.resolve_authority);
    state->view_generation = std::move(config.view_generation);
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
           (state_->capture || state_->target_adapter || state_->evaluator);
}

bool ControlHostUiExecutor::release_controller_scope() noexcept {
    if (!state_)
        return true;
    std::shared_ptr<ControlHostUiTargetAdapter> adapter;
    std::shared_ptr<InspectorMainThreadRpc> rpc;
    {
        std::lock_guard lock(state_->mutex);
        adapter = state_->target_adapter;
        rpc = state_->rpc;
    }
    if (!adapter || !rpc)
        return true;
    static std::atomic<std::int64_t> next_request{
        std::numeric_limits<std::int64_t>::min() + 1024};
    const auto request_id = next_request.fetch_add(1, std::memory_order_relaxed);
    try {
        const auto response = rpc->call(request_id, [adapter, request_id] {
            adapter->release_controller(std::nullopt);
            return make_response(request_id, R"({"released":true})");
        });
        return !response.is_error;
    } catch (...) {
        return false;
    }
}

bool ControlHostUiExecutor::disconnect() noexcept {
    if (!state_)
        return true;
    std::shared_ptr<ControlHostUiTargetAdapter> released_adapter;
    std::shared_ptr<InspectorMainThreadRpc> released_rpc;
    {
        std::lock_guard lock(state_->mutex);
        if (!state_->connected && !state_->target_adapter)
            return true;
        state_->connected = false;
        state_->capture.reset();
        released_adapter = state_->target_adapter;
        released_rpc = state_->rpc;
        state_->evaluator.reset();
    }
    if (released_adapter && released_rpc) {
        constexpr std::int64_t kControllerReleaseRequest =
            std::numeric_limits<std::int64_t>::min();
        InspectorMessage response;
        try {
            response = released_rpc->call(
                kControllerReleaseRequest,
                [released_adapter] {
                    released_adapter->release_controller(std::nullopt);
                    return make_response(kControllerReleaseRequest, R"({"released":true})");
                });
        } catch (...) {
            return false;
        }
        if (response.is_error)
            return false;
        std::lock_guard lock(state_->mutex);
        if (state_->target_adapter == released_adapter) {
            state_->target_adapter.reset();
            state_->resolve_authority = {};
            state_->authority_subscriptions.clear();
        }
    }
    return true;
}

std::string_view control_ui_input_disposition() noexcept {
    return "supported-exact-target-grant-controlled";
}

} // namespace pulp::inspect
