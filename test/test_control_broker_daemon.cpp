#include <catch2/catch_test_macros.hpp>

#include "control_broker_daemon.hpp"

#include <pulp/events/interprocess_connection.hpp>
#include <pulp/inspect/control_artifacts.hpp>
#include <pulp/inspect/control_carrier.hpp>
#include <pulp/inspect/control_operations.hpp>
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
    std::filesystem::path runtime;
    std::filesystem::path state;
    std::string suffix;

    DaemonRoot() {
        const auto random = pulp::runtime::secure_random_bytes(8);
        REQUIRE(random.has_value());
        suffix = pulp::runtime::hex_encode(*random);
        path = std::filesystem::path{"/private/tmp"} / ("pcd-" + suffix);
        REQUIRE(std::filesystem::create_directory(path));
        std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
        runtime = path / "runtime";
        state = path / "state";
    }

    ~DaemonRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

ControlOperationBinding operation_binding() {
    ControlOperationBinding binding;
    static_cast<ControlAuthorityBinding&>(binding) = {
        .broker_id = ControlBrokerId{"broker-a"},
        .client_principal = "peer:client-a",
        .client_id = ControlClientId{"client-a"},
        .registration_id = ControlRegistrationId{"registration-a"},
        .grant_id = ControlGrantId{"grant-a"},
        .session_id = "session-a",
        .instance_id = "instance-a",
        .publication_id = "publication-a",
        .instance_generation = "generation-a",
        .capability = InspectorCapability::StateRead,
        .operation_id = "dev.pulp.state/get@1",
        .operation_version = 1,
        .consent_decision_id = "consent-a",
        .manifest_digest = std::string(64, 'a'),
        .producer_artifact_digest = std::string(64, 'b'),
        .deadline_unix_ms = 4'000'000'000'000,
    };
    binding.request_id = "request-a";
    binding.idempotency_key = "idempotency-a";
    binding.canonical_request_hash = pulp::runtime::sha256_hex("{}");
    return binding;
}

ControlArtifactLineage artifact_lineage(const ControlReceiptId& receipt_id) {
    return {
        .broker_id = "broker-a",
        .receipt_id = receipt_id.value,
        .producer_client_id = "client-a",
        .producer_registration_id = "registration-a",
        .session_id = "session-a",
        .instance_id = "instance-a",
        .publication_id = "publication-a",
        .producer_capability_id = "dev.pulp.state/read@1",
        .producer_operation_id = "dev.pulp.state/get@1",
        .producer_operation_version = 1,
        .original_grant_id = "grant-a",
        .consent_decision_id = "consent-a",
        .manifest_digest = std::string(64, 'a'),
        .producer_artifact_digest = std::string(64, 'b'),
    };
}

bool has_credential_named_entry(const std::filesystem::path& root) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        const auto name = entry.path().filename().string();
        if (name.find("secret") != std::string::npos || name.find("token") != std::string::npos ||
            name.find("ticket") != std::string::npos ||
            name.find("bootstrap") != std::string::npos ||
            name.find("credential") != std::string::npos) {
            return true;
        }
    }
    return false;
}

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
        .runtime_root = root.runtime,
        .state_root = root.state,
        .sdk_version = "0.791.0-test",
        .process_generation = 77,
    };
    ControlBrokerDaemon first{config};
    REQUIRE(first.start());
    CHECK(first.is_running());
    REQUIRE(std::filesystem::exists(first.endpoint_path()));
    CHECK(first.state_directory() == root.state);
    CHECK(std::filesystem::is_directory(root.state / "operations"));
    CHECK(std::filesystem::is_directory(root.state / "artifacts"));
    CHECK_FALSE(
        std::filesystem::exists(default_control_runtime_directory(root.runtime) / "operations"));
    CHECK_FALSE(
        std::filesystem::exists(default_control_runtime_directory(root.runtime) / "artifacts"));
    CHECK_FALSE(has_credential_named_entry(root.state));

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

