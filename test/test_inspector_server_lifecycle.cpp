#include "inspector_client_test_support.hpp"

#include <stdexcept>
#include <vector>

using pulp::inspect::InspectorPublicationBinding;
using pulp::inspect::InspectorPublicationLease;

namespace {

class ThrowingPublicationBinding final
    : public InspectorPublicationBinding {
public:
    bool throw_on_bind = false;
    std::atomic<int> bind_calls{0};
    std::atomic<int> unbind_calls{0};

    class Lease final : public InspectorPublicationLease {
    public:
        explicit Lease(std::atomic<int>& unbind_calls)
            : unbind_calls_(&unbind_calls) {}

        ~Lease() override {
            ++*unbind_calls_;
        }

    private:
        std::atomic<int>* unbind_calls_;
    };

    std::unique_ptr<InspectorPublicationLease> bind_publication(
        const InspectorDiscoveryRecord&) override {
        ++bind_calls;
        if (throw_on_bind)
            throw std::runtime_error("bind failed");
        return std::make_unique<Lease>(unbind_calls);
    }
};

class StaticPublicationBindings final
    : public pulp::inspect::InspectorDomainPublicationBindings {
public:
    explicit StaticPublicationBindings(
        std::vector<pulp::inspect::InspectorPublicationBindingRegistration>
            registrations)
        : registrations_(std::move(registrations)) {}

    std::vector<pulp::inspect::InspectorPublicationBindingRegistration>
    publication_bindings() const override {
        return registrations_;
    }

private:
    std::vector<pulp::inspect::InspectorPublicationBindingRegistration>
        registrations_;
};

class SentinelObservingPublicationBinding final
    : public InspectorPublicationBinding {
public:
    SentinelObservingPublicationBinding(
        InspectorDiscoveryReader& reader,
        InspectorDiscoveryPublisher& competing,
        std::span<const std::uint8_t> token)
        : reader_(&reader),
          competing_(&competing),
          token_(token.begin(), token.end()) {}

    std::atomic<bool> released{false};
    std::atomic<bool> visibility_hidden_before_release{false};
    std::atomic<bool> competitor_acquired_during_release{false};
    std::atomic<bool> reentrant_stop_returned{false};
    std::optional<InspectorDiscoveryRecord> bound_record;
    InspectorServer* reentrant_server = nullptr;

    class Lease final : public InspectorPublicationLease {
    public:
        Lease(
            SentinelObservingPublicationBinding& owner,
            InspectorDiscoveryRecord record)
            : owner_(&owner),
              record_(std::move(record)) {}

        ~Lease() override {
            owner_->visibility_hidden_before_release.store(
                owner_->reader_->list().empty(),
                std::memory_order_release);
            owner_->competitor_acquired_during_release.store(
                owner_->competing_->publish(
                    record_, owner_->token_, std::chrono::seconds(5)),
                std::memory_order_release);
            if (owner_->reentrant_server) {
                owner_->reentrant_server->stop();
                owner_->reentrant_stop_returned.store(
                    true, std::memory_order_release);
            }
            owner_->released.store(true, std::memory_order_release);
        }

    private:
        SentinelObservingPublicationBinding* owner_;
        InspectorDiscoveryRecord record_;
    };

    std::unique_ptr<InspectorPublicationLease> bind_publication(
        const InspectorDiscoveryRecord& record) override {
        bound_record = record;
        return std::make_unique<Lease>(*this, record);
    }

private:
    InspectorDiscoveryReader* reader_;
    InspectorDiscoveryPublisher* competing_;
    std::vector<std::uint8_t> token_;
};

} // namespace

TEST_CASE("authenticated inspector server is reachable only on loopback",
          "[inspect][server][security][authentication]") {
    AuthenticatedFixture fixture;
    const auto port = fixture.server.port();
    REQUIRE(port > 0);

    // Positive control: an unreachable server would make the external-address
    // rejection vacuous.
    Socket loopback;
    REQUIRE(loopback.create(SocketType::TCP));
    REQUIRE(loopback.connect("127.0.0.1", static_cast<std::uint16_t>(port)));

#ifdef _WIN32
    WARN("SKIPPED off-box reachability: no getifaddrs on Windows. The "
         "production loopback control still ran.");
#else
    const auto external = first_non_loopback_ipv4();
    if (!external) {
        WARN("SKIPPED off-box reachability: this host has no non-loopback "
             "IPv4 interface.");
    } else {
        Socket off_box;
        REQUIRE(off_box.create(SocketType::TCP));
        INFO("authenticated inspector must not listen on " << *external
             << ':' << port);
        CHECK_FALSE(off_box.connect(
            *external, static_cast<std::uint16_t>(port)));
    }
#endif
}

