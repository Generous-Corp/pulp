#include <pulp/osc/osc.hpp>
#include <pulp/osc/bundle.hpp>

#include <thread>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <utility>

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

namespace {

constexpr size_t kMaxUdpDatagramSize = 65536;

} // namespace

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
    auto sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == kInvalidSocket) return false;

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &dest.sin_addr) <= 0) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;

        addrinfo* results = nullptr;
        const std::string port_string = std::to_string(port);
        if (getaddrinfo(host.c_str(), port_string.c_str(), &hints, &results) != 0
            || results == nullptr) {
            close_socket(sock);
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
            dest.sin_addr = addr->sin_addr;
            resolved = true;
            break;
        }

        freeaddrinfo(results);

        if (!resolved) {
            close_socket(sock);
            return false;
        }
    }

    if (impl_->sock != kInvalidSocket) close_socket(impl_->sock);
    impl_->sock = sock;
    impl_->dest = dest;
    impl_->connected = true;
    return true;
}

bool Sender::send(const Message& msg) {
    auto data = encode(msg);
    return send_raw(data.data(), data.size());
}

bool Sender::send(const Bundle& bundle) {
    auto data = bundle.serialize();
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
    std::atomic<SocketHandle> sock{kInvalidSocket};
    std::thread thread;
    std::atomic<bool> running{false};
    ReceiverOptions options;
    std::atomic<uint16_t> local_port{0};

    ~Impl() {
        const auto old_sock = sock.exchange(kInvalidSocket, std::memory_order_acq_rel);
        if (old_sock != kInvalidSocket) close_socket(old_sock);
    }

    bool is_running() const {
        return running.load(std::memory_order_acquire);
    }

    void dispatch_error(std::string_view reason) {
        if (!is_running()) return;
        if (options.on_error) options.on_error(reason);
    }

    void dispatch_message(const Message& msg) {
        if (!is_running()) return;
        if (options.on_message) {
            options.on_message(msg);
            if (!is_running()) return;
        }
        for (const auto& route : options.routes) {
            if (!is_running()) return;
            if (!route.handler) continue;
            if (route.address_pattern.empty()
                || address_matches(route.address_pattern, msg.address)) {
                route.handler(msg);
            }
        }
    }

    void dispatch_bundle_messages(const Bundle& bundle) {
        for (const auto& element : bundle.elements) {
            if (!is_running()) return;
            if (element.is_message()) {
                dispatch_message(element.message());
            } else if (element.is_bundle()) {
                dispatch_bundle_messages(element.bundle());
            }
        }
    }

    void dispatch_bundle(const Bundle& bundle) {
        if (!is_running()) return;
        if (options.on_bundle) {
            options.on_bundle(bundle);
            if (!is_running()) return;
        }
        dispatch_bundle_messages(bundle);
    }
};

Receiver::Receiver() : impl_(std::make_shared<Impl>()) {}

Receiver::~Receiver() { stop_impl(true); }

bool Receiver::listen(uint16_t port, MessageHandler handler) {
    ReceiverOptions options;
    options.on_message = std::move(handler);
    return listen_with_options(port, std::move(options));
}

bool Receiver::listen_with_options(uint16_t port, ReceiverOptions options) {
    #if defined(_WIN32)
    if (!ensure_winsock()) return false;
    #endif
    auto impl = impl_;
    if (impl->running.load(std::memory_order_acquire)) return false;
    if (impl->thread.joinable()) {
        if (impl->thread.get_id() == std::this_thread::get_id()) return false;
        stop();
    }
    impl->local_port.store(0, std::memory_order_relaxed);
    auto sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == kInvalidSocket) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    // Keep receivers exclusive. Linux permits multiple UDP binds when all
    // participants opt into SO_REUSEADDR, which makes packet delivery
    // ambiguous for this one-handler Receiver API.
#if defined(_WIN32)
    BOOL exclusive_addr = TRUE;
    if (setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char*>(&exclusive_addr),
                   sizeof(exclusive_addr)) < 0) {
        close_socket(sock);
        return false;
    }
