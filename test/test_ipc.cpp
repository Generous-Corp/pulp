#include <catch2/catch_test_macros.hpp>
#include <pulp/events/child_process_manager.hpp>
#include <pulp/events/interprocess_connection.hpp>
#include <pulp/platform/child_process.hpp>
#include <pulp/runtime/temporary_file.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace pulp::events;
using namespace pulp::runtime;

namespace {

uint16_t socket_port_seed() {
    const auto now = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#ifdef _WIN32
    const auto pid = static_cast<uint64_t>(_getpid());
#else
    const auto pid = static_cast<uint64_t>(getpid());
#endif
    return static_cast<uint16_t>((now + (pid * 131u)) % 20000u);
}

std::optional<uint16_t> start_socket_server_on_loopback(InterprocessConnectionServer& server) {
    const auto seed = socket_port_seed();
    for (uint16_t i = 0; i < 200; ++i) {
        const uint16_t port = static_cast<uint16_t>(20000 + ((seed + i) % 20000));
        if (server.start("127.0.0.1:" + std::to_string(port), IpcTransport::Socket)) {
            return port;
        }
    }
    return std::nullopt;
}

std::optional<uint16_t> start_socket_server_on_any_interface(InterprocessConnectionServer& server) {
    const auto seed = socket_port_seed();
    for (uint16_t i = 0; i < 200; ++i) {
        const uint16_t port = static_cast<uint16_t>(20000 + ((seed + i) % 20000));
        if (server.start(std::to_string(port), IpcTransport::Socket)) {
            return port;
        }
    }
    return std::nullopt;
}

std::string unique_pipe_name(std::string_view stem) {
    const auto now = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#ifdef _WIN32
    const auto pid = static_cast<uint64_t>(_getpid());
    return std::string(stem) + "_" + std::to_string(pid) + "_" +
           std::to_string(now);
#else
    const auto pid = static_cast<uint64_t>(getpid());
    return (std::filesystem::temp_directory_path() /
            (std::string(stem) + "_" + std::to_string(pid) + "_" +
             std::to_string(now)))
        .string();
#endif
}

void wait_for_named_pipe_server_ready(const std::string& pipe_name,
                                      const std::atomic<bool>& server_started) {
    while (!server_started.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
#ifdef _WIN32
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
#else
    const auto reply = std::filesystem::path(pipe_name + ".reply");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((!std::filesystem::exists(pipe_name) || !std::filesystem::exists(reply)) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
#endif
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

std::string connected_child_fixture_path() {
#ifdef PULP_TEST_CONNECTED_CHILD_FIXTURE
    return PULP_TEST_CONNECTED_CHILD_FIXTURE;
#else
    return {};
#endif
}

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

// "IPC socket connect rejects malformed endpoints" lived here before
// companion-track U-8. Its coverage (host-only, non-numeric port,
// out-of-range port) is fully subsumed by the more thorough table
// in test_ipc_endpoints.cpp ("IPC socket client rejects malformed
// port strings without connecting"). Kept here as a pointer so a
// future test author doesn't recreate the duplicate.

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

TEST_CASE("IPC named pipe exchanges framed messages without self-consuming",
          "[events][ipc][named-pipe]") {
    const auto pipe_name = unique_pipe_name("pulp_ipc_named_pipe_exchange");
    InterprocessConnection server;
    InterprocessConnection client;

    std::mutex mutex;
    std::condition_variable cv;
    bool server_ok = false;
    bool server_done = false;
    std::string server_text;
    std::string client_text;

    server.on_text_message = [&](std::string_view message) {
        std::lock_guard<std::mutex> lock(mutex);
        server_text.assign(message);
        cv.notify_all();
    };
    client.on_text_message = [&](std::string_view message) {
        std::lock_guard<std::mutex> lock(mutex);
        client_text.assign(message);
        cv.notify_all();
    };

    std::atomic<bool> server_started{false};
    std::thread server_thread([&] {
        server_started.store(true);
        const bool ok = server.create_server(pipe_name, IpcTransport::NamedPipe);
        {
            std::lock_guard<std::mutex> lock(mutex);
            server_ok = ok;
            server_done = true;
            cv.notify_all();
        }
    });

    wait_for_named_pipe_server_ready(pipe_name, server_started);

    const bool client_ok = client.connect(pipe_name, IpcTransport::NamedPipe);
    if (!client_ok) {
        server.disconnect();
        if (server_thread.joinable())
            server_thread.join();
    }
    REQUIRE(client_ok);
    if (server_thread.joinable())
        server_thread.join();
    REQUIRE(server_ok);
    REQUIRE(server_done);

    REQUIRE(client.send_message("client-to-server"));
    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return server_text == "client-to-server";
        }));
    }

