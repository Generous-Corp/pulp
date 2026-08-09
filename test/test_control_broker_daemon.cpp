#include <catch2/catch_test_macros.hpp>

#include "control_broker_daemon.hpp"
#include "support/thread_progress.hpp"

#include <pulp/events/interprocess_connection.hpp>
#include <pulp/inspect/control_artifacts.hpp>
#include <pulp/inspect/control_carrier.hpp>
#include <pulp/inspect/control_client_connection.hpp>
#include <pulp/inspect/control_operations.hpp>
#include <pulp/inspect/control_protocol.hpp>
#include <pulp/platform/child_process.hpp>
#include <pulp/runtime/crypto.hpp>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <choc/text/choc_JSON.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
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
  "capabilities": ["dev.pulp.instance/read@1", "dev.pulp.session/control@1", "dev.pulp.trace/session-control@1"]
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

int wait_for_host_pid(const std::filesystem::path& path) {
    for (unsigned attempt = 0; attempt < 10'000; ++attempt) {
        std::ifstream input(path);
        std::string registration;
        int process_id = -1;
        if (input >> registration >> process_id)
            return process_id;
        std::this_thread::sleep_for(1ms);
    }
    return -1;
}

std::filesystem::path wait_for_host_working_directory(const std::filesystem::path& path) {
    for (unsigned attempt = 0; attempt < 10'000; ++attempt) {
        std::ifstream input(path);
        std::string registration;
        std::string process_id;
        std::string working_directory;
        if (std::getline(input, registration) && std::getline(input, process_id) &&
            std::getline(input, working_directory) && !working_directory.empty()) {
            return working_directory;
        }
        std::this_thread::sleep_for(1ms);
    }
    return {};
}

