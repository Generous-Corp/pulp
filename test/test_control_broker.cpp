#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/control_broker.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

using namespace pulp::inspect;
using namespace std::chrono_literals;

namespace {

std::optional<VerifiedControlPeerIdentity> verify_peer(
    ControlPeerRole role,
    std::int64_t process_id,
    std::string start_id,
    bool authority_accepts = true) {
    ControlPeerVerifier verifier(
        [authority_accepts](const ControlPeerEvidence&) {
            return authority_accepts;
        });
    return verifier.verify(ControlPeerEvidence{
        role,
        "uid:501",
        process_id,
        std::move(start_id),
        "signed:dev.pulp.fixture",
        "publisher.pulp",
    });
}

ControlRegistrationRequest registration_request(
    ControlHostTier tier,
    std::string suffix) {
    ControlManifest manifest;
    manifest.profile = ControlBuildProfile::DeveloperLocal;
    manifest.target = "BrokerFixture";
    manifest.product_name = "Broker Fixture";
    manifest.bundle_id = "dev.pulp.broker-fixture";
    manifest.build_id = "build:0123456789abcdef0123456789abcdef";
    manifest.endpoint_included = true;
    manifest.capabilities = {
        InspectorCapability::SessionDescribe,
        InspectorCapability::StateRead,
    };
    return {
        tier,
        "session-" + suffix,
        "instance-" + suffix,
        "publication-" + suffix,
        std::move(manifest),
        std::string(64, 'a'),
    };
}

struct BrokerFixture {
    std::chrono::steady_clock::time_point now{};
    std::shared_ptr<ControlSecurityAuditLog> audit =
        std::make_shared<ControlSecurityAuditLog>();
    ControlBroker broker{{}, audit, [&] { return now; }};
    VerifiedControlPeerIdentity client =
        std::move(*verify_peer(ControlPeerRole::Client, 101, "client-start"));
    VerifiedControlPeerIdentity rogue_client =
        std::move(*verify_peer(ControlPeerRole::Client, 102, "rogue-start"));
    VerifiedControlPeerIdentity standalone = std::move(
        *verify_peer(ControlPeerRole::StandaloneHost, 201, "standalone-start"));
    VerifiedControlPeerIdentity offline = std::move(
        *verify_peer(ControlPeerRole::OfflineHost, 202, "offline-start"));

    ControlClientIdentity connect_client() {
        auto ticket = broker.issue_bootstrap(client);
        REQUIRE(ticket.ticket.has_value());
        auto connected = broker.redeem_bootstrap(
            ticket.ticket->ticket_id,
            ticket.ticket->secret.bytes(),
            client);
        REQUIRE(connected.client.has_value());
        return *connected.client;
    }

    ControlRegistration register_standalone() {
        auto result = broker.register_instance(
            standalone,
            registration_request(ControlHostTier::Standalone, "standalone"));
        REQUIRE(result.registration.has_value());
        return *result.registration;
    }
};

ControlConsentDecision consent(std::string id) {
    return {
        true,
        ControlConsentAuthority::TrustedPulpCli,
        std::move(id),
    };
}

bool has_denial(
    const std::vector<ControlSecurityAuditEntry>& events,
    std::string_view action,
    std::string_view reason,
    std::string_view peer_fingerprint,
    std::string_view client_id = {},
    std::string_view registration_id = {}) {
    return std::ranges::any_of(events, [&](const auto& event) {
        return event.action == action && event.reason == reason &&
               event.outcome == ControlSecurityOutcome::Denied &&
               event.peer_fingerprint == peer_fingerprint &&
               event.client_id == client_id &&
               event.registration_id == registration_id &&
               !event.broker_id.empty();
    });
}

class BlockingNthClock {
public:
    std::chrono::steady_clock::time_point operator()() {
        std::unique_lock lock(mutex_);
        ++calls_;
        if (target_call_ != 0 && calls_ == target_call_) {
            blocked_ = true;
            condition_.notify_all();
            condition_.wait(lock, [&] { return released_; });
        }
        return now_;
    }

    void block_after(std::size_t additional_calls) {
        std::lock_guard lock(mutex_);
        target_call_ = calls_ + additional_calls;
        blocked_ = false;
        released_ = false;
    }

    bool wait_until_blocked() {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, 2s, [&] { return blocked_; });
    }

