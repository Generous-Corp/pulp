#include <catch2/catch_test_macros.hpp>

#include "control_broker_daemon.hpp"

#include <pulp/events/interprocess_connection.hpp>
#include <pulp/inspect/control_artifacts.hpp>
#include <pulp/inspect/control_carrier.hpp>
#include <pulp/inspect/control_client_connection.hpp>
#include <pulp/inspect/control_operations.hpp>
#include <pulp/inspect/control_protocol.hpp>
#include <pulp/runtime/crypto.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <choc/text/choc_JSON.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <sys/stat.h>
#endif

using namespace std::chrono_literals;
using namespace pulp::events;
using namespace pulp::inspect;

namespace {

constexpr std::string_view kHostManifest = R"({
  "schema": "dev.pulp.control/artifact-manifest@1",
  "schema_version": 1,
  "profile": "developer-local",
  "target": "pulp-control-trusted-host-e2e-fixture",
  "product_name": "Pulp Trusted Host E2E Fixture",
  "bundle_id": "dev.pulp.test.trusted-host-e2e-fixture",
  "build_id": "build:0123456789abcdef0123456789abcdef",
  "registry_digest": "8cfed31b632c6f75171d57d8d2d5c1c17bccd765e13095771b8f0e97acc08620",
  "endpoint_included": true,
  "unsafe_runtime_eval_acknowledged": false,
  "permission_terms": ["implemented", "built", "host_available", "activated", "policy_eligible", "client_granted", "session_live"],
  "capabilities": ["dev.pulp.session/control@1", "dev.pulp.trace/session-control@1"]
}
)";

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

#ifdef __APPLE__
std::filesystem::path current_executable() {
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (size == 0 || _NSGetExecutablePath(buffer.data(), &size) != 0)
        return {};
    std::error_code error;
    const auto result = std::filesystem::weakly_canonical(buffer.data(), error);
    return error ? std::filesystem::path{} : result;
}

std::string wait_for_registration(const std::filesystem::path& path) {
    for (unsigned attempt = 0; attempt < 10'000; ++attempt) {
        std::ifstream input(path);
        std::string registration;
        if (input >> registration)
            return registration;
        std::this_thread::sleep_for(1ms);
    }
    return {};
}
#endif

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

