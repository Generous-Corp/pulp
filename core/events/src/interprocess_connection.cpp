#include <pulp/events/interprocess_connection.hpp>
#include <pulp/runtime/named_pipe.hpp>
#include <pulp/runtime/socket.hpp>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>

namespace pulp::events {

using namespace pulp::runtime;

namespace {

constexpr uint32_t kDisconnectFrame = 0xFFFFFFFFu;

void encode_u32_le(uint32_t value, uint8_t* out) {
    out[0] = static_cast<uint8_t>(value & 0xFF);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    out[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    out[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

std::optional<uint16_t> parse_port(std::string_view text) {
    if (text.empty()) return std::nullopt;

    uint32_t value = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end || value > 65535) {
        return std::nullopt;
    }
    return static_cast<uint16_t>(value);
}

// Shared socket endpoint parser for connect(), create_server(),
// server.start(), and server.stop().
//
// Behavior:
// - "host:port"  → {host, port}
// - "port"       → {"", port} (caller supplies the host default)
// - Returns nullopt on empty input, empty/malformed port, port > 65535,
//   or trailing garbage in the port (consistent with the prior
//   parse_port behavior).
struct SocketEndpoint {
    std::string host;
    uint16_t port = 0;
};

std::optional<SocketEndpoint> parse_socket_endpoint(std::string_view text) {
    if (text.empty()) return std::nullopt;
    auto colon = text.find(':');
    std::optional<uint16_t> port;
    SocketEndpoint ep;
    if (colon != std::string_view::npos) {
        ep.host = std::string(text.substr(0, colon));
        port = parse_port(text.substr(colon + 1));
    } else {
        port = parse_port(text);
    }
    if (!port) return std::nullopt;
    ep.port = *port;
    return ep;
}

}  // namespace

// ── Impl ────────────────────────────────────────────────────────────────

struct InterprocessConnection::Impl {
    IpcTransport transport = IpcTransport::NamedPipe;
    NamedPipe pipe;
    Socket socket;
    std::mutex write_mutex;
    std::mutex lifecycle_mutex;
    std::condition_variable lifecycle_cv;
    bool disconnecting = false;
    std::thread::id disconnect_owner;
    std::thread::id read_thread_id;

    int raw_write(const uint8_t* data, size_t size) {
        if (transport == IpcTransport::NamedPipe)
            return pipe.write(data, size);
        else
            return socket.send(data, size);
    }

    int raw_read(uint8_t* buffer, size_t size) {
        if (transport == IpcTransport::NamedPipe)
            return pipe.read(buffer, size);
        else
            return socket.receive(buffer, size);
    }

    bool is_open() const {
        return transport == IpcTransport::NamedPipe ? pipe.is_open() : socket.is_open();
    }

    void interrupt_blocking_io() {
        if (transport == IpcTransport::NamedPipe)
            pipe.close();
        else
            socket.shutdown();
    }

    void close() {
        pipe.close();
        socket.close();
    }
};

// ── InterprocessConnection ──────────────────────────────────────────────

InterprocessConnection::InterprocessConnection() : impl_(std::make_unique<Impl>()) {}
InterprocessConnection::~InterprocessConnection() { disconnect(); }

void InterprocessConnection::set_on_connected(std::function<void()> callback) {
    std::lock_guard lock(callback_mutex_);
    on_connected = std::move(callback);
}

void InterprocessConnection::set_on_disconnected(std::function<void()> callback) {
    std::lock_guard lock(callback_mutex_);
    on_disconnected = std::move(callback);
}

void InterprocessConnection::set_on_message(
    std::function<void(const void*, size_t)> callback) {
    bool has_first_frame_handler = false;
    {
        std::lock_guard lock(callback_mutex_);
        on_message = std::move(callback);
        has_first_frame_handler = static_cast<bool>(on_message) ||
                                  static_cast<bool>(on_text_message);
    }
    if (has_first_frame_handler) release_first_dispatch_gate();
}

void InterprocessConnection::set_on_text_message(
    std::function<void(std::string_view)> callback) {
    bool has_first_frame_handler = false;
    {
        std::lock_guard lock(callback_mutex_);
        on_text_message = std::move(callback);
        has_first_frame_handler = static_cast<bool>(on_message) ||
                                  static_cast<bool>(on_text_message);
    }
    if (has_first_frame_handler) release_first_dispatch_gate();
}

void InterprocessConnection::release_first_dispatch_gate() {
    if (auto gate = first_dispatch_gate_) {
        gate->store(true, std::memory_order_release);
    }
}

bool InterprocessConnection::connect(std::string_view name, IpcTransport transport) {
    disconnect();
    write_poisoned_.store(false, std::memory_order_release);
    impl_->transport = transport;
    state_.store(IpcState::Connecting);

    bool ok = false;
    if (transport == IpcTransport::NamedPipe) {
        ok = impl_->pipe.connect_client(name);
    } else {
        // connect() requires "host:port" — host alone is meaningless
        // for a client (no default), so reject endpoints that omit it.
        auto endpoint = parse_socket_endpoint(name);
        if (!endpoint || endpoint->host.empty()) {
            state_.store(IpcState::Error);
            return false;
        }
        impl_->socket.create(SocketType::TCP);
        impl_->socket.set_write_timeout(
            std::chrono::milliseconds(
                write_timeout_ms_.load(std::memory_order_relaxed)));
        ok = impl_->socket.connect(endpoint->host, endpoint->port);
    }

    if (ok) {
        state_.store(IpcState::Connected);
        connection_made();
        std::function<void()> connected_callback;
        {
            std::lock_guard lock(callback_mutex_);
            connected_callback = on_connected;
        }
        if (connected_callback) connected_callback();
        start_read_thread();
    } else {
        IpcState expected = IpcState::Connecting;
        state_.compare_exchange_strong(expected, IpcState::Error);
    }
    return ok;
}

bool InterprocessConnection::create_server(std::string_view name, IpcTransport transport,
                                            int /*timeout_ms*/) {
    disconnect();
    write_poisoned_.store(false, std::memory_order_release);
    impl_->transport = transport;
    state_.store(IpcState::Connecting);

    bool ok = false;
    if (transport == IpcTransport::NamedPipe) {
        ok = impl_->pipe.create_server(name);
    } else {
        // Single-client server: host may be omitted, in which case
        // we bind on all interfaces.
        auto endpoint = parse_socket_endpoint(name);
        if (!endpoint) {
            state_.store(IpcState::Error);
            return false;
        }
        const std::string& host = endpoint->host.empty()
                                      ? std::string{"0.0.0.0"}
                                      : endpoint->host;
        impl_->socket.create(SocketType::TCP);
        impl_->socket.set_write_timeout(
            std::chrono::milliseconds(
                write_timeout_ms_.load(std::memory_order_relaxed)));
        if (impl_->socket.bind(host, endpoint->port) && impl_->socket.listen(1)) {
            auto client = impl_->socket.accept();
            if (client) {
                impl_->socket = std::move(*client);
                ok = true;
            }
        }
    }

    if (ok) {
        state_.store(IpcState::Connected);
        connection_made();
        std::function<void()> connected_callback;
        {
            std::lock_guard lock(callback_mutex_);
            connected_callback = on_connected;
        }
        if (connected_callback) connected_callback();
        start_read_thread();
    } else {
        IpcState expected = IpcState::Connecting;
        state_.compare_exchange_strong(expected, IpcState::Error);
    }
    return ok;
}

void InterprocessConnection::disconnect() {
    const auto caller = std::this_thread::get_id();
    {
        std::unique_lock lifecycle_lock(impl_->lifecycle_mutex);
        if (impl_->disconnecting) {
            if (impl_->disconnect_owner == caller ||
                impl_->read_thread_id == caller) {
                return;
            }
            impl_->lifecycle_cv.wait(lifecycle_lock, [this] {
                return !impl_->disconnecting;
            });
            return;
        }
        impl_->disconnecting = true;
        impl_->disconnect_owner = caller;
    }

    const bool was_connected = state_.exchange(IpcState::Disconnected) == IpcState::Connected;
    running_.store(false);
    impl_->interrupt_blocking_io();
    std::unique_lock write_lock(impl_->write_mutex);
    if (read_thread_.joinable()) {
        if (read_thread_.get_id() == std::this_thread::get_id())
            read_thread_.detach();
        else
            read_thread_.join();
    }
    impl_->close();
    write_lock.unlock();

    if (was_connected) {
        connection_lost();
        std::function<void()> disconnected_callback;
        {
            std::lock_guard lock(callback_mutex_);
            disconnected_callback = on_disconnected;
        }
        if (disconnected_callback) disconnected_callback();
    }

    {
        std::lock_guard lifecycle_lock(impl_->lifecycle_mutex);
        impl_->disconnecting = false;
        impl_->disconnect_owner = {};
        impl_->read_thread_id = {};
    }
    impl_->lifecycle_cv.notify_all();
}

bool InterprocessConnection::send_message(const void* data, size_t size) {
    if (!is_connected()) return false;
    if (write_poisoned_.load(std::memory_order_acquire)) return false;
    if (size > max_message_bytes_.load(std::memory_order_relaxed)) return false;

    std::unique_lock<std::mutex> lock(impl_->write_mutex);
    if (!is_connected() ||
        write_poisoned_.load(std::memory_order_acquire))
        return false;

    // Write 4-byte little-endian length header
    uint32_t len = static_cast<uint32_t>(size);
    uint8_t header[4];
    encode_u32_le(len, header);

    const auto configured_timeout = std::chrono::milliseconds(
        write_timeout_ms_.load(std::memory_order_relaxed));
    const bool has_frame_deadline =
        impl_->transport == IpcTransport::Socket &&
        configured_timeout > std::chrono::milliseconds(0);
    const auto frame_deadline =
        std::chrono::steady_clock::now() + configured_timeout;
    auto write_with_deadline = [&](const uint8_t* bytes,
                                   std::size_t byte_count) {
        if (has_frame_deadline) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= frame_deadline)
                return -1;
            auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    frame_deadline - now);
            if (remaining <= std::chrono::milliseconds(0))
                remaining = std::chrono::milliseconds(1);
            if (!impl_->socket.set_write_timeout(remaining))
                return -1;
        }
        const int written = impl_->raw_write(bytes, byte_count);
        if (has_frame_deadline &&
            std::chrono::steady_clock::now() >= frame_deadline) {
            return -1;
        }
        return written;
    };

