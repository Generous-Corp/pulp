// inspector_server.cpp — TCP server for remote inspector access

#include <pulp/inspect/inspector_server.hpp>
#include <pulp/inspect/authentication.hpp>
#include <choc/text/choc_JSON.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <thread>
#include <utility>

namespace pulp::inspect {

// ── Server implementation using InterprocessConnectionServer ────────────

class InspectorServer::Impl
    : public std::enable_shared_from_this<InspectorServer::Impl> {
public:
    struct AuthenticationState {
        std::unique_ptr<InspectorAuthVerifier> verifier;
        std::string client_id;
        bool authenticated = false;
        std::chrono::steady_clock::time_point deadline;
    };

    class OutboundClient
        : public std::enable_shared_from_this<OutboundClient> {
    public:
        static std::shared_ptr<OutboundClient> create(
            std::shared_ptr<events::InterprocessConnection> connection) {
            auto result = std::shared_ptr<OutboundClient>(
                new OutboundClient(std::move(connection)));
            result->worker = std::thread([result] { result->run(); });
            return result;
        }

        void enqueue(std::string message) {
            {
                std::lock_guard lock(mutex);
                if (stopping)
                    return;
                if (messages.size() >= 32)
                    messages.pop_front();
                messages.push_back(std::move(message));
            }
            cv.notify_one();
        }

        void request_stop() {
            {
                std::lock_guard lock(mutex);
                stopping = true;
                messages.clear();
            }
            cv.notify_all();
        }

        void join() {
            if (worker.joinable() &&
                worker.get_id() == std::this_thread::get_id())
                worker.detach();
            else if (worker.joinable())
                worker.join();
        }

        void shutdown() {
            request_stop();
            join();
        }

    private:
        explicit OutboundClient(
            std::shared_ptr<events::InterprocessConnection> value)
            : connection(std::move(value)) {}

        void run() {
            std::unique_lock lock(mutex);
            while (!stopping) {
                cv.wait(lock, [this] {
                    return stopping || !messages.empty();
                });
                if (stopping)
                    break;
                auto message = std::move(messages.front());
                messages.pop_front();
                auto retained = connection;
                lock.unlock();
                if (retained && !retained->send_message(message)) {
                    lock.lock();
                    stopping = true;
                    messages.clear();
                    break;
                }
                lock.lock();
            }
        }

        std::shared_ptr<events::InterprocessConnection> connection;
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<std::string> messages;
        std::thread worker;
        bool stopping = false;
    };

    events::InterprocessConnectionServer server;
    std::vector<std::shared_ptr<events::InterprocessConnection>> owned_clients;
    std::map<events::InterprocessConnection*, AuthenticationState> authentication;
    std::map<events::InterprocessConnection*,
             std::shared_ptr<OutboundClient>> outbound_clients;
    std::vector<std::shared_ptr<OutboundClient>> retired_outbound_clients;
    std::mutex clients_mutex;
    std::condition_variable cleanup_cv;
    std::thread cleanup_thread;
    bool stopping_cleanup = false;
    bool stopping_callbacks = true;
    std::size_t active_callbacks = 0;
    std::map<std::thread::id, std::size_t> callback_threads;
    std::atomic<std::uint64_t> next_client_id{1};
    std::size_t max_clients = 16;
    std::atomic<int> port{0};
    InspectorSession* session = nullptr;
    InspectorDiscoveryPublisher* discovery = nullptr;
    std::vector<std::uint8_t> token;
    std::chrono::milliseconds authentication_timeout =
        std::chrono::seconds(3);
    std::mutex lifecycle_mutex;
    std::recursive_mutex transition_mutex;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> transition_waiting_for_callbacks{false};
    std::function<void()> heartbeat;
    std::chrono::steady_clock::time_point next_heartbeat{};
    std::chrono::milliseconds heartbeat_interval =
        std::chrono::seconds(10);

    class CallbackGuard {
    public:
        explicit CallbackGuard(std::shared_ptr<Impl> candidate)
            : impl(std::move(candidate)) {
            std::lock_guard lock(impl->clients_mutex);
            if (!impl->stopping_callbacks) {
                ++impl->active_callbacks;
                ++impl->callback_threads[std::this_thread::get_id()];
            } else {
                impl.reset();
            }
        }

        ~CallbackGuard() {
            if (!impl)
                return;
            {
                std::lock_guard lock(impl->clients_mutex);
                --impl->active_callbacks;
                const auto thread = std::this_thread::get_id();
                if (auto found = impl->callback_threads.find(thread);
                    found != impl->callback_threads.end() &&
                    --found->second == 0) {
                    impl->callback_threads.erase(found);
                }
            }
            impl->cleanup_cv.notify_all();
        }

        explicit operator bool() const { return impl != nullptr; }

    private:
        std::shared_ptr<Impl> impl;
    };

    Impl() {
        cleanup_thread = std::thread([this]() { cleanup_loop(); });
    }

    void initialize() {
        const std::weak_ptr<Impl> weak_self = shared_from_this();
        server.on_client_connected =
            [weak_self](std::unique_ptr<events::InterprocessConnection> conn) {
                if (const auto self = weak_self.lock())
                    self->accept_client(std::move(conn));
            };
    }

    void accept_client(
        std::unique_ptr<events::InterprocessConnection> conn) {
            auto client =
                std::shared_ptr<events::InterprocessConnection>(std::move(conn));
            auto* raw = client.get();
            if (!session) {
                raw->disconnect();
                return;
            }
            const auto challenge = make_inspector_auth_challenge(
                session->info().session_id,
                session->info().instance_id,
                session->info().protocol_version);
            if (!challenge) {
                raw->disconnect();
                return;
            }
            const auto client_id =
                "client-" + std::to_string(next_client_id.fetch_add(1));
            {
                std::lock_guard lock(clients_mutex);
                if (stopping_callbacks) {
                    client.reset();
                    return;
                }
                if (owned_clients.size() >= max_clients) {
                    client.reset();
                    return;
                }
                owned_clients.push_back(client);
                outbound_clients.insert_or_assign(
                    raw, OutboundClient::create(client));
                authentication.insert_or_assign(
                    raw,
                    AuthenticationState{
                        std::make_unique<InspectorAuthVerifier>(
                            token, *challenge),
                        client_id,
                        false,
                        std::chrono::steady_clock::now() +
                            authentication_timeout});
            }
            const std::weak_ptr<events::InterprocessConnection> weak = client;
            const std::weak_ptr<Impl> weak_self = shared_from_this();
            raw->set_on_disconnected([weak_self, weak, raw]() {
                const auto self = weak_self.lock();
                const auto keep_alive = weak.lock();
                if (!self || !keep_alive)
                    return;
                CallbackGuard callback(self);
                if (!callback)
                    return;
                std::string authenticated_client;
                std::shared_ptr<OutboundClient> outbound;
                {
                    std::lock_guard lock(self->clients_mutex);
                    if (const auto found = self->authentication.find(raw);
                        found != self->authentication.end()) {
                        if (found->second.authenticated)
                            authenticated_client = found->second.client_id;
                        self->authentication.erase(found);
                    }
                    if (const auto found = self->outbound_clients.find(raw);
                        found != self->outbound_clients.end()) {
                        outbound = std::move(found->second);
                        self->outbound_clients.erase(found);
                        outbound->request_stop();
                        self->retired_outbound_clients.push_back(outbound);
                    }
                    self->owned_clients.erase(
                        std::remove_if(
                            self->owned_clients.begin(),
                            self->owned_clients.end(),
                            [raw](const auto& candidate) {
                                return candidate.get() == raw;
                            }),
                        self->owned_clients.end());
                }
                if (!authenticated_client.empty() && self->session)
                    self->session->disconnect(authenticated_client);
                self->cleanup_cv.notify_one();
            });
            raw->set_on_text_message([weak_self, weak, raw](std::string_view msg) {
                const auto self = weak_self.lock();
                const auto keep_alive = weak.lock();
                if (!self || !keep_alive)
                    return;
                CallbackGuard callback(self);
                if (!callback)
                    return;
                self->on_message_received(std::string(msg), raw);
            });
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
                    "instanceId",
                    choc::value::createString(challenge->instance_id));
                params.addMember(
                    "protocolVersion",
                    choc::value::createString(challenge->protocol_version));
                const auto event = make_event(
                    methods::kSessionAuthChallenge,
                    choc::json::toString(params, false));
                if (!raw->send_message(encode_message(event)))
                    raw->disconnect();
    }

