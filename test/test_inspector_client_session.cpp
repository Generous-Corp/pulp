#include "inspector_client_test_support.hpp"

#include <limits>

TEST_CASE("shared client session owns exact selection and typed responses",
          "[inspect][client][selection][control][typed]") {
    AuthenticatedFixture fixture;
    InspectorClientFailure failure;
    auto client = InspectorClientSession::connect(
        {.session_id = "session-client-test", .instance_id = "instance-client-test"}, &failure,
        std::chrono::seconds(1), fixture.temporary.path);
    REQUIRE(client != nullptr);
    CHECK(failure.code.empty());

    const auto capabilities = client->read_capabilities();
    REQUIRE(capabilities);
    CHECK(capabilities.response_json.find("\"sessionId\"") != std::string::npos);
    CHECK(capabilities.value->session_id == "session-client-test");
    CHECK(capabilities.value->instance_id == "instance-client-test");
    CHECK(capabilities.value->profile == InspectorProfile::Develop);
    CHECK(std::find(capabilities.value->effective.begin(), capabilities.value->effective.end(),
                    InspectorCapability::StateWrite) != capabilities.value->effective.end());

    const auto context = client->read_agent_context();
    REQUIRE(context);
    CHECK(context.value->binary_build_id == "test-build");
    CHECK(context.value->xrun_count == 2);
    CHECK(context.value->actionable_issues == std::vector<std::string>{"test issue"});

    const auto parameters = client->read_parameters();
    REQUIRE(parameters);
    REQUIRE(parameters.value->size() == 1);
    CHECK(parameters.value->front().id == 7);
    CHECK(parameters.value->front().name == "gain");
    CHECK(parameters.value->front().normalized == 0.25);
    CHECK(parameters.value->front().display == std::optional<std::string>{"-6 dB"});

    CHECK_FALSE(
        client->request_controlled(std::string(pulp::inspect::methods::kSessionAcquireController))
            .is_error);
    CHECK_FALSE(
        client->request_controlled(std::string(pulp::inspect::methods::kSessionReleaseController))
            .is_error);
    const auto mutation = client->set_parameter_typed(7, 0.75, true);
    REQUIRE(mutation);
    CHECK(mutation.value->applied);

    const auto screenshot = client->capture_screenshot();
    REQUIRE(screenshot);
    CHECK(screenshot.value->mime_type == "image/png");
    CHECK(screenshot.value->width == 4);
    CHECK(screenshot.value->height == 3);
    CHECK(screenshot.value->data_base64 == "iVBORw0KGgo=");

    CHECK(client->set_parameter(-1, 0.5).error_code == "invalid_params");
    CHECK(client->set_parameter(0, std::numeric_limits<double>::infinity()).error_code ==
          "invalid_params");
    CHECK(client->set_parameter(0, 1.01, true).error_code == "invalid_params");
}

TEST_CASE("shared client session forwards authenticated events",
          "[inspect][client][session][events]") {
    AuthenticatedFixture fixture;
    InspectorClientFailure failure;
    auto client =
        InspectorClientSession::connect({.session_id = fixture.session.info().session_id,
                                         .instance_id = fixture.session.info().instance_id},
                                        &failure, std::chrono::seconds(1), fixture.temporary.path);
    REQUIRE(client != nullptr);

    std::mutex mutex;
    std::condition_variable cv;
    std::string method;
    client->set_event_handler([&](const InspectorMessage& event) {
        {
            std::lock_guard lock(mutex);
            method = event.method;
        }
        cv.notify_one();
    });

    fixture.server.broadcast(
        pulp::inspect::make_event("State.parameterChanged", R"({"id":7,"value":0.5})"));
    std::unique_lock lock(mutex);
    REQUIRE(cv.wait_for(lock, std::chrono::seconds(1), [&] { return !method.empty(); }));
    CHECK(method == "State.parameterChanged");
}

TEST_CASE("shared controlled requests do not send after their deadline",
          "[inspect][client][session][timeout]") {
    AuthenticatedFixture fixture;
    std::atomic<bool> mutation_seen{false};
    fixture.observe_request = [&](const InspectorMessage& request) {
        if (request.method == pulp::inspect::methods::kStateSetParameter)
            mutation_seen.store(true, std::memory_order_release);
    };
    InspectorClientFailure failure;
    auto client =
        InspectorClientSession::connect({.session_id = fixture.session.info().session_id,
                                         .instance_id = fixture.session.info().instance_id},
                                        &failure, std::chrono::seconds(1), fixture.temporary.path);
    REQUIRE(client != nullptr);

    const auto result = client->set_parameter_typed(7, 0.5, true, std::chrono::milliseconds(0));
    REQUIRE_FALSE(result);
    CHECK(result.failure.code == "request_timeout");
    CHECK(result.failure.data_json.find(R"("mayHaveApplied":false)") != std::string::npos);
    CHECK_FALSE(mutation_seen.load(std::memory_order_acquire));

    // An expired pre-send deadline does not fence an otherwise healthy
    // authenticated connection.
    CHECK(client->read_capabilities(std::chrono::seconds(1)));
}