    void release() {
        std::lock_guard lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }

    void advance(std::chrono::steady_clock::duration duration) {
        std::lock_guard lock(mutex_);
        now_ += duration;
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::chrono::steady_clock::time_point now_{};
    std::size_t calls_ = 0;
    std::size_t target_call_ = 0;
    bool blocked_ = false;
    bool released_ = false;
};

class OperationState {
public:
    void mark_started() {
        std::lock_guard lock(mutex_);
        started_ = true;
        condition_.notify_all();
    }

    void mark_finished() {
        std::lock_guard lock(mutex_);
        finished_ = true;
        condition_.notify_all();
    }

    bool wait_until_started() {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, 2s, [&] { return started_; });
    }

    bool finishes_within(std::chrono::milliseconds duration) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, duration, [&] { return finished_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool started_ = false;
    bool finished_ = false;
};

ControlClientIdentity connect_client(
    ControlBroker& broker,
    const VerifiedControlPeerIdentity& peer) {
    auto ticket = broker.issue_bootstrap(peer);
    REQUIRE(ticket.ticket.has_value());
    auto connected = broker.redeem_bootstrap(
        ticket.ticket->ticket_id, ticket.ticket->secret.bytes(), peer);
    REQUIRE(connected.client.has_value());
    return *connected.client;
}

ControlRegistration register_standalone(
    ControlBroker& broker,
    const VerifiedControlPeerIdentity& peer,
    std::string suffix) {
    auto registered = broker.register_instance(
        peer, registration_request(ControlHostTier::Standalone,
                                   std::move(suffix)));
    REQUIRE(registered.registration.has_value());
    return *registered.registration;
}

} // namespace

TEST_CASE("broker admits only authority-verified exact peers",
          "[inspect][control][broker][security]") {
    CHECK_FALSE(verify_peer(
        ControlPeerRole::Client, 100, "rejected", false));

    BrokerFixture fixture;
    CHECK_FALSE(fixture.broker.is_listening());
    auto wrong_role = fixture.broker.issue_bootstrap(fixture.standalone);
    CHECK(wrong_role.status == ControlIdentityStatus::PeerRoleMismatch);

    auto ticket = fixture.broker.issue_bootstrap(fixture.client);
    REQUIRE(ticket.ticket.has_value());
    CHECK(fixture.broker.redeem_bootstrap(
              ticket.ticket->ticket_id,
              ticket.ticket->secret.bytes(),
              fixture.rogue_client).status ==
          ControlIdentityStatus::IdentityMismatch);

    auto client = fixture.connect_client();
    auto registration = fixture.register_standalone();
    ControlGrantRequest request{
        client.client_id,
        registration.registration_id,
        {InspectorCapability::StateRead},
        5min,
    };
    CHECK(fixture.broker.issue_grant(
              fixture.rogue_client, request, consent("rogue-decision")).status ==
          ControlGrantStatus::ClientUnavailable);
    CHECK(fixture.broker.issue_grant(
              fixture.client, std::move(request), consent("owner-decision")).status ==
          ControlGrantStatus::Granted);
}

TEST_CASE("broker composition root exposes only T0 and T1 registrations",
          "[inspect][control][broker][security]") {
    BrokerFixture fixture;

    auto t0 = fixture.broker.register_instance(
        fixture.offline,
        registration_request(ControlHostTier::OfflineJob, "offline"));
    REQUIRE(t0.status == ControlIdentityStatus::Accepted);
    REQUIRE(t0.registration.has_value());

    auto t1 = fixture.broker.register_instance(
        fixture.standalone,
        registration_request(ControlHostTier::Standalone, "standalone"));
    REQUIRE(t1.status == ControlIdentityStatus::Accepted);
    REQUIRE(t1.registration.has_value());

    auto bridge = std::move(*verify_peer(
        ControlPeerRole::TrustedHostBridge, 203, "bridge-start"));
    auto t2 = fixture.broker.register_instance(
        bridge,
        registration_request(ControlHostTier::SharedPluginHost, "shared"));
    CHECK(t2.status == ControlIdentityStatus::HostUnavailable);
    CHECK_FALSE(t2.registration.has_value());

    CHECK(fixture.broker.register_instance(
              fixture.standalone,
              registration_request(ControlHostTier::OfflineJob,
                                   "wrong-role")).status ==
          ControlIdentityStatus::PeerRoleMismatch);
}

