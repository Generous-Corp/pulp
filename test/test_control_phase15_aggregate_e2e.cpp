#include <catch2/catch_test_macros.hpp>

#include "control_broker_daemon.hpp"
#include "support/thread_progress.hpp"

#include <pulp/inspect/control_carrier.hpp>
#include <pulp/inspect/control_client_connection.hpp>
#include <pulp/inspect/control_operations.hpp>
#include <pulp/platform/child_process.hpp>
#include <pulp/runtime/crypto.hpp>

#include <choc/text/choc_JSON.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <sys/stat.h>
#endif

using namespace std::chrono_literals;
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
  "registry_digest": "1a3bcf207e34b79c49e32038699f3738d1d814838766e4d3f7aebaf895770ace",
  "endpoint_included": true,
  "unsafe_runtime_eval_acknowledged": false,
  "permission_terms": ["implemented", "built", "host_available", "activated", "policy_eligible", "client_granted", "session_live"],
  "capabilities": ["dev.pulp.instance/read@1", "dev.pulp.session/control@1", "dev.pulp.trace/session-control@1"]
}
)";

struct AggregateRoot {
    std::filesystem::path path;
    std::filesystem::path runtime;
    std::filesystem::path state;

    AggregateRoot() {
        const auto random = pulp::runtime::secure_random_bytes(8);
        REQUIRE(random);
        // Broker observer and endpoint names must remain below sockaddr_un's
        // path bound after the per-user runtime component is appended.
        path =
            std::filesystem::path{"/private/tmp"} / ("p15a-" + pulp::runtime::hex_encode(*random));
        REQUIRE(std::filesystem::create_directory(path));
        std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
        runtime = path / "runtime";
        state = path / "state";
    }

    ~AggregateRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

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

bool wait_for_path(const std::filesystem::path& path, std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(path))
            return true;
        std::this_thread::sleep_for(2ms);
    }
    return std::filesystem::exists(path);
}

std::string wait_for_registration(const std::filesystem::path& path) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream input(path);
        std::string registration;
        input >> registration;
        if (!registration.empty())
            return registration;
        std::this_thread::sleep_for(2ms);
    }
    return {};
}

std::optional<pulp::platform::ProcessResult>
wait_for_process_exit(pulp::platform::ChildProcess& child) {
    if (!pulp::test::wait_for_condition([&] { return !child.is_running(); })) {
        child.cancel();
        return std::nullopt;
    }
    return child.wait();
}

void stage_signed_binary(const std::filesystem::path& source,
                         const std::filesystem::path& destination, bool sign_copy = true,
                         std::optional<std::string_view> identifier = std::nullopt) {
    std::error_code error;
    std::filesystem::copy_file(source, destination,
                               std::filesystem::copy_options::overwrite_existing, error);
    INFO(source.string());
    INFO(destination.string());
    REQUIRE_FALSE(error);
    ::chmod(destination.c_str(), 0700);

    if (!sign_copy)
        return;

    pulp::platform::ProcessOptions options;
    options.capture_stdout = true;
    options.capture_stderr = true;
    std::vector<std::string> arguments{"--force", "--sign", "-"};
    if (identifier) {
        arguments.emplace_back("--identifier");
        arguments.emplace_back(*identifier);
    }
    arguments.push_back(destination.string());
    const auto signed_copy =
        pulp::platform::ChildProcess::run("/usr/bin/codesign", arguments, options);
    INFO(signed_copy.stdout_output);
    INFO(signed_copy.stderr_output);
    REQUIRE(signed_copy.exit_code == 0);
}

std::string quote(std::string_view value) {
    auto wrapper = choc::value::createObject("");
    wrapper.addMember("value", choc::value::createString(value));
    const auto json = choc::json::toString(wrapper, false);
    return json.substr(std::string_view{"{\"value\":"}.size(),
                       json.size() - std::string_view{"{\"value\":"}.size() - 1);
}

std::string rpc_call(int id, std::string_view name, std::string_view arguments_json) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
           ",\"method\":\"tools/call\",\"params\":{\"name\":" + quote(name) +
           ",\"arguments\":" + std::string(arguments_json) + "}}";
}

