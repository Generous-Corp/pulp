#include "inspector_client_test_support.hpp"

TEST_CASE("server rejects unsupported protocol versions before publishing",
          "[inspect][client][protocol-version]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Observe;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe,
    };
    InspectorSession session({"session-future-version", "instance", "plugin", "2"}, policy,
                             [](const auto& request) { return make_response(request.id, "{}"); });
    InspectorServer server;
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = session.info().session_id;
    record.instance_id = session.info().instance_id;
    record.plugin_id = session.info().plugin_id;
    CHECK_FALSE(
        server.start_authenticated(InspectorServerConfig{&session, &publisher, record, *token}));
    CHECK(server.port() == 0);
    CHECK(reader.list().empty());
}

TEST_CASE("server rejects deeply nested JSON before authentication",
          "[inspect][client][authentication][resource-limit]") {
    AuthenticatedFixture fixture;

    Socket socket;
    REQUIRE(socket.create(SocketType::TCP));
    REQUIRE(socket.set_read_timeout(std::chrono::seconds(1)));
    REQUIRE(socket.connect("127.0.0.1", static_cast<std::uint16_t>(fixture.server.port())));
    REQUIRE(receive_frame(socket).has_value());

    constexpr std::size_t depth = 65;
    std::string params(depth, '[');
    params += '0';
    params.append(depth, ']');
    const auto request =
        std::string(R"({"id":1,"method":"Session.authenticate","params":)") + params + '}';
    REQUIRE(send_frame(socket, request));

    const auto response_frame = receive_frame(socket);
    REQUIRE(response_frame.has_value());
    pulp::inspect::InspectorMessage response;
    REQUIRE(pulp::inspect::decode_message(*response_frame, response));
    CHECK(response.is_error);
    CHECK(response.error_code == "message_too_deep");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (fixture.server.client_count() != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    CHECK(fixture.server.client_count() == 0);
}

TEST_CASE("authenticated client completes read and controlled mutation",
          "[inspect][client][authentication]") {
    AuthenticatedFixture fixture;
    const auto records = fixture.reader.list();
    REQUIRE(records.size() == 1);

    InspectorClient client;
    REQUIRE(client.connect(records.front(), fixture.reader));
    const auto capabilities = client.request("Session.getCapabilities");
    REQUIRE_FALSE(capabilities.is_error);

    const auto read = client.request("State.getParameters");
    REQUIRE_FALSE(read.is_error);
    CHECK(read.params_json.find("\"gain\"") != std::string::npos);

    const auto denied = client.request("State.setParameter", R"({"id":"gain","value":0.75})");
    REQUIRE(denied.is_error);
    CHECK(denied.error_code == "controller_lease_required");

    REQUIRE_FALSE(client.request("Session.acquireController").is_error);
    REQUIRE_FALSE(client.request("State.setParameter", R"({"id":"gain","value":0.75})").is_error);
}

TEST_CASE("shared one-shot client selects exact publications and owns controller leases",
          "[inspect][client][one-shot]") {
    AuthenticatedFixture fixture;
    const auto records = fixture.reader.list();
    REQUIRE(records.size() == 1);
    const auto& record = records.front();

    const auto result = pulp::inspect::request_inspector(
        "State.setParameter", R"({"id":"gain","value":0.75})",
        {record.session_id, record.instance_id, record.publication_id}, std::chrono::seconds(1),
        fixture.reader);

    REQUIRE(result.succeeded());
    REQUIRE(result.publication.has_value());
    CHECK(result.publication->session_id == record.session_id);
    CHECK(result.publication->instance_id == record.instance_id);
    CHECK(result.publication->publication_id == record.publication_id);
    CHECK(choc::json::parse(result.response.params_json)["applied"].getBool());

    InspectorClient next_client;
    REQUIRE(next_client.connect(record, fixture.reader));
    CHECK_FALSE(next_client.request("Session.acquireController").is_error);
}

TEST_CASE("shared one-shot client validates and sends typed standalone test input",
          "[inspect][client][one-shot][test-input]") {
    AuthenticatedFixture fixture;
    const auto records = fixture.reader.list();
    REQUIRE(records.size() == 1);
    const auto& record = records.front();
    const pulp::inspect::InspectorClientTarget target{record.session_id, record.instance_id,
                                                      record.publication_id};

    const auto midi = inject_inspector_midi({.kind = pulp::inspect::MidiTestInputKind::NoteOn,
                                             .channel = 0,
                                             .note = 60,
                                             .velocity = 100},
                                            std::chrono::milliseconds(10), target,
                                            std::chrono::seconds(1), fixture.reader);
    REQUIRE(midi.succeeded());
    CHECK(choc::json::parse(midi.response.params_json)["applied"].getBool());
    {
        std::lock_guard lock(fixture.seen_mutex);
        std::vector<std::string> midi_kinds;
        for (const auto& request : fixture.seen) {
            if (request.method == pulp::inspect::methods::kTestInjectMidi)
                midi_kinds.push_back(
                    std::string(choc::json::parse(request.params_json)["kind"].getString()));
        }
        CHECK(midi_kinds == std::vector<std::string>{"note_on", "note_off"});
    }

    const auto transport =
        set_inspector_transport({.playing = false, .position_samples = 0, .tempo_bpm = 120.0},
                                target, std::chrono::seconds(1), fixture.reader);
    REQUIRE(transport.succeeded());
    CHECK(choc::json::parse(transport.response.params_json)["applied"].getBool());

    const auto invalid_midi = inject_inspector_midi(
        {.kind = pulp::inspect::MidiTestInputKind::NoteOn,
         .channel = 16,
         .note = 60,
         .velocity = 100},
        std::chrono::milliseconds(10), target, std::chrono::seconds(1), fixture.reader);
    CHECK_FALSE(invalid_midi.succeeded());
    CHECK(invalid_midi.response.error_code == "invalid_params");

    const auto invalid_transport =
        set_inspector_transport({}, target, std::chrono::seconds(1), fixture.reader);
    CHECK_FALSE(invalid_transport.succeeded());
    CHECK(invalid_transport.response.error_code == "invalid_params");

    InspectorClient next_client;
    REQUIRE(next_client.connect(record, fixture.reader));
    CHECK_FALSE(next_client.request("Session.acquireController").is_error);
}

TEST_CASE("shared one-shot client returns stable structured selection errors",
          "[inspect][client][one-shot][selection]") {
    TemporaryDirectory temporary;
    std::filesystem::create_directories(temporary.path);
#ifndef _WIN32
    REQUIRE(::chmod(temporary.path.c_str(), 0700) == 0);
#endif
    InspectorDiscoveryReader reader(temporary.path);

    const auto result = pulp::inspect::request_inspector(
        "DOM.getDocument", "{}", {"missing-session", "missing-instance", "missing-publication"},
        std::chrono::milliseconds(50), reader);

    CHECK_FALSE(result.succeeded());
    CHECK_FALSE(result.publication.has_value());
    CHECK(result.response.is_error);
    CHECK(result.response.error_code == "selection_failed");
    CHECK(result.response.error_data_json.find("missing-session") != std::string::npos);
    CHECK(result.response.error_data_json.find("missing-instance") != std::string::npos);
    CHECK(result.response.error_data_json.find("missing-publication") != std::string::npos);
}

#ifndef _WIN32
TEST_CASE("shared one-shot client preserves discovery security failures",
          "[inspect][client][one-shot][discovery]") {
    TemporaryDirectory temporary;
    std::filesystem::create_directories(temporary.path);
    REQUIRE(::chmod(temporary.path.c_str(), 0755) == 0);
    InspectorDiscoveryReader reader(temporary.path);

    const auto result = pulp::inspect::request_inspector("DOM.getDocument", "{}",
                                                         {"session", "instance", "publication"},
                                                         std::chrono::milliseconds(50), reader);

    CHECK_FALSE(result.succeeded());
    CHECK_FALSE(result.publication.has_value());
    CHECK(result.response.is_error);
    CHECK(result.response.error_code == "discovery_unavailable");
    CHECK(result.response.params_json == "runtime directory is not an owner-private directory");
    CHECK(result.response.error_data_json.find(temporary.path.string()) != std::string::npos);
}
#endif

TEST_CASE("authenticated domain mutation applies on main before response",
          "[inspect][client][main-thread][mutation]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Develop;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe,
        InspectorCapability::SessionControl,
        InspectorCapability::StateWrite,
    };

    const auto main_thread = std::this_thread::get_id();
    std::mutex mutex;
    std::condition_variable cv;
    std::function<void()> queued;
    std::atomic<bool> applied{false};
    std::atomic<bool> handler_ran_off_main{false};
    auto rpc = std::make_shared<InspectorMainThreadRpc>(
        InspectorMainThreadRpc::Config{std::chrono::seconds(1), 4},
        [&](auto task) {
            {
                std::lock_guard lock(mutex);
                queued = std::move(task);
            }
            cv.notify_all();
            return true;
        },
        [main_thread] { return std::this_thread::get_id() == main_thread; });
    InspectorSession session(
        {"session-main-thread", "instance", "plugin", "1"}, policy, [&](const auto& request) {
            handler_ran_off_main.store(std::this_thread::get_id() != main_thread,
                                       std::memory_order_release);
            applied.store(true, std::memory_order_release);
            return make_response(request.id, R"({"applied":true})");
        });
    InspectorServer server;
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = session.info().session_id;
    record.instance_id = session.info().instance_id;
    record.plugin_id = session.info().plugin_id;
    InspectorServerConfig config{&session, &publisher, record, *token};
    config.main_thread_rpc = rpc;
    REQUIRE(start_test_inspector_server(server, std::move(config)));
    const auto records = reader.list();
    REQUIRE(records.size() == 1);

    InspectorClient client;
    REQUIRE(client.connect(records.front(), reader));
    REQUIRE_FALSE(client.request("Session.acquireController").is_error);

    pulp::inspect::InspectorMessage response;
    std::atomic<bool> request_returned{false};
    std::thread requester([&] {
        response = client.request("State.setParameter", R"({"id":"gain","value":0.75})");
        request_returned.store(true, std::memory_order_release);
    });
    std::function<void()> main_task;
    {
        std::unique_lock lock(mutex);
        REQUIRE(
            cv.wait_for(lock, std::chrono::seconds(1), [&] { return static_cast<bool>(queued); }));
        main_task = std::move(queued);
    }
    CHECK_FALSE(applied.load(std::memory_order_acquire));
    CHECK_FALSE(request_returned.load(std::memory_order_acquire));

    main_task();
    requester.join();

    CHECK_FALSE(handler_ran_off_main.load(std::memory_order_acquire));
    CHECK(applied.load(std::memory_order_acquire));
    CHECK(request_returned.load(std::memory_order_acquire));
    CHECK_FALSE(response.is_error);
}

