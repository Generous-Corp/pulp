#include <catch2/catch_test_macros.hpp>
#include <pulp/events/child_process_manager.hpp>
#include <pulp/events/interprocess_connection.hpp>
#include <pulp/runtime/temporary_file.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

using namespace pulp::events;
using namespace pulp::runtime;

namespace {

std::optional<uint16_t> start_socket_server_on_loopback(InterprocessConnectionServer& server) {
    const auto seed = static_cast<uint16_t>(
        std::chrono::steady_clock::now().time_since_epoch().count() % 20000);
    for (uint16_t i = 0; i < 200; ++i) {
        const uint16_t port = static_cast<uint16_t>(20000 + ((seed + i) % 20000));
        if (server.start("127.0.0.1:" + std::to_string(port), IpcTransport::Socket)) {
            return port;
        }
    }
    return std::nullopt;
}

struct CapturingServer : InterprocessConnectionServer {
    void client_connected(std::unique_ptr<InterprocessConnection> conn) override {
        conn->on_message = [this](const void*, size_t size) {
            std::lock_guard<std::mutex> lock(mutex);
            ++binary_messages;
            last_binary_size = size;
            cv.notify_all();
        };
        conn->on_text_message = [this](std::string_view message) {
            std::lock_guard<std::mutex> lock(mutex);
            ++text_messages;
            last_text.assign(message);
            cv.notify_all();
        };
        conn->on_disconnected = [this] {
            std::lock_guard<std::mutex> lock(mutex);
            ++disconnects;
            cv.notify_all();
        };

        std::lock_guard<std::mutex> lock(mutex);
        accepted = std::move(conn);
        cv.notify_all();
    }

    std::mutex mutex;
    std::condition_variable cv;
    std::unique_ptr<InterprocessConnection> accepted;
    int binary_messages = 0;
    int text_messages = 0;
    int disconnects = 0;
    size_t last_binary_size = 1;
    std::string last_text = "unset";
};

}  // namespace

// ── InterprocessConnection via named pipe ───────────────────────────────

TEST_CASE("IPC message framing", "[events][ipc]") {
    // Verify the connection won't send when disconnected
    InterprocessConnection conn;
    REQUIRE_FALSE(conn.send_message("test message"));
    REQUIRE_FALSE(conn.send_message(std::string_view("binary data")));
}

TEST_CASE("IPC connection state", "[events][ipc]") {
    InterprocessConnection conn;
    REQUIRE(conn.state() == IpcState::Disconnected);
    REQUIRE_FALSE(conn.is_connected());
}

TEST_CASE("IPC connect to nonexistent fails", "[events][ipc]") {
    InterprocessConnection conn;
    bool ok = conn.connect("/tmp/pulp_nonexistent_pipe_12345", IpcTransport::NamedPipe);
    REQUIRE_FALSE(ok);
    REQUIRE(conn.state() == IpcState::Error);
}

TEST_CASE("IPC socket connect rejects malformed endpoints", "[events][ipc]") {
    InterprocessConnection conn;
    REQUIRE_FALSE(conn.connect("127.0.0.1", IpcTransport::Socket));
    REQUIRE(conn.state() == IpcState::Error);

    REQUIRE_FALSE(conn.connect("127.0.0.1:not-a-port", IpcTransport::Socket));
    REQUIRE(conn.state() == IpcState::Error);

    REQUIRE_FALSE(conn.connect("127.0.0.1:70000", IpcTransport::Socket));
    REQUIRE(conn.state() == IpcState::Error);
    REQUIRE_FALSE(conn.is_connected());
}

TEST_CASE("IPC send while disconnected returns false", "[events][ipc]") {
    InterprocessConnection conn;
    REQUIRE_FALSE(conn.send_message("test"));
}

TEST_CASE("IPC lambda callbacks settable", "[events][ipc]") {
    InterprocessConnection conn;
    bool connected_fired = false;
    bool disconnected_fired = false;

    conn.on_connected = [&]() { connected_fired = true; };
    conn.on_disconnected = [&]() { disconnected_fired = true; };
    conn.on_text_message = [](std::string_view) {};

    // Callbacks are set but won't fire without actual connection
    REQUIRE_FALSE(connected_fired);
}

// ── InterprocessConnectionServer ────────────────────────────────────────

