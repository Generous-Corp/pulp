#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/control_host_development_executor.hpp>

#include <choc/text/choc_JSON.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using namespace pulp::inspect;

namespace {

std::shared_ptr<InspectorMainThreadRpc> inline_rpc() {
    return std::make_shared<InspectorMainThreadRpc>(
        InspectorMainThreadRpc::Config{1s, 8},
        [](std::function<void()> task) {
            task();
            return true;
        },
        [] { return false; });
}

ControlHostDevelopmentBinding binding() {
    ControlManifest manifest;
    manifest.profile = ControlBuildProfile::DeveloperLocal;
    manifest.target = "ControlHostDevelopmentTest";
    manifest.product_name = "Control Host Development Test";
    manifest.bundle_id = "dev.pulp.test.control-host-development";
    manifest.build_id = "build:0123456789abcdef0123456789abcdef";
    manifest.endpoint_included = true;
    manifest.capabilities = {
        InspectorCapability::UiRead,
        InspectorCapability::DiagnosticsRead,
        InspectorCapability::LogsRead,
        InspectorCapability::TestInput,
        InspectorCapability::AuthoringTweaks,
    };

    ControlRegistration registration;
    registration.registration_id = ControlRegistrationId{"registration-a"};
    registration.broker_id = ControlBrokerId{"broker-a"};
    registration.session_id = "session-a";
    registration.instance_id = "instance-a";
    registration.publication_id = "publication-a";
    registration.plugin_id = manifest.bundle_id;
    registration.manifest_digest = control_manifest_digest(manifest);
    registration.artifact_digest = std::string(64, 'c');
    registration.consent_identity =
        control_consent_identity(registration.manifest_digest, registration.artifact_digest);
    registration.profile = manifest.profile;
    registration.host_tier = ControlHostTier::Standalone;
    registration.capabilities = manifest.capabilities;
    return {.registration = std::move(registration), .manifest = std::move(manifest)};
}

ControlAdmissionPlan plan(InspectorCapability capability, std::string operation) {
    const auto authority = binding();
    ControlAdmissionPlan value;
    value.broker_id = authority.registration.broker_id;
    value.client_principal = "principal-a";
    value.client_id = ControlClientId{"client-a"};
    value.registration_id = authority.registration.registration_id;
    value.grant_id = ControlGrantId{"grant-a"};
    value.session_id = authority.registration.session_id;
    value.instance_id = authority.registration.instance_id;
    value.publication_id = authority.registration.publication_id;
    value.instance_generation = authority.registration.publication_id;
    value.capability = capability;
    value.operation_id = std::move(operation);
    value.operation_version = 1;
    value.manifest_digest = authority.registration.manifest_digest;
    value.producer_artifact_digest = authority.registration.artifact_digest;
    value.deadline_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 (std::chrono::system_clock::now() + 5s).time_since_epoch())
                                 .count();
    value.receipt_id = ControlReceiptId{"receipt-a"};
    return value;
}

ControlRequestEnvelope request(const ControlAdmissionPlan& admission, std::string params) {
    return {
        .request_id = "request-a",
        .client_id = admission.client_id.value,
        .registration_id = admission.registration_id.value,
        .grant_id = admission.grant_id.value,
        .instance_generation = admission.instance_generation,
        .operation_id = admission.operation_id,
        .operation_version = admission.operation_version,
        .deadline_unix_ms = admission.deadline_unix_ms,
        .params_json = std::move(params),
    };
}

struct Providers {
    std::optional<ControlUiObservation> observation =
        ControlUiObservation{R"({"nodes":[{"id":"gain"}]})", 7, 1};
    std::vector<ControlDiagnosticItem> diagnostics{
        {"info", ControlDiagnosticSeverity::Info, "ready"},
        {"warning", ControlDiagnosticSeverity::Warning, "warm"},
        {"error", ControlDiagnosticSeverity::Error, "bounded"},
    };
    ControlLogPage logs{{{5, "info", "first"}, {6, "warning", "second"}}, 6};
    TestInputApplyResult note_result = TestInputApplyResult::success();
    TestInputApplyResult transport_result = TestInputApplyResult::success();
    ControlAuthoringApplyResult authoring_result{true, 9, {}};
    ControlUiObservationRequest observed_request;
    ControlTestNoteInput note;
    ControlTestTransportInput transport;
    ControlAuthoringChanges authoring;
    std::vector<TestInputReleaseReason> releases;
    bool throw_observe = false;
    bool throw_diagnostics = false;
    bool throw_logs = false;
    bool throw_note = false;
    bool throw_transport = false;
    bool throw_authoring = false;
};

