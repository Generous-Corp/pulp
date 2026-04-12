#include <catch2/catch_test_macros.hpp>

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
