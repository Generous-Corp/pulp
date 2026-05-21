#include <catch2/catch_test_macros.hpp>

#include <pulp/runtime/ip_address.hpp>
#include <pulp/runtime/network_stream.hpp>
#include <pulp/runtime/socket.hpp>
#include "../external/cpp-httplib/httplib.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <optional>
#include <string>
#include <thread>

using namespace pulp::runtime;
using namespace std::chrono_literals;

namespace {

struct ThreadJoiner {
    std::thread& thread;

    ~ThreadJoiner() {
        join();
    }

    void join() {
        if (thread.joinable()) {
            thread.join();
        }
    }
};

struct HttpServerRunner {
    explicit HttpServerRunner(httplib::Server& s)
        : server(s)
        , thread([this] { server.listen_after_bind(); }) {}

    ~HttpServerRunner() {
        server.stop();
        if (thread.joinable()) {
            thread.join();
        }
    }

    bool wait_until_running(std::chrono::milliseconds timeout = 2s) const {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!server.is_running() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(5ms);
        }
        return server.is_running();
    }

    httplib::Server& server;
    std::thread thread;
};

bool wait_until_http_ready(int port, std::chrono::milliseconds timeout = 30s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        httplib::Client client("127.0.0.1", port);
        client.set_connection_timeout(0, 500000);
        client.set_read_timeout(0, 500000);
        if (auto response = client.Get("/__ready");
            response && response->status == 204) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

// Pick an ephemeral port and return the bound listener + its actual port.
std::optional<std::uint16_t> try_bind_loopback(Socket& server, std::uint16_t port) {
    if (!server.bind("127.0.0.1", port)) return std::nullopt;
    if (!server.listen(1)) return std::nullopt;
    return port;
}

std::optional<std::uint16_t> try_bind_loopback_ephemeral(Socket& server) {
    if (!server.bind("127.0.0.1", 0)) return std::nullopt;
    if (!server.listen(1)) return std::nullopt;
    const auto port = server.local_port();
    if (port == 0) return std::nullopt;
    return port;
}

std::optional<std::uint16_t> try_bind_udp_ephemeral(Socket& socket,
                                                    std::string_view address = "127.0.0.1") {
    if (!socket.bind(address, 0)) return std::nullopt;
    const auto port = socket.local_port();
    if (port == 0) return std::nullopt;
    return port;
}

std::optional<std::uint16_t> try_bind_udp(Socket& socket,
                                          std::uint16_t start,
                                          std::string_view address = "127.0.0.1") {
    for (std::uint16_t port = start; port < start + 120; ++port) {
        if (socket.bind(address, port)) return port;
    }
    return std::nullopt;
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

TEST_CASE("IPv4 validation rejects decorated dotted quads",
          "[network_stream][ip-address][phase3]") {
    REQUIRE_FALSE(is_valid_ipv4("127.0.0.1:80"));
    REQUIRE_FALSE(is_valid_ipv4("127.0.0.1/32"));
    REQUIRE_FALSE(is_valid_ipv4("127.0.0.1\n"));
    REQUIRE_FALSE(is_valid_ipv4("\t127.0.0.1"));
    REQUIRE_FALSE(is_valid_ipv4("http://127.0.0.1"));
}

TEST_CASE("IPv4 validation accepts private network examples",
          "[network_stream][ip-address][phase3]") {
    REQUIRE(is_valid_ipv4("10.0.0.1"));
    REQUIRE(is_valid_ipv4("172.16.0.1"));
    REQUIRE(is_valid_ipv4("192.168.0.1"));
    REQUIRE(is_valid_ipv4("169.254.10.20"));
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
    ThreadJoiner join_server{server_thread};

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
    join_server.join();
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
    ThreadJoiner join_server{server_thread};
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
    join_server.join();
}

// ── Socket edge cases ───────────────────────────────────────────────────

TEST_CASE("Socket reports failures before create",
          "[network_stream][socket][phase3]") {
    Socket socket;
    std::array<std::uint8_t, 4> buffer{1, 2, 3, 4};
    std::string from = "unchanged";
    std::uint16_t from_port = 99;

    REQUIRE_FALSE(socket.is_open());
    REQUIRE_FALSE(socket.bind("127.0.0.1", 1));
    REQUIRE_FALSE(socket.listen());
    REQUIRE_FALSE(socket.accept().has_value());
    REQUIRE(socket.send(buffer.data(), buffer.size()) == -1);
    REQUIRE(socket.send("payload") == -1);
    REQUIRE(socket.send_to(buffer.data(), buffer.size(), "127.0.0.1", 1) == -1);
    REQUIRE(socket.receive(buffer.data(), buffer.size()) == -1);
    REQUIRE(socket.receive_from(buffer.data(), buffer.size(), from, from_port) == -1);
    REQUIRE(from == "unchanged");
    REQUIRE(from_port == 99);
}

TEST_CASE("Socket create close and recreate toggles open state",
          "[network_stream][socket][phase3]") {
    Socket socket;
    REQUIRE(socket.create(SocketType::UDP));
    REQUIRE(socket.is_open());
    socket.close();
    REQUIRE_FALSE(socket.is_open());

    REQUIRE(socket.create(SocketType::TCP));
    REQUIRE(socket.is_open());
    socket.close();
    socket.close();
    REQUIRE_FALSE(socket.is_open());
}

TEST_CASE("UDP socket round-trips datagrams on loopback",
          "[network_stream][socket][udp][phase3]") {
    Socket receiver;
    REQUIRE(receiver.create(SocketType::UDP));
    auto port = try_bind_udp(receiver, 45601);
    if (!port) {
        SUCCEED("could not bind UDP loopback; skipping");
        return;
    }

    Socket sender;
    REQUIRE(sender.create(SocketType::UDP));
    const std::array<std::uint8_t, 5> payload{'p', 'u', 'l', 'p', '!'};
    REQUIRE(sender.send_to(payload.data(), payload.size(), "127.0.0.1", *port)
            == static_cast<int>(payload.size()));

    std::array<std::uint8_t, 16> buffer{};
    std::string from;
    std::uint16_t from_port = 0;
    auto received = receiver.receive_from(buffer.data(), buffer.size(), from, from_port);
    REQUIRE(received == static_cast<int>(payload.size()));
    REQUIRE(std::equal(payload.begin(), payload.end(), buffer.begin()));
    REQUIRE(from == "127.0.0.1");
    REQUIRE(from_port != 0);
}

TEST_CASE("UDP bind accepts empty address as wildcard",
          "[network_stream][socket][udp][phase3]") {
    Socket receiver;
    REQUIRE(receiver.create(SocketType::UDP));
    auto port = try_bind_udp(receiver, 45731, "");
    if (!port) {
        SUCCEED("could not bind UDP wildcard; skipping");
        return;
    }

    Socket sender;
    REQUIRE(sender.create(SocketType::UDP));
    const std::array<std::uint8_t, 3> payload{'a', 'n', 'y'};
    REQUIRE(sender.send_to(payload.data(), payload.size(), "127.0.0.1", *port)
            == static_cast<int>(payload.size()));

    std::array<std::uint8_t, 8> buffer{};
    std::string from;
    std::uint16_t from_port = 0;
    REQUIRE(receiver.receive_from(buffer.data(), buffer.size(), from, from_port)
            == static_cast<int>(payload.size()));
    REQUIRE(std::equal(payload.begin(), payload.end(), buffer.begin()));
}

TEST_CASE("UDP socket move construction transfers the descriptor",
          "[network_stream][socket][udp][phase3]") {
    Socket receiver;
    REQUIRE(receiver.create(SocketType::UDP));
    auto port = try_bind_udp(receiver, 45861);
    if (!port) {
        SUCCEED("could not bind UDP loopback; skipping");
        return;
    }

    Socket moved(std::move(receiver));
    REQUIRE_FALSE(receiver.is_open());
    REQUIRE(moved.is_open());

    Socket sender;
    REQUIRE(sender.create(SocketType::UDP));
    const std::array<std::uint8_t, 1> payload{0x42};
    REQUIRE(sender.send_to(payload.data(), payload.size(), "127.0.0.1", *port)
            == static_cast<int>(payload.size()));

    std::array<std::uint8_t, 4> buffer{};
    std::string from;
    std::uint16_t from_port = 0;
    REQUIRE(moved.receive_from(buffer.data(), buffer.size(), from, from_port) == 1);
    REQUIRE(buffer[0] == 0x42);
}

TEST_CASE("UDP socket move assignment transfers the descriptor",
          "[network_stream][socket][udp][phase3]") {
    Socket receiver;
    REQUIRE(receiver.create(SocketType::UDP));
    auto port = try_bind_udp(receiver, 45991);
    if (!port) {
        SUCCEED("could not bind UDP loopback; skipping");
        return;
    }

    Socket moved;
    REQUIRE(moved.create(SocketType::UDP));
    moved = std::move(receiver);
    REQUIRE_FALSE(receiver.is_open());
    REQUIRE(moved.is_open());

    Socket sender;
    REQUIRE(sender.create(SocketType::UDP));
    const std::array<std::uint8_t, 2> payload{9, 7};
    REQUIRE(sender.send_to(payload.data(), payload.size(), "127.0.0.1", *port)
            == static_cast<int>(payload.size()));

    std::array<std::uint8_t, 4> buffer{};
    std::string from;
    std::uint16_t from_port = 0;
    REQUIRE(moved.receive_from(buffer.data(), buffer.size(), from, from_port)
            == static_cast<int>(payload.size()));
    REQUIRE(buffer[0] == 9);
    REQUIRE(buffer[1] == 7);
}

TEST_CASE("UDP socket self move assignment preserves descriptor",
          "[network_stream][socket][udp][phase3]") {
    Socket receiver;
    REQUIRE(receiver.create(SocketType::UDP));
    auto port = try_bind_udp(receiver, 46121);
    if (!port) {
        SUCCEED("could not bind UDP loopback; skipping");
        return;
    }

    auto* same = &receiver;
    receiver = std::move(*same);
    REQUIRE(receiver.is_open());

    Socket sender;
    REQUIRE(sender.create(SocketType::UDP));
    const std::array<std::uint8_t, 1> payload{0x7f};
    REQUIRE(sender.send_to(payload.data(), payload.size(), "127.0.0.1", *port)
            == static_cast<int>(payload.size()));

    std::array<std::uint8_t, 4> buffer{};
    std::string from;
    std::uint16_t from_port = 0;
    REQUIRE(receiver.receive_from(buffer.data(), buffer.size(), from, from_port) == 1);
    REQUIRE(buffer[0] == 0x7f);
}

TEST_CASE("UDP socket rejects TCP-only listen and accept",
          "[network_stream][socket][udp][phase3]") {
    Socket socket;
    REQUIRE(socket.create(SocketType::UDP));
    REQUIRE_FALSE(socket.listen());
    REQUIRE_FALSE(socket.accept().has_value());
}

TEST_CASE("TcpStream can wrap an accepted Socket",
          "[network_stream][tcp][socket][phase3]") {
    Socket listener;
    REQUIRE(listener.create(SocketType::TCP));

    std::uint16_t port = 0;
    for (std::uint16_t candidate = 46251; candidate < 46330; ++candidate) {
        if (auto bound = try_bind_loopback(listener, candidate)) {
            port = *bound;
            break;
        }
    }
    if (port == 0) {
        SUCCEED("could not bind loopback port; skipping");
        return;
    }

    std::atomic<bool> ready{false};
    std::atomic<bool> done{false};
    std::thread server_thread([&] {
        ready.store(true);
        auto accepted = listener.accept();
        if (!accepted) return;
        TcpStream stream(std::move(*accepted));
        std::array<std::uint8_t, 8> buffer{};
        auto read = stream.read(buffer.data(), buffer.size());
        if (read.ok()) {
            const std::array<std::uint8_t, 2> reply{'o', 'k'};
            auto written = stream.write(reply.data(), reply.size());
            done.store(written.ok());
        }
        stream.close();
    });
    ThreadJoiner join_server{server_thread};
    while (!ready.load()) std::this_thread::sleep_for(1ms);

    bool client_ok = true;
    Socket client;
    client_ok = client.create(SocketType::TCP);
    CHECK(client_ok);
    if (client_ok) {
        client_ok = client.connect("127.0.0.1", port);
        CHECK(client_ok);
    }
    if (client_ok) {
        client_ok = client.send("hello") == 5;
        CHECK(client_ok);
    }
    std::array<std::uint8_t, 4> reply{};
    if (client_ok) {
        client_ok = client.receive(reply.data(), reply.size()) == 2;
        CHECK(client_ok);
    }
    if (client_ok) {
        CHECK(reply[0] == 'o');
        CHECK(reply[1] == 'k');
    }
    client.close();

    join_server.join();
    REQUIRE(client_ok);
    REQUIRE(done.load());
}

TEST_CASE("TcpStream zero-byte I/O succeeds while connected",
          "[network_stream][tcp][phase3]") {
    Socket listener;
    REQUIRE(listener.create(SocketType::TCP));

    std::uint16_t port = 0;
    for (std::uint16_t candidate = 46331; candidate < 46410; ++candidate) {
        if (auto bound = try_bind_loopback(listener, candidate)) {
            port = *bound;
            break;
        }
    }
    if (port == 0) {
        SUCCEED("could not bind loopback port; skipping");
        return;
    }

    std::atomic<bool> ready{false};
    std::thread server_thread([&] {
        ready.store(true);
        auto accepted = listener.accept();
        if (accepted) {
            std::this_thread::sleep_for(100ms);
            accepted->close();
        }
    });
    ThreadJoiner join_server{server_thread};
    while (!ready.load()) std::this_thread::sleep_for(1ms);

    TcpStream stream;
    bool client_ok = stream.connect("127.0.0.1", port);
    CHECK(client_ok);
    std::array<std::uint8_t, 1> byte{0xaa};
    if (client_ok) {
        auto zero_read = stream.read(byte.data(), 0);
        auto zero_write = stream.write(byte.data(), 0);
        CHECK(zero_read.ok());
        CHECK(zero_read.bytes == 0);
        CHECK(zero_write.ok());
        CHECK(zero_write.bytes == 0);
        CHECK(byte[0] == 0xaa);
    }

    stream.close();
    join_server.join();
    REQUIRE(client_ok);
}

TEST_CASE("TcpStream rejects null buffers while connected",
          "[network_stream][tcp][coverage][phase3]") {
    Socket listener;
    REQUIRE(listener.create(SocketType::TCP));

    auto bound_port = try_bind_loopback_ephemeral(listener);
    REQUIRE(bound_port);

    std::atomic<bool> ready{false};
    std::thread server_thread([&] {
        ready.store(true);
        auto accepted = listener.accept();
        if (accepted) {
            std::this_thread::sleep_for(100ms);
            accepted->close();
        }
    });
    ThreadJoiner join_server{server_thread};
    while (!ready.load()) std::this_thread::sleep_for(1ms);

    TcpStream stream;
    REQUIRE(stream.connect("127.0.0.1", *bound_port));

    auto null_read = stream.read(nullptr, 1);
    REQUIRE_FALSE(null_read.ok());
    REQUIRE(null_read.error == StreamError::Invalid);

    auto null_write = stream.write(nullptr, 1);
    REQUIRE_FALSE(null_write.ok());
    REQUIRE(null_write.error == StreamError::Invalid);

    stream.close();
    join_server.join();
}

// ── Socket edge cases ───────────────────────────────────────────────────

TEST_CASE("Socket UDP loopback send_to receive_from reports peer address",
          "[network_stream][socket][coverage][phase3]") {
    Socket receiver;
    REQUIRE(receiver.create(SocketType::UDP));
    auto port = try_bind_udp_ephemeral(receiver);
    REQUIRE(port);

    Socket sender;
    REQUIRE(sender.create(SocketType::UDP));
    const std::array<std::uint8_t, 4> payload{'p', 'u', 'l', 'p'};
    REQUIRE(sender.send_to(payload.data(), payload.size(), "127.0.0.1", *port) ==
            static_cast<int>(payload.size()));

    std::array<std::uint8_t, 8> buffer{};
    std::string from_address;
    std::uint16_t from_port = 0;
    const int received = receiver.receive_from(buffer.data(), buffer.size(),
                                               from_address, from_port);
    REQUIRE(received == static_cast<int>(payload.size()));
    REQUIRE(std::equal(payload.begin(), payload.end(), buffer.begin()));
    REQUIRE(from_address == "127.0.0.1");
    REQUIRE(from_port != 0);
}

TEST_CASE("Socket UDP send_to and receive_from fail before create",
          "[network_stream][socket][coverage][phase3]") {
    Socket socket;
    std::array<std::uint8_t, 4> buffer{};
    std::string from_address = "unchanged";
    std::uint16_t from_port = 7777;

    REQUIRE(socket.send_to(buffer.data(), buffer.size(), "127.0.0.1", 9) == -1);
    REQUIRE(socket.receive_from(buffer.data(), buffer.size(), from_address, from_port) == -1);
    REQUIRE(from_address == "unchanged");
    REQUIRE(from_port == 7777);
}

TEST_CASE("Socket local_port reports bound UDP ephemeral port",
          "[network_stream][socket][coverage][phase3]") {
    Socket socket;
    REQUIRE(socket.local_port() == 0);
    REQUIRE(socket.create(SocketType::UDP));
    REQUIRE(socket.local_port() == 0);
    REQUIRE(socket.bind("127.0.0.1", 0));
    REQUIRE(socket.local_port() != 0);
}

TEST_CASE("Socket TCP loopback send receive and close",
          "[network_stream][socket][coverage][phase3]") {
    Socket listener;
    REQUIRE(listener.create(SocketType::TCP));

    auto bound_port = try_bind_loopback_ephemeral(listener);
    REQUIRE(bound_port);
    const auto port = *bound_port;

    std::atomic<bool> server_ready{false};
    std::atomic<bool> server_done{false};
    std::thread server_thread([&] {
        server_ready.store(true);
        auto accepted = listener.accept();
        if (!accepted) return;

        std::array<std::uint8_t, 8> buffer{};
        const int received = accepted->receive(buffer.data(), buffer.size());
        if (received > 0) {
            accepted->send(buffer.data(), static_cast<std::size_t>(received));
        }
        accepted->close();
        server_done.store(true);
    });
    ThreadJoiner join_server{server_thread};

    while (!server_ready.load()) std::this_thread::sleep_for(1ms);

    Socket client;
    REQUIRE(client.create(SocketType::TCP));
    REQUIRE(client.connect("127.0.0.1", port));

    const std::array<std::uint8_t, 4> payload{'t', 'c', 'p', '!'};
    REQUIRE(client.send(payload.data(), payload.size()) == static_cast<int>(payload.size()));

    std::array<std::uint8_t, 8> echo{};
    REQUIRE(client.receive(echo.data(), echo.size()) == static_cast<int>(payload.size()));
    REQUIRE(std::equal(payload.begin(), payload.end(), echo.begin()));

    client.close();
    join_server.join();
    REQUIRE(server_done.load());
}

TEST_CASE("Socket move construction transfers open UDP handle",
          "[network_stream][socket][coverage][phase3]") {
    Socket original;
    REQUIRE(original.create(SocketType::UDP));
    REQUIRE(original.is_open());

    Socket moved(std::move(original));
    REQUIRE_FALSE(original.is_open());
    REQUIRE(moved.is_open());
    REQUIRE(moved.bind("127.0.0.1", 0));
    moved.close();
    REQUIRE_FALSE(moved.is_open());
}

TEST_CASE("Socket move assignment closes old handle and adopts new one",
          "[network_stream][socket][coverage][phase3]") {
    Socket old_socket;
    REQUIRE(old_socket.create(SocketType::UDP));
    REQUIRE(old_socket.bind("127.0.0.1", 0));

    Socket replacement;
    REQUIRE(replacement.create(SocketType::UDP));

    old_socket = std::move(replacement);
    REQUIRE(old_socket.is_open());
    REQUIRE_FALSE(replacement.is_open());
    old_socket.close();
    REQUIRE_FALSE(old_socket.is_open());
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

TEST_CASE("HttpStream rejects null read buffers before transport errors",
          "[network_stream][http][coverage][phase3]") {
    HttpStream::Request req;
    req.url = "ftp://127.0.0.1/nope";
    req.timeout_seconds = 1;
    HttpStream stream(req);

    auto null_read = stream.read(nullptr, 1);
    REQUIRE_FALSE(null_read.ok());
    REQUIRE(null_read.error == StreamError::Invalid);

    stream.close();
    REQUIRE(stream.read(nullptr, 1).closed());
}

TEST_CASE("HttpStream factories and refetch reset closed state to request results",
          "[network_stream][http][codecov]") {
    auto get_stream = HttpStream::get("http://", 1);
    REQUIRE(get_stream);
    REQUIRE_FALSE(get_stream->is_open());
    REQUIRE(get_stream->status_code() == 0);
    REQUIRE(get_stream->transport_error() == "Invalid URL");

    auto post_stream = HttpStream::post("ftp://127.0.0.1/post", "{}", "application/json", 1);
    REQUIRE(post_stream);
    REQUIRE_FALSE(post_stream->is_open());
    REQUIRE(post_stream->status_code() == 0);
    REQUIRE(post_stream->transport_error() == "Invalid URL");

    HttpStream stream;
    stream.close();
    HttpStream::Request req;
    req.url = "http://";
    req.timeout_seconds = 1;
    REQUIRE_FALSE(stream.fetch(req));
    REQUIRE_FALSE(stream.is_open());
    REQUIRE(stream.transport_error() == "Invalid URL");

    std::uint8_t byte = 0;
    auto read = stream.read(&byte, sizeof(byte));
    REQUIRE_FALSE(read.ok());
    REQUIRE(read.error == StreamError::IoError);
}

TEST_CASE("HttpStream reads successful GET responses in chunks",
          "[network_stream][http][coverage][phase3]") {
    httplib::Server server;
    server.Get("/__ready", [](const httplib::Request&, httplib::Response& response) {
        response.status = 204;
    });
    server.Get("/body", [](const httplib::Request&, httplib::Response& response) {
        response.set_header("X-Pulp-Stream", "ok");
        response.set_content("chunked-body", "text/plain");
    });

    const int port = server.bind_to_any_port("127.0.0.1");
    REQUIRE(port > 0);
    HttpServerRunner runner(server);
    REQUIRE(runner.wait_until_running());
    REQUIRE(wait_until_http_ready(port));

    auto stream = HttpStream::get("http://127.0.0.1:" + std::to_string(port) + "/body", 2);
    REQUIRE(stream);
    INFO(stream->transport_error());
    REQUIRE(stream->status_code() == 200);
    REQUIRE(stream->headers().at("X-Pulp-Stream") == "ok");
    REQUIRE(stream->is_open());
    REQUIRE_FALSE(stream->eof());

    std::array<std::uint8_t, 16> buffer{};
    auto first = stream->read(buffer.data(), 7);
    REQUIRE(first.ok());
    REQUIRE(first.bytes == 7);
    REQUIRE(std::string(reinterpret_cast<char*>(buffer.data()), first.bytes) == "chunked");
    REQUIRE(stream->is_open());

    auto second = stream->read(buffer.data(), buffer.size());
    REQUIRE(second.ok());
    REQUIRE(second.bytes == 5);
    REQUIRE(std::string(reinterpret_cast<char*>(buffer.data()), second.bytes) == "-body");
    REQUIRE_FALSE(stream->is_open());
    REQUIRE(stream->eof());

    auto eof = stream->read(buffer.data(), buffer.size());
    REQUIRE_FALSE(eof.ok());
    REQUIRE(eof.closed());
}

TEST_CASE("HttpStream POST factory exposes successful response bodies",
          "[network_stream][http][coverage][phase3]") {
    httplib::Server server;
    server.Get("/__ready", [](const httplib::Request&, httplib::Response& response) {
        response.status = 204;
    });
    server.Post("/echo", [](const httplib::Request& request, httplib::Response& response) {
        response.set_header("X-Pulp-Method", request.method);
        response.set_content(request.body, "text/plain");
    });

    const int port = server.bind_to_any_port("127.0.0.1");
    REQUIRE(port > 0);
    HttpServerRunner runner(server);
    REQUIRE(runner.wait_until_running());
    REQUIRE(wait_until_http_ready(port));

    auto stream = HttpStream::post("http://127.0.0.1:" + std::to_string(port) + "/echo",
                                   "payload=42",
                                   "text/plain",
                                   2);
    REQUIRE(stream);
    INFO(stream->transport_error());
    REQUIRE(stream->status_code() == 200);
    REQUIRE(stream->headers().at("X-Pulp-Method") == "POST");
    REQUIRE(stream->is_open());

    std::array<std::uint8_t, 32> buffer{};
    auto read = stream->read(buffer.data(), buffer.size());
    REQUIRE(read.ok());
    REQUIRE(read.bytes == 10);
    REQUIRE(std::string(reinterpret_cast<char*>(buffer.data()), read.bytes) == "payload=42");
    REQUIRE(stream->read(buffer.data(), buffer.size()).closed());
}