TEST_CASE("IPC server initial state", "[events][ipc]") {
    InterprocessConnectionServer server;
    REQUIRE_FALSE(server.is_running());
}

// ── Child process manager ───────────────────────────────────────────────

TEST_CASE("ConnectedChildProcess default state is safe to tear down",
          "[events][child-process][issue-642]") {
    ConnectedChildProcess child;

    REQUIRE_FALSE(child.is_running());
    REQUIRE(child.pid() == -1);
    REQUIRE_FALSE(child.send_message("not launched"));

    child.kill();
    REQUIRE(child.wait_for_exit(1) == -1);
}

TEST_CASE("ChildProcessManager empty lifecycle operations are no-ops",
          "[events][child-process][issue-642]") {
    ChildProcessManager manager;
    int exit_callbacks = 0;
    manager.on_child_exit = [&](ConnectedChildProcess*, int) { ++exit_callbacks; };

    REQUIRE(manager.active_count() == 0);
    manager.wait_all(1);
    manager.kill_all();
    manager.cleanup();

    REQUIRE(manager.active_count() == 0);
    REQUIRE(exit_callbacks == 0);
}

TEST_CASE("IPC socket server rejects malformed listen endpoints",
          "[events][ipc][socket]") {
    InterprocessConnectionServer server;
    REQUIRE_FALSE(server.start("", IpcTransport::Socket));
    REQUIRE_FALSE(server.is_running());

    REQUIRE_FALSE(server.start("127.0.0.1:", IpcTransport::Socket));
    REQUIRE_FALSE(server.is_running());

    REQUIRE_FALSE(server.start("127.0.0.1:not-a-port", IpcTransport::Socket));
    REQUIRE_FALSE(server.is_running());

    REQUIRE_FALSE(server.start("70000", IpcTransport::Socket));
    REQUIRE_FALSE(server.is_running());

    InterprocessConnection conn;
    REQUIRE_FALSE(conn.connect("127.0.0.1:", IpcTransport::Socket));
    REQUIRE(conn.state() == IpcState::Error);

    REQUIRE_FALSE(conn.create_server("127.0.0.1:not-a-port", IpcTransport::Socket));
    REQUIRE(conn.state() == IpcState::Error);

    REQUIRE_FALSE(conn.create_server("", IpcTransport::Socket));
    REQUIRE(conn.state() == IpcState::Error);
}

TEST_CASE("IPC socket server stops while waiting for a client",
          "[events][ipc][socket]") {
    InterprocessConnectionServer server;
    auto port = start_socket_server_on_loopback(server);
    REQUIRE(port.has_value());
    REQUIRE(server.is_running());

    server.stop();
    REQUIRE_FALSE(server.is_running());
}

TEST_CASE("IPC socket server accepts client and exchanges framed messages",
          "[events][ipc][socket]") {
    InterprocessConnectionServer server;

    std::mutex mutex;
    std::condition_variable cv;
    std::unique_ptr<InterprocessConnection> accepted;
    bool accepted_connected = false;
    bool server_received = false;
    bool client_connected = false;
    bool client_received = false;
    std::string server_text;
    std::string client_text;

    server.on_client_connected = [&](std::unique_ptr<InterprocessConnection> conn) {
        conn->on_text_message = [&](std::string_view message) {
            std::lock_guard<std::mutex> lock(mutex);
            server_text.assign(message);
            server_received = true;
            cv.notify_all();
        };

        std::lock_guard<std::mutex> lock(mutex);
        accepted = std::move(conn);
        accepted_connected = true;
        cv.notify_all();
    };

    auto port = start_socket_server_on_loopback(server);
    REQUIRE(port.has_value());
    REQUIRE(server.is_running());

    InterprocessConnection client;
    client.on_connected = [&] {
        std::lock_guard<std::mutex> lock(mutex);
        client_connected = true;
        cv.notify_all();
    };
    client.on_text_message = [&](std::string_view message) {
        std::lock_guard<std::mutex> lock(mutex);
        client_text.assign(message);
        client_received = true;
        cv.notify_all();
    };

    REQUIRE(client.connect("127.0.0.1:" + std::to_string(*port), IpcTransport::Socket));

    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return client_connected && accepted_connected;
        }));
    }

    REQUIRE(client.send_message("client-to-server"));

    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return server_received;
        }));
        REQUIRE(server_text == "client-to-server");
    }

    {
        std::lock_guard<std::mutex> lock(mutex);
        REQUIRE(accepted != nullptr);
        REQUIRE(accepted->send_message("server-to-client"));
    }

    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return client_received;
        }));
        REQUIRE(client_text == "server-to-client");
    }

    client.disconnect();
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (accepted) accepted->disconnect();
    }
    server.stop();
    REQUIRE_FALSE(server.is_running());
}

