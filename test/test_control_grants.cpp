#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/control_grants.hpp>

#include <chrono>
#include <memory>
#include <string>

using namespace pulp::inspect;
using namespace std::chrono_literals;

namespace {

VerifiedControlPeerIdentity verified_peer(ControlPeerRole role,
                                          std::int64_t process_id,
                                          std::string start_id) {
    ControlPeerVerifier verifier([](const ControlPeerEvidence&) {
        return true;
    });
    auto peer = verifier.verify(ControlPeerEvidence{
        role, "uid:501", process_id, std::move(start_id),
        "signed:dev.pulp.fixture", "publisher.pulp"});
    REQUIRE(peer.has_value());
    return std::move(*peer);
}

struct GrantFixture {
    std::chrono::steady_clock::time_point now{};
    std::shared_ptr<ControlSecurityAuditLog> audit =
        std::make_shared<ControlSecurityAuditLog>();
    ControlIdentityRegistry identities{
        {}, audit, [&] { return now; }};
    ControlGrantStore grants{
        identities, audit, {}, [&] { return now; }};
    VerifiedControlPeerIdentity client_peer =
        verified_peer(ControlPeerRole::Client, 100, "client-start");
    VerifiedControlPeerIdentity host_peer =
        verified_peer(ControlPeerRole::StandaloneHost, 101, "host-start");
    ControlClientIdentity client;
    ControlRegistration registration;

    GrantFixture() {
        auto ticket = identities.issue_bootstrap(client_peer);
        REQUIRE(ticket.ticket.has_value());
        auto connected = identities.redeem_bootstrap(
            ticket.ticket->ticket_id, ticket.ticket->secret.bytes(),
            client_peer);
        REQUIRE(connected.client.has_value());
        client = *connected.client;

        auto registered = identities.register_instance(
            host_peer,
            [] {
                ControlManifest manifest;
                manifest.profile = ControlBuildProfile::ResearchUnsafe;
                manifest.target = "Fixture";
                manifest.product_name = "Fixture";
                manifest.bundle_id = "dev.pulp.fixture";
                manifest.build_id =
                    "build:0123456789abcdef0123456789abcdef";
                manifest.endpoint_included = true;
                manifest.unsafe_runtime_eval_acknowledged = true;
                manifest.capabilities = {
                    InspectorCapability::SessionDescribe,
                    InspectorCapability::SessionControl,
                    InspectorCapability::StateRead,
                    InspectorCapability::RuntimeEval,
                };
                return ControlRegistrationRequest{
                    ControlHostTier::Standalone,
                    "session-a",
                    "instance-a",
                    "publication-a",
                    std::move(manifest),
                    std::string(64, 'b'),
                };
            }());
        REQUIRE(registered.registration.has_value());
        registration = *registered.registration;
    }

    ControlGrantRequest request(
        std::vector<InspectorCapability> capabilities = {
            InspectorCapability::StateRead}) const {
        return ControlGrantRequest{
            client.client_id,
            registration.registration_id,
            std::move(capabilities),
            15min,
        };
    }

    static ControlConsentDecision consent(
        ControlConsentAuthority authority =
            ControlConsentAuthority::TrustedPulpCli,
        std::string decision_id = "decision-a") {
        return ControlConsentDecision{
            true, authority, std::move(decision_id)};
    }
};

} // namespace

TEST_CASE("only trusted user consent can issue an exact scoped grant",
          "[inspect][control][grants]") {
    GrantFixture fixture;
    CHECK(fixture.grants.issue(
              fixture.request(),
              GrantFixture::consent(ControlConsentAuthority::PluginUi)).status ==
          ControlGrantStatus::ConsentRequired);
    CHECK(fixture.grants.issue(
              fixture.request(),
              GrantFixture::consent(ControlConsentAuthority::AgentClient)).status ==
          ControlGrantStatus::ConsentRequired);

    auto issued = fixture.grants.issue(
        fixture.request(), GrantFixture::consent());
    REQUIRE(issued.status == ControlGrantStatus::Granted);
    REQUIRE(issued.grant.has_value());
    CHECK(fixture.grants.is_granted(
        issued.grant->grant_id,
        fixture.client.client_id,
        fixture.registration.registration_id,
        InspectorCapability::StateRead));
    CHECK_FALSE(fixture.grants.is_granted(
        issued.grant->grant_id,
        fixture.client.client_id,
        fixture.registration.registration_id,
        InspectorCapability::RuntimeEval));
    CHECK_FALSE(fixture.grants.is_granted(
        issued.grant->grant_id,
        ControlClientId{"client-forged"},
        fixture.registration.registration_id,
        InspectorCapability::StateRead));
}

