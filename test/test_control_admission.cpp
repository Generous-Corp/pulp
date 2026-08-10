#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/control_broker.hpp>
#include <pulp/inspect/control_service.hpp>
#include <pulp/runtime/crypto.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace pulp::inspect;
using namespace std::chrono_literals;

namespace {

std::optional<VerifiedControlPeerIdentity> verified_peer(ControlPeerRole role, std::int64_t pid,
                                                         std::string start) {
    ControlPeerVerifier verifier([](const ControlPeerEvidence&) { return true; });
    return verifier.verify(ControlPeerEvidence{
        role,
        "uid:501",
        pid,
        std::move(start),
        "signed:dev.pulp.control-admission-test",
        "publisher.pulp",
    });
}

ControlRegistrationRequest registration_request() {
    ControlManifest manifest;
    manifest.profile = ControlBuildProfile::DeveloperLocal;
    manifest.target = "ControlAdmissionFixture";
    manifest.product_name = "Control Admission Fixture";
    manifest.bundle_id = "dev.pulp.control-admission-fixture";
    manifest.build_id = "build:0123456789abcdef0123456789abcdef";
    manifest.endpoint_included = true;
    manifest.capabilities = {
        InspectorCapability::SessionDescribe,
        InspectorCapability::StateRead,
        InspectorCapability::CaptureImage,
    };
    return {
        ControlHostTier::Standalone, "session-admission", "instance-admission",
        "publication-admission",     std::move(manifest), std::string(64, 'a'),
    };
}

ControlConsentDecision consent(std::string decision_id) {
    return {
        true,
        ControlConsentAuthority::TrustedPulpCli,
        std::move(decision_id),
    };
}

ControlAdmissionPolicy allow_all_policy() {
    auto allow = [](const ControlRegistration&, const ControlOperationDescriptor&) { return true; };
    return {allow, allow, allow};
}

ControlOperationResult state_read_result() {
    ControlOperationResult result;
    result.detail_json = R"({"generation":1,"parameters":[]})";
    return result;
}

ControlOperationResult capture_result(const ControlArtifactMetadata& detail,
                                      const ControlArtifactMetadata& handle) {
    ControlOperationResult result;
    result.detail_json = "{\"artifact_id\":\"" + detail.artifact_id +
                         "\",\"byte_count\":" + std::to_string(detail.byte_size) +
                         ",\"height\":1,\"mime_type\":\"image/png\","
                         "\"redaction_state\":\"redacted\",\"sha256\":\"" + detail.sha256 +
                         "\",\"width\":1}";
    result.artifacts.push_back({handle.artifact_id, handle.content_type, handle.byte_size});
    return result;
}

std::filesystem::path unique_store_path() {
    const auto random = pulp::runtime::secure_random_bytes(8);
    REQUIRE(random);
    return std::filesystem::temp_directory_path() /
           ("pulp-control-admission-" + pulp::runtime::hex_encode(*random));
}

ControlBrokerConfig broker_config(
    ControlAdmissionPolicy policy, const std::filesystem::path& store_path,
    ControlOperationStore::WallClock wall_clock = [] { return std::chrono::system_clock::now(); },
    std::chrono::milliseconds replay_window = std::chrono::hours{24},
    std::chrono::milliseconds retention = std::chrono::hours{24 * 7}) {
    ControlBrokerConfig config;
    config.admission = std::move(policy);
    config.operation_store = ControlOperationStoreConfig{
        .directory = store_path,
        .replay_window = replay_window,
        .retention = retention,
    };
    config.artifact_store = ControlArtifactStoreConfig{
        .root = store_path.parent_path() / (store_path.filename().string() + "-artifacts"),
        .maximum_lifetime = std::chrono::hours{24 * 365 * 200},
    };
    config.wall_clock = std::move(wall_clock);
    return config;
}

void rehash(ControlRequestEnvelope& envelope) {
    envelope.request_hash.clear();
    const auto hash = control_request_hash(envelope);
    REQUIRE(hash);
    envelope.request_hash = *hash;
}

class BlockingWallClock {
  public:
    std::chrono::system_clock::time_point now() {
        std::unique_lock lock(mutex_);
        if (armed_ && ++calls_ == block_on_call_) {
            blocked_ = true;
            condition_.notify_all();
            condition_.wait_for(lock, 10s, [&] { return released_; });
            armed_ = false;
        }
        return std::chrono::system_clock::time_point{std::chrono::milliseconds{1'000}};
    }

    void block_on_call(std::size_t call) {
        std::lock_guard lock(mutex_);
        calls_ = 0;
        block_on_call_ = call;
        blocked_ = false;
        released_ = false;
        armed_ = true;
    }

    bool wait_until_blocked(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [&] { return blocked_; });
    }

    void release() {
        std::lock_guard lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::size_t calls_ = 0;
    std::size_t block_on_call_ = 0;
    bool armed_ = false;
    bool blocked_ = false;
    bool released_ = false;
};

struct AdmissionFixture {
    std::filesystem::path store_path = unique_store_path();
    VerifiedControlPeerIdentity client =
        std::move(*verified_peer(ControlPeerRole::Client, 101, "client-start"));
    VerifiedControlPeerIdentity rogue =
        std::move(*verified_peer(ControlPeerRole::Client, 102, "rogue-start"));
    VerifiedControlPeerIdentity host =
        std::move(*verified_peer(ControlPeerRole::StandaloneHost, 201, "host-start"));
    ControlBroker broker;
    ControlClientIdentity client_identity;
    ControlRegistration registration;
    ControlGrant grant;

    explicit AdmissionFixture(
        ControlAdmissionPolicy policy = allow_all_policy(),
        ControlOperationStore::WallClock wall_clock =
            [] { return std::chrono::system_clock::now(); },
        std::chrono::milliseconds replay_window = std::chrono::hours{24},
        std::chrono::milliseconds retention = std::chrono::hours{24 * 7})
        : broker(broker_config(std::move(policy), store_path, std::move(wall_clock), replay_window,
                               retention)) {
        REQUIRE(broker.operation_store_ready());
        REQUIRE(broker.artifact_store_ready());
        auto ticket = broker.issue_bootstrap(client);
        REQUIRE(ticket.ticket);
        auto connected = broker.redeem_bootstrap(ticket.ticket->ticket_id,
                                                 ticket.ticket->secret.bytes(), client);
        REQUIRE(connected.client);
        client_identity = *connected.client;

        auto registered = broker.register_instance(host, registration_request());
        REQUIRE(registered.registration);
        registration = *registered.registration;

        auto granted = broker.issue_grant(
            client,
            ControlGrantRequest{
                client_identity.client_id,
                registration.registration_id,
                {InspectorCapability::StateRead, InspectorCapability::CaptureImage},
                5min,
            },
            consent("admission-consent"));
        REQUIRE(granted.grant);
        grant = *granted.grant;
    }

    ~AdmissionFixture() {
        std::error_code ignored;
        std::filesystem::remove_all(store_path, ignored);
        std::filesystem::remove_all(
            store_path.parent_path() / (store_path.filename().string() + "-artifacts"), ignored);
    }

    ControlRequestEnvelope state_read(std::string request_id = "request-admission",
                                      std::string idempotency_key = "idempotency-admission") const {
        ControlRequestEnvelope envelope{
            .request_id = std::move(request_id),
            .client_id = client_identity.client_id.value,
            .registration_id = registration.registration_id.value,
            .grant_id = grant.grant_id.value,
            .instance_generation = registration.publication_id,
            .operation_id = "dev.pulp.state/read@1",
            .operation_version = 1,
            .idempotency_key = std::move(idempotency_key),
            .deadline_unix_ms = 4102444800000,
            .params_json = "{}",
        };
        rehash(envelope);
        return envelope;
    }

    ControlRequestEnvelope capture(std::string request_id = "request-capture",
                                   std::string idempotency_key = "idempotency-capture") const {
        auto envelope = state_read(std::move(request_id), std::move(idempotency_key));
        envelope.operation_id = "dev.pulp.ui/capture@1";
        envelope.params_json = R"({"target":"window","format":"png"})";
        rehash(envelope);
        return envelope;
    }

    ControlArtifactStoreResult
    publish_completed_capture_artifact(std::span<const std::uint8_t> bytes,
                                       std::string request_id = "request-artifact-race",
                                       std::string idempotency_key = "idempotency-artifact-race") {
        const auto admitted = broker.admit_operation(
            client, capture(std::move(request_id), std::move(idempotency_key)));
        REQUIRE(admitted.plan);
        REQUIRE(broker.begin_operation(client, *admitted.plan).succeeded());
        auto stored =
            broker.store_operation_artifact(client, *admitted.plan, bytes,
                                            ControlArtifactProperties{
                                                .content_type = "image/png",
                                                .created_at_unix_ms = 1,
                                                .expires_at_unix_ms = 4'102'444'800'000,
                                                .sensitivity =
                                                    ControlArtifactSensitivity::Sensitive,
                                                .redaction_state =
                                                    ControlArtifactRedactionState::Redacted,
                                            });
        REQUIRE(stored.metadata);
        auto completed = capture_result(*stored.metadata, *stored.metadata);
        REQUIRE(broker
                    .finish_operation(client, *admitted.plan, ControlReceiptState::Completed,
                                      std::move(completed))
                    .succeeded());
        return stored;
    }
};

} // namespace

TEST_CASE("Control admission is minted centrally from all seven terms",
          "[inspect][control][admission][security]") {
    AdmissionFixture fixture;
    const auto admitted = fixture.broker.admit_operation(fixture.client, fixture.state_read());
    REQUIRE(admitted.status == ControlAdmissionStatus::Admitted);
    REQUIRE(admitted.permission.allowed);
    REQUIRE(admitted.plan);
    REQUIRE(admitted.receipt);
    CHECK(admitted.should_dispatch);
    CHECK(admitted.receipt->state == ControlReceiptState::Admitted);
    CHECK(admitted.plan->authority_binding() == admitted.receipt->binding.authority_binding());
    CHECK(admitted.plan->operation_id == "dev.pulp.state/read@1");
    CHECK(admitted.plan->session_id == "session-admission");
    CHECK(admitted.plan->publication_id == "publication-admission");
    CHECK(fixture.broker.revalidate_operation(fixture.client, *admitted.plan));

    auto tampered = *admitted.plan;
    tampered.manifest_digest = std::string(64, 'f');
    CHECK_FALSE(fixture.broker.revalidate_operation(fixture.client, tampered));
    CHECK(fixture.broker.begin_operation(fixture.client, tampered).status ==
          ControlOperationStoreStatus::InvalidRequest);
    REQUIRE(fixture.broker.begin_operation(fixture.client, *admitted.plan).succeeded());
}

TEST_CASE("Control admission converts only canonically hashed wire requests",
          "[inspect][control][admission][protocol][security]") {
    AdmissionFixture fixture;
    ControlRequestEnvelope envelope{
        .request_id = "request-admission",
        .client_id = fixture.client_identity.client_id.value,
        .registration_id = fixture.registration.registration_id.value,
        .grant_id = fixture.grant.grant_id.value,
        .instance_generation = fixture.registration.publication_id,
        .operation_id = "dev.pulp.state/read@1",
        .operation_version = 1,
        .idempotency_key = "idempotency-admission",
        .deadline_unix_ms = 4102444800000,
        .params_json = "{}",
    };
    envelope.request_hash = *control_request_hash(envelope);
    CHECK(fixture.broker.admit_operation(fixture.client, envelope).status ==
          ControlAdmissionStatus::Admitted);

    ++envelope.expected_state_generation;
    CHECK_FALSE(control_admission_request(envelope));
    CHECK(fixture.broker.admit_operation(fixture.client, envelope).status ==
          ControlAdmissionStatus::InvalidRequest);

    --envelope.expected_state_generation;
    envelope.params_json = R"({"changed":true})";
    CHECK_FALSE(control_admission_request(envelope));
    CHECK(fixture.broker.admit_operation(fixture.client, envelope).status ==
          ControlAdmissionStatus::InvalidRequest);
}

TEST_CASE("Control admission policies default fail closed",
          "[inspect][control][admission][security]") {
    AdmissionFixture fixture{ControlAdmissionPolicy{}};
    const auto denied = fixture.broker.admit_operation(fixture.client, fixture.state_read());
    CHECK(denied.status == ControlAdmissionStatus::PermissionDenied);
    CHECK_FALSE(denied.permission.allowed);
    REQUIRE(denied.permission.denial);
    CHECK(*denied.permission.denial == ControlDenialReason::HostUnavailable);
    CHECK_FALSE(denied.plan);
}

TEST_CASE("Control admission rejects payload identity and unknown operations",
          "[inspect][control][admission][security]") {
    AdmissionFixture fixture;
    CHECK(fixture.broker.admit_operation(fixture.rogue, fixture.state_read()).status ==
          ControlAdmissionStatus::IdentityMismatch);

    auto unknown = fixture.state_read();
    unknown.operation_id = "dev.pulp.unknown/read@1";
    rehash(unknown);
    CHECK(fixture.broker.admit_operation(fixture.client, unknown).status ==
          ControlAdmissionStatus::UnknownOperation);
}

TEST_CASE("ArtifactRead cannot be admitted through a fresh grant",
          "[inspect][control][admission][artifact][security]") {
    AdmissionFixture fixture;
    auto request = fixture.state_read();
    request.operation_id = "dev.pulp.artifact/read@1";
    request.params_json = R"({"artifact_id":"artifact-test","offset":0,"max_bytes":1})";
    rehash(request);
    const auto denied = fixture.broker.admit_operation(fixture.client, request);
    CHECK(denied.status == ControlAdmissionStatus::ArtifactLineageRequired);
    CHECK_FALSE(denied.plan);
}

TEST_CASE("Artifact reads reauthorize the original producer lineage",
          "[inspect][control][admission][artifact][security]") {
    AdmissionFixture fixture;
    const auto admitted = fixture.broker.admit_operation(fixture.client, fixture.capture());
    REQUIRE(admitted.plan);
    REQUIRE(admitted.receipt);
    REQUIRE(fixture.broker.begin_operation(fixture.client, *admitted.plan).succeeded());
    const std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};
    const auto stored =
        fixture.broker.store_operation_artifact(fixture.client, *admitted.plan, bytes,
                                                ControlArtifactProperties{
                                                    .content_type = "image/png",
                                                    .created_at_unix_ms = 1,
                                                    .expires_at_unix_ms = 4102444800000,
                                                    .sensitivity =
                                                        ControlArtifactSensitivity::Sensitive,
                                                    .redaction_state =
                                                        ControlArtifactRedactionState::Redacted,
                                                });
    REQUIRE(stored.status == ControlArtifactStatus::Stored);
    REQUIRE(stored.metadata);
    auto completed = capture_result(*stored.metadata, *stored.metadata);
    REQUIRE(fixture.broker
                .finish_operation(fixture.client, *admitted.plan, ControlReceiptState::Completed,
                                  std::move(completed))
                .succeeded());
    const auto artifact_id = stored.metadata->artifact_id;