    REQUIRE(server.send_message("server-to-client"));
    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return client_text == "server-to-client";
        }));
    }

    client.disconnect();
    server.disconnect();
}

TEST_CASE("IPC named pipe observes graceful peer disconnect",
          "[events][ipc][named-pipe]") {
    const auto pipe_name = unique_pipe_name("pulp_ipc_named_pipe_disconnect");
    InterprocessConnection server;
    InterprocessConnection client;

    std::mutex mutex;
    std::condition_variable cv;
    bool server_ok = false;
    bool server_disconnected = false;

    server.on_disconnected = [&] {
        std::lock_guard<std::mutex> lock(mutex);
        server_disconnected = true;
        cv.notify_all();
    };

    std::atomic<bool> server_started{false};
    std::thread server_thread([&] {
        server_started.store(true);
        const bool ok = server.create_server(pipe_name, IpcTransport::NamedPipe);
        {
            std::lock_guard<std::mutex> lock(mutex);
            server_ok = ok;
            cv.notify_all();
        }
    });

    wait_for_named_pipe_server_ready(pipe_name, server_started);

    const bool client_ok = client.connect(pipe_name, IpcTransport::NamedPipe);
    if (!client_ok) {
        server.disconnect();
        if (server_thread.joinable())
            server_thread.join();
    }
    REQUIRE(client_ok);
    if (server_thread.joinable())
        server_thread.join();
    REQUIRE(server_ok);

    client.disconnect();
    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return server_disconnected;
        }));
    }

    server.disconnect();
}

TEST_CASE("IPC named pipe observes abrupt peer death",
          "[events][ipc][named-pipe]") {
    const auto fixture = connected_child_fixture_path();
    REQUIRE_FALSE(fixture.empty());

    const auto pipe_name = unique_pipe_name("pulp_ipc_named_pipe_abrupt");
    InterprocessConnection server;

    std::mutex mutex;
    std::condition_variable cv;
    bool server_ok = false;
    bool ready = false;
    bool disconnected = false;

    server.on_text_message = [&](std::string_view message) {
        std::lock_guard<std::mutex> lock(mutex);
        ready = (message == "ready");
        cv.notify_all();
    };
    server.on_disconnected = [&] {
        std::lock_guard<std::mutex> lock(mutex);
        disconnected = true;
        cv.notify_all();
    };

    std::atomic<bool> server_started{false};
    std::thread server_thread([&] {
        server_started.store(true);
        const bool ok = server.create_server(pipe_name, IpcTransport::NamedPipe);
        {
            std::lock_guard<std::mutex> lock(mutex);
            server_ok = ok;
            cv.notify_all();
        }
    });

    wait_for_named_pipe_server_ready(pipe_name, server_started);

    pulp::platform::ChildProcess child;
    pulp::platform::ProcessOptions options;
    options.capture_stdout = false;
    options.capture_stderr = false;
    const bool started = child.start(fixture,
                                     {"--ipc-pipe", pipe_name, "--hold-ms", "50",
                                      "--abrupt-exit"},
                                     options);
    if (!started) {
        server.disconnect();
        if (server_thread.joinable())
            server_thread.join();
    }
    REQUIRE(started);

    server_thread.join();
    REQUIRE(server_ok);

    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return ready;
        }));
    }

    const auto result = child.wait();
    REQUIRE(result.exit_code == 0);

    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return disconnected;
        }));
    }

    server.disconnect();
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

