#include <catch2/catch_test_macros.hpp>
#include <pulp/events/interprocess_connection.hpp>

#include <string_view>
#include <vector>

using namespace pulp::events;

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