    auto sibling_ticket = fixture.broker.issue_bootstrap(fixture.client);
    REQUIRE(sibling_ticket.ticket.has_value());
    auto sibling_connected = fixture.broker.redeem_bootstrap(
        sibling_ticket.ticket->ticket_id, sibling_ticket.ticket->secret.bytes(), fixture.client);
    REQUIRE(sibling_connected.client.has_value());
    REQUIRE(sibling_connected.client->client_id != fixture.client_identity.client_id);

    ControlService artifact_service{fixture.broker};
    auto owner_session =
        artifact_service.open_session(fixture.client, fixture.client_identity.client_id);
    auto sibling_session =
        artifact_service.open_session(fixture.client, sibling_connected.client->client_id);
    const auto artifact_offer = encode_control_envelope({
        .schema_version = kControlProtocolVersion,
        .payload =
            ControlNegotiationOffer{
                .versions = {kControlProtocolVersion, kControlProtocolVersion},
                .mandatory_features = {"receipts"},
                .optional_features = {"artifacts"},
            },
    });
    REQUIRE(owner_session.dispatch(artifact_offer).status == ControlServiceStatus::Responded);
    REQUIRE(sibling_session.dispatch(artifact_offer).status == ControlServiceStatus::Responded);
    CHECK(sibling_session.read_artifact(artifact_id, 0, bytes.size()).status ==
          ControlArtifactStatus::Unauthorized);
    const auto owner_read = owner_session.read_artifact(artifact_id, 0, bytes.size());
    REQUIRE(owner_read.status == ControlArtifactStatus::Read);
    CHECK(owner_read.bytes == std::vector<std::uint8_t>(bytes.begin(), bytes.end()));

