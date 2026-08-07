#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/client.hpp>
#include <pulp/inspect/control_client.hpp>
#include <pulp/inspect/control_service.hpp>
#include <pulp/runtime/crypto.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace pulp::inspect;
using namespace std::chrono_literals;

namespace {

namespace fs = std::filesystem;

struct TemporaryDirectory {
    fs::path path;

    TemporaryDirectory() {
        const auto random = pulp::runtime::secure_random_bytes(8);
        REQUIRE(random.has_value());
        path = fs::temp_directory_path() /
               ("pulp-control-service-" + pulp::runtime::hex_encode(*random));
        REQUIRE(fs::create_directory(path));
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

class RecordingControlTransport final : public ControlClientTransport {
  public:
    explicit RecordingControlTransport(std::string bound_client_id)
        : bound_client_id_(std::move(bound_client_id)) {}

    ControlTransportDispatchResult dispatch(std::string_view encoded_envelope,
                                            std::chrono::milliseconds timeout) override {
        last_dispatch = std::string(encoded_envelope);
        dispatch_timeout = timeout;
        const auto envelope = decode_control_envelope(encoded_envelope);
        if (!envelope || !std::holds_alternative<ControlNegotiationOffer>(envelope->payload)) {
            return {
                .error_code = "unexpected-message",
                .explanation = "test transport expected negotiation",
            };
        }
        negotiated_ = true;
        return {
            .encoded_response = encode_control_envelope({
                .schema_version = kControlProtocolVersion,
                .payload =
                    ControlNegotiationResult{
                        .status = ControlNegotiationStatus::Accepted,
                        .selected_version = kControlProtocolVersion,
                        .features = {"artifacts", "receipts"},
                    },
            }),
        };
    }

    ControlArtifactReadResult read_artifact(std::string_view artifact_id, std::uint64_t offset,
                                            std::size_t maximum_bytes,
                                            std::chrono::milliseconds timeout) override {
        last_artifact_id = std::string(artifact_id);
        last_offset = offset;
        last_maximum_bytes = maximum_bytes;
        read_timeout = timeout;
        if (!negotiated_) {
            return {
                .status = ControlArtifactStatus::Unauthorized,
                .explanation = "test transport negotiation required",
            };
        }
        return {
            .status = ControlArtifactStatus::Read,
            .metadata =
                ControlArtifactMetadata{
                    .artifact_id = std::string(artifact_id),
                    .lineage = {.producer_client_id = bound_client_id_},
                },
            .bytes = {4, 5, 6},
            .eof = true,
        };
    }

    std::string last_dispatch;
    std::string last_artifact_id;
    std::uint64_t last_offset = 0;
    std::size_t last_maximum_bytes = 0;
    std::chrono::milliseconds dispatch_timeout{};
    std::chrono::milliseconds read_timeout{};

  private:
    std::string bound_client_id_;
    bool negotiated_ = false;
};

VerifiedControlPeerIdentity verified_peer(ControlPeerRole role, std::int64_t process_id,
                                          std::string start_id) {
    ControlPeerVerifier verifier([](const ControlPeerEvidence&) { return true; });
    auto verified = verifier.verify({
        .role = role,
        .user_id = "uid:501",
        .process_id = process_id,
        .process_start_id = std::move(start_id),
        .executable_identity = "signed:dev.pulp.control-service-test",
        .publisher_id = "publisher.pulp",
    });
    REQUIRE(verified.has_value());
    return std::move(*verified);
}

ControlRegistrationRequest registration_request() {
    ControlManifest manifest;
    manifest.profile = ControlBuildProfile::DeveloperLocal;
    manifest.target = "ControlServiceFixture";
    manifest.product_name = "Control Service Fixture";
    manifest.bundle_id = "dev.pulp.control-service-fixture";
    manifest.build_id = "build:0123456789abcdef0123456789abcdef";
    manifest.endpoint_included = true;
    manifest.capabilities = {InspectorCapability::SessionDescribe,
                             InspectorCapability::SessionControl, InspectorCapability::StateRead,
                             InspectorCapability::CaptureImage, InspectorCapability::StateWrite};
    return {
        .host_tier = ControlHostTier::Standalone,
        .session_id = "session-a",
        .instance_id = "instance-a",
        .publication_id = "publication-a",
        .manifest = std::move(manifest),
        .artifact_digest = std::string(64, 'a'),
    };
}

ControlAdmissionPolicy allow_all_admission() {
    ControlAdmissionPolicy policy;
    policy.host_available = [](const auto&, const auto&) { return true; };
    policy.activated = [](const auto&, const auto&) { return true; };
    policy.policy_eligible = [](const auto&, const auto&) { return true; };
    return policy;
}

struct ServiceFixture {
    TemporaryDirectory temporary;
    VerifiedControlPeerIdentity client =
        verified_peer(ControlPeerRole::Client, 101, "client-start");
    VerifiedControlPeerIdentity host =
        verified_peer(ControlPeerRole::StandaloneHost, 201, "host-start");
    ControlBroker broker;
    ControlClientIdentity client_identity;
    ControlRegistration registration;
    ControlGrant grant;

    explicit ServiceFixture(
        std::size_t maximum_active_per_client = 64,
        ControlAdmissionPolicy admission = allow_all_admission(),
        ControlOperationStore::WallClock wall_clock =
            [] { return std::chrono::system_clock::now(); },
        std::chrono::milliseconds replay_window = std::chrono::hours{24},
        std::chrono::milliseconds retention = std::chrono::hours{24 * 7})
        : broker([&, maximum_active_per_client, admission = std::move(admission),
                  wall_clock = std::move(wall_clock), replay_window, retention]() mutable {
              ControlBrokerConfig config;
              config.operation_store = ControlOperationStoreConfig{
                  .directory = temporary.path / "receipts",
                  .max_active_receipts_per_client = maximum_active_per_client,
                  .replay_window = replay_window,
                  .retention = retention,
              };
              config.artifact_store = ControlArtifactStoreConfig{
                  .root = temporary.path / "artifacts",
              };
              config.admission = std::move(admission);
              config.wall_clock = wall_clock;
              return config;
          }()) {
        auto ticket = broker.issue_bootstrap(client);
        REQUIRE(ticket.ticket.has_value());
        auto connected = broker.redeem_bootstrap(ticket.ticket->ticket_id,
                                                 ticket.ticket->secret.bytes(), client);
        REQUIRE(connected.client.has_value());
        client_identity = *connected.client;

        auto registered = broker.register_instance(host, registration_request());
        REQUIRE(registered.registration.has_value());
        registration = *registered.registration;

        auto granted = broker.issue_grant(
            client,
            {
                .client_id = client_identity.client_id,
                .registration_id = registration.registration_id,
                .capabilities = {InspectorCapability::StateRead, InspectorCapability::StateWrite,
                                 InspectorCapability::CaptureImage},
                .ttl = 5min,
            },
            {
                .approved = true,
                .authority = ControlConsentAuthority::TrustedPulpCli,
                .decision_id = "decision-a",
            });
        REQUIRE(granted.grant.has_value());
        grant = *granted.grant;
    }

    ControlRequestEnvelope request(std::string params = "{}", std::string request_id = "request-a",
                                   std::string idempotency_key = "idempotency-a") const {
        ControlRequestEnvelope request{
            .request_id = std::move(request_id),
            .client_id = client_identity.client_id.value,
            .registration_id = registration.registration_id.value,
            .grant_id = grant.grant_id.value,
            .instance_generation = registration.publication_id,
            .operation_id = "dev.pulp.state/read@1",
            .operation_version = 1,
            .idempotency_key = std::move(idempotency_key),
            .deadline_unix_ms = 4'102'444'800'000,
            .expected_state_generation = 0,
            .params_json = std::move(params),
        };
        request.request_hash = *control_request_hash(request);
        return request;
    }

    ControlRequestEnvelope capture() const {
        auto capture = request(R"({"target":"window","format":"png"})", "request-capture",
                               "idempotency-capture");
        capture.operation_id = "dev.pulp.ui/capture@1";
        capture.request_hash = *control_request_hash(capture);
        return capture;
    }

    ControlRequestEnvelope mutation_request() const {
        auto mutation = request(
            R"({"parameter_id":1,"normalized_value":0.5,"idempotency_key":"idempotency-a"})");
        mutation.operation_id = "dev.pulp.state/parameter-gesture@1";
        mutation.request_hash = *control_request_hash(mutation);
        return mutation;
    }
};

const ControlReceiptEnvelope& receipt(const ControlServiceResult& result) {
    REQUIRE(result.response.has_value());
    const auto* value = std::get_if<ControlReceiptEnvelope>(&result.response->payload);
    REQUIRE(value != nullptr);
    return *value;
}

ControlExecutionOutcome successful_state_read() {
    return {
        .terminal_state = ControlReceiptState::Completed,
        .result =
            {
                .detail_json = R"({"generation":0,"parameters":[]})",
            },
    };
}

ControlExecutionOutcome successful_state_write(std::string_view receipt_id) {
    return {
        .terminal_state = ControlReceiptState::Completed,
        .result =
            {
                .detail_json = "{\"applied\":true,\"receipt_id\":\"" + std::string(receipt_id) +
                               "\",\"state_generation\":1}",
            },
    };
}

class ArbitraryWhatException final : public std::exception {
  public:
    explicit ArbitraryWhatException(std::string message) : message_(std::move(message)) {}
    const char* what() const noexcept override {
        return message_.c_str();
    }

  private:
    std::string message_;
};

ControlService::Session negotiate(ControlService& service, const ServiceFixture& fixture,
                                  bool progress = false, bool cancellation = true,
                                  bool artifacts = false) {
    auto session = service.open_session(fixture.client, fixture.client_identity.client_id);
    std::vector<std::string> optional_features;
    if (cancellation)
        optional_features.emplace_back("cancellation");
    if (progress)
        optional_features.emplace_back("progress");
    if (artifacts)
        optional_features.emplace_back("artifacts");
    ControlNegotiationOffer offer{
        .versions = {1, 1},
        .mandatory_features = {"receipts"},
        .optional_features = std::move(optional_features),
    };
    const auto result = session.dispatch(encode_control_envelope({
        .schema_version = kControlProtocolVersion,
        .payload = std::move(offer),
    }));
    REQUIRE(result.status == ControlServiceStatus::Responded);
    REQUIRE(result.response);
    const auto* negotiated = std::get_if<ControlNegotiationResult>(&result.response->payload);
    REQUIRE(negotiated);
    REQUIRE(negotiated->status == ControlNegotiationStatus::Accepted);
    return session;
}

} // namespace

TEST_CASE("control service is dormant and negotiates without a listener",
          "[inspect][control][service][security]") {
    ServiceFixture fixture;
    ControlService service{fixture.broker};
    CHECK_FALSE(service.is_listening());
    CHECK_FALSE(fixture.broker.is_listening());

    const ControlEnvelope offer{
        .schema_version = kControlProtocolVersion,
        .payload =
            ControlNegotiationOffer{
                .versions = {1, 1},
                .mandatory_features = {"receipts"},
            },
    };
    auto session = service.open_session(fixture.client, fixture.client_identity.client_id);
    const auto result = session.dispatch(encode_control_envelope(offer));
    REQUIRE(result.status == ControlServiceStatus::Responded);
    REQUIRE(result.response.has_value());
    const auto* negotiated = std::get_if<ControlNegotiationResult>(&result.response->payload);
    REQUIRE(negotiated != nullptr);
    CHECK(negotiated->status == ControlNegotiationStatus::Accepted);
    CHECK(negotiated->selected_version == 1);

    auto fresh_session = service.open_session(fixture.client, fixture.client_identity.client_id);
    const auto fresh_request = fresh_session.dispatch(encode_control_envelope({
        .schema_version = kControlProtocolVersion,
        .payload = fixture.request(),
    }));
    CHECK(fresh_request.status == ControlServiceStatus::NegotiationRequired);

    for (std::size_t index = 0; index < 128; ++index) {
        auto ephemeral = service.open_session(fixture.client, fixture.client_identity.client_id);
        const auto accepted = ephemeral.dispatch(encode_control_envelope(offer));
        INFO(index);
        CHECK(accepted.status == ControlServiceStatus::Responded);
    }

    const auto dormant = session.dispatch(encode_control_envelope({
        .schema_version = kControlProtocolVersion,
        .payload = fixture.request(),
    }));
    CHECK(dormant.status == ControlServiceStatus::UnsupportedMessage);

    std::size_t executions = 0;
    ControlService activated{
        fixture.broker,
        [&](const ControlAdmissionPlan&, const ControlRequestEnvelope&,
            const ControlExecutionContext&) {
            ++executions;
            return successful_state_read();
        },
    };
    auto activated_session = negotiate(activated, fixture);
    const auto after_activation = activated_session.dispatch(encode_control_envelope({
        .schema_version = kControlProtocolVersion,
        .payload = fixture.request(),
    }));
    CHECK(after_activation.status == ControlServiceStatus::Responded);
    CHECK(executions == 1);
}

TEST_CASE("control service gates artifact operations and reads on negotiated support",
          "[inspect][control][service][artifact][negotiation][security]") {
    ServiceFixture fixture;
    std::size_t executions = 0;
    const std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};
    ControlService service{
        fixture.broker,
        [&](const ControlAdmissionPlan& plan, const ControlRequestEnvelope&,
            const ControlExecutionContext&) {
            ++executions;
            const auto stored =
                fixture.broker.store_operation_artifact(fixture.client, plan, bytes,
                                                        {
                                                            .content_type = "image/png",
                                                            .created_at_unix_ms = 1,
                                                            .expires_at_unix_ms = 4'102'444'800'000,
                                                        });
            REQUIRE(stored.status == ControlArtifactStatus::Stored);
            REQUIRE(stored.metadata);
            const auto& metadata = *stored.metadata;
            return ControlExecutionOutcome{
                .terminal_state = ControlReceiptState::Completed,
                .result =
                    {
                        .detail_json = "{\"artifact_id\":\"" + metadata.artifact_id +
                                       "\",\"byte_count\":" + std::to_string(metadata.byte_size) +
                                       ",\"mime_type\":\"image/png\",\"sha256\":\"" +
                                       metadata.sha256 + "\"}",
                        .artifacts = {{metadata.artifact_id, metadata.content_type,
                                       metadata.byte_size}},
                    },
            };
        },
    };
    auto unnegotiated = service.open_session(fixture.client, fixture.client_identity.client_id);
    auto receipts_only = negotiate(service, fixture, false, true, false);
    const auto encoded_capture = encode_control_envelope({
        .schema_version = kControlProtocolVersion,
        .payload = fixture.capture(),
    });