bool wait_for_path(const std::filesystem::path& path) {
    for (unsigned attempt = 0; attempt < 10'000; ++attempt) {
        if (std::filesystem::exists(path))
            return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

std::optional<pulp::platform::ProcessResult>
wait_for_process_exit(pulp::platform::ChildProcess& child) {
    if (!pulp::test::wait_for_condition([&] { return !child.is_running(); })) {
        child.cancel();
        return std::nullopt;
    }
    return child.wait();
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
    const auto registration_path = root.path / "registration";
    const auto stop_path = root.path / "stop";
    const ControlTrustedHostLaunchIntent allowed_intent{
        .executable = host_executable,
        .arguments = {registration_path.string(), stop_path.string()},
        .working_directory = source,
        .host_tier = ControlHostTier::Standalone,
    };
    ControlBrokerDaemonConfig config{
        .runtime_root = root.runtime,
        .state_root = root.state,
        .sdk_version = "0.795.2-test",
        .executable_path = broker_executable,
        .process_generation = 91,
        .trusted_host_allowlist = {allowed_intent},
        .decide_consent =
            [](const VerifiedControlPeerIdentity&, const ControlGrantRequest&) {
                return ControlConsentDecision{true, ControlConsentAuthority::TrustedHostUi,
                                              "daemon-host-e2e-consent"};
            },
    };
    ControlBrokerDaemon daemon{config};
    REQUIRE(daemon.start());

    ControlClientConnection connection({.endpoint_path = daemon.endpoint_path(),
                                        .expected_broker_executable = broker_executable});
    REQUIRE(connection.connect());
    const auto enrolled = connection.manage("enroll");
    INFO(enrolled.explanation);
    REQUIRE(enrolled.status_id == "accepted");
    const auto enrollment_data = choc::json::parse(enrolled.data_json);
    const auto client_id = std::string(enrollment_data["client_id"].getString());
    REQUIRE_FALSE(client_id.empty());

    auto prepare_params = choc::value::createObject("");
    prepare_params.addMember("executable", choc::value::createString(host_executable.string()));
    auto arguments = choc::value::createEmptyArray();
    arguments.addArrayElement(choc::value::createString(registration_path.string()));
    arguments.addArrayElement(choc::value::createString(stop_path.string()));
    prepare_params.addMember("arguments", arguments);
    prepare_params.addMember("working_directory", choc::value::createString(source.string()));
    prepare_params.addMember("host_tier", choc::value::createString("standalone"));

    const auto untrusted_executable = source / "untrusted-host";
    std::filesystem::copy_file(host_executable, untrusted_executable);
    std::filesystem::copy_file(host_executable.string() + ".inspector-capabilities.json",
                               untrusted_executable.string() +
                                   ".inspector-capabilities.json");
    ::chmod(untrusted_executable.c_str(), 0700);
    ::chmod((untrusted_executable.string() + ".inspector-capabilities.json").c_str(), 0600);
    pulp::platform::ProcessOptions sign_options;
    sign_options.timeout_ms = 5'000;
    pulp::platform::ChildProcess signer;
    REQUIRE(signer.start("/usr/bin/codesign",
                         {"--force", "--sign", "-", "--identifier",
                          "dev.pulp.test.untrusted-host", untrusted_executable.string()},
                         sign_options));
    const auto signed_variant = signer.wait();
    INFO(signed_variant.stderr_output);
    REQUIRE(signed_variant.exit_code == 0);
    auto untrusted_params = choc::value::Value(prepare_params);
    untrusted_params.setMember("executable",
                               choc::value::createString(untrusted_executable.string()));
    const auto untrusted = connection.manage(
        "host-prepare", choc::json::toString(untrusted_params, false));
    CHECK(untrusted.status_id == "invalid_request");

    auto altered_arguments = choc::value::Value(prepare_params);
    auto other_arguments = choc::value::createEmptyArray();
    other_arguments.addArrayElement(choc::value::createString(registration_path.string()));
    other_arguments.addArrayElement(choc::value::createString("--client-chosen"));
    altered_arguments.setMember("arguments", other_arguments);
    CHECK(connection
              .manage("host-prepare", choc::json::toString(altered_arguments, false))
              .status_id == "invalid_request");

    auto altered_working_directory = choc::value::Value(prepare_params);
    altered_working_directory.setMember("working_directory",
                                        choc::value::createString(root.path.string()));
    CHECK(connection
              .manage("host-prepare",
                      choc::json::toString(altered_working_directory, false))
              .status_id == "invalid_request");

    auto altered_tier = choc::value::Value(prepare_params);
    altered_tier.setMember("host_tier", choc::value::createString("offline-job"));
    CHECK(connection.manage("host-prepare", choc::json::toString(altered_tier, false))
              .status_id == "invalid_request");

    const auto allowed_backup = source / "trusted-host-backup";
    REQUIRE(std::filesystem::copy_file(host_executable, allowed_backup));
    REQUIRE(std::filesystem::copy_file(untrusted_executable, host_executable,
                                       std::filesystem::copy_options::overwrite_existing));
    ::chmod(host_executable.c_str(), 0700);
    CHECK(connection.manage("host-prepare", choc::json::toString(prepare_params, false))
              .status_id == "invalid_request");
    REQUIRE(std::filesystem::copy_file(allowed_backup, host_executable,
                                       std::filesystem::copy_options::overwrite_existing));
    ::chmod(host_executable.c_str(), 0700);

    const auto prepared =
        connection.manage("host-prepare", choc::json::toString(prepare_params, false));
    INFO(prepared.explanation);
    REQUIRE(prepared.status_id == "prepared");
    const auto inventory_id =
        std::string(choc::json::parse(prepared.data_json)["inventory_id"].getString());
    REQUIRE_FALSE(inventory_id.empty());
    auto launch_params = choc::value::createObject("");
    launch_params.addMember("inventory_id", choc::value::createString(inventory_id));
    const auto approved_source = root.path / "approved-source";
    REQUIRE_NOTHROW(std::filesystem::rename(source, approved_source));
    REQUIRE(std::filesystem::create_directory(source));
    ::chmod(source.c_str(), 0700);
    const auto launched =
        connection.manage("host-launch", choc::json::toString(launch_params, false));
    INFO(launched.explanation);
    REQUIRE(launched.status_id == "launched");
    const ControlRegistrationId registration_id{wait_for_registration(registration_path)};
    REQUIRE(registration_id);
    CHECK(wait_for_host_working_directory(registration_path) == approved_source);
    REQUIRE(std::filesystem::remove(source));
    REQUIRE_NOTHROW(std::filesystem::rename(approved_source, source));

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

    const auto pending =
        connection.manage("host-prepare", choc::json::toString(prepare_params, false));
    REQUIRE(pending.status_id == "prepared");
    const auto pending_id =
        std::string(choc::json::parse(pending.data_json)["inventory_id"].getString());
    connection.disconnect();
    daemon.stop();

    config.process_generation = 92;
    ControlBrokerDaemon restarted{config};
    REQUIRE(restarted.start());
    ControlClientConnection after_restart({.endpoint_path = restarted.endpoint_path(),
                                           .expected_broker_executable = broker_executable});
    REQUIRE(after_restart.connect());
    REQUIRE(after_restart.manage("enroll").status_id == "accepted");
    auto stale_launch_params = choc::value::createObject("");
    stale_launch_params.addMember("inventory_id", choc::value::createString(pending_id));
    CHECK(after_restart
              .manage("host-launch", choc::json::toString(stale_launch_params, false))
              .status_id == "inventory_unavailable");
    const auto empty_inventory = after_restart.manage("instances");
    REQUIRE(empty_inventory.status_id == "completed");
    CHECK(choc::json::parse(empty_inventory.data_json)["instances"].size() == 0);
    after_restart.disconnect();

    restarted.stop();
#else
    SUCCEED("installed daemon host composition is currently macOS-only");
#endif
}

TEST_CASE("installed daemon process enforces host policy and recovers after crash",
          "[inspect][control][daemon][host][process][restart][security]") {
#ifdef __APPLE__
    DaemonRoot root;
    const auto source = root.path / "process-source";
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

    const std::filesystem::path installed_broker{PULP_CONTROL_BROKER_DAEMON};
    REQUIRE(std::filesystem::exists(installed_broker));

    REQUIRE(std::filesystem::create_directory(root.runtime));
    ::chmod(root.runtime.c_str(), 0700);
    const auto process_runtime = default_control_runtime_directory(root.runtime);
    REQUIRE(std::filesystem::create_directory(process_runtime));
    ::chmod(process_runtime.c_str(), 0700);
    std::ofstream(process_runtime / "broker-observer.sock") << "stale crash residue";
    const auto stale_observer = process_runtime / "bo-0000000000000000.sock";
    const int stale_socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(stale_socket >= 0);
    sockaddr_un stale_address{};
    stale_address.sun_family = AF_UNIX;
    const auto stale_path = stale_observer.string();
    REQUIRE(stale_path.size() < sizeof(stale_address.sun_path));
    std::memcpy(stale_address.sun_path, stale_path.c_str(), stale_path.size() + 1);
    REQUIRE(::bind(stale_socket, reinterpret_cast<const sockaddr*>(&stale_address),
                   sizeof(stale_address)) == 0);
    REQUIRE(::close(stale_socket) == 0);
    REQUIRE(std::filesystem::exists(stale_observer));

    const std::vector<std::string> daemon_arguments{
        "PULP_CONTROL_BROKER_RUNTIME_ROOT=" + root.runtime.string(),
        "PULP_CONTROL_BROKER_STATE_ROOT=" + root.state.string(),
        installed_broker.string(),
    };
    pulp::platform::ProcessOptions daemon_options;
    daemon_options.capture_stdout = true;
    daemon_options.capture_stderr = true;
    pulp::platform::ChildProcess daemon_process;
    REQUIRE(daemon_process.start("/usr/bin/env", daemon_arguments, daemon_options));

    const auto endpoint = default_control_endpoint_path(root.runtime);
    std::unique_ptr<ControlClientConnection> connection;
    for (unsigned attempt = 0; attempt < 10'000 && !connection; ++attempt) {
        auto candidate = std::make_unique<ControlClientConnection>(
            ControlClientConnectionConfig{.endpoint_path = endpoint,
                                          .expected_broker_executable = installed_broker});
        if (candidate->connect())
            connection = std::move(candidate);
        else
            std::this_thread::sleep_for(1ms);
    }
    REQUIRE(connection);
    CHECK_FALSE(std::filesystem::exists(stale_observer));
    CHECK(connection->manage("host-prepare", "{}").status_id == "session-required");
    REQUIRE(connection->manage("enroll").status_id == "accepted");

    const auto registration_path = root.path / "process-registration";
    const auto stop_path = root.path / "process-stop";
    auto prepare_params = choc::value::createObject("");
    prepare_params.addMember("executable", choc::value::createString(host_executable.string()));
    auto arguments = choc::value::createEmptyArray();
    arguments.addArrayElement(choc::value::createString(registration_path.string()));
    arguments.addArrayElement(choc::value::createString(stop_path.string()));
    prepare_params.addMember("arguments", arguments);
    prepare_params.addMember("working_directory", choc::value::createString(source.string()));
    prepare_params.addMember("host_tier", choc::value::createString("standalone"));
    const auto prepared =
        connection->manage("host-prepare", choc::json::toString(prepare_params, false));
    INFO(prepared.explanation);
    REQUIRE(prepared.status_id == "invalid_request");
    connection->disconnect();

    REQUIRE(::kill(daemon_process.process_id(), SIGKILL) == 0);
    const auto crashed = wait_for_process_exit(daemon_process);
    INFO("killed installed broker must exit within the progress deadline");
    REQUIRE(crashed);
    CHECK(crashed->exit_code != 0);

    pulp::platform::ChildProcess restarted_process;
    REQUIRE(restarted_process.start("/usr/bin/env", daemon_arguments, daemon_options));
    std::unique_ptr<ControlClientConnection> restarted;
    for (unsigned attempt = 0; attempt < 10'000 && !restarted; ++attempt) {
        auto candidate = std::make_unique<ControlClientConnection>(
            ControlClientConnectionConfig{.endpoint_path = endpoint,
                                          .expected_broker_executable = installed_broker});
        if (candidate->connect())
            restarted = std::move(candidate);
        else
            std::this_thread::sleep_for(1ms);
    }
    REQUIRE(restarted);
    REQUIRE(restarted->manage("enroll").status_id == "accepted");
    const auto empty_inventory = restarted->manage("instances");
    REQUIRE(empty_inventory.status_id == "completed");
    CHECK(choc::json::parse(empty_inventory.data_json)["instances"].size() == 0);
    restarted->disconnect();

    std::ofstream(stop_path) << "stop";
    restarted_process.cancel();
    const auto stopped = wait_for_process_exit(restarted_process);
    INFO("cancelled installed broker must exit within the progress deadline");
    REQUIRE(stopped);
    INFO(stopped->stderr_output);
#else
    SUCCEED("installed daemon process hosting is currently macOS-only");
#endif
}

TEST_CASE("broker and host SIGKILL fail closed during a deferred operation",
          "[inspect][control][daemon][host][process][restart][crash][artifact][security]") {
#ifdef __APPLE__
    DaemonRoot root;
    const auto source = root.path / "crash-source";
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

    const std::filesystem::path broker_executable{PULP_CONTROL_BROKER_CRASH_FIXTURE};
    REQUIRE(std::filesystem::exists(broker_executable));
    const auto registration_path = root.path / "crash-registration";
    const auto stop_path = root.path / "crash-stop";
    const auto deferred_path = root.path / "deferred-active";
    const std::vector<std::string> daemon_arguments{
        "PULP_CONTROL_BROKER_RUNTIME_ROOT=" + root.runtime.string(),
        "PULP_CONTROL_BROKER_STATE_ROOT=" + root.state.string(),
        "PULP_CONTROL_TEST_TRUSTED_HOST_EXECUTABLE=" + host_executable.string(),
        "PULP_CONTROL_TEST_TRUSTED_HOST_WORKING_DIRECTORY=" + source.string(),
        "PULP_CONTROL_TEST_TRUSTED_HOST_REGISTRATION=" + registration_path.string(),
        "PULP_CONTROL_TEST_TRUSTED_HOST_STOP=" + stop_path.string(),
        "PULP_CONTROL_TEST_TRUSTED_HOST_DEFERRED=" + deferred_path.string(),
        broker_executable.string(),
    };
    pulp::platform::ProcessOptions daemon_options;
    daemon_options.capture_stdout = true;
    daemon_options.capture_stderr = true;
    pulp::platform::ChildProcess daemon_process;
    REQUIRE(daemon_process.start("/usr/bin/env", daemon_arguments, daemon_options));

    const auto endpoint = default_control_endpoint_path(root.runtime);
    std::unique_ptr<ControlClientConnection> connection;
    for (unsigned attempt = 0; attempt < 10'000 && !connection; ++attempt) {
        auto candidate = std::make_unique<ControlClientConnection>(
            ControlClientConnectionConfig{.endpoint_path = endpoint,
                                          .expected_broker_executable = broker_executable});
        if (candidate->connect())
            connection = std::move(candidate);
        else
            std::this_thread::sleep_for(1ms);
    }
    REQUIRE(connection);
    const auto enrolled = connection->manage("enroll");
    INFO(enrolled.explanation);
    REQUIRE(enrolled.status_id == "accepted");
    const auto client_id =
        std::string(choc::json::parse(enrolled.data_json)["client_id"].getString());
    REQUIRE_FALSE(client_id.empty());

    auto prepare_params = choc::value::createObject("");
    prepare_params.addMember("executable", choc::value::createString(host_executable.string()));
    auto arguments = choc::value::createEmptyArray();
    arguments.addArrayElement(choc::value::createString(registration_path.string()));
    arguments.addArrayElement(choc::value::createString(stop_path.string()));
    arguments.addArrayElement(choc::value::createString(deferred_path.string()));
    prepare_params.addMember("arguments", arguments);
    prepare_params.addMember("working_directory", choc::value::createString(source.string()));
    prepare_params.addMember("host_tier", choc::value::createString("standalone"));
    const auto prepared =
        connection->manage("host-prepare", choc::json::toString(prepare_params, false));
    INFO(prepared.explanation);
    REQUIRE(prepared.status_id == "prepared");
    const auto inventory_id =
        std::string(choc::json::parse(prepared.data_json)["inventory_id"].getString());
    auto launch_params = choc::value::createObject("");
    launch_params.addMember("inventory_id", choc::value::createString(inventory_id));
    const auto launched =
        connection->manage("host-launch", choc::json::toString(launch_params, false));
    INFO(launched.explanation);
    REQUIRE(launched.status_id == "launched");
    const ControlRegistrationId registration_id{wait_for_registration(registration_path)};
    REQUIRE(registration_id);
    const int host_process_id = wait_for_host_pid(registration_path);
    REQUIRE(host_process_id > 0);

    const auto inventory = connection->manage("instances");
    REQUIRE(inventory.status_id == "completed");
    const auto inventory_data = choc::json::parse(inventory.data_json);
    const auto instances = inventory_data["instances"];
    REQUIRE(instances.size() == 1);
    const auto instance_id = std::string(instances[0]["instance_id"].getString());
    const auto publication_id = std::string(instances[0]["publication_id"].getString());
    auto grant_params = choc::value::createObject("");
    grant_params.addMember("instance_id", choc::value::createString(instance_id));
    grant_params.addMember("profile", choc::value::createString("develop"));
    const auto granted =
        connection->manage("grant-request", choc::json::toString(grant_params, false));
    INFO(granted.explanation);
    REQUIRE(granted.status_id == "granted");
    const auto grant_id =
        std::string(choc::json::parse(granted.data_json)["grant_id"].getString());

    ControlClient client(*connection);
    REQUIRE(client.negotiate({.mandatory_features = {"receipts"}}).succeeded());
    ControlRequestEnvelope request{
        .request_id = "phase15-deferred-request",
        .client_id = client_id,
        .registration_id = registration_id.value,
        .grant_id = grant_id,
        .instance_generation = publication_id,
        .operation_id = "dev.pulp.trace/session-control@1",
        .operation_version = 1,
        .idempotency_key = "phase15-deferred-idempotency",
        .deadline_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                (std::chrono::system_clock::now() + 1s).time_since_epoch())
                                .count(),
        .params_json = R"({"action":"start","ring_mb":9})",
    };
    request.request_hash = *control_request_hash(request);
    const auto deferred = client.request(request, 3s);
    INFO(deferred.error_code);
    INFO(deferred.explanation);
    REQUIRE(deferred.succeeded());
    REQUIRE(deferred.response);
    CHECK(deferred.response->state == ControlReceiptState::UnknownNeedsRefresh);
    const auto receipt_id = deferred.response->receipt_id;
    REQUIRE_FALSE(receipt_id.empty());
    REQUIRE(wait_for_path(deferred_path));

    const auto pending =
        connection->manage("host-prepare", choc::json::toString(prepare_params, false));
    REQUIRE(pending.status_id == "prepared");
    const auto pending_inventory_id =
        std::string(choc::json::parse(pending.data_json)["inventory_id"].getString());

    const auto artifact_root = root.state / "artifacts";
    const auto orphan = artifact_root / "blobs" / (std::string(64, 'c') + ".blob");
    const auto partial = artifact_root / "artifacts" / ".private-publish-phase15-crash";
    {
        std::ofstream(orphan, std::ios::binary) << "orphaned-secret-payload";
        std::ofstream(partial, std::ios::binary) << "partial-secret-payload";
    }
    ::chmod(orphan.c_str(), 0600);
    ::chmod(partial.c_str(), 0600);
    REQUIRE(std::filesystem::exists(orphan));
    REQUIRE(std::filesystem::exists(partial));

    REQUIRE(::kill(daemon_process.process_id(), SIGKILL) == 0);
    const auto crashed = wait_for_process_exit(daemon_process);
    INFO("killed crash fixture must exit within the progress deadline");
    REQUIRE(crashed);
    CHECK(crashed->exit_code != 0);
    for (unsigned attempt = 0; attempt < 2'000 && connection->is_connected(); ++attempt)
        std::this_thread::sleep_for(1ms);
    CHECK_FALSE(connection->is_connected());
    CHECK_FALSE(connection->is_session_open());

    REQUIRE(::kill(host_process_id, SIGKILL) == 0);
    for (unsigned attempt = 0; attempt < 2'000 && ::kill(host_process_id, 0) == 0; ++attempt)
        std::this_thread::sleep_for(1ms);
    CHECK(::kill(host_process_id, 0) != 0);

    pulp::platform::ChildProcess restarted_process;
    REQUIRE(restarted_process.start("/usr/bin/env", daemon_arguments, daemon_options));
    std::unique_ptr<ControlClientConnection> restarted;
    for (unsigned attempt = 0; attempt < 10'000 && !restarted; ++attempt) {
        auto candidate = std::make_unique<ControlClientConnection>(
            ControlClientConnectionConfig{.endpoint_path = endpoint,
                                          .expected_broker_executable = broker_executable});
        if (candidate->connect())
            restarted = std::move(candidate);
        else
            std::this_thread::sleep_for(1ms);
    }
    REQUIRE(restarted);
    const auto restarted_enrollment = restarted->manage("enroll");
    REQUIRE(restarted_enrollment.status_id == "accepted");
    const auto restarted_client_id = std::string(
        choc::json::parse(restarted_enrollment.data_json)["client_id"].getString());
    REQUIRE_FALSE(restarted_client_id.empty());
    const auto empty_inventory = restarted->manage("instances");
    REQUIRE(empty_inventory.status_id == "completed");
    CHECK(choc::json::parse(empty_inventory.data_json)["instances"].size() == 0);
    auto stale_launch = choc::value::createObject("");
    stale_launch.addMember("inventory_id", choc::value::createString(pending_inventory_id));
    CHECK(restarted->manage("host-launch", choc::json::toString(stale_launch, false)).status_id ==
          "inventory_unavailable");

    ControlClient restarted_client(*restarted);
    REQUIRE(restarted_client.negotiate({.mandatory_features = {"receipts"}}).succeeded());
    auto stale_session_request = request;
    stale_session_request.deadline_unix_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            (std::chrono::system_clock::now() + 5s).time_since_epoch())
            .count();
    stale_session_request.request_hash = *control_request_hash(stale_session_request);
    const auto stale_session = restarted_client.request(stale_session_request, 2s);
    CHECK_FALSE(stale_session.succeeded());
    CHECK(stale_session.error_code == "admission-denied");

    auto stale_grant_request = stale_session_request;
    stale_grant_request.request_id = "phase15-stale-grant-request";
    stale_grant_request.idempotency_key = "phase15-stale-grant-idempotency";
    stale_grant_request.client_id = restarted_client_id;
    stale_grant_request.request_hash = *control_request_hash(stale_grant_request);
    const auto stale_grant = restarted_client.request(stale_grant_request, 2s);
    CHECK_FALSE(stale_grant.succeeded());
    CHECK(stale_grant.error_code == "admission-denied");
    CHECK_FALSE(std::filesystem::exists(orphan));
    CHECK_FALSE(std::filesystem::exists(partial));
    restarted->disconnect();

    restarted_process.cancel();
    const auto stopped = wait_for_process_exit(restarted_process);
    INFO("cancelled crash fixture must exit within the progress deadline");
    REQUIRE(stopped);
    INFO(stopped->stderr_output);
    ControlOperationStore store{{.directory = root.state / "operations"}};
    REQUIRE(store.open().succeeded());
    const auto recovered = store.receipt(ControlReceiptId{receipt_id});
    REQUIRE(recovered);
    CHECK(recovered->state == ControlReceiptState::UnknownNeedsRefresh);
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
