// inspector_server.cpp — TCP server for remote inspector access

#include <pulp/inspect/inspector_server.hpp>
#include <pulp/inspect/authentication.hpp>
#include <choc/text/choc_JSON.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <thread>
#include <utility>

namespace pulp::inspect {

// ── Server implementation using InterprocessConnectionServer ────────────

class InspectorServer::Impl {
public:
    struct AuthenticationState {
        std::unique_ptr<InspectorAuthVerifier> verifier;
        std::string client_id;
        bool authenticated = false;
        std::chrono::steady_clock::time_point deadline;
    };

    events::InterprocessConnectionServer server;
    std::vector<std::shared_ptr<events::InterprocessConnection>> owned_clients;
    std::map<events::InterprocessConnection*, AuthenticationState> authentication;
    std::mutex clients_mutex;
    std::condition_variable cleanup_cv;
    std::thread cleanup_thread;
    bool stopping_cleanup = false;
    std::atomic<std::uint64_t> next_client_id{1};
    InspectorServer* owner = nullptr;
    std::function<void()> heartbeat;
    std::chrono::steady_clock::time_point next_heartbeat{};

    Impl() {
        cleanup_thread = std::thread([this]() { cleanup_loop(); });
        server.on_client_connected = [this](std::unique_ptr<events::InterprocessConnection> conn) {
            auto client =
                std::shared_ptr<events::InterprocessConnection>(std::move(conn));
            auto* raw = client.get();
            const std::weak_ptr<events::InterprocessConnection> weak = client;
            raw->set_on_text_message([this, weak, raw](std::string_view msg) {
                const auto keep_alive = weak.lock();
                if (!keep_alive)
                    return;
                on_message(std::string(msg), raw);
            });
            raw->set_on_disconnected([this, weak, raw]() {
                const auto keep_alive = weak.lock();
                if (!keep_alive)
                    return;
                std::string authenticated_client;
                {
                    std::lock_guard lock(clients_mutex);
                    if (const auto found = authentication.find(raw);
                        found != authentication.end()) {
                        if (found->second.authenticated)
                            authenticated_client = found->second.client_id;
                        authentication.erase(found);
                    }
                    owned_clients.erase(
                        std::remove_if(
                            owned_clients.begin(), owned_clients.end(),
                            [raw](const auto& candidate) {
                                return candidate.get() == raw;
                            }),
                        owned_clients.end());
                }
                if (!authenticated_client.empty() && owner && owner->session_)
                    owner->session_->disconnect(authenticated_client);
                cleanup_cv.notify_one();
            });
            {
                std::lock_guard lock(clients_mutex);
                owned_clients.push_back(client);
            }
            if (owner && owner->session_) {
                const auto challenge = make_inspector_auth_challenge(
                    owner->session_->info().session_id,
                    owner->session_->info().protocol_version);
                if (!challenge) {
                    raw->disconnect();
                    return;
                }
                const auto client_id =
                    "client-" +
                    std::to_string(next_client_id.fetch_add(1));
                {
                    std::lock_guard lock(clients_mutex);
                    authentication.insert_or_assign(
                        raw,
                        AuthenticationState{
                            std::make_unique<InspectorAuthVerifier>(
                                owner->token_, *challenge),
                            client_id,
                            false,
                            std::chrono::steady_clock::now() +
                                owner->authentication_timeout_});
                }
                auto params = choc::value::createObject("");
                params.addMember(
                    "scheme",
                    choc::value::createString(challenge->scheme));
                params.addMember(
                    "nonce",
                    choc::value::createString(challenge->nonce_hex));
                params.addMember(
                    "sessionId",
                    choc::value::createString(challenge->session_id));
                params.addMember(
                    "protocolVersion",
                    choc::value::createString(challenge->protocol_version));
                const auto event = make_event(
                    methods::kSessionAuthChallenge,
                    choc::json::toString(params, false));
                raw->send_message(encode_message(event));
            }
        };
    }

    ~Impl() {
        {
            std::lock_guard lock(clients_mutex);
            stopping_cleanup = true;
        }
        cleanup_cv.notify_one();
        if (cleanup_thread.joinable())
            cleanup_thread.join();
    }

    void cleanup_loop() {
        std::unique_lock lock(clients_mutex);
        while (!stopping_cleanup) {
            cleanup_cv.wait_for(lock, std::chrono::milliseconds(50), [this] {
                return stopping_cleanup;
            });
            if (stopping_cleanup)
                break;
            const auto now = std::chrono::steady_clock::now();
            std::vector<std::shared_ptr<events::InterprocessConnection>> expired;
            for (const auto& [client, state] : authentication) {
                if (!state.authenticated && now >= state.deadline) {
                    const auto retained = std::find_if(
                        owned_clients.begin(), owned_clients.end(),
                        [client](const auto& candidate) {
                            return candidate.get() == client;
                        });
                    if (retained != owned_clients.end())
                        expired.push_back(*retained);
                }
            }
            std::function<void()> refresh;
            if (heartbeat && now >= next_heartbeat) {
                refresh = heartbeat;
                next_heartbeat = now + std::chrono::seconds(10);
            }
            lock.unlock();
            for (const auto& client : expired) {
                if (client)
                    client->disconnect();
            }
            if (refresh)
                refresh();
            lock.lock();
        }
    }