TEST_CASE("ConnectedChildProcess reports real child exit code",
          "[events][child-process][ipc]") {
    const auto fixture = connected_child_fixture_path();
    REQUIRE_FALSE(fixture.empty());

    ConnectedChildProcess child;
    std::mutex mutex;
    std::condition_variable cv;
    std::string message;
    bool exit_called = false;
    int exit_code = -1;

    child.on_message = [&](std::string_view msg) {
        std::lock_guard<std::mutex> lock(mutex);
        message.assign(msg);
        cv.notify_all();
    };
    child.on_exit = [&](int code) {
        std::lock_guard<std::mutex> lock(mutex);
        exit_called = true;
        exit_code = code;
        cv.notify_all();
    };

    REQUIRE(child.launch(fixture, {"--exit-code", "37"}));
    REQUIRE(child.pid() > 0);

    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return message == "ready";
        }));
    }

    REQUIRE(child.wait_for_exit(5000) == 37);
    REQUIRE_FALSE(child.is_running());

    std::lock_guard<std::mutex> lock(mutex);
    REQUIRE(exit_called);
    REQUIRE(exit_code == 37);
}

TEST_CASE("ConnectedChildProcess launch failure tears down pending IPC server",
          "[events][child-process][ipc]") {
    ConnectedChildProcess child;
    REQUIRE_FALSE(child.launch("/pulp/definitely/missing/child/process"));
    REQUIRE_FALSE(child.is_running());
    REQUIRE(child.pid() == -1);
}

TEST_CASE("ConnectedChildProcess kill terminates child process",
          "[events][child-process][ipc]") {
    const auto fixture = connected_child_fixture_path();
    REQUIRE_FALSE(fixture.empty());

    ConnectedChildProcess child;
    std::mutex mutex;
    std::condition_variable cv;
    bool ready = false;
    bool exit_called = false;

    child.on_message = [&](std::string_view msg) {
        std::lock_guard<std::mutex> lock(mutex);
        ready = (msg == "ready");
        cv.notify_all();
    };
    child.on_exit = [&](int) {
        std::lock_guard<std::mutex> lock(mutex);
        exit_called = true;
        cv.notify_all();
    };

    REQUIRE(child.launch(fixture, {"--hold-ms", "5000"}));
    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return ready; }));
    }

    REQUIRE(child.is_running());
    child.kill();
    REQUIRE_FALSE(child.is_running());

    std::lock_guard<std::mutex> lock(mutex);
    REQUIRE(exit_called);
}

TEST_CASE("ConnectedChildProcess concurrent kill joins once",
          "[events][child-process][ipc]") {
    const auto fixture = connected_child_fixture_path();
    REQUIRE_FALSE(fixture.empty());

    ConnectedChildProcess child;
    std::mutex mutex;
    std::condition_variable cv;
    bool ready = false;

    child.on_message = [&](std::string_view msg) {
        std::lock_guard<std::mutex> lock(mutex);
        ready = (msg == "ready");
        cv.notify_all();
    };

    REQUIRE(child.launch(fixture, {"--hold-ms", "5000"}));
    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return ready; }));
    }

    std::thread first([&] { child.kill(); });
    std::thread second([&] { child.kill(); });
    first.join();
    second.join();

    REQUIRE_FALSE(child.is_running());
}

