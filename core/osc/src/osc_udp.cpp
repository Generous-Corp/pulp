#include <pulp/osc/osc.hpp>

#include <thread>
#include <atomic>
#include <cerrno>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#endif

namespace pulp::osc {

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
using SockLen = int;

static int close_socket(SocketHandle sock) {
    return closesocket(sock);
}

static int socket_last_error() {
    return WSAGetLastError();
}

static bool is_recv_timeout(int error) {
    return error == WSAETIMEDOUT || error == WSAEWOULDBLOCK;
}

static bool is_recv_shutdown(int error) {
    return error == WSAENOTSOCK || error == WSAESHUTDOWN || error == WSAECONNRESET;
}

struct WSAInit {
    bool ok = false;

    WSAInit() {
        WSADATA wsa{};
        ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    }

    ~WSAInit() {
        if (ok) WSACleanup();
    }
};

static bool ensure_winsock() {
    static WSAInit init;
    return init.ok;
}
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
using SockLen = socklen_t;

static int close_socket(SocketHandle sock) {
    return close(sock);
}

static int socket_last_error() {
    return errno;
}

static bool is_recv_timeout(int error) {
    return error == EAGAIN || error == EWOULDBLOCK;
}

static bool is_recv_shutdown(int error) {
    return error == EBADF || error == ENOTSOCK;
}
#endif

// ── Sender ───────────────────────────────────────────────────────────────────

struct Sender::Impl {
    SocketHandle sock = kInvalidSocket;
    sockaddr_in dest{};
    bool connected = false;
};

Sender::Sender() : impl_(std::make_unique<Impl>()) {}
Sender::~Sender() { disconnect(); }

bool Sender::connect(const std::string& host, uint16_t port) {
    #if defined(_WIN32)
    if (!ensure_winsock()) return false;
    #endif
    impl_->sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (impl_->sock == kInvalidSocket) return false;

    impl_->dest.sin_family = AF_INET;
    impl_->dest.sin_port = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &impl_->dest.sin_addr) <= 0) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;

        addrinfo* results = nullptr;
        const std::string port_string = std::to_string(port);
        if (getaddrinfo(host.c_str(), port_string.c_str(), &hints, &results) != 0
            || results == nullptr) {
            close_socket(impl_->sock);
            impl_->sock = kInvalidSocket;
            return false;
        }

        bool resolved = false;
        for (auto* result = results; result != nullptr; result = result->ai_next) {
            if (result->ai_addr == nullptr
                || result->ai_family != AF_INET
                || static_cast<size_t>(result->ai_addrlen) < sizeof(sockaddr_in)) {
                continue;
            }

            const auto* addr = reinterpret_cast<const sockaddr_in*>(result->ai_addr);
            impl_->dest.sin_addr = addr->sin_addr;
            resolved = true;
            break;
        }

        freeaddrinfo(results);

        if (!resolved) {
            close_socket(impl_->sock);
            impl_->sock = kInvalidSocket;
            return false;
        }
    }

    impl_->connected = true;
    return true;
}

bool Sender::send(const Message& msg) {
    auto data = encode(msg);
    return send_raw(data.data(), data.size());
}

bool Sender::send_raw(const uint8_t* data, size_t size) {
    if (!impl_->connected || data == nullptr || size == 0) return false;
    auto sent = sendto(impl_->sock,
                       reinterpret_cast<const char*>(data),
                       static_cast<int>(size),
                       0,
                       reinterpret_cast<sockaddr*>(&impl_->dest),
                       static_cast<SockLen>(sizeof(impl_->dest)));
    return sent == static_cast<int>(size);
}

void Sender::disconnect() {
    if (impl_->sock != kInvalidSocket) {
        close_socket(impl_->sock);
        impl_->sock = kInvalidSocket;
    }
    impl_->connected = false;
}

bool Sender::is_connected() const { return impl_->connected; }

// ── Receiver ─────────────────────────────────────────────────────────────────

