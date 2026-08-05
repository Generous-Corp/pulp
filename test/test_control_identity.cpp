#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/control_identity.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

using namespace pulp::inspect;
using namespace std::chrono_literals;

namespace {

ControlPeerVerifier verifier() {
    return ControlPeerVerifier([](const ControlPeerEvidence& evidence) {
        return evidence.user_id == "uid:501" &&
               evidence.executable_identity.starts_with("signed:");
    });
}

VerifiedControlPeerIdentity peer(
    ControlPeerRole role,
    std::int64_t process_id,
    std::string process_start_id,
    std::string publisher = "publisher.pulp") {
    auto verified = verifier().verify(ControlPeerEvidence{
        role,
        "uid:501",
        process_id,
        std::move(process_start_id),
        "signed:dev.pulp.fixture",
        std::move(publisher),
    });
    REQUIRE(verified.has_value());
    return std::move(*verified);
}

ControlRegistrationRequest registration_request(
    ControlHostTier tier = ControlHostTier::Standalone,
    std::string publication = "publication-a") {
    ControlManifest manifest;
    manifest.profile = ControlBuildProfile::DeveloperLocal;
    manifest.target = "Fixture";
    manifest.product_name = "Fixture";
    manifest.bundle_id = "dev.pulp.fixture";
    manifest.build_id = "build:0123456789abcdef0123456789abcdef";
    manifest.endpoint_included = true;
    manifest.capabilities = {
        InspectorCapability::SessionDescribe,
        InspectorCapability::StateRead,
    };
    return ControlRegistrationRequest{
        tier,
        "session-a",
        "instance-a",
        std::move(publication),
        std::move(manifest),
        std::string(64, 'a'),
    };
}

} // namespace

TEST_CASE("control peers require complete carrier-verified process evidence",
          "[inspect][control][identity]") {
    auto authority = verifier();
    CHECK_FALSE(authority.verify(ControlPeerEvidence{}));
    CHECK_FALSE(authority.verify(ControlPeerEvidence{
        ControlPeerRole::Client, "uid:502", 10, "start-a",
        "signed:dev.pulp.client", "publisher.pulp"}));

    const auto first = peer(ControlPeerRole::Client, 10, "start-a");
    const auto same = peer(ControlPeerRole::Client, 10, "start-a");
    const auto reused_pid = peer(ControlPeerRole::Client, 10, "start-b");
    CHECK(first == same);
    CHECK_FALSE(first == reused_pid);
}

TEST_CASE("bootstrap is single-use and bound to the verified peer generation",
          "[inspect][control][identity]") {
    auto now = std::chrono::steady_clock::time_point{};
    auto audit = std::make_shared<ControlSecurityAuditLog>();
    ControlIdentityRegistry identities(
        {}, audit, [&] { return now; });
    const auto intended = peer(ControlPeerRole::Client, 20, "start-a");
    const auto same_uid_rogue =
        peer(ControlPeerRole::Client, 21, "rogue-start");
    REQUIRE(same_uid_rogue.evidence().user_id ==
            intended.evidence().user_id);

    auto copied = identities.issue_bootstrap(intended);
    REQUIRE(copied.status == ControlIdentityStatus::Accepted);
    REQUIRE(copied.ticket.has_value());
    std::vector<std::uint8_t> stolen(
        copied.ticket->secret.bytes().begin(),
        copied.ticket->secret.bytes().end());
    const auto wrong_peer = identities.redeem_bootstrap(
        copied.ticket->ticket_id, stolen, same_uid_rogue);
    CHECK(wrong_peer.status == ControlIdentityStatus::IdentityMismatch);
    CHECK(identities.redeem_bootstrap(
              copied.ticket->ticket_id,
              copied.ticket->secret.bytes(), intended).status ==
          ControlIdentityStatus::Replay);

    auto valid = identities.issue_bootstrap(intended);
    REQUIRE(valid.ticket.has_value());
    const auto client = identities.redeem_bootstrap(
        valid.ticket->ticket_id, valid.ticket->secret.bytes(), intended);
    REQUIRE(client.status == ControlIdentityStatus::Accepted);
    REQUIRE(client.client.has_value());
    CHECK(identities.client(client.client->client_id).has_value());
    CHECK(identities.redeem_bootstrap(
              valid.ticket->ticket_id,
              valid.ticket->secret.bytes(), intended).status ==
          ControlIdentityStatus::Replay);

    const auto events = audit->snapshot();
    CHECK(std::ranges::any_of(events, [](const auto& event) {
        return event.action == "bootstrap.redeem" &&
               event.reason == "identity-mismatch";
    }));
    for (const auto& event : events) {
        CHECK(event.reason.find("bootstrap-") == std::string::npos);
        CHECK(event.reason.find("signed:") == std::string::npos);
    }
}