ControlHostDevelopmentExecutorConfig config(const std::shared_ptr<Providers>& providers) {
    return {
        .binding = binding(),
        .main_thread_rpc = inline_rpc(),
        .observe_ui =
            [providers](const ControlUiObservationRequest& input) {
                if (providers->throw_observe)
                    throw std::runtime_error("observe");
                providers->observed_request = input;
                return providers->observation;
            },
        .read_diagnostics =
            [providers] {
                if (providers->throw_diagnostics)
                    throw std::runtime_error("diagnostics");
                return providers->diagnostics;
            },
        .read_logs =
            [providers](std::uint64_t, std::size_t) {
                if (providers->throw_logs)
                    throw std::runtime_error("logs");
                return providers->logs;
            },
        .apply_test_note =
            [providers](const ControlTestNoteInput& input) {
                if (providers->throw_note)
                    throw std::runtime_error("note");
                providers->note = input;
                return providers->note_result;
            },
        .apply_test_transport =
            [providers](const ControlTestTransportInput& input) {
                if (providers->throw_transport)
                    throw std::runtime_error("transport");
                providers->transport = input;
                return providers->transport_result;
            },
        .release_test_input =
            [providers](TestInputReleaseReason reason) { providers->releases.push_back(reason); },
        .apply_authoring =
            [providers](const ControlAuthoringChanges& changes) {
                if (providers->throw_authoring)
                    throw std::runtime_error("authoring");
                providers->authoring = changes;
                return providers->authoring_result;
            },
        .artifact_lifetime = 30s,
    };
}

ControlExecutionContext context(std::vector<std::string>* published = nullptr) {
    return {
        .checkpoint = [] { return ControlExecutionCheckpoint::Continue; },
        .maximum_artifact_bytes = 16 * 1024,
        .publish_artifact =
            [published](std::span<const std::uint8_t> bytes,
                        ControlArtifactPublication publication) {
                if (published)
                    published->emplace_back(reinterpret_cast<const char*>(bytes.data()),
                                            bytes.size());
                ControlArtifactMetadata metadata;
                metadata.artifact_id = "artifact-0123456789abcdef0123456789abcdef";
                metadata.content_type = publication.content_type;
                metadata.sha256 = std::string(64, 'd');
                metadata.byte_size = bytes.size();
                return ControlArtifactStoreResult{ControlArtifactStatus::Stored,
                                                  std::move(metadata)};
            },
    };
}

ControlExecutionOutcome invoke(const ControlOperationExecutor& executor,
                               InspectorCapability capability, std::string operation,
                               std::string params, ControlExecutionContext execution = context()) {
    const auto admission = plan(capability, std::move(operation));
    return executor(admission, request(admission, std::move(params)), execution);
}

} // namespace

TEST_CASE("development executor publishes bounded UI diagnostics and logs",
          "[inspect][control][development][artifact]") {
    auto providers = std::make_shared<Providers>();
    auto adapter = ControlHostDevelopmentExecutor::create(config(providers));
    REQUIRE(adapter);
    REQUIRE(adapter->ready());
    std::vector<std::string> published;

    const auto ui = invoke(adapter->executor(), InspectorCapability::UiRead,
                           "dev.pulp.ui/observe@1",
                           R"({"selector":"#gain","include_geometry":false})",
                           context(&published));
    REQUIRE(ui.terminal_state == ControlReceiptState::Completed);
    CHECK(providers->observed_request.selector == "#gain");
    CHECK_FALSE(providers->observed_request.include_geometry);
    CHECK(choc::json::parse(ui.result.detail_json)["node_count"].getInt64() == 1);

    const auto diagnostics = invoke(adapter->executor(), InspectorCapability::DiagnosticsRead,
                                    "dev.pulp.diagnostics/read@1", "{}",
                                    context(&published));
    REQUIRE(diagnostics.terminal_state == ControlReceiptState::Completed);
    CHECK(choc::json::parse(diagnostics.result.detail_json)["item_count"].getInt64() == 3);
    const auto diagnostics_document = choc::json::parse(published.back());
    CHECK(diagnostics_document["items"][2]["severity"].getString() == "error");

    const auto logs = invoke(adapter->executor(), InspectorCapability::LogsRead,
                             "dev.pulp.logs/read@1", R"({"after_sequence":4,"limit":2})",
                             context(&published));
    REQUIRE(logs.terminal_state == ControlReceiptState::Completed);
    CHECK(choc::json::parse(logs.result.detail_json)["next_sequence"].getInt64() == 6);
    CHECK(published.size() == 3);
}