TEST_CASE("rejected post operation destruction can cancel RPC",
          "[inspect][client][main-thread][post-rejected][reentrant]") {
    struct ReentrantState {
        std::shared_ptr<InspectorMainThreadRpc> rpc;
        std::atomic<bool> armed{false};
        std::atomic<bool> reentered{false};
    };
    struct ReentrantCapture {
        std::shared_ptr<ReentrantState> state;

        ~ReentrantCapture() {
            if (!state->armed.load(std::memory_order_acquire) ||
                state->reentered.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            state->rpc->cancel();
        }
    };
    auto reentrant_state = std::make_shared<ReentrantState>();
    auto rpc = std::make_shared<InspectorMainThreadRpc>(
        InspectorMainThreadRpc::Config{std::chrono::seconds(1), 1},
        [reentrant_state](auto) {
            reentrant_state->armed.store(true, std::memory_order_release);
            return false;
        },
        [] { return false; });
    reentrant_state->rpc = rpc;
    InspectorMainThreadRpc::Operation operation = [capture = ReentrantCapture{reentrant_state}] {
        return make_response(1, "{}");
    };

    const auto response = rpc->call(1, std::move(operation));
    CHECK(reentrant_state->reentered.load(std::memory_order_acquire));
    CHECK(response.error_code == "main_thread_unavailable");

    REQUIRE(rpc->set_posted_lifetime_callbacks({}, {}));
    CHECK_FALSE(rpc->set_posted_lifetime_callbacks({}, {}));
}

TEST_CASE("post admission exceptions are contained and retire pending work",
          "[inspect][client][main-thread][post-exception]") {
    bool throw_non_standard = false;
    SECTION("standard exception") {}
    SECTION("non-standard exception") {
        throw_non_standard = true;
    }

    struct CaptureState {
        std::atomic<bool> armed{false};
        std::atomic<bool> retired{false};
    };
    struct Capture {
        std::shared_ptr<CaptureState> state;
        ~Capture() {
            if (state->armed.load(std::memory_order_acquire))
                state->retired.store(true, std::memory_order_release);
        }
    };
    auto capture_state = std::make_shared<CaptureState>();
    std::atomic<int> posted_begins{0};
    std::atomic<int> posted_ends{0};
    std::atomic<int> completions{0};
    std::atomic<bool> operation_ran{false};
    std::function<void()> retained_post;
    auto rpc = std::make_shared<InspectorMainThreadRpc>(
        InspectorMainThreadRpc::Config{std::chrono::seconds(1), 1},
        [&](auto task) -> bool {
            retained_post = std::move(task);
            capture_state->armed.store(true, std::memory_order_release);
            if (throw_non_standard)
                throw 7;
            throw std::runtime_error("post failed");
        },
        [] { return false; });
    REQUIRE(rpc->set_posted_lifetime_callbacks(
        [&] { posted_begins.fetch_add(1, std::memory_order_relaxed); },
        [&] { posted_ends.fetch_add(1, std::memory_order_relaxed); }));
    InspectorMainThreadRpc::Operation operation = [capture = Capture{capture_state},
                                                   &operation_ran] {
        operation_ran.store(true, std::memory_order_release);
        return make_response(1, "{}");
    };

    const auto response = rpc->call(1, std::move(operation),
                                    [&] { completions.fetch_add(1, std::memory_order_relaxed); });

    REQUIRE(response.is_error);
    CHECK(response.error_code == "dispatch_failed");
    CHECK(response.error_data_json.find("\"mayHaveApplied\":false") != std::string::npos);
    CHECK_FALSE(operation_ran.load(std::memory_order_acquire));
    CHECK(capture_state->retired.load(std::memory_order_acquire));
    CHECK(completions.load(std::memory_order_acquire) == 1);
    CHECK(posted_begins.load(std::memory_order_acquire) == 1);
    CHECK(posted_ends.load(std::memory_order_acquire) == 0);
    retained_post();
    CHECK_FALSE(operation_ran.load(std::memory_order_acquire));
    CHECK(completions.load(std::memory_order_acquire) == 1);
    retained_post = {};
    CHECK(posted_ends.load(std::memory_order_acquire) == 1);
}

TEST_CASE("inline post response survives an admission exception",
          "[inspect][client][main-thread][post-exception][inline]") {
    std::atomic<int> posted_begins{0};
    std::atomic<int> posted_ends{0};
    std::atomic<int> completions{0};
    std::atomic<int> operations{0};
    auto rpc = std::make_shared<InspectorMainThreadRpc>(
        InspectorMainThreadRpc::Config{std::chrono::seconds(1), 1},
        [](auto task) -> bool {
            task();
            throw std::runtime_error("post threw after execution");
        },
        [] { return false; });
    REQUIRE(rpc->set_posted_lifetime_callbacks(
        [&] { posted_begins.fetch_add(1, std::memory_order_relaxed); },
        [&] { posted_ends.fetch_add(1, std::memory_order_relaxed); }));

    const auto response = rpc->call(
        1,
        [&] {
            operations.fetch_add(1, std::memory_order_relaxed);
            return make_response(1, R"({"applied":true})");
        },
        [&] { completions.fetch_add(1, std::memory_order_relaxed); });

    REQUIRE_FALSE(response.is_error);
    CHECK(response.params_json.find("\"applied\":true") != std::string::npos);
    CHECK(operations.load(std::memory_order_acquire) == 1);
    CHECK(completions.load(std::memory_order_acquire) == 1);
    CHECK(posted_begins.load(std::memory_order_acquire) == 1);
    CHECK(posted_ends.load(std::memory_order_acquire) == 1);
}

TEST_CASE("oversized inspector responses return a bounded protocol error",
          "[inspect][client][resource-limit]") {
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
        {"session-large-response", "instance", "plugin", "1"}, policy, [](const auto& request) {
            return make_response(request.id,
                                 std::string(R"({"padding":")") + std::string(4096, 'x') + R"("})");
        });
    InspectorServer server;
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = session.info().session_id;
    record.instance_id = session.info().instance_id;
    record.plugin_id = session.info().plugin_id;
    InspectorServerConfig config{&session, &publisher, record, *token};
    config.max_message_bytes = 1024;
    REQUIRE(start_test_inspector_server(server, std::move(config)));
    const auto records = reader.list();
    REQUIRE(records.size() == 1);

    InspectorClient client;
    REQUIRE(client.connect(records.front(), reader));
    const auto response =
        client.request("State.getParameters", "{}", std::chrono::milliseconds(100));
    REQUIRE(response.is_error);
    CHECK(response.error_code == "response_too_large");
    CHECK(client.is_connected());
    CHECK_FALSE(client.request("Session.getCapabilities").is_error);
}

TEST_CASE("extended inspector transport carries a multi-megabyte capture response",
          "[inspect][client][capture][resource-limit]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Observe;
    policy.available_capabilities = {InspectorCapability::CaptureImage};
    const std::string payload = std::string("{\"mimeType\":\"image/png\",\"data\":\"") +
                                std::string(2u * 1024u * 1024u, 'A') + "\"}";
    InspectorSession session(
        {"session-large-capture", "instance", "plugin", "1"}, policy,
        [&payload](const auto& request) { return make_response(request.id, payload); });
    InspectorServer server;
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = session.info().session_id;
    record.instance_id = session.info().instance_id;
    record.plugin_id = session.info().plugin_id;
    InspectorServerConfig config{&session, &publisher, record, *token};
    config.max_message_bytes = pulp::inspect::kInspectorExtendedMessageBytes;
    config.main_thread_rpc = make_inline_test_main_thread_rpc();
    REQUIRE(server.start_authenticated(std::move(config)));
    const auto records = reader.list();
    REQUIRE(records.size() == 1);

    InspectorClient client;
    REQUIRE(client.connect(records.front(), reader));
    const auto response = client.request("Capture.screenshot", "{}", std::chrono::seconds(3));
    INFO("error code: " << response.error_code);
    INFO("error payload: " << response.error_data_json);
    REQUIRE_FALSE(response.is_error);
    const auto decoded = choc::json::parse(response.params_json);
    REQUIRE(decoded["data"].getString().size() == 2u * 1024u * 1024u);
}

TEST_CASE("authenticated client rejects a challenge for another instance",
          "[inspect][client][authentication][instance]") {
    AuthenticatedFixture fixture;
    const auto records = fixture.reader.list();
    REQUIRE(records.size() == 1);
    auto mismatched = records.front();
    mismatched.instance_id = "another-instance";

    InspectorClient client;
    CHECK_FALSE(client.connect(mismatched, fixture.reader));
    CHECK_FALSE(client.is_connected());
}

TEST_CASE("mutual authentication rejects reflection and gates early events",
          "[inspect][client][authentication][mutual]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());

    pulp::events::InterprocessConnectionServer fake_server;
    std::mutex clients_mutex;
    std::atomic<int> observed_events{0};
    std::atomic<bool> return_valid_server_proof{false};
    std::atomic<bool> send_oversized_event{false};
    std::vector<std::shared_ptr<pulp::events::InterprocessConnection>> fake_clients;
    fake_server.on_client_connected =
        [&](std::unique_ptr<pulp::events::InterprocessConnection> connection) {
            auto client =
                std::shared_ptr<pulp::events::InterprocessConnection>(std::move(connection));
            auto* raw = client.get();
            const auto challenge = pulp::inspect::make_inspector_auth_challenge(
                "mutual-session", "mutual-instance", publisher.record()->publication_id, "1");
            if (!challenge) {
                raw->disconnect();
                return;
            }
            raw->set_on_text_message([raw, challenge = *challenge, &token,
                                      &return_valid_server_proof,
                                      &send_oversized_event](std::string_view message) {
                pulp::inspect::InspectorMessage request;
                if (!pulp::inspect::decode_message(std::string(message), request))
                    return;
                std::string client_proof;
                try {
                    const auto params = choc::json::parse(request.params_json);
                    client_proof = std::string(params["proof"].getString());
                } catch (...) {
                }
                std::string server_proof = client_proof;
                if (return_valid_server_proof.load(std::memory_order_relaxed)) {
                    const auto generated = pulp::inspect::make_inspector_server_auth_proof(
                        *token, challenge, client_proof);
                    if (!generated) {
                        raw->disconnect();
                        return;
                    }
                    server_proof = *generated;
                }
                const auto event_params =
                    send_oversized_event.load(std::memory_order_relaxed)
                        ? std::string(R"({"padding":")") + std::string(70u * 1024u, 'x') + R"("})"
                        : R"({"value":0.9})";
                raw->send_message(pulp::inspect::encode_message(
                    pulp::inspect::make_event("State.parameterChanged", event_params)));
                raw->send_message(pulp::inspect::encode_message(make_response(
                    request.id, std::string(R"({"authenticated":true,"serverProof":")") +
                                    server_proof + R"("})")));
            });
            {
                std::lock_guard lock(clients_mutex);
                fake_clients.push_back(client);
            }
            auto params = choc::value::createObject("");
            params.addMember("scheme", choc::value::createString(challenge->scheme));
            params.addMember("nonce", choc::value::createString(challenge->nonce_hex));
            params.addMember("sessionId", choc::value::createString(challenge->session_id));
            params.addMember("instanceId", choc::value::createString(challenge->instance_id));
            params.addMember("publicationId", choc::value::createString(challenge->publication_id));
            params.addMember("protocolVersion",
                             choc::value::createString(challenge->protocol_version));
            raw->send_message(pulp::inspect::encode_message(pulp::inspect::make_event(
                "Session.authChallenge", choc::json::toString(params, false))));
        };
    REQUIRE(fake_server.start("127.0.0.1:0", pulp::events::IpcTransport::Socket));

    InspectorDiscoveryRecord record;
    record.session_id = "mutual-session";
    record.instance_id = "mutual-instance";
    record.plugin_id = "com.pulp.mutual-test";
    record.endpoint = "127.0.0.1:" + std::to_string(fake_server.bound_port());
    REQUIRE(publisher.publish(record, *token));
    const auto records = reader.list();
    REQUIRE(records.size() == 1);

    InspectorClient client;
    client.set_event_handler(
        [&](const auto&) { observed_events.fetch_add(1, std::memory_order_relaxed); });
    CHECK_FALSE(client.connect(records.front(), reader));
    CHECK_FALSE(client.is_connected());
    CHECK(observed_events.load(std::memory_order_relaxed) == 0);

    send_oversized_event.store(true, std::memory_order_relaxed);
    CHECK_FALSE(client.connect(records.front(), reader));
    CHECK(observed_events.load(std::memory_order_relaxed) == 0);

    send_oversized_event.store(false, std::memory_order_relaxed);
    return_valid_server_proof.store(true, std::memory_order_relaxed);
    REQUIRE(client.connect(records.front(), reader));
    const auto event_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (observed_events.load(std::memory_order_relaxed) != 1 &&
           std::chrono::steady_clock::now() < event_deadline) {
        std::this_thread::yield();
    }
    CHECK(observed_events.load(std::memory_order_relaxed) == 1);
    client.disconnect();

    fake_server.stop();
    std::lock_guard lock(clients_mutex);
    for (const auto& fake_client : fake_clients)
        fake_client->disconnect();
    fake_clients.clear();
}

TEST_CASE("server broadcasts only registered events granted by policy",
          "[inspect][client][events][policy]") {
    AuthenticatedFixture fixture;
    const auto records = fixture.reader.list();
    REQUIRE(records.size() == 1);

    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::string> events;
    InspectorClient client;
    client.set_event_handler([&](const auto& event) {
        std::lock_guard lock(mutex);
        events.push_back(event.method);
        cv.notify_all();
    });
    REQUIRE(client.connect(records.front(), fixture.reader));

    fixture.server.broadcast(pulp::inspect::make_event("Audio.levels", R"({"peak":0.9})"));
    fixture.server.broadcast(pulp::inspect::make_event("Unknown.event", "{}"));
    fixture.server.broadcast(
        pulp::inspect::make_event("State.parameterChanged", R"({"id":"gain","value":0.75})"));

    std::unique_lock lock(mutex);
    REQUIRE(cv.wait_for(lock, std::chrono::seconds(1), [&] { return !events.empty(); }));
    REQUIRE(events.size() == 1);
    CHECK(events.front() == "State.parameterChanged");
}

TEST_CASE("client event handlers can issue follow-up requests",
          "[inspect][client][events][reentrant]") {
    AuthenticatedFixture fixture;
    const auto records = fixture.reader.list();
    REQUIRE(records.size() == 1);

    std::mutex mutex;
    std::condition_variable cv;
    std::optional<pulp::inspect::InspectorMessage> follow_up;
    InspectorClient client;
    client.set_event_handler([&](const auto&) {
        auto response = client.request("State.getParameters");
        {
            std::lock_guard lock(mutex);
            follow_up = std::move(response);
        }
        cv.notify_all();
    });
    REQUIRE(client.connect(records.front(), fixture.reader));
    fixture.server.broadcast(pulp::inspect::make_event("State.parameterChanged", "{}"));

    std::unique_lock lock(mutex);
    REQUIRE(cv.wait_for(lock, std::chrono::seconds(1), [&] { return follow_up.has_value(); }));
    CHECK_FALSE(follow_up->is_error);
}

TEST_CASE("client reconnect discards queued events from the prior session",
          "[inspect][client][events][generation]") {
    AuthenticatedFixture fixture;
    const auto records = fixture.reader.list();
    REQUIRE(records.size() == 1);

    std::mutex mutex;
    std::condition_variable cv;
    bool first_entered = false;
    bool release_first = false;
    std::vector<int> delivered;
    std::optional<pulp::inspect::InspectorMessage> stale_follow_up;
    bool stale_reconnect_succeeded = true;
    InspectorClient client;
    client.set_event_handler([&](const auto& event) {
        const auto params = choc::json::parse(event.params_json);
        const auto sequence = static_cast<int>(params["sequence"].getInt64());
        std::unique_lock lock(mutex);
        delivered.push_back(sequence);
        if (sequence == 1) {
            first_entered = true;
            cv.notify_all();
            cv.wait_for(lock, std::chrono::seconds(2), [&] { return release_first; });
            lock.unlock();
            auto response = client.request("Session.getCapabilities");
            const bool reconnected = client.connect(records.front(), fixture.reader);
            client.disconnect();
            lock.lock();
            stale_follow_up = std::move(response);
            stale_reconnect_succeeded = reconnected;
        }
        cv.notify_all();
    });
    REQUIRE(client.connect(records.front(), fixture.reader));
    fixture.server.broadcast(
        pulp::inspect::make_event("State.parameterChanged", R"({"sequence":1})"));
    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(1), [&] { return first_entered; }));
    }
    fixture.server.broadcast(
        pulp::inspect::make_event("State.parameterChanged", R"({"sequence":2})"));
    REQUIRE_FALSE(client.request("Session.getCapabilities").is_error);
    client.disconnect();
    REQUIRE(client.connect(records.front(), fixture.reader));
    {
        std::lock_guard lock(mutex);
        release_first = true;
    }
    cv.notify_all();
    fixture.server.broadcast(
        pulp::inspect::make_event("State.parameterChanged", R"({"sequence":3})"));

    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(1),
                            [&] { return delivered.size() >= 2 && stale_follow_up.has_value(); }));
        CHECK(delivered == std::vector<int>{1, 3});
        REQUIRE(stale_follow_up->is_error);
        CHECK(stale_follow_up->error_code == "stale_event_callback");
        CHECK_FALSE(stale_reconnect_succeeded);
    }
    CHECK(client.is_connected());
    CHECK_FALSE(client.request("Session.getCapabilities").is_error);
}

