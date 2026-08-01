#include "inspector_client_test_support.hpp"

namespace {

class ScriptedAuthenticatedServer {
  public:
    using Handler = std::function<InspectorMessage(const InspectorMessage&)>;

    explicit ScriptedAuthenticatedServer(Handler handler) : handler_(std::move(handler)) {
        const auto token = generate_inspector_secret();
        REQUIRE(token.has_value());
        token_ = *token;
        server_.on_client_connected =
            [this](std::unique_ptr<pulp::events::InterprocessConnection> connection) {
                auto client =
                    std::shared_ptr<pulp::events::InterprocessConnection>(std::move(connection));
                auto* raw = client.get();
                const auto published = publisher_.record();
                if (!published) {
                    raw->disconnect();
                    return;
                }
                const auto challenge = pulp::inspect::make_inspector_auth_challenge(
                    published->session_id, published->instance_id, published->publication_id,
                    published->protocol_version);
                if (!challenge) {
                    raw->disconnect();
                    return;
                }
                auto verifier =
                    std::make_shared<pulp::inspect::InspectorAuthVerifier>(token_, *challenge);
                raw->set_on_text_message([this, raw, verifier](std::string_view text) {
                    InspectorMessage request;
                    if (!pulp::inspect::decode_message(std::string(text), request))
                        return;
                    if (request.method == pulp::inspect::methods::kSessionAuthenticate) {
                        authenticate(raw, *verifier, request);
                        return;
                    }
                    {
                        std::lock_guard lock(requests_mutex_);
                        requests_.push_back(request.method);
                    }
                    raw->send_message(pulp::inspect::encode_message(handler_(request)));
                });
                {
                    std::lock_guard lock(clients_mutex_);
                    clients_.push_back(std::move(client));
                }
                send_challenge(raw, *challenge);
            };
        REQUIRE(server_.start("127.0.0.1:0", pulp::events::IpcTransport::Socket));
        InspectorDiscoveryRecord record;
        record.session_id = "scripted-session";
        record.instance_id = "scripted-instance";
        record.plugin_id = "com.pulp.scripted-client-test";
        record.endpoint = "127.0.0.1:" + std::to_string(server_.bound_port());
        record.profile = InspectorProfile::Develop;
        REQUIRE(publisher_.publish(record, token_));
    }

    ~ScriptedAuthenticatedServer() {
        server_.stop();
        std::lock_guard lock(clients_mutex_);
        for (const auto& client : clients_)
            client->disconnect();
        clients_.clear();
    }

    std::unique_ptr<InspectorClientSession> connect() {
        const auto record = publisher_.record();
        REQUIRE(record.has_value());
        InspectorClientFailure failure;
        auto client =
            InspectorClientSession::connect({.session_id = record->session_id,
                                             .instance_id = record->instance_id,
                                             .publication_id = record->publication_id},
                                            &failure, std::chrono::seconds(1), temporary_.path);
        INFO(failure.code << ": " << failure.message);
        REQUIRE(client != nullptr);
        return client;
    }

    std::vector<std::string> requests() const {
        std::lock_guard lock(requests_mutex_);
        return requests_;
    }

  private:
    static void authenticate(pulp::events::InterprocessConnection* connection,
                             pulp::inspect::InspectorAuthVerifier& verifier,
                             const InspectorMessage& request) {
        try {
            const auto params = choc::json::parse(request.params_json);
            const auto proof = params["proof"];
            if (!proof.isString())
                throw std::runtime_error("missing proof");
            const auto server_proof = verifier.authenticate(proof.getString());
            if (!server_proof)
                throw std::runtime_error("invalid proof");
            auto response = choc::value::createObject("");
            response.addMember("authenticated", true);
            response.addMember("serverProof", choc::value::createString(*server_proof));
            connection->send_message(pulp::inspect::encode_message(
                make_response(request.id, choc::json::toString(response, false))));
        } catch (...) {
            connection->disconnect();
        }
    }