TEST_CASE("development executor enforces artifact and provider bounds",
          "[inspect][control][development][bounds]") {
    auto providers = std::make_shared<Providers>();
    auto adapter = ControlHostDevelopmentExecutor::create(config(providers));
    REQUIRE(adapter);

    auto unavailable_context = context();
    unavailable_context.publish_artifact = {};
    CHECK(invoke(adapter->executor(), InspectorCapability::UiRead, "dev.pulp.ui/observe@1", "{}",
                 unavailable_context)
              .result.result_code == ControlResultCode::HostUnavailable);

    auto tiny_context = context();
    tiny_context.maximum_artifact_bytes = 2;
    CHECK(invoke(adapter->executor(), InspectorCapability::UiRead, "dev.pulp.ui/observe@1", "{}",
                 tiny_context)
              .result.result_code == ControlResultCode::ResourceExhausted);

    auto failed_store = context();
    failed_store.publish_artifact = [](std::span<const std::uint8_t>,
                                       ControlArtifactPublication) {
        return ControlArtifactStoreResult{ControlArtifactStatus::ResourceExhausted, std::nullopt};
    };
    CHECK(invoke(adapter->executor(), InspectorCapability::UiRead, "dev.pulp.ui/observe@1", "{}",
                 failed_store)
              .result.result_code == ControlResultCode::ResourceExhausted);

    int checkpoints = 0;
    auto expired = context();
    expired.checkpoint = [&] {
        return ++checkpoints == 1 ? ControlExecutionCheckpoint::Continue
                                  : ControlExecutionCheckpoint::DeadlineExceeded;
    };
    CHECK(invoke(adapter->executor(), InspectorCapability::UiRead, "dev.pulp.ui/observe@1", "{}",
                 expired)
              .result.result_code == ControlResultCode::DeadlineExceeded);

    CHECK(invoke(adapter->executor(), InspectorCapability::UiRead, "dev.pulp.ui/observe@1",
                 R"({"selector":7})")
              .result.result_code == ControlResultCode::InvalidRequest);

    providers->observation->node_count = 10'001;
    CHECK(invoke(adapter->executor(), InspectorCapability::UiRead, "dev.pulp.ui/observe@1", "{}")
              .result.result_code == ControlResultCode::HostUnavailable);
    providers->observation->node_count = 1;
    providers->throw_observe = true;
    CHECK(invoke(adapter->executor(), InspectorCapability::UiRead, "dev.pulp.ui/observe@1", "{}")
              .result.result_code == ControlResultCode::InternalError);

    providers->throw_diagnostics = true;
    CHECK(invoke(adapter->executor(), InspectorCapability::DiagnosticsRead,
                 "dev.pulp.diagnostics/read@1", "{}")
              .result.result_code == ControlResultCode::InternalError);
    providers->throw_diagnostics = false;
    providers->diagnostics = {{"", ControlDiagnosticSeverity::Info, "invalid"}};
    CHECK(invoke(adapter->executor(), InspectorCapability::DiagnosticsRead,
                 "dev.pulp.diagnostics/read@1", "{}")
              .result.result_code == ControlResultCode::InvalidRequest);
    providers->diagnostics.assign(1025, {"item", ControlDiagnosticSeverity::Info, "bounded"});
    CHECK(invoke(adapter->executor(), InspectorCapability::DiagnosticsRead,
                 "dev.pulp.diagnostics/read@1", "{}")
              .result.result_code == ControlResultCode::ResourceExhausted);

    providers->logs = {{{5, "", "invalid"}}, 4};
    CHECK(invoke(adapter->executor(), InspectorCapability::LogsRead, "dev.pulp.logs/read@1",
                 R"({"after_sequence":4,"limit":2})")
              .result.result_code == ControlResultCode::InvalidRequest);
    providers->throw_logs = true;
    CHECK(invoke(adapter->executor(), InspectorCapability::LogsRead, "dev.pulp.logs/read@1", "{}")
              .result.result_code == ControlResultCode::InternalError);

    const auto unsupported = invoke(adapter->executor(), InspectorCapability::SessionDescribe,
                                    "dev.pulp.instance/read@1", "{}");
    INFO(unsupported.result.explanation);
    CHECK(unsupported.result.result_code == ControlResultCode::NotImplemented);
}

