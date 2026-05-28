#include <catch2/catch_test_macros.hpp>

#include <pulp/events/interprocess_connection.hpp>
#include <pulp/inspect/inspector_server.hpp>
#include <pulp/inspect/protocol.hpp>
#include <pulp/runtime/socket.hpp>

#include <choc/text/choc_JSON.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using pulp::events::InterprocessConnection;
using pulp::events::IpcTransport;
using pulp::inspect::InspectorMessage;
using pulp::inspect::InspectorServer;
using pulp::inspect::decode_message;
using pulp::inspect::encode_message;
using pulp::inspect::make_error;
using pulp::inspect::make_event;
using pulp::inspect::make_request;
using pulp::inspect::make_response;
using pulp::runtime::Socket;
using pulp::runtime::SocketType;

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

std::optional<uint16_t> start_inspector_server(InspectorServer& server) {
    const auto seed = socket_port_seed();
    for (uint16_t i = 0; i < 200; ++i) {
        const uint16_t port = static_cast<uint16_t>(20000 + ((seed + i) % 20000));
        if (server.start(port)) return port;
    }
    return std::nullopt;
}

std::optional<uint16_t> find_bindable_port() {
    const auto seed = socket_port_seed();
    for (uint16_t i = 0; i < 200; ++i) {
        const uint16_t port = static_cast<uint16_t>(20000 + ((seed + i) % 20000));
        Socket socket;
        if (socket.create(SocketType::TCP) && socket.bind("127.0.0.1", port)) {
            return port;
        }
    }
    return std::nullopt;
}

struct ScopedEnv {
    explicit ScopedEnv(std::string name) : name_(std::move(name)) {
        if (const char* prev = std::getenv(name_.c_str())) {
            prev_ = std::string(prev);
            had_prev_ = true;
        }
    }

    ~ScopedEnv() {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), had_prev_ ? prev_.c_str() : "");
#else
        if (had_prev_) ::setenv(name_.c_str(), prev_.c_str(), 1);
        else ::unsetenv(name_.c_str());
#endif
    }

    void set(const std::string& value) {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), value.c_str());
#else
        ::setenv(name_.c_str(), value.c_str(), 1);
#endif
    }

private:
    std::string name_;
    std::string prev_;
    bool had_prev_ = false;
};

std::filesystem::path inspector_port_file(const std::filesystem::path& dir) {
#ifdef _WIN32
    const auto pid = _getpid();
#else
    const auto pid = getpid();
#endif
    return dir / ("pulp-inspector-" + std::to_string(pid) + ".port");
}

constexpr const char* discovery_env_name() {
#ifdef _WIN32
    return "TEMP";
#else
    return "TMPDIR";
#endif
}

struct RecordingClient {
    InterprocessConnection conn;
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::string> messages;

    RecordingClient() {
        conn.on_text_message = [this](std::string_view message) {
            std::lock_guard<std::mutex> lock(mutex);
            messages.emplace_back(message);
            cv.notify_all();
        };
    }

    bool connect(uint16_t port) {
        return conn.connect("127.0.0.1:" + std::to_string(port), IpcTransport::Socket);
    }

    std::string wait_for_message(std::size_t index) {
        std::unique_lock<std::mutex> lock(mutex);
        const bool ok = cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return messages.size() > index;
        });
        REQUIRE(ok);
        return messages[index];
    }

    bool wait_for_message_count(std::size_t expected,
                                std::chrono::milliseconds timeout = std::chrono::milliseconds(250)) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, timeout, [&] {
            return messages.size() >= expected;
        });
    }

    std::size_t message_count() {
        std::lock_guard<std::mutex> lock(mutex);
        return messages.size();
    }
};