TEST_CASE("broker lifecycle revokes grants only for the owning verified peer",
          "[inspect][control][broker][lifecycle]") {
    BrokerFixture fixture;
    auto client = fixture.connect_client();
    auto registration = fixture.register_standalone();
    auto issued = fixture.broker.issue_grant(
        fixture.client,
        ControlGrantRequest{
            client.client_id,
            registration.registration_id,
            {InspectorCapability::StateRead},
            5min,
        },
        consent("grant-a"));
    REQUIRE(issued.grant.has_value());

    auto rejected = fixture.broker.disconnect_client(
        client.client_id, fixture.rogue_client, "rogue-disconnect");
    CHECK_FALSE(rejected.identity_removed);
    CHECK(rejected.grants_revoked == 0);
    CHECK(fixture.broker.is_granted(
        issued.grant->grant_id,
        client.client_id,
        registration.registration_id,
        InspectorCapability::StateRead));

    auto disconnected = fixture.broker.disconnect_client(
        client.client_id, fixture.client, "client-disconnect");
    CHECK(disconnected.identity_removed);
    CHECK(disconnected.grants_revoked == 1);
    CHECK_FALSE(fixture.broker.client(client.client_id));
    CHECK_FALSE(fixture.broker.grant(issued.grant->grant_id));
}

TEST_CASE("broker registration teardown and expiry remove grant authority",
          "[inspect][control][broker][lifecycle]") {
    BrokerFixture fixture;
    auto client = fixture.connect_client();
    auto registration = fixture.register_standalone();
    auto issued = fixture.broker.issue_grant(
        fixture.client,
        ControlGrantRequest{
            client.client_id,
            registration.registration_id,
            {InspectorCapability::StateRead},
            2s,
        },
        consent("grant-expiring"));
    REQUIRE(issued.grant.has_value());

    fixture.now += 3s;
    fixture.broker.sweep_expired();
    CHECK_FALSE(fixture.broker.grant(issued.grant->grant_id));

    auto second = fixture.broker.issue_grant(
        fixture.client,
        ControlGrantRequest{
            client.client_id,
            registration.registration_id,
            {InspectorCapability::StateRead},
            5min,
        },
        consent("grant-registration"));
    REQUIRE(second.grant.has_value());

    auto removed = fixture.broker.unregister_instance(
        registration.registration_id,
        fixture.standalone,
        "registration-remove");
    CHECK(removed.identity_removed);
    CHECK(removed.grants_revoked == 1);
    CHECK_FALSE(fixture.broker.registration(registration.registration_id));
    CHECK_FALSE(fixture.broker.grant(second.grant->grant_id));
}

TEST_CASE("broker ownership guard denials are metadata-only audited",
          "[inspect][control][broker][security][audit]") {
    BrokerFixture fixture;
    auto client = fixture.connect_client();
    auto registration = fixture.register_standalone();
    auto rogue_host = std::move(*verify_peer(
        ControlPeerRole::StandaloneHost, 299, "rogue-host-start"));

    CHECK_FALSE(fixture.broker.refresh_client(
        client.client_id, fixture.rogue_client));
    CHECK_FALSE(fixture.broker.disconnect_client(
        client.client_id, fixture.rogue_client, "rogue-disconnect")
                    .identity_removed);
    CHECK_FALSE(fixture.broker.disconnect_client(
        client.client_id, fixture.client, {}).identity_removed);
    CHECK_FALSE(fixture.broker.heartbeat(
        registration.registration_id, rogue_host));
    CHECK_FALSE(fixture.broker.unregister_instance(
        registration.registration_id, rogue_host, "rogue-unregister")
                    .identity_removed);
    CHECK_FALSE(fixture.broker.unregister_instance(
        registration.registration_id, fixture.standalone, {})
                    .identity_removed);

    ControlGrantRequest request{
        client.client_id,
        registration.registration_id,
        {InspectorCapability::StateRead},
        5min,
    };
    CHECK(fixture.broker.issue_grant(
              fixture.rogue_client, std::move(request),
              consent("rogue-grant")).status ==
          ControlGrantStatus::ClientUnavailable);

    const auto events = fixture.audit->snapshot();
    CHECK(has_denial(events, "client.refresh", "identity-mismatch",
                     fixture.rogue_client.fingerprint(),
                     client.client_id.value));
    CHECK(has_denial(events, "client.disconnect", "identity-mismatch",
                     fixture.rogue_client.fingerprint(),
                     client.client_id.value));
    CHECK(has_denial(events, "client.disconnect", "invalid-request",
                     fixture.client.fingerprint(), client.client_id.value));
    CHECK(has_denial(events, "registration.heartbeat", "identity-mismatch",
                     rogue_host.fingerprint(), {},
                     registration.registration_id.value));
    CHECK(has_denial(events, "registration.unregister", "identity-mismatch",
                     rogue_host.fingerprint(), {},
                     registration.registration_id.value));
    CHECK(has_denial(events, "registration.unregister", "invalid-request",
                     fixture.standalone.fingerprint(), {},
                     registration.registration_id.value));
    CHECK(has_denial(events, "grant.issue", "client-unavailable",
                     fixture.rogue_client.fingerprint(),
                     client.client_id.value,
                     registration.registration_id.value));
}