    const auto denied_request = receipts_only.dispatch(encoded_capture);
    CHECK(denied_request.status == ControlServiceStatus::UnsupportedMessage);
    CHECK(denied_request.explanation == "artifacts feature was not negotiated");
    CHECK(executions == 0);

    auto artifacts_session = negotiate(service, fixture, false, true, true);
    const auto completed = artifacts_session.dispatch(encoded_capture);
    REQUIRE(completed.status == ControlServiceStatus::Responded);
    REQUIRE(receipt(completed).state == ControlReceiptState::Completed);
    REQUIRE(receipt(completed).artifacts.size() == 1);
    CHECK(executions == 1);
    const auto& artifact_id = receipt(completed).artifacts.front().artifact_id;

    const auto before_negotiation = unnegotiated.read_artifact(artifact_id, 0, bytes.size());
    CHECK(before_negotiation.status == ControlArtifactStatus::Unauthorized);
    CHECK(before_negotiation.explanation == "control protocol negotiation is required");
    const auto without_artifacts = receipts_only.read_artifact(artifact_id, 0, bytes.size());
    CHECK(without_artifacts.status == ControlArtifactStatus::Unauthorized);
    CHECK(without_artifacts.explanation == "artifacts feature was not negotiated");

    const auto read = artifacts_session.read_artifact(artifact_id, 0, bytes.size());
    REQUIRE(read.status == ControlArtifactStatus::Read);
    CHECK(read.bytes == std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
    CHECK(read.eof);
}

TEST_CASE("control service executes admitted request once and replays receipt",
          "[inspect][control][service][idempotency]") {
    ServiceFixture fixture;
    std::size_t executions = 0;
    ControlService service{
        fixture.broker,
        [&](const ControlAdmissionPlan&, const ControlRequestEnvelope&,
            const ControlExecutionContext&) {
            ++executions;
            return ControlExecutionOutcome{
                .terminal_state = ControlReceiptState::Completed,
                .result =
                    {
                        .detail_json = R"({"generation":7,"parameters":[]})",
                    },
            };
        },
    };
    auto session = negotiate(service, fixture);
    const ControlEnvelope envelope{
        .schema_version = kControlProtocolVersion,
        .payload = fixture.request(),
    };
    const auto encoded = encode_control_envelope(envelope);
    REQUIRE_FALSE(encoded.empty());

    const auto first = session.dispatch(encoded);
    INFO(first.explanation);
    REQUIRE(first.status == ControlServiceStatus::Responded);
    CHECK(receipt(first).state == ControlReceiptState::Completed);
    CHECK(executions == 1);

    auto reconnect_request = fixture.request("{}", "request-reconnect", "idempotency-a");
    reconnect_request.deadline_unix_ms += 60'000;
    reconnect_request.request_hash = *control_request_hash(reconnect_request);
    auto reconnected_session = negotiate(service, fixture);
    const auto replay = reconnected_session.dispatch(encode_control_envelope({
        .schema_version = kControlProtocolVersion,
        .payload = reconnect_request,
    }));
    REQUIRE(replay.status == ControlServiceStatus::Responded);
    CHECK(receipt(replay).receipt_id == receipt(first).receipt_id);
    CHECK(receipt(replay).request_id == "request-reconnect");
    CHECK(receipt(replay).state == ControlReceiptState::Completed);
    CHECK(executions == 1);

    const auto durable =
        fixture.broker.operation_receipt(ControlReceiptId{receipt(first).receipt_id});
    REQUIRE(durable);
    CHECK(durable->binding.request_id == "request-a");
    CHECK(durable->binding.deadline_unix_ms == 4'102'444'800'000);
}

TEST_CASE("control service requires connection-bound negotiation",
          "[inspect][control][service][negotiation][security]") {
    ServiceFixture fixture;
    std::size_t executions = 0;
    ControlService service{
        fixture.broker,
        [&](const ControlAdmissionPlan&, const ControlRequestEnvelope&,
            const ControlExecutionContext&) {
            ++executions;
            return successful_state_read();
        },
    };
    auto session = service.open_session(fixture.client, fixture.client_identity.client_id);
    const auto denied = session.dispatch(encode_control_envelope({
        .schema_version = kControlProtocolVersion,
        .payload = fixture.request(),
    }));
    CHECK(denied.status == ControlServiceStatus::NegotiationRequired);
    CHECK(executions == 0);
    CHECK(fixture.broker.operation_store_ready());
}

TEST_CASE("control service emits bounded monotonic progress with backpressure",
          "[inspect][control][service][progress][backpressure]") {
    SECTION("negotiated progress reaches the carrier sink") {
        ServiceFixture fixture;
        std::vector<ControlProgressEnvelope> observed;
        ControlService service{
            fixture.broker,
            [&](const ControlAdmissionPlan&, const ControlRequestEnvelope&,
                const ControlExecutionContext& context) {
                CHECK(context.report_progress(1, 2, R"({"stage":"read"})"));
                CHECK(context.report_progress(2, 2, R"({"stage":"done"})"));
                return successful_state_read();
            },
            [&](const ControlProgressEnvelope& progress) {
                observed.push_back(progress);
                return true;
            },
        };
        auto session = negotiate(service, fixture, true);
        const auto completed = session.dispatch(encode_control_envelope({
            .schema_version = kControlProtocolVersion,
            .payload = fixture.request(),
        }));
        REQUIRE(completed.status == ControlServiceStatus::Responded);
        CHECK(receipt(completed).state == ControlReceiptState::Completed);
        REQUIRE(observed.size() == 2);
        CHECK(valid_control_progress_transition(observed[0], observed[1]));
    }

    SECTION("progress quota exhaustion stops delivery and preserves a synchronous result") {
        ServiceFixture fixture;
        std::size_t executions = 0;
        std::size_t progress_deliveries = 0;
        ControlService service{
            fixture.broker,
            [&](const ControlAdmissionPlan& plan, const ControlRequestEnvelope&,
                const ControlExecutionContext& context) {
                ++executions;
                CHECK(context.report_progress(1, 2, "{}"));
                CHECK_FALSE(context.report_progress(2, 2, "{}"));
                CHECK_FALSE(context.report_progress(2, 2, "{}"));
                return successful_state_write(plan.receipt_id.value);
            },
            [&](const ControlProgressEnvelope&) {
                ++progress_deliveries;
                return true;
            },
            ControlServiceConfig{
                .maximum_progress_events_per_operation = 1,
            },
        };
        auto session = negotiate(service, fixture, true);
        const auto encoded = encode_control_envelope({
            .schema_version = kControlProtocolVersion,
            .payload = fixture.mutation_request(),
        });
        const auto completed = session.dispatch(encoded);
        REQUIRE(completed.status == ControlServiceStatus::Responded);
        CHECK(receipt(completed).state == ControlReceiptState::Completed);
        CHECK(executions == 1);
        CHECK(progress_deliveries == 1);

        const auto replayed = session.dispatch(encoded);
        REQUIRE(replayed.status == ControlServiceStatus::Responded);
        CHECK(receipt(replayed).state == ControlReceiptState::Completed);
        CHECK(receipt(replayed).receipt_id == receipt(completed).receipt_id);
        CHECK(executions == 1);
    }

    SECTION("progress backpressure preserves a deferred terminal result") {
        ServiceFixture fixture;
        ControlDeferredCompletion complete_deferred;
        std::size_t executions = 0;
        std::size_t progress_deliveries = 0;
        ControlService service{
            fixture.broker,
            [&](const ControlAdmissionPlan&, const ControlRequestEnvelope&,
                const ControlExecutionContext& context) {
                ++executions;
                complete_deferred = context.complete_deferred;
                CHECK(context.report_progress(1, 2, "{}"));
                CHECK_FALSE(context.report_progress(2, 2, "{}"));
                CHECK_FALSE(context.report_progress(2, 2, "{}"));
                return ControlExecutionOutcome{
                    .terminal_state = ControlReceiptState::UnknownNeedsRefresh,
                    .result =
                        {
                            .result_code = ControlResultCode::UnknownNeedsRefresh,
                            .retry = ControlRetryClassification::AfterRefresh,
                        },
                    .deferred = true,
                };
            },
            [&](const ControlProgressEnvelope&) {
                ++progress_deliveries;
                return true;
            },
            ControlServiceConfig{
                .maximum_progress_events_per_operation = 1,
            },
        };
        auto session = negotiate(service, fixture, true);
        const auto encoded = encode_control_envelope({
            .schema_version = kControlProtocolVersion,
            .payload = fixture.mutation_request(),
        });
        const auto pending = session.dispatch(encoded);
        REQUIRE(pending.status == ControlServiceStatus::Responded);
        CHECK(receipt(pending).state == ControlReceiptState::UnknownNeedsRefresh);
        REQUIRE(complete_deferred);
        CHECK(progress_deliveries == 1);

        complete_deferred(successful_state_write(receipt(pending).receipt_id));
        const auto durable =
            fixture.broker.operation_receipt(ControlReceiptId{receipt(pending).receipt_id});
        REQUIRE(durable.has_value());
        CHECK(durable->state == ControlReceiptState::Completed);

        const auto replayed = session.dispatch(encoded);
        REQUIRE(replayed.status == ControlServiceStatus::Responded);
        CHECK(receipt(replayed).state == ControlReceiptState::Completed);
        CHECK(receipt(replayed).receipt_id == receipt(pending).receipt_id);
        CHECK(executions == 1);
    }
}

TEST_CASE("control service returns deterministic active-operation backpressure",
          "[inspect][control][service][quota][backpressure]") {
    ServiceFixture fixture{1};
    std::mutex mutex;
    std::condition_variable condition;
    bool executing = false;
    bool release = false;
    ControlService service{
        fixture.broker,
        [&](const ControlAdmissionPlan&, const ControlRequestEnvelope&,
            const ControlExecutionContext&) {
            std::unique_lock lock(mutex);
            executing = true;
            condition.notify_all();
            condition.wait_for(lock, 10s, [&] { return release; });
            return successful_state_read();
        },
    };
    auto session = negotiate(service, fixture);
    ControlServiceResult first;
    std::thread worker([&] {
        first = session.dispatch(encode_control_envelope({
            .schema_version = kControlProtocolVersion,
            .payload = fixture.request(),
        }));
    });
    bool started = false;
    {
        std::unique_lock lock(mutex);
        started = condition.wait_for(lock, 2s, [&] { return executing; });
    }
    CHECK(started);
    if (!started) {
        {
            std::lock_guard lock(mutex);
            release = true;
        }
        condition.notify_all();
        worker.join();
        return;
    }

    const auto saturated = session.dispatch(encode_control_envelope({
        .schema_version = kControlProtocolVersion,
        .payload = fixture.request("{}", "request-b", "idempotency-b"),
    }));
    CHECK(saturated.status == ControlServiceStatus::AdmissionDenied);
    CHECK(saturated.admission_status == ControlAdmissionStatus::ResourceExhausted);
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    condition.notify_all();
    worker.join();
    CHECK(first.status == ControlServiceStatus::Responded);
}

TEST_CASE("control service retains quota until a started timeout settles",
          "[inspect][control][service][quota][timeout][deferred]") {
    ServiceFixture fixture{1};
    ControlDeferredCompletion complete_deferred;
    ControlProgressReporter report_progress;
    std::atomic<unsigned> progress_deliveries{0};
    ControlService service{
        fixture.broker,
        [&](const ControlAdmissionPlan&, const ControlRequestEnvelope&,
            const ControlExecutionContext& context) {
            complete_deferred = context.complete_deferred;
            report_progress = context.report_progress;
            return ControlExecutionOutcome{
                .terminal_state = ControlReceiptState::UnknownNeedsRefresh,
                .result =
                    {
                        .result_code = ControlResultCode::UnknownNeedsRefresh,
                        .retry = ControlRetryClassification::AfterRefresh,
                    },
                .deferred = true,
            };
        },
        [&](const ControlProgressEnvelope&) {
            ++progress_deliveries;
            return true;
        },
    };
    auto session = negotiate(service, fixture, true);
    auto dispatch = [&](ControlRequestEnvelope request) {
        return session.dispatch(encode_control_envelope({
            .schema_version = kControlProtocolVersion,
            .payload = std::move(request),
        }));
    };

    const auto uncertain = dispatch(fixture.request());
    REQUIRE(uncertain.status == ControlServiceStatus::Responded);
    CHECK(receipt(uncertain).state == ControlReceiptState::UnknownNeedsRefresh);
    REQUIRE(complete_deferred);
    REQUIRE(report_progress);
    CHECK_FALSE(report_progress(1, 1, R"({"late":true})"));
    CHECK(progress_deliveries.load() == 0);

    const auto saturated =
        dispatch(fixture.request("{}", "request-deferred-b", "idempotency-deferred-b"));
    CHECK(saturated.status == ControlServiceStatus::AdmissionDenied);
    CHECK(saturated.admission_status == ControlAdmissionStatus::ResourceExhausted);

    auto first_completion = complete_deferred;
    auto duplicate_completion = complete_deferred;
    std::thread first_settler([first_completion] { first_completion(successful_state_read()); });
    std::thread duplicate_settler(
        [duplicate_completion] { duplicate_completion(successful_state_read()); });
    first_settler.join();
    duplicate_settler.join();
    const auto settled =
        fixture.broker.operation_receipt(ControlReceiptId{receipt(uncertain).receipt_id});
    REQUIRE(settled.has_value());
    CHECK(settled->state == ControlReceiptState::Completed);
    complete_deferred(successful_state_read());
    const auto still_settled =
        fixture.broker.operation_receipt(ControlReceiptId{receipt(uncertain).receipt_id});
    REQUIRE(still_settled.has_value());
    CHECK(still_settled->state == ControlReceiptState::Completed);

    const auto admitted =
        dispatch(fixture.request("{}", "request-deferred-c", "idempotency-deferred-c"));
    CHECK(admitted.status == ControlServiceStatus::Responded);
}

TEST_CASE("control service preserves a deferred settlement when the executor then throws",
          "[inspect][control][service][deferred][exception]") {
    ServiceFixture fixture;
    ControlService service{
        fixture.broker,
        [](const ControlAdmissionPlan&, const ControlRequestEnvelope&,
           const ControlExecutionContext& context) -> ControlExecutionOutcome {
            context.complete_deferred(successful_state_read());
            throw std::runtime_error("executor failed after completion");
        },
    };
    auto session = negotiate(service, fixture);
    const auto completed = session.dispatch(encode_control_envelope({
        .schema_version = kControlProtocolVersion,
        .payload = fixture.request(),
    }));

    REQUIRE(completed.status == ControlServiceStatus::Responded);
    CHECK(receipt(completed).state == ControlReceiptState::Completed);
}

TEST_CASE("control service sanitizes executor exceptions before durable settlement",
          "[inspect][control][service][exception][persistence][quota][idempotency]") {
    struct Case {
        std::string label;
        std::string message;
        std::string expected_explanation;
    };
    const std::vector<Case> cases{
        {"oversized", std::string(kControlReceiptMaximumExplanationBytes + 1, 'x'),
         "executor threw an invalid exception message"},
        {"embedded NUL", std::string{"visible\0hidden", 14}, "visible"},
        {"invalid UTF-8", std::string{static_cast<char>(0xff)},
         "executor threw an invalid exception message"},
    };

    for (const auto& test : cases) {
        DYNAMIC_SECTION(test.label) {
            ServiceFixture fixture{1};
            std::size_t executions = 0;
            ControlService service{
                fixture.broker,
                [&](const ControlAdmissionPlan&, const ControlRequestEnvelope&,
                    const ControlExecutionContext&) -> ControlExecutionOutcome {
                    ++executions;
                    throw ArbitraryWhatException(test.message);
                },
            };
            auto session = negotiate(service, fixture);
            const auto encoded = encode_control_envelope({
                .schema_version = kControlProtocolVersion,
                .payload = fixture.request(),
            });

            const auto failed = session.dispatch(encoded);
            REQUIRE(failed.status == ControlServiceStatus::Responded);
            CHECK(receipt(failed).state == ControlReceiptState::Failed);
            CHECK(receipt(failed).result_code == ControlResultCode::InternalError);
            CHECK(receipt(failed).explanation == test.expected_explanation);
            CHECK(executions == 1);

            const auto replayed = session.dispatch(encoded);
            REQUIRE(replayed.status == ControlServiceStatus::Responded);
            CHECK(receipt(replayed).state == ControlReceiptState::Failed);
            CHECK(receipt(replayed).receipt_id == receipt(failed).receipt_id);
            CHECK(executions == 1);

            const auto next = session.dispatch(encode_control_envelope({
                .schema_version = kControlProtocolVersion,
                .payload = fixture.request("{}", "request-b", "idempotency-b"),
            }));
            REQUIRE(next.status == ControlServiceStatus::Responded);
            CHECK(receipt(next).state == ControlReceiptState::Failed);
            CHECK(executions == 2);
        }
    }
}

TEST_CASE("control service binds typed result receipt ids to durable receipts",
          "[inspect][control][service][receipt][persistence][idempotency]") {
    SECTION("matching receipt id completes") {
        ServiceFixture fixture;
        ControlService service{
            fixture.broker,
            [](const ControlAdmissionPlan& plan, const ControlRequestEnvelope&,
               const ControlExecutionContext&) {
                return successful_state_write(plan.receipt_id.value);
            },
        };
        auto session = negotiate(service, fixture);
        const auto completed = session.dispatch(encode_control_envelope({
            .schema_version = kControlProtocolVersion,
            .payload = fixture.mutation_request(),
        }));

        REQUIRE(completed.status == ControlServiceStatus::Responded);
        CHECK(receipt(completed).state == ControlReceiptState::Completed);
    }

    SECTION("mismatched receipt id becomes a durable internal failure") {
        ServiceFixture fixture;
        std::size_t executions = 0;
        ControlService service{
            fixture.broker,
            [&](const ControlAdmissionPlan&, const ControlRequestEnvelope&,
                const ControlExecutionContext&) {
                ++executions;
                return successful_state_write("receipt-00000000000000000000000000000000");
            },
        };
        auto session = negotiate(service, fixture);
        const auto encoded = encode_control_envelope({
            .schema_version = kControlProtocolVersion,
            .payload = fixture.mutation_request(),
        });
        const auto failed = session.dispatch(encoded);

        REQUIRE(failed.status == ControlServiceStatus::Responded);
        CHECK(receipt(failed).state == ControlReceiptState::Failed);
        CHECK(receipt(failed).result_code == ControlResultCode::InternalError);
        CHECK(receipt(failed).explanation == "executor returned an invalid terminal result");
        CHECK(executions == 1);
        const auto durable =
            fixture.broker.operation_receipt(ControlReceiptId{receipt(failed).receipt_id});
        REQUIRE(durable.has_value());
        CHECK(durable->state == ControlReceiptState::Failed);

        const auto replayed = session.dispatch(encoded);
        REQUIRE(replayed.status == ControlServiceStatus::Responded);
        CHECK(receipt(replayed).state == ControlReceiptState::Failed);
        CHECK(receipt(replayed).receipt_id == receipt(failed).receipt_id);
        CHECK(executions == 1);
    }
}

TEST_CASE("control service replaces invalid executor outcomes with durable internal failure",
          "[inspect][control][service][outcome][persistence][quota][idempotency]") {
    struct Case {
        std::string label;
        std::function<ControlExecutionOutcome()> outcome;
    };
    const std::vector<Case> cases{
        {
            "completed with oversized explanation",
            [] {
                auto outcome = successful_state_read();
                outcome.result.explanation.assign(kControlReceiptMaximumExplanationBytes + 1, 'x');
                return outcome;
            },
        },
        {
            "failed with embedded NUL explanation",
            [] {
                return ControlExecutionOutcome{
                    .terminal_state = ControlReceiptState::Failed,
                    .result =
                        {
                            .result_code = ControlResultCode::InternalError,
                            .explanation = std::string{"visible\0hidden", 14},
                        },
                };
            },
        },
        {
            "completed with invalid UTF-8 evidence",
            [] {
                auto outcome = successful_state_read();
                outcome.result.evidence_ids = {std::string{static_cast<char>(0xff)}};
                return outcome;
            },
        },
        {
            "failed with oversized evidence",
            [] {
                return ControlExecutionOutcome{
                    .terminal_state = ControlReceiptState::Failed,
                    .result =
                        {
                            .result_code = ControlResultCode::InternalError,
                            .evidence_ids = {std::string(kControlReceiptMaximumEvidenceIdBytes + 1,
                                                         'e')},
                        },
                };
            },
        },
    };

    for (const auto& test : cases) {
        DYNAMIC_SECTION(test.label) {
            ServiceFixture fixture{1};
            std::size_t executions = 0;
            ControlService service{
                fixture.broker,
                [&](const ControlAdmissionPlan&, const ControlRequestEnvelope&,
                    const ControlExecutionContext&) {
                    ++executions;
                    return test.outcome();
                },
            };
            auto session = negotiate(service, fixture);
            const auto encoded = encode_control_envelope({
                .schema_version = kControlProtocolVersion,
                .payload = fixture.request(),
            });

            const auto failed = session.dispatch(encoded);
            REQUIRE(failed.status == ControlServiceStatus::Responded);
            CHECK(receipt(failed).state == ControlReceiptState::Failed);
            CHECK(receipt(failed).result_code == ControlResultCode::InternalError);
            CHECK(receipt(failed).explanation == "executor returned invalid terminal metadata");
            CHECK(executions == 1);

            const auto replayed = session.dispatch(encoded);
            REQUIRE(replayed.status == ControlServiceStatus::Responded);
            CHECK(receipt(replayed).state == ControlReceiptState::Failed);
            CHECK(receipt(replayed).receipt_id == receipt(failed).receipt_id);
            CHECK(executions == 1);

            const auto next = session.dispatch(encode_control_envelope({
                .schema_version = kControlProtocolVersion,
                .payload = fixture.request("{}", "request-b", "idempotency-b"),
            }));
            REQUIRE(next.status == ControlServiceStatus::Responded);
            CHECK(receipt(next).state == ControlReceiptState::Failed);
            CHECK(executions == 2);
        }
    }
}

TEST_CASE("control service destruction terminalizes owned deferred operations",
          "[inspect][control][service][deferred][lifecycle]") {
    ServiceFixture fixture{1};
    ControlDeferredCompletion complete_deferred;
    auto service = std::make_unique<ControlService>(
        fixture.broker, [&](const ControlAdmissionPlan&, const ControlRequestEnvelope&,
                            const ControlExecutionContext& context) {
            complete_deferred = context.complete_deferred;
            return ControlExecutionOutcome{
                .terminal_state = ControlReceiptState::UnknownNeedsRefresh,
                .result =
                    {
                        .result_code = ControlResultCode::UnknownNeedsRefresh,
                        .retry = ControlRetryClassification::AfterRefresh,
                    },
                .deferred = true,
            };
        });
    auto session = negotiate(*service, fixture);
    const auto pending = session.dispatch(encode_control_envelope({
        .schema_version = kControlProtocolVersion,
        .payload = fixture.request(),
    }));
    REQUIRE(pending.status == ControlServiceStatus::Responded);
    const auto receipt_id = ControlReceiptId{receipt(pending).receipt_id};
    REQUIRE(complete_deferred);

    service.reset();

    const auto uncertain = fixture.broker.operation_receipt(receipt_id);
    REQUIRE(uncertain.has_value());
    CHECK(uncertain->state == ControlReceiptState::UnknownNeedsRefresh);
    CHECK(uncertain->result.result_code == ControlResultCode::UnknownNeedsRefresh);
    CHECK(uncertain->result.retry == ControlRetryClassification::AfterRefresh);

    complete_deferred(successful_state_read());
    const auto still_uncertain = fixture.broker.operation_receipt(receipt_id);
    REQUIRE(still_uncertain.has_value());
    CHECK(still_uncertain->state == ControlReceiptState::UnknownNeedsRefresh);

    ControlService replacement{
        fixture.broker,
        [](const ControlAdmissionPlan&, const ControlRequestEnvelope&,
           const ControlExecutionContext&) { return successful_state_read(); },
    };
    auto replacement_session = negotiate(replacement, fixture);
    const auto admitted = replacement_session.dispatch(encode_control_envelope({
        .schema_version = kControlProtocolVersion,
        .payload = fixture.request("{}", "request-after-shutdown", "idempotency-after-shutdown"),
    }));
    CHECK(admitted.status == ControlServiceStatus::Responded);
    CHECK(receipt(admitted).state == ControlReceiptState::Completed);
}

TEST_CASE("control service does not execute after authority changes at executor admission",
          "[inspect][control][service][admission][revocation][security]") {
    std::size_t activation_checks = 0;
    auto admission = allow_all_admission();
    admission.activated = [&](const auto&, const auto&) { return ++activation_checks < 3; };
    ServiceFixture fixture{64, std::move(admission)};
    std::size_t executions = 0;
    ControlService service{
        fixture.broker,
        [&](const ControlAdmissionPlan&, const ControlRequestEnvelope&,
            const ControlExecutionContext&) {
            ++executions;
            return successful_state_read();
        },
    };
    auto session = negotiate(service, fixture);

    const auto cancelled = session.dispatch(encode_control_envelope({
        .schema_version = kControlProtocolVersion,
        .payload = fixture.request(),
    }));

    REQUIRE(cancelled.status == ControlServiceStatus::Responded);
    CHECK(receipt(cancelled).state == ControlReceiptState::Cancelled);
    CHECK(executions == 0);
}

TEST_CASE("control service projects and replays a post-persist cancellation receipt",
          "[inspect][control][service][admission][receipt][race][idempotency]") {
    std::atomic<unsigned> activation_checks{0};
    auto admission = allow_all_admission();
    admission.activated = [&](const auto&, const auto&) {
        // Admit initially, lose authority only at the post-persist check, then
        // restore it so an identical retry reaches durable idempotent replay.
        return ++activation_checks != 2;
    };
    ServiceFixture fixture{64, std::move(admission)};
    std::atomic<unsigned> executions{0};
    ControlService service{
        fixture.broker,
        [&](const ControlAdmissionPlan&, const ControlRequestEnvelope&,
            const ControlExecutionContext&) {
            ++executions;
            return successful_state_read();
        },
    };
    auto session = negotiate(service, fixture);
    const auto request = fixture.request();
    const auto encoded = encode_control_envelope({
        .schema_version = kControlProtocolVersion,
        .payload = request,
    });

    const auto cancelled = session.dispatch(encoded);
    REQUIRE(cancelled.status == ControlServiceStatus::Responded);
    const auto& cancelled_receipt = receipt(cancelled);
    CHECK(cancelled_receipt.state == ControlReceiptState::Cancelled);
    CHECK(cancelled_receipt.result_code == ControlResultCode::Cancelled);
    CHECK(cancelled_receipt.request_id == request.request_id);
    REQUIRE_FALSE(cancelled_receipt.receipt_id.empty());
    CHECK(executions.load() == 0);

    const auto persisted =
        fixture.broker.operation_receipt(ControlReceiptId{cancelled_receipt.receipt_id});
    REQUIRE(persisted.has_value());
    CHECK(persisted->binding.client_id == fixture.client_identity.client_id);
    CHECK(persisted->binding.request_id == request.request_id);

    const auto replayed = session.dispatch(encoded);
    REQUIRE(replayed.status == ControlServiceStatus::Responded);
    CHECK(receipt(replayed).state == ControlReceiptState::Cancelled);
    CHECK(receipt(replayed).receipt_id == cancelled_receipt.receipt_id);
    CHECK(receipt(replayed).request_id == request.request_id);
    CHECK(executions.load() == 0);
}

TEST_CASE("control service returns a stable replay-window expiry without redispatch",
          "[inspect][control][service][receipt][idempotency][expiry]") {
    auto now = std::make_shared<std::chrono::system_clock::time_point>(
        std::chrono::system_clock::time_point{1000ms});
    ServiceFixture fixture{64, allow_all_admission(), [now] { return *now; }, 100ms, 1000ms};
    std::atomic<unsigned> executions{0};
    ControlService service{
        fixture.broker,
        [&](const ControlAdmissionPlan&, const ControlRequestEnvelope&,
            const ControlExecutionContext&) {
            ++executions;
            return successful_state_read();
        },
    };
    auto session = negotiate(service, fixture);
    const auto encoded = encode_control_envelope({
        .schema_version = kControlProtocolVersion,
        .payload = fixture.request(),
    });
    const auto completed = session.dispatch(encoded);
    REQUIRE(completed.status == ControlServiceStatus::Responded);
    REQUIRE(receipt(completed).state == ControlReceiptState::Completed);
    const auto receipt_id = receipt(completed).receipt_id;
    CHECK(executions.load() == 1);

    *now += 100ms;
    const auto expired = session.dispatch(encoded);
    CHECK(expired.status == ControlServiceStatus::AdmissionDenied);
    CHECK(expired.admission_status == ControlAdmissionStatus::ReplayWindowExpired);
    CHECK(expired.explanation == "replay-window-expired");
    CHECK_FALSE(expired.response.has_value());
    CHECK(executions.load() == 1);
    const auto retained = fixture.broker.operation_receipt(ControlReceiptId{receipt_id});
    REQUIRE(retained.has_value());
    CHECK(retained->state == ControlReceiptState::Completed);

    const auto repeated = session.dispatch(encoded);
    CHECK(repeated.status == ControlServiceStatus::AdmissionDenied);
    CHECK(repeated.admission_status == ControlAdmissionStatus::ReplayWindowExpired);
    CHECK(repeated.explanation == "replay-window-expired");
    CHECK(executions.load() == 1);
}

TEST_CASE("control service rejects idempotency content conflicts",
          "[inspect][control][service][idempotency][security]") {
    ServiceFixture fixture;
    std::size_t executions = 0;
    ControlService service{
        fixture.broker,
        [&](const ControlAdmissionPlan&, const ControlRequestEnvelope&,
            const ControlExecutionContext&) {
            ++executions;
            return successful_state_read();
        },
    };
    auto session = negotiate(service, fixture);
    auto dispatch = [&](ControlRequestEnvelope request) {
        return session.dispatch(encode_control_envelope({
            .schema_version = kControlProtocolVersion,
            .payload = std::move(request),
        }));
    };
    const auto admitted = dispatch(fixture.request(R"({"parameter_ids":[1]})"));
    INFO(admitted.explanation);
    CHECK(admitted.status == ControlServiceStatus::Responded);
    const auto conflict = dispatch(fixture.request(R"({"parameter_ids":[2]})"));
    CHECK(conflict.status == ControlServiceStatus::AdmissionDenied);
    REQUIRE(conflict.admission_status.has_value());
    CHECK(*conflict.admission_status == ControlAdmissionStatus::IdempotencyConflict);
    CHECK(executions == 1);
}

TEST_CASE("control service enforces operation versions and typed schemas",
          "[inspect][control][service][schema][security]") {
    ServiceFixture fixture;
    std::size_t executions = 0;
    ControlService service{
        fixture.broker,
        [&](const ControlAdmissionPlan&, const ControlRequestEnvelope&,
            const ControlExecutionContext&) {
            ++executions;
            return ControlExecutionOutcome{
                .terminal_state = ControlReceiptState::Completed,
                .result = {.detail_json = R"({"generation":7})"},
            };
        },
    };
    auto session = negotiate(service, fixture);
    auto dispatch = [&](ControlRequestEnvelope request) {
        return session.dispatch(encode_control_envelope({
            .schema_version = kControlProtocolVersion,
            .payload = std::move(request),
        }));
    };

    auto malformed_input = fixture.request(R"({"include_sensitive":"yes"})", "request-schema-input",
                                           "idempotency-schema-input");
    const auto denied_input = dispatch(std::move(malformed_input));
    CHECK(denied_input.status == ControlServiceStatus::AdmissionDenied);
    CHECK(denied_input.admission_status == ControlAdmissionStatus::InvalidRequest);
    CHECK(executions == 0);

    auto unsupported_version =
        fixture.request("{}", "request-schema-version", "idempotency-schema-version");
    unsupported_version.operation_version = 2;
    unsupported_version.request_hash = *control_request_hash(unsupported_version);
    const auto denied_version = dispatch(std::move(unsupported_version));
    CHECK(denied_version.status == ControlServiceStatus::AdmissionDenied);
    CHECK(denied_version.admission_status == ControlAdmissionStatus::UnknownOperation);
    CHECK(executions == 0);

    const auto invalid_output =
        dispatch(fixture.request("{}", "request-schema-output", "idempotency-schema-output"));
    REQUIRE(invalid_output.status == ControlServiceStatus::Responded);
    CHECK(receipt(invalid_output).state == ControlReceiptState::Failed);
    CHECK(receipt(invalid_output).result_code == ControlResultCode::InternalError);
    CHECK(executions == 1);
}

TEST_CASE("control service rejects retained request id aliasing",
          "[inspect][control][service][request-id][security]") {
    ServiceFixture fixture;
    ControlService service{
        fixture.broker,
        [](const ControlAdmissionPlan&, const ControlRequestEnvelope&,
           const ControlExecutionContext&) { return successful_state_read(); },
    };
    auto session = negotiate(service, fixture);
    auto dispatch = [&](ControlRequestEnvelope request) {
        return session.dispatch(encode_control_envelope({
            .schema_version = kControlProtocolVersion,
            .payload = std::move(request),
        }));
    };

    REQUIRE(dispatch(fixture.request()).status == ControlServiceStatus::Responded);
    const auto conflict = dispatch(fixture.request("{}", "request-a", "different-idempotency-key"));
    CHECK(conflict.status == ControlServiceStatus::AdmissionDenied);
    CHECK(conflict.admission_status == ControlAdmissionStatus::RequestIdConflict);
}

TEST_CASE("control service binds requests to the authenticated client id",
          "[inspect][control][service][identity][security]") {
    ServiceFixture fixture;
    std::size_t executions = 0;
    ControlService service{
        fixture.broker,
        [&](const ControlAdmissionPlan&, const ControlRequestEnvelope&,
            const ControlExecutionContext&) {
            ++executions;
            return successful_state_read();
        },
    };
    auto session = negotiate(service, fixture);
    auto forged_request = fixture.request();
    forged_request.client_id = "client-other";
    forged_request.request_hash = *control_request_hash(forged_request);
    const auto denied = session.dispatch(encode_control_envelope({
        .schema_version = kControlProtocolVersion,
        .payload = std::move(forged_request),
    }));
    CHECK(denied.status == ControlServiceStatus::AdmissionDenied);
    CHECK(denied.admission_status == ControlAdmissionStatus::IdentityMismatch);
    CHECK(executions == 0);
}

TEST_CASE("control service binds cancellation and preserves an applied completion",
          "[inspect][control][service][cancellation][security]") {
    ServiceFixture fixture;
    std::mutex mutex;
    std::condition_variable condition;
    bool executing = false;
    bool release = false;
    std::size_t applications = 0;
    ControlReceiptId running_receipt_id;
    ControlService service{
        fixture.broker,
        [&](const ControlAdmissionPlan& plan, const ControlRequestEnvelope&,
            const ControlExecutionContext&) {
            std::unique_lock lock(mutex);
            running_receipt_id = plan.receipt_id;
            ++applications;
            executing = true;
            condition.notify_all();
            condition.wait_for(lock, 10s, [&] { return release; });
            return successful_state_read();
        },
    };
    auto session = negotiate(service, fixture);
    auto receipts_only = negotiate(service, fixture, false, false);
    const auto invalid_cancellation = session.dispatch(
        R"({"kind":"cancel","payload":{"reason":"","request_id":"request-a"},"schema":"dev.pulp.control/envelope@1","schema_version":1})");
    CHECK(invalid_cancellation.status == ControlServiceStatus::InvalidEnvelope);
    CHECK(invalid_cancellation.explanation == "field 'reason' must not be empty");
    const auto encoded = encode_control_envelope({
        .schema_version = kControlProtocolVersion,
        .payload = fixture.request(),
    });
    ControlServiceResult completed;
    std::thread worker([&] { completed = session.dispatch(encoded); });
    bool started = false;
    {
        std::unique_lock lock(mutex);
        started = condition.wait_for(lock, 2s, [&] { return executing; });
    }
    CHECK(started);
    if (!started) {
        {
            std::lock_guard lock(mutex);
            release = true;
        }
        condition.notify_all();
        worker.join();
        return;
    }

    const auto cancel_envelope = encode_control_envelope({
        .schema_version = kControlProtocolVersion,
        .payload =
            ControlCancelEnvelope{
                .request_id = "request-a",
                .reason = "user stopped operation",
            },
    });
    const auto denied_cancellation = receipts_only.dispatch(cancel_envelope);
    CHECK(denied_cancellation.status == ControlServiceStatus::UnsupportedMessage);
    CHECK(denied_cancellation.explanation == "cancellation feature was not negotiated");
    const auto still_running = fixture.broker.operation_receipt(running_receipt_id);
    REQUIRE(still_running);
    CHECK(still_running->state == ControlReceiptState::Running);
    CHECK_FALSE(still_running->cancellation_requested);

    const auto cancellation = session.dispatch(cancel_envelope);
    CHECK(cancellation.status == ControlServiceStatus::Responded);
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    condition.notify_all();
    worker.join();

    REQUIRE(completed.status == ControlServiceStatus::Responded);
    CHECK(receipt(completed).state == ControlReceiptState::Completed);
    CHECK_FALSE(receipt(completed).result_code);
    CHECK(applications == 1);
    const auto durable = fixture.broker.operation_receipt(running_receipt_id);
    REQUIRE(durable);
    CHECK(durable->cancellation_requested);
    CHECK(durable->cancellation_reason == "user stopped operation");
    CHECK(durable->result.cancellation_reason == "user stopped operation");
    CHECK(durable->result.detail_json == R"({"generation": 0, "parameters": []})");

    const auto replayed = session.dispatch(encoded);
    REQUIRE(replayed.status == ControlServiceStatus::Responded);
    CHECK(receipt(replayed).state == ControlReceiptState::Completed);
    CHECK(applications == 1);
}

TEST_CASE("control client extends the existing authenticated client",
          "[inspect][control][client][security]") {
    InspectorClient inspector;
    ControlClient client{inspector};
    CHECK_FALSE(inspector.is_connected());
    const auto result = client.negotiate(ControlNegotiationOffer{.versions = {1, 1}});
    CHECK_FALSE(result.succeeded());
    CHECK(result.error_code == "not_connected");
    CHECK_FALSE(inspector.is_connected());

    const auto artifact = client.read_artifact("artifact-unavailable", 0, 1);
    CHECK(artifact.status == ControlArtifactStatus::IoError);
    CHECK(artifact.explanation == "artifact reads require the Phase 3c control carrier");
}

TEST_CASE("control client artifact reads stay bound to the injected transport session",
          "[inspect][control][client][artifact][security]") {
    RecordingControlTransport owner_transport{"client-owner"};
    RecordingControlTransport sibling_transport{"client-sibling"};
    ControlClient owner{owner_transport};
    ControlClient sibling{sibling_transport};

    CHECK(owner.read_artifact("artifact-before-negotiation", 0, 1).status ==
          ControlArtifactStatus::Unauthorized);
    const auto negotiated = owner.negotiate(
        ControlNegotiationOffer{
            .versions = {kControlProtocolVersion, kControlProtocolVersion},
            .mandatory_features = {"receipts"},
            .optional_features = {"artifacts"},
        },
        250ms);
    REQUIRE(negotiated.succeeded());
    CHECK(negotiated.response->status == ControlNegotiationStatus::Accepted);
    CHECK(owner_transport.dispatch_timeout == 250ms);
    CHECK_FALSE(owner_transport.last_dispatch.empty());

    const auto owner_read = owner.read_artifact("artifact-session-bound", 7, 11, 400ms);
    REQUIRE(owner_read.status == ControlArtifactStatus::Read);
    REQUIRE(owner_read.metadata);
    CHECK(owner_read.metadata->lineage.producer_client_id == "client-owner");
    CHECK(owner_read.bytes == std::vector<std::uint8_t>{4, 5, 6});
    CHECK(owner_transport.last_artifact_id == "artifact-session-bound");
    CHECK(owner_transport.last_offset == 7);
    CHECK(owner_transport.last_maximum_bytes == 11);
    CHECK(owner_transport.read_timeout == 400ms);

    CHECK(sibling.read_artifact("artifact-session-bound", 7, 11).status ==
          ControlArtifactStatus::Unauthorized);
}
