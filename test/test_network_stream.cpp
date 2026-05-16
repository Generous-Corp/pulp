#include <catch2/catch_test_macros.hpp>

#include <pulp/runtime/ip_address.hpp>
#include <pulp/runtime/network_stream.hpp>
#include <pulp/runtime/socket.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

using namespace pulp::runtime;
using namespace std::chrono_literals;

namespace {

// Pick an ephemeral port and return the bound listener + its actual port.
std::optional<std::uint16_t> try_bind_loopback(Socket& server, std::uint16_t port) {
    if (!server.bind("127.0.0.1", port)) return std::nullopt;
    if (!server.listen(1)) return std::nullopt;
    return port;
}

}  // namespace

// ── IP address helpers ──────────────────────────────────────────────────

TEST_CASE("IPv4 validation accepts boundary dotted quads",
          "[network_stream][ip-address][issue-641]") {
    REQUIRE(is_valid_ipv4("0.0.0.0"));
    REQUIRE(is_valid_ipv4("127.0.0.1"));
    REQUIRE(is_valid_ipv4("192.168.1.254"));
    REQUIRE(is_valid_ipv4("255.255.255.255"));
}

TEST_CASE("IPv4 validation rejects malformed addresses",
          "[network_stream][ip-address][issue-641]") {
    REQUIRE_FALSE(is_valid_ipv4(""));
    REQUIRE_FALSE(is_valid_ipv4("localhost"));
    REQUIRE_FALSE(is_valid_ipv4("256.0.0.1"));
    REQUIRE_FALSE(is_valid_ipv4("1.2.3"));
    REQUIRE_FALSE(is_valid_ipv4("1.2.3.4.5"));
    REQUIRE_FALSE(is_valid_ipv4(" 127.0.0.1"));
}

TEST_CASE("local IPv4 helpers return valid fallback or interface addresses",
          "[network_stream][ip-address][issue-641]") {
    auto addresses = local_ipv4_addresses();
    for (const auto& address : addresses) {
        REQUIRE(is_valid_ipv4(address));
        REQUIRE(address != "127.0.0.1");
    }

    auto primary = local_ipv4_address();
    REQUIRE(is_valid_ipv4(primary));
    if (addresses.empty()) {
        REQUIRE(primary == "127.0.0.1");
    } else {
        REQUIRE(primary == addresses.front());
    }

    REQUIRE(hostname().find('\0') == std::string::npos);
}

TEST_CASE("TcpStream round-trips bytes on loopback", "[network_stream]") {
    Socket server;
    REQUIRE(server.create(SocketType::TCP));

    std::uint16_t port = 0;
    for (std::uint16_t candidate = 45321; candidate < 45400; ++candidate) {
        if (auto bound = try_bind_loopback(server, candidate)) {
            port = *bound;
            break;
        }
    }
    if (port == 0) {
        SUCCEED("could not bind loopback port; skipping");
        return;
    }

    std::atomic<bool> server_ready{false};
    std::atomic<bool> server_done{false};
    std::thread server_thread([&] {
        server_ready.store(true);
        auto client = server.accept();
        if (!client) return;
        std::uint8_t buf[32]{};
        int n = client->receive(buf, sizeof(buf));
        if (n > 0) {
            client->send(buf, static_cast<std::size_t>(n));  // echo back
        }
        client->close();
        server_done.store(true);
    });

    // Wait for the server to be listening before connecting.
    while (!server_ready.load()) std::this_thread::sleep_for(1ms);

    TcpStream stream;
    REQUIRE(stream.connect("127.0.0.1", port));

    const std::uint8_t payload[] = {'h', 'i', '-', 'p', 'u', 'l', 'p'};
    auto w = stream.write(payload, sizeof(payload));
    REQUIRE(w.ok());
    REQUIRE(w.bytes == sizeof(payload));

    std::uint8_t recv[sizeof(payload)]{};
    std::size_t got = 0;
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (got < sizeof(payload) && std::chrono::steady_clock::now() < deadline) {
        auto r = stream.read(recv + got, sizeof(recv) - got);
        if (r.ok()) {
            got += r.bytes;
        } else if (r.closed()) {
            break;
        } else {
            std::this_thread::sleep_for(5ms);
        }
    }

    REQUIRE(got == sizeof(payload));
    REQUIRE(std::memcmp(recv, payload, sizeof(payload)) == 0);

    stream.close();
    server_thread.join();
    REQUIRE(server_done.load());
}

TEST_CASE("HttpStream reports transport error for unreachable host", "[network_stream]") {
    HttpStream::Request req;
    req.url = "http://127.0.0.1:1";  // guaranteed-unused port
    req.timeout_seconds = 1;
    HttpStream stream(req);

    // Depending on the OS, connection refused may or may not set error —
    // but either status_code == 0 or an empty body must hold, and read()
    // must immediately report Closed.
    std::uint8_t buf[4]{};
    auto r = stream.read(buf, sizeof(buf));
    REQUIRE_FALSE(r.ok());
    REQUIRE((r.closed() || r.error == StreamError::IoError));
}

