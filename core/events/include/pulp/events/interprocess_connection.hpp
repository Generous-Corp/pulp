#pragma once

// InterprocessConnection — bidirectional IPC over named pipes or TCP sockets.
// Provides length-prefixed message framing, connection lifecycle callbacks,
// and background receive thread. Used for crash-isolated plugin scanning,
// multi-process architectures, and standalone↔plugin communication.

#include <pulp/runtime/socket.hpp>
#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <cstdint>

namespace pulp::events {

/// Transport type for IPC
enum class IpcTransport { NamedPipe, Socket };

/// Connection state
enum class IpcState { Disconnected, Connecting, Connected, Error };

/// Interprocess connection — send and receive length-prefixed messages.
/// Messages are framed as: [4-byte little-endian length][payload bytes]
class InterprocessConnection {
public:
    InterprocessConnection();
    virtual ~InterprocessConnection();

    // ── Connection lifecycle ────────────────────────────────────────────

    /// Connect as client to a named pipe or TCP socket.
    /// For pipes: name is the pipe name (e.g., "pulp_scanner").
    /// For sockets: name is "host:port" (e.g., "127.0.0.1:9100").
    bool connect(std::string_view name, IpcTransport transport = IpcTransport::NamedPipe);

    /// Create a server that listens for one client connection.
    /// Blocks until a client connects (or timeout_ms expires, 0 = infinite).
    bool create_server(std::string_view name, IpcTransport transport = IpcTransport::NamedPipe,
                       int timeout_ms = 0);

    /// Disconnect and clean up.
    void disconnect();

    /// Whether currently connected.
    bool is_connected() const { return state_.load() == IpcState::Connected; }

    /// Current state.
    IpcState state() const { return state_.load(); }

    // ── Messaging ───────────────────────────────────────────────────────

    /// Send a message (length-prefixed). Thread-safe.
    /// Returns true if the message was sent successfully.
    bool send_message(const void* data, size_t size);

    /// Send a string message.
    bool send_message(std::string_view message);

    // ── Callbacks (override or set) ─────────────────────────────────────

    /// Called when a connection is established.
    virtual void connection_made() {}

    /// Called when the connection is lost.
    virtual void connection_lost() {}

    /// Called when a message is received. Called on the background read thread.
    virtual void message_received(const void* data, size_t size) {
        (void)data; (void)size;
    }

    /// Convenience: called with string view for text messages.
    virtual void message_received(std::string_view message) {
        (void)message;
    }

    /// Lambda-based callbacks (alternative to overriding)
    ///
    /// Assign these directly before a connection starts. For already-connected
    /// instances, use the setter methods so the background read thread sees a
    /// synchronized callback update.
    std::function<void()> on_connected;
    std::function<void()> on_disconnected;
    std::function<void(const void*, size_t)> on_message;
    std::function<void(std::string_view)> on_text_message;

    void set_on_connected(std::function<void()> callback);
    void set_on_disconnected(std::function<void()> callback);
    void set_on_message(std::function<void(const void*, size_t)> callback);
    void set_on_text_message(std::function<void(std::string_view)> callback);

    // No copy
    InterprocessConnection(const InterprocessConnection&) = delete;
    InterprocessConnection& operator=(const InterprocessConnection&) = delete;

    friend class InterprocessConnectionServer;  // Needs to inject accepted sockets

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<IpcState> state_{IpcState::Disconnected};
    std::thread read_thread_;
    std::atomic<bool> running_{false};
    mutable std::mutex callback_mutex_;

    void start_read_thread();
    void read_loop();
};

/// Interprocess connection server — listens for multiple client connections.
/// Each accepted connection gets its own InterprocessConnection.
class InterprocessConnectionServer {
public:
    InterprocessConnectionServer();
    virtual ~InterprocessConnectionServer();

    /// Start listening on the given name.
    bool start(std::string_view name, IpcTransport transport = IpcTransport::Socket);

    /// Stop listening and disconnect all clients.
    void stop();

    /// Whether the server is running.
    bool is_running() const { return running_.load(); }

    /// Called when a new client connects. Override to handle.
    /// The returned connection is owned by the server.
    virtual void client_connected(std::unique_ptr<InterprocessConnection> connection);

    /// Lambda callback alternative
    std::function<void(std::unique_ptr<InterprocessConnection>)> on_client_connected;

    // No copy
    InterprocessConnectionServer(const InterprocessConnectionServer&) = delete;
    InterprocessConnectionServer& operator=(const InterprocessConnectionServer&) = delete;

private:
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::vector<std::unique_ptr<InterprocessConnection>> clients_;
    struct ServerImpl;
    std::unique_ptr<ServerImpl> server_impl_;
};

}  // namespace pulp::events
