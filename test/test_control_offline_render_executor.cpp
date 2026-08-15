#include <catch2/catch_test_macros.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/inspect/control_offline_render_executor.hpp>
#include <pulp/inspect/control_service.hpp>
#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

using namespace pulp::inspect;
using namespace std::chrono_literals;

namespace {

namespace fs = std::filesystem;

struct TemporaryDirectory {
    fs::path path;
    TemporaryDirectory() {
        const auto random = pulp::runtime::secure_random_bytes(8);
        REQUIRE(random);
        path = fs::temp_directory_path() /
               ("pulp-control-offline-" + pulp::runtime::hex_encode(*random));
        REQUIRE(fs::create_directory(path));
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

std::vector<std::pair<unsigned, int>> observed_midi;

class EventProbeProcessor final : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {.name = "T0EventProbe",
                .manufacturer = "PulpTest",
                .bundle_id = "dev.pulp.test.t0-event-probe",
                .version = "1.0.0",
                .category = pulp::format::PluginCategory::Instrument,
                .output_buses = {{"Output", 1}}};
    }

    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer& midi_in, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        for (const auto& event : midi_in)
            observed_midi.emplace_back(block_, event.sample_offset);
        output.clear();
        ++block_;
    }

  private:
    unsigned block_ = 0;
};

std::unique_ptr<pulp::format::Processor> create_event_probe() {
    return std::make_unique<EventProbeProcessor>();
}

VerifiedControlPeerIdentity peer(ControlPeerRole role, std::int64_t process_id,
                                 std::string start_id) {
    ControlPeerVerifier verifier([](const ControlPeerEvidence&) { return true; });
    auto verified = verifier.verify({.role = role,
                                     .user_id = "uid:501",
                                     .process_id = process_id,
                                     .process_start_id = std::move(start_id),
                                     .executable_identity = "signed:dev.pulp.t0-test",
                                     .publisher_id = "publisher.pulp"});
    REQUIRE(verified);
    return std::move(*verified);
}

ControlManifest offline_manifest() {
    return {.profile = ControlBuildProfile::TestDeterministic,
            .target = "T0OfflineFixture",
            .product_name = "T0 Offline Fixture",
            .bundle_id = "dev.pulp.t0-offline-fixture",
            .build_id = "build:0123456789abcdef0123456789abcdef",
            .endpoint_included = true,
            .capabilities = {InspectorCapability::RenderOffline}};
}

ControlService::Session negotiate(ControlService& service,
                                  const VerifiedControlPeerIdentity& client,
                                  const ControlClientId& client_id) {
    auto session = service.open_session(client, client_id);
    REQUIRE(session.is_open());
    const auto response = session.dispatch(encode_control_envelope(
        {.payload = ControlNegotiationOffer{.versions = {1, 1},
                                            .mandatory_features = {"receipts", "artifacts"}}}));
    REQUIRE(response.status == ControlServiceStatus::Responded);
    return session;
}

const ControlReceiptEnvelope& receipt(const ControlServiceResult& result) {
    REQUIRE(result.response);
    const auto* value = std::get_if<ControlReceiptEnvelope>(&result.response->payload);
    REQUIRE(value);
    return *value;
}

struct Fixture {
    TemporaryDirectory temporary;
    VerifiedControlPeerIdentity client = peer(ControlPeerRole::Client, 1001, "client-a");
    VerifiedControlPeerIdentity other_client = peer(ControlPeerRole::Client, 1002, "client-b");
    VerifiedControlPeerIdentity host = peer(ControlPeerRole::OfflineHost, 2001, "host-a");
    ControlBroker broker;
    ControlClientIdentity identity;
    ControlClientIdentity other_identity;
    ControlRegistration registration;
    ControlGrant grant;