TEST_CASE("grant issue is atomic against client disconnect",
          "[inspect][control][broker][lifecycle][concurrency]") {
    BlockingNthClock clock;
    ControlBroker broker{{}, {}, [&] { return clock(); }};
    auto client_peer = std::move(*verify_peer(
        ControlPeerRole::Client, 401, "client-race"));
    auto host_peer = std::move(*verify_peer(
        ControlPeerRole::StandaloneHost, 402, "host-race"));
    const auto client = connect_client(broker, client_peer);
    const auto registration = register_standalone(
        broker, host_peer, "client-race");

    // issue_grant first sweeps stale capacity, then performs three identity
    // clock reads before its grant-store insertion clock. Pause at that final
    // boundary while holding the broker coordination lock, then prove teardown
    // cannot pass it.
    clock.block_after(5);
    ControlGrantResult issued;
    std::thread issuer([&] {
        issued = broker.issue_grant(
            client_peer,
            ControlGrantRequest{
                client.client_id,
                registration.registration_id,
                {InspectorCapability::StateRead},
                5min,
            },
            consent("client-race-grant"));
    });
    const bool issuer_blocked = clock.wait_until_blocked();
    CHECK(issuer_blocked);
    if (!issuer_blocked) {
        clock.release();
        issuer.join();
        return;
    }

    OperationState disconnect_state;
    ControlBrokerLifecycleResult disconnected;
    std::thread teardown([&] {
        disconnect_state.mark_started();
        disconnected = broker.disconnect_client(
            client.client_id, client_peer, "client-race-disconnect");
        disconnect_state.mark_finished();
    });
    CHECK(disconnect_state.wait_until_started());
    CHECK_FALSE(disconnect_state.finishes_within(10ms));

    clock.release();
    issuer.join();
    teardown.join();

    REQUIRE(issued.grant.has_value());
    CHECK(disconnected.identity_removed);
    CHECK(disconnected.grants_revoked == 1);
    CHECK_FALSE(broker.grant(issued.grant->grant_id));
    CHECK(broker.revoke_grant(
              issued.grant->grant_id, "client-race-repeat-revoke") ==
          ControlGrantStatus::Revoked);
}

TEST_CASE("grant issue is atomic against registration teardown",
          "[inspect][control][broker][lifecycle][concurrency]") {
    BlockingNthClock clock;
    ControlBroker broker{{}, {}, [&] { return clock(); }};
    auto client_peer = std::move(*verify_peer(
        ControlPeerRole::Client, 411, "client-registration-race"));
    auto host_peer = std::move(*verify_peer(
        ControlPeerRole::StandaloneHost, 412, "host-registration-race"));
    const auto client = connect_client(broker, client_peer);
    const auto registration = register_standalone(
        broker, host_peer, "registration-race");

    clock.block_after(5);
    ControlGrantResult issued;
    std::thread issuer([&] {
        issued = broker.issue_grant(
            client_peer,
            ControlGrantRequest{
                client.client_id,
                registration.registration_id,
                {InspectorCapability::StateRead},
                5min,
            },
            consent("registration-race-grant"));
    });
    const bool issuer_blocked = clock.wait_until_blocked();
    CHECK(issuer_blocked);
    if (!issuer_blocked) {
        clock.release();
        issuer.join();
        return;
    }

    OperationState unregister_state;
    ControlBrokerLifecycleResult unregistered;
    std::thread teardown([&] {
        unregister_state.mark_started();
        unregistered = broker.unregister_instance(
            registration.registration_id, host_peer,
            "registration-race-remove");
        unregister_state.mark_finished();
    });
    CHECK(unregister_state.wait_until_started());
    CHECK_FALSE(unregister_state.finishes_within(10ms));

    clock.release();
    issuer.join();
    teardown.join();

    REQUIRE(issued.grant.has_value());
    CHECK(unregistered.identity_removed);
    CHECK(unregistered.grants_revoked == 1);
    CHECK_FALSE(broker.grant(issued.grant->grant_id));
}