TEST_CASE("server stop is reentrant from a request callback",
          "[inspect][client][teardown][reentrant]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    InspectorPolicyConfig config;
    config.profile = InspectorProfile::Observe;
    config.available_capabilities = {
        InspectorCapability::SessionDescribe,
        InspectorCapability::StateRead,
    };
    InspectorServer server;
    InspectorSession session(
        {"session-reentrant-stop", "instance", "plugin", "1"},
        config,
        [&](const auto& request) {
            server.stop();
            return make_response(request.id, "{}");
        });
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = session.info().session_id;
    record.instance_id = session.info().instance_id;
    record.plugin_id = session.info().plugin_id;
    REQUIRE(start_test_inspector_server(
        server, InspectorServerConfig{&session, &publisher, record, *token}));
    const auto records = reader.list();
    REQUIRE(records.size() == 1);
    InspectorClient client;
    REQUIRE(client.connect(records.front(), reader));
    const auto response = client.request("State.getParameters");
    if (response.is_error) {
        CHECK(response.error_code == "connection_closed");
        CHECK(response.error_data_json.find("\"mayHaveApplied\":true") !=
              std::string::npos);
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!reader.list().empty() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(reader.list().empty());
}

TEST_CASE("server can be released from a request callback",
          "[inspect][client][teardown][owner-lifetime]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    InspectorPolicyConfig config;
    config.profile = InspectorProfile::Observe;
    config.available_capabilities = {
        InspectorCapability::SessionDescribe,
        InspectorCapability::StateRead,
    };
    std::unique_ptr<InspectorServer> server =
        std::make_unique<InspectorServer>();
    InspectorSession session(
        {"session-destroy-server", "instance", "plugin", "1"},
        config,
        [&](const auto& request) {
            server.reset();
            return make_response(request.id, "{}");
        });
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = session.info().session_id;
    record.instance_id = session.info().instance_id;
    record.plugin_id = session.info().plugin_id;
    REQUIRE(start_test_inspector_server(
        *server, InspectorServerConfig{&session, &publisher, record, *token}));
    const auto records = reader.list();
    REQUIRE(records.size() == 1);
    InspectorClient client;
    REQUIRE(client.connect(records.front(), reader));
    const auto response = client.request("State.getParameters");
    if (response.is_error)
        CHECK(response.error_code == "connection_closed");
    CHECK_FALSE(server);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!reader.list().empty() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(reader.list().empty());
}

TEST_CASE("server can be destroyed by a callback while another thread stops it",
          "[inspect][client][teardown][owner-lifetime][concurrency]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    InspectorPolicyConfig config;
    config.profile = InspectorProfile::Observe;
    config.available_capabilities = {
        InspectorCapability::SessionDescribe,
        InspectorCapability::StateRead,
    };
    std::unique_ptr<InspectorServer> server =
        std::make_unique<InspectorServer>();
    auto* server_raw = server.get();
    std::mutex mutex;
    std::condition_variable cv;
    bool callback_entered = false;
    bool destroy_server = false;
    InspectorSession session(
        {"session-concurrent-destroy", "instance", "plugin", "1"},
        config,
        [&](const auto& request) {
            {
                std::unique_lock lock(mutex);
                callback_entered = true;
                cv.notify_all();
                cv.wait(lock, [&] { return destroy_server; });
            }
            server.reset();
            return make_response(request.id, "{}");
        });
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = session.info().session_id;
    record.instance_id = session.info().instance_id;
    record.plugin_id = session.info().plugin_id;
    REQUIRE(start_test_inspector_server(
        *server, InspectorServerConfig{&session, &publisher, record, *token}));
    const auto records = reader.list();
    REQUIRE(records.size() == 1);
    InspectorClient client;
    REQUIRE(client.connect(records.front(), reader));

    std::thread requester([&] {
        (void)client.request("State.getParameters");
    });
    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(1),
                            [&] { return callback_entered; }));
    }
    std::atomic<bool> stop_returned{false};
    std::thread stopper([&] {
        server_raw->stop();
        stop_returned.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_FALSE(stop_returned.load(std::memory_order_acquire));
    {
        std::lock_guard lock(mutex);
        destroy_server = true;
    }
    cv.notify_all();
    requester.join();
    stopper.join();
    CHECK_FALSE(server);
    CHECK(stop_returned.load(std::memory_order_acquire));
    CHECK(reader.list().empty());
}

TEST_CASE("serialized callback stop and concurrent request do not deadlock",
          "[inspect][client][teardown][concurrency][reentrant]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    InspectorPolicyConfig config;
    config.profile = InspectorProfile::Observe;
    config.available_capabilities = {
        InspectorCapability::SessionDescribe,
        InspectorCapability::StateRead,
    };
    InspectorServer server;
    std::atomic<int> entered{0};
    InspectorSession session(
        {"session-concurrent-stop", "instance", "plugin", "1"},
        config,
        [&](const auto& request) {
            entered.fetch_add(1, std::memory_order_relaxed);
            server.stop();
            return make_response(request.id, "{}");
        });
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = session.info().session_id;
    record.instance_id = session.info().instance_id;
    record.plugin_id = session.info().plugin_id;
    REQUIRE(start_test_inspector_server(
        server, InspectorServerConfig{&session, &publisher, record, *token}));
    const auto records = reader.list();
    REQUIRE(records.size() == 1);
    InspectorClient first;
    InspectorClient second;
    REQUIRE(first.connect(records.front(), reader));
    REQUIRE(second.connect(records.front(), reader));
    std::thread first_request([&] {
        (void)first.request("State.getParameters");
    });
    std::thread second_request([&] {
        (void)second.request("State.getParameters");
    });
    first_request.join();
    second_request.join();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!reader.list().empty() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(entered.load(std::memory_order_relaxed) >= 1);
    CHECK(reader.list().empty());
}

TEST_CASE("callback stop cancels a concurrent authenticated restart",
          "[inspect][client][teardown][restart][concurrency]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Observe;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe,
        InspectorCapability::DiagnosticsRead,
        InspectorCapability::StateRead,
    };
    InspectorServer server;
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool allow_callback_stop = false;
    InspectorSession session(
        {"session-restart-fence", "instance", "plugin", "1"},
        policy,
        [&](const auto& request) {
            {
                std::unique_lock lock(mutex);
                entered = true;
                cv.notify_all();
                cv.wait(lock, [&] { return allow_callback_stop; });
            }
            server.stop();
            return make_response(request.id, "{}");
        });
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = session.info().session_id;
    record.instance_id = session.info().instance_id;
    record.plugin_id = session.info().plugin_id;
    REQUIRE(start_test_inspector_server(
        server, InspectorServerConfig{&session, &publisher, record, *token}));
    const auto records = reader.list();
    REQUIRE(records.size() == 1);
    InspectorClient client;
    REQUIRE(client.connect(records.front(), reader));

    std::atomic<bool> request_returned{false};
    std::thread requester([&] {
        (void)client.request("State.getParameters");
        request_returned.store(true, std::memory_order_release);
    });
    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(1),
                            [&] { return entered; }));
    }

    const auto replacement_token = generate_inspector_secret();
    REQUIRE(replacement_token.has_value());
    std::atomic<bool> restart_result{true};
    std::thread restarter([&] {
        restart_result.store(
            server.start_authenticated(InspectorServerConfig{
                &session, &publisher, record, *replacement_token}),
            std::memory_order_release);
    });

    const auto disconnect_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!request_returned.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < disconnect_deadline) {
        std::this_thread::yield();
    }
    REQUIRE(request_returned.load(std::memory_order_acquire));
    {
        std::lock_guard lock(mutex);
        allow_callback_stop = true;
    }
    cv.notify_all();

    requester.join();
    restarter.join();
    CHECK_FALSE(restart_result.load(std::memory_order_acquire));
    CHECK(server.port() == 0);
    CHECK(reader.list().empty());
}

