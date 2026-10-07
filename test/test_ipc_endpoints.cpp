#include <catch2/catch_test_macros.hpp>
#include <pulp/events/interprocess_connection.hpp>
#include <pulp/runtime/socket.hpp>

#include <chrono>
#include <future>
#include <string>
#include <string_view>
#include <vector>

using namespace pulp::events;
using namespace pulp::runtime;
using namespace std::chrono_literals;

TEST_CASE("IPC socket endpoints reject empty ports without opening connections",
          "[events][ipc][issue-642]") {
    InterprocessConnection client;
    REQUIRE_FALSE(client.connect("127.0.0.1:", IpcTransport::Socket));
    REQUIRE(client.state() == IpcState::Error);
    REQUIRE_FALSE(client.is_connected());

    client.disconnect();
    REQUIRE(client.state() == IpcState::Disconnected);

    InterprocessConnection server_connection;
    REQUIRE_FALSE(server_connection.create_server("127.0.0.1:", IpcTransport::Socket));
    REQUIRE(server_connection.state() == IpcState::Error);
    REQUIRE_FALSE(server_connection.is_connected());
}

TEST_CASE("IPC socket listener rejects empty endpoint strings",
          "[events][ipc][issue-642]") {
    InterprocessConnectionServer server;

    REQUIRE_FALSE(server.start("", IpcTransport::Socket));
    REQUIRE_FALSE(server.is_running());

    REQUIRE_FALSE(server.start("127.0.0.1:", IpcTransport::Socket));
    REQUIRE_FALSE(server.is_running());

    server.stop();
    REQUIRE_FALSE(server.is_running());
}

TEST_CASE("IPC socket client rejects malformed port strings without connecting",
          "[events][ipc][endpoint][issue-642]") {
    const std::vector<std::string_view> endpoints = {
        "127.0.0.1:not-a-port",
        "127.0.0.1:12x",
        "127.0.0.1:+12",
        "127.0.0.1:-1",
        "127.0.0.1: 12",
        "127.0.0.1:12 ",
        "127.0.0.1:65536",
        "127.0.0.1:999999999999",
    };

    for (auto endpoint : endpoints) {
        InterprocessConnection client;
        REQUIRE_FALSE(client.connect(endpoint, IpcTransport::Socket));
        REQUIRE(client.state() == IpcState::Error);
        REQUIRE_FALSE(client.is_connected());
    }
}

TEST_CASE("IPC socket connect timeout bounds a saturated listener",
          "[events][ipc][timeout]") {
    Socket listener;
    REQUIRE(listener.create(SocketType::TCP));
    REQUIRE(listener.bind("127.0.0.1", 0));
    REQUIRE(listener.listen(1));
    const auto port = listener.local_port();
    REQUIRE(port != 0);

    // Leave accepted connections queued until the kernel refuses to complete
    // another handshake. Timed connects keep this setup bounded even on hosts
    // whose effective listen backlog is larger than the requested one.
    std::vector<Socket> queued;
    bool saturated = false;
    for (int attempt = 0; attempt < 512; ++attempt) {
        Socket client;
        REQUIRE(client.create(SocketType::TCP));
        if (!client.connect("127.0.0.1", port, 10ms)) {
            saturated = true;
            break;
        }
        queued.push_back(std::move(client));
    }
    REQUIRE(saturated);

    InterprocessConnection client;
    const auto started = std::chrono::steady_clock::now();
    CHECK_FALSE(client.connect(
        "127.0.0.1:" + std::to_string(port),
        IpcTransport::Socket, 50ms));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed < 1s);
}