    static void send_challenge(pulp::events::InterprocessConnection* connection,
                               const pulp::inspect::InspectorAuthChallenge& challenge) {
        auto params = choc::value::createObject("");
        params.addMember("scheme", choc::value::createString(challenge.scheme));
        params.addMember("nonce", choc::value::createString(challenge.nonce_hex));
        params.addMember("sessionId", choc::value::createString(challenge.session_id));
        params.addMember("instanceId", choc::value::createString(challenge.instance_id));
        params.addMember("publicationId", choc::value::createString(challenge.publication_id));
        params.addMember("protocolVersion", choc::value::createString(challenge.protocol_version));
        connection->send_message(pulp::inspect::encode_message(
            pulp::inspect::make_event(std::string(pulp::inspect::methods::kSessionAuthChallenge),
                                      choc::json::toString(params, false))));
    }

    TemporaryDirectory temporary_;
    InspectorDiscoveryPublisher publisher_{temporary_.path};
    pulp::events::InterprocessConnectionServer server_;
    std::vector<std::uint8_t> token_;
    Handler handler_;
    mutable std::mutex clients_mutex_;
    std::vector<std::shared_ptr<pulp::events::InterprocessConnection>> clients_;
    mutable std::mutex requests_mutex_;
    std::vector<std::string> requests_;
};

std::vector<std::string> controlled_sequence() {
    return {std::string(pulp::inspect::methods::kSessionAcquireController),
            std::string(pulp::inspect::methods::kStateSetParameter),
            std::string(pulp::inspect::methods::kSessionReleaseController)};
}

} // namespace

TEST_CASE("shared controlled requests report a successful mutation when release fails",
          "[inspect][client][session][control][release]") {
    ScriptedAuthenticatedServer server([](const InspectorMessage& request) {
        if (request.method == pulp::inspect::methods::kSessionReleaseController)
            return pulp::inspect::make_error(request.id, "release rejected", "release_rejected");
        return make_response(request.id, R"({"ok":true})");
    });
    auto client = server.connect();

    const auto response =
        client->request_controlled(std::string(pulp::inspect::methods::kStateSetParameter),
                                   R"({"id":7,"value":0.5})", std::chrono::seconds(1));

    REQUIRE(response.is_error);
    CHECK(response.error_code == "controller_release_failed");
    const auto data = choc::json::parse(response.error_data_json);
    CHECK(data["mutationApplied"].getBool());
    CHECK(data["releaseErrorCode"].getString() == "release_rejected");
    CHECK(server.requests() == controlled_sequence());
}

TEST_CASE("shared controlled requests release after a rejected mutation",
          "[inspect][client][session][control][release]") {
    ScriptedAuthenticatedServer server([](const InspectorMessage& request) {
        if (request.method == pulp::inspect::methods::kStateSetParameter)
            return pulp::inspect::make_error(request.id, "mutation rejected", "mutation_rejected");
        return make_response(request.id, R"({"ok":true})");
    });
    auto client = server.connect();

    const auto response =
        client->request_controlled(std::string(pulp::inspect::methods::kStateSetParameter),
                                   R"({"id":7,"value":0.5})", std::chrono::seconds(1));

    REQUIRE(response.is_error);
    CHECK(response.error_code == "mutation_rejected");
    CHECK(server.requests() == controlled_sequence());
}

TEST_CASE("shared controlled requests use one deadline through controller release",
          "[inspect][client][session][control][timeout][release]") {
    ScriptedAuthenticatedServer server([](const InspectorMessage& request) {
        if (request.method == pulp::inspect::methods::kSessionAcquireController ||
            request.method == pulp::inspect::methods::kStateSetParameter) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        } else if (request.method == pulp::inspect::methods::kSessionReleaseController) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        return make_response(request.id, R"({"ok":true})");
    });
    auto client = server.connect();

    const auto started = std::chrono::steady_clock::now();
    const auto response =
        client->request_controlled(std::string(pulp::inspect::methods::kStateSetParameter),
                                   R"({"id":7,"value":0.5})", std::chrono::milliseconds(120));
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE(response.is_error);
    CHECK(response.error_code == "controller_release_failed");
    const auto data = choc::json::parse(response.error_data_json);
    CHECK(data["mutationApplied"].getBool());
    CHECK(data["releaseErrorCode"].getString() == "request_timeout");
    CHECK(elapsed < std::chrono::milliseconds(220));
    CHECK(server.requests() == controlled_sequence());
}

