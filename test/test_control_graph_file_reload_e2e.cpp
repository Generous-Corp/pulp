#include "../inspect/src/control_broker_daemon.hpp"
#include "support/control_sample_region_e2e.hpp"

#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/crypto.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <string>

static std::size_t occurrences(const std::string& text, std::string_view needle) {
    std::size_t count = 0;
    for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
        ++count;
    return count;
}

TEST_CASE("Graph file watch publishes through the standalone broker and CLI",
          "[inspect][control][graph-file-reload][e2e]") {
#ifdef __APPLE__
    Root root;
    const auto executable = self();
    REQUIRE_FALSE(executable.empty());
    const auto bin = executable.parent_path();
    const auto cli = bin / "pulp";
    const auto mcp = bin / "pulp-mcp";
    const auto broker = bin / "pulp-control-broker";
    const auto built_host = bin / "pulp-control-graph_file_reload-graph-product-fixture";
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
    REQUIRE(manifest["target"].getString() ==
            "pulp-control-graph_file_reload-graph-product-fixture");
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
        route_seen = route_seen || capability["id"].getString() == "dev.pulp.graph/file.reload@1";
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
         .installed_host_selections = {{.host_id = "graph_file_reload-graph-product",
                                        .intent = allow()}},
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

    const auto project = bin / "watch-projects" / root.path.filename();
    REQUIRE(std::filesystem::create_directories(project / "build"));
    {
        std::ofstream file(project / "pulp.toml");
        file << "[plugin]\nname = \"graph-reload-proof\"\n";
    }
    {
        std::ofstream file(project / "CMakeLists.txt");
        file << "cmake_minimum_required(VERSION 3.24)\nproject(GraphReloadWatch "
                "NONE)\nadd_custom_target(proof)\n";
    }
    pulp::platform::ProcessOptions configure_options;
    configure_options.capture_stdout = true;
    configure_options.capture_stderr = true;
    configure_options.timeout_ms = 10000;
    const auto configured = pulp::platform::ChildProcess::run(
        "cmake",
        {"-S", project.string(), "-B", (project / "build").string(), "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=Release", "-DPULP_DEV_SHARED_UI=OFF"},
        configure_options);
    INFO(configured.stderr_output);
    REQUIRE(configured.exit_code == 0);
    const auto file = project / "live.pat";
    const std::string patch =
        "# flow 1\nin : pulp/audio_in\ngain : pulp/gain\nout : pulp/audio_out\n"
        "in.audio >> gain.audio\ngain.audio >> out.audio\ngain.gain = 0.75\n";
    {
        // The watch takes these starting contents as its baseline and never
        // applies them; each run saves its document once the watch is ready.
        std::ofstream save(file);
        save << "# baseline\n";
    }
    const auto child_script = project / "build" / "owned_child.py";
    const auto child_pid_file = project / "build" / "owned_child.pid";
    {
        std::ofstream script(child_script);
        script << "import os, sys, time\nwith open(sys.argv[1], 'w') as f: "
                  "f.write(str(os.getpid()))\ntime.sleep(120)\n";
    }
    std::string state_before_revocation;
    const auto watch = [&](const char* command, std::uint64_t generation,
                           const std::string& save_on_ready, bool acquire = true,
                           std::string watch_grant = {}, bool revoke_after_publication = false) {
        if (watch_grant.empty())
            watch_grant = develop_grant;
        pulp::platform::ProcessOptions options;
        options.capture_stdout = true;
        options.capture_stderr = true;
        options.timeout_ms = 210000;
        bool ready = false, lease_acquired = !acquire;
        std::chrono::steady_clock::time_point ready_at;
        bool publication_seen = false, owner_revoked = false;
        std::filesystem::remove(child_pid_file);
        int child_pid = -1;
        bool child_observed_alive = false;
        const auto observe_child = [&] {
            std::ifstream pid_stream(child_pid_file);
            int observed = -1;
            if (pid_stream >> observed && observed > 0) {
                child_pid = observed;
                child_observed_alive |= ::kill(child_pid, 0) == 0;
            }
        };
        options.on_stderr_line = [&](std::string_view) { observe_child(); };
        options.on_stdout_line = [&](std::string_view line) {
            if (line.starts_with("Dev loop active")) {
                ready = true;
                ready_at = std::chrono::steady_clock::now();
                if (acquire) {
                    const auto acquired =
                        run(cli, root.runtime,
                            {"control", "call", "--instance", instance,
                             "dev.pulp.session/control@1", "--params", R"({"action":"acquire"})",
                             "--json", "--grant", develop_grant});
                    lease_acquired = acquired.exit_code == 0;
                }
                std::ofstream save(file, std::ios::trunc);
                save << save_on_ready;
            }
            observe_child();
            if (revoke_after_publication && !publication_seen &&
                line.find("committed") != std::string_view::npos) {
                publication_seen = true;
                const auto observed =
                    run(cli, root.runtime,
                        {"control", "call", "--instance", instance, "dev.pulp.graph/file.reload@1",
                         "--params",
                         R"({"text":"malformed","format":"notation","expected_generation":3})",
                         "--json", "--grant", watch_grant});
                if (observed.exit_code != 0) {
                    const auto receipt = choc::json::parse(observed.stdout_output);
                    if (receipt["detail"]["generation"].getInt64() == 3 &&
                        receipt["detail"]["code"].getString() == "invalid-document") {
                        std::ifstream captured(host_dir / "graph-audio.json");
                        const std::string bytes(std::istreambuf_iterator<char>{captured}, {});
                        if (!bytes.empty())
                            state_before_revocation =
                                std::string(choc::json::parse(bytes)["state"].getString());
                    }
                }
                const auto revoked =
                    run(cli, root.runtime, {"control", "revoke", "--grant", watch_grant, "--json"});
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
        auto result =
            pulp::platform::ChildProcess::run("/usr/bin/env",
                                              {"PULP_BUILD_JOBS=2",
                                               "TMPDIR=" + root.runtime.string(),
                                               "PULP_HOME=" + (project / "pulp-home").string(),
                                               cli.string(),
                                               command,
                                               "--target",
                                               "proof",
                                               "--no-dev-shared",
                                               "--graph-file",
                                               file.string(),
                                               "--graph-instance",
                                               instance,
                                               "--graph-grant",
                                               watch_grant,
                                               "--graph-generation",
                                               std::to_string(generation),
                                               "--graph-watch-polls",
                                               "12",
                                               "--run",
                                               "/usr/bin/python3",
                                               "--",
                                               child_script.string(),
                                               child_pid_file.string()},
                                              options);
        observe_child();
        const bool child_terminated = child_pid > 0 && ::kill(child_pid, 0) != 0 && errno == ESRCH;
        // Keep a planted cleanup failure from leaving a test-owned sleeper.
        if (child_pid > 0 && !child_terminated)
            (void)::kill(child_pid, SIGTERM);
        INFO(result.stdout_output);
        INFO(result.stderr_output);
        INFO("watch exit=" << result.exit_code << " timed_out=" << result.timed_out);
        REQUIRE(ready);
        REQUIRE(lease_acquired);
        REQUIRE(std::chrono::steady_clock::now() - ready_at < std::chrono::seconds(45));
        REQUIRE(child_observed_alive);
        REQUIRE(child_terminated);
        if (revoke_after_publication) {
            REQUIRE(publication_seen);
            REQUIRE(owner_revoked);
        }
        return result;
    };
    const auto audio_state = [&](std::uint64_t generation, const std::string& grant,
                                 double expected_sample) {
        const auto refused = run(
            cli, root.runtime,
            {"control", "call", "--instance", instance, "dev.pulp.graph/file.reload@1", "--params",
             "{\"text\":\"malformed\",\"format\":\"notation\",\"expected_generation\":" +
                 std::to_string(generation) + "}",
             "--json", "--grant", grant});
        INFO(refused.stdout_output);
        INFO(refused.stderr_output);
        REQUIRE(refused.exit_code != 0);
        const auto receipt = choc::json::parse(refused.stdout_output);
        REQUIRE(receipt["detail"]["generation"].getInt64() == generation);
        REQUIRE(receipt["detail"]["code"].getString() == "invalid-document");
        std::ifstream captured(host_dir / "graph-audio.json");
        const std::string bytes(std::istreambuf_iterator<char>{captured}, {});
        REQUIRE_FALSE(bytes.empty());
        INFO(bytes);
        const auto evidence = choc::json::parse(bytes);
        REQUIRE(evidence["process_count"].getInt64() > 0);
        REQUIRE(evidence["sample"].getFloat64() == expected_sample);
        return std::string(evidence["state"].getString());
    };
    const auto initial_state = audio_state(0, develop_grant, 0.125);
    REQUIRE_FALSE(initial_state.empty());
    const auto dev = watch("dev", 0, patch);
    INFO(dev.stdout_output);
    INFO(dev.stderr_output);
    REQUIRE(dev.exit_code == 0);
    REQUIRE_FALSE(dev.timed_out);
    REQUIRE(dev.stdout_output.find("committed") != std::string::npos);
    REQUIRE(dev.stdout_output.find("completed") != std::string::npos);
    const auto invalid = watch("dev", 1, patch + "bad.port >> out.audio\n");
    INFO(invalid.stdout_output);
    INFO(invalid.stderr_output);
    REQUIRE(invalid.exit_code == 0);
    REQUIRE(invalid.stdout_output.find("invalid-document") != std::string::npos);
    REQUIRE(invalid.stdout_output.find("UnknownNode") != std::string::npos);
    const auto loop = watch("loop", 1, patch);
    INFO(loop.stdout_output);
    INFO(loop.stderr_output);
    REQUIRE(loop.exit_code == 0);
    REQUIRE_FALSE(loop.timed_out);
    REQUIRE(loop.stdout_output.find("committed") != std::string::npos);
    REQUIRE(loop.stdout_output.find("completed") != std::string::npos);
    const auto stale = watch("dev", 0, patch + "# stale generation\n");
    INFO(stale.stdout_output);
    INFO(stale.stderr_output);
    REQUIRE(stale.exit_code == 0);
    REQUIRE(stale.stdout_output.find("stale-base") != std::string::npos);
    const auto before_refusals = audio_state(2, develop_grant, 0.375);
    const auto acquired =
        run(cli, root.runtime,
            {"control", "call", "--instance", instance, "dev.pulp.session/control@1", "--params",
             R"({"action":"acquire"})", "--json", "--grant", develop_grant});
    REQUIRE(acquired.exit_code == 0);
    const auto lease_receipt = choc::json::parse(acquired.stdout_output);
    const auto expires = lease_receipt["detail"]["expires_at_ms"].getInt64();
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    REQUIRE(expires > now);
    REQUIRE(expires - now <= 16000);
    std::this_thread::sleep_for(std::chrono::milliseconds(expires - now + 20));
    const auto expired = watch("dev", 2, patch + "# expired lease\n", false);
    INFO(expired.stdout_output);
    INFO(expired.stderr_output);
    INFO("expired exit=" << expired.exit_code << " timed_out=" << expired.timed_out);
    REQUIRE(expired.exit_code == 0);
    REQUIRE(expired.stderr_output.find("renewal refused") != std::string::npos);
    // One report per distinct refusal (expired, then not owned), not one per renewal.
    REQUIRE(occurrences(expired.stderr_output, "renewal refused") <= 2);
    REQUIRE(expired.stdout_output.find("committed") == std::string::npos);

    const auto restricted = run(cli, root.runtime,
                                {"control", "grant-request", "--instance", instance, "--operation",
                                 "dev.pulp.graph/file.reload@1", "--json"});
    REQUIRE(restricted.exit_code == 0);
    const auto restricted_doc = choc::json::parse(restricted.stdout_output);
    const std::string restricted_grant(restricted_doc["data"]["grant_id"].getString());
    const auto denied_renewal =
        watch("dev", 2, patch + "# denied renewal\n", true, restricted_grant);
    INFO(denied_renewal.stdout_output);
    INFO(denied_renewal.stderr_output);
    REQUIRE(denied_renewal.exit_code == 0);
    REQUIRE(denied_renewal.stderr_output.find("renewal refused") != std::string::npos);
    // Admission refuses this grant for session control: stop once, naming the fix.
    REQUIRE(occurrences(denied_renewal.stderr_output, "renewal refused") == 1);
    REQUIRE(denied_renewal.stderr_output.find("stopped for this watch") != std::string::npos);
    REQUIRE(denied_renewal.stdout_output.find("committed") == std::string::npos);
    CHECK(audio_state(2, develop_grant, 0.375) == before_refusals);
    const auto revoked_watch = watch("dev", 2, patch + "# revocation\n", true, develop_grant, true);
    INFO(revoked_watch.stdout_output);
    INFO(revoked_watch.stderr_output);
    REQUIRE(revoked_watch.exit_code == 0);
    REQUIRE(revoked_watch.stderr_output.find("renewal refused") != std::string::npos);
    // The revoked grant is refused at the renewal before the next send, once,
    // and the watch stops; no reload request reaches admission after revocation.
    REQUIRE(occurrences(revoked_watch.stderr_output, "renewal refused") == 1);
    REQUIRE(revoked_watch.stderr_output.find("stopped for this watch") != std::string::npos);
    REQUIRE(revoked_watch.stdout_output.find("admission-denied") == std::string::npos);
    const auto first_commit = revoked_watch.stdout_output.find("committed");
    REQUIRE(first_commit != std::string::npos);
    REQUIRE(revoked_watch.stdout_output.find("committed", first_commit + 1) == std::string::npos);
    const auto final_grant_response =
        run(cli, root.runtime,
            {"control", "grant-request", "--instance", instance, "--profile", "develop", "--json"});
    REQUIRE(final_grant_response.exit_code == 0);
    const auto final_grant_doc = choc::json::parse(final_grant_response.stdout_output);
    const std::string final_grant(final_grant_doc["data"]["grant_id"].getString());
    const auto final_lease =
        run(cli, root.runtime,
            {"control", "call", "--instance", instance, "dev.pulp.session/control@1", "--params",
             R"({"action":"acquire"})", "--json", "--grant", final_grant});
    REQUIRE(final_lease.exit_code == 0);
    CHECK(audio_state(3, final_grant, 0.375) != initial_state);
    REQUIRE_FALSE(state_before_revocation.empty());
    CHECK(audio_state(3, final_grant, 0.375) == state_before_revocation);
    const auto unchanged_generation =
        run(cli, root.runtime,
            {"control", "call", "--instance", instance, "dev.pulp.graph/file.reload@1", "--params",
             R"({"text":"malformed","format":"notation","expected_generation":3})", "--json",
             "--grant", final_grant});
    REQUIRE(unchanged_generation.exit_code != 0);
    const auto final_receipt = choc::json::parse(unchanged_generation.stdout_output);
    REQUIRE(final_receipt["detail"]["generation"].getInt64() == 3);
    REQUIRE(final_receipt["detail"]["code"].getString() == "invalid-document");
    daemon.stop();
#else
    SKIP("Standalone broker product composition requires macOS; public numeric reload controls run "
         "portably");
#endif
}

static void require_watch_signal_cleanup(bool during_shellout) {
#ifdef __APPLE__
    Root root;
    REQUIRE(std::filesystem::create_directory(root.runtime));
    const auto cli = self().parent_path() / "pulp";
    const auto project = self().parent_path() / "watch-projects" / root.path.filename();
    REQUIRE(std::filesystem::create_directories(project / "build"));
    const auto build_dir = project / "build";
    const auto block = build_dir / "block";
    const auto build_ready = build_dir / "build-ready";
    const auto child_ready = build_dir / "child-ready";
    const auto script = build_dir / "same_sleeper.py";
    const auto build_script = build_dir / "build_step.py";
    std::ofstream(project / "pulp.toml") << "[plugin]\nname = \"watch-signal-proof\"\n";
    std::ofstream(project / "CMakeLists.txt")
        << "cmake_minimum_required(VERSION 3.24)\nproject(WatchSignal NONE)\n"
        << "add_custom_target(proof COMMAND /usr/bin/python3 \"" << build_script.string() << "\" \""
        << block.string() << "\" \"" << build_ready.string() << "\" USES_TERMINAL)\n";
    std::ofstream(script) << "import os, sys, time\nwith open(sys.argv[1], 'w') as f:\n"
                             " f.write(f'{os.getpid()} {os.getppid()} {os.getpgrp()}')\n"
                             "time.sleep(900)\n";
    std::ofstream(build_script)
        << "import os, sys, time\n"
           "assert os.environ.get('PULP_WATCH_ENV_PROOF') == 'inherited'\n"
           "if os.path.exists(sys.argv[1]):\n"
           " sys.stdout.buffer.write(b'watch-shellout-out\\x00tail\\n'); sys.stdout.flush()\n"
           " sys.stderr.write('watch-shellout-err\\n'); sys.stderr.flush()\n"
           " with open(sys.argv[2], 'w') as f:\n"
           "  f.write(f'{os.getpid()} {os.getppid()} {os.getpgrp()}')\n"
           " time.sleep(900)\n";
    const auto source = project / "change.cpp";
    std::ofstream(source) << "// initial source\n";
    pulp::platform::ProcessOptions configure;
    configure.capture_stdout = true;
    configure.capture_stderr = true;
    configure.timeout_ms = 10000;
    const auto configured = pulp::platform::ChildProcess::run(
        "cmake",
        {"-S", project.string(), "-B", build_dir.string(), "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=Release", "-DPULP_DEV_SHARED_UI=OFF"},
        configure);
    INFO(configured.stderr_output);
    REQUIRE(configured.exit_code == 0);
    // Same executable and script, different owned group. Name-based cleanup
    // would kill this positive control as well as the watch's child.
    pulp::platform::ChildProcess unrelated;
    REQUIRE(
        unrelated.start("/usr/bin/python3", {script.string(), (build_dir / "unrelated").string()}));
    REQUIRE(unrelated.is_running());
    for (const auto* command : {"dev", "loop"}) {
        for (const int stop_signal : {SIGINT, SIGTERM}) {
            INFO(command << " signal=" << stop_signal << " shellout=" << during_shellout);
            std::filesystem::remove(block);
            std::filesystem::remove(build_ready);
            std::filesystem::remove(child_ready);
            pulp::platform::ChildProcess watch;
            int child_pid = -1, build_pid = -1, build_group = -1;
            bool child_alive = false, parent_matches = false, signalled = false;
            bool shellout_alive = false;
            std::chrono::steady_clock::time_point signalled_at;
            struct SleeperCleanup {
                int& child;
                int& build;
                int& group;
                ~SleeperCleanup() {
                    if (child > 0 && ::getpgid(child) == child)
                        (void)::kill(-child, SIGKILL);
                    if (build > 0 && group > 0 && group != ::getpgrp() && ::getpgid(build) == group)
                        (void)::kill(-group, SIGKILL);
                }
            } cleanup{child_pid, build_pid, build_group};
            pulp::platform::ProcessOptions options;
            options.capture_stdout = true;
            options.capture_stderr = true;
            options.working_directory = project.string();
            options.timeout_ms = 150000;
            std::atomic<bool> stdout_seen{false}, stderr_seen{false};
            options.on_stdout_line = [&](std::string_view line) {
                if (line.find(std::string_view("watch-shellout-out\0tail", 23)) != line.npos)
                    stdout_seen.store(true, std::memory_order_relaxed);
            };
            options.on_stderr_line = [&](std::string_view line) {
                if (line == "watch-shellout-err")
                    stderr_seen.store(true, std::memory_order_relaxed);
            };
            std::vector<std::string> arguments{
                "PULP_BUILD_JOBS=2", "PULP_WATCH_ENV_PROOF=inherited",
                "TMPDIR=" + root.runtime.string(), "PULP_HOME=" + (project / "pulp-home").string()};
            for (const auto& argument : std::vector<std::string>{
                     cli.string(), command, "--target", "proof", "--no-dev-shared", "--run",
                     "/usr/bin/python3", "--", script.string(), child_ready.string()})
                arguments.push_back(argument);
            REQUIRE(watch.start("/usr/bin/env", arguments, options));
            const int watch_pid = watch.process_id();
            // Readiness comes from files: the existing launch path discards
            // child stdout. Capture the watch PID before wait() owns its lock.
            std::atomic<bool> finished{false};
            std::thread observer([&] {
                const auto deadline = std::chrono::steady_clock::now() + 145s;
                bool changed = false;
                while (!finished.load(std::memory_order_relaxed) &&
                       std::chrono::steady_clock::now() < deadline) {
                    if (child_pid <= 0) {
                        int pid = -1, parent = -1, group = -1;
                        std::ifstream ready(child_ready);
                        if (ready >> pid >> parent >> group && pid > 0 && parent == watch_pid &&
                            group == pid && ::getpgid(pid) == group) {
                            child_pid = pid;
                            parent_matches = true;
                            child_alive = ::kill(child_pid, 0) == 0;
                        }
                    }
                    if (child_alive && during_shellout && !changed) {
                        std::ofstream(block) << "block the next build\n";
                        const auto before = std::filesystem::last_write_time(source);
                        std::ofstream(source) << "// rebuild\n";
                        std::filesystem::last_write_time(source, before + 2s);
                        changed = true;
                    }
                    if (child_alive && during_shellout) {
                        int parent = -1;
                        std::ifstream ready(build_ready);
                        if (ready >> build_pid >> parent >> build_group && build_pid > 0 &&
                            build_group > 0 && build_group != ::getpgrp() &&
                            ::getpgid(build_pid) == build_group)
                            shellout_alive = ::kill(build_pid, 0) == 0;
                    }
                    // A worker's flush can still be inside cmake's pipe. Wait
                    // for forwarded bytes before killing that upstream pipe.
                    if (child_alive &&
                        (!during_shellout ||
                         (shellout_alive && stdout_seen.load(std::memory_order_relaxed) &&
                          stderr_seen.load(std::memory_order_relaxed)))) {
                        signalled_at = std::chrono::steady_clock::now();
                        signalled = ::kill(watch_pid, stop_signal) == 0;
                        break;
                    }
                    std::this_thread::sleep_for(20ms);
                }
            });
            // unbounded-wait: allow options.timeout_ms (150 s) bounds this child process
            const auto result = watch.wait();
            const auto returned_at = std::chrono::steady_clock::now();
            finished.store(true, std::memory_order_relaxed);
            observer.join();
            const auto absent = [](int pid) {
                return pid > 0 && ::kill(pid, 0) != 0 && errno == ESRCH;
            };
            const auto reap_deadline = std::chrono::steady_clock::now() + 2s;
            while (during_shellout && !absent(build_pid) &&
                   std::chrono::steady_clock::now() < reap_deadline)
                std::this_thread::sleep_for(20ms);
            INFO(result.stdout_output);
            INFO(result.stderr_output);
            REQUIRE(parent_matches);
            REQUIRE(child_alive);
            REQUIRE(signalled);
            REQUIRE_FALSE(result.timed_out);
            REQUIRE(result.exit_code == 128 + stop_signal);
            REQUIRE(returned_at - signalled_at < 10s);
            REQUIRE(absent(child_pid));
            REQUIRE(unrelated.is_running());
            if (during_shellout) {
                REQUIRE(shellout_alive);
                REQUIRE(absent(build_pid));
                REQUIRE(result.stdout_output.find(std::string("watch-shellout-out\0tail\n", 24)) !=
                        std::string::npos);
                REQUIRE(result.stderr_output.find("watch-shellout-err\n") != std::string::npos);
            }
        }
    }
    unrelated.cancel();
    // unbounded-wait: allow cancel() has already terminated and reaped this process group
    (void)unrelated.wait();
#else
    (void)during_shellout;
    SKIP("Product CLI watch signal proof runs with the macOS control fixture");
#endif
}

TEST_CASE("Dev and loop signals join their child without killing an unrelated sleeper",
          "[inspect][control][graph-file-reload][signal-cleanup]") {
    require_watch_signal_cleanup(false);
}

TEST_CASE("Dev and loop signals cancel an active rebuild and join both child groups",
          "[inspect][control][graph-file-reload][signal-cleanup][shellout-signal]") {
    require_watch_signal_cleanup(true);
}
