#include "../inspect/src/control_broker_daemon.hpp"
#include "support/control_sample_region_e2e.hpp"

#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

#ifdef __APPLE__
std::string vendor_call(int id, std::string_view instance, std::string_view operation,
                        std::string_view input, std::string_view profile = {}) {
    auto arguments = choc::value::createObject("");
    arguments.setMember("instance_id", std::string(instance));
    arguments.setMember("operation_id", std::string(operation));
    if (!profile.empty())
        arguments.setMember("profile", std::string(profile));
    arguments.setMember("input", choc::json::parse(input));
    auto params = choc::value::createObject("");
    params.setMember("name", "pulp_control_vendor_call");
    params.setMember("arguments", arguments);
    auto request = choc::value::createObject("");
    request.setMember("jsonrpc", "2.0");
    request.setMember("id", id);
    request.setMember("method", "tools/call");
    request.setMember("params", params);
    return choc::json::toString(request, false) + "\n";
}

std::string instances_call(int id) {
    return std::string(R"({"jsonrpc":"2.0","id":)") + std::to_string(id) +
           R"(,"method":"tools/call","params":{"name":"pulp_control_instances","arguments":{}}})" +
           "\n";
}
#endif

} // namespace

TEST_CASE("vendor operations reach a live product through the broker, CLI, and MCP",
          "[inspect][control][vendor][e2e]") {
#ifdef __APPLE__
    Root root;
    const auto executable = self();
    REQUIRE_FALSE(executable.empty());
    const auto bin = executable.parent_path();
    const auto cli = bin / "pulp";
    const auto mcp = bin / "pulp-mcp";
    const auto broker = bin / "pulp-control-broker";
    const auto built_host = std::filesystem::path{PULP_VENDOR_PRODUCT_FIXTURE};
    REQUIRE(std::filesystem::exists(cli));
    REQUIRE(std::filesystem::exists(mcp));
    REQUIRE(std::filesystem::exists(broker));
    // The broker enrolls this process only as the trusted client copy beside
    // it; a copy overwritten by another target fails here.
    REQUIRE(pulp::runtime::sha256_file_hex(bin / "tools" / "cli" / "pulp-cpp",
                                           1024ULL * 1024 * 1024) ==
            pulp::runtime::sha256_file_hex(executable, 1024ULL * 1024 * 1024));
    REQUIRE(std::filesystem::exists(built_host));
    const auto host_dir = root.path / "product";
    REQUIRE(std::filesystem::create_directory(host_dir));
    const auto host = host_dir / built_host.filename();
    REQUIRE(std::filesystem::copy_file(built_host, host));
    REQUIRE(std::filesystem::copy_file(built_host.string() + ".inspector-capabilities.json",
                                       host.string() + ".inspector-capabilities.json"));
    std::ifstream manifest_stream(host.string() + ".inspector-capabilities.json");
    const std::string manifest_bytes(std::istreambuf_iterator<char>{manifest_stream}, {});
    const auto manifest = choc::json::parse(manifest_bytes);
    REQUIRE(manifest["bundle_id"].getString() == "org.example.vendor-product");
    // The declaration rides in the manifest whose digest consent binds.
    REQUIRE(manifest["vendor_operations"].isString());
    REQUIRE(std::string(manifest["vendor_operations"].getString()).find("level.set@1") !=
            std::string::npos);
    const auto manifest_digest = pulp::runtime::sha256_hex(manifest_bytes);
    std::filesystem::permissions(host, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
    std::filesystem::permissions(host.string() + ".inspector-capabilities.json",
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace);

    std::mutex consent_mutex;
    std::vector<std::string> consented_selectors;
    std::atomic<std::uint64_t> consent_sequence{0};
    ControlBrokerDaemon daemon(
        {.runtime_root = root.runtime,
         .state_root = root.state,
         .sdk_version = "vendor-product",
         .executable_path = executable,
         .process_generation = 405,
         .installed_host_selections =
             {{.host_id = "vendor-product",
               .intent = ControlTrustedHostLaunchIntent{.executable = host,
                                                        .arguments = {},
                                                        .working_directory = host.parent_path(),
                                                        .host_tier = ControlHostTier::Standalone}}},
         .decide_consent = [&](const ControlGrantConsentRequest& request) {
             {
                 std::lock_guard lock(consent_mutex);
                 consented_selectors.push_back(request.selector_id);
             }
             return ControlConsentDecision{true,
                                           ControlConsentAuthority::TrustedHostUi,
                                           "vendor-e2e-consent-" +
                                               std::to_string(consent_sequence.fetch_add(1)),
                                           {}};
         }});
    REQUIRE(daemon.start());
    ControlClientConnection management(
        {.endpoint_path = daemon.endpoint_path(), .expected_broker_executable = broker});
    REQUIRE(management.connect());
    REQUIRE(management.manage("enroll").status_id == "accepted");
    auto selection = choc::value::createObject("");
    selection.addMember("host_id", "vendor-product");
    const auto prepared =
        management.manage("host-prepare-installed", choc::json::toString(selection, false), 10s);
    INFO(prepared.explanation);
    REQUIRE(prepared.status_id == "prepared");
    auto launch = choc::value::createObject("");
    launch.addMember("inventory_id",
                     choc::value::createString(
                         choc::json::parse(prepared.data_json)["inventory_id"].getString()));
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
    for (std::size_t index = 0; index < child_environment.size(); ++index)
        if (saved_environment[index])
            REQUIRE(setenv(child_environment[index], saved_environment[index]->c_str(), 1) == 0);
    INFO(launched.explanation);
    REQUIRE(launched.status_id == "launched");

    const auto identity = wait_for_instance(management, "org.example.vendor-product");
    const std::string instance(identity["instance_id"].getString());
    REQUIRE_FALSE(instance.empty());
    CHECK(identity["manifest_digest"].getString() == manifest_digest);

    // Discovery lists exactly the declared operations with tier and schemas.
    const auto listed = identity["vendor_operations"];
    REQUIRE(listed.isArray());
    REQUIRE(listed.size() == 6);
    std::vector<std::string> listed_ids;
    for (const auto entry : listed) {
        listed_ids.emplace_back(entry["id"].getString());
        CHECK(entry["input_schema"].isObject());
        CHECK(entry["output_schema"].isObject());
        CHECK_FALSE(entry["description"].getString().empty());
    }
    CHECK(listed_ids == std::vector<std::string>{
                            "dev.org.example/level.read@1", "dev.org.example/level.set@1",
                            "dev.org.example/render.ping@1", "dev.org.example/render.unbound@1",
                            "dev.org.example/preset.delete@1", "dev.org.example/preset.purge@1"});
    CHECK(listed[1]["tier"].getString() == "gesture");
    CHECK(listed[1]["capability"].getString() == "dev.pulp.vendor/gesture@1");

    const auto grant = [&](std::vector<std::string> selector) {
        std::vector<std::string> arguments{"control", "grant-request", "--instance", instance};
        arguments.insert(arguments.end(), selector.begin(), selector.end());
        arguments.emplace_back("--json");
        const auto issued = run(cli, root.runtime, arguments);
        INFO(issued.stdout_output);
        REQUIRE(issued.exit_code == 0);
        return std::string(choc::json::parse(issued.stdout_output)["data"]["grant_id"].getString());
    };
    const auto call = [&](std::string operation, std::string params, std::string grant_id) {
        std::vector<std::string> arguments{
            "control",         "call",  "--instance", instance, std::move(operation), "--params",
            std::move(params), "--json"};
        if (!grant_id.empty()) {
            arguments.emplace_back("--grant");
            arguments.emplace_back(std::move(grant_id));
        }
        return run(cli, root.runtime, arguments);
    };
    const auto operations = daemon.state_directory() / "operations";

    // Read tier through the read-only profile.
    const auto observe_grant = grant({"--profile", "observe"});
    const auto read = call("dev.org.example/level.read@1", "{}", observe_grant);
    INFO(cli_failure_diagnostics(read, operations));
    REQUIRE(read.exit_code == 0);
    const auto read_receipt = choc::json::parse(read.stdout_output);
    CHECK(read_receipt["state"].getString() == "completed");
    CHECK(std::abs(read_receipt["detail"]["level"].getFloat64() - 0.25) < 1e-6);

    // The read-only grant does not reach the gesture tier.
    const auto escalated = call("dev.org.example/level.set@1", R"({"level":0.5})", observe_grant);
    CHECK(escalated.exit_code != 0);

    // Gesture tier: develop profile plus the controller lease.
    const auto develop_grant = grant({"--profile", "develop"});
    const auto lease = call("dev.pulp.session/control@1", R"({"action":"acquire"})", develop_grant);
    INFO(cli_failure_diagnostics(lease, operations));
    REQUIRE(lease.exit_code == 0);
    const auto set = call("dev.org.example/level.set@1", R"({"level":0.75})", develop_grant);
    INFO(cli_failure_diagnostics(set, operations));
    REQUIRE(set.exit_code == 0);
    const auto set_receipt = choc::json::parse(set.stdout_output);
    REQUIRE(set_receipt["state"].getString() == "completed");
    CHECK(std::abs(set_receipt["detail"]["level"].getFloat64() - 0.75) < 1e-6);
    const auto durable =
        wait_terminal_receipt(operations, std::string(set_receipt["receipt_id"].getString()));
    require_instance_binding(durable, identity);
    CHECK(durable["manifest_digest"].getString() == manifest_digest);
    CHECK(durable["operation_id"].getString() == "dev.org.example/level.set@1");

    const auto reread = call("dev.org.example/level.read@1", "{}", observe_grant);
    REQUIRE(reread.exit_code == 0);
    CHECK(std::abs(choc::json::parse(reread.stdout_output)["detail"]["level"].getFloat64() - 0.75) <
          1e-6);

    // Arguments outside the declared schema never reach the handler.
    const auto out_of_range = call("dev.org.example/level.set@1", R"({"level":2})", develop_grant);
    CHECK(out_of_range.exit_code != 0);
    CHECK(out_of_range.stdout_output.find("invalid-request") != std::string::npos);

    // A handler the product bound without declaring it is unreachable.
    const auto undeclared = call("dev.org.example/level.reset@1", "{}", develop_grant);
    CHECK(undeclared.exit_code != 0);
    const auto after_undeclared = call("dev.org.example/level.read@1", "{}", observe_grant);
    REQUIRE(after_undeclared.exit_code == 0);
    CHECK(
        std::abs(choc::json::parse(after_undeclared.stdout_output)["detail"]["level"].getFloat64() -
                 0.75) < 1e-6);

    // Render tier is outside every profile and needs its own consent.
    const auto render_with_profile = call("dev.org.example/render.ping@1", "{}", develop_grant);
    CHECK(render_with_profile.exit_code != 0);
    const auto ping_grant = grant({"--operation", "dev.org.example/render.ping@1"});
    const auto ping = call("dev.org.example/render.ping@1", "{}", ping_grant);
    INFO(cli_failure_diagnostics(ping, operations));
    REQUIRE(ping.exit_code == 0);
    CHECK(choc::json::parse(ping.stdout_output)["detail"]["rendered"].getBool());
    // That consent named one operation; a sibling of the same tier is refused.
    const auto sibling = call("dev.org.example/render.unbound@1", "{}", ping_grant);
    CHECK(sibling.exit_code != 0);
    CHECK(sibling.stdout_output.find("not-implemented") == std::string::npos);
    // With its own consent, the declared operation without a handler fails
    // as not implemented rather than succeeding or crashing the host.
    const auto unbound_grant = grant({"--operation", "dev.org.example/render.unbound@1"});
    const auto unbound = call("dev.org.example/render.unbound@1", "{}", unbound_grant);
    const auto unbound_diagnostics = cli_failure_diagnostics(unbound, operations);
    INFO(unbound_diagnostics);
    CHECK(unbound.exit_code != 0);
    {
        std::lock_guard lock(consent_mutex);
        CHECK(std::ranges::find(consented_selectors, "dev.org.example/render.ping@1") !=
              consented_selectors.end());
    }

    const auto release =
        call("dev.pulp.session/control@1", R"({"action":"release"})", develop_grant);
    REQUIRE(release.exit_code == 0);

    // Destructive tier: a single-operation grant, the controller lease, and
    // confinement to the consented id.
    const auto level_now = [&] {
        const auto current = call("dev.org.example/level.read@1", "{}", observe_grant);
        REQUIRE(current.exit_code == 0);
        return choc::json::parse(current.stdout_output)["detail"]["level"].getFloat64();
    };
    const auto profile_delete =
        call("dev.org.example/preset.delete@1", R"({"name":"Lead"})", develop_grant);
    CHECK(profile_delete.exit_code != 0);
    const auto delete_grant = grant({"--operation", "dev.org.example/preset.delete@1"});
    const auto without_lease =
        call("dev.org.example/preset.delete@1", R"({"name":"Lead"})", delete_grant);
    INFO(cli_failure_diagnostics(without_lease, operations));
    CHECK(without_lease.exit_code != 0);
    CHECK(std::abs(level_now() - 0.75) < 1e-6);
    const auto destructive_lease =
        call("dev.pulp.session/control@1", R"({"action":"acquire"})", develop_grant);
    REQUIRE(destructive_lease.exit_code == 0);
    const auto deleted =
        call("dev.org.example/preset.delete@1", R"({"name":"Lead"})", delete_grant);
    INFO(cli_failure_diagnostics(deleted, operations));
    REQUIRE(deleted.exit_code == 0);
    const auto deleted_receipt = choc::json::parse(deleted.stdout_output);
    CHECK(deleted_receipt["detail"]["deleted"].getBool());
    const auto deleted_durable =
        wait_terminal_receipt(operations, std::string(deleted_receipt["receipt_id"].getString()));
    CHECK(deleted_durable["capability_id"].getString() == "dev.pulp.vendor/destructive@1");
    CHECK(deleted_durable["operation_id"].getString() == "dev.org.example/preset.delete@1");
    CHECK(level_now() == 0.0);
    // The lease is held, yet the confined grant still refuses the sibling
    // before any handler runs.
    const auto purge = call("dev.org.example/preset.purge@1", "{}", delete_grant);
    CHECK(purge.exit_code != 0);
    CHECK(cli_failure_diagnostics(purge, operations).find("not_implemented") == std::string::npos);
    {
        std::lock_guard lock(consent_mutex);
        CHECK(std::ranges::find(consented_selectors, "dev.org.example/preset.delete@1") !=
              consented_selectors.end());
    }
    const auto destructive_release =
        call("dev.pulp.session/control@1", R"({"action":"release"})", develop_grant);
    REQUIRE(destructive_release.exit_code == 0);

    // The same live instance through pulp-mcp.
    const auto responses = run_mcp(
        mcp, root.runtime,
        instances_call(1) +
            mcp_call(2, "pulp_control_session_control", instance, R"({"action":"acquire"})") +
            vendor_call(3, instance, "dev.org.example/level.set@1", R"({"level":0.5})", "develop") +
            vendor_call(4, instance, "dev.org.example/level.read@1", "{}") +
            vendor_call(5, instance, "dev.org.example/render.ping@1", "{}") +
            vendor_call(6, instance, "dev.org.example/level.reset@1", "{}") +
            vendor_call(7, instance, "dev.org.example/level.set@1", R"({"level":-1})", "develop") +
            mcp_call(8, "pulp_control_session_control", instance, R"({"action":"release"})"));
    REQUIRE(responses.size() == 8);
    const auto structured = [&](std::size_t index) {
        INFO(choc::json::toString(responses[index], false));
        REQUIRE_FALSE(responses[index]["result"]["isError"].getWithDefault<bool>(false));
        return choc::value::Value(responses[index]["result"]["structuredContent"]);
    };
    bool mcp_listed = false;
    const auto mcp_instances = structured(0);
    for (const auto item : mcp_instances["instances"])
        if (item["instance_id"].getString() == instance)
            mcp_listed =
                item["vendor_operations"].isArray() && item["vendor_operations"].size() == 6;
    CHECK(mcp_listed);
    CHECK(structured(1)["state"].getString() == "completed");
    const auto mcp_set = structured(2);
    CHECK(mcp_set["operation_id"].getString() == "dev.org.example/level.set@1");
    CHECK(mcp_set["state"].getString() == "completed");
    CHECK(std::abs(mcp_set["result"]["level"].getFloat64() - 0.5) < 1e-6);
    const auto mcp_read = structured(3);
    CHECK(std::abs(mcp_read["result"]["level"].getFloat64() - 0.5) < 1e-6);
    const auto mcp_ping = structured(4);
    CHECK(mcp_ping["result"]["rendered"].getBool());
    CHECK(responses[5]["result"]["isError"].getWithDefault<bool>(false));
    CHECK(choc::json::toString(responses[5], false).find("unknown-operation") != std::string::npos);
    CHECK(responses[6]["result"]["isError"].getWithDefault<bool>(false));
    CHECK(choc::json::toString(responses[6], false).find("invalid-arguments") != std::string::npos);
    CHECK(structured(7)["state"].getString() == "completed");
#else
    SKIP("the vendor operations product fixture is Apple-only");
#endif
}
