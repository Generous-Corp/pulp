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
| Send structured messages (JSON, OSC, WebSocket frames) | **Not yet** — Phase 4 `MessageChannel` is deferred |

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

### 4. Writes and auto-reads run on separate threads

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

## References

- Docs: `docs/reference/streams.md` (full API + backpressure flow)
- Headers: `core/runtime/include/pulp/runtime/{stream,async_stream,network_stream}.hpp`
- Example: `examples/stream-demo/main.cpp`
- Tests: `test/test_{stream,async_stream,network_stream}.cpp` — copy these
  patterns for new transport tests
- Feature plan: `planning/next-features-plan.md` § Feature 3 (Phase 4
  MessageChannel is the next piece, currently deferred)
