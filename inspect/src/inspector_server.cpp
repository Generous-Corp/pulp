// inspector_server.cpp — TCP server for remote inspector access

#include <pulp/inspect/inspector_server.hpp>
#include <pulp/runtime/system.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace pulp::inspect {

static constexpr int kDefaultPort = 9147;

// ── Server implementation using InterprocessConnectionServer ────────────

class InspectorServer::Impl {
public:
    events::InterprocessConnectionServer server;
    std::vector<events::InterprocessConnection*> client_ptrs;
    std::vector<std::unique_ptr<events::InterprocessConnection>> owned_clients;
    std::vector<std::pair<events::InterprocessConnection*, std::chrono::steady_clock::time_point>> disconnected_at;
    std::mutex clients_mutex;
    std::condition_variable cleanup_cv;
    std::thread cleanup_thread;
    bool stopping_cleanup = false;

    Impl() {
        cleanup_thread = std::thread([this]() { cleanup_loop(); });
        server.on_client_connected = [this](std::unique_ptr<events::InterprocessConnection> conn) {
            prune_disconnected_clients();
            auto* raw = conn.get();
            raw->on_text_message = [this, raw](std::string_view msg) {
                on_message(std::string(msg), raw);
            };
            raw->on_disconnected = [this, raw]() {
                std::lock_guard lock(clients_mutex);
                client_ptrs.erase(std::remove(client_ptrs.begin(), client_ptrs.end(), raw), client_ptrs.end());
                auto it = std::find_if(disconnected_at.begin(), disconnected_at.end(),
                                       [raw](const auto& entry) { return entry.first == raw; });
                if (it == disconnected_at.end())
                    disconnected_at.emplace_back(raw, std::chrono::steady_clock::now());
                else
                    it->second = std::chrono::steady_clock::now();
                cleanup_cv.notify_one();
            };
            {
                std::lock_guard lock(clients_mutex);
                client_ptrs.push_back(raw);
                owned_clients.push_back(std::move(conn));
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
            if (disconnected_at.empty()) {
                cleanup_cv.wait(lock, [this] {
                    return stopping_cleanup || !disconnected_at.empty();
                });
                continue;
            }
            cleanup_cv.wait_for(lock, std::chrono::milliseconds(50), [this] {
                return stopping_cleanup;
            });
            if (!stopping_cleanup)
                prune_disconnected_clients_locked();
        }
    }

    void prune_disconnected_clients() {
        std::lock_guard lock(clients_mutex);
        prune_disconnected_clients_locked();
    }

    void prune_disconnected_clients_locked() {
        client_ptrs.erase(std::remove_if(client_ptrs.begin(), client_ptrs.end(),
                                         [](auto* c) { return !c || !c->is_connected(); }),
                          client_ptrs.end());

        const auto now = std::chrono::steady_clock::now();
        std::vector<events::InterprocessConnection*> reclaimable;
        for (const auto& [client, disconnected] : disconnected_at) {
            if (now - disconnected >= std::chrono::milliseconds(50))
                reclaimable.push_back(client);
        }

        owned_clients.erase(
            std::remove_if(owned_clients.begin(), owned_clients.end(),
                           [&reclaimable](const auto& c) {
                               return c && !c->is_connected() &&
                                      std::find(reclaimable.begin(), reclaimable.end(), c.get()) !=
                                          reclaimable.end();
                           }),
            owned_clients.end());
        disconnected_at.erase(
            std::remove_if(disconnected_at.begin(), disconnected_at.end(),
                           [&reclaimable](const auto& entry) {
                               return std::find(reclaimable.begin(), reclaimable.end(), entry.first) !=
                                      reclaimable.end();
                           }),
            disconnected_at.end());
    }

    std::function<void(const std::string&, events::InterprocessConnection*)> on_message;
};

InspectorServer::InspectorServer() : impl_(std::make_unique<Impl>()) {
    impl_->on_message = [this](const std::string& data, events::InterprocessConnection* sender) {
        on_message_received(data, sender);
    };
}

InspectorServer::~InspectorServer() {
    stop();
}

bool InspectorServer::start(int port) {
    if (port == 0) {
        // Check env var
        if (auto env = pulp::runtime::get_env("PULP_INSPECTOR_PORT")) {
            try { port = std::stoi(*env); } catch (...) {}
        }
        if (port == 0) port = kDefaultPort;
    }

    port_ = port;

    if (!impl_->server.start(std::to_string(port), events::IpcTransport::Socket)) {
        return false;
    }

    advertise_port();
    return true;
}

void InspectorServer::stop() {
    impl_->server.stop();
    std::vector<std::unique_ptr<events::InterprocessConnection>> clients;
    {
        std::lock_guard lock(impl_->clients_mutex);
        impl_->client_ptrs.clear();
        clients = std::move(impl_->owned_clients);
    }
    clients.clear();
}

void InspectorServer::set_request_handler(RequestHandler handler) {
    handler_ = std::move(handler);
}

void InspectorServer::broadcast(const InspectorMessage& event) {
    auto json = encode_message(event);
    std::lock_guard lock(impl_->clients_mutex);
    impl_->prune_disconnected_clients_locked();
    for (auto* client : impl_->client_ptrs) {
        client->send_message(json.data(), json.size());
    }
}

int InspectorServer::client_count() const {
    std::lock_guard lock(impl_->clients_mutex);
    impl_->prune_disconnected_clients_locked();
    return static_cast<int>(impl_->client_ptrs.size());
}

void InspectorServer::advertise_port() const {
    // Write port file so CLI tools can discover us
    std::string tmp_dir;
#ifdef _WIN32
    if (auto env = pulp::runtime::get_env("TEMP")) tmp_dir = *env;
    else tmp_dir = ".";
#else
    if (auto env = pulp::runtime::get_env("TMPDIR")) tmp_dir = *env;
    else tmp_dir = "/tmp";
#endif

    auto port_file = tmp_dir + "/pulp-inspector-" + std::to_string(getpid()) + ".port";
    std::ofstream f(port_file);
    if (f.good()) {
        f << port_;
    }
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

    if (handler_) {
        auto response = handler_(request);
        if (response.id != 0 || !response.method.empty()) {
            auto json = encode_message(response);
            sender->send_message(json.data(), json.size());
        }
    }
}

} // namespace pulp::inspect