    // Write header with retry for short writes
    size_t header_sent = 0;
    while (header_sent < 4) {
        int n = write_with_deadline(header + header_sent, 4 - header_sent);
        if (n <= 0) {
            write_poisoned_.store(true, std::memory_order_release);
            lock.unlock();
            disconnect();
            return false;
        }
        header_sent += static_cast<size_t>(n);
    }

    // Write payload with retry for short writes
    if (size > 0) {
        size_t payload_sent = 0;
        auto* payload = static_cast<const uint8_t*>(data);
        while (payload_sent < size) {
            int n = write_with_deadline(payload + payload_sent,
                                        size - payload_sent);
            if (n <= 0) {
                write_poisoned_.store(true, std::memory_order_release);
                lock.unlock();
                disconnect();
                return false;
            }
            payload_sent += static_cast<size_t>(n);
        }
    }
    return true;
}

bool InterprocessConnection::send_message(std::string_view message) {
    return send_message(message.data(), message.size());
}

void InterprocessConnection::set_max_message_bytes(std::size_t bytes) {
    max_message_bytes_.store(
        std::clamp<std::size_t>(bytes, 1, UINT32_MAX),
        std::memory_order_relaxed);
}

void InterprocessConnection::set_write_timeout(
    std::chrono::milliseconds timeout) {
    const auto value = std::max(timeout, std::chrono::milliseconds(0));
    write_timeout_ms_.store(value.count(), std::memory_order_relaxed);
    std::lock_guard write_lock(impl_->write_mutex);
    if (impl_->transport == IpcTransport::Socket && impl_->socket.is_open())
        impl_->socket.set_write_timeout(value);
}


