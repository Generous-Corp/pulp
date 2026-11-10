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

TEST_CASE("Graph file watch publishes through the standalone broker and CLI",
          "[inspect][control][dspx-04][e2e]") {
#ifdef __APPLE__
    Root root;
    const auto executable = self();
    REQUIRE_FALSE(executable.empty());
    const auto bin = executable.parent_path();
    const auto cli = bin / "pulp";
    const auto mcp = bin / "pulp-mcp";
    const auto broker = bin / "pulp-control-broker";
    const auto built_host = std::filesystem::path{PULP_GRAPH_FILE_RELOAD_GRAPH_PRODUCT_FIXTURE};
    REQUIRE(std::filesystem::exists(cli));
    REQUIRE(std::filesystem::exists(mcp));
    REQUIRE(std::filesystem::exists(broker));
    // The broker enrolls this process only as the trusted client copy beside
    // it. Another target overwriting that copy shows up here, not as an
    // unexplained enrollment denial.
    const auto trusted_client = bin / "tools" / "cli" / "pulp-cpp";
    REQUIRE(pulp::runtime::sha256_file_hex(trusted_client, 1024ULL * 1024 * 1024) ==
            pulp::runtime::sha256_file_hex(executable, 1024ULL * 1024 * 1024));
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
    REQUIRE(manifest["bundle_id"].getString() == "dev.pulp.test.graph-file-reload");
    REQUIRE(manifest["target"].getString() == "pulp-control-graph_file_reload-graph-product-fixture");
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
            route_seen || capability["id"].getString() == "dev.pulp.graph/file.reload@1";
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
         .sdk_version = "graph_file_reload-product",
         .executable_path = executable,
         .process_generation = 404,
         .installed_host_selections = {{.host_id = "graph_file_reload-graph-product", .intent = allow()}},
         .decide_consent = [&consent_sequence](const ControlGrantConsentRequest&) {
             return ControlConsentDecision{true,
                                           ControlConsentAuthority::TrustedHostUi,
                                           "graph_file_reload-e2e-consent-" +
                                               std::to_string(consent_sequence.fetch_add(1)),
                                           {}};
         }});
    REQUIRE(daemon.start());
    ControlClientConnection management(
        {.endpoint_path = daemon.endpoint_path(), .expected_broker_executable = broker});
    REQUIRE(management.connect());
    REQUIRE(management.manage("enroll").status_id == "accepted");
    auto selection = choc::value::createObject("");
    selection.addMember("host_id", "graph_file_reload-graph-product");
    const auto prepared =
        management.manage("host-prepare-installed", choc::json::toString(selection, false), 10s);
    INFO(prepared.explanation);
    INFO(prepared.data_json);
    REQUIRE(prepared.status_id == "prepared");
    const auto prepared_data = choc::json::parse(prepared.data_json);
    REQUIRE(prepared_data["schema"].getString() == "pulp.control.host-prepare-installed.v1");
    REQUIRE(prepared_data["host_id"].getString() == "graph_file_reload-graph-product");
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
    const auto identity = wait_for_instance(management, "dev.pulp.test.graph-file-reload");
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

    const auto project = std::filesystem::path{PULP_GRAPH_FILE_RELOAD_PROJECT_ROOT} / root.path.filename();
    REQUIRE(std::filesystem::create_directories(project / "build"));
    { std::ofstream file(project / "pulp.toml"); file << "[plugin]\nname = \"graph-reload-proof\"\n"; }
    { std::ofstream file(project / "CMakeLists.txt");
      file << "cmake_minimum_required(VERSION 3.24)\nproject(GraphReloadWatch NONE)\nadd_custom_target(proof)\n"; }
    pulp::platform::ProcessOptions configure_options;
    configure_options.capture_stdout = true;
    configure_options.capture_stderr = true;
    configure_options.timeout_ms = 10000;
    const auto configured = pulp::platform::ChildProcess::run("cmake",
        {"-S", project.string(), "-B", (project / "build").string(), "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=Release", "-DPULP_DEV_SHARED_UI=OFF"}, configure_options);
    INFO(configured.stderr_output);
    REQUIRE(configured.exit_code == 0);
    const auto file = project / "live.pat";
    const std::string patch = "# flow 1\nin : pulp/audio_in\ngain : pulp/gain\nout : pulp/audio_out\n"
                              "in.audio >> gain.audio\ngain.audio >> out.audio\ngain.gain = 0.75\n";
    { std::ofstream save(file); save << patch; }
    const auto watch = [&](const char* command, std::uint64_t generation,
                           bool acquire = true, std::string watch_grant = {},
                           bool revoke_after_publication = false) {
        if (watch_grant.empty()) watch_grant = develop_grant;
        pulp::platform::ProcessOptions options;
        options.capture_stdout = true;
        options.capture_stderr = true;
        options.timeout_ms = 210000;
        bool ready = false, lease_acquired = !acquire;
        bool publication_seen = false, owner_revoked = false;
        options.on_stdout_line = [&](std::string_view line) {
            if (line.starts_with("Dev loop active")) {
                ready = true;
                if (acquire) {
                    const auto acquired = run(cli, root.runtime,
                        {"control", "call", "--instance", instance, "dev.pulp.session/control@1", "--params",
                         R"({"action":"acquire"})", "--json", "--grant", develop_grant});
                    lease_acquired = acquired.exit_code == 0;
                }
            }
            if (revoke_after_publication && !publication_seen &&
                line.find("committed") != std::string_view::npos) {
                publication_seen = true;
                const auto revoked = run(cli, root.runtime,
                    {"control", "revoke", "--grant", watch_grant, "--json"});
                owner_revoked = revoked.exit_code == 0;
                // A second real save must not publish after the owner revokes
                // the caller's original grant while this watch is running.
                std::ofstream save(file);
                auto changed = patch;
                changed.replace(changed.find("0.75"), 4, "0.25");
                save << changed;
            }
        };
        options.working_directory = project.string();
        auto result = pulp::platform::ChildProcess::run("/usr/bin/env",
            {"TMPDIR=" + root.runtime.string(), "PULP_HOME=" + (project / "pulp-home").string(), cli.string(), command, "--target", "proof",
             "--no-dev-shared", "--graph-file", file.string(), "--graph-instance", instance,
             "--graph-grant", watch_grant, "--graph-generation", std::to_string(generation),
             "--graph-watch-polls", "12"}, options);
        INFO(result.stdout_output);
        INFO(result.stderr_output);
        INFO("watch exit=" << result.exit_code << " timed_out=" << result.timed_out);
        REQUIRE(ready);
        REQUIRE(lease_acquired);
        if (revoke_after_publication) {
            REQUIRE(publication_seen);
            REQUIRE(owner_revoked);
        }
        return result;
    };
    const auto dev = watch("dev", 0);
    INFO(dev.stdout_output); INFO(dev.stderr_output);
    REQUIRE(dev.exit_code == 0);
    REQUIRE_FALSE(dev.timed_out);
    REQUIRE(dev.stdout_output.find("committed") != std::string::npos);
    REQUIRE(dev.stdout_output.find("completed") != std::string::npos);
    { std::ofstream save(file); save << patch << "bad.port >> out.audio\n"; }
    const auto invalid = watch("dev", 1);
    INFO(invalid.stdout_output); INFO(invalid.stderr_output);
    REQUIRE(invalid.exit_code == 0);
    REQUIRE(invalid.stdout_output.find("invalid-document") != std::string::npos);
    REQUIRE(invalid.stdout_output.find("UnknownNode") != std::string::npos);
    { std::ofstream save(file); save << patch; }
    const auto loop = watch("loop", 1);
    INFO(loop.stdout_output); INFO(loop.stderr_output);
    REQUIRE(loop.exit_code == 0);
    REQUIRE_FALSE(loop.timed_out);
    REQUIRE(loop.stdout_output.find("committed") != std::string::npos);
    REQUIRE(loop.stdout_output.find("completed") != std::string::npos);
    const auto stale = watch("dev", 0);
    INFO(stale.stdout_output); INFO(stale.stderr_output);
    REQUIRE(stale.exit_code == 0);
    REQUIRE(stale.stdout_output.find("stale-base") != std::string::npos);
    const auto acquired = run(cli, root.runtime,
        {"control", "call", "--instance", instance, "dev.pulp.session/control@1", "--params",
         R"({"action":"acquire"})", "--json", "--grant", develop_grant});
    REQUIRE(acquired.exit_code == 0);
    const auto lease_receipt = choc::json::parse(acquired.stdout_output);
    const auto expires = lease_receipt["detail"]["expires_at_ms"].getInt64();
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    REQUIRE(expires > now);
    REQUIRE(expires - now <= 16000);
    std::this_thread::sleep_for(std::chrono::milliseconds(expires - now + 20));
    const auto expired = watch("dev", 2, false);
    INFO(expired.stdout_output); INFO(expired.stderr_output);
    INFO("expired exit=" << expired.exit_code << " timed_out=" << expired.timed_out);
    REQUIRE(expired.exit_code == 0);
    REQUIRE(expired.stderr_output.find("renewal refused") != std::string::npos);
    REQUIRE(expired.stdout_output.find("committed") == std::string::npos);

    const auto restricted = run(cli, root.runtime,
        {"control", "grant-request", "--instance", instance, "--operation",
         "dev.pulp.graph/file.reload@1", "--json"});
    REQUIRE(restricted.exit_code == 0);
    const auto restricted_doc = choc::json::parse(restricted.stdout_output);
    const std::string restricted_grant(restricted_doc["data"]["grant_id"].getString());
    const auto denied_renewal = watch("dev", 2, true, restricted_grant);
    INFO(denied_renewal.stdout_output); INFO(denied_renewal.stderr_output);
    REQUIRE(denied_renewal.exit_code == 0);
    REQUIRE(denied_renewal.stderr_output.find("renewal refused") != std::string::npos);
    REQUIRE(denied_renewal.stdout_output.find("committed") == std::string::npos);
    const auto revoked_watch = watch("dev", 2, true, develop_grant, true);
    INFO(revoked_watch.stdout_output); INFO(revoked_watch.stderr_output);
    REQUIRE(revoked_watch.exit_code == 0);
    REQUIRE(revoked_watch.stderr_output.find("renewal refused") != std::string::npos);
    const auto first_commit = revoked_watch.stdout_output.find("committed");
    REQUIRE(first_commit != std::string::npos);
    REQUIRE(revoked_watch.stdout_output.find("committed", first_commit + 1) == std::string::npos);
    const auto final_grant_response = run(cli, root.runtime,
        {"control", "grant-request", "--instance", instance, "--profile", "develop", "--json"});
    REQUIRE(final_grant_response.exit_code == 0);
    const auto final_grant_doc = choc::json::parse(final_grant_response.stdout_output);
    const std::string final_grant(final_grant_doc["data"]["grant_id"].getString());
    const auto final_lease = run(cli, root.runtime,
        {"control", "call", "--instance", instance, "dev.pulp.session/control@1", "--params",
         R"({"action":"acquire"})", "--json", "--grant", final_grant});
    REQUIRE(final_lease.exit_code == 0);
    const auto unchanged_generation = run(cli, root.runtime,
        {"control", "call", "--instance", instance, "dev.pulp.graph/file.reload@1", "--params",
         R"({"text":"malformed","format":"notation","expected_generation":3})",
         "--json", "--grant", final_grant});
    REQUIRE(unchanged_generation.exit_code != 0);
    const auto final_receipt = choc::json::parse(unchanged_generation.stdout_output);
    REQUIRE(final_receipt["detail"]["generation"].getInt64() == 3);
    REQUIRE(final_receipt["detail"]["code"].getString() == "invalid-document");
    daemon.stop();
#else
    SKIP("Standalone broker product composition requires macOS; public numeric reload controls run portably");
#endif
}
