#include <catch2/catch_test_macros.hpp>

#include "control_broker_daemon.hpp"

#include <pulp/events/interprocess_connection.hpp>
#include <pulp/inspect/control_protocol.hpp>
#include <pulp/runtime/crypto.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>

using namespace std::chrono_literals;
using namespace pulp::events;
using namespace pulp::inspect;

namespace {

struct DaemonRoot {
    std::filesystem::path path;
    std::string suffix;

    DaemonRoot() {
        const auto random = pulp::runtime::secure_random_bytes(8);
        REQUIRE(random.has_value());
        suffix = pulp::runtime::hex_encode(*random);
        path = std::filesystem::path{"/private/tmp"} / ("pcd-" + suffix);
        REQUIRE(std::filesystem::create_directory(path));
    }

    ~DaemonRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

std::optional<ControlEnvelope> request_health(const std::filesystem::path& endpoint) {
    std::mutex mutex;
    std::condition_variable ready;
    std::optional<ControlEnvelope> response;
    InterprocessConnection client;
    client.set_on_message([&](const void* data, std::size_t size) {
        {
            std::lock_guard lock(mutex);
            response =
                decode_control_envelope(std::string_view(static_cast<const char*>(data), size));
        }
        ready.notify_all();
    });
    if (!client.connect(endpoint.string(), IpcTransport::LocalSocket, 2s))
        return std::nullopt;
    if (!client.send_message(encode_control_envelope(
            ControlEnvelope{.payload = ControlHealthEnvelope{.request_id = "daemon-health"}})))
        return std::nullopt;
    {
        std::unique_lock lock(mutex);
        if (!ready.wait_for(lock, 2s, [&] { return response.has_value(); }))
            return std::nullopt;
    }
    client.disconnect();
    return response;
}

} // namespace

TEST_CASE("control broker daemon is a restartable per-user singleton with health",
          "[inspect][control][daemon][health]") {
#ifdef __APPLE__
    DaemonRoot root;
    const ControlBrokerDaemonConfig config{
        .runtime_root = root.path,
        .sdk_version = "0.791.0-test",
        .process_generation = 77,
    };
    ControlBrokerDaemon first{config};
    REQUIRE(first.start());
    CHECK(first.is_running());
    REQUIRE(std::filesystem::exists(first.endpoint_path()));

    ControlBrokerDaemon second{config};
    CHECK_FALSE(second.start());
    CHECK_FALSE(second.is_running());

    const auto response = request_health(first.endpoint_path());
    REQUIRE(response.has_value());
    const auto* health = std::get_if<ControlHealthResult>(&response->payload);
    REQUIRE(health != nullptr);
    CHECK(health->request_id == "daemon-health");
    CHECK(health->sdk_version == "0.791.0-test");
    CHECK(health->process_generation == 77);
    CHECK_FALSE(health->broker_id.empty());

    first.stop();
    CHECK_FALSE(first.is_running());
    CHECK_FALSE(std::filesystem::exists(first.endpoint_path()));
    REQUIRE(second.start());
    CHECK(second.is_running());
    second.stop();

    InterprocessConnectionServer live_endpoint;
    REQUIRE(live_endpoint.start(first.endpoint_path().string(), IpcTransport::LocalSocket));
    ControlBrokerDaemon contender{config};
    CHECK_FALSE(contender.start());
    CHECK(std::filesystem::exists(first.endpoint_path()));
    live_endpoint.stop();
#else
    SUCCEED("the authenticated control broker daemon is currently macOS-only");
#endif
}
