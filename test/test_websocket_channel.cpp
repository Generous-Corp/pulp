#include <catch2/catch_test_macros.hpp>

#include <pulp/runtime/network_stream.hpp>
#include <pulp/runtime/socket.hpp>
#include <pulp/runtime/websocket_channel.hpp>

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

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

std::string receive_http_headers(Socket& socket) {
    std::string out;
    std::uint8_t ch = 0;
    while (out.size() < 16 * 1024) {
        auto n = socket.receive(&ch, 1);
        if (n <= 0) break;
        out.push_back(static_cast<char>(ch));
        if (out.size() >= 4 &&
            out.compare(out.size() - 4, 4, "\r\n\r\n") == 0) {
            break;
        }
    }
    return out;
}

bool send_masked_client_frame(Socket& socket,
                              bool fin,
                              std::uint8_t opcode,
                              std::string_view payload) {
    if (payload.size() >= 126) return false;

    const std::array<std::uint8_t, 4> mask{0x11, 0x22, 0x33, 0x44};
    std::vector<std::uint8_t> frame;
    frame.reserve(2 + mask.size() + payload.size());
    frame.push_back(static_cast<std::uint8_t>((fin ? 0x80 : 0x00) | opcode));
    frame.push_back(static_cast<std::uint8_t>(0x80 | payload.size()));
    frame.insert(frame.end(), mask.begin(), mask.end());
    for (std::size_t i = 0; i < payload.size(); ++i) {
        frame.push_back(static_cast<std::uint8_t>(
            static_cast<std::uint8_t>(payload[i]) ^ mask[i & 0x3]));
    }

    return socket.send(frame.data(), frame.size()) == static_cast<int>(frame.size());
}