TEST_CASE("server stop releases leases before a session restart",
          "[inspect][client][teardown][lease]") {
    AuthenticatedFixture fixture;
    auto records = fixture.reader.list();
    REQUIRE(records.size() == 1);

    InspectorClient first;
    REQUIRE(first.connect(records.front(), fixture.reader));
    REQUIRE_FALSE(first.request("Session.acquireController").is_error);

    fixture.server.stop();

    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = fixture.session.info().session_id;
    record.instance_id = fixture.session.info().instance_id;
    record.plugin_id = fixture.session.info().plugin_id;
    REQUIRE(fixture.server.start_authenticated(InspectorServerConfig{
        &fixture.session, &fixture.publisher, record, *token}));

    records = fixture.reader.list();
    REQUIRE(records.size() == 1);
    InspectorClient replacement;
    REQUIRE(replacement.connect(records.front(), fixture.reader));
    const auto acquired =
        replacement.request("Session.acquireController");
    CHECK_FALSE(acquired.is_error);
}

TEST_CASE("session restart serializes publication with heartbeat refresh",
          "[inspect][client][restart][heartbeat][concurrency]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Observe;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe,
    };
    InspectorSession first(
        {"session-heartbeat-a", "instance-a", "plugin", "1"},
        policy,
        [](const auto& request) { return make_response(request.id, "{}"); });
    InspectorSession second(
        {"session-heartbeat-b", "instance-b", "plugin", "1"},
        policy,
        [](const auto& request) { return make_response(request.id, "{}"); });
    InspectorServer server;

    auto start = [&](InspectorSession& session) {
        const auto token = generate_inspector_secret();
        REQUIRE(token.has_value());
        InspectorDiscoveryRecord record;
        record.session_id = session.info().session_id;
        record.instance_id = session.info().instance_id;
        record.plugin_id = session.info().plugin_id;
        InspectorServerConfig config{
            &session, &publisher, record, *token};
        config.heartbeat_interval = std::chrono::milliseconds(1);
        REQUIRE(server.start_authenticated(std::move(config)));
    };

    for (int iteration = 0; iteration < 8; ++iteration) {
        auto& expected = iteration % 2 == 0 ? first : second;
        start(expected);
        // The cleanup worker polls at 50 ms, so this crosses a refresh
        // boundary before the next generation replaces the publication.
        std::this_thread::sleep_for(std::chrono::milliseconds(55));
        const auto records = reader.list();
        REQUIRE(records.size() == 1);
        CHECK(records.front().session_id == expected.info().session_id);
        CHECK(records.front().instance_id == expected.info().instance_id);
    }
}