TEST_CASE("stale event callback destruction closes the current connection",
          "[inspect][client][events][generation][teardown]") {
    AuthenticatedFixture fixture;
    const auto records = fixture.reader.list();
    REQUIRE(records.size() == 1);

    std::mutex mutex;
    std::condition_variable cv;
    bool handler_entered = false;
    bool release_handler = false;
    bool destroyed = false;
    auto client = std::make_unique<InspectorClient>();
    client->set_event_handler([&](const auto&) {
        {
            std::unique_lock lock(mutex);
            handler_entered = true;
            cv.notify_all();
            cv.wait_for(lock, std::chrono::seconds(2), [&] { return release_handler; });
        }
        client.reset();
        {
            std::lock_guard lock(mutex);
            destroyed = true;
        }
        cv.notify_all();
    });
    REQUIRE(client->connect(records.front(), fixture.reader));
    fixture.server.broadcast(pulp::inspect::make_event("State.parameterChanged", "{}"));
    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(1), [&] { return handler_entered; }));
    }

    client->disconnect();
    REQUIRE(client->connect(records.front(), fixture.reader));
    {
        std::lock_guard lock(mutex);
        release_handler = true;
    }
    cv.notify_all();
    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(1), [&] { return destroyed; }));
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (fixture.server.client_count() != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    CHECK(fixture.server.client_count() == 0);
}