TEST_CASE("ConnectedChildProcess can relaunch after async completion",
          "[events][child-process][ipc]") {
    const auto fixture = connected_child_fixture_path();
    REQUIRE_FALSE(fixture.empty());

    ConnectedChildProcess child;
    std::mutex mutex;
    std::condition_variable cv;
    int exits = 0;
    int last_exit_code = -1;

    child.on_exit = [&](int code) {
        std::lock_guard<std::mutex> lock(mutex);
        ++exits;
        last_exit_code = code;
        cv.notify_all();
    };

    REQUIRE(child.launch(fixture, {"--exit-code", "5"}));
    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return exits == 1 && last_exit_code == 5;
        }));
    }
    REQUIRE_FALSE(child.is_running());

    REQUIRE(child.launch(fixture, {"--exit-code", "6"}));
    REQUIRE(child.wait_for_exit(5000) == 6);

    std::lock_guard<std::mutex> lock(mutex);
    REQUIRE(exits == 2);
    REQUIRE(last_exit_code == 6);
}

TEST_CASE("ConnectedChildProcess wait_for_exit handles timeout then blocking wait",
          "[events][child-process][ipc]") {
    const auto fixture = connected_child_fixture_path();
    REQUIRE_FALSE(fixture.empty());

    ConnectedChildProcess child;
    REQUIRE(child.launch(fixture, {"--exit-code", "31", "--hold-ms", "150"}));

    REQUIRE(child.wait_for_exit(1) == -1);
    REQUIRE(child.wait_for_exit(0) == 31);
    REQUIRE_FALSE(child.is_running());
}

TEST_CASE("ConnectedChildProcess wait_for_exit is safe from exit callback",
          "[events][child-process][ipc]") {
    const auto fixture = connected_child_fixture_path();
    REQUIRE_FALSE(fixture.empty());

    ConnectedChildProcess child;
    std::mutex mutex;
    std::condition_variable cv;
    bool callback_done = false;
    int callback_wait_code = -1;

    child.on_exit = [&](int) {
        const int waited = child.wait_for_exit(0);
        std::lock_guard<std::mutex> lock(mutex);
        callback_wait_code = waited;
        callback_done = true;
        cv.notify_all();
    };

    REQUIRE(child.launch(fixture, {"--exit-code", "32"}));
    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return callback_done;
        }));
    }

    REQUIRE(callback_wait_code == 32);
    REQUIRE_FALSE(child.is_running());
}

TEST_CASE("ConnectedChildProcess wait_for_exit is safe from message callback kill",
          "[events][child-process][ipc]") {
    const auto fixture = connected_child_fixture_path();
    REQUIRE_FALSE(fixture.empty());

    ConnectedChildProcess child;
    std::mutex mutex;
    std::condition_variable cv;
    bool callback_done = false;
    bool running_after_callback_wait = true;
    int callback_pid = -1;

    child.on_message = [&](std::string_view msg) {
        if (msg != "ready") return;

        const int observed_pid = child.pid();
        child.kill();
        (void)child.wait_for_exit(5000);

        std::lock_guard<std::mutex> lock(mutex);
        callback_pid = observed_pid;
        running_after_callback_wait = child.is_running();
        callback_done = true;
        cv.notify_all();
    };

    REQUIRE(child.launch(fixture, {"--hold-ms", "5000"}));
    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return callback_done;
        }));
    }

    REQUIRE_FALSE(running_after_callback_wait);
    REQUIRE(callback_pid > 0);
    REQUIRE_FALSE(child.is_running());
}

TEST_CASE("ConnectedChildProcess wait_for_exit supports concurrent waiters",
          "[events][child-process][ipc]") {
    const auto fixture = connected_child_fixture_path();
    REQUIRE_FALSE(fixture.empty());

    ConnectedChildProcess child;
    REQUIRE(child.launch(fixture, {"--exit-code", "33", "--hold-ms", "75"}));

    int first_code = -1;
    int second_code = -1;
    std::thread first_waiter([&] {
        first_code = child.wait_for_exit(0);
    });
    std::thread second_waiter([&] {
        second_code = child.wait_for_exit(0);
    });

    first_waiter.join();
    second_waiter.join();

    REQUIRE(first_code == 33);
    REQUIRE(second_code == 33);
    REQUIRE_FALSE(child.is_running());
}