    CHECK(fixture.broker
              .read_artifact(fixture.rogue, fixture.client_identity.client_id, artifact_id, 0,
                             bytes.size())
              .status == ControlArtifactStatus::Unauthorized);
    CHECK(fixture.broker
              .read_artifact(fixture.client, fixture.client_identity.client_id, artifact_id, 0, 0)
              .status == ControlArtifactStatus::InvalidRequest);
    CHECK(fixture.broker
              .read_artifact(fixture.client, fixture.client_identity.client_id, artifact_id,
                             bytes.size() + 1, 1)
              .status == ControlArtifactStatus::InvalidRequest);
    CHECK(fixture.broker
              .read_artifact(fixture.client, fixture.client_identity.client_id, artifact_id,
                             std::numeric_limits<std::uint64_t>::max(), 1)
              .status == ControlArtifactStatus::InvalidRequest);
    const auto first = fixture.broker.read_artifact(
        fixture.client, fixture.client_identity.client_id, artifact_id, 0, 2);
    REQUIRE(first.status == ControlArtifactStatus::Read);
    CHECK(first.bytes == std::vector<std::uint8_t>{1, 2});
    CHECK_FALSE(first.eof);
    const auto last = fixture.broker.read_artifact(
        fixture.client, fixture.client_identity.client_id, artifact_id, 2, 2);
    REQUIRE(last.status == ControlArtifactStatus::Read);
    CHECK(last.bytes == std::vector<std::uint8_t>{3, 4});
    CHECK(last.eof);
    const auto end = fixture.broker.read_artifact(fixture.client, fixture.client_identity.client_id,
                                                  artifact_id, bytes.size(), 2);
    REQUIRE(end.status == ControlArtifactStatus::Read);
    CHECK(end.bytes.empty());
    CHECK(end.eof);