TEST_CASE("IPC socket connection-server rejects malformed port strings",
          "[events][ipc][endpoint][issue-642]") {
    const std::vector<std::string_view> endpoints = {
        "127.0.0.1:not-a-port",
        "127.0.0.1:12x",
        "127.0.0.1:+12",
        "127.0.0.1:-1",
        "127.0.0.1: 12",
        "127.0.0.1:12 ",
        "127.0.0.1:65536",
        "127.0.0.1:999999999999",
    };

    for (auto endpoint : endpoints) {
        InterprocessConnection connection;
        REQUIRE_FALSE(connection.create_server(endpoint, IpcTransport::Socket));
        REQUIRE(connection.state() == IpcState::Error);
        REQUIRE_FALSE(connection.is_connected());
    }
}

TEST_CASE("IPC socket listener rejects malformed port strings without staying running",
          "[events][ipc][endpoint][issue-642]") {
    const std::vector<std::string_view> endpoints = {
        "not-a-port",
        "12x",
        "+12",
        "-1",
        " 12",
        "12 ",
        "127.0.0.1:not-a-port",
        "127.0.0.1:12x",
        "127.0.0.1:+12",
        "127.0.0.1:-1",
        "127.0.0.1: 12",
        "127.0.0.1:12 ",
        "127.0.0.1:65536",
        "127.0.0.1:999999999999",
    };

    for (auto endpoint : endpoints) {
        InterprocessConnectionServer server;
        REQUIRE_FALSE(server.start(endpoint, IpcTransport::Socket));
        REQUIRE_FALSE(server.is_running());
        server.stop();
        REQUIRE_FALSE(server.is_running());
    }
}

TEST_CASE("IPC socket endpoint failure can be reset with disconnect",
          "[events][ipc][endpoint][issue-642]") {
    InterprocessConnection connection;

    REQUIRE_FALSE(connection.connect("127.0.0.1:bad", IpcTransport::Socket));
    REQUIRE(connection.state() == IpcState::Error);

    connection.disconnect();
    REQUIRE(connection.state() == IpcState::Disconnected);
    REQUIRE_FALSE(connection.is_connected());

    REQUIRE_FALSE(connection.create_server("127.0.0.1:bad", IpcTransport::Socket));
    REQUIRE(connection.state() == IpcState::Error);

    connection.disconnect();
    REQUIRE(connection.state() == IpcState::Disconnected);
}

TEST_CASE("IPC socket client rejects hostless numeric endpoints",
          "[events][ipc][endpoint]") {
    const std::vector<std::string_view> endpoints = {
        "0",
        "1",
        ":12345",
        "12345",
        "65535",
    };

    for (auto endpoint : endpoints) {
        InterprocessConnection client;
        REQUIRE_FALSE(client.connect(endpoint, IpcTransport::Socket));
        REQUIRE(client.state() == IpcState::Error);
        REQUIRE_FALSE(client.is_connected());

        client.disconnect();
        REQUIRE(client.state() == IpcState::Disconnected);
        REQUIRE_FALSE(client.is_connected());
    }
}

TEST_CASE("IPC socket disconnect returns while the peer stays connected and silent",
          "[events][ipc][socket][regression]") {
    Socket listener;
    REQUIRE(listener.create(SocketType::TCP));
    REQUIRE(listener.bind("127.0.0.1", 0));
    REQUIRE(listener.listen(1));

    InterprocessConnection client;
    REQUIRE(
        client.connect("127.0.0.1:" + std::to_string(listener.local_port()), IpcTransport::Socket));
    auto peer = listener.accept(2000ms);
    REQUIRE(peer.has_value());
    REQUIRE(client.is_connected());

    // The read thread is blocked waiting for a frame the peer never sends, and
    // the peer never closes: disconnect() must still wake it.
    auto disconnecting = std::async(std::launch::async, [&client] { client.disconnect(); });
    const bool returned = disconnecting.wait_for(2s) == std::future_status::ready;
    bool joined = returned;
    if (!returned) {
        peer->close(); // release the reader so the future can be joined
        joined = disconnecting.wait_for(10s) == std::future_status::ready;
    }

    REQUIRE(returned);
    REQUIRE(joined);
    REQUIRE_FALSE(client.is_connected());
}
