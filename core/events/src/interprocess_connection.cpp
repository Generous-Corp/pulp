#include <pulp/events/interprocess_connection.hpp>
#include <pulp/runtime/named_pipe.hpp>
#include <pulp/runtime/socket.hpp>
#include <charconv>
#include <cstring>
#include <mutex>
#include <optional>

namespace pulp::events {

using namespace pulp::runtime;

namespace {

constexpr uint32_t kDisconnectFrame = 0xFFFFFFFFu;
constexpr uint32_t kMaxMessageBytes = 64u * 1024u * 1024u;

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

// Companion-track U-8 (planning/2026-05-17-refactor-roadmap-final.md).
// Consolidates host:port parsing previously duplicated across
// connect(), create_server(), server.start(), and server.stop().
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

    void close() {
        pipe.close();
        socket.close();
    }
};

// ── InterprocessConnection ──────────────────────────────────────────────

InterprocessConnection::InterprocessConnection() : impl_(std::make_unique<Impl>()) {}
InterprocessConnection::~InterprocessConnection() { disconnect(); }

bool InterprocessConnection::connect(std::string_view name, IpcTransport transport) {
    disconnect();
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
        ok = impl_->socket.connect(endpoint->host, endpoint->port);
    }

    if (ok) {
        state_.store(IpcState::Connected);
        connection_made();
        if (on_connected) on_connected();
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
        if (on_connected) on_connected();
        start_read_thread();
    } else {
        IpcState expected = IpcState::Connecting;
        state_.compare_exchange_strong(expected, IpcState::Error);
    }
    return ok;
}

void InterprocessConnection::disconnect() {
    const bool was_connected = state_.exchange(IpcState::Disconnected) == IpcState::Connected;
    running_.store(false);
    impl_->close();
    if (read_thread_.joinable()) {
        if (read_thread_.get_id() == std::this_thread::get_id())
            read_thread_.detach();
        else
            read_thread_.join();
    }

    if (was_connected) {
        connection_lost();
        if (on_disconnected) on_disconnected();
    }
}

bool InterprocessConnection::send_message(const void* data, size_t size) {
    if (!is_connected()) return false;
    if (size > kMaxMessageBytes) return false;

    std::lock_guard<std::mutex> lock(impl_->write_mutex);

    // Write 4-byte little-endian length header
    uint32_t len = static_cast<uint32_t>(size);
    uint8_t header[4];
    encode_u32_le(len, header);

    // Write header with retry for short writes
    size_t header_sent = 0;
    while (header_sent < 4) {
        int n = impl_->raw_write(header + header_sent, 4 - header_sent);
        if (n <= 0) return false;
        header_sent += static_cast<size_t>(n);
    }

    // Write payload with retry for short writes
    if (size > 0) {
        size_t payload_sent = 0;
        auto* payload = static_cast<const uint8_t*>(data);
        while (payload_sent < size) {
            int n = impl_->raw_write(payload + payload_sent, size - payload_sent);
            if (n <= 0) return false;
            payload_sent += static_cast<size_t>(n);
        }
    }
    return true;
}

bool InterprocessConnection::send_message(std::string_view message) {
    return send_message(message.data(), message.size());
}



void InterprocessConnection::start_read_thread() {
    running_.store(true);
    read_thread_ = std::thread([this]() { read_loop(); });
}

void InterprocessConnection::read_loop() {
    std::vector<uint8_t> buffer;
    auto notify_lost = [this]() {
        running_.store(false);
        if (state_.exchange(IpcState::Disconnected) == IpcState::Connected) {
            connection_lost();
            if (on_disconnected) on_disconnected();
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
        if (msg_len > kMaxMessageBytes) {
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

        // Dispatch message
        message_received(buffer.data(), msg_len);
        if (on_message) on_message(buffer.data(), msg_len);

        std::string_view text_view(reinterpret_cast<const char*>(buffer.data()), msg_len);
        message_received(text_view);
        if (on_text_message) on_text_message(text_view);
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
                // Inject the accepted socket via friend access
                conn->impl_->transport = IpcTransport::Socket;
                conn->impl_->socket = std::move(*client_sock);
                conn->state_.store(IpcState::Connected);
                conn->connection_made();
                if (conn->on_connected) conn->on_connected();

                // Start read thread while we still own the connection,
                // then hand off. This avoids use-after-free if the
                // callback destroys the connection immediately.
                // The read thread uses atomics so it's safe to start
                // before handlers are set — worst case it reads a
                // message and calls the default no-op virtual methods.
                conn->start_read_thread();

                if (on_client_connected)
                    on_client_connected(std::move(conn));
                else
                    client_connected(std::move(conn));
            }
        }
    });

    return true;
}

void InterprocessConnectionServer::stop() {
    const bool was_running = running_.exchange(false);
    if (was_running && server_impl_->transport == IpcTransport::Socket &&
        server_impl_->listen_socket.is_open()) {
        // Wake the accept() blocked in the accept thread by making a
        // dummy connection to ourselves. The "0.0.0.0" bind address
        // isn't connectable directly — fall back to loopback.
        auto endpoint = parse_socket_endpoint(std::string_view(server_impl_->name));
        if (endpoint) {
            std::string host = (endpoint->host.empty() || endpoint->host == "0.0.0.0")
                                   ? std::string{"127.0.0.1"}
                                   : endpoint->host;
            Socket wake;
            if (wake.create(SocketType::TCP)) {
                (void)wake.connect(host, endpoint->port);
            }
        }
    }
    server_impl_->listen_socket.close();
    if (accept_thread_.joinable()) accept_thread_.join();
    clients_.clear();
}

void InterprocessConnectionServer::client_connected(
    std::unique_ptr<InterprocessConnection> connection) {
    clients_.push_back(std::move(connection));
}

}  // namespace pulp::events
