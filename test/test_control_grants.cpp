#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/control_grants.hpp>

#include <chrono>
#include <algorithm>
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
            true, authority, std::move(decision_id), {}};
    }
};

class AdvancingNthClock {
public:
    std::chrono::steady_clock::time_point operator()() {
        if (remaining_ > 0 && --remaining_ == 0)
            now_ += advance_;
        return now_;
    }

    void advance_on_call(std::size_t call, std::chrono::seconds advance) {
        remaining_ = call;
        advance_ = advance;
    }

private:
    std::chrono::steady_clock::time_point now_{};
    std::size_t remaining_ = 0;
    std::chrono::seconds advance_{};
};

ControlRegistrationRequest timed_registration_request(std::string suffix) {
    ControlManifest manifest;
    manifest.profile = ControlBuildProfile::DeveloperLocal;
    manifest.target = "TimedFixture";
    manifest.product_name = "Timed Fixture";
    manifest.bundle_id = "dev.pulp.timed-fixture";
    manifest.build_id = "build:0123456789abcdef0123456789abcdef";
    manifest.endpoint_included = true;
    manifest.capabilities = {
        InspectorCapability::SessionDescribe,
        InspectorCapability::StateRead,
    };
    return {
        ControlHostTier::Standalone,
        "session-" + suffix,
        "instance-" + suffix,
        "publication-" + suffix,
        std::move(manifest),
        std::string(64, 'c'),
    };
}

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

