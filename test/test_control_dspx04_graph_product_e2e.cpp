#include "../inspect/src/control_broker_daemon.hpp"
#include "support/control_sample_region_e2e.hpp"

#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/crypto.hpp>

#include <fstream>
#include <iterator>
#include <set>

TEST_CASE("DSPX-04 product binding reaches modulation route through broker",
          "[inspect][control][dspx-04][e2e]") {
#ifdef __APPLE__
    Root root;
    const auto executable = self();
    REQUIRE_FALSE(executable.empty());
    const auto bin = executable.parent_path();
    const auto cli = bin / "pulp";
    const auto broker = bin / "pulp-control-broker";
    const auto built_host = std::filesystem::path{PULP_DSPX04_GRAPH_PRODUCT_FIXTURE};
    REQUIRE(std::filesystem::exists(cli));
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
    auto launch = choc::value::createObject("");
    launch.addMember("inventory_id",
                     choc::value::createString(
                         choc::json::parse(prepared.data_json)["inventory_id"].getString()));
    const auto launched =
        management.manage("host-launch", choc::json::toString(launch, false), 10s);
    INFO(launched.explanation);
    INFO(launched.data_json);
    REQUIRE(launched.status_id == "launched");
    const auto identity = wait_for_instance(management, "dev.pulp.test.dspx04-graph-product");
    const std::string instance(identity["instance_id"].getString());
    REQUIRE_FALSE(instance.empty());

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
    REQUIRE(applied_receipt["state"].getString() == "completed");
    REQUIRE(applied_receipt["detail"]["code"].getString() == "applied");
    REQUIRE(applied_receipt["detail"]["applied"].getInt64() == 1);

    const auto rewired = call(
        R"({"commands":[{"kind":"rewire","source":1,"source_port":0,"destination":2,"parameter_id":7,"previous_source":1,"previous_source_port":0,"range_lo":0.0,"range_hi":1.0}]})");
    INFO(cli_failure_diagnostics(rewired, daemon.state_directory() / "operations"));
    REQUIRE(rewired.exit_code == 0);
    const auto rewired_receipt = choc::json::parse(rewired.stdout_output);
    REQUIRE(rewired_receipt["state"].getString() == "completed");
    REQUIRE(rewired_receipt["detail"]["code"].getString() == "applied");
    REQUIRE(rewired_receipt["detail"]["applied"].getInt64() == 1);

    const auto removed = call(
        R"({"commands":[{"kind":"remove","source":1,"source_port":0,"destination":2,"parameter_id":7}]})");
    INFO(cli_failure_diagnostics(removed, daemon.state_directory() / "operations"));
    REQUIRE(removed.exit_code == 0);
    const auto removed_receipt = choc::json::parse(removed.stdout_output);
    REQUIRE(removed_receipt["state"].getString() == "completed");
    REQUIRE(removed_receipt["detail"]["code"].getString() == "applied");
    REQUIRE(removed_receipt["detail"]["applied"].getInt64() == 1);

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
#else
    SKIP("DSPX-04 product broker fixture is Apple-only");
#endif
}
