#include <catch2/catch_test_macros.hpp>

#include "control_broker_daemon.hpp"
#include <pulp/inspect/control_client_connection.hpp>
#include <pulp/inspect/control_operations.hpp>
#include <pulp/platform/child_process.hpp>
#include <pulp/runtime/crypto.hpp>

#include <choc/text/choc_JSON.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <sys/stat.h>
#endif

using namespace pulp::inspect;
using namespace std::chrono_literals;

namespace {

struct Root {
    std::filesystem::path path;
    std::filesystem::path runtime;
    std::filesystem::path state;
    Root() {
        const auto bytes = pulp::runtime::secure_random_bytes(8);
        REQUIRE(bytes);
        path = std::filesystem::path{"/private/tmp"} /
               ("sample-region-c4-" + pulp::runtime::hex_encode(*bytes));
        REQUIRE(std::filesystem::create_directory(path));
        std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
        runtime = path / "runtime";
        state = path / "state";
    }
    ~Root() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

#ifdef __APPLE__
std::filesystem::path self() {
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    std::vector<char> bytes(size);
    if (size == 0 || _NSGetExecutablePath(bytes.data(), &size) != 0)
        return {};
    return std::filesystem::weakly_canonical(bytes.data());
}

pulp::platform::ProcessResult run(const std::filesystem::path& binary,
                                  const std::filesystem::path& runtime,
                                  std::vector<std::string> args) {
    args.insert(args.begin(), binary.string());
    pulp::platform::ProcessOptions options;
    options.capture_stdout = true;
    options.capture_stderr = true;
    options.timeout_ms = 20'000;
    std::vector<std::string> env{"TMPDIR=" + runtime.string()};
    env.insert(env.end(), args.begin(), args.end());
    return pulp::platform::ChildProcess::run("/usr/bin/env", env, options);
}

std::string mcp_call(int id, std::string_view name, std::string_view instance,
                     std::string_view input, std::string_view profile = "develop") {
    auto request = choc::value::createObject("");
    request.setMember("jsonrpc", "2.0");
    request.setMember("id", id);
    request.setMember("method", "tools/call");
    auto params = choc::value::createObject("");
    params.setMember("name", std::string(name));
    auto arguments = choc::value::createObject("");
    arguments.setMember("instance_id", std::string(instance));
    arguments.setMember("profile", std::string(profile));
    arguments.setMember("input", choc::json::parse(input));
    params.setMember("arguments", arguments);
    request.setMember("params", params);
    return choc::json::toString(request, false) + "\n";
}

std::vector<choc::value::Value> run_mcp(const std::filesystem::path& binary,
                                        const std::filesystem::path& runtime,
                                        std::string transcript) {
    pulp::platform::ProcessOptions options;
    options.capture_stdout = true;
    options.capture_stderr = true;
    options.timeout_ms = 30'000;
    pulp::platform::ChildProcess process;
    const std::vector<std::uint8_t> bytes(transcript.begin(), transcript.end());
    REQUIRE(process.start_with_standard_input(
        "/usr/bin/env", {"TMPDIR=" + runtime.string(), binary.string()}, bytes, options));
    const auto result = process.wait();
    INFO(result.stdout_output);
    INFO(result.stderr_output);
    REQUIRE(result.exit_code == 0);
    std::vector<choc::value::Value> responses;
    std::size_t start = 0;
    while (start < result.stdout_output.size()) {
        const auto end = result.stdout_output.find('\n', start);
        const auto line = result.stdout_output.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty()) {
            auto response = choc::json::parse(line);
            if (response.hasObjectMember("id"))
                responses.push_back(std::move(response));
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return responses;
}

std::set<std::string> operation_set(choc::value::ValueView detail) {
    std::set<std::string> result;
    const auto capabilities = detail["capabilities"];
    for (std::uint32_t i = 0; i < capabilities.size(); ++i)
        result.emplace(capabilities[i].getString());
    return result;
}

void require_manifest(choc::value::ValueView detail, std::string_view expected_plugin_id,
                      std::initializer_list<std::string_view> expected_operations) {
    REQUIRE(detail["plugin_id"].getString() == expected_plugin_id);
    std::set<std::string> expected;
    for (const auto operation : expected_operations)
        expected.emplace(operation);
    REQUIRE(operation_set(detail) == expected);
}

#endif
} // namespace

TEST_CASE("sample-region control reaches editable and frozen products through CLI",
          "[inspect][control][sample-region][e2e][cli]") {
#ifdef __APPLE__
    Root root;
    const auto executable = self();
    REQUIRE_FALSE(executable.empty());
    const auto bin = executable.parent_path();
    const auto cli = bin / "pulp";
    const auto broker = bin / "pulp-control-broker";
    const auto host = std::filesystem::path{PULP_SAMPLE_REGION_EDITABLE_STANDALONE};
    const auto frozen = std::filesystem::path{PULP_SAMPLE_REGION_FROZEN_STANDALONE};
    REQUIRE(std::filesystem::exists(cli));
    REQUIRE(std::filesystem::exists(broker));
    REQUIRE(std::filesystem::exists(host));
    REQUIRE(std::filesystem::exists(frozen));

    const auto allow = [&] {
        return ControlTrustedHostLaunchIntent{.executable = host,
                                              .arguments = {},
                                              .working_directory = host.parent_path(),
                                              .host_tier = ControlHostTier::Standalone};
    };
    std::atomic<std::uint64_t> consent_sequence{0};
    ControlBrokerDaemon daemon(
        {.runtime_root = root.runtime,
         .state_root = root.state,
         .sdk_version = "c4-sample-region",
         .executable_path = executable,
         .process_generation = 4001,
         .installed_host_selections = {{.host_id = "sample-region-editable", .intent = allow()},
                                       {.host_id = "sample-region-frozen",
                                        .intent = {.executable = frozen,
                                                   .arguments = {},
                                                   .working_directory = frozen.parent_path(),
                                                   .host_tier = ControlHostTier::Standalone}}},
         .decide_consent = [&consent_sequence](const ControlGrantConsentRequest&) {
             return ControlConsentDecision{
                 true,
                 ControlConsentAuthority::TrustedHostUi,
                 "sample-region-c4-consent-" +
                     std::to_string(consent_sequence.fetch_add(1, std::memory_order_relaxed)),
                 {}};
         }});
    REQUIRE(daemon.start());
    ControlClientConnection management(
        {.endpoint_path = daemon.endpoint_path(), .expected_broker_executable = broker});
    REQUIRE(management.connect());
    REQUIRE(management.manage("enroll").status_id == "accepted");
    auto selection = choc::value::createObject("");
    selection.addMember("host_id", choc::value::createString("sample-region-editable"));
    const auto prepared =
        management.manage("host-prepare-installed", choc::json::toString(selection, false), 10s);
    REQUIRE(prepared.status_id == "prepared");
    auto launch = choc::value::createObject("");
    launch.addMember("inventory_id",
                     choc::value::createString(
                         choc::json::parse(prepared.data_json)["inventory_id"].getString()));
    REQUIRE(management.manage("host-launch", choc::json::toString(launch, false), 10s).status_id ==
            "launched");
    const auto instances = management.manage("instances", "{}", 10s);
    REQUIRE(instances.status_id == "completed");
    const auto values_document = choc::json::parse(instances.data_json);
    const auto values = values_document["instances"];
    constexpr std::string_view editable_plugin_id = "dev.pulp.sample-region-allpass.editable";
    constexpr std::string_view frozen_plugin_id = "dev.pulp.sample-region-allpass.frozen";
    std::string instance;
    for (std::uint32_t i = 0; i < values.size(); ++i)
        if (values[i]["plugin_id"].getString() == editable_plugin_id)
            instance = values[i]["instance_id"].getString();
    REQUIRE_FALSE(instance.empty());
    const auto read = run(cli, root.runtime,
                          {"control", "call", "--instance", instance, "dev.pulp.instance/read@1",
                           "--params", "{}", "--json"});
    INFO(read.stderr_output);
    REQUIRE(read.exit_code == 0);
    const auto read_receipt = choc::json::parse(read.stdout_output);
    REQUIRE(read_receipt["state"].getString() == "completed");
    require_manifest(read_receipt["detail"], editable_plugin_id,
                     {"dev.pulp.instance/read@1", "dev.pulp.session/control@1",
                      "dev.pulp.state/read@1", "dev.pulp.state/parameter-gesture@1",
                      "dev.pulp.graph/sample-region.read@1",
                      "dev.pulp.graph/sample-region.edit@1"});
    const auto region_read =
        run(cli, root.runtime,
            {"control", "call", "--instance", instance, "dev.pulp.graph/sample-region.read@1",
             "--params", "{}", "--json"});
    REQUIRE(region_read.exit_code == 0);
    REQUIRE(region_read.stdout_output.find("graph_generation") != std::string::npos);

    const auto cli_call = [&](std::string_view operation, std::string params,
                              const std::string& grant = std::string{}) {
        std::vector<std::string> args{
            "control",         "call",  "--instance", instance, std::string(operation), "--params",
            std::move(params), "--json"};
        if (!grant.empty()) {
            args.emplace_back("--grant");
            args.push_back(grant);
        }
        return run(cli, root.runtime, std::move(args));
    };
    const auto before_state = cli_call("dev.pulp.state/read@1", "{}");
    INFO(before_state.stdout_output);
    INFO(before_state.stderr_output);
    REQUIRE(before_state.exit_code == 0);
    const auto before_detail_document = choc::json::parse(before_state.stdout_output);
    const auto before_detail = before_detail_document["detail"];
    REQUIRE(before_detail["parameters"].size() == 1);
    const auto coefficient = before_detail["parameters"][0];
    CHECK(coefficient["id"].getInt64() == 2901);
    CHECK(coefficient["rate"].getString() == "control");
    CHECK_FALSE(coefficient.hasObjectMember("smoothing_ramp_seconds"));
    const auto state_generation = before_detail["state_generation"].getInt64();
    const auto catalog_generation = before_detail["catalog_generation"].getInt64();
    const auto full_read = cli_call("dev.pulp.graph/sample-region.read@1",
                                    R"({"region_id":901,"include_definition":true})");
    REQUIRE(full_read.exit_code == 0);
    const auto full_detail_document = choc::json::parse(full_read.stdout_output);
    const auto full_detail = full_detail_document["detail"];
    const auto graph_generation = full_detail["graph_generation"].getInt64();
    const auto definition = full_detail["regions"][0]["definition"];
    CHECK(definition["promoted_parameters"][0]["smoothing_ramp_seconds"].getInt64() == 0);
    CHECK(definition["promoted_parameters"][0]["minimum"].getFloat64() ==
          coefficient["min"].getFloat64());
    CHECK(definition["promoted_parameters"][0]["maximum"].getFloat64() ==
          coefficient["max"].getFloat64());

    const auto develop =
        run(cli, root.runtime,
            {"control", "grant-request", "--instance", instance, "--profile", "develop", "--json"});
    INFO(develop.stdout_output);
    INFO(develop.stderr_output);
    REQUIRE(develop.exit_code == 0);
    const std::string develop_grant(
        choc::json::parse(develop.stdout_output)["data"]["grant_id"].getString());
    const auto lease =
        cli_call("dev.pulp.session/control@1", R"({"action":"acquire"})", develop_grant);
    INFO(lease.stdout_output);
    INFO(lease.stderr_output);
    REQUIRE(lease.exit_code == 0);
    const auto gesture = cli_call(
        "dev.pulp.state/parameter-gesture@1",
        R"({"parameter_id":2901,"normalized_value":0.625,"idempotency_key":"coefficient-cli"})",
        develop_grant);
    INFO(gesture.stdout_output);
    INFO(gesture.stderr_output);
    REQUIRE(gesture.exit_code == 0);
    CHECK(choc::json::parse(gesture.stdout_output)["detail"]["applied"].getBool());
    const auto after_state = cli_call("dev.pulp.state/read@1", "{}");
    REQUIRE(after_state.exit_code == 0);
    const auto after_detail_document = choc::json::parse(after_state.stdout_output);
    const auto after_detail = after_detail_document["detail"];
    CHECK(after_detail["state_generation"].getInt64() == state_generation + 1);
    CHECK(after_detail["catalog_generation"].getInt64() == catalog_generation);
    CHECK(after_detail["parameters"][0]["normalized"].getFloat64() == 0.625);
    const auto after_region = cli_call("dev.pulp.graph/sample-region.read@1", "{}");
    REQUIRE(after_region.exit_code == 0);
    CHECK(choc::json::parse(after_region.stdout_output)["detail"]["graph_generation"].getInt64() ==
          graph_generation);

    const auto observe =
        run(cli, root.runtime,
            {"control", "grant-request", "--instance", instance, "--profile", "observe", "--json"});
    INFO(observe.stdout_output);
    INFO(observe.stderr_output);
    REQUIRE(observe.exit_code == 0);
    const std::string observe_grant(
        choc::json::parse(observe.stdout_output)["data"]["grant_id"].getString());
    const auto forbidden_gesture = cli_call(
        "dev.pulp.state/parameter-gesture@1",
        R"({"parameter_id":2901,"normalized_value":0.75,"idempotency_key":"observe-denied"})",
        observe_grant);
    CHECK(forbidden_gesture.exit_code != 0);
    const auto output_node = definition["output_boundaries"][0].getInt64();
    std::int64_t source_node = 0, source_port = 0;
    const auto connections = definition["connections"];
    for (std::uint32_t index = 0; index < connections.size(); ++index) {
        const auto edge = connections[index];
        if (edge["destination_node_id"].getInt64() == output_node && !edge["crossing"].getBool()) {
            source_node = edge["source_node_id"].getInt64();
            source_port = edge["source_port"].getInt64();
        }
    }
    REQUIRE(source_node != 0);
    const auto edit_input =
        "{\"region_id\":901,\"expected_graph_generation\":" + std::to_string(graph_generation) +
        ",\"actions\":[" +
        R"({"op":"add_supported_kernel","temporary_node_id":"t1","type_id":"pulp.core.unit-delay","type_version":1,"config":{"kind":"none"}},)" +
        "{\"op\":\"disconnect\",\"source_node_id\":" + std::to_string(source_node) +
        ",\"source_port\":" + std::to_string(source_port) +
        ",\"destination_node_id\":" + std::to_string(output_node) + R"(,"destination_port":0},)" +
        "{\"op\":\"connect\",\"source_node_id\":" + std::to_string(source_node) +
        ",\"source_port\":" + std::to_string(source_port) +
        R"(,"destination_temporary_node_id":"t1","destination_port":0},)" +
        R"({"op":"connect","source_temporary_node_id":"t1","source_port":0,"destination_node_id":)" +
        std::to_string(output_node) + R"(,"destination_port":0}]})";
    const auto edited = cli_call("dev.pulp.graph/sample-region.edit@1", edit_input, develop_grant);
    INFO(edited.stdout_output);
    INFO(edited.stderr_output);
    REQUIRE(edited.exit_code == 0);
    const auto edit_document = choc::json::parse(edited.stdout_output);
    const auto edit_detail = edit_document["detail"];
    CHECK(edit_detail["old_graph_generation"].getInt64() == graph_generation);
    CHECK(edit_detail["new_graph_generation"].getInt64() == graph_generation + 1);
    CHECK(edit_detail["receipt_id"].getString() == edit_document["receipt_id"].getString());
    CHECK(edit_detail["applied_action_count"].getInt64() == 4);
    REQUIRE(edit_detail["node_mapping"].size() == 1);
    CHECK(edit_detail["node_mapping"][0]["temporary_node_id"].getString() == "t1");
    CHECK(edit_detail["node_mapping"][0]["node_id"].getInt64() > 0);
    CHECK(edit_detail["state_retained_count"].getInt64() == 2);
    CHECK(edit_detail["state_reset_count"].getInt64() == 1);
    CHECK(edit_detail["resources"]["delay_nodes"].getInt64() == 3);
    const auto stale = cli_call("dev.pulp.graph/sample-region.edit@1", edit_input, develop_grant);
    CHECK(stale.exit_code != 0);
    const auto unchanged_state = cli_call("dev.pulp.state/read@1", "{}");
    REQUIRE(unchanged_state.exit_code == 0);
    const auto unchanged_state_document = choc::json::parse(unchanged_state.stdout_output);
    CHECK(unchanged_state_document["detail"]["state_generation"].getInt64() ==
          state_generation + 1);
    CHECK(unchanged_state_document["detail"]["catalog_generation"].getInt64() ==
          catalog_generation);
    const auto release =
        cli_call("dev.pulp.session/control@1", R"({"action":"release"})", develop_grant);
    REQUIRE(release.exit_code == 0);

    const auto editable_status =
        run(cli, root.runtime, {"control", "status", "--instance", instance, "--json"});
    REQUIRE(editable_status.exit_code == 0);
    require_manifest(
        choc::json::parse(editable_status.stdout_output)["instance"], editable_plugin_id,
        {"dev.pulp.instance/read@1", "dev.pulp.session/control@1", "dev.pulp.state/read@1",
         "dev.pulp.state/parameter-gesture@1", "dev.pulp.graph/sample-region.read@1",
         "dev.pulp.graph/sample-region.edit@1"});

    auto frozen_selection = choc::value::createObject("");
    frozen_selection.addMember("host_id", choc::value::createString("sample-region-frozen"));
    const auto frozen_prepared = management.manage(
        "host-prepare-installed", choc::json::toString(frozen_selection, false), 10s);
    REQUIRE(frozen_prepared.status_id == "prepared");
    auto frozen_launch = choc::value::createObject("");
    frozen_launch.addMember(
        "inventory_id",
        choc::value::createString(
            choc::json::parse(frozen_prepared.data_json)["inventory_id"].getString()));
    REQUIRE(management.manage("host-launch", choc::json::toString(frozen_launch, false), 10s)
                .status_id == "launched");
    std::string frozen_instance;
    for (int attempt = 0; attempt != 500 && frozen_instance.empty(); ++attempt) {
        const auto inventory = management.manage("instances", "{}", 10s);
        REQUIRE(inventory.status_id == "completed");
        const auto list_document = choc::json::parse(inventory.data_json);
        const auto list = list_document["instances"];
        for (std::uint32_t i = 0; i < list.size(); ++i) {
            const auto candidate = std::string(list[i]["instance_id"].getString());
            if (candidate != instance && list[i]["plugin_id"].getString() == frozen_plugin_id)
                frozen_instance = candidate;
        }
        if (frozen_instance.empty())
            std::this_thread::sleep_for(10ms);
    }
    REQUIRE_FALSE(frozen_instance.empty());
    const auto frozen_read = run(cli, root.runtime,
                                 {"control", "call", "--instance", frozen_instance,
                                  "dev.pulp.instance/read@1", "--params", "{}", "--json"});
    REQUIRE(frozen_read.exit_code == 0);
    const auto frozen_read_receipt = choc::json::parse(frozen_read.stdout_output);
    REQUIRE(frozen_read_receipt["state"].getString() == "completed");
    require_manifest(frozen_read_receipt["detail"], frozen_plugin_id,
                     {"dev.pulp.instance/read@1", "dev.pulp.session/control@1",
                      "dev.pulp.state/read@1", "dev.pulp.state/parameter-gesture@1",
                      "dev.pulp.graph/sample-region.read@1"});
    const auto frozen_region_read =
        run(cli, root.runtime,
            {"control", "call", "--instance", frozen_instance,
             "dev.pulp.graph/sample-region.read@1", "--params", "{}", "--json"});
    REQUIRE(frozen_region_read.exit_code == 0);
    const auto frozen_status =
        run(cli, root.runtime, {"control", "status", "--instance", frozen_instance, "--json"});
    REQUIRE(frozen_status.exit_code == 0);
    require_manifest(choc::json::parse(frozen_status.stdout_output)["instance"], frozen_plugin_id,
                     {"dev.pulp.instance/read@1", "dev.pulp.session/control@1",
                      "dev.pulp.state/read@1", "dev.pulp.state/parameter-gesture@1",
                      "dev.pulp.graph/sample-region.read@1"});
    const auto frozen_edit = run(
        cli, root.runtime,
        {"control", "call", "--instance", frozen_instance, "dev.pulp.graph/sample-region.edit@1",
         "--params",
         R"({"region_id":901,"expected_graph_generation":1,"actions":[{"op":"set_finite_constant","node_id":1,"value":0.5}]})",
         "--json"});
    CHECK(frozen_edit.exit_code != 0);
    CHECK((frozen_edit.stdout_output.find("unadvertised") != std::string::npos ||
           frozen_edit.stderr_output.find("unadvertised") != std::string::npos));
    const auto mcp = bin / "pulp-mcp";
    REQUIRE(std::filesystem::exists(mcp));
    for (const auto& id : {instance, frozen_instance}) {
        const bool editable_product = id == instance;
        const auto responses = run_mcp(
            mcp, root.runtime,
            std::string(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})") + "\n" +
                mcp_call(2, "pulp_control_state_read", id, "{}") +
                mcp_call(3, "pulp_control_graph_sample_region_read", id, "{}") +
                mcp_call(4, "pulp_control_graph_sample_region_read", id,
                         R"({"region_id":901,"include_definition":true})") +
                mcp_call(5, "pulp_control_session_control", id, R"({"action":"acquire"})") +
                mcp_call(
                    6, "pulp_control_state_parameter_gesture", id,
                    R"({"parameter_id":2901,"normalized_value":0.75,"idempotency_key":"coefficient-mcp"})") +
                mcp_call(7, "pulp_control_state_read", id, "{}") +
                mcp_call(8, "pulp_control_graph_sample_region_read", id, "{}") +
                mcp_call(9, "pulp_control_session_control", id, R"({"action":"release"})"));
        REQUIRE(responses.size() == 9);
        const auto tools = responses[0]["result"]["tools"];
        std::set<std::string> names;
        for (std::uint32_t index = 0; index < tools.size(); ++index)
            names.emplace(tools[index]["name"].getString());
        CHECK(names.contains("pulp_control_graph_sample_region_read"));
        CHECK(names.contains("pulp_control_graph_sample_region_edit") == editable_product);
        for (std::size_t index = 1; index < responses.size(); ++index) {
            INFO(choc::json::toString(responses[index]));
            REQUIRE_FALSE(responses[index]["result"]["isError"].getWithDefault<bool>(false));
            REQUIRE(responses[index]["result"]["structuredContent"]["state"].getString() ==
                    "completed");
        }
        const auto state_before = responses[1]["result"]["structuredContent"]["result"];
        const auto state_after = responses[6]["result"]["structuredContent"]["result"];
        CHECK(state_before["parameters"][0]["id"].getInt64() == 2901);
        CHECK(state_before["parameters"][0]["rate"].getString() == "control");
        CHECK(state_after["parameters"][0]["normalized"].getFloat64() == 0.75);
        CHECK(state_after["state_generation"].getInt64() ==
              state_before["state_generation"].getInt64() + 1);
        CHECK(state_after["catalog_generation"].getInt64() ==
              state_before["catalog_generation"].getInt64());
        const auto summary_before = responses[2]["result"]["structuredContent"]["result"];
        const auto full = responses[3]["result"]["structuredContent"]["result"];
        const auto summary_after = responses[7]["result"]["structuredContent"]["result"];
        CHECK_FALSE(summary_before["regions"][0].hasObjectMember("definition"));
        CHECK(full["regions"][0]["definition"]["promoted_parameters"][0]["smoothing_ramp_seconds"]
                  .getInt64() == 0);
        CHECK(summary_after["graph_generation"].getInt64() ==
              summary_before["graph_generation"].getInt64());
        if (!editable_product) {
            const auto edit_responses = run_mcp(
                mcp, root.runtime,
                mcp_call(10, "pulp_control_graph_sample_region_edit", id,
                         R"({"region_id":901,"expected_graph_generation":1,"actions":[]})"));
            REQUIRE(edit_responses.size() == 1);
            CHECK(edit_responses[0]["result"]["isError"].getWithDefault<bool>(false));
        }
    }
    management.disconnect();
    daemon.stop();
#else
    SKIP("sample-region broker E2E is macOS-only");
#endif
}