    ~Impl() {
        stop_cleanup();
    }

    void stop_cleanup() {
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
        while (true) {
            cleanup_cv.wait_for(lock, std::chrono::milliseconds(50), [this] {
                return stopping_cleanup ||
                       !retired_outbound_clients.empty();
            });
            const bool should_stop = stopping_cleanup;
            auto retired = std::move(retired_outbound_clients);
            const auto now = std::chrono::steady_clock::now();
            std::vector<std::shared_ptr<events::InterprocessConnection>> expired;
            if (!should_stop) {
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
            }
            std::function<void()> refresh;
            if (!should_stop && heartbeat && now >= next_heartbeat) {
                refresh = heartbeat;
                next_heartbeat = now + heartbeat_interval;
            }
            lock.unlock();
            for (const auto& outbound : retired) {
                if (outbound)
                    outbound->shutdown();
            }
            for (const auto& client : expired) {
                if (client)
                    client->disconnect();
            }
            if (refresh)
                refresh();
            if (should_stop)
                break;
            lock.lock();
        }
    }

    bool start_authenticated(InspectorServerConfig config);
    void stop();
    void stop_locked();
    void broadcast(const InspectorMessage& event);
    int client_count();
    void on_message_received(const std::string& data,
                             events::InterprocessConnection* sender);
};