struct Receiver::Impl {
    SocketHandle sock = kInvalidSocket;
    std::thread thread;
    std::atomic<bool> running{false};
    MessageHandler handler;
    std::atomic<uint16_t> local_port{0};
};

Receiver::Receiver() : impl_(std::make_unique<Impl>()) {}

Receiver::~Receiver() { stop(); }

bool Receiver::listen(uint16_t port, MessageHandler handler) {
    #if defined(_WIN32)
    if (!ensure_winsock()) return false;
    #endif
    impl_->local_port.store(0, std::memory_order_relaxed);
    impl_->sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (impl_->sock == kInvalidSocket) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    // Keep receivers exclusive. Linux permits multiple UDP binds when all
    // participants opt into SO_REUSEADDR, which makes packet delivery
    // ambiguous for this one-handler Receiver API.
    if (bind(impl_->sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close_socket(impl_->sock);
        impl_->sock = kInvalidSocket;
        return false;
    }

    sockaddr_in bound_addr{};
    SockLen bound_len = static_cast<SockLen>(sizeof(bound_addr));
    if (getsockname(impl_->sock,
                    reinterpret_cast<sockaddr*>(&bound_addr),
                    &bound_len) < 0) {
        close_socket(impl_->sock);
        impl_->sock = kInvalidSocket;
        return false;
    }
    impl_->local_port.store(ntohs(bound_addr.sin_port), std::memory_order_relaxed);

    // Set receive timeout so we can check running_ flag
#if defined(_WIN32)
    DWORD timeout_ms = 100;
    setsockopt(impl_->sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    timeval tv{0, 100000}; // 100ms
    setsockopt(impl_->sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif

    impl_->handler = std::move(handler);
    impl_->running = true;

    impl_->thread = std::thread([this] {
        uint8_t buf[4096];
        while (impl_->running) {
            auto n = recv(impl_->sock, reinterpret_cast<char*>(buf), sizeof(buf), 0);
            if (n > 0 && impl_->handler) {
                auto msg = decode(buf, static_cast<size_t>(n));
                if (!msg.address.empty()) {
                    impl_->handler(msg);
                }
                continue;
            }

            if (n == 0) {
                continue;
            }

            const int error = socket_last_error();
            if (!impl_->running) {
                break;
            }
            if (is_recv_timeout(error)) {
                continue;
            }
            if (is_recv_shutdown(error)) {
                break;
            }
        }
    });

    return true;
}

void Receiver::stop() {
    // Shutdown ordering matters (#490). The receive thread may be
    // blocked inside recv() on this FD right now. If we close the FD
    // before joining, POSIX kernels are free to recycle that FD number
    // for an unrelated file/socket immediately — the receive thread's
    // NEXT iteration would then read from the wrong FD. That's UB.
    //
    // Correct order:
    //   1. Flip `running` so the thread exits its loop after any
    //      current recv() returns.
    //   2. shutdown() the socket to wake recv() with the sentinel
    //      error we classify as is_recv_shutdown(). The FD stays
    //      valid, so there's no race window.
    //   3. Join the thread — guaranteed to see running=false and exit.
    //   4. Only then close the FD. No one else holds it anymore.
    impl_->running = false;
    if (impl_->sock != kInvalidSocket) {
#if defined(_WIN32)
        ::shutdown(impl_->sock, SD_BOTH);
#else
        ::shutdown(impl_->sock, SHUT_RDWR);
#endif
    }
    if (impl_->thread.joinable()) impl_->thread.join();
    if (impl_->sock != kInvalidSocket) {
        close_socket(impl_->sock);
        impl_->sock = kInvalidSocket;
    }
    impl_->local_port.store(0, std::memory_order_relaxed);
}

bool Receiver::is_listening() const { return impl_->running; }

uint16_t Receiver::local_port() const {
    return impl_->local_port.load(std::memory_order_relaxed);
}

} // namespace pulp::osc
