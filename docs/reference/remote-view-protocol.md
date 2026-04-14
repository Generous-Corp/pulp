# Remote View Protocol (Feature 1, Phase 4)

Wire format for attaching a remote view to a `pulp::format::ViewBridge`.
A remote view is a secondary view (role `ViewRole::Remote`) whose paint
and input live on another process or machine; `attach_remote_view(url)`
opens a WebSocket to that endpoint and exchanges the messages below.

**Status:** **MVP** — parameter sync + input events + basic metadata are
live. Paint-op streaming (canvas-command mirroring) is staged for a
follow-up; clients render their own view tree today and the protocol
is the coherent parameter-and-input bus that wires into it.

## Transport

- **Carrier:** RFC 6455 WebSocket. Text frames carry JSON; binary
  frames are reserved for future paint-op streams.
- **Connection:** client side is the Pulp host opening a connection to
  the remote-view server (`attach_remote_view("ws://…")`). Server side
  is the remote renderer (web page, Electron app, native client).
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
| `view.param_set` | `{id, normalized}` | `null` | Drive parameter automation from the remote UI; resolved into the shared `StateStore`. |
| `view.param_get` | `{id}` | `{normalized}` | Cheap resync. |
| `view.input` | `{kind, ...}` | `null` | Forwarded input event. `kind` is `"click" \| "key" \| "wheel" \| "text"`; the per-kind payload mirrors `pulp::view::input_events.hpp`. |

## Lifecycle

```
client (ViewBridge host)                server (remote renderer)
──────────────────────────                ────────────────────────
attach_remote_view("ws://…")
  │
  │  WebSocket upgrade  ──────────────►  accept
  │  ◄─────────────────────────────────  101 Switching Protocols
  │
  │  view.hello ───────────────────────►  view.hello (reply)
  │  ◄─────────────────────────────────
  │  view.metadata ────────────────────►
  │
  ... per-frame param sync + input ...
  │  ◄─── view.param_set ──────────────
  │   param_changed ──────────────────►
  │  ◄─── view.input ──────────────────
  │
  (bridge is closed)
  │  view.close ───────────────────────►
  │  close handshake  ─────────────────►
```

## Failure modes

- **Connection refused / handshake failure** → `attach_remote_view` returns
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

`tools/mcp/pulp_mcp.cpp` exposes the protocol as MCP tools so Claude
Code can attach to a running plugin and inspect / drive its editor:

- `view_attach` — wraps `attach_remote_view`
- `view_param_set` / `view_param_get` — drives `view.param_*`
- `view_list` — enumerates attached secondary views
- `view_input` — sends synthetic input
- `view_close` — detaches

See `.agents/skills/view-bridge/SKILL.md` for the full MCP command
recipe.

## References

- `core/format/include/pulp/format/view_bridge.hpp` — public API
- `core/runtime/include/pulp/runtime/websocket_channel.hpp` — carrier
- `core/runtime/include/pulp/runtime/json_rpc.hpp` — JsonRpcPeer
- `docs/guides/view-bridge.md` — user-facing guide
