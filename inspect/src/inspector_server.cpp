// inspector_server.cpp — TCP server for remote inspector access

#include <pulp/inspect/inspector_server.hpp>
#include <pulp/inspect/authentication.hpp>
#include <pulp/runtime/crypto.hpp>
#include <choc/text/choc_JSON.h>

#include "inspector_connected_client.hpp"
#include "inspector_publication.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <thread>
#include <utility>

namespace pulp::inspect {
namespace {

constexpr std::size_t kMaxJsonNestingDepth = 64;
constexpr std::size_t kMinimumMessageBytes = 1024;

class SensitiveTokenWiper {
public:
    explicit SensitiveTokenWiper(std::vector<std::uint8_t>& token)
        : token_(&token) {}

    ~SensitiveTokenWiper() noexcept {
        if (token_ && !token_->empty())
            pulp::runtime::secure_zero_memory(
                token_->data(), token_->size());
    }

    SensitiveTokenWiper(const SensitiveTokenWiper&) = delete;
    SensitiveTokenWiper& operator=(const SensitiveTokenWiper&) = delete;

    void disarm() noexcept {
        token_ = nullptr;
    }

private:
    std::vector<std::uint8_t>* token_;
};

bool json_nesting_is_bounded(std::string_view text) {
    std::size_t depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (const char character : text) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == '"') {
                in_string = false;
            }
            continue;
        }
        if (character == '"') {
            in_string = true;
        } else if (character == '{' || character == '[') {
            if (++depth > kMaxJsonNestingDepth)
                return false;
        } else if ((character == '}' || character == ']') && depth > 0) {
            --depth;
        }
    }
    return true;
}

} // namespace

// ── Server implementation using InterprocessConnectionServer ────────────