TEST_CASE("response timeout fences may-have-applied requests",
          "[inspect][client][timeout][mutation]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Develop;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe,
        InspectorCapability::SessionControl,
        InspectorCapability::StateWrite,
    };
    std::atomic<bool> applied{false};
    InspectorSession session({"session-timeout", "instance", "plugin", "1"}, policy,
                             [&](const auto& request) {
                                 std::this_thread::sleep_for(std::chrono::milliseconds(80));
                                 applied.store(true, std::memory_order_release);
                                 return make_response(request.id, R"({"applied":true})");
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
    REQUIRE_FALSE(client.request("Session.acquireController").is_error);
    const auto response = client.request("State.setParameter", R"({"id":"gain","value":0.75})",
                                         std::chrono::milliseconds(10));
    REQUIRE(response.is_error);
    CHECK(response.error_code == "request_timeout");
    CHECK(response.error_data_json.find("\"mayHaveApplied\":true") != std::string::npos);
    CHECK_FALSE(client.is_connected());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!applied.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    CHECK(applied.load(std::memory_order_acquire));
}

TEST_CASE("client can be released from its event handler", "[inspect][client][events][teardown]") {
    AuthenticatedFixture fixture;
    const auto records = fixture.reader.list();
    REQUIRE(records.size() == 1);

    std::mutex mutex;
    std::condition_variable cv;
    bool released = false;
    auto client = std::make_unique<InspectorClient>();
    client->set_event_handler([&](const auto&) {
        client.reset();
        {
            std::lock_guard lock(mutex);
            released = true;
        }
        cv.notify_all();
    });
    REQUIRE(client->connect(records.front(), fixture.reader));
    fixture.server.broadcast(pulp::inspect::make_event("State.parameterChanged", "{}"));

    std::unique_lock lock(mutex);
    REQUIRE(cv.wait_for(lock, std::chrono::seconds(1), [&] { return released; }));
    CHECK_FALSE(client);
}

TEST_CASE("client disconnect during event writes does not deadlock",
          "[inspect][client][events][teardown][concurrency]") {
    AuthenticatedFixture fixture;
    const auto records = fixture.reader.list();
    REQUIRE(records.size() == 1);
    const std::string payload =
        R"({"id":"gain","padding":")" + std::string(256 * 1024, 'x') + R"("})";

    for (int iteration = 0; iteration < 8; ++iteration) {
        InspectorClient client;
        REQUIRE(client.connect(records.front(), fixture.reader));
        std::atomic<bool> started{false};
        std::thread broadcaster([&] {
            started.store(true, std::memory_order_release);
            for (int event = 0; event < 16; ++event) {
                fixture.server.broadcast(
                    pulp::inspect::make_event("State.parameterChanged", payload));
            }
        });
        while (!started.load(std::memory_order_acquire))
            std::this_thread::yield();
        client.disconnect();
        broadcaster.join();
    }

    InspectorClient replacement;
    REQUIRE(replacement.connect(records.front(), fixture.reader));
    CHECK_FALSE(replacement.request("State.getParameters").is_error);
}
