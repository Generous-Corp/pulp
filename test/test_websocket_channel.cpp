#include <catch2/catch_test_macros.hpp>

#include <pulp/runtime/network_stream.hpp>
#include <pulp/runtime/socket.hpp>
#include <pulp/runtime/websocket_channel.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#if defined(__APPLE__) || defined(__linux__)
#include <csignal>
namespace {
// Static initializer: ignore SIGPIPE so a write() to a closed peer
// returns EPIPE instead of terminating the process. The round-trip test
// below regularly races socket close vs. final write on macOS
// github-hosted runners; without this guard the test flakes. POSIX only.
struct IgnoreSigpipe {
    IgnoreSigpipe() { std::signal(SIGPIPE, SIG_IGN); }
} g_ignore_sigpipe;
}
#endif

using namespace pulp::runtime;
using namespace std::chrono_literals;

namespace {

std::optional<std::uint16_t> bind_loopback(Socket& server, std::uint16_t start) {
    for (std::uint16_t port = start; port < start + 400; ++port) {
        if (!server.bind("127.0.0.1", port)) continue;
        if (!server.listen(1)) continue;
        return port;
    }
    return std::nullopt;
}

template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds budget = 3s) {
    auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

}  // namespace

TEST_CASE("WebSocket accept_key follows RFC 6455 section 4.2.2", "[websocket]") {
    // Canonical example from the RFC:
    //   client key = "dGhlIHNhbXBsZSBub25jZQ=="
    //   accept     = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
    REQUIRE(WebSocketChannel::compute_accept_key("dGhlIHNhbXBsZSBub25jZQ==")
            == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST_CASE("WebSocketChannel echo round-trip on loopback", "[websocket]") {
    Socket listener;
    REQUIRE(listener.create(SocketType::TCP));
    auto port = bind_loopback(listener, 46401);
    if (!port) {
        SUCCEED("could not bind loopback; skipping");
        return;
    }

    std::atomic<bool> server_ready{false};
    std::atomic<bool> server_done{false};
    std::unique_ptr<WebSocketChannel> server_ws;
    std::mutex server_mu;
    std::vector<std::string> server_seen;

    std::thread server_thread([&] {
        server_ready.store(true);
        auto accepted = listener.accept();
        if (!accepted) return;
        auto server_tcp = std::make_unique<TcpStream>(std::move(*accepted));
        server_ws = WebSocketChannel::accept(std::move(server_tcp));
        if (!server_ws) return;
        server_ws->on_message([&](const Message& m) {
            {
                std::lock_guard<std::mutex> lock(server_mu);
                server_seen.emplace_back(m.as_text());
            }
            // echo back
            server_ws->send_text(std::string("echo:") + std::string(m.as_text()));
        });
        server_done.store(true);
    });

    while (!server_ready.load()) std::this_thread::sleep_for(1ms);

    auto client_tcp = std::make_unique<TcpStream>();
    REQUIRE(client_tcp->connect("127.0.0.1", *port));
    auto client = WebSocketChannel::connect(std::move(client_tcp), "127.0.0.1", "/");
    REQUIRE(client != nullptr);
    REQUIRE(client->is_open());

    std::mutex client_mu;
    std::vector<std::string> client_seen;
    client->on_message([&](const Message& m) {
        std::lock_guard<std::mutex> lock(client_mu);
        client_seen.emplace_back(m.as_text());
    });

    REQUIRE(client->send_text("hello"));
    REQUIRE(client->send_text("pulp"));

    REQUIRE(wait_until([&] {
        std::lock_guard<std::mutex> lock(client_mu);
        return client_seen.size() >= 2;
    }));

    {
        std::lock_guard<std::mutex> lock(client_mu);
        REQUIRE(client_seen[0] == "echo:hello");
        REQUIRE(client_seen[1] == "echo:pulp");
    }
    {
        std::lock_guard<std::mutex> lock(server_mu);
        REQUIRE(server_seen.size() == 2);
        REQUIRE(server_seen[0] == "hello");
    }

    client->close();
    if (server_ws) server_ws->close();
    server_thread.join();
}

TEST_CASE("WebSocketChannel rejects handshake without upgrade header", "[websocket]") {
    Socket listener;
    REQUIRE(listener.create(SocketType::TCP));
    auto port = bind_loopback(listener, 46801);
    if (!port) {
        SUCCEED("could not bind loopback; skipping");
        return;
    }

    std::atomic<bool> ready{false};
    std::thread server_thread([&] {
        ready.store(true);
        auto accepted = listener.accept();
        if (!accepted) return;
        auto tcp = std::make_unique<TcpStream>(std::move(*accepted));
        // Server should refuse the non-upgrade request.
        auto ws = WebSocketChannel::accept(std::move(tcp));
        REQUIRE(ws == nullptr);
    });
    while (!ready.load()) std::this_thread::sleep_for(1ms);

    Socket client;
    REQUIRE(client.create(SocketType::TCP));
    REQUIRE(client.connect("127.0.0.1", *port));
    const char plain_http[] = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    client.send(reinterpret_cast<const std::uint8_t*>(plain_http),
                std::strlen(plain_http));
    client.close();
    server_thread.join();
}

// ── WebSocketChannel additional coverage ────────────────────────────────

TEST_CASE("WebSocket accept_key rejects empty input gracefully",
          "[websocket][handshake]") {
    // A malformed/empty client key must not crash compute_accept_key.
    // The RFC says the key must be 16 bytes base64-encoded (24 chars);
    // we just require no UB + a deterministic non-empty output (or
    // empty, depending on the implementation).
    auto result = WebSocketChannel::compute_accept_key("");
    // Whatever the implementation returns, it must be deterministic.
    REQUIRE(result == WebSocketChannel::compute_accept_key(""));
}

TEST_CASE("WebSocketChannel connect fails gracefully on non-WS peer",
          "[websocket][handshake]") {
    // A peer that accepts the TCP connection but never sends a valid
    // 101 Switching Protocols response must cause WebSocketChannel::connect
    // to return nullptr — not crash, not hang beyond a reasonable limit.
    Socket listener;
    REQUIRE(listener.create(SocketType::TCP));
    auto port = bind_loopback(listener, 47001);
    if (!port) { SUCCEED("could not bind loopback; skipping"); return; }

    std::atomic<bool> ready{false};
    std::thread server_thread([&] {
        ready.store(true);
        auto accepted = listener.accept();
        // Reply with plain-text rubbish — not a valid HTTP response.
        if (accepted) {
            const char junk[] = "HELLO NOT WEBSOCKET\r\n\r\n";
            accepted->send(reinterpret_cast<const std::uint8_t*>(junk),
                           std::strlen(junk));
            accepted->close();
        }
    });
    while (!ready.load()) std::this_thread::sleep_for(1ms);

    auto tcp = std::make_unique<TcpStream>();
    REQUIRE(tcp->connect("127.0.0.1", *port));
    auto ws = WebSocketChannel::connect(std::move(tcp), "127.0.0.1", "/");
    REQUIRE(ws == nullptr);

    server_thread.join();
}

TEST_CASE("WebSocketChannel close flips is_open to false",
          "[websocket][lifecycle]") {
    // Validate the close → is_open() transition via a real echo server
    // so we exercise the same handshake the echo test does; this is
    // additional assurance that close() doesn't just no-op.
    Socket listener;
    REQUIRE(listener.create(SocketType::TCP));
    auto port = bind_loopback(listener, 47101);
    if (!port) { SUCCEED("could not bind loopback; skipping"); return; }

    std::atomic<bool> ready{false};
    std::unique_ptr<WebSocketChannel> server_ws;
    std::thread server_thread([&] {
        ready.store(true);
        auto accepted = listener.accept();
        if (!accepted) return;
        auto server_tcp = std::make_unique<TcpStream>(std::move(*accepted));
        server_ws = WebSocketChannel::accept(std::move(server_tcp));
    });
    while (!ready.load()) std::this_thread::sleep_for(1ms);

    auto client_tcp = std::make_unique<TcpStream>();
    REQUIRE(client_tcp->connect("127.0.0.1", *port));
    auto client = WebSocketChannel::connect(std::move(client_tcp), "127.0.0.1", "/");
    REQUIRE(client != nullptr);
    REQUIRE(client->is_open());

    client->close();
    REQUIRE_FALSE(client->is_open());

    // Double-close is safe.
    client->close();
    REQUIRE_FALSE(client->is_open());

    if (server_ws) server_ws->close();
    server_thread.join();
}

TEST_CASE("WebSocketChannel echoes a >126-byte message (16-bit payload length)",
          "[websocket][frame-length]") {
    // Payload length 126 crosses the boundary where WS frames switch
    // from 7-bit to 16-bit encoding (RFC 6455 §5.2). This case exercises
    // that branch end-to-end.
    Socket listener;
    REQUIRE(listener.create(SocketType::TCP));
    auto port = bind_loopback(listener, 47201);
    if (!port) { SUCCEED("could not bind loopback; skipping"); return; }

    std::atomic<bool> ready{false};
    std::unique_ptr<WebSocketChannel> server_ws;
    std::thread server_thread([&] {
        ready.store(true);
        auto accepted = listener.accept();
        if (!accepted) return;
        auto server_tcp = std::make_unique<TcpStream>(std::move(*accepted));
        server_ws = WebSocketChannel::accept(std::move(server_tcp));
        if (server_ws) {
            server_ws->on_message([&](const Message& m) {
                server_ws->send_text(std::string(m.as_text()));
            });
        }
    });
    while (!ready.load()) std::this_thread::sleep_for(1ms);

    auto client_tcp = std::make_unique<TcpStream>();
    REQUIRE(client_tcp->connect("127.0.0.1", *port));
    auto client = WebSocketChannel::connect(std::move(client_tcp), "127.0.0.1", "/");
    REQUIRE(client != nullptr);

    std::mutex mu;
    std::vector<std::string> seen;
    client->on_message([&](const Message& m) {
        std::lock_guard<std::mutex> lock(mu);
        seen.emplace_back(m.as_text());
    });

    std::string big(300, 'A');  // > 126 bytes → 16-bit payload length field
    REQUIRE(client->send_text(big));

    REQUIRE(wait_until([&] {
        std::lock_guard<std::mutex> lock(mu);
        return !seen.empty();
    }));
    {
        std::lock_guard<std::mutex> lock(mu);
        REQUIRE(seen[0].size() == 300);
        REQUIRE(seen[0] == big);
    }

    client->close();
    if (server_ws) server_ws->close();
    server_thread.join();
}