void InterprocessConnection::start_read_thread() {
    running_.store(true);
    const auto start_gate = std::make_shared<std::atomic<bool>>(false);
    read_thread_ = std::thread([this, start_gate]() {
        while (!start_gate->load(std::memory_order_acquire))
            std::this_thread::yield();
        read_loop();
    });
    {
        std::lock_guard lifecycle_lock(impl_->lifecycle_mutex);
        impl_->read_thread_id = read_thread_.get_id();
    }
    start_gate->store(true, std::memory_order_release);
}

void InterprocessConnection::read_loop() {
    const auto alive = alive_.capture();
    std::vector<uint8_t> buffer;
    auto notify_lost = [this, alive]() {
        running_.store(false);
        if (state_.exchange(IpcState::Disconnected) == IpcState::Connected) {
            connection_lost();
            if (!runtime::AliveToken::is_alive(alive)) return;
            std::function<void()> disconnected_callback;
            {
                std::lock_guard lock(callback_mutex_);
                disconnected_callback = on_disconnected;
            }
            if (disconnected_callback) disconnected_callback();
        }
    };

    auto read_exact = [this](uint8_t* dst, size_t size) {
        size_t read_so_far = 0;
        while (read_so_far < size && running_.load()) {
            int got = impl_->raw_read(dst + read_so_far, size - read_so_far);
            if (got <= 0)
                return false;
            read_so_far += static_cast<size_t>(got);
        }
        return read_so_far == size;
    };

    while (running_.load()) {
        // Read 4-byte length header
        uint8_t header[4];
        if (!read_exact(header, 4)) {
            if (running_.load())
                notify_lost();
            return;
        }

        uint32_t msg_len = static_cast<uint32_t>(header[0]) |
                           (static_cast<uint32_t>(header[1]) << 8) |
                           (static_cast<uint32_t>(header[2]) << 16) |
                           (static_cast<uint32_t>(header[3]) << 24);
        if (msg_len == kDisconnectFrame) {
            notify_lost();
            return;
        }
        if (msg_len > max_message_bytes_.load(std::memory_order_relaxed)) {
            notify_lost();
            return;
        }

        // Read payload
        buffer.resize(msg_len);
        if (!read_exact(buffer.data(), msg_len)) {
            if (running_.load())
                notify_lost();
            return;
        }

        std::function<void(const void*, size_t)> message_callback;
        std::function<void(std::string_view)> text_callback;
        const bool wait_for_first_callback =
            defer_first_dispatch_until_callback_.exchange(false);
        if (wait_for_first_callback) {
            auto gate = first_dispatch_gate_;
            while (running_.load() && gate &&
                   !gate->load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        {
            std::lock_guard lock(callback_mutex_);
            message_callback = on_message;
            text_callback = on_text_message;
        }

        // Dispatch message
        message_received(buffer.data(), msg_len);
        if (!runtime::AliveToken::is_alive(alive)) return;
        if (message_callback) message_callback(buffer.data(), msg_len);
        if (!runtime::AliveToken::is_alive(alive)) return;

        std::string_view text_view(reinterpret_cast<const char*>(buffer.data()), msg_len);
        message_received(text_view);
        if (!runtime::AliveToken::is_alive(alive)) return;
        if (text_callback) text_callback(text_view);
        if (!runtime::AliveToken::is_alive(alive)) return;
    }
}

// ── InterprocessConnectionServer ────────────────────────────────────────

struct InterprocessConnectionServer::ServerImpl {
    IpcTransport transport = IpcTransport::Socket;
    Socket listen_socket;
    std::string name;
};

InterprocessConnectionServer::InterprocessConnectionServer()
    : server_impl_(std::make_unique<ServerImpl>()) {}

InterprocessConnectionServer::~InterprocessConnectionServer() { stop(); }

std::uint16_t InterprocessConnectionServer::bound_port() const {
    if (!is_running() || server_impl_->transport != IpcTransport::Socket)
        return 0;
    return server_impl_->listen_socket.local_port();
}

void InterprocessConnectionServer::set_max_message_bytes(std::size_t bytes) {
    max_message_bytes_.store(
        std::clamp<std::size_t>(bytes, 1, UINT32_MAX),
        std::memory_order_relaxed);
}

void InterprocessConnectionServer::set_write_timeout(
    std::chrono::milliseconds timeout) {
    write_timeout_ms_.store(
        std::max(timeout, std::chrono::milliseconds(0)).count(),
        std::memory_order_relaxed);
}

bool InterprocessConnectionServer::start(std::string_view name, IpcTransport transport) {
    stop();
    server_impl_->transport = transport;
    server_impl_->name = std::string(name);

    if (transport == IpcTransport::Socket) {
        // Multi-client listener: host may be omitted, in which case
        // we bind on all interfaces.
        auto endpoint = parse_socket_endpoint(name);
        if (!endpoint) return false;
        const std::string& host = endpoint->host.empty()
                                      ? std::string{"0.0.0.0"}
                                      : endpoint->host;
        server_impl_->listen_socket.create(SocketType::TCP);
        if (!server_impl_->listen_socket.bind(host, endpoint->port)) return false;
        if (!server_impl_->listen_socket.listen(5)) return false;
    }

    running_.store(true);
    accept_thread_ = std::thread([this]() {
        while (running_.load()) {
            if (server_impl_->transport == IpcTransport::Socket) {
                auto client_sock = server_impl_->listen_socket.accept();
                if (!client_sock) continue;
                if (!running_.load()) {
                    client_sock->close();
                    break;
                }

                auto conn = std::make_unique<InterprocessConnection>();
                conn->set_max_message_bytes(
                    max_message_bytes_.load(std::memory_order_relaxed));
                auto first_dispatch_gate = std::make_shared<std::atomic<bool>>(false);
                // Inject the accepted socket via friend access
                conn->impl_->transport = IpcTransport::Socket;
                conn->impl_->socket = std::move(*client_sock);
                conn->set_write_timeout(std::chrono::milliseconds(
                    write_timeout_ms_.load(std::memory_order_relaxed)));
                conn->state_.store(IpcState::Connected);
                conn->write_poisoned_.store(false, std::memory_order_release);
                conn->defer_first_dispatch_until_callback_.store(true);
                conn->first_dispatch_gate_ = first_dispatch_gate;
                conn->connection_made();
                std::function<void()> connected_callback;
                {
                    std::lock_guard lock(conn->callback_mutex_);
                    connected_callback = conn->on_connected;
                }
                if (connected_callback) connected_callback();

                // Start read thread while we still own the connection,
                // then hand off. This avoids use-after-free if the
                // callback destroys the connection immediately.
                // The read thread uses atomics so it can start before
                // handlers are set. Its first dispatch waits for the
                // accepted callback to finish installing handlers, preserving
                // an eager client's first frame without relying on a fixed
                // timeout or handing out a connection whose lifetime we no
                // longer control.
                conn->start_read_thread();

                if (on_client_connected)
                    on_client_connected(std::move(conn));
                else
                    client_connected(std::move(conn));
                first_dispatch_gate->store(true, std::memory_order_release);
            }
        }
    });

    return true;
}

void InterprocessConnectionServer::stop() {
    const bool was_running = running_.exchange(false);
    if (was_running && server_impl_->transport == IpcTransport::Socket &&
        server_impl_->listen_socket.is_open()) {
        // Close before join so accept() cannot keep the accept thread blocked
        // on a listener that is no longer meant to serve clients.
        server_impl_->listen_socket.close();
    }
    if (accept_thread_.joinable()) accept_thread_.join();
    // Idempotent; keeps non-socket and partially started server cleanup simple.
    server_impl_->listen_socket.close();
    clients_.clear();
}

void InterprocessConnectionServer::client_connected(
    std::unique_ptr<InterprocessConnection> connection) {
    clients_.push_back(std::move(connection));
}

}  // namespace pulp::events
