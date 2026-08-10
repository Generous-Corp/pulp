#include <pulp/inspect/control_host_development_executor.hpp>

#include <pulp/inspect/control_main_thread_executor.hpp>
#include <pulp/inspect/control_protocol.hpp>
#include <pulp/runtime/crypto.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <ranges>
#include <unordered_map>
#include <utility>

namespace pulp::inspect {
namespace {

constexpr std::string_view kUiObserveOperation = "dev.pulp.ui/observe@1";
constexpr std::string_view kDiagnosticsOperation = "dev.pulp.diagnostics/read@1";
constexpr std::string_view kLogsOperation = "dev.pulp.logs/read@1";
constexpr std::string_view kTestInputOperation = "dev.pulp.test/input@1";
constexpr std::string_view kAuthoringOperation = "dev.pulp.authoring/tweaks@1";

ControlExecutionOutcome failure(
    ControlResultCode code, std::string explanation,
    ControlRetryClassification retry = ControlRetryClassification::Never) {
    return {.terminal_state = ControlReceiptState::Failed,
            .result = {.result_code = code,
                       .retry = retry,
                       .explanation = std::move(explanation)}};
}

ControlExecutionOutcome checkpoint_failure(ControlExecutionCheckpoint checkpoint) {
    if (checkpoint == ControlExecutionCheckpoint::DeadlineExceeded)
        return failure(ControlResultCode::DeadlineExceeded,
                       "development operation deadline exceeded");
    if (checkpoint == ControlExecutionCheckpoint::AuthorityRevoked)
        return failure(ControlResultCode::PolicyDenied,
                       "development operation authority was revoked");
    return {.terminal_state = ControlReceiptState::Cancelled,
            .result = {.result_code = ControlResultCode::Cancelled,
                       .explanation = "development operation was cancelled",
                       .cancellation_reason = "control-checkpoint"}};
}

bool bounded_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum || value.find('\0') != value.npos)
        return false;
    return std::ranges::all_of(value, [](unsigned char character) {
        return character >= 0x20 || character == '\n' || character == '\r' || character == '\t';
    });
}

bool exact_binding(const ControlAdmissionPlan& plan, const ControlRequestEnvelope& request,
                   const ControlHostDevelopmentBinding& binding) {
    const auto& registration = binding.registration;
    return plan.registration_id == registration.registration_id &&
           plan.broker_id == registration.broker_id && plan.session_id == registration.session_id &&
           plan.instance_id == registration.instance_id &&
           plan.publication_id == registration.publication_id &&
           plan.manifest_digest == registration.manifest_digest &&
           plan.producer_artifact_digest == registration.artifact_digest &&
           request.client_id == plan.client_id.value &&
           request.registration_id == plan.registration_id.value &&
           request.grant_id == plan.grant_id.value &&
           request.instance_generation == plan.instance_generation &&
           request.operation_id == plan.operation_id &&
           request.operation_version == plan.operation_version;
}

std::string_view severity_id(ControlDiagnosticSeverity severity) {
    switch (severity) {
    case ControlDiagnosticSeverity::Info: return "info";
    case ControlDiagnosticSeverity::Warning: return "warning";
    case ControlDiagnosticSeverity::Error: return "error";
    }
    return "error";
}

std::int64_t unix_milliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::optional<std::pair<std::string, std::size_t>> canonical_artifact(
    std::string json, std::size_t maximum_bytes) {
    if (json.size() > maximum_bytes)
        return std::nullopt;
    auto canonical = canonicalize_control_json(json);
    if (!canonical || canonical->size() > maximum_bytes)
        return std::nullopt;
    return std::pair{std::move(*canonical), canonical->size()};
}

} // namespace

struct ControlHostDevelopmentExecutor::State {
    explicit State(ControlHostDevelopmentExecutorConfig config_in)
        : config(std::move(config_in)) {}