TEST_CASE("expiry sweep removes identity-stale grants from capacity",
          "[inspect][control][broker][lifecycle][capacity]") {
    BlockingNthClock clock;
    ControlBrokerConfig config;
    config.identities.client_ttl = 1s;
    config.identities.registration_ttl = 1s;
    config.grants.max_grants = 1;
    auto audit = std::make_shared<ControlSecurityAuditLog>();
    ControlBroker broker{config, audit, [&] { return clock(); }};
    auto client_peer = std::move(*verify_peer(
        ControlPeerRole::Client, 421, "client-expiry"));
    auto host_peer = std::move(*verify_peer(
        ControlPeerRole::StandaloneHost, 422, "host-expiry"));
    auto client = connect_client(broker, client_peer);
    auto registration = register_standalone(broker, host_peer, "expiry-a");
    auto stale = broker.issue_grant(
        client_peer,
        ControlGrantRequest{
            client.client_id,
            registration.registration_id,
            {InspectorCapability::StateRead},
            30min,
        },
        consent("expiry-grant-a"));
    REQUIRE(stale.grant.has_value());

    clock.advance(2s);
    broker.sweep_expired();
    CHECK_FALSE(broker.grant(stale.grant->grant_id));
    const auto expiry_events = audit->snapshot();
    CHECK(std::ranges::any_of(expiry_events, [&](const auto& event) {
        return event.action == "grant.remove-unavailable" &&
               event.grant_id == stale.grant->grant_id.value &&
               event.outcome == ControlSecurityOutcome::Revoked &&
               event.reason == "identity-unavailable";
    }));

    client = connect_client(broker, client_peer);
    registration = register_standalone(broker, host_peer, "expiry-b");
    auto replacement = broker.issue_grant(
        client_peer,
        ControlGrantRequest{
            client.client_id,
            registration.registration_id,
            {InspectorCapability::StateRead},
            30min,
        },
        consent("expiry-grant-b"));
    CHECK(replacement.status == ControlGrantStatus::Granted);
    CHECK(replacement.grant.has_value());
}

TEST_CASE("grant issue reclaims expired active capacity",
          "[inspect][control][broker][lifecycle][capacity]") {
    BlockingNthClock clock;
    ControlBrokerConfig config;
    config.grants.max_grants = 1;
    ControlBroker broker{config, {}, [&] { return clock(); }};
    auto client_peer = std::move(*verify_peer(
        ControlPeerRole::Client, 425, "client-grant-expiry"));
    auto host_peer = std::move(*verify_peer(
        ControlPeerRole::StandaloneHost, 426, "host-grant-expiry"));
    const auto client = connect_client(broker, client_peer);
    const auto registration = register_standalone(
        broker, host_peer, "grant-expiry");
    auto expiring = broker.issue_grant(
        client_peer,
        ControlGrantRequest{
            client.client_id,
            registration.registration_id,
            {InspectorCapability::StateRead},
            1s,
        },
        consent("grant-expiry-a"));
    REQUIRE(expiring.grant.has_value());

    clock.advance(2s);
    auto replacement = broker.issue_grant(
        client_peer,
        ControlGrantRequest{
            client.client_id,
            registration.registration_id,
            {InspectorCapability::StateRead},
            5min,
        },
        consent("grant-expiry-b"));
    CHECK(replacement.status == ControlGrantStatus::Granted);
    CHECK(replacement.grant.has_value());
    CHECK_FALSE(broker.grant(expiring.grant->grant_id));
}

