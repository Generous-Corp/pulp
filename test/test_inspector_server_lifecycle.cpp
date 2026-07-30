#include "inspector_client_test_support.hpp"

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
    REQUIRE(server.start_authenticated(
        InspectorServerConfig{&session, &publisher, record, *token}));
    const auto records = reader.list();
    REQUIRE(records.size() == 1);
    InspectorClient client;
    REQUIRE(client.connect(records.front(), reader));
    const auto response = client.request("State.getParameters");
    REQUIRE(response.is_error);
    CHECK(response.error_code == "connection_closed");
    CHECK(response.error_data_json.find("\"mayHaveApplied\":true") !=
          std::string::npos);
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
    REQUIRE(server->start_authenticated(
        InspectorServerConfig{&session, &publisher, record, *token}));
    const auto records = reader.list();
    REQUIRE(records.size() == 1);
    InspectorClient client;
    REQUIRE(client.connect(records.front(), reader));
    CHECK(client.request("State.getParameters").is_error);
    CHECK_FALSE(server);
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
    REQUIRE(server->start_authenticated(
        InspectorServerConfig{&session, &publisher, record, *token}));
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

TEST_CASE("concurrent callback stop requests do not deadlock",
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
    std::mutex barrier_mutex;
    std::condition_variable barrier_cv;
    int entered = 0;
    InspectorSession session(
        {"session-concurrent-stop", "instance", "plugin", "1"},
        config,
        [&](const auto& request) {
            {
                std::unique_lock lock(barrier_mutex);
                ++entered;
                barrier_cv.notify_all();
                barrier_cv.wait(lock, [&] { return entered == 2; });
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
    REQUIRE(server.start_authenticated(
        InspectorServerConfig{&session, &publisher, record, *token}));
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
    REQUIRE(server.start_authenticated(
        InspectorServerConfig{&session, &publisher, record, *token}));
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