class InspectorServer::Impl
    : public std::enable_shared_from_this<InspectorServer::Impl> {
public:
    events::InterprocessConnectionServer server;
    std::map<events::InterprocessConnection*,
             std::shared_ptr<detail::InspectorConnectedClient>> clients;
    std::vector<std::shared_ptr<detail::InspectorOutboundClient>>
        retired_outbound_clients;
    std::mutex clients_mutex;
    std::condition_variable cleanup_cv;
    std::thread cleanup_thread;
    bool stopping_cleanup = false;
    bool stopping_callbacks = true;
    std::size_t active_callbacks = 0;
    std::map<std::thread::id, std::size_t> callback_threads;
    std::atomic<std::uint64_t> next_client_id{1};
    std::atomic<std::uint64_t> session_generation{0};
    std::size_t max_clients = 16;
    std::atomic<int> port{0};
    InspectorSession* session = nullptr;
    std::shared_ptr<InspectorMainThreadRpc> main_thread_rpc;
    detail::InspectorPublication publication;
    std::vector<std::uint8_t> token;
    std::chrono::milliseconds authentication_timeout =
        std::chrono::seconds(3);
    std::mutex lifecycle_mutex;
    // Publication lease destruction is extension code and may synchronously
    // re-enter stop(); recursive serialization keeps that teardown idempotent.
    std::recursive_mutex transition_mutex;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> transition_waiting_for_callbacks{false};
    std::atomic<bool> deferred_stop_started{false};
    std::size_t max_message_bytes = 1024u * 1024u;

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
                publication.publication_id(),
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
                if (clients.size() >= max_clients) {
                    client.reset();
                    return;
                }
                clients.insert_or_assign(
                    raw,
                    detail::InspectorConnectedClient::create(
                        client,
                        std::make_unique<InspectorAuthVerifier>(
                            token, *challenge),
                        client_id,
                        std::chrono::steady_clock::now() +
                            authentication_timeout,
                        session_generation.load(
                            std::memory_order_acquire)));
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
                std::shared_ptr<detail::InspectorConnectedClient> client;
                {
                    std::lock_guard lock(self->clients_mutex);
                    if (const auto found = self->clients.find(raw);
                        found != self->clients.end()) {
                        client = std::move(found->second);
                        self->clients.erase(found);
                        if (client->authenticated)
                            authenticated_client = client->client_id;
                        client->outbound->request_stop();
                        self->retired_outbound_clients.push_back(
                            client->outbound);
                    }
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
                    "publicationId",
                    choc::value::createString(challenge->publication_id));
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
                for (const auto& [raw, client] : clients) {
                    (void)raw;
                    if (!client->authenticated &&
                        now >= client->deadline) {
                        expired.push_back(client->connection);
                    }
                }
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
            if (!should_stop) {
                const auto refresh_generation =
                    session_generation.load(std::memory_order_acquire);
                if (!publication.refresh_if_due(now))
                    stop_generation(refresh_generation);
            }
            if (should_stop)
                break;
            lock.lock();
        }
    }

    bool start_authenticated(InspectorServerConfig config);
    void stop();
    void stop_generation(std::uint64_t expected_generation);
    void stop_locked();
    void broadcast(const InspectorMessage& event);
    int client_count();
    void on_message_received(const std::string& data,
                             events::InterprocessConnection* sender);
    bool send_response(events::InterprocessConnection* sender,
                       const InspectorMessage& response);
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
    SensitiveTokenWiper config_token_wiper(config.token);
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
    auto binding_registrations = config.domain_bindings
        ? config.domain_bindings->publication_bindings()
        : std::vector<InspectorPublicationBindingRegistration>{};
    std::vector<InspectorCapability> bound_capabilities;
    std::vector<std::shared_ptr<InspectorPublicationBinding>>
        publication_bindings;
    for (auto& registration : binding_registrations) {
        if (!registration.binding ||
            std::find(
                bound_capabilities.begin(),
                bound_capabilities.end(),
                registration.capability) != bound_capabilities.end()) {
            return false;
        }
        bound_capabilities.push_back(registration.capability);
        if (std::none_of(
                publication_bindings.begin(),
                publication_bindings.end(),
                [&registration](const auto& binding) {
                    return binding.get() == registration.binding.get();
                })) {
            publication_bindings.push_back(
                std::move(registration.binding));
        }
    }
    for (const auto& descriptor : inspector_capability_registry()) {
        if (config.session->policy().is_granted(
                descriptor.capability) &&
            capability_requires_publication_binding(
                descriptor.capability) &&
            std::find(
                bound_capabilities.begin(),
                bound_capabilities.end(),
                descriptor.capability) == bound_capabilities.end()) {
            return false;
        }
    }
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex);
        session = config.session;
        main_thread_rpc = config.main_thread_rpc
            ? std::move(config.main_thread_rpc)
            : std::make_shared<InspectorMainThreadRpc>();
        if (!main_thread_rpc->active()) {
            session = nullptr;
            main_thread_rpc.reset();
            return false;
        }
        session->set_main_thread_rpc(main_thread_rpc);
        session->resume_dispatches();
        token = std::move(config.token);
        config_token_wiper.disarm();
        session_generation.fetch_add(1, std::memory_order_acq_rel);
    }
    authentication_timeout =
        std::max(config.authentication_timeout, std::chrono::milliseconds(1));
    max_message_bytes = std::clamp<std::size_t>(
        config.max_message_bytes, kMinimumMessageBytes,
        16u * 1024u * 1024u);
    server.set_max_message_bytes(max_message_bytes);
    server.set_write_timeout(std::chrono::seconds(3));
    server.set_frame_read_timeout(
        std::max(config.frame_read_timeout, std::chrono::milliseconds(1)));
    {
        std::lock_guard lock(clients_mutex);
        max_clients =
            std::clamp<std::size_t>(config.max_clients, 1, 64);
        stopping_callbacks = false;
    }
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
    config.record.protocol_version = session->info().protocol_version;
    config.record.profile = session->policy().profile();
    if (!publication.publish(
            *config.discovery, config.record, token,
            config.heartbeat_interval,
            std::move(publication_bindings))) {
        stop_locked();
        return false;
    }
    if (stop_requested.load(std::memory_order_acquire)) {
        stop_locked();
        return false;
    }
    return true;
}

