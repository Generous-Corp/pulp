#include <pulp/osc/osc.hpp>

#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <thread>
#include <atomic>

namespace pulp::osc {

// ── Sender ───────────────────────────────────────────────────────────────────

struct Sender::Impl {
    int sock = -1;
    sockaddr_in dest{};
    bool connected = false;
};

Sender::Sender() : impl_(std::make_unique<Impl>()) {}
Sender::~Sender() { disconnect(); }

bool Sender::connect(const std::string& host, uint16_t port) {
    impl_->sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (impl_->sock < 0) return false;

    impl_->dest.sin_family = AF_INET;
    impl_->dest.sin_port = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &impl_->dest.sin_addr) <= 0) {
        // Try hostname resolution
        auto* he = gethostbyname(host.c_str());
        if (!he) { close(impl_->sock); impl_->sock = -1; return false; }
        std::memcpy(&impl_->dest.sin_addr, he->h_addr_list[0], he->h_length);
    }

    impl_->connected = true;
    return true;
}

bool Sender::send(const Message& msg) {
    if (!impl_->connected) return false;
    auto data = encode(msg);
    auto sent = sendto(impl_->sock, data.data(), data.size(), 0,
                       reinterpret_cast<sockaddr*>(&impl_->dest), sizeof(impl_->dest));
    return sent == static_cast<ssize_t>(data.size());
}

void Sender::disconnect() {
    if (impl_->sock >= 0) { close(impl_->sock); impl_->sock = -1; }
    impl_->connected = false;
}

bool Sender::is_connected() const { return impl_->connected; }

// ── Receiver ─────────────────────────────────────────────────────────────────

struct Receiver::Impl {
    int sock = -1;
    std::thread thread;
    std::atomic<bool> running{false};
    MessageHandler handler;
};

Receiver::Receiver() : impl_(std::make_unique<Impl>()) {}

Receiver::~Receiver() { stop(); }

bool Receiver::listen(uint16_t port, MessageHandler handler) {
    impl_->sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (impl_->sock < 0) return false;

    // Allow address reuse
    int opt = 1;
    setsockopt(impl_->sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(impl_->sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(impl_->sock);
        impl_->sock = -1;
        return false;
    }

    // Set receive timeout so we can check running_ flag
    timeval tv{0, 100000}; // 100ms
    setsockopt(impl_->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    impl_->handler = std::move(handler);
    impl_->running = true;

    impl_->thread = std::thread([this] {
        uint8_t buf[4096];
        while (impl_->running) {
            auto n = recv(impl_->sock, buf, sizeof(buf), 0);
            if (n > 0 && impl_->handler) {
                auto msg = decode(buf, static_cast<size_t>(n));
                if (!msg.address.empty()) {
                    impl_->handler(msg);
                }
            }
        }
    });

    return true;
}

void Receiver::stop() {
    impl_->running = false;
    if (impl_->thread.joinable()) impl_->thread.join();
    if (impl_->sock >= 0) { close(impl_->sock); impl_->sock = -1; }
}

bool Receiver::is_listening() const { return impl_->running; }

} // namespace pulp::osc