std::vector<choc::value::Value> parse_lines(std::string_view output) {
    std::vector<choc::value::Value> values;
    std::size_t start = 0;
    while (start < output.size()) {
        const auto end = output.find('\n', start);
        const auto line = output.substr(start, end == std::string_view::npos ? output.size() - start
                                                                             : end - start);
        if (!line.empty())
            values.push_back(choc::json::parse(line));
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return values;
}

pulp::platform::ProcessResult run_staged(const std::filesystem::path& binary,
                                         const std::filesystem::path& runtime,
                                         std::vector<std::string> arguments,
                                         int timeout_ms = 15'000) {
    std::vector<std::string> env_args{
        "TMPDIR=" + runtime.string(),
        binary.string(),
    };
    env_args.insert(env_args.end(), std::make_move_iterator(arguments.begin()),
                    std::make_move_iterator(arguments.end()));
    pulp::platform::ProcessOptions options;
    options.timeout_ms = timeout_ms;
    options.capture_stdout = true;
    options.capture_stderr = true;
    return pulp::platform::ChildProcess::run("/usr/bin/env", env_args, options);
}

pulp::platform::ProcessResult run_mcp(const std::filesystem::path& binary,
                                      const std::filesystem::path& runtime, std::string payload,
                                      int timeout_ms = 15'000) {
    std::vector<std::string> env_args{
        "TMPDIR=" + runtime.string(),
        binary.string(),
    };
    pulp::platform::ProcessOptions options;
    options.timeout_ms = timeout_ms;
    options.capture_stdout = true;
    options.capture_stderr = true;
    pulp::platform::ChildProcess process;
    const std::vector<std::uint8_t> bytes(payload.begin(), payload.end());
    if (!process.start_with_standard_input("/usr/bin/env", env_args, bytes, options))
        return {};
    return process.wait();
}

struct LiveInstance {
    std::string instance_id;
    std::string plugin_id;
    std::string publication_id;
    std::string registration_id;
    std::filesystem::path stop_path;
    std::filesystem::path deferred_path;
};

LiveInstance launch_host(ControlClientConnection& management, const AggregateRoot& root,
                         const std::filesystem::path& host_executable, std::string_view suffix) {
    const auto registration_path = root.path / ("registration-" + std::string(suffix));
    const auto stop_path = root.path / ("stop-" + std::string(suffix));
    const auto deferred_path = root.path / ("deferred-" + std::string(suffix));
    auto prepare = choc::value::createObject("");
    prepare.addMember("executable", choc::value::createString(host_executable.string()));
    auto arguments = choc::value::createEmptyArray();
    arguments.addArrayElement(choc::value::createString(registration_path.string()));
    arguments.addArrayElement(choc::value::createString(stop_path.string()));
    arguments.addArrayElement(choc::value::createString(deferred_path.string()));
    prepare.addMember("arguments", arguments);
    prepare.addMember("working_directory",
                      choc::value::createString(host_executable.parent_path().string()));
    prepare.addMember("host_tier", choc::value::createString("standalone"));
    const auto prepared =
        management.manage("host-prepare", choc::json::toString(prepare, false), 10s);
    INFO(prepared.explanation);
    REQUIRE(prepared.status_id == "prepared");
    const auto inventory_id =
        std::string(choc::json::parse(prepared.data_json)["inventory_id"].getString());
    auto launch = choc::value::createObject("");
    launch.addMember("inventory_id", choc::value::createString(inventory_id));
    const auto launched =
        management.manage("host-launch", choc::json::toString(launch, false), 10s);
    INFO(launched.explanation);
    REQUIRE(launched.status_id == "launched");
    const auto registration_id = wait_for_registration(registration_path);
    REQUIRE_FALSE(registration_id.empty());

    const auto inventory = management.manage("instances", "{}", 10s);
    REQUIRE(inventory.status_id == "completed");
    const auto inventory_data = choc::json::parse(inventory.data_json);
    const auto instances = inventory_data["instances"];
    for (std::uint32_t index = 0; index < instances.size(); ++index) {
        if (instances[index]["registration_id"].getString() == registration_id) {
            return {
                .instance_id = std::string(instances[index]["instance_id"].getString()),
                .plugin_id = std::string(instances[index]["plugin_id"].getString()),
                .publication_id = std::string(instances[index]["publication_id"].getString()),
                .registration_id = registration_id,
                .stop_path = stop_path,
                .deferred_path = deferred_path,
            };
        }
    }
    FAIL("launched host did not publish its exact registration");
    return {};
}

std::optional<std::pair<ControlReceiptId, choc::value::Value>>
find_receipt(const std::filesystem::path& directory, std::string_view request_id) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error))
        return std::nullopt;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        std::ifstream input(entry.path());
        const std::string contents((std::istreambuf_iterator<char>(input)),
                                   std::istreambuf_iterator<char>());
        try {
            auto value = choc::json::parse(contents);
            if (value.isObject() && value["request_id"].isString() &&
                value["request_id"].getString() == request_id) {
                return std::pair{ControlReceiptId{entry.path().stem().string()}, std::move(value)};
            }
        } catch (...) {
        }
    }
    return std::nullopt;
}

