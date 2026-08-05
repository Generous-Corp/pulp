#include <catch2/catch_test_macros.hpp>

#include <pulp/events/interprocess_connection.hpp>
#include <pulp/inspect/control_peer.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>

#ifndef _WIN32
#include <unistd.h>
#endif

using namespace std::chrono_literals;
using namespace pulp::events;
using namespace pulp::inspect;

namespace {

#ifndef _WIN32
class LocalPeerFixture {
public:
    LocalPeerFixture() {
        const auto serial = std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count();
        directory = std::filesystem::temp_directory_path() /
                    ("pulp-control-peer-" + std::to_string(getpid()) + "-" +
                     std::to_string(serial));
        std::filesystem::create_directory(directory);
        std::filesystem::permissions(
            directory, std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace);
        endpoint = directory / "broker.sock";
    }

    ~LocalPeerFixture() {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }

    std::filesystem::path directory;
    std::filesystem::path endpoint;
};
#endif

} // namespace

TEST_CASE("control peer identity is minted only from exact live carrier evidence",
          "[inspect][control][identity][peer][security]") {
#ifdef __APPLE__
    LocalPeerFixture fixture;
    InterprocessConnectionServer server;
    std::mutex mutex;
    std::condition_variable ready;
    std::unique_ptr<InterprocessConnection> accepted;
    server.on_client_connected =
        [&](std::unique_ptr<InterprocessConnection> connection) {
            std::lock_guard lock(mutex);
            accepted = std::move(connection);
            ready.notify_all();
        };

    REQUIRE(server.start(fixture.endpoint.string(), IpcTransport::LocalSocket));
    InterprocessConnection client;
    REQUIRE(client.connect(
        fixture.endpoint.string(), IpcTransport::LocalSocket, 2s));
    {
        std::unique_lock lock(mutex);
        REQUIRE(ready.wait_for(lock, 2s, [&] { return accepted != nullptr; }));
    }

    const auto observed = observe_control_peer(*accepted, ControlPeerRole::Client);
    REQUIRE(observed.has_value());
    CHECK(observed->process_id == static_cast<std::int64_t>(getpid()));
    CHECK(observed->user_id == "uid:" + std::to_string(getuid()));
    REQUIRE_FALSE(observed->process_start_id.empty());
    REQUIRE_FALSE(observed->executable_identity.empty());
    REQUIRE_FALSE(observed->publisher_id.empty());

    ControlPeerExpectation expectation{.evidence = *observed};
    const auto verified = verify_control_peer(*accepted, expectation);
    REQUIRE(verified.has_value());
    CHECK(verified->evidence().process_start_id == observed->process_start_id);
    CHECK(verified->evidence().executable_identity == observed->executable_identity);
    CHECK(verified->evidence().publisher_id == observed->publisher_id);

    auto tampered = expectation;
    tampered.evidence.user_id += "-forged";
    CHECK_FALSE(verify_control_peer(*accepted, tampered).has_value());
    tampered = expectation;
    ++tampered.evidence.process_id;
    CHECK_FALSE(verify_control_peer(*accepted, tampered).has_value());
    tampered = expectation;
    tampered.evidence.process_start_id += "-stale";
    CHECK_FALSE(verify_control_peer(*accepted, tampered).has_value());
    tampered = expectation;
    tampered.evidence.executable_identity += "-forged";
    CHECK_FALSE(verify_control_peer(*accepted, tampered).has_value());
    tampered = expectation;
    tampered.evidence.publisher_id += "-forged";
    CHECK_FALSE(verify_control_peer(*accepted, tampered).has_value());

    client.disconnect();
    accepted.reset();
    server.stop();
#else
    InterprocessConnection connection;
    CHECK_FALSE(observe_control_peer(
                    connection, ControlPeerRole::Client)
                    .has_value());
#endif
}
