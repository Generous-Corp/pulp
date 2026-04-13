#pragma once

// MessageChannel — structured-message layer on top of byte streams.
//
// `Stream` and `AsyncStream` deal in raw bytes. MessageChannel deals in
// whole messages: one `send()` transmits one message, one `on_message`
// callback fires per received message. This is the right abstraction for
// WebSocket, OSC, JSON-RPC, and similar request/response protocols where
// a partial read is always meaningless.
//
// Layering:
//   byte transport  →  Stream / AsyncStream
//   framing         →  MessageChannel implementations
//   protocol        →  JsonRpcClient, remote-control, user code
//
// All MessageChannel methods are thread-safe unless documented otherwise.
// Callback dispatch follows the same executor contract as AsyncStream: if
// a `MessageExecutor` is supplied at construction, callbacks land on that
// executor; otherwise they run on whichever thread the implementation uses
// internally (usually its reader thread).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::runtime {

/// Classification of a delivered message. Transports that do not
/// distinguish (OSC, JSON-RPC) report Binary.
enum class MessageKind : uint8_t {
    Binary,  ///< opaque bytes
    Text,    ///< UTF-8 text (WebSocket text frames, JSON-RPC envelopes)
};

/// A single structured message as delivered by the channel.
struct Message {
    MessageKind kind = MessageKind::Binary;
    std::vector<std::uint8_t> payload;

    /// View the payload as a text string. Only meaningful for
    /// `kind == MessageKind::Text`.
    std::string_view as_text() const {
        return {reinterpret_cast<const char*>(payload.data()), payload.size()};
    }
};

/// Called when the channel receives a complete message.
using MessageCallback = std::function<void(const Message& message)>;

/// Called once when the channel transitions to a closed state — either
/// the peer closed, the transport errored, or `close()` was invoked.
using ChannelClosedCallback = std::function<void()>;

/// Called when a transport-level error occurs. After an error callback
/// fires, the channel is no longer usable and a close callback will
/// follow.
using ChannelErrorCallback = std::function<void(std::string_view reason)>;

/// Executor used to dispatch callbacks off whichever worker thread the
/// channel runs internally. Same shape as `AsyncExecutor`.
using MessageExecutor = std::function<void(std::function<void()>)>;

/// Abstract base for message-oriented channels.
///
/// Implementations never throw from send/receive paths; errors surface as
/// the return value of `send()` or through the error callback.
class MessageChannel {
public:
    virtual ~MessageChannel() = default;

    MessageChannel(const MessageChannel&) = delete;
    MessageChannel& operator=(const MessageChannel&) = delete;

    /// Send a message. Returns true if the payload was accepted for
    /// transmission (queued or written); false if the channel is closed,
    /// errored, or the payload was rejected (e.g., frame too large).
    ///
    /// The default overload sends binary; override `send_text` if the
    /// transport distinguishes text from binary.
    virtual bool send(const std::uint8_t* data, std::size_t size) = 0;

    bool send(std::string_view text) {
        return send_text(text);
    }

    virtual bool send_text(std::string_view text) {
        // Default: transports that don't distinguish just send bytes.
        return send(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    }

    /// Install / replace callbacks. Safe to call at any time; callbacks
    /// installed during send-processing apply to subsequent messages.
    virtual void on_message(MessageCallback callback) = 0;
    virtual void on_closed(ChannelClosedCallback callback) = 0;
    virtual void on_error(ChannelErrorCallback callback) = 0;

    /// Close the channel. Idempotent. Fires the closed callback.
    virtual void close() = 0;

    /// Whether the channel is currently usable.
    virtual bool is_open() const = 0;

protected:
    MessageChannel() = default;
};

}  // namespace pulp::runtime