TEST_CASE("IPC socket server virtual callback accepts empty frames",
          "[events][ipc][socket][issue-642]") {
    CapturingServer server;
    auto port = start_socket_server_on_loopback(server);
    REQUIRE(port.has_value());

    InterprocessConnection client;
    REQUIRE(client.connect("127.0.0.1:" + std::to_string(*port), IpcTransport::Socket));

    {
        std::unique_lock<std::mutex> lock(server.mutex);
        REQUIRE(server.cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return server.accepted != nullptr;
        }));
    }

    REQUIRE(client.send_message(std::string_view{}));

    {
        std::unique_lock<std::mutex> lock(server.mutex);
        REQUIRE(server.cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return server.binary_messages == 1 && server.text_messages == 1;
        }));
        REQUIRE(server.last_binary_size == 0);
        REQUIRE(server.last_text.empty());
    }

    client.disconnect();
    if (server.accepted) server.accepted->disconnect();
    server.stop();
    REQUIRE_FALSE(server.is_running());
}

TEST_CASE("IPC socket server receives binary payload frames",
          "[events][ipc][socket][codecov]") {
    CapturingServer server;
    auto port = start_socket_server_on_loopback(server);
    REQUIRE(port.has_value());

    InterprocessConnection client;
    REQUIRE(client.connect("127.0.0.1:" + std::to_string(*port), IpcTransport::Socket));

    {
        std::unique_lock<std::mutex> lock(server.mutex);
        REQUIRE(server.cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return server.accepted != nullptr;
        }));
    }

    const std::vector<uint8_t> payload{0x00, 0x01, 0x7f, 0xff};
    REQUIRE(client.send_message(payload.data(), payload.size()));

    {
        std::unique_lock<std::mutex> lock(server.mutex);
        REQUIRE(server.cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return server.binary_messages == 1 && server.text_messages == 1;
        }));
        REQUIRE(server.last_binary_size == payload.size());
        REQUIRE(server.last_text.size() == payload.size());
    }

    client.disconnect();
    if (server.accepted) server.accepted->disconnect();
    server.stop();
    REQUIRE_FALSE(server.is_running());
}

TEST_CASE("IPC socket server observes client disconnect",
          "[events][ipc][socket][codecov]") {
    CapturingServer server;
    auto port = start_socket_server_on_loopback(server);
    REQUIRE(port.has_value());

    InterprocessConnection client;
    REQUIRE(client.connect("127.0.0.1:" + std::to_string(*port), IpcTransport::Socket));

    {
        std::unique_lock<std::mutex> lock(server.mutex);
        REQUIRE(server.cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return server.accepted != nullptr;
        }));
    }

    client.disconnect();
    {
        std::unique_lock<std::mutex> lock(server.mutex);
        REQUIRE(server.cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return server.disconnects == 1;
        }));
    }

    if (server.accepted) server.accepted->disconnect();
    server.stop();
    REQUIRE_FALSE(server.is_running());
}

TEST_CASE("IPC socket client reports disconnect callback once",
          "[events][ipc][socket][codecov]") {
    CapturingServer server;
    auto port = start_socket_server_on_loopback(server);
    REQUIRE(port.has_value());

    int disconnected = 0;
    InterprocessConnection client;
    client.on_disconnected = [&] { ++disconnected; };
    REQUIRE(client.connect("127.0.0.1:" + std::to_string(*port), IpcTransport::Socket));

    {
        std::unique_lock<std::mutex> lock(server.mutex);
        REQUIRE(server.cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return server.accepted != nullptr;
        }));
    }

    client.disconnect();
    client.disconnect();
    REQUIRE(disconnected == 1);
    REQUIRE_FALSE(client.is_connected());
    REQUIRE(client.state() == IpcState::Disconnected);

    if (server.accepted) server.accepted->disconnect();
    server.stop();
    REQUIRE_FALSE(server.is_running());
}
