# Remote View Protocol

Wire format for a remote view attached to a `pulp::format::ViewBridge`.
A remote view is registered through
`ViewBridge::attach_remote_channel(channel, label)`: callers supply a
connected `pulp::runtime::MessageChannel`, usually a `WebSocketChannel`
connected to another process or machine.

**Status:** **MVP** — read-only parameter observation + basic metadata are
live. Input notifications are transport-visible but are not dispatched into
the host's primary view. Paint-op streaming (canvas-command mirroring) is staged for a
follow-up; clients render their own view tree today and the protocol
is the observation bus that wires into it.

The protocol grants no remote mutation authority. In particular,
`view.param_set` is intentionally unsupported. A future writable remote surface
must enter through Pulp's capability/grant controller; it must not add a direct
StateStore write handler here.

## Transport

- **Carrier:** RFC 6455 WebSocket. Text frames carry JSON; binary
  frames are reserved for future paint-op streams.
- **Connection:** client side is the Pulp host opening a connection to
  the remote-view server with `WebSocketChannel::connect(...)`, then
  passing the channel to `ViewBridge::attach_remote_channel(...)`.
  Server side is the remote renderer (web page, Electron app, native client).
- **Framing:** JSON-RPC 2.0 (`jsonrpc: "2.0"`) using the in-tree
  `pulp::runtime::JsonRpcPeer` on top of `WebSocketChannel`.
- **Thread model:** all protocol dispatch runs on the UI / host thread
  of the hosting `ViewBridge` (same thread that dispatches
  `Processor::on_view_*`). Audio thread is never touched.

## Messages

### Bidirectional

| Method | Direction | Payload | Purpose |
|---|---|---|---|
| `view.hello` | client → server | `{protocol_version:1, bridge_id, role:"remote"}` | First message after WebSocket open. Server replies with its own `view.hello`. |
| `view.resize` | either | `{w, h}` | Notify peer of a new preferred size. |
| `view.close` | either | `{reason}` | Graceful detach; mirrors `bridge->detach_secondary_view`. |

### Server-initiated (host → remote)

| Method | Payload | Notes |
|---|---|---|
| `view.metadata` | `{title, size_hints: ViewSize, params: [ParamInfo]}` | Sent once after `view.hello`. Remote uses this to lay out its UI. |
| `view.param_changed` | `{id, normalized}` | Notification (no `id`). Fires whenever `StateStore` changes, whether triggered locally or by a different view. |

### Client-initiated (remote → host)

| Method | Payload | Response | Notes |
|---|---|---|---|
| `view.param_get` | `{id}` | `{normalized}` | Cheap resync. |
| `view.input` | `{kind, ...}` | `null` | Reserved notification. The host currently logs and ignores it; it does not dispatch into the primary view. |

## Lifecycle

```
client (ViewBridge host)                server (remote renderer)
──────────────────────────                ────────────────────────
attach_remote_channel(WebSocketChannel::connect(...))
  │
  │  WebSocket upgrade  ──────────────►  accept
  │  ◄─────────────────────────────────  101 Switching Protocols
  │
  │  view.hello ───────────────────────►  view.hello (reply)
  │  ◄─────────────────────────────────
  │  view.metadata ────────────────────►
  │
  ... parameter observation ...
  │   param_changed ──────────────────►
  │
  (bridge is closed)
  │  view.close ───────────────────────►
  │  close handshake  ─────────────────►
```

## Failure modes

- **Connection refused** → `WebSocketChannel::connect(...)` returns `nullptr`
  before the bridge is attached.
- **Protocol handshake failure** → `attach_remote_channel(...)` returns
  `nullptr` and sets `bridge->last_error()`.
- **Mid-session disconnect** → `on_closed` fires on the WebSocket;
  the bridge detaches the remote view automatically and dispatches
  `Processor::on_view_closed` for that role only (primary editor keeps
  running).
- **Malformed frame** → logged, frame dropped, connection stays up.
- **JSON-RPC error responses** → surfaced via the remote session's error
  callback; the host never crashes on a misbehaving remote.

## Not yet wired (follow-ups)

- **Paint-op streaming** — mirroring `Canvas` draw commands to the
  remote over binary frames. This is the hardest piece (command
  encoding, image caching, resource handles). Remote clients currently
  render their own view tree informed by `view.metadata` and stay in
  sync via parameter changes.
- **Reconnection w/ backoff** — sessions today go one-shot: a dropped
  socket detaches the remote view. A reconnect policy and state
  replay are tracked as a separate task.
- **Auth / TLS** — plain `ws://` loopback is the only tested target;
  `wss://` works transport-level but the handshake carries no auth
  beyond whatever the TLS layer provides.

## MCP integration

`tools/mcp/pulp_mcp.cpp` is the repo-level MCP server today. It does not yet
expose per-plugin remote-view tools, but a host-side MCP wrapper can use this
protocol to attach to a running plugin for read-only inspection:

- `view_attach` — connects a `WebSocketChannel` and calls `attach_remote_channel`
- `view_param_get` — reads through `RemoteViewSession::get_parameter`
- `view_list` — enumerates attached secondary views
- `view_close` — detaches

Do not expose parameter writes or synthetic input by wrapping
`RemoteViewSession`; future mutation belongs behind the capability controller's
explicit grant and audit path.

See `.agents/skills/view-bridge/SKILL.md` for the full MCP command
recipe.

## References

- `core/format/include/pulp/format/view_bridge.hpp` — public API
- `core/runtime/include/pulp/runtime/websocket_channel.hpp` — carrier
- `core/runtime/include/pulp/runtime/json_rpc.hpp` — JsonRpcPeer
- `docs/guides/view-bridge.md` — user-facing guide