TEST_CASE("broker prompt consent must remain unexpired when the grant is issued",
          "[inspect][control][grants][consent]") {
    GrantFixture fixture;
    auto decision = GrantFixture::consent(ControlConsentAuthority::BrokerUserPrompt,
                                          "broker-prompt-a");
    CHECK(fixture.grants.issue(fixture.request(), decision).status ==
          ControlGrantStatus::ConsentRequired);

    decision.expires_at = fixture.now + 2s;
    REQUIRE(fixture.grants.issue(fixture.request(), decision).status ==
            ControlGrantStatus::Granted);

    fixture.now += 2s;
    decision.decision_id = "broker-prompt-b";
    CHECK(fixture.grants.issue(fixture.request(), decision).status ==
          ControlGrantStatus::ConsentRequired);

    fixture.now = {};
    unsigned clock_calls = 0;
    ControlGrantStore delayed_store{fixture.identities, fixture.audit, {}, [&] {
        if (++clock_calls == 2)
            fixture.now += 2s;
        return fixture.now;
    }};
    decision.decision_id = "broker-prompt-c";
    decision.expires_at = fixture.now + 2s;
    CHECK(delayed_store.issue(fixture.request(), decision).status ==
          ControlGrantStatus::ConsentRequired);
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

TEST_CASE("grant requests fail closed for missing identity consent scope and ttl",
          "[inspect][control][grants]") {
    GrantFixture fixture;

    auto missing_client = fixture.request();
    missing_client.client_id = ControlClientId{"missing-client"};
    CHECK(fixture.grants.issue(
              std::move(missing_client), GrantFixture::consent()).status ==
          ControlGrantStatus::ClientUnavailable);

    auto missing_registration = fixture.request();
    missing_registration.registration_id =
        ControlRegistrationId{"missing-registration"};
    CHECK(fixture.grants.issue(
              std::move(missing_registration),
              GrantFixture::consent()).status ==
          ControlGrantStatus::RegistrationUnavailable);

    CHECK(fixture.grants.issue(
              fixture.request(),
              ControlConsentDecision{false,
                                     ControlConsentAuthority::TrustedPulpCli,
                                     "denied", {}}).status ==
          ControlGrantStatus::ConsentRequired);
    CHECK(fixture.grants.issue(
              fixture.request(),
              ControlConsentDecision{true,
                                     ControlConsentAuthority::TrustedPulpCli,
                                     {}, {}}).status ==
          ControlGrantStatus::ConsentRequired);
    CHECK(fixture.grants.issue(
              fixture.request({}),
              GrantFixture::consent()).status ==
          ControlGrantStatus::CapabilityUnavailable);

    auto zero_ttl = fixture.request();
    zero_ttl.ttl = 0ms;
    CHECK(fixture.grants.issue(
              std::move(zero_ttl),
              GrantFixture::consent(ControlConsentAuthority::TrustedPulpCli,
                                    "zero-ttl")).status ==
          ControlGrantStatus::InvalidRequest);
    auto excessive_ttl = fixture.request();
    excessive_ttl.ttl = 25h;
    CHECK(fixture.grants.issue(
              std::move(excessive_ttl),
              GrantFixture::consent(ControlConsentAuthority::TrustedPulpCli,
                                    "excessive-ttl")).status ==
          ControlGrantStatus::InvalidRequest);
}

TEST_CASE("grant revocation APIs are idempotent and scope their authority",
          "[inspect][control][grants]") {
    GrantFixture fixture;
    CHECK(fixture.grants.revoke({}, {}) ==
          ControlGrantStatus::InvalidRequest);
    CHECK(fixture.grants.revoke(ControlGrantId{"missing"}, "revoke") ==
          ControlGrantStatus::NotFound);

    auto direct = fixture.grants.issue(
        fixture.request(),
        GrantFixture::consent(ControlConsentAuthority::TrustedPulpCli,
                              "direct"));
    REQUIRE(direct.grant.has_value());
    CHECK(fixture.grants.grant(direct.grant->grant_id).has_value());
    CHECK(fixture.grants.revoke(direct.grant->grant_id, "revoke") ==
          ControlGrantStatus::Revoked);
    CHECK(fixture.grants.revoke(direct.grant->grant_id, "revoke-again") ==
          ControlGrantStatus::Revoked);
    CHECK_FALSE(fixture.grants.grant(direct.grant->grant_id));

    auto by_client = fixture.grants.issue(
        fixture.request(),
        GrantFixture::consent(ControlConsentAuthority::TrustedPulpCli,
                              "by-client"));
    REQUIRE(by_client.grant.has_value());
    CHECK(fixture.grants.revoke_client({}, "decision") == 0);
    CHECK(fixture.grants.revoke_client(fixture.client.client_id,
                                       "client-revoke") == 1);
    CHECK_FALSE(fixture.grants.is_granted(
        by_client.grant->grant_id, fixture.client.client_id,
        fixture.registration.registration_id,
        InspectorCapability::StateRead));

    auto by_registration = fixture.grants.issue(
        fixture.request(),
        GrantFixture::consent(ControlConsentAuthority::TrustedPulpCli,
                              "by-registration"));
    REQUIRE(by_registration.grant.has_value());
    CHECK(fixture.grants.revoke_registration({}, "decision") == 0);
    CHECK(fixture.grants.revoke_registration(
              fixture.registration.registration_id,
              "registration-revoke") == 1);
    CHECK_FALSE(fixture.grants.grant(by_registration.grant->grant_id));
}

TEST_CASE("interactive consent replay history is bounded",
          "[inspect][control][grants]") {
    GrantFixture fixture;
    ControlGrantStore bounded(
        fixture.identities, fixture.audit,
        ControlGrantStoreConfig{4, 1, 1h},
        [&] { return fixture.now; });
    REQUIRE(bounded.issue(
                fixture.request(),
                GrantFixture::consent(ControlConsentAuthority::TrustedHostUi,
                                      "first-decision")).status ==
            ControlGrantStatus::Granted);
    CHECK(bounded.issue(
              fixture.request(),
              GrantFixture::consent(ControlConsentAuthority::TrustedHostUi,
                                    "second-decision")).status ==
          ControlGrantStatus::ResourceExhausted);
}

TEST_CASE("grant status identifiers are stable",
          "[inspect][control][grants]") {
    CHECK(control_grant_status_id(ControlGrantStatus::Granted) == "granted");
    CHECK(control_grant_status_id(ControlGrantStatus::InvalidRequest) ==
          "invalid-request");
    CHECK(control_grant_status_id(ControlGrantStatus::BrokerMismatch) ==
          "broker-mismatch");
    CHECK(control_grant_status_id(ControlGrantStatus::ClientUnavailable) ==
          "client-unavailable");
    CHECK(control_grant_status_id(
              ControlGrantStatus::RegistrationUnavailable) ==
          "registration-unavailable");
    CHECK(control_grant_status_id(ControlGrantStatus::ConsentRequired) ==
          "consent-required");
    CHECK(control_grant_status_id(ControlGrantStatus::ConsentReplay) ==
          "consent-replay");
    CHECK(control_grant_status_id(
              ControlGrantStatus::CapabilityUnavailable) ==
          "capability-unavailable");
    CHECK(control_grant_status_id(ControlGrantStatus::ResourceExhausted) ==
          "resource-exhausted");
    CHECK(control_grant_status_id(ControlGrantStatus::NotFound) ==
          "not-found");
    CHECK(control_grant_status_id(ControlGrantStatus::Expired) == "expired");
    CHECK(control_grant_status_id(ControlGrantStatus::Revoked) == "revoked");
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
                                     "sensitive-user-text", {}}).status ==
          ControlGrantStatus::ResourceExhausted);

    const auto events = fixture.audit->snapshot();
    REQUIRE_FALSE(events.empty());
    for (const auto& event : events) {
        CHECK(event.reason.find("sensitive-user-text") == std::string::npos);
        CHECK(event.reason.find("manifest-a") == std::string::npos);
    }
}