std::optional<choc::value::Value> read_receipt(const std::filesystem::path& directory,
                                               std::string_view receipt_id) {
    std::ifstream input(directory / (std::string(receipt_id) + ".json"));
    const std::string contents((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    try {
        auto value = choc::json::parse(contents);
        if (value.isObject())
            return value;
    } catch (...) {
    }
    return std::nullopt;
}
#endif

} // namespace

TEST_CASE("Phase 15 aggregate exact-instance CLI MCP revocation and disconnect E2E",
          "[inspect][control][e2e][phase15][aggregate][cli][mcp][security]") {
#ifdef __APPLE__
    AggregateRoot root;
    const auto executable = current_executable();
    REQUIRE_FALSE(executable.empty());
    const auto bin = executable.parent_path();
    const auto cli = bin / "pulp";
    const auto mcp = bin / "pulp-mcp";
    const auto broker_identity = bin / "pulp-control-broker";
    stage_signed_binary(PULP_CONTROL_PHASE15_CLI, cli);
    stage_signed_binary(PULP_CONTROL_PHASE15_MCP, mcp);
    // Preserve the aggregate binary's embedded identity for the broker and
    // management-client aliases; re-signing under another basename changes it.
    stage_signed_binary(executable, broker_identity, false);
    stage_signed_binary(executable, bin.parent_path() / "pulp", false);
    for (const auto& staged : {cli, mcp, broker_identity}) {
        INFO(staged.string());
        REQUIRE(std::filesystem::is_regular_file(staged));
    }

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

    const auto allowed_host = [&](std::string_view suffix) {
        return ControlTrustedHostLaunchIntent{
            .executable = host_executable,
            .arguments = {
                (root.path / ("registration-" + std::string(suffix))).string(),
                (root.path / ("stop-" + std::string(suffix))).string(),
                (root.path / ("deferred-" + std::string(suffix))).string(),
            },
            .working_directory = host_executable.parent_path(),
            .host_tier = ControlHostTier::Standalone,
        };
    };
    std::atomic<std::uint64_t> consent_sequence{0};
    ControlBrokerDaemon daemon({
        .runtime_root = root.runtime,
        .state_root = root.state,
        .sdk_version = "0.796.0-phase15-aggregate",
        .executable_path = executable,
        .process_generation = 1515,
        .trusted_host_allowlist = {allowed_host("a"), allowed_host("b")},
        .decide_consent =
            [&consent_sequence](const ControlGrantConsentRequest&) {
                return ControlConsentDecision{
                    true, ControlConsentAuthority::TrustedHostUi,
                    "phase15-aggregate-consent-" +
                        std::to_string(consent_sequence.fetch_add(1, std::memory_order_relaxed)), {}};
            },
    });
    REQUIRE(daemon.start());

    ControlClientConnection management(
        {.endpoint_path = daemon.endpoint_path(), .expected_broker_executable = broker_identity});
    REQUIRE(management.connect());
    REQUIRE(management.manage("enroll").status_id == "accepted");
    const auto first = launch_host(management, root, host_executable, "a");
    const auto second = launch_host(management, root, host_executable, "b");
    REQUIRE(first.instance_id != second.instance_id);
    REQUIRE(first.registration_id != second.registration_id);
    REQUIRE(first.publication_id != second.publication_id);
    REQUIRE(first.plugin_id == second.plugin_id);

    // A duplicated human-facing plugin label is never resolved as an instance
    // selector. Both installed clients require one of the exact broker IDs.
    const auto label_refused = run_staged(
        cli, root.runtime, {"control", "status", "--instance", first.plugin_id, "--json"});
    CHECK(label_refused.exit_code == 1);
    CHECK(label_refused.stdout_output.find("instance-not-found") != std::string::npos);

    const auto cli_read = run_staged(cli, root.runtime,
                                     {"control", "call", "--instance", first.instance_id,
                                      "dev.pulp.instance/read@1", "--params", "{}", "--json"});
    INFO(cli_read.stderr_output);
    REQUIRE(cli_read.exit_code == 0);
    const auto cli_receipt = choc::json::parse(cli_read.stdout_output);
    REQUIRE(cli_receipt["operation_id"].getString() == "dev.pulp.instance/read@1");
    REQUIRE(cli_receipt["state"].getString() == "completed");

    const auto mcp_read_args = "{\"instance_id\":" + quote(first.instance_id) +
                               ",\"request_id\":\"phase15-parity-read\",\"input\":{}}";
    const auto mcp_read =
        run_mcp(mcp, root.runtime, rpc_call(1, "pulp_control_instance_read", mcp_read_args) + "\n");
    INFO(mcp_read.stdout_output);
    INFO(mcp_read.stderr_output);
    REQUIRE(mcp_read.exit_code == 0);
    const auto mcp_read_lines = parse_lines(mcp_read.stdout_output);
    REQUIRE(mcp_read_lines.size() == 1);
    const auto mcp_structured = mcp_read_lines[0]["result"]["structuredContent"];
    REQUIRE_FALSE(mcp_read_lines[0]["result"]["isError"].getWithDefault<bool>(false));
    REQUIRE(mcp_structured["operation_id"].getString() == cli_receipt["operation_id"].getString());
    CHECK(choc::json::toString(mcp_structured["result"], false) ==
          choc::json::toString(cli_receipt["detail"], false));

    // A matching basename is not an authorization boundary. This functional
    // MCP copy can still authenticate the broker, but its separately issued
    // code identity was not captured from an authoritative install/build slot.
    const auto untrusted_bin = root.path / "untrusted-bin";
    REQUIRE(std::filesystem::create_directory(untrusted_bin));
    ::chmod(untrusted_bin.c_str(), 0700);
    const auto untrusted_mcp = untrusted_bin / "pulp-mcp";
    const auto untrusted_broker = untrusted_bin / "pulp-control-broker";
    stage_signed_binary(PULP_CONTROL_PHASE15_MCP, untrusted_mcp, true,
                        "dev.pulp.test.untrusted-mcp");
    stage_signed_binary(broker_identity, untrusted_broker, false);
    const auto untrusted_read =
        run_mcp(untrusted_mcp, root.runtime, rpc_call(99, "pulp_control_instances", "{}") + "\n");
    INFO(untrusted_read.stdout_output);
    INFO(untrusted_read.stderr_output);
    REQUIRE(untrusted_read.exit_code == 0);
    const auto untrusted_lines = parse_lines(untrusted_read.stdout_output);
    REQUIRE(untrusted_lines.size() == 1);
    CHECK(untrusted_lines[0]["result"]["isError"].getWithDefault<bool>(false));
    CHECK(untrusted_read.stdout_output.find("enrollment-denied") != std::string::npos);

    const auto grant_args =
        "{\"instance_id\":" + quote(first.instance_id) + ",\"profile\":\"inspect-readonly\"}";
    const auto grant_rpc =
        run_mcp(mcp, root.runtime,
                "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}\n" +
                    rpc_call(2, "pulp_control_grant_request", grant_args) + "\n");
    INFO(grant_rpc.stderr_output);
    REQUIRE(grant_rpc.exit_code == 0);
    const auto grant_lines = parse_lines(grant_rpc.stdout_output);
    REQUIRE(grant_lines.size() == 2);
    REQUIRE(grant_lines[0]["result"]["tools"].isArray());
    REQUIRE(grant_rpc.stdout_output.find("pulp_control_instance_read") != std::string::npos);
    const auto grant_id =
        std::string(grant_lines[1]["result"]["structuredContent"]["data"]["grant_id"].getString());
    REQUIRE_FALSE(grant_id.empty());

    const auto revoked_read_args = "{\"instance_id\":" + quote(first.instance_id) +
                                   ",\"grant_id\":" + quote(grant_id) + ",\"input\":{}}";
    const auto revoke_rpc =
        run_mcp(mcp, root.runtime,
                rpc_call(3, "pulp_control_revoke", "{\"grant_id\":" + quote(grant_id) + "}") +
                    "\n" + rpc_call(4, "pulp_control_instance_read", revoked_read_args) + "\n");
    INFO(revoke_rpc.stdout_output);
    INFO(revoke_rpc.stderr_output);
    REQUIRE(revoke_rpc.exit_code == 0);
    const auto revoke_lines = parse_lines(revoke_rpc.stdout_output);
    REQUIRE(revoke_lines.size() == 2);
    // A grant belongs to the MCP server process that requested it. A later
    // server process cannot mutate or spend that process's authority.
    CHECK(revoke_lines[0]["result"]["isError"].getWithDefault<bool>(false));
    CHECK(revoke_rpc.stdout_output.find("grant is unavailable to this client") !=
          std::string::npos);
    CHECK(revoke_lines[1]["result"]["isError"].getWithDefault<bool>(false));
    CHECK(revoke_rpc.stdout_output.find("admission-denied") != std::string::npos);

    // Normal one-shot MCP processes must not strand process-scoped durable
    // clients. This exceeds the registry's 16-client bound while preserving
    // the broker-durable CLI identity below.
    for (int iteration = 0; iteration < 20; ++iteration) {
        const auto probe = run_mcp(
            mcp, root.runtime,
            rpc_call(100 + iteration, "pulp_control_instances", "{}") + "\n");
        INFO(iteration);
        INFO(probe.stdout_output);
        INFO(probe.stderr_output);
        REQUIRE(probe.exit_code == 0);
        const auto probe_lines = parse_lines(probe.stdout_output);
        REQUIRE(probe_lines.size() == 1);
        CHECK_FALSE(
            probe_lines[0]["result"]["isError"].getWithDefault<bool>(false));
    }

    const auto cli_grant = run_staged(
        cli, root.runtime,
        {"control", "grant-request", "--instance", first.instance_id, "--profile",
         "inspect-readonly", "--json"});
    INFO(cli_grant.stdout_output);
    INFO(cli_grant.stderr_output);
    REQUIRE(cli_grant.exit_code == 0);
    const auto cli_grant_json = choc::json::parse(cli_grant.stdout_output);
    const auto cli_grant_id =
        std::string(cli_grant_json["data"]["grant_id"].getString());
    REQUIRE_FALSE(cli_grant_id.empty());
    const auto cli_revoke = run_staged(
        cli, root.runtime, {"control", "revoke", "--grant", cli_grant_id, "--json"});
    INFO(cli_revoke.stdout_output);
    INFO(cli_revoke.stderr_output);
    REQUIRE(cli_revoke.exit_code == 0);
    CHECK(choc::json::parse(cli_revoke.stdout_output)["status"].getString() == "revoked");

    const auto active_args = "{\"instance_id\":" + quote(second.instance_id) +
                             ",\"request_id\":\"phase15-mcp-disconnect-active\","
                             "\"timeout_ms\":8000,\"input\":{\"action\":\"start\","
                             "\"ring_mb\":9}}";
    const auto active_payload =
        rpc_call(5, "pulp_control_trace_session_control", active_args) + "\n";
    std::vector<std::string> active_env{
        "TMPDIR=" + root.runtime.string(),
        mcp.string(),
    };
    pulp::platform::ProcessOptions active_options;
    active_options.timeout_ms = 12'000;
    active_options.capture_stdout = true;
    active_options.capture_stderr = true;
    pulp::platform::ChildProcess active_mcp;
    const std::vector<std::uint8_t> active_bytes(active_payload.begin(), active_payload.end());
    REQUIRE(active_mcp.start_with_standard_input("/usr/bin/env", active_env, active_bytes,
                                                 active_options));
    REQUIRE(wait_for_path(second.deferred_path));
    REQUIRE(active_mcp.is_running());

    const auto parallel_mcp_args =
        "{\"instance_id\":" + quote(first.instance_id) +
        ",\"request_id\":\"phase15-concurrent-mcp-read\",\"input\":{}}";
    const auto parallel_mcp_read =
        run_mcp(mcp, root.runtime,
                rpc_call(6, "pulp_control_instance_read", parallel_mcp_args) + "\n");
    INFO(parallel_mcp_read.stdout_output);
    INFO(parallel_mcp_read.stderr_output);
    REQUIRE(parallel_mcp_read.exit_code == 0);
    const auto parallel_mcp_lines = parse_lines(parallel_mcp_read.stdout_output);
    REQUIRE(parallel_mcp_lines.size() == 1);
    REQUIRE_FALSE(
        parallel_mcp_lines[0]["result"]["isError"].getWithDefault<bool>(false));

    std::optional<std::pair<ControlReceiptId, choc::value::Value>> running_receipt;
    std::optional<std::pair<ControlReceiptId, choc::value::Value>> parallel_mcp_receipt;
    for (unsigned attempt = 0;
         attempt < 1'000 && (!running_receipt || !parallel_mcp_receipt); ++attempt) {
        running_receipt =
            find_receipt(root.state / "operations", "phase15-mcp-disconnect-active");
        parallel_mcp_receipt =
            find_receipt(root.state / "operations", "phase15-concurrent-mcp-read");
        if (!running_receipt || !parallel_mcp_receipt)
            std::this_thread::sleep_for(2ms);
    }
    REQUIRE(running_receipt);
    REQUIRE(parallel_mcp_receipt);
    REQUIRE(running_receipt->second["client_id"].isString());
    REQUIRE(parallel_mcp_receipt->second["client_id"].isString());
    CHECK(running_receipt->second["client_id"].getString() !=
          parallel_mcp_receipt->second["client_id"].getString());
    REQUIRE(active_mcp.is_running());

    auto simultaneous_cli = std::async(std::launch::async, [&] {
        return run_staged(cli, root.runtime,
                          {"control", "call", "--instance", first.instance_id,
                           "dev.pulp.instance/read@1", "--params", "{}", "--json"});
    });
    const auto concurrent_read = simultaneous_cli.get();
    INFO(concurrent_read.stderr_output);
    REQUIRE(concurrent_read.exit_code == 0);
    REQUIRE(active_mcp.is_running());

    const auto concurrent_cli_receipt = choc::json::parse(concurrent_read.stdout_output);
    const auto first_cli_durable = read_receipt(
        root.state / "operations", std::string(cli_receipt["receipt_id"].getString()));
    const auto concurrent_cli_durable = read_receipt(
        root.state / "operations",
        std::string(concurrent_cli_receipt["receipt_id"].getString()));
    REQUIRE(first_cli_durable);
    REQUIRE(concurrent_cli_durable);
    REQUIRE((*first_cli_durable)["client_id"].isString());
    REQUIRE((*concurrent_cli_durable)["client_id"].isString());
    CHECK((*first_cli_durable)["client_id"].getString() ==
          (*concurrent_cli_durable)["client_id"].getString());

    active_mcp.cancel();
    const auto disconnected_mcp = wait_for_process_exit(active_mcp);
    INFO("cancelled MCP process must exit within the progress deadline");
    REQUIRE(disconnected_mcp);
    CHECK((disconnected_mcp->was_cancelled || disconnected_mcp->exit_code != 0));

    CHECK(running_receipt->second["operation_id"].getString() ==
          "dev.pulp.trace/session-control@1");
    CHECK(running_receipt->second["instance_id"].getString() == second.instance_id);
    CHECK(running_receipt->second["canonical_request_hash"].getString().size() == 64);
    CHECK(running_receipt->second["state"].getString() == "running");

    // Installed MCP clients have durable principals: losing the stdio
    // transport must preserve the active canonical receipt for reconciliation,
    // rather than cancelling or deleting it with the endpoint session.
    std::this_thread::sleep_for(50ms);
    const auto receipt_after_disconnect =
        find_receipt(root.state / "operations", "phase15-mcp-disconnect-active");
    REQUIRE(receipt_after_disconnect);
    CHECK(receipt_after_disconnect->first.value == running_receipt->first.value);
    CHECK(receipt_after_disconnect->second["canonical_request_hash"].getString() ==
          running_receipt->second["canonical_request_hash"].getString());
    CHECK(receipt_after_disconnect->second["state"].getString() == "running");

    std::ofstream(first.stop_path) << "stop\n";
    std::ofstream(second.stop_path) << "stop\n";
    management.disconnect();
    daemon.stop();

    // Reopening the durable store after broker shutdown performs the separate
    // crash-recovery transition for an operation whose host outcome is unknown.
    ControlOperationStore durable({.directory = root.state / "operations"});
    REQUIRE(durable.open().succeeded());
    const auto final_receipt = durable.receipt(running_receipt->first);
    REQUIRE(final_receipt);
    CHECK(final_receipt->binding.request_id == "phase15-mcp-disconnect-active");
    CHECK(final_receipt->binding.instance_id == second.instance_id);
    CHECK(final_receipt->binding.operation_id == "dev.pulp.trace/session-control@1");
    CHECK(final_receipt->binding.canonical_request_hash.size() == 64);
    CHECK(final_receipt->state == ControlReceiptState::UnknownNeedsRefresh);
#else
    SUCCEED("Phase 15 installed aggregate control E2E is currently macOS-only");
#endif
}
