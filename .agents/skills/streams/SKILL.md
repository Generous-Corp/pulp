---
name: streams
description: Pick the right Pulp Stream for a given I/O task, wire async callbacks correctly without deadlocking the worker, and avoid the backpressure / cancellation footguns in `pulp::runtime::AsyncStream`.
---

# Streams

Use this skill when reaching for any I/O in Pulp: reading files, sending
bytes over a socket, polling an HTTP endpoint, piping between processes,
or wrapping a new transport. The `pulp::runtime::Stream` / `AsyncStream`
hierarchy is the sanctioned path; do not add new `Socket::send` /
`http_get` callers unless you have a specific reason.

## Decision tree

| You need to... | Use |
|----------------|-----|
| Read or write a local file | `FileStream` (sync) — or wrap in `AsyncStream` for large files |
| Keep bytes in memory for tests / round-trips | `MemoryStream` |
| Talk to another process via pipe | `PipeStream` around `NamedPipe` |
| Open a TCP connection | `TcpStream`, wrapped in `AsyncStream` so `connect()` doesn't block the caller |
| Fetch an HTTP/S body | `HttpStream::get(url)` / `post(url, body)` |
| Send structured WebSocket frames | `WebSocketChannel::connect()` / `::accept()` over a `TcpStream` |
| Send/receive OSC messages | `OscChannel::open(host, remote_port, local_port)` |
| RPC-style request/response over any transport | `JsonRpcPeer` wrapping any `MessageChannel` |
| In-process message bridge (tests, inspector) | `MemoryMessageChannel::make_pair()` |

## AsyncStream — the patterns that actually work

### 1. Dispatch callbacks onto your own loop

`AsyncStream` never links `pulp::events` directly (that would be a
library cycle). Pass an executor closure:

```cpp
AsyncStream::Options opts;
opts.executor = [loop](std::function<void()> fn) { loop->dispatch(std::move(fn)); };
```

Without an executor, callbacks run on the AsyncStream's worker thread —
fine for tests, usually wrong for UI state.

### 2. Backpressure: check the return of `write_async`

`write_async` returns **false** when the pending byte count would exceed
`options.write_high_water` (default 1 MiB). When false, wait for
`on_drain` before retrying. Ignoring the bool silently drops the write.

### 3. Cancelling drains queued writes

`cancel()` and `stop()` both complete any queued write callbacks with
`StreamError::Closed`. Do not write code that assumes a cancel
"silently forgets" in-flight writes — the callback will fire.

### 4. Restarting after cancel resets the token

`AsyncStream::start()` clears a previously cancelled token before launching
workers. A stream can therefore be cancelled, stopped, and started again for
tests or reusable transports. Do not preserve a stale cancelled token across
restart.

### 5. Null writes are invalid unless size is zero

`write_async(nullptr, 0, cb)` is the normal zero-byte no-op and completes
successfully. `write_async(nullptr, nonzero, cb)` queues the callback with
`StreamError::Invalid` instead of dereferencing the pointer. Keep that
distinction when adding transport wrappers.

### 6. Writes and auto-reads run on separate threads

When `options.auto_read = true` the reader and writer run on
**separate** threads so a blocking `read()` on a `TcpStream` cannot
starve queued writes. Do not re-introduce a single worker loop
without understanding this — request/response flows break otherwise.

## Extending with a new transport

To add WebSocket, S3, or any other transport:

1. Implement `Stream::read` / `write` / `close` / `is_open`.
2. Return `StreamResult::fail(StreamError::WouldBlock)` from `read()`
   when no data is available; `AsyncStream` handles backoff.
3. Keep `read()` non-blocking if at all possible — if it must block,
   document it so callers know to always wrap in `AsyncStream` with
   `auto_read = true`.

## Message channels (Phase 4)

`MessageChannel` is the structured-message layer: one `send()` = one
delivered message. Use it when the peer protocol doesn't tolerate
partial reads (WebSocket, OSC, JSON-RPC, etc.). Callback dispatch
follows the same executor contract as `AsyncStream`.

Patterns that are easy to get wrong:

1. **WebSocket handshake failure returns `nullptr`.** `WebSocketChannel::connect`
   and `::accept` both return an empty unique_ptr on a bad handshake — do not
   dereference blindly.
2. **OSC is UDP; packets can be dropped or reordered.** For anything that
   needs reliability, layer `JsonRpcPeer` over `WebSocketChannel`, not
   `OscChannel`.
3. **`JsonRpcPeer` is symmetric.** Either side can register methods, send
   requests, and fire notifications; a client/server split is only a
   convention.
4. **JSON-RPC params are JSON strings, not choc values.** This keeps the
   public surface free of CHOC types. Format with `choc::json::toString`
   on the way in and `choc::json::parse` on the way out if you need
   structured access.

## References

- Docs: `docs/reference/streams.md` (full API + backpressure flow + MessageChannel)
- Headers: `core/runtime/include/pulp/runtime/{stream,async_stream,network_stream,message_channel,websocket_channel,memory_message_channel,json_rpc}.hpp`, `core/osc/include/pulp/osc/osc_channel.hpp`
- Example: `examples/stream-demo/main.cpp`
- Tests: `test/test_{stream,async_stream,network_stream,websocket_channel,osc_channel,json_rpc}.cpp` — copy these
  patterns for new transport tests
- Feature plan: `planning/next-features-plan.md` § Feature 3 (Phase 1–4 landed)