TEST_CASE("identity API expiry sweep is atomic with grant issue",
          "[inspect][control][broker][lifecycle][concurrency]") {
    BlockingNthClock clock;
    ControlBrokerConfig config;
    config.identities.client_ttl = 1s;
    config.identities.registration_ttl = 1s;
    ControlBroker broker{config, {}, [&] { return clock(); }};
    auto client_peer = std::move(*verify_peer(
        ControlPeerRole::Client, 431, "client-identity-sweep"));
    auto other_client_peer = std::move(*verify_peer(
        ControlPeerRole::Client, 432, "other-client-identity-sweep"));
    auto host_peer = std::move(*verify_peer(
        ControlPeerRole::StandaloneHost, 433, "host-identity-sweep"));
    const auto client = connect_client(broker, client_peer);
    const auto registration = register_standalone(
        broker, host_peer, "identity-sweep");

    clock.block_after(5);
    ControlGrantResult issued;
    std::thread issuer([&] {
        issued = broker.issue_grant(
            client_peer,
            ControlGrantRequest{
                client.client_id,
                registration.registration_id,
                {InspectorCapability::StateRead},
                5min,
            },
            consent("identity-sweep-grant"));
    });
    const bool issuer_blocked = clock.wait_until_blocked();
    CHECK(issuer_blocked);
    if (!issuer_blocked) {
        clock.release();
        issuer.join();
        return;
    }

    clock.advance(2s);
    OperationState sweep_state;
    ControlBootstrapResult bootstrap;
    std::thread identity_api([&] {
        sweep_state.mark_started();
        bootstrap = broker.issue_bootstrap(other_client_peer);
        sweep_state.mark_finished();
    });
    CHECK(sweep_state.wait_until_started());
    CHECK_FALSE(sweep_state.finishes_within(10ms));

    clock.release();
    issuer.join();
    identity_api.join();

    CHECK(issued.status == ControlGrantStatus::ClientUnavailable);
    CHECK_FALSE(issued.grant.has_value());
    REQUIRE(bootstrap.ticket.has_value());
    CHECK_FALSE(broker.client(client.client_id));
    CHECK_FALSE(broker.registration(registration.registration_id));
}

TEST_CASE("authorization is atomic with client disconnect",
          "[inspect][control][broker][authorization][concurrency]") {
    BlockingNthClock clock;
    ControlBroker broker{{}, {}, [&] { return clock(); }};
    auto client_peer = std::move(*verify_peer(
        ControlPeerRole::Client, 441, "client-authorize-disconnect"));
    auto host_peer = std::move(*verify_peer(
        ControlPeerRole::StandaloneHost, 442, "host-authorize-disconnect"));
    const auto client = connect_client(broker, client_peer);
    const auto registration = register_standalone(
        broker, host_peer, "authorize-disconnect");
    auto issued = broker.issue_grant(
        client_peer,
        ControlGrantRequest{
            client.client_id,
            registration.registration_id,
            {InspectorCapability::StateRead},
            5min,
        },
        consent("authorize-disconnect-grant"));
    REQUIRE(issued.grant.has_value());

    // Pause after the grant and client snapshots, before registration lookup.
    // A disconnect must not complete while that authorization is in flight.
    clock.block_after(3);
    bool authorized = false;
    std::thread authorizer([&] {
        authorized = broker.is_granted(
            issued.grant->grant_id, client.client_id,
            registration.registration_id, InspectorCapability::StateRead);
    });
    const bool authorization_blocked = clock.wait_until_blocked();
    CHECK(authorization_blocked);
    if (!authorization_blocked) {
        clock.release();
        authorizer.join();
        return;
    }

    OperationState disconnect_state;
    ControlBrokerLifecycleResult disconnected;
    std::thread teardown([&] {
        disconnect_state.mark_started();
        disconnected = broker.disconnect_client(
            client.client_id, client_peer, "authorize-disconnect");
        disconnect_state.mark_finished();
    });
    CHECK(disconnect_state.wait_until_started());
    CHECK_FALSE(disconnect_state.finishes_within(10ms));

    clock.release();
    authorizer.join();
    teardown.join();

    CHECK(authorized);
    CHECK(disconnected.identity_removed);
    CHECK_FALSE(broker.is_granted(
        issued.grant->grant_id, client.client_id,
        registration.registration_id, InspectorCapability::StateRead));
}