    Fixture()
        : broker([&] {
              ControlBrokerConfig config;
              config.operation_store =
                  ControlOperationStoreConfig{.directory = temporary.path / "receipts"};
              config.artifact_store =
                  ControlArtifactStoreConfig{.root = temporary.path / "artifacts"};
              config.admission.host_available = [](const auto&, const auto&) { return true; };
              config.admission.activated = [](const auto&, const auto&) { return true; };
              config.admission.policy_eligible = [](const auto&, const auto&) { return true; };
              return config;
          }()) {
        identity = connect(client);
        other_identity = connect(other_client);
        const auto standalone = peer(ControlPeerRole::StandaloneHost, 2002, "host-standalone");
        auto denied_tier = broker.register_instance(
            standalone, {.host_tier = ControlHostTier::Standalone,
                         .session_id = "standalone-session",
                         .instance_id = "standalone-instance",
                         .publication_id = "standalone-publication",
                         .manifest = offline_manifest(),
                         .artifact_digest = std::string(64, 'b')});
        CHECK(denied_tier.status == ControlIdentityStatus::InvalidRequest);
        auto registered = broker.register_instance(
            host, {.host_tier = ControlHostTier::OfflineJob,
                   .session_id = "job-session-a",
                   .instance_id = "job-instance-a",
                   .publication_id = "job-publication-a",
                   .manifest = offline_manifest(),
                   .artifact_digest = std::string(64, 'a')});
        REQUIRE(registered.registration);
        registration = *registered.registration;
        auto granted = broker.issue_grant(
            client,
            {.client_id = identity.client_id,
             .registration_id = registration.registration_id,
             .capabilities = {InspectorCapability::RenderOffline},
             .ttl = 5min},
            {.approved = true,
             .authority = ControlConsentAuthority::TrustedPulpCli,
             .decision_id = "decision-t0"});
        REQUIRE(granted.grant);
        grant = *granted.grant;
    }

    ControlClientIdentity connect(const VerifiedControlPeerIdentity& who) {
        auto ticket = broker.issue_bootstrap(who);
        REQUIRE(ticket.ticket);
        auto connected = broker.redeem_bootstrap(ticket.ticket->ticket_id,
                                                 ticket.ticket->secret.bytes(), who);
        REQUIRE(connected.client);
        return *connected.client;
    }

    ControlRequestEnvelope request(std::string request_id, std::string idempotency_key) const {
        ControlRequestEnvelope value{
            .request_id = std::move(request_id),
            .client_id = identity.client_id.value,
            .registration_id = registration.registration_id.value,
            .grant_id = grant.grant_id.value,
            .instance_generation = registration.publication_id,
            .operation_id = "dev.pulp.render/offline@1",
            .operation_version = 1,
            .idempotency_key = std::move(idempotency_key),
            .deadline_unix_ms = 4'102'444'800'000,
            .params_json =
                R"({"input_artifact_id":"fixture:impulse-midi","max_frames":8,"timeout_ms":1000})"};
        value.request_hash = *control_request_hash(value);
        return value;
    }
};

ControlOfflineRenderSource fixture_source() {
    ControlOfflineRenderSource source;
    source.config = {.sample_rate = 48000.0,
                     .max_block_frames = 4,
                     .input_channels = 0,
                     .output_channels = 1};
    source.frame_count = 8;
    source.block_frames = 4;
    source.midi_events = {
        {.frame = 5, .event = pulp::midi::MidiEvent::note_on(0, 60, 1.0f)}};
    return source;
}

} // namespace