TEST_CASE("development executor sequences test input and releases controller state",
          "[inspect][control][development][test-input]") {
    auto providers = std::make_shared<Providers>();
    auto adapter = ControlHostDevelopmentExecutor::create(config(providers));
    REQUIRE(adapter);

    const auto note = invoke(
        adapter->executor(), InspectorCapability::TestInput, "dev.pulp.test/input@1",
        R"({"sequence":1,"kind":"note-on","channel":2,"note":64,"velocity":0.75})");
    REQUIRE(note.terminal_state == ControlReceiptState::Completed);
    CHECK(providers->note.sequence == 1);
    CHECK(providers->note.note_on);
    CHECK(providers->note.channel == 2);
    CHECK(providers->note.note == 64);

    CHECK(invoke(adapter->executor(), InspectorCapability::TestInput, "dev.pulp.test/input@1",
                 R"({"sequence":1,"kind":"note-off","channel":2,"note":64,"velocity":0})")
              .result.result_code == ControlResultCode::StateConflict);
    const auto transport = invoke(
        adapter->executor(), InspectorCapability::TestInput, "dev.pulp.test/input@1",
        R"({"sequence":2,"kind":"transport","playing":true,"position_beats":8,"tempo_bpm":123})");
    REQUIRE(transport.terminal_state == ControlReceiptState::Completed);
    CHECK(providers->transport.playing);
    CHECK(providers->transport.tempo_bpm == 123.0);

    adapter->end_controller_scope("principal-a", TestInputReleaseReason::ControllerExpired);
    REQUIRE(providers->releases.size() == 1);
    CHECK(providers->releases.back() == TestInputReleaseReason::ControllerExpired);

    providers->note_result = TestInputApplyResult::failure("queue_full", "busy");
    CHECK(invoke(adapter->executor(), InspectorCapability::TestInput, "dev.pulp.test/input@1",
                 R"({"sequence":3,"kind":"note-off","channel":2,"note":64,"velocity":0})")
              .result.result_code == ControlResultCode::ResourceExhausted);
    providers->throw_transport = true;
    CHECK(invoke(adapter->executor(), InspectorCapability::TestInput, "dev.pulp.test/input@1",
                 R"({"sequence":4,"kind":"transport","playing":false,"position_beats":0,"tempo_bpm":120})")
              .result.result_code == ControlResultCode::InternalError);
    providers->throw_transport = false;
    providers->note_result = TestInputApplyResult::success();
    int checkpoints = 0;
    auto revoked = context();
    revoked.checkpoint = [&] {
        return ++checkpoints < 3 ? ControlExecutionCheckpoint::Continue
                                 : ControlExecutionCheckpoint::AuthorityRevoked;
    };
    CHECK(invoke(adapter->executor(), InspectorCapability::TestInput, "dev.pulp.test/input@1",
                 R"({"sequence":5,"kind":"note-on","channel":2,"note":65,"velocity":0.5})",
                 revoked)
              .terminal_state == ControlReceiptState::Failed);
    CHECK(providers->releases.back() == TestInputReleaseReason::ControllerReleased);

    REQUIRE(invoke(adapter->executor(), InspectorCapability::TestInput, "dev.pulp.test/input@1",
                   R"({"sequence":6,"kind":"note-on","channel":2,"note":66,"velocity":0.5})")
                .terminal_state == ControlReceiptState::Completed);
    adapter->end_authority("client-a", TestInputReleaseReason::ClientDisconnected);
    CHECK(providers->releases.back() == TestInputReleaseReason::ClientDisconnected);

    REQUIRE(invoke(adapter->executor(), InspectorCapability::TestInput, "dev.pulp.test/input@1",
                   R"({"sequence":7,"kind":"note-on","channel":2,"note":67,"velocity":0.5})")
                .terminal_state == ControlReceiptState::Completed);
    adapter->disconnect();
    CHECK(providers->releases.back() == TestInputReleaseReason::SessionTeardown);
}

