#pragma once

// InterprocessConnection — bidirectional IPC over named pipes, TCP sockets,
// or credential-bearing OS-local sockets.
// Provides length-prefixed message framing, connection lifecycle callbacks,
// and background receive thread. Used for crash-isolated plugin scanning,
// multi-process architectures, and standalone↔plugin communication.

#include <pulp/runtime/socket.hpp>
#include <pulp/runtime/alive_token.hpp>
#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <memory>
#include <optional>
#include <atomic>
#include <thread>
#include <mutex>
#include <cstdint>
#include <chrono>

namespace pulp::events {

/// Transport type for IPC
enum class IpcTransport { NamedPipe, Socket, LocalSocket };

/// Connection state
enum class IpcState { Disconnected, Connecting, Connected, Error };

/// Interprocess connection — send and receive length-prefixed messages.
/// Messages are framed as: [4-byte little-endian length][payload bytes]
class InterprocessConnection {
public:
    InterprocessConnection();
    virtual ~InterprocessConnection();

    // ── Connection lifecycle ────────────────────────────────────────────

    /// Connect as client to a named pipe, TCP socket, or OS-local socket.
    /// For pipes: name is the pipe name (e.g., "pulp_scanner").
    /// For sockets: name is "host:port" (e.g., "127.0.0.1:9100").
    /// For local sockets: name is an absolute filesystem endpoint path.
    /// A positive timeout bounds the socket connect phase.
    bool connect(
        std::string_view name,
        IpcTransport transport = IpcTransport::NamedPipe,
        std::chrono::milliseconds timeout = {});

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

    /// Kernel-observed credentials for the process on the other end of a
    /// connected local socket. Request payloads cannot influence this result.
    std::optional<runtime::LocalPeerCredentials> local_peer_credentials() const;

    // ── Messaging ───────────────────────────────────────────────────────

    /// Send a message (length-prefixed). Thread-safe.
    /// Returns true if the message was sent successfully.
    bool send_message(const void* data, size_t size);

    /// Send a string message.
    bool send_message(std::string_view message);

    /// Override the framing ceiling before connecting or accepting work.
    /// Oversized outbound frames are rejected; oversized inbound frames close
    /// the connection before allocating their declared payload. Values above
    /// the socket API's INT_MAX transfer-count boundary are clamped.
    void set_max_message_bytes(std::size_t bytes);
    std::size_t max_message_bytes() const {
        return max_message_bytes_.load(std::memory_order_relaxed);
    }

    /// Apply a blocking write deadline for socket transports. A timed-out
    /// frame poisons and closes the connection so no later frame can be
    /// appended to a truncated stream.
    void set_write_timeout(std::chrono::milliseconds timeout);

    /// Bound a frame once its first byte arrives. Idle connections may wait
    /// indefinitely for the next frame, but a partial header or payload is
    /// disconnected when this cumulative deadline expires.
    void set_frame_read_timeout(std::chrono::milliseconds timeout);

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
    std::shared_ptr<Impl> impl_;
    std::atomic<IpcState> state_{IpcState::Disconnected};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> connection_generation_{0};
    std::atomic<std::uint64_t> read_generation_{0};
    std::atomic<std::size_t> max_message_bytes_{64u * 1024u * 1024u};
    std::atomic<std::int64_t> write_timeout_ms_{0};
    std::atomic<std::int64_t> frame_read_timeout_ms_{0};
    std::atomic<bool> write_poisoned_{false};
    std::atomic<bool> defer_first_dispatch_until_callback_{false};
    std::shared_ptr<std::atomic<bool>> first_dispatch_gate_;
    mutable std::mutex callback_mutex_;
    runtime::AliveToken alive_;

    void disconnect_impl(bool destroying);
    void release_first_dispatch_gate();
    void start_read_thread(bool allow_active_disconnect_owner = false,
                           std::uint64_t expected_connection_generation = 0);
    void read_loop(std::uint64_t generation);
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

    /// Apply a framing ceiling to every subsequently accepted connection.
    void set_max_message_bytes(std::size_t bytes);
    void set_write_timeout(std::chrono::milliseconds timeout);
    void set_frame_read_timeout(std::chrono::milliseconds timeout);

    /// Whether the server is running.
    bool is_running() const { return running_.load(); }

    /// Actual TCP port after binding, including an OS-assigned port requested
    /// with `host:0`. Returns zero for local, pipe, or unbound servers.
    std::uint16_t bound_port() const;

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
    std::atomic<std::size_t> max_message_bytes_{64u * 1024u * 1024u};
    std::atomic<std::int64_t> write_timeout_ms_{0};
    std::atomic<std::int64_t> frame_read_timeout_ms_{0};
    std::thread accept_thread_;
    std::vector<std::unique_ptr<InterprocessConnection>> clients_;
    struct ServerImpl;
    std::unique_ptr<ServerImpl> server_impl_;
};

}  // namespace pulp::events