    std::function<void(const std::string&, events::InterprocessConnection*)> on_message;
};

InspectorServer::InspectorServer() : impl_(std::make_unique<Impl>()) {
    impl_->owner = this;
    impl_->on_message = [this](const std::string& data, events::InterprocessConnection* sender) {
        on_message_received(data, sender);
    };
}

InspectorServer::~InspectorServer() {
    stop();
}

bool InspectorServer::start_authenticated(InspectorServerConfig config) {
    stop();
    if (!config.session || !config.discovery ||
        config.token.size() != inspector_token_size ||
        config.record.session_id != config.session->info().session_id ||
        config.record.instance_id != config.session->info().instance_id ||
        config.record.plugin_id != config.session->info().plugin_id) {
        return false;
    }
    session_ = config.session;
    discovery_ = config.discovery;
    token_ = std::move(config.token);
    authentication_timeout_ =
        std::max(config.authentication_timeout, std::chrono::milliseconds(1));
    impl_->server.set_max_message_bytes(
        std::clamp<std::size_t>(config.max_message_bytes, 1,
                                16u * 1024u * 1024u));
    impl_->server.set_write_timeout(std::chrono::seconds(3));
    if (!impl_->server.start("127.0.0.1:0",
                             events::IpcTransport::Socket)) {
        stop();
        return false;
    }
    port_ = impl_->server.bound_port();
    if (port_ == 0) {
        stop();
        return false;
    }
    config.record.endpoint = "127.0.0.1:" + std::to_string(port_);
    config.record.protocol_version = session_->info().protocol_version;
    config.record.profile = session_->policy().profile();
    if (!discovery_->publish(config.record, token_)) {
        stop();
        return false;
    }
    {
        std::lock_guard lock(impl_->clients_mutex);
        impl_->next_heartbeat =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        impl_->heartbeat = [this] {
            std::lock_guard lifecycle_lock(lifecycle_mutex_);
            if (discovery_)
                (void)discovery_->refresh();
        };
    }
    return true;
}

void InspectorServer::stop() {
    {
        std::lock_guard lock(impl_->clients_mutex);
        impl_->heartbeat = {};
    }
    impl_->server.stop();
    std::vector<std::shared_ptr<events::InterprocessConnection>> clients;
    {
        std::lock_guard lock(impl_->clients_mutex);
        clients = std::move(impl_->owned_clients);
        impl_->authentication.clear();
    }
    clients.clear();
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        if (discovery_)
            discovery_->remove();
        discovery_ = nullptr;
        session_ = nullptr;
        std::fill(token_.begin(), token_.end(), std::uint8_t{0});
        token_.clear();
    }
    port_ = 0;
}

void InspectorServer::broadcast(const InspectorMessage& event) {
    auto json = encode_message(event);
    std::vector<std::shared_ptr<events::InterprocessConnection>> clients;
    {
        std::lock_guard lock(impl_->clients_mutex);
        for (const auto& client : impl_->owned_clients) {
            const auto auth = impl_->authentication.find(client.get());
            if (auth != impl_->authentication.end() &&
                auth->second.authenticated) {
                clients.push_back(client);
            }
        }
    }
    for (const auto& client : clients) {
        if (client)
            client->send_message(json.data(), json.size());
    }
}

int InspectorServer::client_count() const {
    std::lock_guard lock(impl_->clients_mutex);
    return static_cast<int>(std::count_if(
        impl_->owned_clients.begin(), impl_->owned_clients.end(),
        [&](const auto& client) {
            const auto auth = impl_->authentication.find(client.get());
            return auth != impl_->authentication.end() &&
                   auth->second.authenticated;
        }));
}

void InspectorServer::on_message_received(const std::string& data,
                                           events::InterprocessConnection* sender) {
    InspectorMessage request;
    if (!decode_message(data, request)) {
        // Invalid JSON — send error
        auto err = make_error(0, "Invalid JSON message");
        auto json = encode_message(err);
        sender->send_message(json.data(), json.size());
        return;
    }

    if (session_) {
        std::string client_id;
        bool authenticated = false;
        {
            std::lock_guard lock(impl_->clients_mutex);
            const auto found = impl_->authentication.find(sender);
            if (found == impl_->authentication.end()) {
                sender->disconnect();
                return;
            }
            client_id = found->second.client_id;
            authenticated = found->second.authenticated;
        }

        if (!authenticated) {
            if (request.method != methods::kSessionAuthenticate ||
                request.id == 0) {
                const auto response = make_error(
                    request.id,
                    "Authenticate before issuing inspector requests",
                    "authentication_required");
                sender->send_message(encode_message(response));
                sender->disconnect();
                return;
            }
            std::string proof;
            try {
                const auto params = choc::json::parse(request.params_json);
                proof = std::string(params["proof"].getString());
            } catch (...) {
            }
            bool accepted = false;
            {
                std::lock_guard lock(impl_->clients_mutex);
                const auto found = impl_->authentication.find(sender);
                if (found != impl_->authentication.end() &&
                    found->second.verifier) {
                    accepted = found->second.verifier->verify(proof);
                    found->second.verifier.reset();
                    found->second.authenticated = accepted;
                }
            }
            if (!accepted) {
                const auto response =
                    make_error(request.id,
                               "Inspector authentication failed",
                               "authentication_failed");
                sender->send_message(encode_message(response));
                sender->disconnect();
                return;
            }
            sender->send_message(
                encode_message(make_response(
                    request.id, R"({"authenticated":true})")));
            return;
        }

        const auto response = session_->handle(client_id, request);
        if (response.id != 0 || !response.method.empty())
            sender->send_message(encode_message(response));
        return;
    }

    sender->send_message(encode_message(
        make_error(request.id,
                   "No authenticated inspector session is active",
                   "session_inactive")));
}

} // namespace pulp::inspect