TEST_CASE("direct revoke frees active capacity and retains bounded idempotence",
          "[inspect][control][grants][capacity]") {
    GrantFixture fixture;
    ControlGrantStore bounded(
        fixture.identities, fixture.audit,
        ControlGrantStoreConfig{1, 4, 1h, 2},
        [&] { return fixture.now; });
    auto first = bounded.issue(
        fixture.request(),
        GrantFixture::consent(ControlConsentAuthority::TrustedPulpCli,
                              "capacity-first"));
    REQUIRE(first.grant.has_value());
    CHECK(bounded.revoke(first.grant->grant_id, "capacity-revoke") ==
          ControlGrantStatus::Revoked);
    CHECK(bounded.revoke(first.grant->grant_id, "capacity-revoke-again") ==
          ControlGrantStatus::Revoked);

    auto replacement = bounded.issue(
        fixture.request(),
        GrantFixture::consent(ControlConsentAuthority::TrustedPulpCli,
                              "capacity-second"));
    CHECK(replacement.status == ControlGrantStatus::Granted);
    CHECK(replacement.grant.has_value());
}

TEST_CASE("zero retired grant capacity disables revoke tombstones exactly",
          "[inspect][control][grants][capacity]") {
    GrantFixture fixture;
    ControlGrantStore bounded(
        fixture.identities, fixture.audit,
        ControlGrantStoreConfig{1, 4, 1h, 0},
        [&] { return fixture.now; });
    auto first = bounded.issue(
        fixture.request(),
        GrantFixture::consent(ControlConsentAuthority::TrustedPulpCli,
                              "zero-retired-first"));
    REQUIRE(first.grant.has_value());
    CHECK(bounded.revoke(first.grant->grant_id, "zero-retired-revoke") ==
          ControlGrantStatus::Revoked);
    CHECK(bounded.revoke(first.grant->grant_id,
                         "zero-retired-revoke-again") ==
          ControlGrantStatus::NotFound);

    auto replacement = bounded.issue(
        fixture.request(),
        GrantFixture::consent(ControlConsentAuthority::TrustedPulpCli,
                              "zero-retired-second"));
    CHECK(replacement.status == ControlGrantStatus::Granted);
    CHECK(replacement.grant.has_value());
}

