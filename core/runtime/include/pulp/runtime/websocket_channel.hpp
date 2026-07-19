#pragma once

// WebSocketChannel — RFC 6455 MessageChannel over TcpStream.
//
// Scope:
//   - Client handshake: `Upgrade: websocket`, Sec-WebSocket-Key / Accept
//     validation (SHA-1 + base64 via pulp::runtime::crypto).
//   - Server handshake: parse client upgrade, echo a 101 Switching
//     Protocols with the correct Sec-WebSocket-Accept.
//   - Frames: text (0x1), binary (0x2), close (0x8), ping (0x9),
//     pong (0xA). Control frames (close/ping/pong) are handled
//     internally; ping triggers an automatic pong response.
//   - 7/16/64-bit payload length. Client frames are masked (per the
//     RFC "MUST"); server frames are unmasked.
//
// Out of scope:
//   - Extension negotiation (permessage-deflate etc.)
//   - Fragmented messages across multiple frames (accepted but not
//     emitted; see implementation notes)
//   - TLS — for `wss://`, feed a TLS-wrapped TcpStream or use the
//     forthcoming SecureTcpStream work.
//
// Threading: the channel spins up one reader thread that dispatches
// completed messages via the provided `MessageExecutor` (or inline on
// the reader thread when none is set). `send()` is thread-safe and
// serializes writes to the underlying TcpStream via an internal mutex.

#include <pulp/runtime/message_channel.hpp>
#include <pulp/runtime/network_stream.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace pulp::runtime {

/// Options for WebSocketChannel construction.
struct WebSocketOptions {
    /// Executor used to dispatch `on_message`, `on_closed`, `on_error`.
    /// When empty, callbacks run on the reader thread.
    /// Do not destroy the channel from an inline callback; call `close()` and
    /// defer destruction, or provide an executor that owns callback lifetime.
    MessageExecutor executor;

    /// Maximum inbound payload size (bytes). Frames larger than this
    /// trigger an error + close. Default 16 MiB — enough for normal
    /// control traffic while bounding pathological peers.
    std::size_t max_payload = 16u * 1024u * 1024u;

    /// Maximum total size (bytes) of a message reassembled across
    /// continuation frames. `max_payload` bounds a single frame; without a
    /// separate total cap a peer streaming endless non-final fragments would
    /// grow the reassembly buffer without limit (memory-exhaustion DoS).
    /// Exceeding this triggers an error + close. Default 64 MiB.
    std::size_t max_message = 64u * 1024u * 1024u;
};

class WebSocketChannel : public MessageChannel {
public:
    /// Perform a client handshake over `tcp`, then start the reader.
    /// `host` and `path` populate the HTTP/1.1 upgrade request
    /// (`Host:` header + request-URI). Returns nullptr on failure.
    static std::unique_ptr<WebSocketChannel> connect(
        std::unique_ptr<TcpStream> tcp,
        std::string_view host,
        std::string_view path = "/",
        WebSocketOptions options = {});

    /// Perform a server-side handshake over an already-accepted `tcp`
    /// connection. Reads the HTTP upgrade request, validates the key,
    /// writes a 101 Switching Protocols response, and starts the
    /// reader. Returns nullptr on failure.
    static std::unique_ptr<WebSocketChannel> accept(
        std::unique_ptr<TcpStream> tcp,
        WebSocketOptions options = {});

    ~WebSocketChannel() override;

    // MessageChannel interface
    bool send(const std::uint8_t* data, std::size_t size) override;
    bool send_text(std::string_view text) override;
    void on_message(MessageCallback callback) override;
    void on_closed(ChannelClosedCallback callback) override;
    void on_error(ChannelErrorCallback callback) override;
    void close() override;
    bool is_open() const override;

    /// Expose the handshake Sec-WebSocket-Accept for debugging and for
    /// tests that want to verify RFC 6455 compliance.
    static std::string compute_accept_key(std::string_view client_key);

private:
    enum class Role : uint8_t { Client, Server };

    WebSocketChannel(std::unique_ptr<TcpStream> tcp, Role role,
                     WebSocketOptions options);

    bool send_frame(uint8_t opcode, const std::uint8_t* data, std::size_t size);
    void reader_main();
    void fire_error(std::string_view reason);
    void fire_closed();
    void dispatch(std::function<void()> task);

    std::unique_ptr<TcpStream> tcp_;
    Role role_;
    WebSocketOptions options_;

    mutable std::mutex mutex_;
    std::mutex write_mutex_;  ///< serializes frame writes
    std::atomic<bool> open_{false};
    std::atomic<bool> closed_fired_{false};
    std::thread reader_;

    MessageCallback on_message_;
    ChannelClosedCallback on_closed_;
    ChannelErrorCallback on_error_;
};

}  // namespace pulp::runtime