TEST_CASE("T0 offline render uses canonical service and broker-owned artifacts",
          "[inspect][control][offline][t0][artifact]") {
    const auto capabilities = inspector_capability_registry();
    const auto descriptor = std::find_if(capabilities.begin(), capabilities.end(), [](const auto& value) {
        return value.capability == InspectorCapability::RenderOffline;
    });
    REQUIRE(descriptor != capabilities.end());
    CHECK(descriptor->executor == InspectorExecutor::OfflineJob);
    CHECK(descriptor->required_build_feature == "inspect-offline-runtime");
    CHECK(descriptor->runtime_contexts == "offline");

    observed_midi.clear();
    Fixture fixture;
    ControlService service{
        fixture.broker,
        make_control_offline_render_executor(
            create_event_probe, [&fixture](const ControlAdmissionPlan& plan, std::string_view id)
                                    -> std::optional<ControlOfflineRenderSource> {
                REQUIRE(plan.client_id == fixture.identity.client_id);
                REQUIRE(plan.registration_id == fixture.registration.registration_id);
                REQUIRE(plan.grant_id == fixture.grant.grant_id);
                if (id != "fixture:impulse-midi")
                    return std::nullopt;
                return fixture_source();
            })};
    auto session = negotiate(service, fixture.client, fixture.identity.client_id);

    const auto first = session.dispatch(encode_control_envelope(
        {.payload = fixture.request("render-a", "render-key-a")}));
    REQUIRE(first.status == ControlServiceStatus::Responded);
    const auto first_receipt = receipt(first);
    REQUIRE(first_receipt.state == ControlReceiptState::Completed);
    REQUIRE(first_receipt.artifacts.size() == 1);
    CHECK(first_receipt.detail_json.find("\"plugin_id\": \"dev.pulp.test.t0-event-probe\"") !=
          std::string::npos);
    CHECK(first_receipt.detail_json.find("\"blocks\": 2") != std::string::npos);
    CHECK(first_receipt.detail_json.find("\"midi_events\": 1") != std::string::npos);
    CHECK(first_receipt.detail_json.find("\"parameter_count\": 0") != std::string::npos);
    CHECK(observed_midi == std::vector<std::pair<unsigned, int>>{{1, 1}});

    const auto first_read = session.read_artifact(first_receipt.artifacts[0].artifact_id, 0, 4096);
    REQUIRE(first_read.status == ControlArtifactStatus::Read);
    REQUIRE(first_read.metadata);
    CHECK(first_read.metadata->lineage.receipt_id == first_receipt.receipt_id);
    CHECK(first_read.metadata->lineage.producer_operation_id == "dev.pulp.render/offline@1");
    CHECK(first_read.metadata->lineage.session_id == "job-session-a");
    CHECK(first_read.metadata->content_type == "audio/wav");
    REQUIRE(first_read.bytes.size() > 12);
    CHECK(std::string(first_read.bytes.begin(), first_read.bytes.begin() + 4) == "RIFF");

    observed_midi.clear();
    const auto second = session.dispatch(encode_control_envelope(
        {.payload = fixture.request("render-b", "render-key-b")}));
    REQUIRE(second.status == ControlServiceStatus::Responded);
    const auto second_receipt = receipt(second);
    REQUIRE(second_receipt.state == ControlReceiptState::Completed);
    const auto second_read = session.read_artifact(second_receipt.artifacts[0].artifact_id, 0, 4096);
    REQUIRE(second_read.status == ControlArtifactStatus::Read);
    REQUIRE(second_read.metadata);
    CHECK(second_read.metadata->sha256 == first_read.metadata->sha256);
    CHECK(second_read.bytes == first_read.bytes);

    auto other_session = negotiate(service, fixture.other_client, fixture.other_identity.client_id);
    CHECK(other_session.read_artifact(first_receipt.artifacts[0].artifact_id, 0, 4096).status ==
          ControlArtifactStatus::Unauthorized);
    REQUIRE(fixture.broker.revoke_grant(fixture.grant.grant_id, "revoke-t0") ==
            ControlGrantStatus::Revoked);
    CHECK(session.read_artifact(first_receipt.artifacts[0].artifact_id, 0, 4096).status ==
          ControlArtifactStatus::Unauthorized);
}

TEST_CASE("T0 offline render remains default denied without resolver or artifact publisher",
          "[inspect][control][offline][t0][security]") {
    const auto executor = make_control_offline_render_executor(create_event_probe, {});
    const auto outcome = executor({}, {.operation_id = "dev.pulp.render/offline@1"}, {});
    CHECK(outcome.terminal_state == ControlReceiptState::Failed);
    CHECK(outcome.result.result_code == ControlResultCode::InvalidRequest);
}

TEST_CASE("T0 offline render rejects source formats that WAV cannot represent",
          "[inspect][control][offline][t0][validation]") {
    auto invalid_source = fixture_source();
    invalid_source.config.sample_rate = 48000.5;
    const auto executor = make_control_offline_render_executor(
        create_event_probe,
        [source = std::move(invalid_source)](const ControlAdmissionPlan&, std::string_view)
            -> std::optional<ControlOfflineRenderSource> { return source; });
    bool published = false;
    const auto outcome = executor(
        {},
        {.operation_id = "dev.pulp.render/offline@1",
         .operation_version = 1,
         .params_json = R"({"input_artifact_id":"fixture:invalid","max_frames":8,"timeout_ms":1000})"},
        {.checkpoint = [] { return ControlExecutionCheckpoint::Continue; },
         .maximum_artifact_bytes = 16u * 1024u * 1024u,
         .publish_artifact = [&published](std::span<const std::uint8_t>,
                                          ControlArtifactPublication) {
             published = true;
             return ControlArtifactStoreResult{.status = ControlArtifactStatus::Stored};
         }});
    CHECK(outcome.terminal_state == ControlReceiptState::Failed);
    CHECK(outcome.result.result_code == ControlResultCode::InvalidRequest);
    CHECK_FALSE(published);
}