TEST_CASE("bootstrap and client expiry fail closed across broker restart",
          "[inspect][control][identity]") {
    auto now = std::chrono::steady_clock::time_point{};
    ControlIdentityRegistryConfig config;
    config.bootstrap_ttl = 5s;
    config.client_ttl = 10s;
    const auto client_peer = peer(ControlPeerRole::Client, 30, "start-a");
    ControlClientId old_client;
    ControlBrokerId old_broker;

    {
        ControlIdentityRegistry first(config, {}, [&] { return now; });
        old_broker = first.broker_id();
        auto expired = first.issue_bootstrap(client_peer);
        REQUIRE(expired.ticket.has_value());
        now += 6s;
        CHECK(first.redeem_bootstrap(
                  expired.ticket->ticket_id,
                  expired.ticket->secret.bytes(), client_peer).status ==
              ControlIdentityStatus::Expired);

        auto valid = first.issue_bootstrap(client_peer);
        REQUIRE(valid.ticket.has_value());
        auto connected = first.redeem_bootstrap(
            valid.ticket->ticket_id, valid.ticket->secret.bytes(), client_peer);
        REQUIRE(connected.client.has_value());
        old_client = connected.client->client_id;
        now += 11s;
        CHECK_FALSE(first.client(old_client));
    }

    ControlIdentityRegistry restarted(config, {}, [&] { return now; });
    CHECK_FALSE(restarted.broker_id() == old_broker);
    CHECK_FALSE(restarted.client(old_client));
}

TEST_CASE("registration is exact, tier-limited, expiring, and peer-bound",
          "[inspect][control][identity]") {
    auto now = std::chrono::steady_clock::time_point{};
    ControlIdentityRegistryConfig config;
    config.registration_ttl = 10s;
    ControlIdentityRegistry identities(config, {}, [&] { return now; });
    const auto standalone =
        peer(ControlPeerRole::StandaloneHost, 40, "start-a");
    const auto reused_pid =
        peer(ControlPeerRole::StandaloneHost, 40, "start-b");
    const auto shared_host =
        peer(ControlPeerRole::TrustedHostBridge, 41, "start-c");

    auto registered = identities.register_instance(
        standalone, registration_request());
    REQUIRE(registered.status == ControlIdentityStatus::Accepted);
    REQUIRE(registered.registration.has_value());
    const auto id = registered.registration->registration_id;

    CHECK(identities.registration(
              "session-a", "instance-a", "publication-a").has_value());
    CHECK_FALSE(identities.registration("", "", ""));
    CHECK_FALSE(identities.heartbeat(id, reused_pid));
    CHECK(identities.heartbeat(id, standalone));

    auto shared = registration_request(ControlHostTier::SharedPluginHost,
                                       "publication-shared");
    CHECK(identities.register_instance(shared_host, std::move(shared)).status ==
          ControlIdentityStatus::HostUnavailable);

    now += 11s;
    CHECK_FALSE(identities.registration(id));
    CHECK_FALSE(identities.heartbeat(id, standalone));
}

TEST_CASE("registration rejects forged manifests and artifact identities",
          "[inspect][control][identity]") {
    ControlIdentityRegistry identities;
    const auto host =
        peer(ControlPeerRole::StandaloneHost, 45, "start-a");

    auto forged_manifest = registration_request();
    forged_manifest.manifest.registry_digest = std::string(64, '0');
    CHECK(identities.register_instance(
              host, std::move(forged_manifest)).status ==
          ControlIdentityStatus::InvalidRequest);

    auto forged_artifact = registration_request();
    forged_artifact.artifact_digest = "not-a-digest";
    CHECK(identities.register_instance(
              host, std::move(forged_artifact)).status ==
          ControlIdentityStatus::InvalidRequest);
}

TEST_CASE("client and registration lifecycle require the owning peer",
          "[inspect][control][identity]") {
    auto audit = std::make_shared<ControlSecurityAuditLog>();
    ControlIdentityRegistry identities({}, audit);
    const auto client_peer = peer(ControlPeerRole::Client, 46, "client-start");
    const auto rogue_peer = peer(ControlPeerRole::Client, 47, "rogue-start");
    auto ticket = identities.issue_bootstrap(client_peer);
    REQUIRE(ticket.ticket.has_value());
    auto connected = identities.redeem_bootstrap(
        ticket.ticket->ticket_id, ticket.ticket->secret.bytes(), client_peer);
    REQUIRE(connected.client.has_value());

    CHECK_FALSE(identities.refresh_client(
        connected.client->client_id, rogue_peer));
    CHECK(identities.refresh_client(connected.client->client_id, client_peer));
    CHECK(identities.disconnect_client(connected.client->client_id));
    CHECK_FALSE(identities.disconnect_client(connected.client->client_id));

    const auto host = peer(ControlPeerRole::StandaloneHost, 48, "host-start");
    const auto other_host =
        peer(ControlPeerRole::StandaloneHost, 49, "other-host-start");
    auto registered = identities.register_instance(
        host, registration_request());
    REQUIRE(registered.registration.has_value());
    CHECK_FALSE(identities.unregister_instance(
        registered.registration->registration_id, other_host));
    CHECK(identities.unregister_instance(
        registered.registration->registration_id, host));
    CHECK_FALSE(identities.registration(
        "session-a", "instance-a", "publication-a"));

    const auto events = audit->snapshot();
    CHECK(std::ranges::any_of(events, [](const auto& event) {
        return event.action == "client.disconnect" &&
               event.outcome == ControlSecurityOutcome::Revoked;
    }));
    CHECK(std::ranges::any_of(events, [](const auto& event) {
        return event.action == "registration.unregister" &&
               event.outcome == ControlSecurityOutcome::Denied;
    }));
}