    const auto artifact_root =
        fixture.store_path.parent_path() / (fixture.store_path.filename().string() + "-artifacts");
    const auto blob_path = artifact_root / "blobs" / (stored.metadata->sha256 + ".blob");
    {
        std::fstream tamper(blob_path, std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(tamper.is_open());
        tamper.put(static_cast<char>(9));
    }
    CHECK(fixture.broker
              .read_artifact(fixture.client, fixture.client_identity.client_id, artifact_id, 0,
                             bytes.size())
              .status == ControlArtifactStatus::Corrupt);

    REQUIRE(fixture.broker.revoke_grant(fixture.grant.grant_id, "revoke-artifact-lineage") ==
            ControlGrantStatus::Revoked);
    CHECK(fixture.broker
              .read_artifact(fixture.client, fixture.client_identity.client_id, artifact_id, 0,
                             bytes.size())
              .status == ControlArtifactStatus::Unauthorized);
}

TEST_CASE("Blocked artifact reads do not serialize broker authority changes",
          "[inspect][control][admission][artifact][security][concurrency]") {
    auto clock = std::make_shared<BlockingWallClock>();
    AdmissionFixture fixture{allow_all_policy(), [clock] { return clock->now(); }};
    const std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};
    const auto stored = fixture.publish_completed_capture_artifact(bytes);
    REQUIRE(stored.metadata);

    // One artifact metadata clock read and one broker authorization clock read
    // precede the artifact store's second metadata check and blob read.
    clock->block_on_call(3);
    auto read_future = std::async(std::launch::async, [&] {
        return fixture.broker.read_artifact(fixture.client, fixture.client_identity.client_id,
                                            stored.metadata->artifact_id, 0, bytes.size());
    });
    const bool read_blocked = clock->wait_until_blocked(5s);
    if (!read_blocked)
        clock->release();
    REQUIRE(read_blocked);

