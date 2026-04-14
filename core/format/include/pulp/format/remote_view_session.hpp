#pragma once

/// @file remote_view_session.hpp
/// Client-side session that speaks the Remote View Protocol over a
/// `pulp::runtime::MessageChannel` (usually a `WebSocketChannel`).
///
/// See `docs/reference/remote-view-protocol.md` for the wire format.
/// ViewBridge::attach_remote_view() owns the session and registers the
/// remote view as a secondary view with role `ViewRole::Remote`.

#include <pulp/format/view_bridge.hpp>
#include <pulp/runtime/json_rpc.hpp>
#include <pulp/runtime/message_channel.hpp>
#include <memory>
#include <string>
#include <string_view>

namespace pulp::format {

/// A connected remote-view session. Holds the `MessageChannel` and the
/// `JsonRpcPeer` that drives the protocol. The session is owned by the
/// `ViewBridge` it was attached to; it outlives neither the bridge nor
/// the remote peer.
///
/// Thread model: all callbacks fire on the bridge's UI thread. The
/// underlying channel uses its own reader thread internally; the
/// session's `JsonRpcPeer` translates between the two.
class RemoteViewSession {
public:
    /// Role this session drives in the owning bridge — always
    /// `ViewRole::Remote` in the current MVP; reserved for future
    /// differentiation between different remote renderers.
    ViewRole role() const { return ViewRole::Remote; }

    /// URL (or descriptive label) this session was opened against.
    const std::string& url() const { return url_; }

    /// Sends a `view.param_set` notification to the remote and updates
    /// the shared StateStore. Returns false if the channel is closed.
    bool set_parameter(uint32_t id, float normalized);

    /// Sends a `view.param_get` request, returning the remote's most
    /// recent value for the parameter. Returns std::nullopt on error
    /// or timeout.
    std::optional<float> get_parameter(uint32_t id);

    /// Sends a `view.input` notification. `kind_and_payload` is the
    /// already-serialized JSON value (as accepted by the protocol's
    /// `view.input` schema). Caller is responsible for payload
    /// shape; the session does not validate.
    bool send_input(std::string_view kind_and_payload);

    /// Graceful detach: sends `view.close`, tears down the JSON-RPC
    /// peer, closes the underlying channel. Idempotent.
    void close();

    /// True while the underlying channel is open and the peer hasn't
    /// closed.
    bool is_open() const;

    /// Last error message (handshake failure, JSON-RPC transport
    /// error, peer disconnect). Empty before the first failure.
    const std::string& last_error() const { return last_error_; }

    ~RemoteViewSession();
    RemoteViewSession(const RemoteViewSession&) = delete;
    RemoteViewSession& operator=(const RemoteViewSession&) = delete;

private:
    friend class ViewBridge;

    RemoteViewSession(std::string url,
                      state::StateStore& store,
                      std::unique_ptr<runtime::MessageChannel> channel);

    // Perform the initial hello + metadata handshake. Returns false
    // on failure with `last_error_` populated.
    bool handshake_(Processor& processor);

    // Register method handlers + param change listener.
    void install_handlers_(Processor& processor);

    std::string url_;
    state::StateStore& store_;
    std::unique_ptr<runtime::MessageChannel> channel_;
    std::unique_ptr<runtime::JsonRpcPeer> peer_;
    std::string last_error_;
    bool closed_ = false;
};

} // namespace pulp::format
