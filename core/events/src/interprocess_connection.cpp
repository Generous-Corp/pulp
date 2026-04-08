#include <pulp/events/interprocess_connection.hpp>
#include <pulp/runtime/named_pipe.hpp>
#include <pulp/runtime/socket.hpp>
#include <cstring>
#include <mutex>

namespace pulp::events {

using namespace pulp::runtime;

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
        // Parse host:port
        auto colon = name.find(':');
        if (colon == std::string_view::npos) return false;
        std::string host(name.substr(0, colon));
        uint16_t port = static_cast<uint16_t>(std::stoi(std::string(name.substr(colon + 1))));
        impl_->socket.create(SocketType::TCP);
        ok = impl_->socket.connect(host, port);
    }

    if (ok) {
        state_.store(IpcState::Connected);
        connection_made();
        if (on_connected) on_connected();
        start_read_thread();
    } else {
        state_.store(IpcState::Error);
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
        auto colon = name.find(':');
        uint16_t port = 0;
        std::string host = "0.0.0.0";
        if (colon != std::string_view::npos) {
            host = std::string(name.substr(0, colon));
            port = static_cast<uint16_t>(std::stoi(std::string(name.substr(colon + 1))));
        } else {
            port = static_cast<uint16_t>(std::stoi(std::string(name)));
        }
        impl_->socket.create(SocketType::TCP);
        if (impl_->socket.bind(host, port) && impl_->socket.listen(1)) {
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
        state_.store(IpcState::Error);
    }
    return ok;
}

void InterprocessConnection::disconnect() {
    running_.store(false);
    impl_->close();
    if (read_thread_.joinable()) read_thread_.join();

    if (state_.load() == IpcState::Connected) {
        state_.store(IpcState::Disconnected);
        connection_lost();
        if (on_disconnected) on_disconnected();
    }
    state_.store(IpcState::Disconnected);
}

bool InterprocessConnection::send_message(const void* data, size_t size) {
    if (!is_connected()) return false;

    std::lock_guard<std::mutex> lock(impl_->write_mutex);

    // Write 4-byte little-endian length header
    uint32_t len = static_cast<uint32_t>(size);
    uint8_t header[4] = {
        static_cast<uint8_t>(len & 0xFF),
        static_cast<uint8_t>((len >> 8) & 0xFF),
        static_cast<uint8_t>((len >> 16) & 0xFF),
        static_cast<uint8_t>((len >> 24) & 0xFF)
    };

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

void InterprocessConnection::adopt_socket(runtime::Socket&& socket) {
    disconnect();
    impl_->transport = IpcTransport::Socket;
    impl_->socket = std::move(socket);
    state_.store(IpcState::Connected);
    connection_made();
    if (on_connected) on_connected();
    start_read_thread();
}

void InterprocessConnection::start_read_thread() {
    running_.store(true);
    read_thread_ = std::thread([this]() { read_loop(); });
}

void InterprocessConnection::read_loop() {
    std::vector<uint8_t> buffer;

    while (running_.load()) {
        // Read 4-byte length header
        uint8_t header[4];
        int n = impl_->raw_read(header, 4);
        if (n <= 0) {
            if (running_.load()) {
                state_.store(IpcState::Disconnected);
                connection_lost();
                if (on_disconnected) on_disconnected();
            }
            return;
        }
        if (n != 4) continue;

        uint32_t msg_len = header[0] | (header[1] << 8) | (header[2] << 16) | (header[3] << 24);
        if (msg_len > 64 * 1024 * 1024) continue;  // Sanity: max 64MB

        // Read payload
        buffer.resize(msg_len);
        size_t read_so_far = 0;
        while (read_so_far < msg_len && running_.load()) {
            int got = impl_->raw_read(buffer.data() + read_so_far, msg_len - read_so_far);
            if (got <= 0) {
                state_.store(IpcState::Disconnected);
                connection_lost();
                if (on_disconnected) on_disconnected();
                return;
            }
            read_so_far += static_cast<size_t>(got);
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
        auto colon = name.find(':');
        std::string host = "0.0.0.0";
        uint16_t port = 0;
        if (colon != std::string_view::npos) {
            host = std::string(name.substr(0, colon));
            port = static_cast<uint16_t>(std::stoi(std::string(name.substr(colon + 1))));
        } else {
            port = static_cast<uint16_t>(std::stoi(std::string(name)));
        }

        server_impl_->listen_socket.create(SocketType::TCP);
        if (!server_impl_->listen_socket.bind(host, port)) return false;
        if (!server_impl_->listen_socket.listen(5)) return false;
    }

    running_.store(true);
    accept_thread_ = std::thread([this]() {
        while (running_.load()) {
            if (server_impl_->transport == IpcTransport::Socket) {
                auto client_sock = server_impl_->listen_socket.accept();
                if (!client_sock) continue;

                auto conn = std::make_unique<InterprocessConnection>();
                conn->adopt_socket(std::move(*client_sock));
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
    running_.store(false);
    server_impl_->listen_socket.close();
    if (accept_thread_.joinable()) accept_thread_.join();
    clients_.clear();
}

void InterprocessConnectionServer::client_connected(
    std::unique_ptr<InterprocessConnection> connection) {
    clients_.push_back(std::move(connection));
}

}  // namespace pulp::events
