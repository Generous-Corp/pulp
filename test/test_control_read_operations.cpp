#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/control_service.hpp>
#include <pulp/inspect/control_state_read_executor.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/state/store.hpp>

#include <choc/text/choc_JSON.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

using namespace pulp::inspect;
using namespace std::chrono_literals;

namespace {

namespace fs = std::filesystem;

struct TemporaryDirectory {
    fs::path path;
    TemporaryDirectory() {
        const auto random = pulp::runtime::secure_random_bytes(8);
        REQUIRE(random);
        path =
            fs::temp_directory_path() / ("pulp-control-read-" + pulp::runtime::hex_encode(*random));
        REQUIRE(fs::create_directory(path));
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

VerifiedControlPeerIdentity peer(ControlPeerRole role, std::int64_t pid, std::string start,
                                 std::string executable) {
    ControlPeerVerifier verifier([](const ControlPeerEvidence&) { return true; });
    auto verified = verifier.verify({.role = role,
                                     .user_id = "uid:501",
                                     .process_id = pid,
                                     .process_start_id = std::move(start),
                                     .executable_identity = std::move(executable),
                                     .publisher_id = "publisher.pulp"});
    REQUIRE(verified);
    return std::move(*verified);
}

ControlManifest read_manifest(ControlBuildProfile profile, std::string target, std::string bundle,
                              std::string build) {
    return {.profile = profile,
            .target = std::move(target),
            .product_name = "Phase 4 Read Fixture",
            .bundle_id = std::move(bundle),
            .build_id = std::move(build),
            .endpoint_included = true,
            .capabilities = {InspectorCapability::SessionDescribe, InspectorCapability::StateRead}};
}

const ControlReceiptEnvelope& receipt(const ControlServiceResult& result) {
    REQUIRE(result.response);
    const auto* value = std::get_if<ControlReceiptEnvelope>(&result.response->payload);
    REQUIRE(value);
    return *value;
}

choc::value::Value detail(const ControlServiceResult& result) {
    const auto& value = receipt(result);
    REQUIRE(value.state == ControlReceiptState::Completed);
    return choc::json::parse(value.detail_json);
}

struct Fixture {
    TemporaryDirectory temporary;
    VerifiedControlPeerIdentity client_peer =
        peer(ControlPeerRole::Client, 1001, "client-start", "signed:client");
    VerifiedControlPeerIdentity offline_peer =
        peer(ControlPeerRole::OfflineHost, 2001, "offline-start", "signed:offline");
    VerifiedControlPeerIdentity standalone_a_peer =
        peer(ControlPeerRole::StandaloneHost, 3001, "standalone-a", "signed:standalone-a");
    VerifiedControlPeerIdentity standalone_b_peer =
        peer(ControlPeerRole::StandaloneHost, 3002, "standalone-b", "signed:standalone-b");
    ControlBroker broker;
    ControlClientIdentity client;
    std::unordered_map<std::string, std::unique_ptr<pulp::state::StateStore>> stores;

    Fixture()
        : broker([&] {
              ControlBrokerConfig config;
              config.operation_store = {.directory = temporary.path / "receipts"};
              config.admission.host_available = [](const auto&, const auto&) { return true; };
              config.admission.activated = [](const auto&, const auto&) { return true; };
              config.admission.policy_eligible = [](const auto&, const auto&) { return true; };
              return config;
          }()) {
        auto ticket = broker.issue_bootstrap(client_peer);
        REQUIRE(ticket.ticket);
        auto connected = broker.redeem_bootstrap(ticket.ticket->ticket_id,
                                                 ticket.ticket->secret.bytes(), client_peer);
        REQUIRE(connected.client);
        client = *connected.client;
    }

    ControlRegistration register_host(const VerifiedControlPeerIdentity& host, ControlHostTier tier,
                                      std::string session, std::string instance,
                                      std::string publication, char digest, std::string build) {
        auto registered = broker.register_instance(
            host, {.host_tier = tier,
                   .session_id = std::move(session),
                   .instance_id = std::move(instance),
                   .publication_id = std::move(publication),
                   .manifest = read_manifest(
                       tier == ControlHostTier::OfflineJob ? ControlBuildProfile::TestDeterministic
                                                           : ControlBuildProfile::DeveloperLocal,
                       "ReadFixture", "dev.pulp.read-fixture", std::move(build)),
                   .artifact_digest = std::string(64, digest)});
        REQUIRE(registered.registration);
        auto store = std::make_unique<pulp::state::StateStore>();
        store->add_parameter({.id = 1,
                              .name = "Gain",
                              .unit = "dB",
                              .range = {-60.0f, 12.0f, 0.0f, 0.5f},
                              .group_id = 7,
                              .rate = pulp::state::ParamRate::ControlRate,
                              .kind = pulp::state::ParamKind::Continuous});
        store->add_parameter({.id = 2,
                              .name = "Secret Mode",
                              .range = {0.0f, 2.0f, 0.0f, 1.0f},
                              .rate = pulp::state::ParamRate::AudioRate,
                              .designation = pulp::state::ParamDesignation::Reset,
                              .is_trigger = true,
                              .kind = pulp::state::ParamKind::Enum,
                              .value_labels = {"Off", "A", "B"}});
        store->set_value(1, -6.0f);
        store->set_value(2, 1.0f);
        while (store->state_generation() < 42)
            store->set_value(1, store->get_value(1));
        stores.emplace(registered.registration->registration_id.value, std::move(store));
        return *registered.registration;
    }

    ControlGrant grant(const ControlRegistration& registration) {
        auto result = broker.issue_grant(
            client_peer,
            {.client_id = client.client_id,
             .registration_id = registration.registration_id,
             .capabilities = {InspectorCapability::SessionDescribe, InspectorCapability::StateRead},
             .ttl = 5min},
            {.approved = true,
             .authority = ControlConsentAuthority::TrustedPulpCli,
             .decision_id = "decision-" + registration.registration_id.value});
        REQUIRE(result.grant);
        return *result.grant;
    }

    ControlRequestEnvelope request(const ControlRegistration& registration,
                                   const ControlGrant& authority, std::string operation,
                                   std::string params, std::string suffix) const {
        ControlRequestEnvelope value{
            .request_id = "request-" + suffix,
            .client_id = client.client_id.value,
            .registration_id = registration.registration_id.value,
            .grant_id = authority.grant_id.value,
            .instance_generation = registration.publication_id,
            .operation_id = std::move(operation),
            .operation_version = 1,
            .idempotency_key = "key-" + suffix,
            .deadline_unix_ms = 4'102'444'800'000,
            .params_json = std::move(params),
        };
        value.request_hash = *control_request_hash(value);
        return value;
    }

    ControlService service() {
        return ControlService{
            broker,
            make_control_state_read_executor(
                [this](const ControlAdmissionPlan& plan) -> std::optional<ControlStateReadSource> {
                    const auto found = stores.find(plan.registration_id.value);
                    if (found == stores.end())
                        return std::nullopt;
                    const auto registration = broker.registration(plan.registration_id);
                    if (!registration)
                        return std::nullopt;
                    return ControlStateReadSource{
                        .registration_id = plan.registration_id,
                        .host_tier = registration->host_tier,
                        .store = found->second.get(),
                        .state_generation = found->second->state_generation(),
                        .catalog_generation = 3,
                        .is_sensitive = [](pulp::state::ParamID id) { return id == 2; }};
                })};
    }

    ControlService::Session session(ControlService& service) {
        auto value = service.open_session(client_peer, client.client_id);
        REQUIRE(value.is_open());
        const auto negotiated = value.dispatch(encode_control_envelope(
            {.payload =
                 ControlNegotiationOffer{.versions = {1, 1}, .mandatory_features = {"receipts"}}}));
        REQUIRE(negotiated.status == ControlServiceStatus::Responded);
        return value;
    }
};

} // namespace

TEST_CASE("Phase 4 instance reads preserve exact T0/T1 lifecycle identity",
          "[inspect][control][read][t0][t1][identity]") {
    Fixture fixture;
    auto offline = fixture.register_host(fixture.offline_peer, ControlHostTier::OfflineJob,
                                         "job-session", "job-1", "job-generation-1", 'a',
                                         "build:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    auto standalone_a = fixture.register_host(
        fixture.standalone_a_peer, ControlHostTier::Standalone, "standalone-session", "instance-a",
        "publication-a", 'b', "build:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    auto standalone_b = fixture.register_host(
        fixture.standalone_b_peer, ControlHostTier::Standalone, "standalone-session", "instance-b",
        "publication-b", 'c', "build:cccccccccccccccccccccccccccccccc");
    auto offline_grant = fixture.grant(offline);
    auto grant_a = fixture.grant(standalone_a);
    auto grant_b = fixture.grant(standalone_b);
    auto service = fixture.service();
    auto session = fixture.session(service);

    const auto offline_result = detail(session.dispatch(encode_control_envelope(
        {.payload = fixture.request(offline, offline_grant, "dev.pulp.instance/read@1", "{}",
                                    "offline-identity")})));
    CHECK(offline_result["instance_kind"].getString() == "offline-job");
    CHECK(offline_result["session_id"].getString() == "job-session");
    CHECK(offline_result["build_id"].getString() == "build:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");

    const auto result_a = detail(session.dispatch(encode_control_envelope(
        {.payload = fixture.request(standalone_a, grant_a, "dev.pulp.instance/read@1", "{}",
                                    "standalone-a")})));
    const auto result_b = detail(session.dispatch(encode_control_envelope(
        {.payload = fixture.request(standalone_b, grant_b, "dev.pulp.instance/read@1", "{}",
                                    "standalone-b")})));
    CHECK(result_a["instance_kind"].getString() == "standalone");
    CHECK(result_a["instance_id"].getString() == "instance-a");
    CHECK(result_b["instance_id"].getString() == "instance-b");
    CHECK(result_a["registration_id"].getString() != result_b["registration_id"].getString());
    CHECK(result_a["peer_identity_sha256"].getString() !=
          result_b["peer_identity_sha256"].getString());

    REQUIRE(fixture.broker.heartbeat(standalone_a.registration_id, fixture.standalone_a_peer));
    const auto heartbeat = detail(session.dispatch(encode_control_envelope(
        {.payload = fixture.request(standalone_a, grant_a, "dev.pulp.instance/read@1", "{}",
                                    "standalone-heartbeat")})));
    CHECK(heartbeat["liveness_generation"].getInt64() == 2);

    const auto old_registration_id = standalone_a.registration_id.value;
    REQUIRE(fixture.broker
                .unregister_instance(standalone_a.registration_id, fixture.standalone_a_peer,
                                     "rapid-restart")
                .identity_removed);
    const auto stale = session.dispatch(encode_control_envelope(
        {.payload = fixture.request(standalone_a, grant_a, "dev.pulp.instance/read@1", "{}",
                                    "stale-after-restart")}));
    CHECK(stale.status == ControlServiceStatus::AdmissionDenied);

    auto restarted_peer = peer(ControlPeerRole::StandaloneHost, 3001,
                               "standalone-a-restarted", "signed:standalone-a");
    auto restarted = fixture.register_host(restarted_peer, ControlHostTier::Standalone,
                                           "standalone-session", "instance-a", "publication-a", 'b',
                                           "build:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    CHECK(restarted.registration_id.value != old_registration_id);
    auto restarted_grant = fixture.grant(restarted);
    const auto restart_result = detail(session.dispatch(encode_control_envelope(
        {.payload = fixture.request(restarted, restarted_grant, "dev.pulp.instance/read@1", "{}",
                                    "restarted")})));
    CHECK(restart_result["lifecycle_status"].getString() == "active");
    CHECK(restart_result["liveness_generation"].getInt64() == 1);
}

TEST_CASE("Phase 4 state reads return bounded canonical catalog snapshots",
          "[inspect][control][read][t0][t1][state][catalog]") {
    Fixture fixture;
    auto offline = fixture.register_host(
        fixture.offline_peer, ControlHostTier::OfflineJob, "offline-session", "offline-instance",
        "offline-publication", 'f', "build:ffffffffffffffffffffffffffffffff");
    auto standalone = fixture.register_host(fixture.standalone_a_peer, ControlHostTier::Standalone,
                                            "session", "instance", "publication", 'd',
                                            "build:dddddddddddddddddddddddddddddddd");
    auto& standalone_store = *fixture.stores.at(standalone.registration_id.value);
    standalone_store.add_parameter({.id = 1,
                                    .name = "Replacement Gain",
                                    .unit = "dB",
                                    .range = {-60.0f, 12.0f, -6.0f, 0.5f},
                                    .group_id = 7,
                                    .rate = pulp::state::ParamRate::ControlRate,
                                    .kind = pulp::state::ParamKind::Continuous});
    REQUIRE(standalone_store.set_parameter_display_name(1, "Live Gain"));
    auto offline_authority = fixture.grant(offline);
    auto authority = fixture.grant(standalone);
    auto service = fixture.service();
    auto session = fixture.session(service);

    auto offline_values = detail(session.dispatch(encode_control_envelope(
        {.payload = fixture.request(offline, offline_authority, "dev.pulp.state/read@1",
                                    R"({"include_catalog":false,"parameter_ids":[1]})",
                                    "offline-values")})));
    REQUIRE(offline_values["parameters"].size() == 1);
    CHECK(offline_values["parameters"][0]["id"].getInt64() == 1);

    auto visible = detail(session.dispatch(encode_control_envelope(
        {.payload = fixture.request(standalone, authority, "dev.pulp.state/read@1",
                                    R"({"include_catalog":true,"include_sensitive":false})",
                                    "visible-state")})));
    CHECK(visible["state_generation"].getInt64() == 42);
    CHECK(visible["catalog_generation"].getInt64() == 3);
    CHECK(visible["catalog_included"].getBool());
    CHECK(visible["redacted_count"].getInt64() == 1);
    REQUIRE(visible["parameters"].size() == 1);
    const auto gain = visible["parameters"][0];
    CHECK(gain["id"].getInt64() == 1);
    CHECK(gain["name"].getString() == "Live Gain");
    CHECK(gain["unit"].getString() == "dB");
    CHECK(gain["value"].getWithDefault<double>(0.0) == -6.0);
    CHECK(gain["normalized"].getWithDefault<double>(0.0) == 0.75);
    CHECK(gain["kind"].getString() == "continuous");
    CHECK(gain["rate"].getString() == "control");

    auto sensitive = detail(session.dispatch(encode_control_envelope(
        {.payload = fixture.request(
             standalone, authority, "dev.pulp.state/read@1",
             R"({"include_catalog":true,"include_sensitive":true,"parameter_ids":[2]})",
             "sensitive-state")})));
    CHECK(sensitive["redacted_count"].getInt64() == 0);
    REQUIRE(sensitive["parameters"].size() == 1);
    const auto secret = sensitive["parameters"][0];
    CHECK(secret["sensitive"].getBool());
    CHECK(secret["kind"].getString() == "enum");
    CHECK(secret["designation"].getString() == "reset");
    CHECK(secret["isTrigger"].getBool());
    CHECK(secret["rate"].getString() == "audio");
    REQUIRE(secret["labels"].size() == 3);

    auto values_only = detail(session.dispatch(encode_control_envelope(
        {.payload =
             fixture.request(standalone, authority, "dev.pulp.state/read@1",
                             R"({"include_catalog":false,"parameter_ids":[1]})", "values-only")})));
    CHECK_FALSE(values_only["catalog_included"].getBool());
    REQUIRE(values_only["parameters"].size() == 1);
    CHECK_FALSE(values_only["parameters"][0].hasObjectMember("kind"));
    CHECK_FALSE(values_only["parameters"][0].hasObjectMember("rate"));

    auto explicitly_empty = detail(session.dispatch(encode_control_envelope(
        {.payload = fixture.request(standalone, authority, "dev.pulp.state/read@1",
                                    R"({"parameter_ids":[]})", "explicit-empty-filter")})));
    CHECK(explicitly_empty["parameters"].size() == 0);

    auto stale_request =
        fixture.request(standalone, authority, "dev.pulp.state/read@1", "{}", "stale-generation");
    stale_request.expected_state_generation = 41;
    stale_request.request_hash = *control_request_hash(stale_request);
    const auto stale_result =
        session.dispatch(encode_control_envelope({.payload = std::move(stale_request)}));
    const auto& stale_receipt = receipt(stale_result);
    CHECK(stale_receipt.state == ControlReceiptState::Failed);
    CHECK(stale_receipt.result_code == ControlResultCode::StateConflict);
}

TEST_CASE("Phase 4 read operations stay production-default-denied",
          "[inspect][control][read][security][production]") {
    auto production =
        read_manifest(ControlBuildProfile::ProductionStripped, "Production", "dev.pulp.production",
                      "build:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
    CHECK_FALSE(validate_control_manifest_detailed(production).valid);

    Fixture fixture;
    const auto denied = fixture.broker.register_instance(
        fixture.standalone_a_peer, {.host_tier = ControlHostTier::Standalone,
                                    .session_id = "production-session",
                                    .instance_id = "production-instance",
                                    .publication_id = "production-publication",
                                    .manifest = std::move(production),
                                    .artifact_digest = std::string(64, 'e')});
    CHECK(denied.status == ControlIdentityStatus::InvalidRequest);

    const auto bare = make_control_state_read_executor({});
    const auto outcome = bare({}, {.operation_id = "dev.pulp.state/read@1"}, {});
    CHECK(outcome.terminal_state == ControlReceiptState::Failed);
    CHECK(outcome.result.result_code == ControlResultCode::InvalidRequest);
}