    ControlExecutionOutcome publish_json(const ControlAdmissionPlan& plan,
                                         const ControlExecutionContext& context,
                                         std::string json, std::string_view content_type,
                                         choc::value::Value detail) {
        const auto maximum = std::min(context.maximum_artifact_bytes,
                                      kControlHostMaximumArtifactPublicationBytes);
        if (maximum == 0 || !context.publish_artifact)
            return failure(ControlResultCode::HostUnavailable,
                           "broker artifact publication is unavailable",
                           ControlRetryClassification::AfterRefresh);
        const auto canonical = canonical_artifact(std::move(json), maximum);
        if (!canonical)
            return failure(ControlResultCode::ResourceExhausted,
                           "development artifact exceeds its canonical bound");
        const auto checkpoint = context.checkpoint
                                    ? context.checkpoint()
                                    : ControlExecutionCheckpoint::AuthorityRevoked;
        if (checkpoint != ControlExecutionCheckpoint::Continue)
            return checkpoint_failure(checkpoint);
        const auto bytes = std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(canonical->first.data()), canonical->first.size());
        const auto stored = context.publish_artifact(
            bytes, {.content_type = std::string(content_type),
                    .sensitivity = ControlArtifactSensitivity::Sensitive,
                    .redaction_state = ControlArtifactRedactionState::Original,
                    .lifetime = config.artifact_lifetime});
        if (stored.status != ControlArtifactStatus::Stored || !stored.metadata)
            return failure(stored.status == ControlArtifactStatus::ResourceExhausted
                               ? ControlResultCode::ResourceExhausted
                               : ControlResultCode::InternalError,
                           "development artifact publication failed");
        const auto& metadata = *stored.metadata;
        detail.addMember("artifact_id", metadata.artifact_id);
        detail.addMember("mime_type", metadata.content_type);
        detail.addMember("sha256", metadata.sha256);
        detail.addMember("byte_count", static_cast<std::int64_t>(metadata.byte_size));
        return {.terminal_state = ControlReceiptState::Completed,
                .result = {.detail_json = choc::json::toString(detail, true),
                           .artifacts = {{.artifact_id = metadata.artifact_id,
                                          .media_type = metadata.content_type,
                                          .byte_size = metadata.byte_size}}}};
    }

    ControlExecutionOutcome execute(const ControlAdmissionPlan& plan,
                                    const ControlRequestEnvelope& request,
                                    const ControlExecutionContext& context) {
        std::lock_guard execution_lock(execution_mutex);
        if (!connected)
            return failure(ControlResultCode::SessionStale,
                           "development executor is disconnected",
                           ControlRetryClassification::AfterRefresh);
        if (!exact_binding(plan, request, config.binding))
            return failure(ControlResultCode::PolicyDenied,
                           "development authority does not match the bound instance");
        if (!context.checkpoint)
            return failure(ControlResultCode::PolicyDenied,
                           "development execution requires broker revalidation");
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
                               ? "development request does not match its canonical schema"
                               : diagnostics.explanation);
        }

        if (request.operation_id == kUiObserveOperation)
            return execute_ui(plan, request, context);
        if (request.operation_id == kDiagnosticsOperation)
            return execute_diagnostics(plan, context);
        if (request.operation_id == kLogsOperation)
            return execute_logs(plan, request, context);
        if (request.operation_id == kTestInputOperation)
            return execute_test_input(plan, request, context);
        if (request.operation_id == kAuthoringOperation)
            return execute_authoring(plan, request, context);
        return failure(ControlResultCode::NotImplemented,
                       "development executor does not implement this operation");
    }

    ControlExecutionOutcome execute_ui(const ControlAdmissionPlan& plan,
                                       const ControlRequestEnvelope& request,
                                       const ControlExecutionContext& context) {
        if (plan.capability != InspectorCapability::UiRead || !config.observe_ui)
            return failure(ControlResultCode::HostUnavailable, "UI observation is unavailable",
                           ControlRetryClassification::AfterRefresh);
        ControlUiObservationRequest input;
        try {
            const auto params = choc::json::parse(request.params_json);
            if (params.hasObjectMember("selector"))
                input.selector = std::string(params["selector"].getString());
            if (params.hasObjectMember("include_geometry"))
                input.include_geometry = params["include_geometry"].getBool();
        } catch (...) {
            return failure(ControlResultCode::InvalidRequest,
                           "UI observation request could not be decoded");
        }
        std::optional<ControlUiObservation> observed;
        try {
            observed = config.observe_ui(input);
        } catch (...) {
            return failure(ControlResultCode::InternalError,
                           "UI observation provider failed");
        }
        if (!observed || observed->node_count > 10'000)
            return failure(ControlResultCode::HostUnavailable,
                           "the requested UI observation is unavailable",
                           ControlRetryClassification::AfterRefresh);
        auto detail = choc::value::createObject("ControlUiObservationResult");
        detail.addMember("generation", static_cast<std::int64_t>(observed->generation));
        detail.addMember("node_count", static_cast<std::int64_t>(observed->node_count));
        return publish_json(plan, context, observed->tree_json,
                            "application/vnd.pulp.ui-tree+json", std::move(detail));
    }

    ControlExecutionOutcome execute_diagnostics(const ControlAdmissionPlan& plan,
                                                const ControlExecutionContext& context) {
        if (plan.capability != InspectorCapability::DiagnosticsRead || !config.read_diagnostics)
            return failure(ControlResultCode::HostUnavailable, "diagnostics are unavailable",
                           ControlRetryClassification::AfterRefresh);
        std::vector<ControlDiagnosticItem> items;
        try {
            items = config.read_diagnostics();
        } catch (...) {
            return failure(ControlResultCode::InternalError,
                           "diagnostic provider failed");
        }
        if (items.size() > 1024)
            return failure(ControlResultCode::ResourceExhausted,
                           "diagnostic item count exceeds the canonical bound");
        auto array = choc::value::createEmptyArray();
        for (const auto& item : items) {
            if (!bounded_text(item.id, 128) ||
                !bounded_text(item.message, kControlDevelopmentMaximumTextBytes))
                return failure(ControlResultCode::InvalidRequest,
                               "diagnostic provider returned an invalid item");
            auto encoded = choc::value::createObject("ControlDiagnosticItem");
            encoded.addMember("id", item.id);
            encoded.addMember("severity", severity_id(item.severity));
            encoded.addMember("message", item.message);
            array.addArrayElement(std::move(encoded));
        }
        const auto sampled_at = unix_milliseconds();
        auto document = choc::value::createObject("ControlDiagnosticsSnapshot");
        document.addMember("schema", "dev.pulp.diagnostics/snapshot@1");
        document.addMember("sampled_at_ms", sampled_at);
        document.addMember("items", std::move(array));
        auto detail = choc::value::createObject("ControlDiagnosticsResult");
        detail.addMember("item_count", static_cast<std::int64_t>(items.size()));
        detail.addMember("sampled_at_ms", sampled_at);
        return publish_json(plan, context, choc::json::toString(document, false),
                            "application/vnd.pulp.diagnostics+json", std::move(detail));
    }

    ControlExecutionOutcome execute_logs(const ControlAdmissionPlan& plan,
                                         const ControlRequestEnvelope& request,
                                         const ControlExecutionContext& context) {
        if (plan.capability != InspectorCapability::LogsRead || !config.read_logs)
            return failure(ControlResultCode::HostUnavailable, "logs are unavailable",
                           ControlRetryClassification::AfterRefresh);
        std::uint64_t after = 0;
        std::size_t limit = 200;
        try {
            const auto params = choc::json::parse(request.params_json);
            if (params.hasObjectMember("after_sequence"))
                after = static_cast<std::uint64_t>(params["after_sequence"].getInt64());
            if (params.hasObjectMember("limit"))
                limit = static_cast<std::size_t>(params["limit"].getInt64());
        } catch (...) {
            return failure(ControlResultCode::InvalidRequest, "log request could not be decoded");
        }
        ControlLogPage page;
        try {
            page = config.read_logs(after, limit);
        } catch (...) {
            return failure(ControlResultCode::InternalError, "log provider failed");
        }
        if (page.entries.size() > limit || page.entries.size() > 2000 ||
            page.next_sequence < after)
            return failure(ControlResultCode::InvalidRequest,
                           "log provider returned a non-canonical page");
        auto entries = choc::value::createEmptyArray();
        std::uint64_t previous = after;
        for (const auto& entry : page.entries) {
            if (entry.sequence <= previous || entry.sequence > page.next_sequence ||
                !bounded_text(entry.level, 32) ||
                !bounded_text(entry.message, kControlDevelopmentMaximumTextBytes, true))
                return failure(ControlResultCode::InvalidRequest,
                               "log provider returned an invalid entry");
            previous = entry.sequence;
            auto encoded = choc::value::createObject("ControlLogEntry");
            encoded.addMember("sequence", static_cast<std::int64_t>(entry.sequence));
            encoded.addMember("level", entry.level);
            encoded.addMember("message", entry.message);
            entries.addArrayElement(std::move(encoded));
        }
        auto document = choc::value::createObject("ControlLogPage");
        document.addMember("schema", "dev.pulp.logs/page@1");
        document.addMember("next_sequence", static_cast<std::int64_t>(page.next_sequence));
        document.addMember("entries", std::move(entries));
        auto detail = choc::value::createObject("ControlLogsResult");
        detail.addMember("entry_count", static_cast<std::int64_t>(page.entries.size()));
        detail.addMember("next_sequence", static_cast<std::int64_t>(page.next_sequence));
        return publish_json(plan, context, choc::json::toString(document, false),
                            "application/vnd.pulp.logs+json", std::move(detail));
    }

    ControlExecutionOutcome execute_test_input(const ControlAdmissionPlan& plan,
                                               const ControlRequestEnvelope& request,
                                               const ControlExecutionContext& context) {
        if (plan.capability != InspectorCapability::TestInput ||
            (!config.apply_test_note && !config.apply_test_transport))
            return failure(ControlResultCode::HostUnavailable, "test input is unavailable",
                           ControlRetryClassification::AfterRefresh);
        const auto params = choc::json::parse(request.params_json);
        const auto sequence = static_cast<std::uint64_t>(params["sequence"].getInt64());
        const auto owner = plan.client_principal;
        if (const auto found = test_input_sequences.find(owner);
            found != test_input_sequences.end() && sequence <= found->second.sequence)
            return failure(ControlResultCode::StateConflict,
                           "test input sequence must increase for this authority");

        TestInputApplyResult applied;
        const auto kind = params["kind"].getString();
        if (kind == "transport") {
            if (!config.apply_test_transport)
                return failure(ControlResultCode::HostUnavailable,
                               "transport test input is unavailable",
                               ControlRetryClassification::AfterRefresh);
            try {
                applied = config.apply_test_transport(
                    {.sequence = sequence,
                     .playing = params["playing"].getBool(),
                     .position_beats = params["position_beats"].getWithDefault(0.0),
                     .tempo_bpm = params["tempo_bpm"].getWithDefault(120.0)});
            } catch (...) {
                return failure(ControlResultCode::InternalError,
                               "transport test-input provider failed");
            }
        } else {
            if (!config.apply_test_note)
                return failure(ControlResultCode::HostUnavailable, "MIDI test input is unavailable",
                               ControlRetryClassification::AfterRefresh);
            try {
                applied = config.apply_test_note(
                    {.sequence = sequence,
                     .note_on = kind == "note-on",
                     .channel = static_cast<std::uint8_t>(params["channel"].getInt64()),
                     .note = static_cast<std::uint8_t>(params["note"].getInt64()),
                     .velocity = params["velocity"].getWithDefault(0.0)});
            } catch (...) {
                return failure(ControlResultCode::InternalError,
                               "MIDI test-input provider failed");
            }
        }
        if (!applied.applied)
            return failure(applied.error_code == "queue_full" ? ControlResultCode::ResourceExhausted
                                                              : ControlResultCode::InvalidRequest,
                           applied.error_message.empty() ? "test input was rejected"
                                                         : applied.error_message);
        const auto after_apply = context.checkpoint();
        if (after_apply != ControlExecutionCheckpoint::Continue) {
            release_input(TestInputReleaseReason::ControllerReleased);
            return checkpoint_failure(after_apply);
        }
        test_input_sequences[owner] = {
            .opaque_authority_id = plan.client_id.value,
            .sequence = sequence,
        };
        auto detail = choc::value::createObject("ControlTestInputResult");
        detail.addMember("receipt_id", plan.receipt_id.value);
        detail.addMember("accepted_sequence", static_cast<std::int64_t>(sequence));
        return {.terminal_state = ControlReceiptState::Completed,
                .result = {.detail_json = choc::json::toString(detail, true)}};
    }

    ControlExecutionOutcome execute_authoring(const ControlAdmissionPlan& plan,
                                              const ControlRequestEnvelope& request,
                                              const ControlExecutionContext& context) {
        if (plan.capability != InspectorCapability::AuthoringTweaks || !config.apply_authoring)
            return failure(ControlResultCode::HostUnavailable,
                           "authoring tweaks are unavailable",
                           ControlRetryClassification::AfterRefresh);
        ControlAuthoringChanges changes;
        try {
            const auto params = choc::json::parse(request.params_json);
            if (params.hasObjectMember("anchor_id"))
                changes.anchor_id = std::string(params["anchor_id"].getString());
            const auto values = params["changes"];
            if (values.hasObjectMember("bypass"))
                changes.bypass = values["bypass"].getBool();
            if (values.hasObjectMember("lock"))
                changes.locked = values["lock"].getBool();
            if (values.hasObjectMember("highlight_node_id"))
                changes.highlight_node_id = std::string(values["highlight_node_id"].getString());
            if (values.hasObjectMember("repaint_flash"))
                changes.repaint_flash = values["repaint_flash"].getBool();
            if (values.hasObjectMember("constants")) {
                values["constants"].visitObjectMembers(
                    [&](std::string_view name, choc::value::ValueView value) {
                        changes.constants.emplace_back(
                            name, value.getWithDefault(std::numeric_limits<double>::quiet_NaN()));
                    });
            }
        } catch (...) {
            return failure(ControlResultCode::InvalidRequest,
                           "authoring request could not be decoded");
        }
        ControlAuthoringApplyResult applied;
        try {
            applied = config.apply_authoring(changes);
        } catch (...) {
            return failure(ControlResultCode::InternalError,
                           "authoring provider failed");
        }
        if (!applied.applied)
            return failure(ControlResultCode::InvalidRequest,
                           applied.explanation.empty() ? "authoring changes were rejected"
                                                       : applied.explanation);
        const auto after_apply = context.checkpoint();
        if (after_apply != ControlExecutionCheckpoint::Continue)
            return {.terminal_state = ControlReceiptState::CompletedAfterRevocation,
                    .result = {.result_code = ControlResultCode::CompletedAfterRevocation,
                               .explanation = "authoring changes completed after authority ended"}};
        auto detail = choc::value::createObject("ControlAuthoringResult");
        detail.addMember("receipt_id", plan.receipt_id.value);
        detail.addMember("generation", static_cast<std::int64_t>(applied.generation));
        return {.terminal_state = ControlReceiptState::Completed,
                .result = {.detail_json = choc::json::toString(detail, true)}};
    }

    void end_authority(std::string_view authority, TestInputReleaseReason reason) noexcept {
        std::lock_guard lock(execution_mutex);
        const auto erased = std::erase_if(test_input_sequences, [&](const auto& entry) {
            return entry.second.opaque_authority_id == authority;
        });
        if (erased != 0)
            release_input(reason);
    }

    void end_controller_scope(std::string_view principal,
                              TestInputReleaseReason reason) noexcept {
        std::lock_guard lock(execution_mutex);
        if (test_input_sequences.erase(std::string(principal)) != 0)
            release_input(reason);
    }

    void disconnect() noexcept {
        std::lock_guard lock(execution_mutex);
        if (!connected)
            return;
        connected = false;
        if (!test_input_sequences.empty())
            release_input(TestInputReleaseReason::SessionTeardown);
        test_input_sequences.clear();
    }

    void release_input(TestInputReleaseReason reason) noexcept {
        if (!config.release_test_input)
            return;
        try {
            config.release_test_input(reason);
        } catch (...) {
        }
    }

    ControlHostDevelopmentExecutorConfig config;
    mutable std::mutex execution_mutex;
    struct TestInputSequence {
        std::string opaque_authority_id;
        std::uint64_t sequence = 0;
    };
    std::unordered_map<std::string, TestInputSequence> test_input_sequences;
    bool connected = true;
};