InspectorServer::InspectorServer() : impl_(std::make_shared<Impl>()) {
    impl_->initialize();
}

InspectorServer::~InspectorServer() {
    auto impl = std::move(impl_);
    impl->stop();
    impl->stop_cleanup();
}

bool InspectorServer::start_authenticated(InspectorServerConfig config) {
    return impl_->start_authenticated(std::move(config));
}

void InspectorServer::stop() {
    impl_->stop();
}

void InspectorServer::broadcast(const InspectorMessage& event) {
    impl_->broadcast(event);
}

int InspectorServer::client_count() const {
    return impl_->client_count();
}

int InspectorServer::port() const {
    return impl_->port.load(std::memory_order_acquire);
}

bool InspectorServer::Impl::start_authenticated(InspectorServerConfig config) {
    std::lock_guard transition_lock(transition_mutex);
    stop_requested.store(false, std::memory_order_release);
    stop_locked();
    if (stop_requested.load(std::memory_order_acquire))
        return false;
    if (!config.session || !config.discovery ||
        config.token.size() != inspector_token_size ||
        config.heartbeat_interval <= std::chrono::milliseconds(0) ||
        config.heartbeat_interval >
            std::chrono::milliseconds::max() / 3 ||
        config.session->info().protocol_version != "1" ||
        config.record.session_id != config.session->info().session_id ||
        config.record.instance_id != config.session->info().instance_id ||
        config.record.plugin_id != config.session->info().plugin_id) {
        return false;
    }
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex);
        session = config.session;
        discovery = config.discovery;
        token = std::move(config.token);
    }
    authentication_timeout =
        std::max(config.authentication_timeout, std::chrono::milliseconds(1));
    server.set_max_message_bytes(
        std::clamp<std::size_t>(config.max_message_bytes, 1,
                                16u * 1024u * 1024u));
    server.set_write_timeout(std::chrono::seconds(3));
    server.set_frame_read_timeout(
        std::max(config.frame_read_timeout, std::chrono::milliseconds(1)));
    {
        std::lock_guard lock(clients_mutex);
        max_clients =
            std::clamp<std::size_t>(config.max_clients, 1, 64);
        heartbeat_interval = config.heartbeat_interval;
        stopping_callbacks = false;
    }
    const auto discovery_ttl = std::max(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::seconds(30)),
        heartbeat_interval * 3);
    if (!server.start("127.0.0.1:0", events::IpcTransport::Socket)) {
        stop_locked();
        return false;
    }
    if (stop_requested.load(std::memory_order_acquire)) {
        stop_locked();
        return false;
    }
    port.store(server.bound_port(), std::memory_order_release);
    if (port.load(std::memory_order_acquire) == 0) {
        stop_locked();
        return false;
    }
    config.record.endpoint =
        "127.0.0.1:" +
        std::to_string(port.load(std::memory_order_acquire));
    bool published = false;
    {
        // A prior generation's heartbeat may already have copied its refresh
        // closure before stop_locked() cleared it. Serialize publication with
        // that closure so the shared publisher is never mutated concurrently.
        std::lock_guard lifecycle_lock(lifecycle_mutex);
        config.record.protocol_version = session->info().protocol_version;
        config.record.profile = session->policy().profile();
        published = discovery->publish(config.record, token, discovery_ttl);
    }
    if (!published) {
        stop_locked();
        return false;
    }
    if (stop_requested.load(std::memory_order_acquire)) {
        stop_locked();
        return false;
    }
    {
        std::lock_guard lock(clients_mutex);
        next_heartbeat =
            std::chrono::steady_clock::now() + heartbeat_interval;
        const std::weak_ptr<Impl> weak_self = shared_from_this();
        heartbeat = [weak_self, discovery_ttl] {
            const auto self = weak_self.lock();
            if (!self)
                return;
            std::lock_guard lifecycle_lock(self->lifecycle_mutex);
            if (self->discovery)
                (void)self->discovery->refresh(discovery_ttl);
        };
    }
    return true;
}