TEST_CASE("interactive consent decisions are single-use while policy is reusable",
          "[inspect][control][grants]") {
    GrantFixture fixture;
    REQUIRE(fixture.grants.issue(
                fixture.request(),
                GrantFixture::consent(ControlConsentAuthority::TrustedPulpCli,
                                      "interactive-a")).status ==
            ControlGrantStatus::Granted);
    CHECK(fixture.grants.issue(
              fixture.request(),
              GrantFixture::consent(ControlConsentAuthority::TrustedPulpCli,
                                    "interactive-a")).status ==
          ControlGrantStatus::ConsentReplay);

    REQUIRE(fixture.grants.issue(
                fixture.request(),
                GrantFixture::consent(ControlConsentAuthority::ExistingUserPolicy,
                                      "policy-a")).status ==
            ControlGrantStatus::Granted);
    CHECK(fixture.grants.issue(
              fixture.request(),
              GrantFixture::consent(ControlConsentAuthority::ExistingUserPolicy,
                                    "policy-a")).status ==
          ControlGrantStatus::Granted);
}

TEST_CASE("grant issue rejects unavailable and non-grantable capabilities",
          "[inspect][control][grants]") {
    GrantFixture fixture;
    CHECK(fixture.grants.issue(
              fixture.request({InspectorCapability::RenderOffline}),
              GrantFixture::consent()).status ==
          ControlGrantStatus::CapabilityUnavailable);
    CHECK(fixture.grants.issue(
              fixture.request({InspectorCapability::UiInput}),
              GrantFixture::consent()).status ==
          ControlGrantStatus::CapabilityUnavailable);
    CHECK(fixture.grants.issue(
              fixture.request({InspectorCapability::StateRead,
                               InspectorCapability::StateRead}),
              GrantFixture::consent()).status ==
          ControlGrantStatus::CapabilityUnavailable);
}

TEST_CASE("grant expiry revocation and disconnect remove authority",
          "[inspect][control][grants]") {
    GrantFixture fixture;
    auto request = fixture.request();
    request.ttl = 5s;
    auto expiring = fixture.grants.issue(
        std::move(request),
        GrantFixture::consent(ControlConsentAuthority::TrustedPulpCli,
                              "decision-expiring"));
    REQUIRE(expiring.grant.has_value());
    fixture.now += 6s;
    CHECK_FALSE(fixture.grants.is_granted(
        expiring.grant->grant_id,
        fixture.client.client_id,
        fixture.registration.registration_id,
        InspectorCapability::StateRead));
    fixture.grants.sweep_expired();

    auto revoked = fixture.grants.issue(
        fixture.request(),
        GrantFixture::consent(ControlConsentAuthority::TrustedPulpCli,
                              "decision-revoked"));
    REQUIRE(revoked.grant.has_value());
    CHECK(fixture.grants.revoke(
              revoked.grant->grant_id, "user-revoke") ==
          ControlGrantStatus::Revoked);
    CHECK_FALSE(fixture.grants.is_granted(
        revoked.grant->grant_id,
        fixture.client.client_id,
        fixture.registration.registration_id,
        InspectorCapability::StateRead));

    auto disconnected = fixture.grants.issue(
        fixture.request(),
        GrantFixture::consent(ControlConsentAuthority::TrustedPulpCli,
                              "decision-disconnected"));
    REQUIRE(disconnected.grant.has_value());
    REQUIRE(fixture.identities.disconnect_client(
        fixture.client.client_id));
    CHECK_FALSE(fixture.grants.is_granted(
        disconnected.grant->grant_id,
        fixture.client.client_id,
        fixture.registration.registration_id,
        InspectorCapability::StateRead));
}

TEST_CASE("registration replacement and broker restart cannot inherit grants",
          "[inspect][control][grants]") {
    GrantFixture fixture;
    auto issued = fixture.grants.issue(
        fixture.request(), GrantFixture::consent());
    REQUIRE(issued.grant.has_value());
    REQUIRE(fixture.identities.unregister_instance(
        fixture.registration.registration_id, fixture.host_peer));
    CHECK_FALSE(fixture.grants.is_granted(
        issued.grant->grant_id,
        fixture.client.client_id,
        fixture.registration.registration_id,
        InspectorCapability::StateRead));

    ControlIdentityRegistry restarted;
    ControlGrantStore restarted_grants(restarted, {});
    CHECK_FALSE(restarted.broker_id() == fixture.identities.broker_id());
    CHECK_FALSE(restarted_grants.grant(issued.grant->grant_id));
}

TEST_CASE("grant store is bounded and audit contains no consent payload",
          "[inspect][control][grants]") {
    GrantFixture fixture;
    ControlGrantStore bounded(
        fixture.identities, fixture.audit,
        ControlGrantStoreConfig{1, 4, 1h},
        [&] { return fixture.now; });
    auto first = bounded.issue(
        fixture.request(), GrantFixture::consent());
    REQUIRE(first.grant.has_value());
    CHECK(bounded.issue(
              fixture.request(),
              ControlConsentDecision{true,
                                     ControlConsentAuthority::TrustedPulpCli,
                                     "sensitive-user-text"}).status ==
          ControlGrantStatus::ResourceExhausted);

    const auto events = fixture.audit->snapshot();
    REQUIRE_FALSE(events.empty());
    for (const auto& event : events) {
        CHECK(event.reason.find("sensitive-user-text") == std::string::npos);
        CHECK(event.reason.find("manifest-a") == std::string::npos);
    }
}
