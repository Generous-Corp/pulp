# Streams — Unified Async I/O

Pulp's streams module provides one byte-oriented interface across files, memory buffers, named pipes, TCP sockets, and HTTP response bodies. It replaces the ad-hoc `http_get` / `Socket::send` / `NamedPipe::read` surfaces with a common contract that layers cleanly:

| Layer | Header | What it adds |
|-------|--------|--------------|
| `Stream` | `pulp/runtime/stream.hpp` | Synchronous `read`/`write`/`close`/`is_open` with a typed `StreamResult`. |
| `AsyncStream` | `pulp/runtime/async_stream.hpp` | Background worker + callbacks, bounded write queue with backpressure, cancellation token, optional executor for callback routing. |
| `TcpStream`, `HttpStream` | `pulp/runtime/network_stream.hpp` | Network-backed `Stream` subclasses that plug into the same `AsyncStream` wrapper without additional adapter code. |

The layering matters: because `AsyncStream` only knows about `Stream`, adding a new transport (WebSocket, IPC channel, TLS sub-protocol) only requires implementing `Stream::read` and `Stream::write`. Backpressure, cancellation, and callback dispatch come for free.

## `StreamResult` — why it isn't `Result<size_t>`

Every `read`/`write` returns `StreamResult { size_t bytes; StreamError error; }`. Callers normally check `result.ok()`; an `Ok` result with `bytes == 0` means "end of stream" on read or "no room right now" on write.

`StreamError` is deliberately small:

| Value | Meaning |
|-------|---------|
| `Ok` | Operation succeeded. |
| `Closed` | Stream is not open (EOF / peer hung up / never opened). |
| `WouldBlock` | No data / no room right now. Non-blocking sockets use this. |
| `IoError` | OS-level failure. `errno`/`GetLastError` semantics at the backend. |
| `Invalid` | Bad argument or wrong-direction operation (e.g., writing to an `HttpStream`). |

The small enum is deliberate — consumers handle the same five cases across every backend, and `AsyncStream` can translate them into callbacks uniformly.

## Synchronous Stream backends

### `FileStream`

```cpp
FileStream out("cache.bin", FileStream::Mode::Write);
const std::uint8_t payload[] = "hello";
out.write(payload, sizeof(payload));
out.flush();
```

Modes: `Read`, `Write`, `Append`, `ReadWrite`. The stream owns a `std::FILE*` for cross-platform consistency.

### `MemoryStream`

An in-memory byte buffer that satisfies the `Stream` contract. Useful for tests and for round-tripping serialized state without hitting the filesystem.

### `PipeStream`

Wraps `NamedPipe`, which uses directional paired FIFOs under one public pipe name on POSIX and `CreateNamedPipe` on Windows. The pipe is opened by the caller (`create_server` blocks until a peer connects; `connect_client` attaches from the other side) and handed to `PipeStream` for the unified interface. Reads return EOF when the peer closes or exits.

### `TcpStream`

```cpp
TcpStream tcp;
if (!tcp.connect("127.0.0.1", 8080)) { /* error */ }
tcp.write(payload, sizeof(payload));
```

`connect()` performs a blocking DNS + TCP handshake. To keep that off the caller's thread, wrap in `AsyncStream`:

```cpp
auto tcp = std::make_unique<TcpStream>();
tcp->connect("host", port);   // still blocks here; move into a worker first
AsyncStream io(std::move(tcp));
io.start();
```

### `HttpStream`

```cpp
auto stream = HttpStream::get("https://example.com/data.json");
if (stream->status_code() == 200) {
    std::uint8_t buf[4096]{};
    while (true) {
        auto r = stream->read(buf, sizeof(buf));
        if (!r.ok()) break;
        // process buf[0..r.bytes]
    }
}
```

HTTPS is inherited from `cpp-httplib`, which ships with mbedTLS. `HttpStream` is currently read-only: calling `write()` returns `StreamError::Invalid`. Request-body streaming is Phase 4 of the streams feature plan.

## `AsyncStream` — non-blocking with backpressure

`AsyncStream` wraps any `Stream` and runs a background worker that pumps `read`/`write` calls. Events surface as callbacks:

```cpp
AsyncStream::Options opts;
opts.read_chunk = 8 * 1024;          // bytes pulled per iteration
opts.write_high_water = 1 << 20;     // 1 MiB pending-write limit
opts.executor = [&loop](auto fn) { loop.dispatch(std::move(fn)); };

AsyncStream io(std::make_unique<FileStream>("big.wav"), opts);
io.on_data([](const auto* d, auto n) { /* process chunk */ });
io.on_error([](StreamError e) { /* log and degrade */ });
io.on_close([] { /* flush UI state */ });
io.start();
```

### Backpressure