void InspectorServer::Impl::stop() {
    // Keep the shared teardown state alive if this call is the last owner and
    // must defer to a transition that is waiting for the current callback.
    const auto keep_alive = shared_from_this();
    stop_requested.store(true, std::memory_order_release);
    std::shared_ptr<InspectorMainThreadRpc> rpc;
    InspectorSession* active_session = nullptr;
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex);
        rpc = main_thread_rpc;
        active_session = session;
    }
    if (rpc && rpc->executing_on_current_thread()) {
        // InterprocessConnectionServer::stop() joins its reader callbacks. The
        // reader that issued this RPC is waiting for the current main-thread
        // operation, so joining it here would deadlock. Close admission now,
        // retain Impl, and perform the exact teardown after this operation's
        // completion lets its causal reader callback unwind.
        rpc->cancel();
        if (active_session)
            active_session->suspend_dispatches();
        {
            std::lock_guard lock(clients_mutex);
            stopping_callbacks = true;
        }
        bool expected = false;
        if (deferred_stop_started.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            const bool deferred = rpc->after_current_operation(
                [keep_alive] {
                    keep_alive->stop();
                    keep_alive->deferred_stop_started.store(
                        false, std::memory_order_release);
                });
            if (!deferred) {
                deferred_stop_started.store(false,
                                            std::memory_order_release);
            }
        }
        return;
    }
    std::unique_lock transition_lock(transition_mutex, std::defer_lock);
    if (!transition_lock.try_lock()) {
        bool called_from_callback = false;
        {
            std::lock_guard lock(clients_mutex);
            called_from_callback =
                callback_threads.contains(std::this_thread::get_id());
        }
        if (called_from_callback) {
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

void InspectorServer::Impl::stop_generation(
    std::uint64_t expected_generation) {
    std::lock_guard transition_lock(transition_mutex);
    if (session_generation.load(std::memory_order_acquire) !=
        expected_generation) {
        return;
    }
    stop_requested.store(true, std::memory_order_release);
    stop_locked();
}

void InspectorServer::Impl::stop_locked() {
    session_generation.fetch_add(1, std::memory_order_acq_rel);
    transition_waiting_for_callbacks.store(true, std::memory_order_release);
    std::shared_ptr<InspectorMainThreadRpc> rpc_to_cancel;
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex);
        rpc_to_cancel = main_thread_rpc;
    }
    if (rpc_to_cancel)
        rpc_to_cancel->cancel();
    if (session)
        session->suspend_dispatches();
    std::vector<std::shared_ptr<events::InterprocessConnection>>
        connections_to_interrupt;
    {
        std::lock_guard lock(clients_mutex);
        stopping_callbacks = true;
        connections_to_interrupt.reserve(clients.size());
        for (const auto& [raw, client] : clients) {
            (void)raw;
            connections_to_interrupt.push_back(client->connection);
        }
    }
    // Wake remote callers before draining their already-started handlers. The
    // session and all publication wiring remain attached until the drain below
    // reaches true operation completion.
    for (const auto& connection : connections_to_interrupt) {
        if (connection)
            connection->disconnect();
    }
    if (rpc_to_cancel)
        rpc_to_cancel->cancel_and_wait();
    server.stop();
    port.store(0, std::memory_order_release);
    std::vector<std::shared_ptr<detail::InspectorConnectedClient>>
        clients_to_stop;
    std::vector<std::shared_ptr<detail::InspectorOutboundClient>>
        retired_to_stop;
    std::vector<std::string> authenticated_clients;
    {
        std::lock_guard lock(clients_mutex);
        for (auto& [raw, client] : clients) {
            (void)raw;
            if (client->authenticated)
                authenticated_clients.push_back(client->client_id);
            clients_to_stop.push_back(std::move(client));
        }
        clients.clear();
        retired_to_stop = std::move(retired_outbound_clients);
        retired_outbound_clients.clear();
    }
    for (const auto& client : clients_to_stop) {
        if (client->connection)
            client->connection->disconnect();
    }
    for (const auto& client : clients_to_stop) {
        if (client->outbound)
            client->outbound->shutdown();
    }
    for (const auto& outbound : retired_to_stop) {
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
    }
    transition_waiting_for_callbacks.store(false, std::memory_order_release);
    clients_to_stop.clear();
    // Publication callbacks acquire lifecycle_mutex so detach the callback
    // before taking that mutex. The generation check makes a delayed loss
    // notification inert after a subsequent start.
    publication.clear_after_endpoint_stop();
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex);
        if (session) {
            for (const auto& client_id : authenticated_clients)
                session->disconnect(client_id);
        }
        if (session)
            session->set_main_thread_rpc({});
        session = nullptr;
        main_thread_rpc.reset();
        pulp::runtime::secure_zero_memory(token.data(), token.size());
        token.clear();
    }
}