bool wait_for_client_count(InspectorServer& server, int expected) {
    for (int i = 0; i < 100; ++i) {
        if (server.client_count() == expected) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool wait_for_disconnect(InterprocessConnection& conn) {
    for (int i = 0; i < 100; ++i) {
        if (!conn.is_connected()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

}  // namespace

TEST_CASE("InspectorServer starts on an explicit port and writes discovery file",
          "[inspect][server]") {
    const auto tmp = std::filesystem::temp_directory_path() /
                     ("pulp-inspector-server-test-" + std::to_string(socket_port_seed()));
    std::filesystem::create_directories(tmp);
    ScopedEnv tmpdir(discovery_env_name());
    tmpdir.set(tmp.string());

    InspectorServer server;
    auto port = start_inspector_server(server);
    REQUIRE(port.has_value());
    REQUIRE(server.port() == *port);

    const auto file = inspector_port_file(tmp);
    REQUIRE(std::filesystem::exists(file));
    std::ifstream in(file);
    std::string contents;
    in >> contents;
    REQUIRE(contents == std::to_string(*port));

    server.stop();
    server.stop();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("InspectorServer honors PULP_INSPECTOR_PORT when starting with zero",
          "[inspect][server]") {
    auto candidate = find_bindable_port();
    REQUIRE(candidate.has_value());

    const auto tmp = std::filesystem::temp_directory_path() /
                     ("pulp-inspector-env-test-" + std::to_string(*candidate));
    std::filesystem::create_directories(tmp);
    ScopedEnv tmpdir(discovery_env_name());
    ScopedEnv port_env("PULP_INSPECTOR_PORT");
    tmpdir.set(tmp.string());
    port_env.set(std::to_string(*candidate));

    InspectorServer server;
    REQUIRE(server.start(0));
    REQUIRE(server.port() == *candidate);

    const auto file = inspector_port_file(tmp);
    std::ifstream in(file);
    std::string contents;
    in >> contents;
    REQUIRE(contents == std::to_string(*candidate));

    server.stop();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("InspectorServer ignores malformed PULP_INSPECTOR_PORT values",
          "[inspect][server][coverage][requested]") {
    const auto tmp = std::filesystem::temp_directory_path() /
                     ("pulp-inspector-invalid-env-test-" +
                      std::to_string(socket_port_seed()));
    std::filesystem::create_directories(tmp);
    ScopedEnv tmpdir(discovery_env_name());
    ScopedEnv port_env("PULP_INSPECTOR_PORT");
    tmpdir.set(tmp.string());
    port_env.set("not-a-port");

    InspectorServer server;
    if (!server.start(0)) {
        std::filesystem::remove_all(tmp);
        SKIP("default inspector port is already in use");
    }

    REQUIRE(server.port() == 9147);

    const auto file = inspector_port_file(tmp);
    std::ifstream in(file);
    std::string contents;
    in >> contents;
    REQUIRE(contents == "9147");

    server.stop();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("InspectorServer rejects a port already owned by another server",
          "[inspect][server]") {
    InspectorServer first;
    auto port = start_inspector_server(first);
    REQUIRE(port.has_value());

    InspectorServer second;
    REQUIRE_FALSE(second.start(*port));

    second.broadcast(make_event("Inspector.test", R"({"ignored":true})"));
    second.stop();
    first.stop();
}

TEST_CASE("InspectorServer tracks client connections and disconnects",
          "[inspect][server]") {
    InspectorServer server;
    auto port = start_inspector_server(server);
    REQUIRE(port.has_value());

    RecordingClient first;
    RecordingClient second;
    REQUIRE(first.connect(*port));
    REQUIRE(second.connect(*port));
    REQUIRE(wait_for_client_count(server, 2));

    first.conn.disconnect();
    REQUIRE(wait_for_client_count(server, 1));

    second.conn.disconnect();
    REQUIRE(wait_for_client_count(server, 0));
    server.stop();
}

TEST_CASE("InspectorServer dispatches requests through the configured handler",
          "[inspect][server]") {
    InspectorServer server;
    std::atomic<int> handled{0};
    std::string seen_method;
    std::string seen_params;
    std::mutex seen_mutex;

    server.set_request_handler([&](const InspectorMessage& request) {
        std::lock_guard<std::mutex> lock(seen_mutex);
        ++handled;
        seen_method = request.method;
        seen_params = request.params_json;
        return make_response(request.id, R"({"ok":true,"source":"handler"})");
    });

    auto port = start_inspector_server(server);
    REQUIRE(port.has_value());

    RecordingClient client;
    REQUIRE(client.connect(*port));
    REQUIRE(wait_for_client_count(server, 1));
    REQUIRE(client.conn.send_message(encode_message(
        make_request(41, "Inspector.getInfo", R"({"detail":"full"})"))));

    InspectorMessage response;
    REQUIRE(decode_message(client.wait_for_message(0), response));
    REQUIRE(response.id == 41);
    REQUIRE_FALSE(response.is_error);
    const auto response_json = choc::json::parse(response.params_json);
    REQUIRE(response_json["ok"].getBool());
    REQUIRE(std::string(response_json["source"].getString()) == "handler");

    {
        std::lock_guard<std::mutex> lock(seen_mutex);
        REQUIRE(handled.load() == 1);
        REQUIRE(seen_method == "Inspector.getInfo");
        const auto request_json = choc::json::parse(seen_params);
        REQUIRE(std::string(request_json["detail"].getString()) == "full");
    }

    client.conn.disconnect();
    server.stop();
}

TEST_CASE("InspectorServer uses the latest installed request handler",
          "[inspect][server][coverage][requested]") {
    InspectorServer server;
    auto port = start_inspector_server(server);
    REQUIRE(port.has_value());

    server.set_request_handler([](const InspectorMessage& request) {
        return make_response(request.id, R"({"value":"first"})");
    });
    server.set_request_handler([](const InspectorMessage& request) {
        return make_response(request.id, R"({"value":"second"})");
    });

    RecordingClient client;
    REQUIRE(client.connect(*port));
    REQUIRE(wait_for_client_count(server, 1));

    REQUIRE(client.conn.send_message(encode_message(make_request(17, "Echo.latest"))));

    InspectorMessage message;
    REQUIRE(decode_message(client.wait_for_message(0), message));
    REQUIRE(message.id == 17);
    REQUIRE_FALSE(message.is_error);
    const auto response_json = choc::json::parse(message.params_json);
    REQUIRE(std::string(response_json["value"].getString()) == "second");

    client.conn.disconnect();
    server.stop();
}

TEST_CASE("InspectorServer can refresh its discovery file after startup",
          "[inspect][server]") {
    const auto tmp = std::filesystem::temp_directory_path() /
                     ("pulp-inspector-refresh-test-" + std::to_string(socket_port_seed()));
    std::filesystem::create_directories(tmp);
    ScopedEnv tmpdir(discovery_env_name());
    tmpdir.set(tmp.string());

    InspectorServer server;
    auto port = start_inspector_server(server);
    REQUIRE(port.has_value());
    REQUIRE(server.port() == *port);

    const auto file = inspector_port_file(tmp);
    REQUIRE(std::filesystem::exists(file));
    std::filesystem::remove(file);
    REQUIRE_FALSE(std::filesystem::exists(file));

    server.advertise_port();
    REQUIRE(std::filesystem::exists(file));
    std::ifstream in(file);
    std::string contents;
    in >> contents;
    REQUIRE(contents == std::to_string(*port));

    server.stop();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("InspectorServer suppresses empty handler responses",
          "[inspect][server]") {
    InspectorServer server;
    std::atomic<int> handled{0};
    std::string seen_method;
    std::mutex seen_mutex;

    server.set_request_handler([&](const InspectorMessage& request) {
        std::lock_guard<std::mutex> lock(seen_mutex);
        ++handled;
        seen_method = request.method;
        return InspectorMessage{};
    });

    auto port = start_inspector_server(server);
    REQUIRE(port.has_value());

    RecordingClient client;
    REQUIRE(client.connect(*port));
    REQUIRE(wait_for_client_count(server, 1));
    REQUIRE(client.conn.send_message(encode_message(
        make_request(77, "Inspector.observeOnly", R"({"silent":true})"))));

    for (int i = 0; i < 100 && handled.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(handled.load() == 1);
    {
        std::lock_guard<std::mutex> lock(seen_mutex);
        REQUIRE(seen_method == "Inspector.observeOnly");
    }
    REQUIRE_FALSE(client.wait_for_message_count(1));
    REQUIRE(client.message_count() == 0);

    client.conn.disconnect();
    REQUIRE(wait_for_client_count(server, 0));
    server.stop();
}

TEST_CASE("InspectorServer can send handler-produced events",
          "[inspect][server]") {
    InspectorServer server;
    std::atomic<int> handled{0};
    std::mutex seen_mutex;
    int seen_id = 0;
    std::string seen_method;

    server.set_request_handler([&](const InspectorMessage& request) {
        std::lock_guard<std::mutex> lock(seen_mutex);
        ++handled;
        seen_id = request.id;
        seen_method = request.method;
        return make_event("Inspector.subscriptionReady", R"({"channel":"dom","ready":true})");
    });

    auto port = start_inspector_server(server);
    REQUIRE(port.has_value());

    RecordingClient client;
    REQUIRE(client.connect(*port));
    REQUIRE(wait_for_client_count(server, 1));
    REQUIRE(client.conn.send_message(encode_message(
        make_request(5, "Inspector.subscribe", R"({"channel":"dom"})"))));

    InspectorMessage event;
    REQUIRE(decode_message(client.wait_for_message(0), event));
    REQUIRE(handled.load() == 1);
    {
        std::lock_guard<std::mutex> lock(seen_mutex);
        REQUIRE(seen_id == 5);
        REQUIRE(seen_method == "Inspector.subscribe");
    }
    REQUIRE(event.method == "Inspector.subscriptionReady");
    const auto event_json = choc::json::parse(event.params_json);
    REQUIRE(std::string(event_json["channel"].getString()) == "dom");
    REQUIRE(event_json["ready"].getBool());

    client.conn.disconnect();
    server.stop();
}

TEST_CASE("InspectorServer returns protocol errors for invalid JSON frames",
          "[inspect][server]") {
    InspectorServer server;
    server.set_request_handler([](const InspectorMessage&) {
        return make_error(99, "handler should not run");
    });

    auto port = start_inspector_server(server);
    REQUIRE(port.has_value());

    RecordingClient client;
    REQUIRE(client.connect(*port));
    REQUIRE(wait_for_client_count(server, 1));
    REQUIRE(client.conn.send_message("{not-json"));

    InspectorMessage response;
    REQUIRE(decode_message(client.wait_for_message(0), response));
    REQUIRE(response.id == 0);
    REQUIRE(response.is_error);
    REQUIRE(response.params_json == "Invalid JSON message");

    client.conn.disconnect();
    server.stop();
}

TEST_CASE("InspectorServer drops valid requests when no handler is installed",
          "[inspect][server][coverage][requested]") {
    InspectorServer server;
    auto port = start_inspector_server(server);
    REQUIRE(port.has_value());

    RecordingClient client;
    REQUIRE(client.connect(*port));
    REQUIRE(wait_for_client_count(server, 1));
    REQUIRE(server.client_count() == 1);

    REQUIRE(client.conn.send_message(encode_message(
        make_request(88, "Inspector.unhandled", R"({"quiet":true})"))));

    REQUIRE_FALSE(client.wait_for_message_count(1));
    REQUIRE(client.message_count() == 0);
    REQUIRE(server.client_count() == 1);

    server.broadcast(make_event("Inspector.afterUnhandled", R"({"ok":true})"));

    InspectorMessage event;
    REQUIRE(decode_message(client.wait_for_message(0), event));
    REQUIRE(event.id == 0);
    REQUIRE(event.method == "Inspector.afterUnhandled");
    const auto event_json = choc::json::parse(event.params_json);
    REQUIRE(event_json["ok"].getBool());

    client.conn.disconnect();
    REQUIRE(wait_for_client_count(server, 0));
    server.stop();
}

TEST_CASE("InspectorServer broadcasts events to every connected client",
          "[inspect][server]") {
    InspectorServer server;
    auto port = start_inspector_server(server);
    REQUIRE(port.has_value());

    RecordingClient first;
    RecordingClient second;
    REQUIRE(first.connect(*port));
    REQUIRE(second.connect(*port));
    REQUIRE(wait_for_client_count(server, 2));

    server.broadcast(make_event("DOM.documentUpdated", R"({"reason":"test"})"));

    InspectorMessage first_event;
    InspectorMessage second_event;
    REQUIRE(decode_message(first.wait_for_message(0), first_event));
    REQUIRE(decode_message(second.wait_for_message(0), second_event));
    REQUIRE(first_event.id == 0);
    REQUIRE(first_event.method == "DOM.documentUpdated");
    const auto event_json = choc::json::parse(first_event.params_json);
    REQUIRE(std::string(event_json["reason"].getString()) == "test");
    REQUIRE(second_event.method == first_event.method);
    REQUIRE(second_event.params_json == first_event.params_json);

    first.conn.disconnect();
    second.conn.disconnect();
    server.stop();
}

TEST_CASE("InspectorServer prunes disconnected clients before later broadcasts",
          "[inspect][server]") {
    InspectorServer server;
    auto port = start_inspector_server(server);
    REQUIRE(port.has_value());

    RecordingClient first;
    RecordingClient second;
    REQUIRE(first.connect(*port));
    REQUIRE(second.connect(*port));
    REQUIRE(wait_for_client_count(server, 2));

    server.broadcast(make_event("Inspector.first", R"({"sequence":1})"));
    InspectorMessage first_initial;
    InspectorMessage second_initial;
    REQUIRE(decode_message(first.wait_for_message(0), first_initial));
    REQUIRE(decode_message(second.wait_for_message(0), second_initial));
    REQUIRE(first_initial.method == "Inspector.first");

    first.conn.disconnect();
    REQUIRE(wait_for_client_count(server, 1));

    server.broadcast(make_event("Inspector.second", R"({"sequence":2})"));
    InspectorMessage second_later;
    REQUIRE(decode_message(second.wait_for_message(1), second_later));
    REQUIRE(second_later.method == "Inspector.second");
    const auto later_json = choc::json::parse(second_later.params_json);
    REQUIRE(static_cast<int>(later_json["sequence"].getInt64()) == 2);
    REQUIRE_FALSE(first.wait_for_message_count(2));

    second.conn.disconnect();
    REQUIRE(wait_for_client_count(server, 0));
    server.stop();
}

TEST_CASE("InspectorServer stop disconnects clients and is idempotent after activity",
          "[inspect][server]") {
    InspectorServer server;
    auto port = start_inspector_server(server);
    REQUIRE(port.has_value());

    RecordingClient client;
    REQUIRE(client.connect(*port));
    REQUIRE(wait_for_client_count(server, 1));

    server.broadcast(make_event("Inspector.beforeStop", R"({"active":true})"));
    InspectorMessage event;
    REQUIRE(decode_message(client.wait_for_message(0), event));
    REQUIRE(event.method == "Inspector.beforeStop");

    server.stop();
    REQUIRE(server.client_count() == 0);
    REQUIRE(wait_for_disconnect(client.conn));

    server.stop();
    REQUIRE(server.client_count() == 0);
    REQUIRE(server.port() == *port);
}