#endif
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close_socket(sock);
        return false;
    }

    sockaddr_in bound_addr{};
    SockLen bound_len = static_cast<SockLen>(sizeof(bound_addr));
    if (getsockname(sock,
                    reinterpret_cast<sockaddr*>(&bound_addr),
                    &bound_len) < 0) {
        close_socket(sock);
        return false;
    }
    impl->local_port.store(ntohs(bound_addr.sin_port), std::memory_order_relaxed);

    // Set receive timeout so we can check running_ flag
#if defined(_WIN32)
    DWORD timeout_ms = 100;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms)) < 0) {
        close_socket(sock);
        impl->local_port.store(0, std::memory_order_relaxed);
        return false;
    }
#else
    timeval tv{0, 100000}; // 100ms
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&tv), sizeof(tv)) < 0) {
        close_socket(sock);
        impl->local_port.store(0, std::memory_order_relaxed);
        return false;
    }
#endif

    impl->options = std::move(options);
    impl->sock.store(sock, std::memory_order_release);
    impl->running = true;

    impl->thread = std::thread([impl] {
        std::vector<uint8_t> buf(kMaxUdpDatagramSize);
        while (impl->running) {
            const auto sock = impl->sock.load(std::memory_order_acquire);
            if (sock == kInvalidSocket)
                break;
            auto n = recv(sock,
                          reinterpret_cast<char*>(buf.data()),
                          static_cast<int>(buf.size()),
                          0);
            if (n > 0) {
                const auto packet_size = static_cast<size_t>(n);
                if (packet_size >= 8 && std::memcmp(buf.data(), "#bundle", 8) == 0) {
                    auto bundle = Bundle::deserialize(buf.data(), packet_size);
                    if (bundle) {
                        impl->dispatch_bundle(*bundle);
                    } else {
                        impl->dispatch_error("malformed OSC bundle");
                    }
                } else {
                    auto msg = decode(buf.data(), packet_size);
                    if (!msg.address.empty()) {
                        impl->dispatch_message(msg);
                    } else {
                        impl->dispatch_error("malformed OSC message");
                    }
                }
                continue;
            }

            if (n == 0) {
                impl->dispatch_error("malformed OSC message");
                continue;
            }

            const int error = socket_last_error();
            if (!impl->running) {
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
    stop_impl(false);
}

void Receiver::stop_impl(bool destroying) {
    // Shutdown ordering matters. The receive thread may be
    // blocked inside recv() on this FD right now. If we close the FD
    // before joining, POSIX kernels are free to recycle that FD number
    // for an unrelated file/socket immediately — the receive thread's
    // NEXT iteration would then read from the wrong FD. That's UB.
    //
    // Correct order for callers outside the receiver thread:
    //   1. Flip `running` so the thread exits its loop after any
    //      current recv() returns.
    //   2. shutdown() the socket to wake recv() with the sentinel
    //      error we classify as is_recv_shutdown(). The FD stays
    //      valid, so there's no race window.
    //   3. Join the thread — guaranteed to see running=false and exit.
    //   4. Only then close the FD. No one else holds it anymore.
    // If a callback calls stop() on the receiver thread itself, close the
    // socket now so the port is released immediately, but leave thread join to
    // the next outside stop()/destructor call. Joining here would self-join and
    // terminate.
    auto impl = impl_;
    if (!impl) return;

    const bool called_from_receiver_thread =
        impl->thread.joinable()
        && impl->thread.get_id() == std::this_thread::get_id();

    impl->running = false;
    const auto sock = impl->sock.exchange(kInvalidSocket, std::memory_order_acq_rel);
    if (sock != kInvalidSocket) {
#if defined(_WIN32)
        ::shutdown(sock, SD_BOTH);
#else
        ::shutdown(sock, SHUT_RDWR);
#endif
    }
    if (called_from_receiver_thread) {
        impl->local_port.store(0, std::memory_order_relaxed);
        if (sock != kInvalidSocket) close_socket(sock);
        if (destroying && impl->thread.joinable()) impl->thread.detach();
        return;
    }
    if (impl->thread.joinable()) impl->thread.join();
    if (sock != kInvalidSocket) close_socket(sock);
    impl->local_port.store(0, std::memory_order_relaxed);
    impl->options = {};
}

bool Receiver::is_listening() const {
    return impl_->running.load(std::memory_order_acquire);
}

uint16_t Receiver::local_port() const {
    return impl_->local_port.load(std::memory_order_relaxed);
}

} // namespace pulp::osc
