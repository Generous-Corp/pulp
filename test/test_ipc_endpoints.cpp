#include <catch2/catch_test_macros.hpp>
#include <pulp/events/interprocess_connection.hpp>

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