void InspectorServer::Impl::stop() {
    // Keep the shared teardown state alive if this call is the last owner and
    // must defer to a transition that is waiting for the current callback.
    const auto keep_alive = shared_from_this();
    stop_requested.store(true, std::memory_order_release);
    std::unique_lock transition_lock(transition_mutex, std::defer_lock);
    if (!transition_lock.try_lock()) {
        bool called_from_callback = false;
        {
            std::lock_guard lock(clients_mutex);
            called_from_callback =
                callback_threads.contains(std::this_thread::get_id());
        }
        if (called_from_callback) {
            // A transition tearing down the current generation must wait for
            // this callback, so blocking on its mutex would deadlock. Defer
            // only in that interval. Otherwise keep trying until the
            // transition completes, then perform the requested stop.
            while (!transition_lock.try_lock()) {
                if (transition_waiting_for_callbacks.load(
                        std::memory_order_acquire))
                    return;
                std::this_thread::yield();
            }
        } else {
            transition_lock.lock();
        }
    }
    stop_locked();
}

void InspectorServer::Impl::stop_locked() {
    transition_waiting_for_callbacks.store(true, std::memory_order_release);
    {
        std::lock_guard lock(clients_mutex);
        heartbeat = {};
        stopping_callbacks = true;
    }
    server.stop();
    std::vector<std::shared_ptr<events::InterprocessConnection>> clients;
    std::vector<std::shared_ptr<Impl::OutboundClient>> outbound_to_stop;
    std::vector<std::string> authenticated_clients;
    {
        std::lock_guard lock(clients_mutex);
        clients = std::move(owned_clients);
        for (auto& [client, outbound] : this->outbound_clients) {
            (void)client;
            outbound_to_stop.push_back(std::move(outbound));
        }
        this->outbound_clients.clear();
        for (auto& outbound : retired_outbound_clients)
            outbound_to_stop.push_back(std::move(outbound));
        retired_outbound_clients.clear();
        for (const auto& [client, state] : authentication) {
            (void)client;
            if (state.authenticated)
                authenticated_clients.push_back(state.client_id);
        }
    }
    for (const auto& client : clients) {
        if (client)
            client->disconnect();
    }
    for (const auto& outbound : outbound_to_stop) {
        if (outbound)
            outbound->shutdown();
    }
    {
        std::unique_lock lock(clients_mutex);
        const auto current = callback_threads.find(
            std::this_thread::get_id());
        const auto current_callbacks =
            current == callback_threads.end() ? 0 : current->second;
        cleanup_cv.wait(lock, [this, current_callbacks] {
            return active_callbacks <= current_callbacks;
        });
        authentication.clear();
    }
    transition_waiting_for_callbacks.store(false, std::memory_order_release);
    clients.clear();
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex);
        if (session) {
            for (const auto& client_id : authenticated_clients)
                session->disconnect(client_id);
        }
        if (discovery)
            discovery->remove();
        discovery = nullptr;
        session = nullptr;
        std::fill(token.begin(), token.end(), std::uint8_t{0});
        token.clear();
    }
    port.store(0, std::memory_order_release);
}

void InspectorServer::Impl::broadcast(const InspectorMessage& event) {
    const auto* descriptor = find_inspector_method(event.method);
    if (event.id != 0 || !descriptor ||
        descriptor->kind != InspectorMethodKind::Event) {
        return;
    }
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex);
        if (!session ||
            !session->policy().is_available(descriptor->capability) ||
            !session->policy().is_granted(descriptor->capability)) {
            return;
        }
    }

    auto json = encode_message(event);
    {
        std::lock_guard lock(clients_mutex);
        for (const auto& client : owned_clients) {
            const auto auth = authentication.find(client.get());
            if (auth != authentication.end() &&
                auth->second.authenticated) {
                if (const auto outbound =
                        outbound_clients.find(client.get());
                    outbound != outbound_clients.end()) {
                    outbound->second->enqueue(json);
                }
            }
        }
    }
}

int InspectorServer::Impl::client_count() {
    std::lock_guard lock(clients_mutex);
    return static_cast<int>(std::count_if(
        owned_clients.begin(), owned_clients.end(),
        [&](const auto& client) {
            const auto auth = authentication.find(client.get());
            return auth != authentication.end() &&
                   auth->second.authenticated;
        }));
}

void InspectorServer::Impl::on_message_received(
    const std::string& data,
    events::InterprocessConnection* sender) {
    InspectorMessage request;
    if (!decode_message(data, request)) {
        // Invalid JSON — send error
        auto err = make_error(0, "Invalid JSON message");
        auto json = encode_message(err);
        sender->send_message(json.data(), json.size());
        return;
    }

    if (session) {
        std::string client_id;
        bool authenticated = false;
        {
            std::lock_guard lock(clients_mutex);
            const auto found = authentication.find(sender);
            if (found == authentication.end()) {
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
                std::lock_guard lock(clients_mutex);
                const auto found = authentication.find(sender);
                if (found != authentication.end() &&
                    found->second.verifier &&
                    std::chrono::steady_clock::now() <
                        found->second.deadline) {
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

        const auto response = session->handle(client_id, request);
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
