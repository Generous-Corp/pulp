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
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
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
  "registry_digest": "1ef00512c588766b7ec414c2f4bf1b2572e115b2e9be83ea61cc35ee434ad086",
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

pulp::platform::ProcessResult run_installed_client(
    const std::filesystem::path& executable, const std::filesystem::path& runtime,
    std::vector<std::string> arguments,
    std::optional<std::string> standard_input = std::nullopt) {
    std::vector<std::string> command{"TMPDIR=" + runtime.string(), executable.string()};
    command.insert(command.end(), std::make_move_iterator(arguments.begin()),
                   std::make_move_iterator(arguments.end()));
    pulp::platform::ProcessOptions options;
    options.timeout_ms = 15'000;
    options.capture_stdout = true;
    options.capture_stderr = true;
    if (!standard_input)
        return pulp::platform::ChildProcess::run("/usr/bin/env", command, options);
    pulp::platform::ChildProcess process;
    const std::vector<std::uint8_t> bytes(standard_input->begin(), standard_input->end());
    if (!process.start_with_standard_input("/usr/bin/env", command, bytes, options))
        return {};
    return process.wait();
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
            [](const ControlGrantConsentRequest&) {
                return ControlConsentDecision{true, ControlConsentAuthority::TrustedHostUi,
                                              "daemon-host-e2e-consent", {}};
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
    const auto signed_variant = signer.wait(); // unbounded-wait: allow codesign is capped by timeout_ms
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
        connection.manage("host-launch", choc::json::toString(launch_params, false), 15s);
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
    REQUIRE(prepared.status_id == "unavailable");
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

TEST_CASE("installed broker launches only its named ordinary Standalone host",
          "[inspect][control][daemon][host][process][standalone][security][author-catalog-process]") {
#ifdef __APPLE__
    DaemonRoot root;
    const auto* author_broker_environment = std::getenv("PULP_CONTROL_AUTHOR_BROKER");
    const auto* author_host_environment = std::getenv("PULP_CONTROL_AUTHOR_HOST");
    const auto installed_broker = author_broker_environment
                                      ? std::filesystem::path{author_broker_environment}
                                      : std::filesystem::path{PULP_CONTROL_BROKER_DAEMON};
    const auto installed_host = author_host_environment
                                    ? std::filesystem::path{author_host_environment}
                                    : installed_broker.parent_path() /
                                          "pulp-control-standalone-host";
    const auto host_id = author_host_environment
                             ? std::string{"dev-pulp-installed-control-standalone-18f9d0d67fc6aec8"}
                                                  : std::string{"ordinary-standalone"};
    const auto expected_plugin = author_host_environment
                                     ? std::string_view{"dev.pulp.installed-control-standalone"}
                                     : std::string_view{"dev.pulp.control-standalone-host"};
    REQUIRE(std::filesystem::is_regular_file(installed_broker));
    REQUIRE(std::filesystem::is_regular_file(installed_host));
    REQUIRE(std::filesystem::is_regular_file(
        installed_host.string() + ".inspector-capabilities.json"));

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
    REQUIRE(connection->manage("enroll").status_id == "accepted");

    auto client_chosen = choc::value::createObject("");
    client_chosen.addMember("executable", choc::value::createString("/tmp/untrusted"));
    client_chosen.addMember("arguments", choc::value::createEmptyArray());
    client_chosen.addMember("working_directory", choc::value::createString("/tmp"));
    client_chosen.addMember("host_tier", choc::value::createString("standalone"));
    CHECK(connection
              ->manage("host-prepare", choc::json::toString(client_chosen, false))
              .status_id == "unavailable");

    auto unknown = choc::value::createObject("");
    unknown.addMember("host_id", choc::value::createString("client-selected"));
    CHECK(connection
              ->manage("host-prepare-installed", choc::json::toString(unknown, false))
              .status_id == "invalid_request");

    auto named = choc::value::createObject("");
    named.addMember("host_id", choc::value::createString(host_id));
    const auto prepared = connection->manage(
        "host-prepare-installed", choc::json::toString(named, false));
    INFO(prepared.explanation);
    REQUIRE(prepared.status_id == "prepared");
    const auto inventory_id = std::string(
        choc::json::parse(prepared.data_json)["inventory_id"].getString());
    REQUIRE_FALSE(inventory_id.empty());

    auto launch = choc::value::createObject("");
    launch.addMember("inventory_id", choc::value::createString(inventory_id));
    const auto launched =
        connection->manage("host-launch", choc::json::toString(launch, false), 15s);
    INFO(launched.explanation);
    REQUIRE(launched.status_id == "launched");

    choc::value::Value instances;
    for (unsigned attempt = 0; attempt < 10'000; ++attempt) {
        const auto result = connection->manage("instances");
        REQUIRE(result.status_id == "completed");
        instances = choc::json::parse(result.data_json)["instances"];
        if (instances.size() == 1)
            break;
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(instances.size() == 1);
    const auto instance = instances[0];
    CHECK(instance["plugin_id"].getString() == expected_plugin);
    if (author_host_environment) {
        CHECK(instance["capabilities"].size() == 10);
        const auto encoded = choc::json::toString(instance["capabilities"], false);
        for (const auto capability : {
                 "dev.pulp.instance/read@1", "dev.pulp.session/control@1",
                 "dev.pulp.state/read@1", "dev.pulp.ui/capture@1",
                 "dev.pulp.ui/input@1", "dev.pulp.trace/control@1",
                 "dev.pulp.trace/session-control@1",
                 "dev.pulp.state/parameter-gesture@1",
                 "dev.pulp.telemetry/subscribe@1", "dev.pulp.runtime/evaluate@1"})
            CHECK(encoded.find(capability) != std::string::npos);
    } else {
        REQUIRE(instance["capabilities"].size() == 2);
    }

    if (author_host_environment) {
        const auto version_directory = installed_host.parent_path();
        const auto catalog_entry = version_directory.parent_path();
        const auto active = catalog_entry / "active";
        const auto inactive = catalog_entry / ".active.test-removed";
        std::error_code rename_error;
        std::filesystem::rename(active, inactive, rename_error);
        INFO(rename_error.message());
        REQUIRE_FALSE(rename_error);

        bool disconnected_for_reload = false;
        for (unsigned attempt = 0; attempt < 5'000; ++attempt) {
            const auto result = connection->manage("instances");
            if (result.status_id != "completed") {
                disconnected_for_reload = true;
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
        CHECK(disconnected_for_reload);
        connection->disconnect();
        connection.reset();
        for (unsigned attempt = 0; attempt < 10'000 && !connection; ++attempt) {
            auto candidate = std::make_unique<ControlClientConnection>(
                ControlClientConnectionConfig{.endpoint_path = endpoint,
                                              .expected_broker_executable = installed_broker});
            if (candidate->connect() &&
                candidate->manage("enroll").status_id == "accepted")
                connection = std::move(candidate);
            else
                std::this_thread::sleep_for(1ms);
        }
        REQUIRE(connection);
        CHECK(connection
                  ->manage("host-prepare-installed", choc::json::toString(named, false))
                  .status_id == "invalid_request");

        std::filesystem::rename(inactive, active, rename_error);
        INFO(rename_error.message());
        REQUIRE_FALSE(rename_error);
        bool disconnected_for_restore = false;
        for (unsigned attempt = 0; attempt < 5'000; ++attempt) {
            const auto result = connection->manage("instances");
            if (result.status_id != "completed") {
                disconnected_for_restore = true;
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
        CHECK(disconnected_for_restore);
        connection->disconnect();
        connection.reset();
        for (unsigned attempt = 0; attempt < 10'000 && !connection; ++attempt) {
            auto candidate = std::make_unique<ControlClientConnection>(
                ControlClientConnectionConfig{.endpoint_path = endpoint,
                                              .expected_broker_executable = installed_broker});
            if (candidate->connect() &&
                candidate->manage("enroll").status_id == "accepted")
                connection = std::move(candidate);
            else
                std::this_thread::sleep_for(1ms);
        }
        REQUIRE(connection);
        const auto restored_prepared = connection->manage(
            "host-prepare-installed", choc::json::toString(named, false));
        INFO(restored_prepared.explanation);
        REQUIRE(restored_prepared.status_id == "prepared");
        auto restored_launch = choc::value::createObject("");
        restored_launch.addMember(
            "inventory_id",
            choc::value::createString(std::string(
                choc::json::parse(restored_prepared.data_json)["inventory_id"].getString())));
            REQUIRE(connection
                    ->manage("host-launch", choc::json::toString(restored_launch, false), 15s)
                    .status_id == "launched");
        choc::value::Value restored_instances;
        for (unsigned attempt = 0; attempt < 10'000; ++attempt) {
            const auto result = connection->manage("instances");
            REQUIRE(result.status_id == "completed");
            restored_instances = choc::json::parse(result.data_json)["instances"];
            if (restored_instances.size() == 1)
                break;
            std::this_thread::sleep_for(1ms);
        }
        if (restored_instances.size() != 1) {
            daemon_process.cancel();
            const auto failed_daemon = wait_for_process_exit(daemon_process);
            REQUIRE(failed_daemon);
            INFO(failed_daemon->stdout_output);
            INFO(failed_daemon->stderr_output);
        }
        REQUIRE(restored_instances.size() == 1);
        CHECK(restored_instances[0]["plugin_id"].getString() == expected_plugin);
    }

    connection->disconnect();
    daemon_process.cancel();
    const auto stopped = wait_for_process_exit(daemon_process);
    INFO("cancelled installed broker must exit within the progress deadline");
    REQUIRE(stopped);
    INFO(stopped->stderr_output);
#else
    SUCCEED("installed ordinary Standalone hosting is currently macOS-only");
#endif
}

TEST_CASE("installed SDK ordinary author Standalone full parity aggregate",
          "[inspect][control][daemon][host][standalone][e2e][aggregate][installed-author-full-parity][author-catalog]") {
#ifdef __APPLE__
    DaemonRoot root;
    const auto broker_executable = current_executable();
    const std::filesystem::path installed_broker{PULP_CONTROL_BROKER_DAEMON};
    const auto* author_host_environment = std::getenv("PULP_CONTROL_AUTHOR_HOST");
    const auto installed_host = author_host_environment
                                    ? std::filesystem::path{author_host_environment}
                                    : installed_broker.parent_path() /
                                          "pulp-control-standalone-host";
    const auto host_id = author_host_environment
                             ? std::string{"dev-pulp-installed-control-standalone-18f9d0d67fc6aec8"}
                                                  : std::string{"ordinary-standalone"};
    const auto expected_parameter = author_host_environment ? std::string_view{"Author Level"}
                                                             : std::string_view{"Level"};
    REQUIRE_FALSE(broker_executable.empty());
    REQUIRE(std::filesystem::is_regular_file(installed_host));

    std::atomic<std::uint64_t> consent_sequence{0};
    ControlBrokerDaemon daemon({
        .runtime_root = root.runtime,
        .state_root = root.state,
        .sdk_version = "0.798.0-test",
        .executable_path = broker_executable,
        .process_generation = 211,
        .installed_host_selections =
            {{.host_id = host_id,
              .intent = {.executable = installed_host,
                         .arguments = {},
                         .working_directory = installed_host.parent_path(),
                         .host_tier = ControlHostTier::Standalone}}},
        .decide_consent =
            [&consent_sequence](const ControlGrantConsentRequest& consent_request) {
                if (std::ranges::find(consent_request.grant.capabilities,
                                      InspectorCapability::RuntimeEval) !=
                    consent_request.grant.capabilities.end()) {
                    return ControlConsentDecision{
                        true, ControlConsentAuthority::TrustedHostUi,
                        "standalone-runtime-eval-single-use-consent", {}};
                }
                return ControlConsentDecision{true, ControlConsentAuthority::TrustedHostUi,
                                              "standalone-state-read-test-consent-" +
                                                  std::to_string(consent_sequence.fetch_add(
                                                      1, std::memory_order_relaxed)),
                                              {}};
            },
    });
    REQUIRE(daemon.start());
    ControlClientConnection connection({.endpoint_path = daemon.endpoint_path(),
                                        .expected_broker_executable = broker_executable});
    REQUIRE(connection.connect());
    const auto enrolled = connection.manage("enroll");
    REQUIRE(enrolled.status_id == "accepted");
    const auto client_id = std::string(
        choc::json::parse(enrolled.data_json)["client_id"].getString());

    auto named = choc::value::createObject("");
    named.addMember("host_id", choc::value::createString(host_id));
    const auto prepared = connection.manage(
        "host-prepare-installed", choc::json::toString(named, false));
    INFO(prepared.explanation);
    REQUIRE(prepared.status_id == "prepared");
    auto launch = choc::value::createObject("");
    launch.addMember(
        "inventory_id",
        choc::value::createString(std::string(
            choc::json::parse(prepared.data_json)["inventory_id"].getString())));
    REQUIRE(connection.manage("host-launch", choc::json::toString(launch, false), 15s)
                .status_id == "launched");

    std::string instance_id;
    std::string registration_id;
    std::string publication_id;
    for (unsigned attempt = 0; attempt < 10'000; ++attempt) {
        const auto inventory = connection.manage("instances");
        REQUIRE(inventory.status_id == "completed");
        const auto inventory_data = choc::json::parse(inventory.data_json);
        const auto instances = inventory_data["instances"];
        if (instances.size() == 1) {
            instance_id = std::string(instances[0]["instance_id"].getString());
            registration_id =
                std::string(instances[0]["registration_id"].getString());
            publication_id =
                std::string(instances[0]["publication_id"].getString());
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE_FALSE(instance_id.empty());
    auto grant = choc::value::createObject("");
    grant.addMember("instance_id", choc::value::createString(instance_id));
    grant.addMember("profile", choc::value::createString("develop"));
    const auto granted =
        connection.manage("grant-request", choc::json::toString(grant, false));
    INFO(granted.explanation);
    REQUIRE(granted.status_id == "granted");

    ControlClient client(connection);
    REQUIRE(client.negotiate({.mandatory_features = {"receipts"},
                              .optional_features = {"artifacts"}}).succeeded());
    ControlRequestEnvelope request{
        .request_id = "ordinary-standalone-state-read",
        .client_id = client_id,
        .registration_id = registration_id,
        .grant_id = std::string(
            choc::json::parse(granted.data_json)["grant_id"].getString()),
        .instance_generation = publication_id,
        .operation_id = "dev.pulp.state/read@1",
        .operation_version = 1,
        .idempotency_key = "ordinary-standalone-state-read-key",
        .deadline_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                (std::chrono::system_clock::now() + 5s).time_since_epoch())
                                .count(),
        .params_json = R"({"include_catalog":true})",
    };
    request.request_hash = *control_request_hash(request);
    INFO("dispatching installed Standalone state read");
    const auto result = client.request(request, 5s);
    INFO(result.error_code);
    INFO(result.explanation);
    REQUIRE(result.succeeded());
    REQUIRE(result.response);
    REQUIRE(result.response->state == ControlReceiptState::Completed);
    const auto detail = choc::json::parse(result.response->detail_json);
    INFO("decoded installed Standalone state read");
    REQUIRE(detail["parameters"].size() == 1);
    CHECK(detail["parameters"][0]["name"].getString() == expected_parameter);

    if (author_host_environment) {
        std::uint64_t sequence = 0;
        const auto invoke = [&](std::string operation, std::string params,
                                std::optional<std::uint64_t> expected_generation = std::nullopt,
                                std::optional<std::string> selected_grant = std::nullopt) {
            ControlRequestEnvelope next{
                .request_id = "installed-parity-" + std::to_string(++sequence),
                .client_id = client_id,
                .registration_id = registration_id,
                .grant_id = selected_grant.value_or(request.grant_id),
                .instance_generation = publication_id,
                .operation_id = std::move(operation),
                .operation_version = 1,
                .idempotency_key = "installed-parity-key-" + std::to_string(sequence),
                .deadline_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        (std::chrono::system_clock::now() + 5s)
                                            .time_since_epoch())
                                        .count(),
                .params_json = std::move(params),
            };
            next.expected_state_generation = expected_generation.value_or(0);
            next.request_hash = *control_request_hash(next);
            return client.request(next, 5s);
        };

        const auto initial_generation =
            static_cast<std::uint64_t>(detail["state_generation"].getInt64());
        const auto denied_without_controller = invoke(
            "dev.pulp.state/parameter-gesture@1",
            R"({"idempotency_key":"missing-controller","normalized_value":0.5,"parameter_id":1})",
            initial_generation);
        REQUIRE(denied_without_controller.response);
        CHECK(denied_without_controller.response->result_code ==
              ControlResultCode::LeaseConflict);

        const auto acquired_controller =
            invoke("dev.pulp.session/control@1", R"({"action":"acquire"})");
        REQUIRE(acquired_controller.succeeded());
        auto controller_lease_id = std::string(
            choc::json::parse(acquired_controller.response->detail_json)["lease_id"].getString());
        REQUIRE_FALSE(controller_lease_id.empty());
        const auto renewed_controller =
            invoke("dev.pulp.session/control@1", R"({"action":"renew"})");
        REQUIRE(renewed_controller.succeeded());
        CHECK(choc::json::parse(renewed_controller.response->detail_json)["lease_id"].getString() ==
              controller_lease_id);

        const auto gesture = invoke(
            "dev.pulp.state/parameter-gesture@1",
            R"({"idempotency_key":"installed-gesture","normalized_value":0.75,"parameter_id":1})",
            initial_generation);
        INFO(gesture.explanation);
        REQUIRE(gesture.succeeded());
        REQUIRE(gesture.response->state == ControlReceiptState::Completed);
        CHECK(choc::json::parse(gesture.response->detail_json)["applied"].getBool());
        const auto after_gesture = invoke("dev.pulp.state/read@1", R"({"include_catalog":true})");
        REQUIRE(after_gesture.succeeded());
        CHECK(choc::json::parse(after_gesture.response->detail_json)["parameters"][0]["normalized"]
                  .getFloat64() == 0.75);

        const auto* cli_environment = std::getenv("PULP_CONTROL_AUTHOR_CLI");
        const auto* mcp_environment = std::getenv("PULP_CONTROL_AUTHOR_MCP");
        REQUIRE(cli_environment);
        REQUIRE(mcp_environment);
        const auto cli_read = run_installed_client(
            cli_environment, root.runtime,
            {"control", "call", "--instance", instance_id, "dev.pulp.state/read@1",
             "--params", "{}", "--json"});
        INFO(cli_read.stdout_output);
        INFO(cli_read.stderr_output);
        REQUIRE(cli_read.exit_code == 0);
        const auto cli_receipt = choc::json::parse(cli_read.stdout_output);
        const auto cli_detail = cli_receipt["detail"];
        const auto mcp_arguments =
            std::string{"{\"instance_id\":"} +
            choc::json::getEscapedQuotedString(instance_id) +
            R"(,"request_id":"installed-parity-mcp-read","input":{}})";
        const auto mcp_read = run_installed_client(
            mcp_environment, root.runtime, {},
            std::string{"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                        "\"params\":{\"name\":\"pulp_control_state_read\",\"arguments\":"} +
                mcp_arguments + "}}\n");
        INFO(mcp_read.stdout_output);
        INFO(mcp_read.stderr_output);
        REQUIRE(mcp_read.exit_code == 0);
        const auto mcp_line = choc::json::parse(
            std::string_view{mcp_read.stdout_output}.substr(
                0, mcp_read.stdout_output.find('\n')));
        REQUIRE_FALSE(mcp_line["result"]["isError"].getWithDefault<bool>(false));
        const auto mcp_detail = mcp_line["result"]["structuredContent"]["result"];
        CHECK(choc::json::toString(cli_detail, false) ==
              choc::json::toString(mcp_detail, false));

        const auto window_capture =
            invoke("dev.pulp.ui/capture@1", R"({"target":"window","format":"png"})");
        INFO(window_capture.explanation);
        REQUIRE(window_capture.succeeded());
        const auto& window_receipt = *window_capture.response;
        INFO("window capture state: " << control_receipt_state_id(window_receipt.state));
        if (window_receipt.result_code)
            INFO("window capture code: " << control_result_code_id(*window_receipt.result_code));
        INFO("window capture explanation: " << window_receipt.explanation);
        INFO("window capture detail: " << window_receipt.detail_json);
        REQUIRE(window_receipt.state == ControlReceiptState::Completed);
        REQUIRE(window_receipt.artifacts.size() == 1);
        const auto artifact = client.read_artifact(
            window_receipt.artifacts[0].artifact_id, 0, 1024 * 1024, 5s);
        CHECK(artifact.status == ControlArtifactStatus::Read);
        CHECK_FALSE(artifact.bytes.empty());
        const auto node_capture = invoke(
            "dev.pulp.ui/capture@1",
            "{\"target\":\"node\",\"format\":\"png\",\"node_id\":\"author-input\","
            "\"view_generation\":" + choc::json::getEscapedQuotedString(publication_id) + "}");
        REQUIRE(node_capture.succeeded());
        const auto& node_receipt = *node_capture.response;
        INFO("node capture state: " << control_receipt_state_id(node_receipt.state));
        INFO("node capture explanation: " << node_receipt.explanation);
        INFO("node capture detail: " << node_receipt.detail_json);
        REQUIRE(node_receipt.state == ControlReceiptState::Completed);
        REQUIRE(node_receipt.artifacts.size() == 1);

        const auto input = [&](std::string body) {
            const auto result = invoke("dev.pulp.ui/input@1", std::move(body));
            INFO(result.explanation);
            REQUIRE(result.succeeded());
            REQUIRE(result.response->state == ControlReceiptState::Completed);
        };
        const auto target_prefix =
            "{\"target_id\":\"author-input\",\"view_generation\":" +
            choc::json::getEscapedQuotedString(publication_id);
        input(target_prefix +
              R"(,"kind":"pointer","event":{"phase":"down","x":30,"y":30,"button":0}})");
        input(target_prefix + R"(,"kind":"focus","event":{"focused":true}})");
        input(target_prefix +
              R"(,"kind":"keyboard","event":{"phase":"down","key":"a","repeat":false}})");
        input(target_prefix + R"(,"kind":"text","event":{"text":"bounded"}})");
        input(target_prefix +
              R"(,"kind":"pointer","event":{"phase":"up","x":30,"y":30,"button":0}})");

        const auto motion_start = invoke(
            "dev.pulp.trace/control@1",
            R"({"action":"motion-start-trace","metrics":[{"kind":"geometry","node_id":"author-input"},{"kind":"scroll-geometry","node_id":"author-scroll","properties":["contentOffsetY"]}]})");
        REQUIRE(motion_start.succeeded());
        const auto motion_trace_id =
            choc::json::parse(motion_start.response->detail_json)["trace_id"].getInt64();
        CHECK(motion_trace_id > 0);
        for (const auto action : {R"({"action":"motion-play","maximum_events":2})",
                                  R"({"action":"motion-pause"})",
                                  R"({"action":"motion-scrub-to","frame":1})",
                                  R"({"action":"motion-enable-cost"})"}) {
            const auto outcome = invoke("dev.pulp.trace/control@1", action);
            INFO(action);
            INFO(outcome.explanation);
            REQUIRE(outcome.succeeded());
        }
        input(target_prefix +
              R"(,"kind":"pointer","event":{"phase":"down","x":30,"y":30,"button":0}})");
        input(target_prefix +
              R"(,"kind":"pointer","event":{"phase":"up","x":30,"y":30,"button":0}})");
        const auto sampled_cost = invoke(
            "dev.pulp.trace/control@1",
            R"({"action":"motion-sample-cost","maximum_samples":1})");
        REQUIRE(sampled_cost.succeeded());
        const auto cost_detail = choc::json::parse(sampled_cost.response->detail_json);
        REQUIRE(cost_detail["samples"].size() == 1);
        CHECK(std::isfinite(
            cost_detail["samples"][0]["render_pass_duration_ms"].getWithDefault<double>(
                std::numeric_limits<double>::quiet_NaN())));
        CHECK(std::isfinite(
            cost_detail["samples"][0]["dirty_rect_area_px"].getWithDefault<double>(
                std::numeric_limits<double>::quiet_NaN())));
        const auto motion_stop = invoke(
            "dev.pulp.trace/control@1",
            "{\"action\":\"motion-stop-trace\",\"trace_id\":" +
                std::to_string(motion_trace_id) + "}");
        REQUIRE(motion_stop.succeeded());

        const auto trace_start = invoke("dev.pulp.trace/session-control@1",
                                        R"({"action":"start","ring_mb":9})");
        REQUIRE(trace_start.succeeded());
        const auto trace_stop = invoke("dev.pulp.trace/session-control@1",
                                       R"({"action":"stop"})");
        REQUIRE(trace_stop.succeeded());

        const auto subscribed = invoke(
            "dev.pulp.telemetry/subscribe@1",
            R"({"action":"subscribe","channel_ids":["author-level"],"max_hz":15,"buffer_samples":2})");
        REQUIRE(subscribed.succeeded());
        const auto stream_id = std::string(
            choc::json::parse(subscribed.response->detail_json)["stream_id"].getString());
        std::this_thread::sleep_for(400ms);
        const auto polled = invoke(
            "dev.pulp.telemetry/subscribe@1",
            "{\"action\":\"poll\",\"stream_id\":" +
                choc::json::getEscapedQuotedString(stream_id) + "}");
        REQUIRE(polled.succeeded());
        const auto telemetry = choc::json::parse(polled.response->detail_json);
        CHECK(telemetry["available"].getBool());
        CHECK(telemetry["samples"].size() <= 2);
        CHECK(telemetry["dropped"].getWithDefault<std::int64_t>(-1) > 0);

        auto eval_grant_request = choc::value::createObject("");
        eval_grant_request.addMember("instance_id", choc::value::createString(instance_id));
        eval_grant_request.addMember(
            "operation_id", choc::value::createString("dev.pulp.runtime/evaluate@1"));
        const auto eval_granted = connection.manage(
            "grant-request", choc::json::toString(eval_grant_request, false));
        REQUIRE(eval_granted.status_id == "granted");
        const auto eval_grant_id = std::string(
            choc::json::parse(eval_granted.data_json)["grant_id"].getString());
        const auto refreshed_controller =
            invoke("dev.pulp.session/control@1", R"({"action":"acquire"})");
        REQUIRE(refreshed_controller.succeeded());
        REQUIRE(refreshed_controller.response->state == ControlReceiptState::Completed);
        controller_lease_id = std::string(
            choc::json::parse(refreshed_controller.response->detail_json)["lease_id"].getString());
        const auto evaluated = invoke(
            "dev.pulp.runtime/evaluate@1",
            R"json({"source":"({ answer: 42 })","timeout_ms":1000,"idempotency_key":"installed-eval"})json",
            std::nullopt, eval_grant_id);
        INFO(evaluated.explanation);
        REQUIRE(evaluated.succeeded());
        INFO(evaluated.response->explanation);
        INFO(evaluated.response->detail_json);
        REQUIRE(evaluated.response->state == ControlReceiptState::Completed);
        CHECK(choc::json::parse(evaluated.response->detail_json)["result_json"].getString() ==
              std::string_view{R"({"redacted":true})"});
        const auto replayed_eval_consent = connection.manage(
            "grant-request", choc::json::toString(eval_grant_request, false));
        CHECK(replayed_eval_consent.status_id == "consent-replay");

        const auto released_controller =
            invoke("dev.pulp.session/control@1", R"({"action":"release"})");
        REQUIRE(released_controller.succeeded());
        CHECK(choc::json::parse(released_controller.response->detail_json)["lease_id"].getString() ==
              controller_lease_id);

        auto revoke = choc::value::createObject("");
        revoke.addMember("grant_id", choc::value::createString(request.grant_id));
        REQUIRE(connection.manage("revoke", choc::json::toString(revoke, false)).status_id ==
                "revoked");
        const auto denied_after_revoke =
            invoke("dev.pulp.state/read@1", "{}", std::nullopt, request.grant_id);
        CHECK_FALSE(denied_after_revoke.succeeded());

        const auto visible_after_revoke = run_installed_client(
            mcp_environment, root.runtime, {},
            R"({"jsonrpc":"2.0","id":2,"method":"tools/list"}
)");
        REQUIRE(visible_after_revoke.exit_code == 0);
        CHECK(visible_after_revoke.stdout_output.find("pulp_control_state_read") !=
              std::string::npos);
    }

    connection.disconnect();
    INFO("stopping installed Standalone daemon");
    daemon.stop();
    INFO("stopped installed Standalone daemon");
    if (author_host_environment) {
        ControlBrokerDaemon restarted({
            .runtime_root = root.runtime,
            .state_root = root.state,
            .sdk_version = "0.798.0-installed-parity-restarted",
            .executable_path = broker_executable,
            .process_generation = 212,
            .installed_host_selections =
                {{.host_id = host_id,
                  .intent = {.executable = installed_host,
                             .arguments = {},
                             .working_directory = installed_host.parent_path(),
                             .host_tier = ControlHostTier::Standalone}}},
            .decide_consent =
                [](const ControlGrantConsentRequest&) {
                    return ControlConsentDecision{
                        true, ControlConsentAuthority::TrustedHostUi,
                        "standalone-restarted-consent", {}};
                },
        });
        REQUIRE(restarted.start());
        ControlClientConnection restarted_connection(
            {.endpoint_path = restarted.endpoint_path(),
             .expected_broker_executable = broker_executable});
        REQUIRE(restarted_connection.connect());
        REQUIRE(restarted_connection.manage("enroll").status_id == "accepted");
        const auto inventory = restarted_connection.manage("instances");
        REQUIRE(inventory.status_id == "completed");
        CHECK(choc::json::parse(inventory.data_json)["instances"].size() == 0);
        ControlClient restarted_client(restarted_connection);
        REQUIRE(restarted_client.negotiate({.mandatory_features = {"receipts"}}).succeeded());
        request.request_id = "installed-parity-stale-after-restart";
        request.idempotency_key = "installed-parity-stale-after-restart-key";
        request.request_hash = *control_request_hash(request);
        const auto stale = restarted_client.request(request, 5s);
        CHECK_FALSE(stale.succeeded());
        restarted_connection.disconnect();
        restarted.stop();
    }
#else
    SUCCEED("ordinary Standalone state-read E2E is currently macOS-only");
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
        connection->manage("host-launch", choc::json::toString(launch_params, false), 15s);
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