`write_async(buf, size, callback)` returns `false` when the pending byte count would exceed `write_high_water`. Callers should wait for the `on_drain` callback (fired when the queue empties) before retrying. This keeps the write queue bounded so producers cannot exhaust memory when the network is slow.

### Cancellation

Every `AsyncStream` owns a `CancellationToken`. Calling `cancel()` — or destroying the stream — sets the token, drains pending writes with `StreamError::Closed`, exits the worker, and fires `on_close` exactly once. The token can be shared out via `cancellation_token()` so user-level work (retries, coalescing writes) observes cancellation at the same moment the worker does.

### Executor routing

`AsyncStream` intentionally does *not* know about `pulp::events::EventLoop`. Keeping runtime free of event-system dependencies avoids a library link cycle. Instead, callers pass an executor closure:

```cpp
opts.executor = [loop](std::function<void()> fn) { loop->dispatch(std::move(fn)); };
```

When `executor` is empty, callbacks fire inline on the worker thread — convenient for short-lived tools and tests.

## Example

The `stream-demo` example in `examples/stream-demo/` exercises all three layers: synchronous file I/O, an `AsyncStream` over a pre-populated file dispatched onto a `pulp::events::EventLoop`, and an optional HTTP GET via `HttpStream`:

```
./build/examples/stream-demo/pulp-stream-demo
./build/examples/stream-demo/pulp-stream-demo --http https://example.com
```

## Message channels (Phase 4)

Bytewise transports stay in `Stream`. Structured messages — where one `send` matches one delivered message — live behind `pulp::runtime::MessageChannel`:

```cpp
class MessageChannel {
    virtual bool send(const uint8_t* data, size_t size);
    virtual bool send_text(std::string_view);
    virtual void on_message(MessageCallback);
    virtual void on_closed(ChannelClosedCallback);
    virtual void on_error(ChannelErrorCallback);
    virtual void close();
    virtual bool is_open() const;
};
```

Four implementations ship with this phase:

| Channel | Header | Transport |
|---------|--------|-----------|
| `WebSocketChannel` | `pulp/runtime/websocket_channel.hpp` | RFC 6455 over `TcpStream`; client handshake, server handshake, text/binary frames, ping/pong/close. |
| `OscChannel` | `pulp/osc/osc_channel.hpp` | UDP via the existing `pulp::osc::Sender` / `Receiver`. Messages are carried as encoded OSC packets. |
| `MemoryMessageChannel` | `pulp/runtime/memory_message_channel.hpp` | In-process pair for tests and intra-process bridges. |
| `JsonRpcPeer` | `pulp/runtime/json_rpc.hpp` | JSON-RPC 2.0 *over* any `MessageChannel` — symmetric peer that can send/serve requests and notifications. |

### WebSocket

```cpp
auto tcp = std::make_unique<TcpStream>();
tcp->connect("example.com", 80);
auto ws = WebSocketChannel::connect(std::move(tcp), "example.com", "/chat");
ws->on_message([](const Message& m) { /* m.kind is Text or Binary */ });
ws->send_text("hello");
```

- TLS is not currently handled at the channel layer — for `wss://`, supply a TLS-wrapped TCP stream (future `SecureTcpStream`).
- Control frames (ping/pong/close) are handled internally; ping triggers an automatic pong.
- Payloads up to `options.max_payload` (default 16 MiB) are accepted; larger frames fire `on_error` and close the channel.

### OSC

```cpp
auto osc = OscChannel::open("127.0.0.1", /*remote=*/8000, /*local=*/9000);
osc->on_message([](const Message& m) {
    auto decoded = pulp::osc::decode(m.payload.data(), m.payload.size());
});
osc->send(pulp::osc::Message("/synth/freq").add(440.f));
```

### JSON-RPC

`JsonRpcPeer` is symmetric — either side can send requests, serve requests, emit notifications, and subscribe to notifications. It works over any `MessageChannel`:

```cpp
JsonRpcPeer peer(*ws);
peer.register_method("add", [](std::string_view params) {
    // params is a JSON-encoded array/object; return a JSON-encoded result
    return JsonRpcResult::ok("42");
});
peer.send_request("echo", R"(["hi"])", [](const JsonRpcResult& r) {
    if (r.error) { /* handle */ } else { /* r.result_json */ }
});
peer.notify("progress", R"({"percent":50})");
```

Errors follow JSON-RPC 2.0 §5: `-32601` is reported for unknown methods, `-32603` for handler exceptions, and custom codes can be returned via `JsonRpcResult::fail`.

## Further reading

- Feature plan: `planning/next-features-plan.md` § Feature 3
- Ralph automation prompt: `planning/ralph-prompt-streams.md`
- Related headers: `pulp/runtime/socket.hpp`, `pulp/runtime/http.hpp`, `pulp/runtime/named_pipe.hpp`, `pulp/runtime/message_channel.hpp`, `pulp/osc/osc_channel.hpp`
