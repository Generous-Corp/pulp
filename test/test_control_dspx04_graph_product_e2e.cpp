#include "../inspect/src/control_broker_daemon.hpp"
#include "support/control_sample_region_e2e.hpp"

#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/crypto.hpp>

#include <array>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>

TEST_CASE("DSPX-04 product binding reaches modulation route through broker",
          "[inspect][control][dspx-04][e2e]") {
#ifdef __APPLE__
    Root root;
    const auto executable = self();
    REQUIRE_FALSE(executable.empty());
    const auto bin = executable.parent_path();
    const auto cli = bin / "pulp";
    const auto mcp = bin / "pulp-mcp";
    const auto broker = bin / "pulp-control-broker";
    const auto built_host = std::filesystem::path{PULP_DSPX04_GRAPH_PRODUCT_FIXTURE};
    REQUIRE(std::filesystem::exists(cli));
    REQUIRE(std::filesystem::exists(mcp));
    REQUIRE(std::filesystem::exists(broker));
    REQUIRE(std::filesystem::exists(built_host));
    const auto host_dir = root.path / "product";
    REQUIRE(std::filesystem::create_directory(host_dir));
    const auto host = host_dir / built_host.filename();
    REQUIRE(std::filesystem::copy_file(built_host, host));
    REQUIRE(std::filesystem::copy_file(built_host.string() + ".inspector-capabilities.json",
                                       host.string() + ".inspector-capabilities.json"));
    const auto source_digest = pulp::runtime::sha256_file_hex(built_host, 1024ULL * 1024 * 1024);
    const auto staged_digest = pulp::runtime::sha256_file_hex(host, 1024ULL * 1024 * 1024);
    REQUIRE(source_digest);
    REQUIRE(staged_digest);
    REQUIRE(*staged_digest == *source_digest);
    std::ifstream manifest_stream(host.string() + ".inspector-capabilities.json");
    const std::string manifest_bytes(std::istreambuf_iterator<char>{manifest_stream}, {});
    REQUIRE_FALSE(manifest_bytes.empty());
    const auto manifest = choc::json::parse(manifest_bytes);
    REQUIRE(manifest["schema"].getString() == "dev.pulp.control/artifact-manifest@1");
    REQUIRE(manifest["bundle_id"].getString() == "dev.pulp.test.dspx04-graph-product");
    REQUIRE(manifest["target"].getString() == "pulp-control-dspx04-graph-product-fixture");
    const auto manifest_digest = pulp::runtime::sha256_hex(manifest_bytes);
    std::filesystem::permissions(host, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
    std::filesystem::permissions(host.string() + ".inspector-capabilities.json",
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace);

    const auto capabilities = run(cli, root.runtime, {"control", "capabilities", "--json"});
    REQUIRE(capabilities.exit_code == 0);
    const auto registry = choc::json::parse(capabilities.stdout_output);
    bool route_seen = false;
    for (const auto capability : registry["capabilities"])
        route_seen =
            route_seen || capability["id"].getString() == "dev.pulp.graph/modulation-route.edit@1";
    REQUIRE(route_seen);

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
         .sdk_version = "dspx04-product",
         .executable_path = executable,
         .process_generation = 404,
         .installed_host_selections = {{.host_id = "dspx04-graph-product", .intent = allow()}},
         .decide_consent = [&consent_sequence](const ControlGrantConsentRequest&) {
             return ControlConsentDecision{true,
                                           ControlConsentAuthority::TrustedHostUi,
                                           "dspx04-e2e-consent-" +
                                               std::to_string(consent_sequence.fetch_add(1)),
                                           {}};
         }});
    REQUIRE(daemon.start());
    ControlClientConnection management(
        {.endpoint_path = daemon.endpoint_path(), .expected_broker_executable = broker});
    REQUIRE(management.connect());
    REQUIRE(management.manage("enroll").status_id == "accepted");
    auto selection = choc::value::createObject("");
    selection.addMember("host_id", "dspx04-graph-product");
    const auto prepared =
        management.manage("host-prepare-installed", choc::json::toString(selection, false), 10s);
    INFO(prepared.explanation);
    INFO(prepared.data_json);
    REQUIRE(prepared.status_id == "prepared");
    const auto prepared_data = choc::json::parse(prepared.data_json);
    REQUIRE(prepared_data["schema"].getString() == "pulp.control.host-prepare-installed.v1");
    REQUIRE(prepared_data["host_id"].getString() == "dspx04-graph-product");
    REQUIRE_FALSE(prepared_data["inventory_id"].getString().empty());
    auto launch = choc::value::createObject("");
    launch.addMember("inventory_id",
                     choc::value::createString(prepared_data["inventory_id"].getString()));
    // The product fixture is a real standalone control host. Keep CI's parent
    // environment from forcing the child into screenshot-only headless mode,
    // while retaining the deterministic null audio device for CI hardware.
    const std::array<const char*, 6> child_environment = {"CI",
                                                          "PULP_HEADLESS",
                                                          "PULP_TEST_MODE",
                                                          "PULP_SCREENSHOT",
                                                          "PULP_SCREENSHOT_PATH",
                                                          "PULP_SCREENSHOT_KEEP_AUDIO"};
    std::array<std::optional<std::string>, 6> saved_environment;
    for (std::size_t index = 0; index < child_environment.size(); ++index) {
        if (const auto* value = std::getenv(child_environment[index]))
            saved_environment[index] = value;
        REQUIRE(unsetenv(child_environment[index]) == 0);
    }
    REQUIRE(setenv("PULP_AUDIO_DEVICE", "null", 1) == 0);
    const auto launched =
        management.manage("host-launch", choc::json::toString(launch, false), 10s);
    REQUIRE(unsetenv("PULP_AUDIO_DEVICE") == 0);
    for (std::size_t index = 0; index < child_environment.size(); ++index) {
        if (saved_environment[index])
            REQUIRE(setenv(child_environment[index], saved_environment[index]->c_str(), 1) == 0);
    }
    INFO(launched.explanation);
    INFO(launched.data_json);
    REQUIRE(launched.status_id == "launched");
    const auto launch_data = choc::json::parse(launched.data_json);
    REQUIRE(launch_data["schema"].getString() == "pulp.control.host-launch.v1");
    REQUIRE(launch_data["inventory_id"].getString() == prepared_data["inventory_id"].getString());
    const auto replay = management.manage("host-launch", choc::json::toString(launch, false), 10s);
    CHECK(replay.status_id == "inventory_unavailable");
    const auto identity = wait_for_instance(management, "dev.pulp.test.dspx04-graph-product");
    const std::string instance(identity["instance_id"].getString());
    REQUIRE_FALSE(instance.empty());
    CHECK(identity["artifact_digest"].getString() == *source_digest);
    CHECK(identity["manifest_digest"].getString() == manifest_digest);
    CHECK_FALSE(identity["registration_id"].getString().empty());
    CHECK_FALSE(identity["publication_id"].getString().empty());

    const auto develop =
        run(cli, root.runtime,
            {"control", "grant-request", "--instance", instance, "--profile", "develop", "--json"});
    REQUIRE(develop.exit_code == 0);
    const std::string develop_grant(
        choc::json::parse(develop.stdout_output)["data"]["grant_id"].getString());
    REQUIRE_FALSE(develop_grant.empty());
    const auto lease =
        run(cli, root.runtime,
            {"control", "call", "--instance", instance, "dev.pulp.session/control@1", "--params",
             R"({"action":"acquire"})", "--json", "--grant", develop_grant});
    INFO(cli_failure_diagnostics(lease, daemon.state_directory() / "operations"));
    REQUIRE(lease.exit_code == 0);

    const auto call = [&](std::string params) {
        return run(cli, root.runtime,
                   {"control", "call", "--instance", instance,
                    "dev.pulp.graph/modulation-route.edit@1", "--params", std::move(params),
                    "--json", "--grant", develop_grant});
    };
    const auto applied = call(
        R"({"commands":[{"kind":"insert","source":1,"source_port":0,"destination":2,"parameter_id":7,"range_lo":0,"range_hi":1,"smoothing_ms":10}]})");
    INFO(cli_failure_diagnostics(applied, daemon.state_directory() / "operations"));
    REQUIRE(applied.exit_code == 0);
    const auto applied_receipt = choc::json::parse(applied.stdout_output);
    REQUIRE_FALSE(applied_receipt["receipt_id"].getString().empty());
    REQUIRE(applied_receipt["state"].getString() == "completed");
    REQUIRE(applied_receipt["detail"]["code"].getString() == "applied");
    REQUIRE(applied_receipt["detail"]["receipt_id"].getString() ==
            applied_receipt["receipt_id"].getString());
    REQUIRE(applied_receipt["detail"]["applied"].getInt64() == 1);
    REQUIRE(applied_receipt["detail"]["generation"].getInt64() >= 1);
    const auto applied_operation =
        wait_terminal_receipt(daemon.state_directory() / "operations",
                              std::string(applied_receipt["receipt_id"].getString()));
    require_instance_binding(applied_operation, identity);
    CHECK(applied_operation["manifest_digest"].getString() == manifest_digest);
    CHECK(applied_operation["producer_artifact_digest"].getString() == *source_digest);
    CHECK_FALSE(applied_operation["instance_generation"].getString().empty());

    const auto release_for_mcp =
        run(cli, root.runtime,
            {"control", "call", "--instance", instance, "dev.pulp.session/control@1", "--params",
             R"({"action":"release"})", "--json", "--grant", develop_grant});
    REQUIRE(release_for_mcp.exit_code == 0);

    // Exercise the generated MCP projection against the same live product
    // instance and operation schema. The broker session and grant are kept
    // inside this E2E, so this receipt cannot be reconstructed after the test.
    const auto mcp_rewired = run_mcp(
        mcp, root.runtime,
        mcp_call(2, "pulp_control_session_control", instance, R"({"action":"acquire"})") +
            mcp_call(
                3, "pulp_control_graph_modulation_route_edit", instance,
                R"({"commands":[{"kind":"rewire","source":1,"source_port":0,"destination":2,"parameter_id":7,"previous_source":1,"previous_source_port":0,"range_lo":0.0,"range_hi":1.0}]})") +
            mcp_call(4, "pulp_control_session_control", instance, R"({"action":"release"})"));
    REQUIRE(mcp_rewired.size() == 3);
    REQUIRE_FALSE(mcp_rewired[0]["result"]["isError"].getWithDefault<bool>(false));
    REQUIRE(mcp_rewired[0]["result"]["structuredContent"]["state"].getString() == "completed");
    const auto& mcp_response = mcp_rewired[1];
    REQUIRE_FALSE(mcp_response["result"]["isError"].getWithDefault<bool>(false));
    const auto mcp_structured = mcp_response["result"]["structuredContent"];
    REQUIRE(mcp_structured["schema"].getString() == "dev.pulp.control/mcp-receipt@1");
    REQUIRE(mcp_structured["operation_id"].getString() == "dev.pulp.graph/modulation-route.edit@1");
    REQUIRE(mcp_structured["state"].getString() == "completed");
    const auto mcp_result = mcp_structured["result"];
    REQUIRE(mcp_result["code"].getString() == "applied");
    REQUIRE_FALSE(mcp_result["receipt_id"].getString().empty());
    REQUIRE(mcp_result["generation"].getInt64() >=
            applied_receipt["detail"]["generation"].getInt64());
    REQUIRE(mcp_result["applied"].getInt64() == 1);
    REQUIRE_FALSE(mcp_rewired[2]["result"]["isError"].getWithDefault<bool>(false));
    REQUIRE(mcp_rewired[2]["result"]["structuredContent"]["state"].getString() == "completed");

    const auto reacquired =
        run(cli, root.runtime,
            {"control", "call", "--instance", instance, "dev.pulp.session/control@1", "--params",
             R"({"action":"acquire"})", "--json", "--grant", develop_grant});
    REQUIRE(reacquired.exit_code == 0);

    const auto removed = call(
        R"({"commands":[{"kind":"remove","source":1,"source_port":0,"destination":2,"parameter_id":7}]})");
    INFO(cli_failure_diagnostics(removed, daemon.state_directory() / "operations"));
    REQUIRE(removed.exit_code == 0);
    const auto removed_receipt = choc::json::parse(removed.stdout_output);
    REQUIRE(removed_receipt["state"].getString() == "completed");
    REQUIRE(removed_receipt["detail"]["code"].getString() == "applied");
    REQUIRE(removed_receipt["detail"]["applied"].getInt64() == 1);

    const auto stale_generation = call(
        R"({"commands":[{"kind":"remove","source":1,"source_port":0,"destination":2,"parameter_id":7}]})");
    INFO(cli_failure_diagnostics(stale_generation, daemon.state_directory() / "operations"));
    CHECK(stale_generation.exit_code != 0);
    CHECK(stale_generation.exit_code != 0);

    const auto invalid = call(
        R"({"commands":[{"kind":"unknown","source":1,"source_port":0,"destination":2,"parameter_id":7}]})");
    INFO(cli_failure_diagnostics(invalid, daemon.state_directory() / "operations"));
    REQUIRE(invalid.exit_code != 0);
    REQUIRE(invalid.stdout_output.find("invalid-request") != std::string::npos);

    const auto oversized = call(
        R"({"commands":[{"kind":"remove","source":4294967297,"source_port":0,"destination":2,"parameter_id":7}]})");
    INFO(cli_failure_diagnostics(oversized, daemon.state_directory() / "operations"));
    REQUIRE(oversized.exit_code != 0);
    REQUIRE(oversized.stdout_output.find("invalid-request") != std::string::npos);

    std::string overflow = R"({"commands":[)";
    for (int i = 0; i < 65; ++i) {
        if (i)
            overflow += ",";
        overflow +=
            R"({"kind":"remove","source":1,"source_port":0,"destination":2,"parameter_id":7})";
    }
    overflow += "]}";
    const auto refused = call(std::move(overflow));
    INFO(cli_failure_diagnostics(refused, daemon.state_directory() / "operations"));
    REQUIRE(refused.exit_code != 0);
    // The canonical schema rejects a batch beyond the bounded dense capacity
    // before dispatch; the authority-level test separately proves the same
    // bound preserves graph state when a dense batch reaches the executor.
    REQUIRE(refused.stdout_output.find("invalid-request") != std::string::npos);

    const auto unbound = run(
        cli, root.runtime,
        {"control", "call", "--instance", "instance-unbound",
         "dev.pulp.graph/modulation-route.edit@1", "--params",
         R"({"commands":[{"kind":"remove","source":1,"source_port":0,"destination":2,"parameter_id":7}]})",
         "--json", "--grant", develop_grant});
    CHECK(unbound.exit_code != 0);
    const bool unbound_refused = unbound.stdout_output.find("not-found") != std::string::npos ||
                                 unbound.stdout_output.find("unavailable") != std::string::npos;
    CHECK(unbound_refused);

    auto revoke = choc::value::createObject("");
    revoke.addMember("grant_id", choc::value::createString(develop_grant));
    const auto revoked = management.manage("revoke", choc::json::toString(revoke, false), 10s);
    INFO(revoked.status_id);
    CHECK_FALSE(revoked.status_id.empty());
    const auto revoked_call = run(
        cli, root.runtime,
        std::vector<std::string>{
            "control", "call", "--instance", std::string(instance),
            "dev.pulp.graph/modulation-route.edit@1", "--params",
            R"({"commands":[{"kind":"remove","source":1,"source_port":0,"destination":2,"parameter_id":7}]})",
            "--json", "--grant", develop_grant});
    CHECK(revoked_call.exit_code != 0);
    CHECK_FALSE(revoked_call.stdout_output.empty());
#else
    SKIP("DSPX-04 product broker fixture is Apple-only");
#endif
}