TEST_CASE("discovery lifetime remains longer than a configured heartbeat",
          "[inspect][client][heartbeat][discovery]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Observe;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe,
    };
    InspectorSession session(
        {"session-long-heartbeat", "instance", "plugin", "1"},
        policy,
        [](const auto& request) { return make_response(request.id, "{}"); });
    InspectorServer server;
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = session.info().session_id;
    record.instance_id = session.info().instance_id;
    record.plugin_id = session.info().plugin_id;
    InspectorServerConfig config{
        &session, &publisher, record, *token};
    config.heartbeat_interval = std::chrono::seconds(40);
    const auto started_at = std::chrono::duration_cast<
        std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    REQUIRE(server.start_authenticated(std::move(config)));

    const auto records = reader.list();
    REQUIRE(records.size() == 1);
    // A 40-second heartbeat would outlive the publisher's historical
    // 30-second default. The server derives a three-interval TTL instead.
    CHECK(records.front().expires_at_unix_ms - started_at >= 119'000);
}

TEST_CASE("server rejects heartbeat schedules that cannot be represented",
          "[inspect][client][heartbeat][resource-limit]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Observe;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe,
    };
    InspectorSession session(
        {"session-overflow-heartbeat", "instance", "plugin", "1"},
        policy,
        [](const auto& request) { return make_response(request.id, "{}"); });
    InspectorServer server;
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = session.info().session_id;
    record.instance_id = session.info().instance_id;
    record.plugin_id = session.info().plugin_id;
    InspectorServerConfig config{
        &session, &publisher, record, *token};
    config.heartbeat_interval =
        std::chrono::milliseconds::max() / 3;
    CHECK_FALSE(server.start_authenticated(std::move(config)));
    CHECK_FALSE(publisher.record().has_value());
}