TEST_CASE("T0 offline render enforces request timeout between process blocks",
          "[inspect][control][offline][t0][deadline]") {
    using Clock = std::chrono::steady_clock;
    const auto epoch = Clock::time_point{};
    unsigned clock_calls = 0;
    const auto executor = make_control_offline_render_executor(
        create_event_probe,
        [](const ControlAdmissionPlan&, std::string_view)
            -> std::optional<ControlOfflineRenderSource> {
            return fixture_source();
        },
        [&] { return clock_calls++ < 3 ? epoch : epoch + 2ms; });
    bool published = false;
    const auto outcome = executor(
        {},
        {.operation_id = "dev.pulp.render/offline@1",
         .operation_version = 1,
         .params_json = R"({"input_artifact_id":"fixture:timeout","max_frames":8,"timeout_ms":1})"},
        {.checkpoint = [] { return ControlExecutionCheckpoint::Continue; },
         .maximum_artifact_bytes = 16u * 1024u * 1024u,
         .publish_artifact = [&published](std::span<const std::uint8_t>,
                                          ControlArtifactPublication) {
             published = true;
             return ControlArtifactStoreResult{.status = ControlArtifactStatus::Stored};
         }});
    CHECK(outcome.terminal_state == ControlReceiptState::Failed);
    CHECK(outcome.result.result_code == ControlResultCode::DeadlineExceeded);
    CHECK_FALSE(published);
}

TEST_CASE("T0 offline render rejects output beyond broker artifact capacity before render",
          "[inspect][control][offline][t0][artifact][quota]") {
    const auto executor = make_control_offline_render_executor(
        create_event_probe,
        [](const ControlAdmissionPlan&, std::string_view)
            -> std::optional<ControlOfflineRenderSource> {
            return fixture_source();
        });
    bool published = false;
    const auto outcome = executor(
        {},
        {.operation_id = "dev.pulp.render/offline@1",
         .operation_version = 1,
         .params_json = R"({"input_artifact_id":"fixture:oversize","max_frames":8,"timeout_ms":1000})"},
        {.checkpoint = [] { return ControlExecutionCheckpoint::Continue; },
         .maximum_artifact_bytes = 4096,
         .publish_artifact = [&published](std::span<const std::uint8_t>,
                                          ControlArtifactPublication) {
             published = true;
             return ControlArtifactStoreResult{.status = ControlArtifactStatus::Stored};
         }});
    CHECK(outcome.terminal_state == ControlReceiptState::Failed);
    CHECK(outcome.result.result_code == ControlResultCode::ResourceExhausted);
    CHECK_FALSE(published);
}

TEST_CASE("T0 offline render preserves revocation raced with artifact publication",
          "[inspect][control][offline][t0][artifact][authority]") {
    unsigned checkpoints = 0;
    const auto executor = make_control_offline_render_executor(
        create_event_probe,
        [](const ControlAdmissionPlan&, std::string_view)
            -> std::optional<ControlOfflineRenderSource> {
            return fixture_source();
        });
    const auto outcome = executor(
        {},
        {.operation_id = "dev.pulp.render/offline@1",
         .operation_version = 1,
         .params_json = R"({"input_artifact_id":"fixture:revoked","max_frames":8,"timeout_ms":1000})"},
        {.checkpoint = [&] {
             return ++checkpoints <= 5 ? ControlExecutionCheckpoint::Continue
                                       : ControlExecutionCheckpoint::AuthorityRevoked;
         },
         .maximum_artifact_bytes = 16u * 1024u * 1024u,
         .publish_artifact = [](std::span<const std::uint8_t>,
                                ControlArtifactPublication) {
             return ControlArtifactStoreResult{.status = ControlArtifactStatus::Unauthorized};
         }});
    CHECK(outcome.terminal_state == ControlReceiptState::Cancelled);
    CHECK(outcome.result.result_code == ControlResultCode::Cancelled);
    CHECK(outcome.result.cancellation_reason == "authority-revoked");
}