TEST_CASE("control broker daemon preserves durable receipts and artifacts across restart",
          "[inspect][control][daemon][persistence][restart]") {
#ifdef __APPLE__
    DaemonRoot root;
    const auto operations = root.state / "operations";
    const auto artifacts = root.state / "artifacts";
    ControlReceiptId receipt_id;
    std::string artifact_id;
    {
        ControlBrokerDaemon initializer{{.runtime_root = root.runtime,
                                         .state_root = root.state,
                                         .sdk_version = "0.795.0-test"}};
        REQUIRE(initializer.start());
        initializer.stop();
    }
    {
        ControlOperationStore store{{.directory = operations}};
        REQUIRE(store.open().succeeded());
        const auto admitted = store.admit(operation_binding());
        REQUIRE(admitted.receipt);
        receipt_id = admitted.receipt->receipt_id;
        REQUIRE(store.begin(receipt_id).succeeded());

        ControlArtifactStore artifact_store{{.root = artifacts}};
        REQUIRE(artifact_store.is_ready());
        const std::string payload = "persistent-artifact";
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
        const auto stored = artifact_store.store(
            {reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()},
            artifact_lineage(receipt_id),
            {.content_type = "text/plain",
             .created_at_unix_ms = static_cast<std::uint64_t>(now_ms),
             .expires_at_unix_ms = static_cast<std::uint64_t>(now_ms) + 3'600'000});
        REQUIRE(stored.metadata);
        artifact_id = stored.metadata->artifact_id;
    }

    ControlBrokerDaemon daemon{
        {.runtime_root = root.runtime, .state_root = root.state, .sdk_version = "0.795.0-test"}};
    REQUIRE(daemon.start());
    daemon.stop();

    REQUIRE(std::filesystem::remove_all(root.runtime) > 0);
    CHECK(std::filesystem::exists(operations / (receipt_id.value + ".json")));
    ControlBrokerDaemon restarted{
        {.runtime_root = root.runtime, .state_root = root.state, .sdk_version = "0.795.0-test"}};
    REQUIRE(restarted.start());
    restarted.stop();

    ControlOperationStore reopened{{.directory = operations}};
    REQUIRE(reopened.open().succeeded());
    REQUIRE(reopened.receipt(receipt_id));
    CHECK(reopened.receipt(receipt_id)->state == ControlReceiptState::UnknownNeedsRefresh);
    ControlArtifactStore reopened_artifacts{{.root = artifacts}};
    REQUIRE(reopened_artifacts.is_ready());
    CHECK(reopened_artifacts.metadata(artifact_id).has_value());
    CHECK_FALSE(has_credential_named_entry(root.state));
#else
    SUCCEED("the authenticated control broker daemon is currently macOS-only");
#endif
}

TEST_CASE("control broker daemon rejects insecure symlinked or overlapping state roots",
          "[inspect][control][daemon][state][security]") {
#ifdef __APPLE__
    DaemonRoot root;
    REQUIRE(std::filesystem::create_directory(root.state));
    std::filesystem::permissions(root.state, std::filesystem::perms::all,
                                 std::filesystem::perm_options::replace);
    ControlBrokerDaemon insecure{
        {.runtime_root = root.runtime, .state_root = root.state, .sdk_version = "0.795.0-test"}};
    CHECK_FALSE(insecure.start());

    std::filesystem::permissions(root.state, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
    REQUIRE(std::filesystem::create_directory(root.state / "operations"));
    std::filesystem::permissions(root.state / "operations", std::filesystem::perms::all,
                                 std::filesystem::perm_options::replace);
    ControlBrokerDaemon insecure_store{
        {.runtime_root = root.runtime, .state_root = root.state, .sdk_version = "0.795.0-test"}};
    CHECK_FALSE(insecure_store.start());
    std::filesystem::remove_all(root.state / "operations");

    const auto link = root.path / "state-link";
    REQUIRE(std::filesystem::create_directory(root.path / "state-target"));
    std::filesystem::create_directory_symlink(root.path / "state-target", link);
    REQUIRE(std::filesystem::is_symlink(link));
    ControlBrokerDaemon symlinked{
        {.runtime_root = root.runtime, .state_root = link, .sdk_version = "0.795.0-test"}};
    CHECK_FALSE(symlinked.start());

    const auto runtime_target = root.path / "runtime-target";
    REQUIRE(std::filesystem::create_directory(runtime_target));
    std::filesystem::permissions(runtime_target, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
    const auto runtime_link = root.path / "runtime-link";
    std::filesystem::create_directory_symlink(runtime_target, runtime_link);
    REQUIRE(std::filesystem::is_symlink(runtime_link));
    ControlBrokerDaemon aliased_runtime{
        {.runtime_root = runtime_link, .state_root = root.state, .sdk_version = "0.795.0-test"}};
    CHECK_FALSE(aliased_runtime.start());

    const auto runtime_directory = default_control_runtime_directory(root.runtime);
    ControlBrokerDaemon overlapping{{.runtime_root = root.runtime,
                                     .state_root = runtime_directory / "state",
                                     .sdk_version = "0.795.0-test"}};
    CHECK_FALSE(overlapping.start());
    CHECK_FALSE(std::filesystem::exists(runtime_directory / "state"));
#else
    SUCCEED("the authenticated control broker daemon is currently macOS-only");
#endif
}