TEST_CASE("shared controlled requests serialize complete controller transactions",
          "[inspect][client][session][control][concurrency]") {
    std::mutex mutex;
    std::condition_variable cv;
    bool acquisition_started = false;
    bool allow_acquisition = false;
    ScriptedAuthenticatedServer server([&](const InspectorMessage& request) {
        if (request.method == pulp::inspect::methods::kSessionAcquireController) {
            std::unique_lock lock(mutex);
            if (!acquisition_started) {
                acquisition_started = true;
                cv.notify_all();
                cv.wait(lock, [&] { return allow_acquisition; });
            }
        }
        return make_response(request.id, R"({"ok":true})");
    });
    auto client = server.connect();

    InspectorMessage first;
    std::thread first_thread([&] {
        first = client->request_controlled(std::string(pulp::inspect::methods::kStateSetParameter),
                                           R"({"id":7,"value":0.25})", std::chrono::seconds(1));
    });
    {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock, std::chrono::seconds(1), [&] { return acquisition_started; })) {
            allow_acquisition = true;
            lock.unlock();
            cv.notify_all();
            first_thread.join();
            FAIL("first controlled request did not begin controller acquisition");
        }
    }

    const auto second =
        client->request_controlled(std::string(pulp::inspect::methods::kStateSetParameter),
                                   R"({"id":7,"value":0.75})", std::chrono::milliseconds(20));

    {
        std::lock_guard lock(mutex);
        allow_acquisition = true;
    }
    cv.notify_all();
    first_thread.join();

    CHECK_FALSE(first.is_error);
    REQUIRE(second.is_error);
    CHECK(second.error_code == "request_timeout");
    CHECK(second.error_data_json == R"({"mayHaveApplied":false})");
    CHECK(server.requests() == controlled_sequence());
}

TEST_CASE("explicit lease operations cannot interleave an automatic controller transaction",
          "[inspect][client][session][control][concurrency][lease]") {
    std::mutex mutex;
    std::condition_variable cv;
    bool acquisition_started = false;
    bool allow_acquisition = false;
    ScriptedAuthenticatedServer server([&](const InspectorMessage& request) {
        if (request.method == pulp::inspect::methods::kSessionAcquireController) {
            std::unique_lock lock(mutex);
            acquisition_started = true;
            cv.notify_all();
            cv.wait(lock, [&] { return allow_acquisition; });
        }
        return make_response(request.id, R"({"ok":true})");
    });
    auto client = server.connect();

    InspectorMessage mutation;
    std::thread mutation_thread([&] {
        mutation =
            client->request_controlled(std::string(pulp::inspect::methods::kStateSetParameter),
                                       R"({"id":7,"value":0.25})", std::chrono::seconds(1));
    });
    {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock, std::chrono::seconds(1), [&] { return acquisition_started; })) {
            allow_acquisition = true;
            lock.unlock();
            cv.notify_all();
            mutation_thread.join();
            FAIL("automatic transaction did not begin controller acquisition");
        }
    }

    const auto release =
        client->request_controlled(std::string(pulp::inspect::methods::kSessionReleaseController),
                                   "{}", std::chrono::milliseconds(20));

    {
        std::lock_guard lock(mutex);
        allow_acquisition = true;
    }
    cv.notify_all();
    mutation_thread.join();

    CHECK_FALSE(mutation.is_error);
    REQUIRE(release.is_error);
    CHECK(release.error_code == "request_timeout");
    CHECK(release.error_data_json == R"({"mayHaveApplied":false})");
    CHECK(server.requests() == controlled_sequence());
}