TEST_CASE("HttpStream write is Invalid (read-only)", "[network_stream]") {
    HttpStream::Request req;
    req.url = "http://127.0.0.1:1";
    req.timeout_seconds = 1;
    HttpStream stream(req);

    std::uint8_t payload[3] = {1, 2, 3};
    auto w = stream.write(payload, sizeof(payload));
    REQUIRE_FALSE(w.ok());
    REQUIRE(w.error == StreamError::Invalid);
}

// ── TcpStream edge cases ────────────────────────────────────────────────

TEST_CASE("TcpStream read on unconnected stream returns Closed",
          "[network_stream][tcp][edge]") {
    TcpStream stream;
    REQUIRE_FALSE(stream.is_open());
    std::uint8_t buf[4]{};
    auto r = stream.read(buf, sizeof(buf));
    REQUIRE_FALSE(r.ok());
    // Stream contract: reading an unopened stream is Closed (EOF-equivalent)
    // or Invalid (state error) — both are non-ok.
    REQUIRE((r.closed() || r.error == StreamError::Invalid));
}

TEST_CASE("TcpStream write on unconnected stream is non-ok",
          "[network_stream][tcp][edge]") {
    TcpStream stream;
    const std::uint8_t payload[] = {'x'};
    auto w = stream.write(payload, sizeof(payload));
    REQUIRE_FALSE(w.ok());
}

TEST_CASE("TcpStream connect to unreachable host:port fails fast",
          "[network_stream][tcp][edge]") {
    TcpStream stream;
    // Port 1 on loopback is never bound in a default environment;
    // the kernel returns ECONNREFUSED synchronously from connect().
    REQUIRE_FALSE(stream.connect("127.0.0.1", 1));
    REQUIRE_FALSE(stream.is_open());
}

TEST_CASE("TcpStream double-close is safe and idempotent",
          "[network_stream][tcp][edge]") {
    TcpStream stream;
    stream.close();        // close on never-opened stream
    stream.close();        // and again — must not crash/UB
    REQUIRE_FALSE(stream.is_open());
}

TEST_CASE("TcpStream detects peer-close on the next read",
          "[network_stream][tcp][peer-close]") {
    Socket server;
    REQUIRE(server.create(SocketType::TCP));

    std::uint16_t port = 0;
    for (std::uint16_t candidate = 45501; candidate < 45580; ++candidate) {
        if (auto bound = try_bind_loopback(server, candidate)) {
            port = *bound;
            break;
        }
    }
    if (port == 0) {
        SUCCEED("could not bind loopback; skipping");
        return;
    }

    std::atomic<bool> server_ready{false};
    std::thread server_thread([&] {
        server_ready.store(true);
        auto client = server.accept();
        if (!client) return;
        client->close();  // immediate peer-close
    });
    while (!server_ready.load()) std::this_thread::sleep_for(1ms);

    TcpStream stream;
    REQUIRE(stream.connect("127.0.0.1", port));

    // Read until the peer-close is observed. The first read may return
    // WouldBlock on some platforms before the FIN propagates.
    std::uint8_t buf[8]{};
    bool saw_close = false;
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        auto r = stream.read(buf, sizeof(buf));
        if (r.closed()) { saw_close = true; break; }
        if (r.ok() && r.bytes == 0) { saw_close = true; break; }
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(saw_close);

    stream.close();
    server_thread.join();
}

// ── HttpStream edge cases ───────────────────────────────────────────────

TEST_CASE("HttpStream with empty URL reports transport failure",
          "[network_stream][http][edge]") {
    HttpStream::Request req;
    req.url = "";
    req.timeout_seconds = 1;
    HttpStream stream(req);

    std::uint8_t buf[4]{};
    auto r = stream.read(buf, sizeof(buf));
    REQUIRE_FALSE(r.ok());
}

TEST_CASE("HttpStream with unsupported scheme reports transport failure",
          "[network_stream][http][edge]") {
    HttpStream::Request req;
    req.url = "ftp://127.0.0.1/nope";
    req.timeout_seconds = 1;
    HttpStream stream(req);

    std::uint8_t buf[4]{};
    auto r = stream.read(buf, sizeof(buf));
    REQUIRE_FALSE(r.ok());
}

TEST_CASE("HttpStream status_code is 0 before fetch",
          "[network_stream][http][state]") {
    HttpStream stream;
    REQUIRE(stream.status_code() == 0);
}

TEST_CASE("HttpStream default state supports zero reads and idempotent close",
          "[network_stream][http][coverage][phase3]") {
    HttpStream stream;
    REQUIRE_FALSE(stream.is_open());
    REQUIRE(stream.eof());
    REQUIRE(stream.transport_error().empty());
    REQUIRE(stream.headers().empty());

    std::uint8_t byte = 0x7f;
    auto zero_read = stream.read(&byte, 0);
    REQUIRE(zero_read.ok());
    REQUIRE(zero_read.bytes == 0);
    REQUIRE(byte == 0x7f);

    auto eof_read = stream.read(&byte, 1);
    REQUIRE_FALSE(eof_read.ok());
    REQUIRE(eof_read.closed());

    stream.close();
    stream.close();
    REQUIRE_FALSE(stream.is_open());
    auto closed_read = stream.read(&byte, 1);
    REQUIRE_FALSE(closed_read.ok());
    REQUIRE(closed_read.closed());
}