TEST_CASE("ConnectedChildProcess can relaunch after message callback kill",
          "[events][child-process][ipc]") {
    const auto fixture = connected_child_fixture_path();
    REQUIRE_FALSE(fixture.empty());

    ConnectedChildProcess child;
    std::mutex mutex;
    std::condition_variable cv;
    bool callback_done = false;

    child.on_message = [&](std::string_view msg) {
        if (msg != "ready") return;

        child.kill();
        (void)child.wait_for_exit(5000);

        std::lock_guard<std::mutex> lock(mutex);
        callback_done = true;
        cv.notify_all();
    };

    REQUIRE(child.launch(fixture, {"--hold-ms", "5000"}));
    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return callback_done;
        }));
    }
    REQUIRE_FALSE(child.is_running());

    child.on_message = nullptr;
    REQUIRE(child.launch(fixture, {"--exit-code", "36"}));
    REQUIRE(child.wait_for_exit(0) == 36);
    REQUIRE_FALSE(child.is_running());
}

TEST_CASE("ConnectedChildProcess destructor waits for active message callback",
          "[events][child-process][ipc]") {
    const auto fixture = connected_child_fixture_path();
    REQUIRE_FALSE(fixture.empty());

    auto child = std::make_unique<ConnectedChildProcess>();
    std::mutex mutex;
    std::condition_variable cv;
    bool callback_entered = false;
    bool release_callback = false;
    bool callback_finished = false;
    bool destructor_started = false;
    bool destructor_finished = false;

    child->on_message = [&](std::string_view msg) {
        if (msg != "ready") return;

        std::unique_lock<std::mutex> lock(mutex);
        callback_entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release_callback; });
        callback_finished = true;
        cv.notify_all();
    };

    REQUIRE(child->launch(fixture, {"--hold-ms", "5000"}));
    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return callback_entered;
        }));
    }

    std::thread destroyer([&] {
        {
            std::lock_guard<std::mutex> lock(mutex);
            destructor_started = true;
        }
        cv.notify_all();

        child.reset();

        {
            std::lock_guard<std::mutex> lock(mutex);
            destructor_finished = true;
        }
        cv.notify_all();
    });

    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return destructor_started;
        }));
        REQUIRE_FALSE(cv.wait_for(lock, std::chrono::milliseconds(100), [&] {
            return destructor_finished;
        }));
        release_callback = true;
    }
    cv.notify_all();

    destroyer.join();
    REQUIRE(callback_finished);
    REQUIRE(destructor_finished);
}

TEST_CASE("ChildProcessManager cleanup keeps live children",
          "[events][child-process][ipc]") {
    const auto fixture = connected_child_fixture_path();
    REQUIRE_FALSE(fixture.empty());

    ChildProcessManager manager;
    auto* child = manager.launch(fixture, {"--hold-ms", "5000"});
    REQUIRE(child != nullptr);
    REQUIRE(manager.active_count() == 1);

    manager.cleanup();
    REQUIRE(manager.active_count() == 1);

    manager.kill_all();
    REQUIRE_FALSE(child->is_running());
}

