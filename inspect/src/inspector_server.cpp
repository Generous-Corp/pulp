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
    InspectorServer* owner = nullptr;
    std::function<void()> heartbeat;
    std::chrono::steady_clock::time_point next_heartbeat{};

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
            if (!owner || !owner->session_) {
                raw->disconnect();
                return;
            }
            const auto challenge = make_inspector_auth_challenge(
                owner->session_->info().session_id,
                owner->session_->info().instance_id,
                owner->session_->info().protocol_version);
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
                            owner->token_, *challenge),
                        client_id,
                        false,
                        std::chrono::steady_clock::now() +
                            owner->authentication_timeout_});
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
                if (!authenticated_client.empty() && self->owner &&
                    self->owner->session_)
                    self->owner->session_->disconnect(authenticated_client);
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
                self->on_message(std::string(msg), raw);
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
                next_heartbeat = now + std::chrono::seconds(10);
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

    std::function<void(const std::string&, events::InterprocessConnection*)> on_message;
};

InspectorServer::InspectorServer() : impl_(std::make_shared<Impl>()) {
    impl_->owner = this;
    impl_->initialize();
    impl_->on_message = [this](const std::string& data, events::InterprocessConnection* sender) {
        on_message_received(data, sender);
    };
}

InspectorServer::~InspectorServer() {
    stop();
    impl_->stop_cleanup();
    impl_->owner = nullptr;
    impl_->on_message = {};
}

bool InspectorServer::start_authenticated(InspectorServerConfig config) {
    std::lock_guard transition_lock(transition_mutex_);
    stop_requested_.store(false, std::memory_order_release);
    stop_locked();
    if (stop_requested_.load(std::memory_order_acquire))
        return false;
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
    {
        std::lock_guard lock(impl_->clients_mutex);
        impl_->max_clients =
            std::clamp<std::size_t>(config.max_clients, 1, 64);
        impl_->stopping_callbacks = false;
    }
    if (!impl_->server.start("127.0.0.1:0",
                             events::IpcTransport::Socket)) {
        stop_locked();
        return false;
    }
    if (stop_requested_.load(std::memory_order_acquire)) {
        stop_locked();
        return false;
    }
    port_ = impl_->server.bound_port();
    if (port_ == 0) {
        stop_locked();
        return false;
    }
    config.record.endpoint = "127.0.0.1:" + std::to_string(port_);
    config.record.protocol_version = session_->info().protocol_version;
    config.record.profile = session_->policy().profile();
    if (!discovery_->publish(config.record, token_)) {
        stop_locked();
        return false;
    }
    if (stop_requested_.load(std::memory_order_acquire)) {
        stop_locked();
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
    stop_requested_.store(true, std::memory_order_release);
    std::unique_lock transition_lock(transition_mutex_, std::defer_lock);
    if (!transition_lock.try_lock()) {
        bool called_from_callback = false;
        {
            std::lock_guard lock(impl_->clients_mutex);
            called_from_callback =
                impl_->callback_threads.contains(std::this_thread::get_id());
        }
        if (called_from_callback) {
            // A transition tearing down the current generation must wait for
            // this callback, so blocking on its mutex would deadlock. Defer
            // only in that interval. Otherwise keep trying until the
            // transition completes, then perform the requested stop.
            while (!transition_lock.try_lock()) {
                if (transition_waiting_for_callbacks_.load(
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

void InspectorServer::stop_locked() {
    transition_waiting_for_callbacks_.store(true,
                                            std::memory_order_release);
    {
        std::lock_guard lock(impl_->clients_mutex);
        impl_->heartbeat = {};
        impl_->stopping_callbacks = true;
    }
    impl_->server.stop();
    std::vector<std::shared_ptr<events::InterprocessConnection>> clients;
    std::vector<std::shared_ptr<Impl::OutboundClient>> outbound_clients;
    std::vector<std::string> authenticated_clients;
    {
        std::lock_guard lock(impl_->clients_mutex);
        clients = std::move(impl_->owned_clients);
        for (auto& [client, outbound] : impl_->outbound_clients) {
            (void)client;
            outbound_clients.push_back(std::move(outbound));
        }
        impl_->outbound_clients.clear();
        for (auto& outbound : impl_->retired_outbound_clients)
            outbound_clients.push_back(std::move(outbound));
        impl_->retired_outbound_clients.clear();
        for (const auto& [client, state] : impl_->authentication) {
            (void)client;
            if (state.authenticated)
                authenticated_clients.push_back(state.client_id);
        }
    }
    for (const auto& client : clients) {
        if (client)
            client->disconnect();
    }
    for (const auto& outbound : outbound_clients) {
        if (outbound)
            outbound->shutdown();
    }
    {
        std::unique_lock lock(impl_->clients_mutex);
        const auto current = impl_->callback_threads.find(
            std::this_thread::get_id());
        const auto current_callbacks =
            current == impl_->callback_threads.end() ? 0 : current->second;
        impl_->cleanup_cv.wait(lock, [this, current_callbacks] {
            return impl_->active_callbacks <= current_callbacks;
        });
        impl_->authentication.clear();
    }
    transition_waiting_for_callbacks_.store(false,
                                            std::memory_order_release);
    clients.clear();
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        if (session_) {
            for (const auto& client_id : authenticated_clients)
                session_->disconnect(client_id);
        }
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
    const auto* descriptor = find_inspector_method(event.method);
    if (event.id != 0 || !descriptor ||
        descriptor->kind != InspectorMethodKind::Event) {
        return;
    }
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        if (!session_ ||
            !session_->policy().is_available(descriptor->capability) ||
            !session_->policy().is_granted(descriptor->capability)) {
            return;
        }
    }

    auto json = encode_message(event);
    {
        std::lock_guard lock(impl_->clients_mutex);
        for (const auto& client : impl_->owned_clients) {
            const auto auth = impl_->authentication.find(client.get());
            if (auth != impl_->authentication.end() &&
                auth->second.authenticated) {
                if (const auto outbound =
                        impl_->outbound_clients.find(client.get());
                    outbound != impl_->outbound_clients.end()) {
                    outbound->second->enqueue(json);
                }
            }
        }
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