TEST_CASE("registration rejects peer roles and duplicate exact publications",
          "[inspect][control][identity]") {
    ControlIdentityRegistry identities;
    const auto client = peer(ControlPeerRole::Client, 54, "client-start");
    CHECK(identities.register_instance(
              client, registration_request()).status ==
          ControlIdentityStatus::PeerRoleMismatch);

    const auto host = peer(ControlPeerRole::StandaloneHost, 55, "host-start");
    REQUIRE(identities.register_instance(
                host, registration_request()).registration.has_value());
    CHECK(identities.register_instance(
              host, registration_request()).status ==
          ControlIdentityStatus::IdentityMismatch);
}

TEST_CASE("identity status identifiers are stable",
          "[inspect][control][identity]") {
    CHECK(control_identity_status_id(ControlIdentityStatus::Accepted) ==
          "accepted");
    CHECK(control_identity_status_id(ControlIdentityStatus::InvalidRequest) ==
          "invalid-request");
    CHECK(control_identity_status_id(ControlIdentityStatus::PeerRoleMismatch) ==
          "peer-role-mismatch");
    CHECK(control_identity_status_id(ControlIdentityStatus::HostUnavailable) ==
          "host-unavailable");
    CHECK(control_identity_status_id(
              ControlIdentityStatus::AttestationUnavailable) ==
          "attestation-unavailable");
    CHECK(control_identity_status_id(ControlIdentityStatus::IdentityMismatch) ==
          "identity-mismatch");
    CHECK(control_identity_status_id(ControlIdentityStatus::NotFound) ==
          "not-found");
    CHECK(control_identity_status_id(ControlIdentityStatus::Expired) ==
          "expired");
    CHECK(control_identity_status_id(ControlIdentityStatus::Replay) ==
          "replay");
    CHECK(control_identity_status_id(ControlIdentityStatus::ResourceExhausted) ==
          "resource-exhausted");
    CHECK(control_identity_status_id(ControlIdentityStatus::EntropyUnavailable) ==
          "entropy-unavailable");
}

TEST_CASE("identity registry bounds clients registrations and challenges",
          "[inspect][control][identity]") {
    ControlIdentityRegistryConfig config;
    config.max_bootstrap_tickets = 1;
    config.max_clients = 1;
    config.max_registrations = 1;
    ControlIdentityRegistry identities(config);
    const auto first_client = peer(ControlPeerRole::Client, 50, "start-a");
    const auto second_client = peer(ControlPeerRole::Client, 51, "start-b");

    auto first_ticket = identities.issue_bootstrap(first_client);
    REQUIRE(first_ticket.ticket.has_value());
    CHECK(identities.issue_bootstrap(second_client).status ==
          ControlIdentityStatus::ResourceExhausted);
    auto first_connected = identities.redeem_bootstrap(
        first_ticket.ticket->ticket_id,
        first_ticket.ticket->secret.bytes(), first_client);
    REQUIRE(first_connected.client.has_value());
    auto second_ticket = identities.issue_bootstrap(second_client);
    REQUIRE(second_ticket.ticket.has_value());
    CHECK(identities.redeem_bootstrap(
              second_ticket.ticket->ticket_id,
              second_ticket.ticket->secret.bytes(), second_client).status ==
          ControlIdentityStatus::ResourceExhausted);

    const auto first_host =
        peer(ControlPeerRole::StandaloneHost, 52, "start-c");
    const auto second_host =
        peer(ControlPeerRole::StandaloneHost, 53, "start-d");
    REQUIRE(identities.register_instance(
                first_host, registration_request()).registration.has_value());
    CHECK(identities.register_instance(
              second_host,
              registration_request(ControlHostTier::Standalone,
                                   "publication-b")).status ==
          ControlIdentityStatus::ResourceExhausted);
}
