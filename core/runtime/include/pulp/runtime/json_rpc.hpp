#pragma once

// JSON-RPC 2.0 over MessageChannel.
//
// `JsonRpcPeer` is symmetric: any peer can send requests, receive
// requests, emit notifications, and receive notifications. One peer
// plus one MessageChannel equals one side of a JSON-RPC session.
//
// Wire format is pure JSON text (per the spec). The peer serializes
// into the channel via `send_text()`, so transports that distinguish
// text from binary (WebSocket) will carry it as text frames.
//
// Error semantics follow JSON-RPC 2.0 §5:
//   -32700 Parse error
//   -32600 Invalid Request
//   -32601 Method not found
//   -32602 Invalid params
//   -32603 Internal error
// User code can return custom codes from request handlers.

#include <pulp/runtime/message_channel.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace pulp::runtime {

/// Structured JSON-RPC error. Matches spec §5.1.
struct JsonRpcError {
    int code = 0;
    std::string message;
    std::string data_json;  ///< Optional JSON-encoded error.data (empty → omit)

    static JsonRpcError parse_error()       { return {-32700, "Parse error", ""}; }
    static JsonRpcError invalid_request()   { return {-32600, "Invalid Request", ""}; }
    static JsonRpcError method_not_found()  { return {-32601, "Method not found", ""}; }
    static JsonRpcError invalid_params()    { return {-32602, "Invalid params", ""}; }
    static JsonRpcError internal_error()    { return {-32603, "Internal error", ""}; }
};

/// Result of a request handler: success JSON (result field) or error.
struct JsonRpcResult {
    std::string result_json;  ///< JSON-encoded result (e.g., "42" or "\"ok\"")
    std::optional<JsonRpcError> error;

    static JsonRpcResult ok(std::string result_json) {
        return {std::move(result_json), std::nullopt};
    }
    static JsonRpcResult fail(JsonRpcError err) {
        return {"", std::move(err)};
    }
};

/// Request handler: given a JSON-encoded params string (empty if omitted),
/// return a JsonRpcResult. Runs on whichever thread the underlying channel
/// dispatches on.
using JsonRpcMethodHandler =
    std::function<JsonRpcResult(std::string_view params_json)>;

/// Notification handler (incoming notifications carry no id and expect
/// no response).
using JsonRpcNotificationHandler =
    std::function<void(std::string_view params_json)>;

/// Callback for the response to a request we sent.
using JsonRpcResponseCallback =
    std::function<void(const JsonRpcResult& response)>;

class JsonRpcPeer {
public:
    /// Wrap a message channel. The peer installs an `on_message` handler
    /// on the channel; do not replace it after construction.
    explicit JsonRpcPeer(MessageChannel& channel);
    ~JsonRpcPeer();

    JsonRpcPeer(const JsonRpcPeer&) = delete;
    JsonRpcPeer& operator=(const JsonRpcPeer&) = delete;

    /// Register a method handler. Replacing a handler with nullptr
    /// unregisters.
    void register_method(std::string_view name, JsonRpcMethodHandler handler);

    /// Register a notification handler.
    void on_notification(std::string_view name, JsonRpcNotificationHandler handler);

    /// Send a request. `params_json` must be a JSON-encoded array or
    /// object (or empty for no params). The callback fires exactly once
    /// with the response. Returns false if the channel is closed.
    bool send_request(std::string_view method,
                      std::string_view params_json,
                      JsonRpcResponseCallback callback);

    /// Emit a notification.
    bool notify(std::string_view method, std::string_view params_json);

private:
    void handle_message(const Message& msg);
    void handle_object(std::string_view obj_json);
    void dispatch_response(std::string_view response_json);
    void dispatch_request(std::string_view request_json);

    MessageChannel* channel_;
    std::atomic<std::uint64_t> next_id_{1};

    std::mutex mutex_;
    std::unordered_map<std::string, JsonRpcMethodHandler> methods_;
    std::unordered_map<std::string, JsonRpcNotificationHandler> notifications_;
    std::unordered_map<std::uint64_t, JsonRpcResponseCallback> pending_;
};

}  // namespace pulp::runtime