void InspectorServer::Impl::broadcast(const InspectorMessage& event) {
    const auto* descriptor = find_inspector_method(event.method);
    if (event.id != 0 || !descriptor ||
        descriptor->kind != InspectorMethodKind::Event) {
        return;
    }
    std::uint64_t generation = 0;
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex);
        if (!session ||
            !session->policy().is_available(descriptor->capability) ||
            !session->policy().is_granted(descriptor->capability)) {
            return;
        }
        generation =
            session_generation.load(std::memory_order_acquire);
    }

    auto json = encode_message(event);
    std::vector<std::shared_ptr<events::InterprocessConnection>> overflowed;
    {
        std::lock_guard lock(clients_mutex);
        if (session_generation.load(std::memory_order_acquire) !=
            generation) {
            return;
        }
        for (const auto& [raw, client] : clients) {
            (void)raw;
            if (client->authenticated &&
                client->generation == generation &&
                client->outbound->enqueue(
                    json, inspector_event_is_lossy(event.method)) ==
                    detail::EventQueuePushResult::ReliableOverflow) {
                overflowed.push_back(client->connection);
            }
        }
    }
    for (const auto& client : overflowed)
        client->disconnect();
}

int InspectorServer::Impl::client_count() {
    std::lock_guard lock(clients_mutex);
    return static_cast<int>(std::count_if(
        clients.begin(), clients.end(),
        [](const auto& entry) {
            return entry.second->authenticated;
        }));
}

bool InspectorServer::Impl::send_response(
    events::InterprocessConnection* sender,
    const InspectorMessage& response) {
    auto payload = encode_message(response);
    if (payload.size() > max_message_bytes) {
        payload = encode_message(make_error(
            response.id,
            "Inspector response exceeds the message-size limit",
            "response_too_large"));
    }
    if (payload.size() > max_message_bytes ||
        !sender->send_message(payload)) {
        sender->disconnect();
        return false;
    }
    return true;
}

void InspectorServer::Impl::on_message_received(
    const std::string& data,
    events::InterprocessConnection* sender) {
    if (!json_nesting_is_bounded(data)) {
        send_response(sender, make_error(
            0,
            "Inspector message exceeds the JSON nesting limit",
            "message_too_deep"));
        sender->disconnect();
        return;
    }
    InspectorMessage request;
    if (!decode_message(data, request)) {
        // Invalid JSON — send error
        auto err = make_error(0, "Invalid JSON message");
        send_response(sender, err);
        return;
    }

    if (session) {
        std::string client_id;
        bool authenticated = false;
        {
            std::lock_guard lock(clients_mutex);
            const auto found = clients.find(sender);
            if (found == clients.end()) {
                sender->disconnect();
                return;
            }
            client_id = found->second->client_id;
            authenticated = found->second->authenticated;
        }

        if (!authenticated) {
            if (request.method != methods::kSessionAuthenticate ||
                request.id == 0) {
                const auto response = make_error(
                    request.id,
                    "Authenticate before issuing inspector requests",
                    "authentication_required");
                send_response(sender, response);
                sender->disconnect();
                return;
            }
            std::string proof;
            try {
                const auto params = choc::json::parse(request.params_json);
                proof = std::string(params["proof"].getString());
            } catch (...) {
            }
            std::optional<std::string> server_proof;
            {
                std::lock_guard lock(clients_mutex);
                const auto found = clients.find(sender);
                if (found != clients.end() &&
                    found->second->verifier &&
                    std::chrono::steady_clock::now() <
                        found->second->deadline) {
                    server_proof =
                        found->second->verifier->authenticate(proof);
                    found->second->verifier.reset();
                    found->second->authenticated =
                        server_proof.has_value();
                }
            }
            if (!server_proof) {
                const auto response =
                    make_error(request.id,
                               "Inspector authentication failed",
                               "authentication_failed");
                send_response(sender, response);
                sender->disconnect();
                return;
            }
            auto result = choc::value::createObject("");
            result.addMember("authenticated",
                             choc::value::createBool(true));
            result.addMember(
                "serverProof",
                choc::value::createString(*server_proof));
            send_response(sender,
                          make_response(
                              request.id,
                              choc::json::toString(result, false)));
            return;
        }

        const auto response = session->handle(client_id, request);
        if (response.id != 0 || !response.method.empty())
            send_response(sender, response);
        return;
    }

    send_response(
        sender,
        make_error(request.id,
                   "No authenticated inspector session is active",
                   "session_inactive"));
}

} // namespace pulp::inspect