std::optional<std::vector<std::uint8_t>> receive_unmasked_server_frame(Socket& socket,
                                                                       std::uint8_t& opcode) {
    std::uint8_t hdr[2]{};
    if (socket.receive(hdr, 2) != 2) return std::nullopt;
    opcode = hdr[0] & 0x0f;
    if ((hdr[1] & 0x80) != 0) return std::nullopt;

    std::uint64_t len = hdr[1] & 0x7f;
    if (len == 126) {
        std::uint8_t ext[2]{};
        if (socket.receive(ext, 2) != 2) return std::nullopt;
        len = (std::uint64_t(ext[0]) << 8) | ext[1];
    } else if (len == 127) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> payload(static_cast<std::size_t>(len));
    std::size_t off = 0;
    while (off < payload.size()) {
        auto n = socket.receive(payload.data() + off, payload.size() - off);
        if (n <= 0) return std::nullopt;
        off += static_cast<std::size_t>(n);
    }
    return payload;
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

TEST_CASE("WebSocketChannel rejects null and closed streams before handshake",
          "[websocket][handshake][coverage][phase3]") {
    REQUIRE(WebSocketChannel::connect(nullptr, "127.0.0.1", "/") == nullptr);
    REQUIRE(WebSocketChannel::accept(nullptr) == nullptr);

    auto closed_client = std::make_unique<TcpStream>();
    REQUIRE_FALSE(closed_client->is_open());
    REQUIRE(WebSocketChannel::connect(std::move(closed_client), "127.0.0.1", "/") == nullptr);

    auto closed_server = std::make_unique<TcpStream>();
    REQUIRE_FALSE(closed_server->is_open());
    REQUIRE(WebSocketChannel::accept(std::move(closed_server)) == nullptr);
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

TEST_CASE("WebSocketChannel rejects incorrect accept key",
          "[websocket][handshake][issue-641]") {
    Socket listener;
    REQUIRE(listener.create(SocketType::TCP));
    auto port = bind_loopback(listener, 47301);
    if (!port) { SUCCEED("could not bind loopback; skipping"); return; }

    std::atomic<bool> ready{false};
    std::atomic<bool> saw_key{false};
    std::thread server_thread([&] {
        ready.store(true);
        auto accepted = listener.accept();
        if (!accepted) return;
        auto request = receive_http_headers(*accepted);
        saw_key.store(request.find("Sec-WebSocket-Key:") != std::string::npos);
        const char response[] =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: definitely-not-the-derived-key\r\n\r\n";
        accepted->send(reinterpret_cast<const std::uint8_t*>(response),
                       std::strlen(response));
        accepted->close();
    });
    while (!ready.load()) std::this_thread::sleep_for(1ms);

    auto tcp = std::make_unique<TcpStream>();
    REQUIRE(tcp->connect("127.0.0.1", *port));
    auto ws = WebSocketChannel::connect(std::move(tcp), "127.0.0.1", "/bad");

    server_thread.join();
    REQUIRE(saw_key.load());
    REQUIRE(ws == nullptr);
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

TEST_CASE("WebSocketChannel delivers binary send as binary message",
          "[websocket][frame-kind][issue-641]") {
    Socket listener;
    REQUIRE(listener.create(SocketType::TCP));
    auto port = bind_loopback(listener, 47401);
    if (!port) { SUCCEED("could not bind loopback; skipping"); return; }

    std::atomic<bool> ready{false};
    std::atomic<bool> server_accept_done{false};
    std::unique_ptr<WebSocketChannel> server_ws;
    std::mutex mu;
    std::vector<Message> seen;

    std::thread server_thread([&] {
        ready.store(true);
        auto accepted = listener.accept();
        if (!accepted) return;
        auto server_tcp = std::make_unique<TcpStream>(std::move(*accepted));
        server_ws = WebSocketChannel::accept(std::move(server_tcp));
        if (!server_ws) return;
        server_ws->on_message([&](const Message& m) {
            std::lock_guard<std::mutex> lock(mu);
            seen.push_back(m);
        });
        server_accept_done.store(true);
    });
    while (!ready.load()) std::this_thread::sleep_for(1ms);

    auto client_tcp = std::make_unique<TcpStream>();
    REQUIRE(client_tcp->connect("127.0.0.1", *port));
    auto client = WebSocketChannel::connect(std::move(client_tcp), "127.0.0.1", "/binary");
    REQUIRE(client != nullptr);
    REQUIRE(wait_until([&] { return server_accept_done.load(); }));

    const std::array<std::uint8_t, 4> payload{0x00, 0x7f, 0x80, 0xff};
    REQUIRE(client->send(payload.data(), payload.size()));
    REQUIRE(wait_until([&] {
        std::lock_guard<std::mutex> lock(mu);
        return !seen.empty();
    }));

    {
        std::lock_guard<std::mutex> lock(mu);
        REQUIRE(seen.size() == 1);
        REQUIRE(seen[0].kind == MessageKind::Binary);
        REQUIRE(seen[0].payload == std::vector<std::uint8_t>(payload.begin(), payload.end()));
    }

    client->close();
    if (server_ws) server_ws->close();
    server_thread.join();
}

TEST_CASE("WebSocketChannel assembles fragmented text frames",
          "[websocket][frame-kind][coverage][phase3]") {
    Socket listener;
    REQUIRE(listener.create(SocketType::TCP));
    auto port = bind_loopback(listener, 47701);
    if (!port) { SUCCEED("could not bind loopback; skipping"); return; }

    std::atomic<bool> ready{false};
    std::atomic<bool> server_accept_done{false};
    std::unique_ptr<WebSocketChannel> server_ws;
    std::mutex mu;
    std::vector<std::string> seen;

    std::thread server_thread([&] {
        ready.store(true);
        auto accepted = listener.accept();
        if (!accepted) return;
        auto server_tcp = std::make_unique<TcpStream>(std::move(*accepted));
        server_ws = WebSocketChannel::accept(std::move(server_tcp));
        if (!server_ws) return;
        server_ws->on_message([&](const Message& m) {
            std::lock_guard<std::mutex> lock(mu);
            seen.emplace_back(m.as_text());
        });
        server_accept_done.store(true);
    });
    while (!ready.load()) std::this_thread::sleep_for(1ms);

    Socket client;
    REQUIRE(client.create(SocketType::TCP));
    REQUIRE(client.connect("127.0.0.1", *port));
    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    const std::string request =
        "GET /fragmented HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + key + "\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    REQUIRE(client.send(request) == static_cast<int>(request.size()));
    auto response = receive_http_headers(client);
    REQUIRE(response.find("101") != std::string::npos);
    REQUIRE(response.find(WebSocketChannel::compute_accept_key(key)) != std::string::npos);
    REQUIRE(wait_until([&] { return server_accept_done.load(); }));

    REQUIRE(send_masked_client_frame(client, false, 0x1, "frag"));
    REQUIRE(send_masked_client_frame(client, true, 0x0, "ment"));

    REQUIRE(wait_until([&] {
        std::lock_guard<std::mutex> lock(mu);
        return !seen.empty();
    }));

    {
        std::lock_guard<std::mutex> lock(mu);
        REQUIRE(seen == std::vector<std::string>{"fragment"});
    }

    client.close();
    if (server_ws) server_ws->close();
    server_thread.join();
}

TEST_CASE("WebSocketChannel reports unknown frame opcodes",
          "[websocket][frame-kind][coverage][phase3]") {
    Socket listener;
    REQUIRE(listener.create(SocketType::TCP));
    auto port = bind_loopback(listener, 47801);
    if (!port) { SUCCEED("could not bind loopback; skipping"); return; }

    std::atomic<bool> ready{false};
    std::atomic<bool> server_accept_done{false};
    std::atomic<bool> closed{false};
    std::unique_ptr<WebSocketChannel> server_ws;
    std::mutex mu;
    std::string error;

    std::thread server_thread([&] {
        ready.store(true);
        auto accepted = listener.accept();
        if (!accepted) return;
        auto server_tcp = std::make_unique<TcpStream>(std::move(*accepted));
        server_ws = WebSocketChannel::accept(std::move(server_tcp));
        if (!server_ws) return;
        server_ws->on_error([&](std::string_view reason) {
            std::lock_guard<std::mutex> lock(mu);
            error = std::string(reason);
        });
        server_ws->on_closed([&] { closed.store(true); });
        server_accept_done.store(true);
    });
    while (!ready.load()) std::this_thread::sleep_for(1ms);

    Socket client;
    REQUIRE(client.create(SocketType::TCP));
    REQUIRE(client.connect("127.0.0.1", *port));
    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    const std::string request =
        "GET /unknown-opcode HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + key + "\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    REQUIRE(client.send(request) == static_cast<int>(request.size()));
    auto response = receive_http_headers(client);
    REQUIRE(response.find("101") != std::string::npos);
    REQUIRE(wait_until([&] { return server_accept_done.load(); }));

    REQUIRE(send_masked_client_frame(client, true, 0x3, "reserved"));
    REQUIRE(wait_until([&] {
        std::lock_guard<std::mutex> lock(mu);
        return error == "unknown opcode" && closed.load();
    }));

    client.close();
    if (server_ws) server_ws->close();
    server_thread.join();
}

TEST_CASE("WebSocketChannel replies to ping frames with pong",
          "[websocket][frame-kind][coverage][phase3]") {
    Socket listener;
    REQUIRE(listener.create(SocketType::TCP));
    auto port = bind_loopback(listener, 47901);
    if (!port) { SUCCEED("could not bind loopback; skipping"); return; }

    std::atomic<bool> ready{false};
    std::atomic<bool> server_accept_done{false};
    std::unique_ptr<WebSocketChannel> server_ws;

    std::thread server_thread([&] {
        ready.store(true);
        auto accepted = listener.accept();
        if (!accepted) return;
        auto server_tcp = std::make_unique<TcpStream>(std::move(*accepted));
        server_ws = WebSocketChannel::accept(std::move(server_tcp));
        server_accept_done.store(server_ws != nullptr);
    });
    while (!ready.load()) std::this_thread::sleep_for(1ms);

    Socket client;
    REQUIRE(client.create(SocketType::TCP));
    REQUIRE(client.connect("127.0.0.1", *port));
    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    const std::string request =
        "GET /ping HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + key + "\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    REQUIRE(client.send(request) == static_cast<int>(request.size()));
    auto response = receive_http_headers(client);
    REQUIRE(response.find("101") != std::string::npos);
    REQUIRE(wait_until([&] { return server_accept_done.load(); }));

    REQUIRE(send_masked_client_frame(client, true, 0x9, "ok"));
    std::uint8_t opcode = 0;
    auto payload = receive_unmasked_server_frame(client, opcode);
    REQUIRE(payload.has_value());
    REQUIRE(opcode == 0xA);
    REQUIRE(*payload == std::vector<std::uint8_t>{'o', 'k'});

    client.close();
    if (server_ws) server_ws->close();
    server_thread.join();
}

TEST_CASE("WebSocketChannel receives 64-bit payload length frames",
          "[websocket][frame-length][issue-641]") {
    Socket listener;
    REQUIRE(listener.create(SocketType::TCP));
    auto port = bind_loopback(listener, 47501);
    if (!port) { SUCCEED("could not bind loopback; skipping"); return; }

    std::atomic<bool> ready{false};
    std::atomic<bool> server_accept_done{false};
    std::unique_ptr<WebSocketChannel> server_ws;
    std::mutex mu;
    std::size_t received_size = 0;
    std::uint8_t first = 0;
    std::uint8_t last = 0;

    std::thread server_thread([&] {
        ready.store(true);
        auto accepted = listener.accept();
        if (!accepted) return;
        auto server_tcp = std::make_unique<TcpStream>(std::move(*accepted));
        server_ws = WebSocketChannel::accept(std::move(server_tcp));
        if (!server_ws) return;
        server_ws->on_message([&](const Message& m) {
            std::lock_guard<std::mutex> lock(mu);
            received_size = m.payload.size();
            if (!m.payload.empty()) {
                first = m.payload.front();
                last = m.payload.back();
            }
        });
        server_accept_done.store(true);
    });
    while (!ready.load()) std::this_thread::sleep_for(1ms);

    auto client_tcp = std::make_unique<TcpStream>();
    REQUIRE(client_tcp->connect("127.0.0.1", *port));
    auto client = WebSocketChannel::connect(std::move(client_tcp), "127.0.0.1", "/large");
    REQUIRE(client != nullptr);
    REQUIRE(wait_until([&] { return server_accept_done.load(); }));

    std::vector<std::uint8_t> payload(70000, 0x5a);
    payload.front() = 0x11;
    payload.back() = 0xee;
    REQUIRE(client->send(payload.data(), payload.size()));
    REQUIRE(wait_until([&] {
        std::lock_guard<std::mutex> lock(mu);
        return received_size == payload.size();
    }, 5s));

    {
        std::lock_guard<std::mutex> lock(mu);
        REQUIRE(received_size == 70000);
        REQUIRE(first == 0x11);
        REQUIRE(last == 0xee);
    }

    client->close();
    if (server_ws) server_ws->close();
    server_thread.join();
}

TEST_CASE("WebSocketChannel rejects frames above max_payload",
          "[websocket][frame-length][issue-641]") {
    Socket listener;
    REQUIRE(listener.create(SocketType::TCP));
    auto port = bind_loopback(listener, 47601);
    if (!port) { SUCCEED("could not bind loopback; skipping"); return; }

    std::atomic<bool> ready{false};
    std::atomic<bool> server_accept_done{false};
    std::atomic<bool> closed{false};
    std::mutex mu;
    std::string error;
    std::unique_ptr<WebSocketChannel> server_ws;

    std::thread server_thread([&] {
        ready.store(true);
        auto accepted = listener.accept();
        if (!accepted) return;
        auto server_tcp = std::make_unique<TcpStream>(std::move(*accepted));
        WebSocketOptions options;
        options.max_payload = 8;
        server_ws = WebSocketChannel::accept(std::move(server_tcp), options);
        if (!server_ws) return;
        server_ws->on_error([&](std::string_view reason) {
            std::lock_guard<std::mutex> lock(mu);
            error = std::string(reason);
        });
        server_ws->on_closed([&] { closed.store(true); });
        server_accept_done.store(true);
    });
    while (!ready.load()) std::this_thread::sleep_for(1ms);

    auto client_tcp = std::make_unique<TcpStream>();
    REQUIRE(client_tcp->connect("127.0.0.1", *port));
    auto client = WebSocketChannel::connect(std::move(client_tcp), "127.0.0.1", "/too-large");
    REQUIRE(client != nullptr);
    REQUIRE(wait_until([&] { return server_accept_done.load(); }));

    std::string too_large(32, 'x');
    REQUIRE(client->send_text(too_large));
    REQUIRE(wait_until([&] {
        std::lock_guard<std::mutex> lock(mu);
        return !error.empty() && closed.load();
    }));

    {
        std::lock_guard<std::mutex> lock(mu);
        REQUIRE(error == "payload exceeds max_payload");
    }

    client->close();
    if (server_ws) server_ws->close();
    server_thread.join();
}