    auto admission_future = std::async(std::launch::async, [&] {
        return fixture.broker.admit_operation(
            fixture.client, fixture.state_read("request-unrelated-while-reading",
                                               "idempotency-unrelated-while-reading"));
    });
    const bool admission_completed = admission_future.wait_for(5s) == std::future_status::ready;
    if (!admission_completed) {
        clock->release();
        admission_future.wait_for(10s);
        read_future.wait_for(10s);
    }
    REQUIRE(admission_completed);
    CHECK(admission_future.get().status == ControlAdmissionStatus::Admitted);

    auto revocation_future = std::async(std::launch::async, [&] {
        return fixture.broker.revoke_grant(fixture.grant.grant_id, "revoke-during-artifact-read");
    });
    const bool revocation_completed = revocation_future.wait_for(5s) == std::future_status::ready;
    if (!revocation_completed) {
        clock->release();
        revocation_future.wait_for(10s);
        read_future.wait_for(10s);
    }
    REQUIRE(revocation_completed);
    CHECK(revocation_future.get() == ControlGrantStatus::Revoked);

    clock->release();
    const auto read = read_future.get();
    CHECK(read.status == ControlArtifactStatus::Unauthorized);
    CHECK_FALSE(read.metadata);
    CHECK(read.bytes.empty());
}

TEST_CASE("Artifact blob tampering during a blocked read cannot leak bytes",
          "[inspect][control][admission][artifact][security][concurrency]") {
    auto clock = std::make_shared<BlockingWallClock>();
    AdmissionFixture fixture{allow_all_policy(), [clock] { return clock->now(); }};
    const std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};
    const auto stored = fixture.publish_completed_capture_artifact(bytes);
    REQUIRE(stored.metadata);

    clock->block_on_call(3);
    auto read_future = std::async(std::launch::async, [&] {
        return fixture.broker.read_artifact(fixture.client, fixture.client_identity.client_id,
                                            stored.metadata->artifact_id, 0, bytes.size());
    });
    const bool read_blocked = clock->wait_until_blocked(5s);
    if (!read_blocked)
        clock->release();
    REQUIRE(read_blocked);

    const auto artifact_root =
        fixture.store_path.parent_path() / (fixture.store_path.filename().string() + "-artifacts");
    const auto blob_path = artifact_root / "blobs" / (stored.metadata->sha256 + ".blob");
    bool tampered = false;
    {
        std::fstream blob(blob_path, std::ios::binary | std::ios::in | std::ios::out);
        if (blob.is_open()) {
            blob.put(static_cast<char>(9));
            tampered = blob.good();
        }
    }
    clock->release();
    REQUIRE(tampered);

    const auto read = read_future.get();
    CHECK(read.status == ControlArtifactStatus::Corrupt);
    CHECK(read.bytes.empty());
}

