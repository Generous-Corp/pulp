#include "inspector_client_test_support.hpp"

TEST_CASE("server stop waits for an active request callback",
          "[inspect][client][teardown][concurrency]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Develop;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe,
        InspectorCapability::StateRead,
    };

    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
    InspectorSession session(
        {"session-stop-waits", "instance", "plugin", "1"},
        policy,
        [&](const auto& request) {
            std::unique_lock lock(mutex);
            entered = true;
            cv.notify_all();
            cv.wait(lock, [&] { return release; });
            return make_response(request.id, "{}");
        });
    InspectorServer server;
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
    std::thread requester([&] {
        (void)client.request("State.getParameters");
    });
    bool request_entered = false;
    {
        std::unique_lock lock(mutex);
        request_entered = cv.wait_for(
            lock, std::chrono::seconds(1), [&] { return entered; });
        if (!request_entered)
            release = true;
    }
    cv.notify_all();
    if (!request_entered) {
        requester.join();
        FAIL("request handler did not start");
    }

    std::atomic<bool> stop_returned{false};
    std::thread stopper([&] {
        server.stop();
        stop_returned.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_FALSE(stop_returned.load(std::memory_order_acquire));
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    cv.notify_all();
    stopper.join();
    requester.join();
    CHECK(stop_returned.load(std::memory_order_acquire));
}

TEST_CASE("authentication rejects a replaced credential and teardown removes discovery",
          "[inspect][client][authentication][teardown]") {
    TemporaryDirectory temporary;
    {
        InspectorDiscoveryPublisher publisher(temporary.path);
        InspectorDiscoveryReader reader(temporary.path);
        InspectorPolicyConfig policy;
        policy.profile = InspectorProfile::Observe;
        policy.available_capabilities = {
            InspectorCapability::SessionDescribe,
            InspectorCapability::StateRead,
        };
        InspectorSession session(
            {"session-wrong-token", "instance", "plugin", "1"},
            policy,
            [](const auto& request) { return make_response(request.id, "{}"); });
        InspectorServer server;
        const auto token = generate_inspector_secret();
        REQUIRE(token.has_value());
        InspectorDiscoveryRecord record;
        record.session_id = session.info().session_id;
        record.instance_id = session.info().instance_id;
        record.plugin_id = session.info().plugin_id;
        REQUIRE(start_test_inspector_server(
            server,
            InspectorServerConfig{&session, &publisher, record, *token}));
        auto records = reader.list();
        REQUIRE(records.size() == 1);
        {
            std::ofstream replaced(records.front().credential_path,
                                   std::ios::trunc);
            replaced << std::string(64, '0');
        }
#ifndef _WIN32
        REQUIRE(::chmod(records.front().credential_path.c_str(), 0600) == 0);
#endif
        InspectorClient client;
        CHECK_FALSE(client.connect(records.front(), reader));
        server.stop();
        CHECK(reader.list().empty());
    }
    for (const auto& entry :
         std::filesystem::directory_iterator(temporary.path)) {
        CHECK(entry.path().extension() == ".lock");
    }
}

TEST_CASE("unauthenticated connections are closed at the authentication deadline",
          "[inspect][authentication][timeout]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Observe;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe,
    };
    InspectorSession session(
        {"session-auth-timeout", "instance", "plugin", "1"},
        policy,
        [](const auto& request) { return make_response(request.id, "{}"); });
    InspectorServer server;
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = session.info().session_id;
    record.instance_id = session.info().instance_id;
    record.plugin_id = session.info().plugin_id;
    InspectorServerConfig config{&session, &publisher, record, *token};
    config.authentication_timeout = std::chrono::milliseconds(20);
    REQUIRE(start_test_inspector_server(server, std::move(config)));

    std::mutex mutex;
    std::condition_variable cv;
    bool disconnected = false;
    pulp::events::InterprocessConnection connection;
    connection.set_on_text_message([](std::string_view) {});
    connection.set_on_disconnected([&] {
        std::lock_guard lock(mutex);
        disconnected = true;
        cv.notify_all();
    });
    REQUIRE(connection.connect(
        "127.0.0.1:" + std::to_string(server.port()),
        pulp::events::IpcTransport::Socket));
    std::unique_lock lock(mutex);
    CHECK(cv.wait_for(lock, std::chrono::seconds(1),
                      [&] { return disconnected; }));
}

TEST_CASE("server bounds pending unauthenticated clients",
          "[inspect][authentication][resource-limit]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Observe;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe,
        InspectorCapability::StateRead,
    };
    InspectorSession session(
        {"session-client-cap", "instance", "plugin", "1"},
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
    config.max_clients = 2;
    REQUIRE(start_test_inspector_server(server, std::move(config)));
    const auto records = reader.list();
    REQUIRE(records.size() == 1);

    std::vector<std::unique_ptr<pulp::events::InterprocessConnection>> pending;
    for (int index = 0; index < 2; ++index) {
        auto connection =
            std::make_unique<pulp::events::InterprocessConnection>();
        connection->set_on_text_message([](std::string_view) {});
        REQUIRE(connection->connect(
            records.front().endpoint,
            pulp::events::IpcTransport::Socket));
        pending.push_back(std::move(connection));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    InspectorClient rejected;
    CHECK_FALSE(rejected.connect(
        records.front(), reader, std::chrono::milliseconds(250)));

    pending.front()->disconnect();
    pending.erase(pending.begin());
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    InspectorClient admitted;
    CHECK(admitted.connect(records.front(), reader));
}

TEST_CASE("authenticated partial frames expire without consuming the client pool",
          "[inspect][authentication][resource-limit][timeout]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Observe;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe,
    };
    InspectorSession session(
        {"session-frame-timeout", "instance", "plugin", "1"},
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
    config.max_clients = 1;
    config.frame_read_timeout = std::chrono::milliseconds(40);
    REQUIRE(start_test_inspector_server(server, std::move(config)));
    const auto records = reader.list();
    REQUIRE(records.size() == 1);

    Socket partial;
    REQUIRE(partial.create(SocketType::TCP));
    REQUIRE(partial.set_read_timeout(std::chrono::seconds(1)));
    REQUIRE(partial.connect("127.0.0.1",
                            static_cast<std::uint16_t>(server.port())));
    REQUIRE(authenticate_raw(partial, *token));
    REQUIRE(server.client_count() == 1);

    // Declare a 64-byte frame but send only one payload byte. The timeout is
    // cumulative from the first header byte, so drip-feeding cannot retain
    // this sole slot indefinitely.
    const std::array<std::uint8_t, 5> partial_frame{64, 0, 0, 0, '{'};
    REQUIRE(send_all(partial, partial_frame));
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (server.client_count() != 0 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    REQUIRE(server.client_count() == 0);

    InspectorClient admitted;
    CHECK(admitted.connect(records.front(), reader));
}