TEST_CASE("grant insertion time rejects identities that expired after lookup",
          "[inspect][control][grants][lifecycle][time]") {
    SECTION("client expiry does not consume consent or active capacity") {
        AdvancingNthClock clock;
        auto audit = std::make_shared<ControlSecurityAuditLog>();
        ControlIdentityRegistryConfig identity_config;
        identity_config.client_ttl = 1s;
        identity_config.registration_ttl = 10s;
        ControlIdentityRegistry identities{
            identity_config, audit, [&] { return clock(); }};
        ControlGrantStore grants{
            identities, audit, ControlGrantStoreConfig{1, 4, 1h, 4},
            [&] { return clock(); }};
        auto client_peer = verified_peer(
            ControlPeerRole::Client, 501, "timed-client");
        auto host_peer = verified_peer(
            ControlPeerRole::StandaloneHost, 502, "timed-host");

        auto ticket = identities.issue_bootstrap(client_peer);
        REQUIRE(ticket.ticket.has_value());
        auto connected = identities.redeem_bootstrap(
            ticket.ticket->ticket_id, ticket.ticket->secret.bytes(),
            client_peer);
        REQUIRE(connected.client.has_value());
        auto registered = identities.register_instance(
            host_peer, timed_registration_request("client-expiry"));
        REQUIRE(registered.registration.has_value());

        ControlGrantRequest request{
            connected.client->client_id,
            registered.registration->registration_id,
            {InspectorCapability::StateRead},
            5min,
        };
        const ControlConsentDecision consent{
            true, ControlConsentAuthority::TrustedPulpCli,
            "timed-consent-client", {}};
        clock.advance_on_call(3, 2s);
        auto rejected = grants.issue(request, consent);
        CHECK(rejected.status == ControlGrantStatus::ClientUnavailable);
        CHECK_FALSE(rejected.grant.has_value());

        const auto rejected_events = audit->snapshot();
        CHECK(std::ranges::any_of(rejected_events, [](const auto& event) {
            return event.action == "grant.issue" &&
                   event.outcome == ControlSecurityOutcome::Denied &&
                   event.reason == "client-unavailable" &&
                   event.grant_id.empty();
        }));
        CHECK_FALSE(std::ranges::any_of(
            rejected_events, [](const auto& event) {
                return event.action == "grant.issue" &&
                       event.outcome == ControlSecurityOutcome::Accepted;
            }));

        ticket = identities.issue_bootstrap(client_peer);
        REQUIRE(ticket.ticket.has_value());
        connected = identities.redeem_bootstrap(
            ticket.ticket->ticket_id, ticket.ticket->secret.bytes(),
            client_peer);
        REQUIRE(connected.client.has_value());
        request.client_id = connected.client->client_id;
        auto replacement = grants.issue(request, consent);
        CHECK(replacement.status == ControlGrantStatus::Granted);
        CHECK(replacement.grant.has_value());
    }

    SECTION("registration expiry does not consume consent or active capacity") {
        AdvancingNthClock clock;
        auto audit = std::make_shared<ControlSecurityAuditLog>();
        ControlIdentityRegistryConfig identity_config;
        identity_config.client_ttl = 10s;
        identity_config.registration_ttl = 1s;
        ControlIdentityRegistry identities{
            identity_config, audit, [&] { return clock(); }};
        ControlGrantStore grants{
            identities, audit, ControlGrantStoreConfig{1, 4, 1h, 4},
            [&] { return clock(); }};
        auto client_peer = verified_peer(
            ControlPeerRole::Client, 511, "timed-client-registration");
        auto host_peer = verified_peer(
            ControlPeerRole::StandaloneHost, 512,
            "timed-host-registration");

        auto ticket = identities.issue_bootstrap(client_peer);
        REQUIRE(ticket.ticket.has_value());
        auto connected = identities.redeem_bootstrap(
            ticket.ticket->ticket_id, ticket.ticket->secret.bytes(),
            client_peer);
        REQUIRE(connected.client.has_value());
        auto registered = identities.register_instance(
            host_peer, timed_registration_request("registration-expiry"));
        REQUIRE(registered.registration.has_value());

        ControlGrantRequest request{
            connected.client->client_id,
            registered.registration->registration_id,
            {InspectorCapability::StateRead},
            5min,
        };
        const ControlConsentDecision consent{
            true, ControlConsentAuthority::TrustedPulpCli,
            "timed-consent-registration", {}};
        clock.advance_on_call(3, 2s);
        auto rejected = grants.issue(request, consent);
        CHECK(rejected.status == ControlGrantStatus::RegistrationUnavailable);
        CHECK_FALSE(rejected.grant.has_value());

        const auto rejected_events = audit->snapshot();
        CHECK(std::ranges::any_of(rejected_events, [](const auto& event) {
            return event.action == "grant.issue" &&
                   event.outcome == ControlSecurityOutcome::Denied &&
                   event.reason == "registration-unavailable" &&
                   event.grant_id.empty();
        }));
        CHECK_FALSE(std::ranges::any_of(
            rejected_events, [](const auto& event) {
                return event.action == "grant.issue" &&
                       event.outcome == ControlSecurityOutcome::Accepted;
            }));

        registered = identities.register_instance(
            host_peer, timed_registration_request("registration-replacement"));
        REQUIRE(registered.registration.has_value());
        request.registration_id = registered.registration->registration_id;
        auto replacement = grants.issue(request, consent);
        CHECK(replacement.status == ControlGrantStatus::Granted);
        CHECK(replacement.grant.has_value());
    }
}