TEST_CASE("Artifact-producing completion binds typed detail to the authorized artifact",
          "[inspect][control][admission][artifact][security]") {
    AdmissionFixture fixture;
    const auto admitted = fixture.broker.admit_operation(fixture.client, fixture.capture());
    REQUIRE(admitted.plan);
    REQUIRE(fixture.broker.begin_operation(fixture.client, *admitted.plan).succeeded());

    const std::array<std::uint8_t, 4> first_bytes{1, 2, 3, 4};
    const std::array<std::uint8_t, 3> second_bytes{5, 6, 7};
    const auto publish = [&](auto bytes, std::string content_type = "image/png") {
        return fixture.broker.store_operation_artifact(fixture.client, *admitted.plan, bytes,
                                                       ControlArtifactProperties{
                                                           .content_type = std::move(content_type),
                                                           .created_at_unix_ms = 1,
                                                           .expires_at_unix_ms = 4102444800000,
                                                           .sensitivity =
                                                               ControlArtifactSensitivity::Sensitive,
                                                           .redaction_state =
                                                               ControlArtifactRedactionState::Redacted,
                                                       });
    };
    const auto first = publish(first_bytes);
    const auto second = publish(second_bytes);
    const auto wrong_type = publish(first_bytes, "application/octet-stream");
    REQUIRE(first.metadata);
    REQUIRE(second.metadata);
    REQUIRE(wrong_type.metadata);

    const auto result_for = capture_result;
    const auto finish = [&](ControlOperationResult result) {
        return fixture.broker.finish_operation(fixture.client, *admitted.plan,
                                               ControlReceiptState::Completed, std::move(result));
    };

    CHECK(finish(result_for(*first.metadata, *second.metadata)).status ==
          ControlOperationStoreStatus::InvalidRequest);

    auto wrong_hash = result_for(*first.metadata, *first.metadata);
    wrong_hash.detail_json.replace(wrong_hash.detail_json.find(first.metadata->sha256), 64,
                                   std::string(64, 'f'));
    CHECK(finish(std::move(wrong_hash)).status == ControlOperationStoreStatus::InvalidRequest);

    auto wrong_size = result_for(*first.metadata, *first.metadata);
    wrong_size.detail_json.replace(wrong_size.detail_json.find("\"byte_count\":4"), 14,
                                   "\"byte_count\":5");
    CHECK(finish(std::move(wrong_size)).status == ControlOperationStoreStatus::InvalidRequest);

    CHECK(finish(result_for(*wrong_type.metadata, *wrong_type.metadata)).status ==
          ControlOperationStoreStatus::InvalidRequest);

    auto missing_handle = result_for(*first.metadata, *first.metadata);
    missing_handle.artifacts.clear();
    CHECK(finish(std::move(missing_handle)).status == ControlOperationStoreStatus::InvalidRequest);

    auto failed_with_artifact = result_for(*first.metadata, *first.metadata);
    failed_with_artifact.result_code = ControlResultCode::InternalError;
    CHECK(fixture.broker
              .finish_operation(fixture.client, *admitted.plan, ControlReceiptState::Failed,
                                std::move(failed_with_artifact))
              .status == ControlOperationStoreStatus::InvalidRequest);

    const auto completed = finish(result_for(*first.metadata, *first.metadata));
    REQUIRE(completed.succeeded());
    REQUIRE(completed.receipt);
    CHECK(completed.receipt->state == ControlReceiptState::Completed);
    REQUIRE(completed.receipt->result.artifacts.size() == 1);
    CHECK(completed.receipt->result.artifacts.front().artifact_id == first.metadata->artifact_id);

    const auto exact =
        fixture.broker.read_artifact(fixture.client, fixture.client_identity.client_id,
                                     first.metadata->artifact_id, 0, first_bytes.size());
    REQUIRE(exact.status == ControlArtifactStatus::Read);
    CHECK(exact.bytes == std::vector<std::uint8_t>(first_bytes.begin(), first_bytes.end()));

    const std::array<std::uint8_t, 4> substituted_bytes{9, 8, 7, 6};
    const auto substituted_hash =
        pulp::runtime::sha256_hex(substituted_bytes.data(), substituted_bytes.size());
    const auto artifact_root =
        fixture.store_path.parent_path() / (fixture.store_path.filename().string() + "-artifacts");
    const auto substituted_blob = artifact_root / "blobs" / (substituted_hash + ".blob");
    {
        std::ofstream blob(substituted_blob, std::ios::binary | std::ios::trunc);
        REQUIRE(blob.is_open());
        blob.write(reinterpret_cast<const char*>(substituted_bytes.data()),
                   static_cast<std::streamsize>(substituted_bytes.size()));
        REQUIRE(blob.good());
    }
    std::filesystem::permissions(
        substituted_blob, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace);

    const auto metadata_path =
        artifact_root / "artifacts" / (first.metadata->artifact_id + ".meta");
    std::ifstream metadata_input(metadata_path, std::ios::binary);
    REQUIRE(metadata_input.is_open());
    std::string metadata_text((std::istreambuf_iterator<char>(metadata_input)),
                              std::istreambuf_iterator<char>());
    const auto encoded_old_hash = pulp::runtime::hex_encode(
        reinterpret_cast<const std::uint8_t*>(first.metadata->sha256.data()),
        first.metadata->sha256.size());
    const auto encoded_new_hash = pulp::runtime::hex_encode(
        reinterpret_cast<const std::uint8_t*>(substituted_hash.data()), substituted_hash.size());
    const auto hash_at = metadata_text.find("sha256=" + encoded_old_hash);
    REQUIRE(hash_at != std::string::npos);
    metadata_text.replace(hash_at, 7 + encoded_old_hash.size(), "sha256=" + encoded_new_hash);
    metadata_input.close();
    {
        std::ofstream metadata_output(metadata_path, std::ios::binary | std::ios::trunc);
        REQUIRE(metadata_output.is_open());
        metadata_output.write(metadata_text.data(),
                              static_cast<std::streamsize>(metadata_text.size()));
        REQUIRE(metadata_output.good());
    }
    std::filesystem::permissions(
        metadata_path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace);

    CHECK(fixture.broker
              .read_artifact(fixture.client, fixture.client_identity.client_id,
                             first.metadata->artifact_id, 0, first_bytes.size())
              .status == ControlArtifactStatus::Unauthorized);
}

TEST_CASE("Non-artifact operations cannot retain published blobs",
          "[inspect][control][admission][artifact][security]") {
    AdmissionFixture fixture;
    const auto admitted = fixture.broker.admit_operation(fixture.client, fixture.state_read());
    REQUIRE(admitted.plan);
    REQUIRE(fixture.broker.begin_operation(fixture.client, *admitted.plan).succeeded());
    const std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};
    const auto stored = fixture.broker.store_operation_artifact(
        fixture.client, *admitted.plan, bytes,
        {.content_type = "application/octet-stream",
         .created_at_unix_ms = 1,
         .expires_at_unix_ms = 4102444800000});
    REQUIRE(stored.metadata);

    auto result = state_read_result();
    result.artifacts.push_back(
        {stored.metadata->artifact_id, stored.metadata->content_type, stored.metadata->byte_size});
    CHECK(fixture.broker
              .finish_operation(fixture.client, *admitted.plan, ControlReceiptState::Completed,
                                std::move(result))
              .status == ControlOperationStoreStatus::InvalidRequest);

    REQUIRE(fixture.broker
                .finish_operation(fixture.client, *admitted.plan, ControlReceiptState::Failed,
                                  {.result_code = ControlResultCode::InternalError})
                .succeeded());
    CHECK(fixture.broker
              .read_artifact(fixture.client, fixture.client_identity.client_id,
                             stored.metadata->artifact_id, 0, bytes.size())
              .status == ControlArtifactStatus::Unauthorized);
}

