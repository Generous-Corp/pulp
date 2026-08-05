#include "unsafe_legacy_inspector_server.hpp"

#include <pulp/runtime/system.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace pulp::inspect::test {

class UnsafeLegacyInspectorServer::Impl {
public:
    events::InterprocessConnectionServer server;
    std::vector<events::InterprocessConnection*> clients;
    std::vector<std::unique_ptr<events::InterprocessConnection>> owned;
    std::vector<std::pair<events::InterprocessConnection*,
                          std::chrono::steady_clock::time_point>>
        disconnected;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::thread cleanup;
    bool stopping = false;
    std::function<void(const std::string&, events::InterprocessConnection*)>
        on_message;

    Impl() {
        cleanup = std::thread([this] {
            std::unique_lock lock(mutex);
            while (!stopping) {
                cv.wait_for(lock, std::chrono::milliseconds(50));
                if (stopping)
                    break;
                prune_locked();
            }
        });
        server.on_client_connected =
            [this](std::unique_ptr<events::InterprocessConnection> connection) {
                auto* raw = connection.get();
                raw->set_on_text_message(
                    [this, raw](std::string_view message) {
                        on_message(std::string(message), raw);
                    });
                raw->set_on_disconnected([this, raw] {
                    std::lock_guard lock(mutex);
                    clients.erase(
                        std::remove(clients.begin(), clients.end(), raw),
                        clients.end());
                    disconnected.emplace_back(
                        raw, std::chrono::steady_clock::now());
                    cv.notify_one();
                });
                std::lock_guard lock(mutex);
                clients.push_back(raw);
                owned.push_back(std::move(connection));
            };
    }

    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        cv.notify_one();
        if (cleanup.joinable())
            cleanup.join();
    }

    void prune_locked() {
        const auto now = std::chrono::steady_clock::now();
        owned.erase(
            std::remove_if(
                owned.begin(), owned.end(), [&](const auto& connection) {
                    if (!connection || connection->is_connected())
                        return false;
                    const auto found = std::find_if(
                        disconnected.begin(), disconnected.end(),
                        [&](const auto& item) {
                            return item.first == connection.get() &&
                                   now - item.second >=
                                       std::chrono::milliseconds(50);
                        });
                    return found != disconnected.end();
                }),
            owned.end());
        disconnected.erase(
            std::remove_if(
                disconnected.begin(), disconnected.end(),
                [&](const auto& item) {
                    return now - item.second >=
                           std::chrono::milliseconds(50);
                }),
            disconnected.end());
    }
};

UnsafeLegacyInspectorServer::UnsafeLegacyInspectorServer()
    : impl_(std::make_unique<Impl>()) {
    impl_->on_message = [this](const std::string& message,
                               events::InterprocessConnection* sender) {
        on_message_received(message, sender);
    };
}

UnsafeLegacyInspectorServer::~UnsafeLegacyInspectorServer() {
    stop();
}

bool UnsafeLegacyInspectorServer::start(int port) {
    stop();
    if (port < 0 || port > 65535)
        return false;
    impl_->server.set_max_message_bytes(1024u * 1024u);
    if (!impl_->server.start("127.0.0.1:" + std::to_string(port),
                             events::IpcTransport::Socket))
        return false;
    port_ = impl_->server.bound_port();
    if (port_ == 0) {
        impl_->server.stop();
        return false;
    }
    advertise_port();
    return true;
}

void UnsafeLegacyInspectorServer::stop() {
    impl_->server.stop();
    std::vector<std::unique_ptr<events::InterprocessConnection>> clients;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->clients.clear();
        clients = std::move(impl_->owned);
    }
    clients.clear();
    if (!discovery_file_.empty()) {
        std::error_code error;
        std::filesystem::remove(discovery_file_, error);
        discovery_file_.clear();
    }
    port_ = 0;
}

void UnsafeLegacyInspectorServer::set_request_handler(
    RequestHandler handler) {
    handler_ = std::move(handler);
}

void UnsafeLegacyInspectorServer::broadcast(const InspectorMessage& event) {
    const auto message = encode_message(event);
    std::lock_guard lock(impl_->mutex);
    impl_->prune_locked();
    for (auto* client : impl_->clients)
        client->send_message(message);
}

int UnsafeLegacyInspectorServer::client_count() const {
    std::lock_guard lock(impl_->mutex);
    impl_->prune_locked();
    return static_cast<int>(impl_->clients.size());
}

void UnsafeLegacyInspectorServer::advertise_port() const {
    std::string temporary;
#ifdef _WIN32
    temporary = pulp::runtime::get_env("TEMP").value_or(".");
#else
    temporary = pulp::runtime::get_env("TMPDIR").value_or("/tmp");
#endif
    discovery_file_ = temporary + "/pulp-inspector-" +
                      std::to_string(getpid()) + ".port";
    std::ofstream output(discovery_file_, std::ios::trunc);
    if (output)
        output << port_;
}

void UnsafeLegacyInspectorServer::on_message_received(
    const std::string& data,
    events::InterprocessConnection* sender) {
    InspectorMessage request;
    if (!decode_message(data, request)) {
        sender->send_message(
            encode_message(make_error(0, "Invalid JSON message")));
        return;
    }
    if (!handler_)
        return;
    const auto response = handler_(request);
    if (response.id != 0 || !response.method.empty())
        sender->send_message(encode_message(response));
}

} // namespace pulp::inspect::test