ControlHostDevelopmentExecutor::ControlHostDevelopmentExecutor(
    std::shared_ptr<State> state, ControlOperationExecutor executor)
    : state_(std::move(state)), executor_(std::move(executor)) {}

ControlHostDevelopmentExecutor::~ControlHostDevelopmentExecutor() {
    disconnect();
}

std::unique_ptr<ControlHostDevelopmentExecutor>
ControlHostDevelopmentExecutor::create(ControlHostDevelopmentExecutorConfig config) {
    if (!config.main_thread_rpc || !config.binding.registration.registration_id ||
        config.binding.registration.host_tier != ControlHostTier::Standalone ||
        control_manifest_digest(config.binding.manifest) !=
            config.binding.registration.manifest_digest ||
        config.binding.manifest.bundle_id != config.binding.registration.plugin_id ||
        config.artifact_lifetime.count() <= 0)
        return nullptr;
    auto state = std::make_shared<State>(std::move(config));
    ControlMainThreadExecutor main_thread(
        state->config.main_thread_rpc,
        [state](const ControlAdmissionPlan& plan, const ControlRequestEnvelope& request,
                const ControlExecutionContext& context) {
            return state->execute(plan, request, context);
        });
    return std::unique_ptr<ControlHostDevelopmentExecutor>(
        new ControlHostDevelopmentExecutor(std::move(state), main_thread.executor()));
}

ControlOperationExecutor ControlHostDevelopmentExecutor::executor() const {
    return executor_;
}

void ControlHostDevelopmentExecutor::end_authority(std::string_view opaque_authority_id,
                                                   TestInputReleaseReason reason) noexcept {
    state_->end_authority(opaque_authority_id, reason);
}

void ControlHostDevelopmentExecutor::end_controller_scope(
    std::string_view controller_principal,
    TestInputReleaseReason reason) noexcept {
    state_->end_controller_scope(controller_principal, reason);
}

void ControlHostDevelopmentExecutor::disconnect() noexcept {
    if (state_)
        state_->disconnect();
}

bool ControlHostDevelopmentExecutor::ready() const {
    std::lock_guard lock(state_->execution_mutex);
    return state_->connected;
}

} // namespace pulp::inspect