TEST_CASE("Control admission rejects elapsed deadlines and expires queued work",
          "[inspect][control][admission][deadline][race]") {
    auto now_ms = std::make_shared<std::int64_t>(100);
    AdmissionFixture fixture{allow_all_policy(), [now_ms] {
                                 return std::chrono::system_clock::time_point{
                                     std::chrono::milliseconds(*now_ms)};
                             }};

    auto expired = fixture.state_read("expired-request", "expired-key");
    expired.deadline_unix_ms = 99;
    rehash(expired);
    CHECK(fixture.broker.admit_operation(fixture.client, expired).status ==
          ControlAdmissionStatus::DeadlineExceeded);

    auto queued = fixture.state_read("queued-request", "queued-key");
    queued.deadline_unix_ms = 101;
    rehash(queued);
    const auto admitted = fixture.broker.admit_operation(fixture.client, queued);
    REQUIRE(admitted.plan);
    *now_ms = 102;
    const auto finished = fixture.broker.begin_operation(fixture.client, *admitted.plan);
    REQUIRE(finished.succeeded());
    REQUIRE(finished.receipt);
    CHECK(finished.receipt->state == ControlReceiptState::Failed);
    REQUIRE(finished.receipt->result.result_code);
    CHECK(*finished.receipt->result.result_code == ControlResultCode::DeadlineExceeded);
}

TEST_CASE("Control cancellation blocks boundaries and preserves applied success",
          "[inspect][control][admission][cancellation][race]") {
    AdmissionFixture fixture;
    const auto queued = fixture.broker.admit_operation(
        fixture.client, fixture.state_read("cancel-queued", "cancel-queued-key"));
    REQUIRE(queued.plan);
    const auto requested = fixture.broker.cancel_operation(
        fixture.client, fixture.client_identity.client_id, "cancel-queued", "caller-cancelled");
    REQUIRE(requested.succeeded());
    REQUIRE(requested.receipt);
    CHECK(requested.receipt->cancellation_requested);
    const auto cancelled = fixture.broker.begin_operation(fixture.client, *queued.plan);
    REQUIRE(cancelled.succeeded());
    REQUIRE(cancelled.receipt);
    CHECK(cancelled.receipt->state == ControlReceiptState::Cancelled);
    CHECK(cancelled.receipt->result.cancellation_reason == "caller-cancelled");

    const auto running = fixture.broker.admit_operation(
        fixture.client, fixture.state_read("cancel-running", "cancel-running-key"));
    REQUIRE(running.plan);
    REQUIRE(fixture.broker.begin_operation(fixture.client, *running.plan).succeeded());
    REQUIRE(fixture.broker
                .cancel_operation(fixture.client, fixture.client_identity.client_id,
                                  "cancel-running", "stop-at-boundary")
                .succeeded());
    CHECK(fixture.broker.operation_cancellation_requested(fixture.client, *running.plan));
    CHECK_FALSE(fixture.broker.operation_cancellation_requested(fixture.rogue, *running.plan));

    const std::array<std::uint8_t, 1> bytes{1};
    CHECK(fixture.broker
              .store_operation_artifact(fixture.client, *running.plan, bytes,
                                        ControlArtifactProperties{
                                            .content_type = "application/octet-stream",
                                            .created_at_unix_ms = 1,
                                            .expires_at_unix_ms = 4102444800000,
                                        })
              .status == ControlArtifactStatus::Unauthorized);

    const auto stopped = fixture.broker.finish_operation(
        fixture.client, *running.plan, ControlReceiptState::Completed, state_read_result());
    REQUIRE(stopped.succeeded());
    REQUIRE(stopped.receipt);
    CHECK(stopped.receipt->state == ControlReceiptState::Completed);
    CHECK_FALSE(stopped.receipt->result.result_code);
    CHECK(stopped.receipt->cancellation_requested);
    CHECK(stopped.receipt->cancellation_reason == "stop-at-boundary");
    CHECK(stopped.receipt->result.cancellation_reason == "stop-at-boundary");
    CHECK(stopped.receipt->result.detail_json == R"({"generation": 1, "parameters": []})");
}