TEST_CASE("installed daemon composes host enrollment routing execution and restart teardown",
          "[inspect][control][daemon][host][e2e][restart]") {
#ifdef __APPLE__
    DaemonRoot root;
    const auto source = root.path / "source";
    REQUIRE(std::filesystem::create_directory(source));
    ::chmod(source.c_str(), 0700);
    const auto host_executable = source / "trusted-host";
    std::filesystem::copy_file(PULP_CONTROL_TRUSTED_HOST_E2E_FIXTURE, host_executable);
    ::chmod(host_executable.c_str(), 0700);
    {
        std::ofstream manifest(host_executable.string() + ".inspector-capabilities.json");
        manifest << kHostManifest;
    }
    ::chmod((host_executable.string() + ".inspector-capabilities.json").c_str(), 0600);

    const auto broker_executable = current_executable();
    REQUIRE_FALSE(broker_executable.empty());
    const auto install = root.path / "install";
    REQUIRE(std::filesystem::create_directory(install));
    const auto installed_broker = install / "pulp-control-broker";
    const auto installed_client = install / "pulp";
    std::filesystem::copy_file(broker_executable, installed_broker);
    std::filesystem::copy_file(broker_executable, installed_client);
    ::chmod(installed_broker.c_str(), 0700);
    ::chmod(installed_client.c_str(), 0700);
    ControlBrokerDaemonConfig config{
        .runtime_root = root.runtime,
        .state_root = root.state,
        .sdk_version = "0.795.2-test",
        .executable_path = installed_broker,
        .process_generation = 91,
        .decide_consent =
            [](const VerifiedControlPeerIdentity&, const ControlGrantRequest&) {
                return ControlConsentDecision{true, ControlConsentAuthority::TrustedHostUi,
                                              "daemon-host-e2e-consent"};
            },
    };
    ControlBrokerDaemon daemon{config};
    REQUIRE(daemon.start());

    const auto registration_path = root.path / "registration";
    const auto stop_path = root.path / "stop";
    const auto prepared = daemon.prepare_trusted_host({
        .executable = host_executable,
        .arguments = {registration_path.string(), stop_path.string()},
        .working_directory = source,
        .host_tier = ControlHostTier::Standalone,
    });
    INFO(control_trusted_host_inventory_status_id(prepared.status));
    REQUIRE(prepared.ticket);
    pulp::platform::ProcessOptions options;
    options.timeout_ms = 15'000;
    auto launched = daemon.launch_trusted_host(prepared.ticket->inventory_id, options);
    INFO(control_trusted_host_launch_status_id(launched.status));
    INFO(launched.explanation);
    REQUIRE(launched.launched());
    const ControlRegistrationId registration_id{wait_for_registration(registration_path)};
    REQUIRE(registration_id);

    ControlClientConnection connection({.endpoint_path = daemon.endpoint_path(),
                                        .expected_broker_executable = broker_executable});
    REQUIRE(connection.connect());
    const auto enrolled = connection.manage("enroll");
    INFO(enrolled.explanation);
    REQUIRE(enrolled.status_id == "accepted");
    const auto enrollment_data = choc::json::parse(enrolled.data_json);
    const auto client_id = std::string(enrollment_data["client_id"].getString());
    REQUIRE_FALSE(client_id.empty());

    const auto inventory = connection.manage("instances");
    REQUIRE(inventory.status_id == "completed");
    const auto inventory_data = choc::json::parse(inventory.data_json);
    REQUIRE(inventory_data["instances"].size() == 1);
    const auto instance = inventory_data["instances"][0];
    CHECK(instance["registration_id"].getString() == registration_id.value);
    const auto instance_id = std::string(instance["instance_id"].getString());
    const auto publication_id = std::string(instance["publication_id"].getString());

    auto grant_params = choc::value::createObject("");
    grant_params.addMember("instance_id", choc::value::createString(instance_id));
    grant_params.addMember("profile", choc::value::createString("develop"));
    const auto granted =
        connection.manage("grant-request", choc::json::toString(grant_params, false));
    INFO(granted.explanation);
    REQUIRE(granted.status_id == "granted");
    const auto grant_data = choc::json::parse(granted.data_json);
    const auto grant_id = std::string(grant_data["grant_id"].getString());
    REQUIRE_FALSE(grant_id.empty());

    ControlClient client(connection);
    REQUIRE(client.negotiate({.mandatory_features = {"receipts"}}).succeeded());
    ControlRequestEnvelope request{
        .request_id = "daemon-trace-request",
        .client_id = client_id,
        .registration_id = registration_id.value,
        .grant_id = grant_id,
        .instance_generation = publication_id,
        .operation_id = "dev.pulp.trace/session-control@1",
        .operation_version = 1,
        .idempotency_key = "daemon-trace-idempotency",
        .deadline_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                (std::chrono::system_clock::now() + 5s).time_since_epoch())
                                .count(),
        .params_json = R"({"action":"start","ring_mb":8})",
    };
    request.request_hash = *control_request_hash(request);
    const auto dispatched = client.request(request, 5s);
    INFO(dispatched.error_code);
    INFO(dispatched.explanation);
    REQUIRE(dispatched.succeeded());
    CHECK((dispatched.response->state == ControlReceiptState::Completed ||
           dispatched.response->result_code == ControlResultCode::NotBuilt));

    const auto pending = daemon.prepare_trusted_host({
        .executable = host_executable,
        .arguments = {registration_path.string(), stop_path.string()},
        .working_directory = source,
        .host_tier = ControlHostTier::Standalone,
    });
    REQUIRE(pending.ticket);
    connection.disconnect();
    daemon.stop();

    config.process_generation = 92;
    ControlBrokerDaemon restarted{config};
    REQUIRE(restarted.start());
    CHECK(restarted.launch_trusted_host(pending.ticket->inventory_id).status ==
          ControlTrustedHostLaunchStatus::InventoryUnavailable);
    ControlClientConnection after_restart({.endpoint_path = restarted.endpoint_path(),
                                           .expected_broker_executable = broker_executable});
    REQUIRE(after_restart.connect());
    REQUIRE(after_restart.manage("enroll").status_id == "accepted");
    const auto empty_inventory = after_restart.manage("instances");
    REQUIRE(empty_inventory.status_id == "completed");
    CHECK(choc::json::parse(empty_inventory.data_json)["instances"].size() == 0);
    after_restart.disconnect();

    std::ofstream(stop_path) << "stop";
    const auto process = launched.process->wait();
    INFO(process.stderr_output);
    CHECK(process.exit_code == 0);
    restarted.stop();
#else
    SUCCEED("installed daemon host composition is currently macOS-only");
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