TEST_CASE("development executor applies authoring and fences ended authority",
          "[inspect][control][development][authoring]") {
    auto providers = std::make_shared<Providers>();
    auto adapter = ControlHostDevelopmentExecutor::create(config(providers));
    REQUIRE(adapter);
    const auto params =
        R"({"anchor_id":"gain","idempotency_key":"tweak-a","changes":{"bypass":true,"lock":false,"constants":{"gain":0.25},"highlight_node_id":"meter","repaint_flash":true}})";
    const auto applied = invoke(adapter->executor(), InspectorCapability::AuthoringTweaks,
                                "dev.pulp.authoring/tweaks@1", params);
    REQUIRE(applied.terminal_state == ControlReceiptState::Completed);
    CHECK(providers->authoring.anchor_id == "gain");
    CHECK(providers->authoring.bypass == true);
    CHECK(providers->authoring.locked == false);
    REQUIRE(providers->authoring.constants.size() == 1);
    CHECK(providers->authoring.constants[0] == std::pair<std::string, double>{"gain", 0.25});

    int checkpoints = 0;
    auto revoked = context();
    revoked.checkpoint = [&] {
        return ++checkpoints < 3 ? ControlExecutionCheckpoint::Continue
                                 : ControlExecutionCheckpoint::AuthorityRevoked;
    };
    CHECK(invoke(adapter->executor(), InspectorCapability::AuthoringTweaks,
                 "dev.pulp.authoring/tweaks@1", params, revoked)
              .terminal_state == ControlReceiptState::CompletedAfterRevocation);

    providers->authoring_result = {false, 0, "rejected"};
    CHECK(invoke(adapter->executor(), InspectorCapability::AuthoringTweaks,
                 "dev.pulp.authoring/tweaks@1", params)
              .result.result_code == ControlResultCode::InvalidRequest);
    providers->throw_authoring = true;
    CHECK(invoke(adapter->executor(), InspectorCapability::AuthoringTweaks,
                 "dev.pulp.authoring/tweaks@1", params)
              .result.result_code == ControlResultCode::InternalError);
}

TEST_CASE("development executor rejects invalid construction binding and lifecycle",
          "[inspect][control][development][security]") {
    auto providers = std::make_shared<Providers>();
    auto invalid = config(providers);
    invalid.binding.registration.host_tier = ControlHostTier::OfflineJob;
    CHECK_FALSE(ControlHostDevelopmentExecutor::create(std::move(invalid)));
    invalid = config(providers);
    invalid.binding.manifest.bundle_id = "dev.pulp.test.mismatch";
    CHECK_FALSE(ControlHostDevelopmentExecutor::create(std::move(invalid)));
    invalid = config(providers);
    invalid.artifact_lifetime = 0ms;
    CHECK_FALSE(ControlHostDevelopmentExecutor::create(std::move(invalid)));

    auto adapter = ControlHostDevelopmentExecutor::create(config(providers));
    REQUIRE(adapter);
    auto wrong = plan(InspectorCapability::UiRead, "dev.pulp.ui/observe@1");
    wrong.instance_id = "other-instance";
    CHECK(adapter->executor()(wrong, request(wrong, "{}"), context()).result.result_code ==
          ControlResultCode::PolicyDenied);

    auto no_checkpoint = context();
    no_checkpoint.checkpoint = {};
    CHECK(invoke(adapter->executor(), InspectorCapability::UiRead, "dev.pulp.ui/observe@1", "{}",
                 no_checkpoint)
              .result.result_code == ControlResultCode::Cancelled);

    adapter->disconnect();
    CHECK_FALSE(adapter->ready());
    adapter->disconnect();
    CHECK(invoke(adapter->executor(), InspectorCapability::UiRead, "dev.pulp.ui/observe@1", "{}")
              .result.result_code == ControlResultCode::SessionStale);
}