TEST_CASE("Control broker durably deduplicates before dispatch",
          "[inspect][control][admission][receipt][idempotency]") {
    AdmissionFixture fixture;
    const auto request = fixture.state_read();
    const auto first = fixture.broker.admit_operation(fixture.client, request);
    REQUIRE(first.plan);
    REQUIRE(first.receipt);
    CHECK(first.should_dispatch);

    const auto replay = fixture.broker.admit_operation(fixture.client, request);
    REQUIRE(replay.receipt);
    CHECK_FALSE(replay.should_dispatch);
    CHECK(replay.receipt->receipt_id == first.receipt->receipt_id);

    auto drifted = request;
    drifted.params_json = R"({"include_sensitive":true})";
    rehash(drifted);
    const auto conflict = fixture.broker.admit_operation(fixture.client, drifted);
    CHECK(conflict.status == ControlAdmissionStatus::IdempotencyConflict);
    CHECK_FALSE(conflict.should_dispatch);

    REQUIRE(fixture.broker.begin_operation(fixture.client, *first.plan).succeeded());
    const auto invalid = fixture.broker.finish_operation(fixture.client, *first.plan,
                                                         ControlReceiptState::Completed);
    CHECK(invalid.status == ControlOperationStoreStatus::InvalidRequest);
    REQUIRE(invalid.receipt);
    CHECK(invalid.receipt->state == ControlReceiptState::Running);

    auto claimed_revoked = state_read_result();
    claimed_revoked.result_code = ControlResultCode::CompletedAfterRevocation;
    const auto finished = fixture.broker.finish_operation(
        fixture.client, *first.plan, ControlReceiptState::CompletedAfterRevocation,
        std::move(claimed_revoked));
    REQUIRE(finished.succeeded());
    REQUIRE(finished.receipt);
    CHECK(finished.receipt->state == ControlReceiptState::Completed);
    CHECK_FALSE(finished.receipt->result.result_code);
}

TEST_CASE("Control broker reports terminal replay-window expiry without redispatch",
          "[inspect][control][admission][receipt][idempotency][expiry]") {
    auto now = std::make_shared<std::chrono::system_clock::time_point>(
        std::chrono::system_clock::time_point{1000ms});
    AdmissionFixture fixture{allow_all_policy(), [now] { return *now; }, 100ms, 1000ms};
    const auto request = fixture.state_read();
    const auto admitted = fixture.broker.admit_operation(fixture.client, request);
    REQUIRE(admitted.plan.has_value());
    REQUIRE(admitted.receipt.has_value());
    REQUIRE(fixture.broker.begin_operation(fixture.client, *admitted.plan).succeeded());
    const auto completed = fixture.broker.finish_operation(
        fixture.client, *admitted.plan, ControlReceiptState::Completed, state_read_result());
    REQUIRE(completed.succeeded());
    REQUIRE(completed.receipt.has_value());

    *now += 100ms;
    const auto expired = fixture.broker.admit_operation(fixture.client, request);
    CHECK(expired.status == ControlAdmissionStatus::ReplayWindowExpired);
    CHECK_FALSE(expired.should_dispatch);
    CHECK_FALSE(expired.plan.has_value());
    REQUIRE(expired.receipt.has_value());
    CHECK(expired.receipt->receipt_id == completed.receipt->receipt_id);
    CHECK(expired.receipt->state == ControlReceiptState::Completed);

    const auto repeated = fixture.broker.admit_operation(fixture.client, request);
    CHECK(repeated.status == ControlAdmissionStatus::ReplayWindowExpired);
    CHECK_FALSE(repeated.should_dispatch);
    REQUIRE(repeated.receipt.has_value());
    CHECK(repeated.receipt->receipt_id == completed.receipt->receipt_id);
}

TEST_CASE("Control admission revalidates revocation before side effects",
          "[inspect][control][admission][grant][race]") {
    AdmissionFixture fixture;
    const auto admitted = fixture.broker.admit_operation(fixture.client, fixture.state_read());
    REQUIRE(admitted.plan);

    CHECK(fixture.broker.revoke_grant(fixture.grant.grant_id, "revoke-before-dispatch") ==
          ControlGrantStatus::Revoked);
    CHECK_FALSE(fixture.broker.revalidate_operation(fixture.client, *admitted.plan));
    const auto cancelled = fixture.broker.begin_operation(fixture.client, *admitted.plan);
    REQUIRE(cancelled.succeeded());
    REQUIRE(cancelled.receipt);
    CHECK(cancelled.receipt->state == ControlReceiptState::Cancelled);
}

TEST_CASE("Control completion reports completed after revocation truthfully",
          "[inspect][control][admission][receipt][grant][race]") {
    AdmissionFixture fixture;
    const auto admitted = fixture.broker.admit_operation(fixture.client, fixture.state_read());
    REQUIRE(admitted.plan);
    REQUIRE(fixture.broker.begin_operation(fixture.client, *admitted.plan).succeeded());

    REQUIRE(fixture.broker.revoke_grant(fixture.grant.grant_id, "revoke-while-running") ==
            ControlGrantStatus::Revoked);
    const std::array<std::uint8_t, 1> bytes{1};
    CHECK(fixture.broker
              .store_operation_artifact(fixture.client, *admitted.plan, bytes,
                                        ControlArtifactProperties{
                                            .content_type = "application/octet-stream",
                                            .created_at_unix_ms = 1,
                                            .expires_at_unix_ms = 4102444800000,
                                        })
              .status == ControlArtifactStatus::Unauthorized);
    const auto finished = fixture.broker.finish_operation(
        fixture.client, *admitted.plan, ControlReceiptState::Completed, state_read_result());
    REQUIRE(finished.succeeded());
    REQUIRE(finished.receipt);
    CHECK(finished.receipt->state == ControlReceiptState::CompletedAfterRevocation);
    REQUIRE(finished.receipt->result.result_code);
    CHECK(*finished.receipt->result.result_code == ControlResultCode::CompletedAfterRevocation);
}