TEST_CASE("publication binding exceptions cannot escape server lifecycle",
          "[inspect][publication][exceptions]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Observe;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe,
    };
    InspectorSession session(
        {"session-binding-exception", "instance", "plugin", "1"},
        policy,
        [](const auto& request) { return make_response(request.id, "{}"); });
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = session.info().session_id;
    record.instance_id = session.info().instance_id;
    record.plugin_id = session.info().plugin_id;

    auto prerequisite = std::make_shared<ThrowingPublicationBinding>();
    auto binding = std::make_shared<ThrowingPublicationBinding>();
    StaticPublicationBindings bindings({{
        InspectorCapability::SessionDescribe,
        prerequisite,
    }, {
        InspectorCapability::DiagnosticsRead,
        binding,
    }});
    binding->throw_on_bind = true;
    InspectorServerConfig rejected{
        &session, &publisher, record, *token};
    rejected.domain_bindings = &bindings;
    InspectorServer server;
    CHECK_FALSE(server.start_authenticated(std::move(rejected)));
    CHECK(server.port() == 0);
    CHECK(reader.list().empty());
    CHECK(prerequisite->bind_calls.load(std::memory_order_acquire) == 1);
    CHECK(prerequisite->unbind_calls.load(std::memory_order_acquire) == 1);
    CHECK(binding->bind_calls.load(std::memory_order_acquire) == 1);
    CHECK(binding->unbind_calls.load(std::memory_order_acquire) == 0);

    binding->throw_on_bind = false;
    InspectorServerConfig accepted{
        &session, &publisher, record, *token};
    accepted.domain_bindings = &bindings;
    accepted.heartbeat_interval = std::chrono::milliseconds(1);
    REQUIRE(server.start_authenticated(std::move(accepted)));
    CHECK(prerequisite->bind_calls.load(std::memory_order_acquire) == 2);
    CHECK(binding->bind_calls.load(std::memory_order_acquire) == 2);
#ifndef _WIN32
    REQUIRE(publisher.record().has_value());
    auto ownership_path = publisher.record()->record_path;
    ownership_path.replace_extension(".lock");
    {
        std::ofstream corrupted(ownership_path, std::ios::trunc);
        REQUIRE(corrupted.good());
        corrupted << "not-the-live-owner";
    }
    const auto loss_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (binding->unbind_calls.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < loss_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(binding->unbind_calls.load(std::memory_order_acquire) == 1);
    CHECK(prerequisite->unbind_calls.load(std::memory_order_acquire) == 2);
    CHECK(server.port() == 0);
    // The corrupted ownership marker prevents safe file removal, so the stale
    // record may remain until TTL expiry, but it no longer names a live server.
    CHECK(reader.list().size() == 1);
#endif
    CHECK_NOTHROW(server.stop());
    CHECK(binding->unbind_calls.load(std::memory_order_acquire) == 1);
    CHECK(prerequisite->unbind_calls.load(std::memory_order_acquire) == 2);
}

TEST_CASE("publication retirement hides visibility before releasing its binding",
          "[inspect][publication][ownership][teardown]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryPublisher competing(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Observe;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe,
    };
    InspectorSession session(
        {"session-retirement-order", "instance", "plugin", "1"},
        policy,
        [](const auto& request) { return make_response(request.id, "{}"); });
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = session.info().session_id;
    record.instance_id = session.info().instance_id;
    record.plugin_id = session.info().plugin_id;

    auto binding = std::make_shared<SentinelObservingPublicationBinding>(
        reader, competing, *token);
    StaticPublicationBindings bindings({{
        InspectorCapability::SessionDescribe,
        binding,
    }});
    InspectorServerConfig config{
        &session, &publisher, record, *token};
    config.domain_bindings = &bindings;
    InspectorServer server;
    binding->reentrant_server = &server;
    REQUIRE(server.start_authenticated(std::move(config)));
    REQUIRE(reader.list().size() == 1);

    server.stop();

    CHECK(binding->released.load(std::memory_order_acquire));
    CHECK(binding->visibility_hidden_before_release.load(
        std::memory_order_acquire));
    CHECK_FALSE(binding->competitor_acquired_during_release.load(
        std::memory_order_acquire));
    CHECK(binding->reentrant_stop_returned.load(std::memory_order_acquire));
    CHECK_FALSE(publisher.record().has_value());
    REQUIRE(binding->bound_record.has_value());
    REQUIRE(competing.publish(
        *binding->bound_record, *token, std::chrono::seconds(5)));
}