TEST_CASE("shared typed client rejects malformed method responses",
          "[inspect][client][typed][schema]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Develop;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe, InspectorCapability::SessionControl,
        InspectorCapability::StateRead,       InspectorCapability::StateWrite,
        InspectorCapability::CaptureImage,
    };
    InspectorSession session({"session-malformed", "instance-malformed", "plugin-malformed", "1"},
                             policy, [](const auto& request) {
                                 if (request.method == "State.getParameters")
                                     return make_response(request.id, R"([{"id":"wrong"}])");
                                 if (request.method == "State.setParameter")
                                     return make_response(request.id, R"({"ok":false})");
                                 if (request.method == "Capture.screenshot")
                                     return make_response(request.id,
                                                          R"({"mimeType":"text/plain"})");
                                 return make_response(request.id, R"({"identity":[]})");
                             });
    InspectorServer server;
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = session.info().session_id;
    record.instance_id = session.info().instance_id;
    record.plugin_id = session.info().plugin_id;
    InspectorServerConfig config{&session, &publisher, record, *token};
    REQUIRE(start_test_inspector_server(server, std::move(config)));

    InspectorClientFailure failure;
    auto client = InspectorClientSession::connect(
        {.session_id = session.info().session_id, .instance_id = session.info().instance_id},
        &failure, std::chrono::seconds(1), temporary.path);
    REQUIRE(client != nullptr);

    const auto context = client->read_agent_context();
    CHECK_FALSE(context);
    CHECK(context.failure.code == "invalid_response");
    CHECK(context.failure.data_json.find("Inspector.getAgentContext") != std::string::npos);

    const auto parameters = client->read_parameters();
    CHECK_FALSE(parameters);
    CHECK(parameters.failure.code == "invalid_response");
    CHECK(parameters.failure.data_json.find("parameters[]") != std::string::npos);

    const auto mutation = client->set_parameter_typed(7, 0.5);
    CHECK_FALSE(mutation);
    CHECK(mutation.failure.code == "invalid_response");
    CHECK(mutation.failure.data_json.find("State.setParameter") != std::string::npos);

    const auto screenshot = client->capture_screenshot();
    CHECK_FALSE(screenshot);
    CHECK(screenshot.failure.code == "invalid_response");
    CHECK(screenshot.failure.data_json.find("Capture.screenshot") != std::string::npos);
}

TEST_CASE("shared client session fails closed on invalid or absent selectors",
          "[inspect][client][selection][negative]") {
    TemporaryDirectory temporary;
    InspectorClientFailure failure;
    CHECK(InspectorClientSession::connect({.host = "0.0.0.0"}, &failure,
                                          std::chrono::milliseconds(20),
                                          temporary.path) == nullptr);
    CHECK(failure.code == "invalid_selector");

    CHECK(InspectorClientSession::connect({.instance_id = "orphan-instance"}, &failure,
                                          std::chrono::milliseconds(20),
                                          temporary.path) == nullptr);
    CHECK(failure.code == "invalid_selector");

    CHECK(InspectorClientSession::connect({.session_id = "missing-session"}, &failure,
                                          std::chrono::milliseconds(20),
                                          temporary.path) == nullptr);
    CHECK(failure.code == "session_selection_failed");
}

TEST_CASE("shared client session preserves transport connection failures",
          "[inspect][client][session][connect][error]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    pulp::events::InterprocessConnectionServer unavailable;
    REQUIRE(unavailable.start("127.0.0.1:0", pulp::events::IpcTransport::Socket));
    const auto unavailable_port = unavailable.bound_port();
    REQUIRE(unavailable_port != 0);
    unavailable.stop();
    InspectorDiscoveryRecord record;
    record.session_id = "unreachable-session";
    record.instance_id = "unreachable-instance";
    record.plugin_id = "unreachable-plugin";
    record.endpoint = "127.0.0.1:" + std::to_string(unavailable_port);
    REQUIRE(publisher.publish(record, *token));

    const auto published = publisher.record();
    REQUIRE(published.has_value());
    InspectorClientFailure failure;
    CHECK(InspectorClientSession::connect({.session_id = published->session_id,
                                           .instance_id = published->instance_id,
                                           .publication_id = published->publication_id},
                                          &failure, std::chrono::milliseconds(100),
                                          temporary.path) == nullptr);
    CHECK(failure.code == "transport_connection_failed");
    CHECK(failure.message.find("transport") != std::string::npos);
    CHECK(failure.data_json.find(published->publication_id) != std::string::npos);
}

TEST_CASE("shared client session distinguishes a pre-challenge disconnect",
          "[inspect][client][session][connect][error]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    pulp::events::InterprocessConnectionServer server;
    server.on_client_connected = [](auto connection) { connection->disconnect(); };
    REQUIRE(server.start("127.0.0.1:0", pulp::events::IpcTransport::Socket));
    InspectorDiscoveryRecord record;
    record.session_id = "disconnect-session";
    record.instance_id = "disconnect-instance";
    record.plugin_id = "disconnect-plugin";
    record.endpoint = "127.0.0.1:" + std::to_string(server.bound_port());
    REQUIRE(publisher.publish(record, *token));

    const auto published = publisher.record();
    REQUIRE(published.has_value());
    InspectorClientFailure failure;
    CHECK(InspectorClientSession::connect(
              {.session_id = published->session_id,
               .instance_id = published->instance_id,
               .publication_id = published->publication_id},
              &failure, std::chrono::seconds(1), temporary.path) == nullptr);
    CHECK(failure.code == "connection_closed");
    CHECK(failure.message.find("challenge") != std::string::npos);
}