TEST_CASE("authorization is atomic with registration teardown",
          "[inspect][control][broker][authorization][concurrency]") {
    BlockingNthClock clock;
    ControlBroker broker{{}, {}, [&] { return clock(); }};
    auto client_peer = std::move(*verify_peer(
        ControlPeerRole::Client, 451, "client-authorize-registration"));
    auto host_peer = std::move(*verify_peer(
        ControlPeerRole::StandaloneHost, 452, "host-authorize-registration"));
    const auto client = connect_client(broker, client_peer);
    const auto registration = register_standalone(
        broker, host_peer, "authorize-registration");
    auto issued = broker.issue_grant(
        client_peer,
        ControlGrantRequest{
            client.client_id,
            registration.registration_id,
            {InspectorCapability::StateRead},
            5min,
        },
        consent("authorize-registration-grant"));
    REQUIRE(issued.grant.has_value());

    clock.block_after(3);
    bool authorized = false;
    std::thread authorizer([&] {
        authorized = broker.is_granted(
            issued.grant->grant_id, client.client_id,
            registration.registration_id, InspectorCapability::StateRead);
    });
    const bool authorization_blocked = clock.wait_until_blocked();
    CHECK(authorization_blocked);
    if (!authorization_blocked) {
        clock.release();
        authorizer.join();
        return;
    }

    OperationState unregister_state;
    ControlBrokerLifecycleResult unregistered;
    std::thread teardown([&] {
        unregister_state.mark_started();
        unregistered = broker.unregister_instance(
            registration.registration_id, host_peer,
            "authorize-registration-remove");
        unregister_state.mark_finished();
    });
    CHECK(unregister_state.wait_until_started());
    CHECK_FALSE(unregister_state.finishes_within(10ms));

    clock.release();
    authorizer.join();
    teardown.join();

    CHECK(authorized);
    CHECK(unregistered.identity_removed);
    CHECK_FALSE(broker.is_granted(
        issued.grant->grant_id, client.client_id,
        registration.registration_id, InspectorCapability::StateRead));
}

TEST_CASE("authorization is atomic with direct revocation",
          "[inspect][control][broker][authorization][concurrency]") {
    BlockingNthClock clock;
    ControlBroker broker{{}, {}, [&] { return clock(); }};
    auto client_peer = std::move(*verify_peer(
        ControlPeerRole::Client, 461, "client-authorize-revoke"));
    auto host_peer = std::move(*verify_peer(
        ControlPeerRole::StandaloneHost, 462, "host-authorize-revoke"));
    const auto client = connect_client(broker, client_peer);
    const auto registration = register_standalone(
        broker, host_peer, "authorize-revoke");
    auto issued = broker.issue_grant(
        client_peer,
        ControlGrantRequest{
            client.client_id,
            registration.registration_id,
            {InspectorCapability::StateRead},
            5min,
        },
        consent("authorize-revoke-grant"));
    REQUIRE(issued.grant.has_value());

    clock.block_after(3);
    bool authorized = false;
    std::thread authorizer([&] {
        authorized = broker.is_granted(
            issued.grant->grant_id, client.client_id,
            registration.registration_id, InspectorCapability::StateRead);
    });
    const bool authorization_blocked = clock.wait_until_blocked();
    CHECK(authorization_blocked);
    if (!authorization_blocked) {
        clock.release();
        authorizer.join();
        return;
    }

    OperationState revoke_state;
    ControlGrantStatus revoked = ControlGrantStatus::InvalidRequest;
    std::thread revoker([&] {
        revoke_state.mark_started();
        revoked = broker.revoke_grant(
            issued.grant->grant_id, "authorize-direct-revoke");
        revoke_state.mark_finished();
    });
    CHECK(revoke_state.wait_until_started());
    CHECK_FALSE(revoke_state.finishes_within(10ms));

    clock.release();
    authorizer.join();
    revoker.join();

    CHECK(authorized);
    CHECK(revoked == ControlGrantStatus::Revoked);
    CHECK_FALSE(broker.is_granted(
        issued.grant->grant_id, client.client_id,
        registration.registration_id, InspectorCapability::StateRead));
}