TEST_CASE("ChildProcessManager wait_all waits for active connected children",
          "[events][child-process][ipc]") {
    const auto fixture = connected_child_fixture_path();
    REQUIRE_FALSE(fixture.empty());

    ChildProcessManager manager;
    std::mutex mutex;
    int callback_count = 0;
    bool saw_first = false;
    bool saw_second = false;
    manager.on_child_exit = [&](ConnectedChildProcess*, int code) {
        if (code == 34)
            manager.cleanup();

        std::lock_guard<std::mutex> lock(mutex);
        ++callback_count;
        saw_first = saw_first || code == 34;
        saw_second = saw_second || code == 35;
    };

    auto* first = manager.launch(fixture, {"--exit-code", "34", "--hold-ms", "75"});
    REQUIRE(first != nullptr);
    auto* second = manager.launch(fixture, {"--exit-code", "35", "--hold-ms", "150"});
    REQUIRE(second != nullptr);
    REQUIRE(manager.active_count() == 2);

    manager.wait_all(5000);
    REQUIRE_FALSE(second->is_running());
    {
        std::lock_guard<std::mutex> lock(mutex);
        REQUIRE(callback_count == 2);
        REQUIRE(saw_first);
        REQUIRE(saw_second);
    }

    manager.cleanup();
    REQUIRE(manager.active_count() == 0);
}

TEST_CASE("ChildProcessManager cleanup is safe from exit callback",
          "[events][child-process][ipc]") {
    const auto fixture = connected_child_fixture_path();
    REQUIRE_FALSE(fixture.empty());

    ChildProcessManager manager;
    std::mutex mutex;
    std::condition_variable cv;
    bool callback_done = false;
    int callback_code = -1;
    int count_after_cleanup = -1;
    bool pointer_still_usable = false;

    manager.on_child_exit = [&](ConnectedChildProcess* exited, int code) {
        manager.cleanup();
        const bool exited_is_not_running = !exited->is_running();
        std::lock_guard<std::mutex> lock(mutex);
        callback_code = code;
        count_after_cleanup = manager.active_count();
        pointer_still_usable = exited_is_not_running;
        callback_done = true;
        cv.notify_all();
    };

    auto* child = manager.launch(fixture, {"--exit-code", "29"});
    REQUIRE(child != nullptr);

    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return callback_done;
        }));
    }

    REQUIRE(callback_code == 29);
    REQUIRE(count_after_cleanup == 0);
    REQUIRE(pointer_still_usable);
    REQUIRE(manager.active_count() == 0);
}

TEST_CASE("ChildProcessManager cleans up completed connected children",
          "[events][child-process][ipc]") {
    const auto fixture = connected_child_fixture_path();
    REQUIRE_FALSE(fixture.empty());

    ChildProcessManager manager;
    int callback_code = -1;
    manager.on_child_exit = [&](ConnectedChildProcess*, int code) {
        callback_code = code;
    };

    auto* child = manager.launch(fixture, {"--exit-code", "23", "--hold-ms", "200"});
    REQUIRE(child != nullptr);
    REQUIRE(manager.active_count() == 1);
    REQUIRE(child->wait_for_exit(5000) == 23);
    REQUIRE(callback_code == 23);

    manager.cleanup();
    REQUIRE(manager.active_count() == 0);
}

// "IPC socket server rejects malformed listen endpoints" lived here
// before companion-track U-8. Its coverage is subsumed by the more
// thorough tables in test_ipc_endpoints.cpp ("IPC socket listener
// rejects empty endpoint strings" + "...rejects malformed port
// strings without staying running"). Kept here as a pointer so a
// future test author doesn't recreate the duplicate.

TEST_CASE("IPC socket server stops while waiting for a client",
          "[events][ipc][socket]") {
    InterprocessConnectionServer server;
    auto port = start_socket_server_on_loopback(server);
    REQUIRE(port.has_value());
    REQUIRE(server.is_running());

    server.stop();
    REQUIRE_FALSE(server.is_running());
}

TEST_CASE("IPC socket server default callback owns accepted clients",
          "[events][ipc][socket][codecov][phase3]") {
    InterprocessConnectionServer server;
    auto port = start_socket_server_on_any_interface(server);
    REQUIRE(port.has_value());
    REQUIRE(server.is_running());

    InterprocessConnection client;
    REQUIRE(client.connect("127.0.0.1:" + std::to_string(*port), IpcTransport::Socket));

    client.disconnect();
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
