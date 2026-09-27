#include "support/control_sample_region_e2e.hpp"
#include <limits>

#if defined(__APPLE__) && !defined(PULP_SAMPLE_REGION_BROKER_PROOF_HOST)
#include <cerrno>
#include <csignal>
#include <libproc.h>
#include <sys/proc.h>
#include <sys/wait.h>
#endif

#ifdef PULP_SAMPLE_REGION_BROKER_PROOF_HOST
int main(int argc, char** argv) {
    return sample_region_broker_proof_host_main(argc, argv);
}
#else

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

    const auto capabilities = run(cli, root.runtime, {"control", "capabilities", "--json"});
    REQUIRE(capabilities.exit_code == 0);
    const auto registry = choc::json::parse(capabilities.stdout_output);
    REQUIRE(registry["schema"].getString() == "dev.pulp.control/registry@1");
    std::set<std::string> sample_contracts;
    for (const auto capability : registry["capabilities"]) {
        const std::string id(capability["id"].getString());
        if (id != "dev.pulp.graph/sample-region.read@1" &&
            id != "dev.pulp.graph/sample-region.edit@1")
            continue;
        sample_contracts.emplace(id);
        CHECK_FALSE(capability["operation"]["input_schema_digest"].getString().empty());
        CHECK_FALSE(capability["operation"]["output_schema_digest"].getString().empty());
    }
    REQUIRE(sample_contracts.size() == 2);

    const auto invalid_directory = root.path / "missing-dependency";
    REQUIRE(std::filesystem::create_directory(invalid_directory));
    std::filesystem::permissions(invalid_directory, std::filesystem::perms::owner_all);
    const auto invalid_host = invalid_directory / host.filename();
    REQUIRE(std::filesystem::copy_file(host, invalid_host));
    std::filesystem::permissions(invalid_host, std::filesystem::perms::owner_all);
    std::ifstream manifest_stream(host.string() + ".inspector-capabilities.json");
    const std::string manifest_bytes(std::istreambuf_iterator<char>{manifest_stream}, {});
    auto invalid_manifest = choc::json::parse(manifest_bytes);
    auto without_controller = choc::value::createEmptyArray();
    for (const auto capability : invalid_manifest["capabilities"])
        if (capability.getString() != "dev.pulp.session/control@1")
            without_controller.addArrayElement(capability);
    invalid_manifest.setMember("capabilities", without_controller);
    const auto invalid_manifest_bytes = choc::json::toString(invalid_manifest);
    ControlManifestDiagnostics missing_dependency_diagnostics;
    REQUIRE_FALSE(parse_control_manifest(invalid_manifest_bytes, &missing_dependency_diagnostics));
    REQUIRE(missing_dependency_diagnostics.code ==
            ControlManifestError::MissingCapabilityDependency);
    const auto invalid_sidecar = invalid_host.string() + ".inspector-capabilities.json";
    std::ofstream(invalid_sidecar) << invalid_manifest_bytes;
    std::filesystem::permissions(invalid_sidecar, std::filesystem::perms::owner_read |
                                                      std::filesystem::perms::owner_write);

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
                                       {.host_id = "sample-region-missing-dependency",
                                        .intent = {.executable = invalid_host,
                                                   .arguments = {},
                                                   .working_directory = invalid_directory,
                                                   .host_tier = ControlHostTier::Standalone}},
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
    const auto enrollment = management.manage("enroll");
    REQUIRE(enrollment.status_id == "accepted");
    const std::string client_id(choc::json::parse(enrollment.data_json)["client_id"].getString());
    const auto missing_dependency = management.manage(
        "host-prepare-installed", R"({"host_id":"sample-region-missing-dependency"})", 10s);
    INFO(missing_dependency.explanation);
    CHECK(missing_dependency.status_id != "prepared");
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
    constexpr std::string_view editable_plugin_id = "dev.pulp.sample-region-allpass.editable";
    constexpr std::string_view frozen_plugin_id = "dev.pulp.sample-region-allpass.frozen";
    const auto editable_identity = wait_for_instance(management, editable_plugin_id);
    const std::string instance(editable_identity["instance_id"].getString());
    REQUIRE_FALSE(instance.empty());
    const auto read = run(cli, root.runtime,
                          {"control", "call", "--instance", instance, "dev.pulp.instance/read@1",
                           "--params", "{}", "--json"});
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
    INFO(cli_failure_diagnostics(region_read, daemon.state_directory() / "operations"));
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
    REQUIRE(before_state.exit_code == 0);
    const auto before_detail_document = choc::json::parse(before_state.stdout_output);
    const auto before_detail = before_detail_document["detail"];
    REQUIRE(before_detail["parameters"].size() == 1);
    const auto coefficient = before_detail["parameters"][0];
    CHECK(coefficient["id"].getInt64() == 2901);
    CHECK(coefficient["rate"].getString() == "control");
    CHECK(std::abs(coefficient["min"].getFloat64() + 0.99) < 1.0e-6);
    CHECK(std::abs(coefficient["max"].getFloat64() - 0.99) < 1.0e-6);
    CHECK_FALSE(coefficient.hasObjectMember("smoothing_ramp_seconds"));
    const auto state_generation = before_detail["state_generation"].getInt64();
    const auto catalog_generation = before_detail["catalog_generation"].getInt64();
    const auto full_read = cli_call("dev.pulp.graph/sample-region.read@1",
                                    R"({"region_id":901,"include_definition":true})");
    REQUIRE(full_read.exit_code == 0);
    const auto full_detail_document = choc::json::parse(full_read.stdout_output);
    const auto full_detail = full_detail_document["detail"];
    const auto graph_generation = full_detail["graph_generation"].getInt64();
    require_allpass_resources(full_detail["regions"][0]["resources"], 0);
    const auto definition = full_detail["regions"][0]["definition"];
    CHECK(definition["promoted_parameters"][0]["smoothing_ramp_seconds"].getInt64() == 0);
    CHECK(definition["promoted_parameters"][0]["minimum"].getFloat64() ==
          coefficient["min"].getFloat64());
    CHECK(definition["promoted_parameters"][0]["maximum"].getFloat64() ==
          coefficient["max"].getFloat64());
    const auto initial_audio = require_audio(root, false, graph_generation, state_generation, 0,
                                             coefficient["value"].getFloat64());
    const auto assert_live_preserved = [&](choc::value::ValueView expected_graph,
                                           choc::value::ValueView expected_state,
                                           unsigned delay_samples) {
        require_preserved(root, cli, instance, false, expected_graph, expected_state,
                          delay_samples);
    };

    const auto develop =
        run(cli, root.runtime,
            {"control", "grant-request", "--instance", instance, "--profile", "develop", "--json"});
    REQUIRE(develop.exit_code == 0);
    const std::string develop_grant(
        choc::json::parse(develop.stdout_output)["data"]["grant_id"].getString());
    const auto lease =
        cli_call("dev.pulp.session/control@1", R"({"action":"acquire"})", develop_grant);
    REQUIRE(lease.exit_code == 0);
    const auto gesture = cli_call(
        "dev.pulp.state/parameter-gesture@1",
        R"({"parameter_id":2901,"normalized_value":0.625,"idempotency_key":"coefficient-cli"})",
        develop_grant);
    REQUIRE(gesture.exit_code == 0);
    CHECK(choc::json::parse(gesture.stdout_output)["detail"]["applied"].getBool());
    const auto after_state = cli_call("dev.pulp.state/read@1", "{}");
    REQUIRE(after_state.exit_code == 0);
    const auto after_detail_document = choc::json::parse(after_state.stdout_output);
    const auto after_detail = after_detail_document["detail"];
    CHECK(after_detail["state_generation"].getInt64() == state_generation + 1);
    CHECK(after_detail["catalog_generation"].getInt64() == catalog_generation);
    CHECK(std::abs(after_detail["parameters"][0]["normalized"].getFloat64() - 0.625) <=
          std::numeric_limits<float>::epsilon());
    const auto after_region = cli_call("dev.pulp.graph/sample-region.read@1", "{}");
    REQUIRE(after_region.exit_code == 0);
    CHECK(choc::json::parse(after_region.stdout_output)["detail"]["graph_generation"].getInt64() ==
          graph_generation);
    const auto gesture_coefficient = after_detail["parameters"][0]["value"].getFloat64();
    CHECK(std::abs(gesture_coefficient - (-0.99 + 1.98 * 0.625)) < 1.0e-6);
    const auto gesture_audio =
        require_audio(root, false, graph_generation, state_generation + 1, 0, gesture_coefficient);
    CHECK(initial_audio["coefficient"].getFloat64() != gesture_audio["coefficient"].getFloat64());

    const auto observe =
        run(cli, root.runtime,
            {"control", "grant-request", "--instance", instance, "--profile", "observe", "--json"});
    REQUIRE(observe.exit_code == 0);
    const std::string observe_grant(
        choc::json::parse(observe.stdout_output)["data"]["grant_id"].getString());
    const auto forbidden_gesture = cli_call(
        "dev.pulp.state/parameter-gesture@1",
        R"({"parameter_id":2901,"normalized_value":0.75,"idempotency_key":"observe-denied"})",
        observe_grant);
    CHECK(forbidden_gesture.exit_code != 0);
    assert_live_preserved(full_detail, after_detail, 0);
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
    REQUIRE(
        cli_call("dev.pulp.session/control@1", R"({"action":"renew"})", develop_grant).exit_code ==
        0);
    const auto edited = cli_call("dev.pulp.graph/sample-region.edit@1", edit_input, develop_grant);
    INFO(cli_failure_diagnostics(edited, daemon.state_directory() / "operations"));
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
    (void)require_audio(root, false, graph_generation + 1, state_generation + 1, 1,
                        gesture_coefficient);
    const auto edited_graph = cli_call("dev.pulp.graph/sample-region.read@1",
                                       R"({"region_id":901,"include_definition":true})");
    REQUIRE(edited_graph.exit_code == 0);
    const auto edited_graph_document = choc::json::parse(edited_graph.stdout_output);
    require_delay_mapping(edit_detail, full_detail, edited_graph_document["detail"]);
    REQUIRE(
        cli_call("dev.pulp.session/control@1", R"({"action":"renew"})", develop_grant).exit_code ==
        0);
    const auto stale = cli_call("dev.pulp.graph/sample-region.edit@1", edit_input, develop_grant);
    require_cli_refusal(stale, daemon.state_directory() / "operations",
                        ControlResultCode::StateConflict, "graph generation changed");
    CHECK(choc::json::parse(stale.stdout_output)["detail"]["path"].getString() ==
          "/expected_graph_generation");
    assert_live_preserved(edited_graph_document["detail"], after_detail, 1);
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

    // A persistent authenticated client controls envelope idempotency independently
    // of the CLI's deliberately fresh per-invocation request keys.
    auto grant_params = choc::value::createObject("");
    grant_params.setMember("instance_id", instance);
    grant_params.setMember("profile", "develop");
    const auto issued = management.manage("grant-request", choc::json::toString(grant_params));
    REQUIRE(issued.status_id == "granted");
    const std::string wire_grant(choc::json::parse(issued.data_json)["grant_id"].getString());
    ControlClient wire(management);
    REQUIRE(wire.negotiate({.mandatory_features = {"receipts"}}).succeeded());
    {
        const auto denied = request_without_session(
            daemon.endpoint_path(),
            wire_request(client_id, editable_identity, wire_grant,
                         "dev.pulp.graph/sample-region.read@1", "{}", "missing-session"));
        CHECK(denied.error_code == "session-required");
        CHECK(denied.explanation == "control requests require an authenticated session");
    }
    assert_live_preserved(edited_graph_document["detail"], after_detail, 1);
    const auto wire_call = [&](std::string_view operation, std::string params, std::string key) {
        const auto result = wire.request(wire_request(client_id, editable_identity, wire_grant,
                                                      operation, std::move(params), std::move(key)),
                                         20s);
        INFO(result.explanation);
        REQUIRE(result.response);
        return *result.response;
    };
    const auto current_read = cli_call("dev.pulp.graph/sample-region.read@1",
                                       R"({"region_id":901,"include_definition":true})");
    REQUIRE(current_read.exit_code == 0);
    const auto current_document = choc::json::parse(current_read.stdout_output);
    const auto second_edit = delay_edit(current_document["detail"]);
    const auto missing_lease =
        wire_call("dev.pulp.graph/sample-region.edit@1", second_edit, "missing-lease");
    CHECK(missing_lease.state == ControlReceiptState::Failed);
    CHECK(missing_lease.result_code == ControlResultCode::LeaseConflict);
    assert_live_preserved(current_document["detail"], after_detail, 1);
    REQUIRE(
        wire_call("dev.pulp.session/control@1", R"({"action":"acquire"})", "wire-acquire").state ==
        ControlReceiptState::Completed);
    auto replay_request =
        wire_request(client_id, editable_identity, wire_grant,
                     "dev.pulp.graph/sample-region.edit@1", second_edit, "same-content-edit");
    const auto first_result = wire.request(replay_request, 20s);
    REQUIRE(first_result.response);
    REQUIRE(first_result.response->state == ControlReceiptState::Completed);
    const auto original_receipt = *first_result.response;
    replay_request.request_id = "different-request-same-content";
    replay_request.request_hash = *control_request_hash(replay_request);
    const auto replay = wire.request(replay_request, 20s);
    REQUIRE(replay.response);
    CHECK(replay.response->receipt_id == original_receipt.receipt_id);
    CHECK(replay.response->detail_json == original_receipt.detail_json);
    CHECK(replay.response->state == ControlReceiptState::Completed);
    auto conflicting_params = choc::json::parse(second_edit);
    conflicting_params.setMember("expected_graph_generation", graph_generation + 2);
    replay_request.request_id = "changed-content-same-key";
    replay_request.params_json = choc::json::toString(conflicting_params, false);
    replay_request.request_hash = *control_request_hash(replay_request);
    const auto conflict = wire.request(replay_request, 20s);
    CHECK_FALSE(conflict.response);
    CHECK(conflict.explanation == "idempotency-conflict");
    const auto after_replay = cli_call("dev.pulp.graph/sample-region.read@1",
                                       R"({"region_id":901,"include_definition":true})");
    REQUIRE(after_replay.exit_code == 0);
    const auto after_replay_document = choc::json::parse(after_replay.stdout_output);
    REQUIRE(after_replay_document["detail"]["graph_generation"].getInt64() == graph_generation + 2);
    require_delay_mapping(choc::json::parse(original_receipt.detail_json),
                          current_document["detail"], after_replay_document["detail"]);
    (void)require_audio(root, false, graph_generation + 2, state_generation + 1, 2,
                        gesture_coefficient);

    const auto assert_preserved = [&] {
        assert_live_preserved(after_replay_document["detail"], after_detail, 2);
    };
    assert_preserved();
    const auto denied_edit = [&](std::string actions, std::string key,
                                 std::string_view expected_reason) {
        REQUIRE(wire_call("dev.pulp.session/control@1", R"({"action":"renew"})", key + "-renew")
                    .state == ControlReceiptState::Completed);
        const auto params = "{\"region_id\":901,\"expected_graph_generation\":" +
                            std::to_string(graph_generation + 2) + ",\"actions\":[" + actions +
                            "]}";
        const auto denied = wire_call("dev.pulp.graph/sample-region.edit@1", params, key);
        INFO(denied.explanation);
        CHECK(denied.state == ControlReceiptState::Failed);
        CHECK(denied.result_code == ControlResultCode::InvalidRequest);
        CHECK(choc::json::parse(denied.detail_json)["code"].getString() == expected_reason);
        assert_preserved();
    };
    denied_edit(
        R"({"op":"add_supported_kernel","temporary_node_id":"t1","type_id":"unsupported.kernel","type_version":1,"config":{"kind":"none"}})",
        "unsupported-type", "UnsupportedNodeKind");
    const auto parameter_node = definition["promoted_parameters"][0]["bound_node_id"].getInt64();
    denied_edit("{\"op\":\"set_finite_constant\",\"node_id\":" + std::to_string(parameter_node) +
                    ",\"value\":0.25}",
                "wrong-constant-kernel", "UnknownMember");
    REQUIRE(
        wire_call("dev.pulp.session/control@1", R"({"action":"renew"})", "promotion-schema-renew")
            .state == ControlReceiptState::Completed);
    auto promotion_change = choc::json::parse(delay_edit(after_replay_document["detail"]));
    auto promoted = choc::value::createEmptyArray();
    promoted.addArrayElement(
        choc::json::parse(R"({"param_id":2901,"minimum":-0.5,"maximum":0.5})"));
    promotion_change.setMember("promoted_parameters", promoted);
    const auto promotion_denied =
        wire.request(wire_request(client_id, editable_identity, wire_grant,
                                  "dev.pulp.graph/sample-region.edit@1",
                                  choc::json::toString(promotion_change), "promotion-schema"),
                     20s);
    CHECK_FALSE(promotion_denied.response);
    CHECK(promotion_denied.error_code == "admission-denied");
    CHECK(promotion_denied.explanation == "invalid-request");
    assert_preserved();
    const auto input_node = definition["input_boundaries"][0].getInt64();
    std::int64_t multiply = 0;
    for (const auto member : definition["members"])
        if (member["type_id"].getString() == "pulp.core.sample-region.multiply") {
            for (const auto edge : definition["connections"])
                if (edge["source_node_id"].getInt64() == input_node &&
                    edge["destination_node_id"].getInt64() == member["node_id"].getInt64())
                    multiply = member["node_id"].getInt64();
        }
    REQUIRE(multiply != 0);
    denied_edit("{\"op\":\"disconnect\",\"source_node_id\":" + std::to_string(input_node) +
                    ",\"source_port\":0,\"destination_node_id\":" + std::to_string(multiply) +
                    ",\"destination_port\":0},{\"op\":\"connect\",\"source_node_id\":" +
                    std::to_string(multiply) + ",\"source_port\":0,\"destination_node_id\":" +
                    std::to_string(multiply) + ",\"destination_port\":0}",
                "algebraic-cycle", "InstantaneousCycle");
    auto over_limit = choc::json::parse(second_edit);
    auto oversized_actions = choc::value::createEmptyArray();
    for (int i = 0; i < 65; ++i)
        oversized_actions.addArrayElement(
            choc::json::parse(R"({"op":"set_finite_constant","node_id":1,"value":0.5})"));
    over_limit.setMember("actions", oversized_actions);
    const auto too_many = wire.request(wire_request(client_id, editable_identity, wire_grant,
                                                    "dev.pulp.graph/sample-region.edit@1",
                                                    choc::json::toString(over_limit), "over-limit"),
                                       20s);
    CHECK_FALSE(too_many.response);
    CHECK(too_many.error_code == "admission-denied");
    CHECK(too_many.explanation == "invalid-request");
    assert_preserved();
    const auto expiring =
        wire_call("dev.pulp.session/control@1", R"({"action":"renew"})", "expiry-renew");
    REQUIRE(expiring.state == ControlReceiptState::Completed);
    const auto expires = choc::json::parse(expiring.detail_json)["expires_at_ms"].getInt64();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
               .count() <= expires + 25)
        std::this_thread::sleep_for(10ms);
    const auto expired = wire_call("dev.pulp.graph/sample-region.edit@1",
                                   delay_edit(after_replay_document["detail"]), "expired-lease");
    CHECK(expired.state == ControlReceiptState::Failed);
    CHECK(expired.result_code == ControlResultCode::LeaseConflict);
    assert_preserved();
    REQUIRE(wire_call("dev.pulp.session/control@1", R"({"action":"acquire"})", "expiry-reacquire")
                .state == ControlReceiptState::Completed);
    REQUIRE(
        wire_call("dev.pulp.session/control@1", R"({"action":"release"})", "wire-release").state ==
        ControlReceiptState::Completed);

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
    const auto frozen_identity = wait_for_instance(management, frozen_plugin_id);
    const std::string frozen_instance(frozen_identity["instance_id"].getString());
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
    const auto frozen_state_read = run(cli, root.runtime,
                                       {"control", "call", "--instance", frozen_instance,
                                        "dev.pulp.state/read@1", "--params", "{}", "--json"});
    REQUIRE(frozen_state_read.exit_code == 0);
    const auto frozen_before_graph = choc::json::parse(frozen_region_read.stdout_output);
    const auto frozen_before_state = choc::json::parse(frozen_state_read.stdout_output);
    const auto frozen_edit = run(
        cli, root.runtime,
        {"control", "call", "--instance", frozen_instance, "dev.pulp.graph/sample-region.edit@1",
         "--params",
         R"({"region_id":901,"expected_graph_generation":1,"actions":[{"op":"set_finite_constant","node_id":1,"value":0.5}]})",
         "--profile", "develop", "--json"});
    CHECK(frozen_edit.exit_code != 0);
    const auto frozen_refusal = choc::json::parse(frozen_edit.stdout_output);
    CHECK(frozen_refusal["error"].getString() == "admission-denied");
    CHECK(frozen_refusal["explanation"].getString() == "permission-denied");
    CHECK_FALSE(frozen_refusal.hasObjectMember("receipt_id"));
    require_preserved(root, cli, frozen_instance, true, frozen_before_graph["detail"],
                      frozen_before_state["detail"], 0, false);
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
                (editable_product ? mcp_call(9, "pulp_control_graph_sample_region_edit", id,
                                             delay_edit(after_replay_document["detail"]))
                                  : mcp_call(9, "pulp_control_instance_read", id, "{}")) +
                mcp_call(10, "pulp_control_graph_sample_region_read", id, "{}") +
                mcp_call(11, "pulp_control_session_control", id, R"({"action":"release"})") +
                mcp_call(12, "pulp_control_graph_sample_region_read", id,
                         R"({"region_id":901,"include_definition":true})"));
        REQUIRE(responses.size() == 12);
        const auto tools = responses[0]["result"]["tools"];
        std::set<std::string> names;
        for (std::uint32_t index = 0; index < tools.size(); ++index)
            names.emplace(tools[index]["name"].getString());
        CHECK(names.contains("pulp_control_graph_sample_region_read"));
        // MCP projects registry schemas; exact live availability is checked on
        // the selected instance, including the frozen refusal below.
        CHECK(names.contains("pulp_control_graph_sample_region_edit"));
        for (std::size_t index = 1; index < responses.size(); ++index) {
            REQUIRE_FALSE(responses[index]["result"]["isError"].getWithDefault<bool>(false));
            REQUIRE(responses[index]["result"]["structuredContent"]["state"].getString() ==
                    "completed");
        }
        const auto state_before = responses[1]["result"]["structuredContent"]["result"];
        const auto state_after = responses[6]["result"]["structuredContent"]["result"];
        CHECK(state_before["parameters"][0]["id"].getInt64() == 2901);
        CHECK(state_before["parameters"][0]["rate"].getString() == "control");
        CHECK(state_before["parameters"][0]["min"].getFloat64() == coefficient["min"].getFloat64());
        CHECK(state_before["parameters"][0]["max"].getFloat64() == coefficient["max"].getFloat64());
        CHECK(std::abs(state_after["parameters"][0]["normalized"].getFloat64() - 0.75) <=
              std::numeric_limits<float>::epsilon());
        CHECK(state_after["state_generation"].getInt64() ==
              state_before["state_generation"].getInt64() + 1);
        CHECK(state_after["catalog_generation"].getInt64() ==
              state_before["catalog_generation"].getInt64());
        const auto summary_before = responses[2]["result"]["structuredContent"]["result"];
        const auto full = responses[3]["result"]["structuredContent"]["result"];
        require_allpass_resources(full["regions"][0]["resources"], editable_product ? 2 : 0);
        const auto summary_after = responses[7]["result"]["structuredContent"]["result"];
        CHECK_FALSE(summary_before["regions"][0].hasObjectMember("definition"));
        CHECK(full["regions"][0]["definition"]["promoted_parameters"][0]["smoothing_ramp_seconds"]
                  .getInt64() == 0);
        CHECK(summary_after["graph_generation"].getInt64() ==
              summary_before["graph_generation"].getInt64());
        const auto final_summary = responses[9]["result"]["structuredContent"]["result"];
        CHECK(final_summary["graph_generation"].getInt64() ==
              summary_before["graph_generation"].getInt64() + (editable_product ? 1 : 0));
        if (editable_product) {
            const auto mutation = responses[8]["result"]["structuredContent"]["result"];
            const auto final_definition = responses[11]["result"]["structuredContent"]["result"];
            CHECK(mutation["old_graph_generation"].getInt64() == graph_generation + 2);
            CHECK(mutation["new_graph_generation"].getInt64() == graph_generation + 3);
            require_delay_mapping(mutation, full, final_definition);
            CHECK(mutation["state_retained_count"].getInt64() == 4);
            CHECK(mutation["state_reset_count"].getInt64() == 1);
        }
        (void)require_audio(root, !editable_product, final_summary["graph_generation"].getInt64(),
                            state_after["state_generation"].getInt64(), editable_product ? 3 : 0,
                            state_after["parameters"][0]["value"].getFloat64());
        if (!editable_product) {
            const auto edit_responses =
                run_mcp(mcp, root.runtime,
                        mcp_call(10, "pulp_control_graph_sample_region_edit", id, second_edit));
            REQUIRE(edit_responses.size() == 1);
            CHECK(edit_responses[0]["result"]["isError"].getWithDefault<bool>(false));
            const auto refusal = edit_responses[0]["result"]["structuredContent"];
            CHECK_FALSE(refusal["ok"].getBool());
            CHECK(refusal["error"]["code"].getString() == "admission-denied");
            CHECK(refusal["error"]["message"].getString() == "permission-denied");
            CHECK_FALSE(refusal.hasObjectMember("receipt_id"));
            require_preserved(root, cli, frozen_instance, true, final_summary, state_after, 0,
                              false);
        }
    }
    management.disconnect();
    daemon.stop();
#else
    SKIP("sample-region broker E2E is macOS-only");
#endif
}

#if defined(__APPLE__) && defined(PULP_SAMPLE_REGION_PROOF_HOST)
TEST_CASE("broker cancellation and revocation fence prepared topology with live audio",
          "[inspect][control][sample-region][e2e][transaction]") {
    Root root;
    const auto executable = self();
    const auto broker = executable.parent_path() / "pulp-control-broker";
    const auto host = std::filesystem::path{PULP_SAMPLE_REGION_PROOF_HOST};
    const auto barriers = root.path / "barriers";
    const auto evidence = root.path / "audio" / "editable";
    REQUIRE(std::filesystem::create_directory(barriers));
    std::filesystem::permissions(barriers, std::filesystem::perms::owner_all);
    std::atomic<unsigned> consent_sequence{0};
    ControlBrokerDaemon daemon(
        {.runtime_root = root.runtime,
         .state_root = root.state,
         .sdk_version = "sample-region-broker-proof",
         .executable_path = executable,
         .process_generation = 4002,
         .installed_host_selections = {{.host_id = "sample-region-proof",
                                        .intent = {.executable = host,
                                                   .arguments = {barriers.string(),
                                                                 evidence.string()},
                                                   .working_directory = host.parent_path(),
                                                   .host_tier = ControlHostTier::Standalone}}},
         .decide_consent = [&consent_sequence](const ControlGrantConsentRequest&) {
             return ControlConsentDecision{true,
                                           ControlConsentAuthority::TrustedHostUi,
                                           "proof-consent-" +
                                               std::to_string(consent_sequence.fetch_add(1)),
                                           {}};
         }});
    REQUIRE(daemon.start());
    ControlClientConnection primary(
        {.endpoint_path = daemon.endpoint_path(), .expected_broker_executable = broker});
    ControlClientConnection authority(
        {.endpoint_path = daemon.endpoint_path(), .expected_broker_executable = broker});
    ControlClientConnection parameter(
        {.endpoint_path = daemon.endpoint_path(), .expected_broker_executable = broker});
    std::string client_id;
    for (auto* connection : {&primary, &authority, &parameter}) {
        REQUIRE(connection->connect());
        const auto enrollment = connection->manage("enroll");
        REQUIRE(enrollment.status_id == "accepted");
        const std::string observed(
            choc::json::parse(enrollment.data_json)["client_id"].getString());
        if (client_id.empty())
            client_id = observed;
        REQUIRE(observed == client_id);
    }
    const auto prepared =
        primary.manage("host-prepare-installed", R"({"host_id":"sample-region-proof"})", 10s);
    INFO(prepared.explanation);
    REQUIRE(prepared.status_id == "prepared");
    auto launch = choc::value::createObject("");
    launch.setMember("inventory_id", choc::json::parse(prepared.data_json)["inventory_id"]);
    const auto launched = primary.manage("host-launch", choc::json::toString(launch), 10s);
    INFO(launched.explanation);
    REQUIRE(launched.status_id == "launched");
    CHECK(primary.manage("host-launch", choc::json::toString(launch), 10s).status_id != "launched");
    const auto identity = wait_for_instance(primary, "dev.pulp.sample-region-allpass.proof");
    const auto binding = wait_json(barriers / "binding.json");
    require_instance_binding(binding, identity);
    const auto host_pid = static_cast<pid_t>(binding["producer"]["process_id"].getInt64());
    const std::string host_start(binding["producer"]["process_start_id"].getString());
    const auto observed_process_start = [host_pid]() -> std::optional<std::string> {
        proc_bsdinfo process{};
        if (proc_pidinfo(host_pid, PROC_PIDTBSDINFO, 0, &process, sizeof(process)) != sizeof(process))
            return std::nullopt;
        return std::to_string(process.pbi_start_tvsec) + ":" +
               std::to_string(process.pbi_start_tvusec);
    };
    REQUIRE(observed_process_start() == host_start);
    CHECK(binding["session_id"].getString() == identity["session_id"].getString());
    CHECK(binding["manifest_digest"].getString() == identity["manifest_digest"].getString());
    CHECK(binding["producer_artifact_digest"].getString() ==
          identity["artifact_digest"].getString());
    CHECK(binding["producer"]["executable_sha256"].getString() ==
          identity["artifact_digest"].getString());
    CHECK(binding["preflight"]["status"].getString() == "accepted");
    CHECK(binding["preflight"]["transport"].getString() == "inherited_host_preflight");
    CHECK(binding["preflight"]["credential_kind"].getString() == "enrollment");
    CHECK(binding["preflight"]["broker_process_id"].getInt64() ==
          static_cast<std::int64_t>(::getpid()));
    CHECK_FALSE(binding["preflight"]["broker_process_start_id"].getString().empty());
    CHECK_FALSE(binding["preflight"]["broker_executable_identity"].getString().empty());
    CHECK_FALSE(binding["manifest_digest"].getString().empty());
    CHECK_FALSE(binding["producer_artifact_digest"].getString().empty());
    CHECK(binding["producer"]["plugin_id"].getString() == "dev.pulp.sample-region-allpass.proof");
    ControlClient editor(primary), controller(authority), gesturer(parameter);
    for (auto* client : {&editor, &controller, &gesturer})
        REQUIRE(
            client
                ->negotiate({.mandatory_features = {"receipts", "cancellation"},
                             .optional_features = {"progress"}})
                .succeeded());
    const auto grant = [&] {
        auto params = choc::value::createObject("");
        params.setMember("instance_id", identity["instance_id"]);
        params.setMember("profile", "develop");
        const auto issued = authority.manage("grant-request", choc::json::toString(params));
        REQUIRE(issued.status_id == "granted");
        return std::string(choc::json::parse(issued.data_json)["grant_id"].getString());
    };
    const auto persistent_grant = grant();
    unsigned sequence = 0;
    const auto call = [&](ControlClient& client, std::string_view operation, std::string params,
                          const std::string& selected_grant) {
        const auto result =
            client.request(wire_request(client_id, identity, selected_grant, operation,
                                        std::move(params), "proof-" + std::to_string(++sequence)),
                           20s);
        INFO(result.explanation);
        REQUIRE(result.response);
        INFO(result.response->explanation);
        REQUIRE(result.response->state == ControlReceiptState::Completed);
        return *result.response;
    };
    auto acquire = [&] {
        return call(controller, "dev.pulp.session/control@1", R"({"action":"acquire"})",
                    persistent_grant);
    };
    (void)acquire();
    unsigned appended_delays = 0;
    for (const std::string kind : {"cancel", "revoke", "allow"}) {
        CAPTURE(kind);
        (void)call(controller, "dev.pulp.session/control@1", R"({"action":"renew"})",
                   persistent_grant);
        const auto topology =
            call(editor, "dev.pulp.graph/sample-region.read@1",
                 R"({"region_id":901,"include_definition":true})", persistent_grant);
        const auto state = call(editor, "dev.pulp.state/read@1", "{}", persistent_grant);
        const auto before_graph = choc::json::parse(topology.detail_json);
        const auto before_state = choc::json::parse(state.detail_json);
        const auto graph_generation = before_graph["graph_generation"].getInt64();
        const auto state_generation = before_state["state_generation"].getInt64();
        const auto before_audio =
            require_audio(root, false, graph_generation, state_generation, appended_delays,
                          before_state["parameters"][0]["value"].getFloat64(), true, host);
        require_render_binding(before_audio, binding);
        const auto edit_grant = grant();
        const bool separate_edit_grant = edit_grant != persistent_grant;
        REQUIRE(separate_edit_grant);
        const double local_normalized = kind == "cancel" ? 0.40 : kind == "revoke" ? 0.45 : 0.50;
        std::ofstream(barriers / "arm") << kind << (kind == "allow" ? " allow " : " stop ")
                                        << local_normalized << '\n';
        const auto edit_request =
            wire_request(client_id, identity, edit_grant, "dev.pulp.graph/sample-region.edit@1",
                         delay_edit(before_graph), "prepared-" + kind);
        auto pending_edit =
            std::async(std::launch::async, [&] { return editor.request(edit_request, 20s); });
        const auto compiled = wait_json(barriers / (kind + ".prepared.json"));
        require_instance_binding(compiled, binding);
        REQUIRE(compiled["graph_generation"].getInt64() == graph_generation);
        REQUIRE(compiled["state_generation"].getInt64() == state_generation);
        const auto intervening = wait_json(barriers / (kind + ".parameter.json"));
        require_instance_binding(intervening, binding);
        CHECK(intervening["source"].getString() == "host_main_thread");
        CHECK(intervening["on_main_thread"].getBool());
        CHECK(intervening["state_generation"].getInt64() == state_generation + 1);
        CHECK(intervening["graph_generation"].getInt64() == graph_generation);
        CHECK(intervening["catalog_generation"].getInt64() ==
              before_state["catalog_generation"].getInt64());
        CHECK(std::abs(intervening["normalized"].getFloat64() - local_normalized) < 1.0e-6);
        CHECK(std::abs(intervening["coefficient"].getFloat64() -
                       (-0.99 + 1.98 * local_normalized)) < 1.0e-6);
        // This broker gesture overlaps in admission. The local host write
        // above is the mutation that actually occurs between prepare and swap.
        const double normalized = kind == "cancel" ? 0.60 : kind == "revoke" ? 0.65 : 0.70;
        const auto gesture_request = wire_request(
            client_id, identity, persistent_grant, "dev.pulp.state/parameter-gesture@1",
            "{\"parameter_id\":2901,\"normalized_value\":" + std::to_string(normalized) +
                ",\"idempotency_key\":\"concurrent-" + kind + "\"}",
            "concurrent-" + kind);
        auto pending_gesture =
            std::async(std::launch::async, [&] { return gesturer.request(gesture_request, 20s); });
        wait_running_receipt(daemon.state_directory() / "operations", gesture_request.request_id);
        std::optional<ControlClientReceiptResult> interrupted_edit;
        if (kind == "cancel") {
            const auto cancelled = controller.cancel({.request_id = edit_request.request_id,
                                                      .reason = "prepared candidate cancellation"},
                                                     5s);
            INFO(cancelled.explanation);
            REQUIRE(cancelled.response);
        } else if (kind == "revoke") {
            auto params = choc::value::createObject("");
            params.setMember("grant_id", edit_grant);
            const auto revoked = authority.manage("revoke", choc::json::toString(params), 5s);
            INFO(revoked.explanation);
            REQUIRE(revoked.status_id == "revoked");
        }
        if (kind != "allow") {
            REQUIRE(pending_edit.wait_for(5s) == std::future_status::ready);
            interrupted_edit = pending_edit.get();
            REQUIRE(interrupted_edit->response);
            CHECK(interrupted_edit->response->state == ControlReceiptState::UnknownNeedsRefresh);
            CHECK(interrupted_edit->response->result_code == ControlResultCode::UnknownNeedsRefresh);
            CHECK(interrupted_edit->response->retry == ControlRetryClassification::AfterRefresh);
        }
        std::ofstream(barriers / (kind + ".resume")) << "resume\n";
        const auto edit_result = interrupted_edit ? std::move(*interrupted_edit) : pending_edit.get();
        const auto gesture_result = pending_gesture.get();
        INFO(edit_result.explanation);
        REQUIRE(edit_result.response);
        REQUIRE(gesture_result.response);
        INFO(gesture_result.response->explanation);
        REQUIRE(gesture_result.response->state == ControlReceiptState::Completed);
        const auto finished = wait_json(barriers / (kind + ".finished.json"));
        const auto settled = wait_terminal_receipt(daemon.state_directory() / "operations",
                                                  edit_result.response->receipt_id);
        require_instance_binding(settled, binding);
        const bool authority_bound =
            settled["request_id"].getString() == edit_request.request_id &&
            settled["client_id"].getString() == client_id &&
            settled["grant_id"].getString() == edit_grant &&
            settled["canonical_request_hash"].getString() == edit_request.request_hash;
        REQUIRE(authority_bound);
        for (const auto* field : {"broker_id", "session_id", "instance_generation", "manifest_digest",
                                 "producer_artifact_digest"})
            REQUIRE(settled[field].getString() == binding[field].getString());
        CHECK(settled["operation_id"].getString() == edit_request.operation_id);
        CHECK(settled["operation_version"].getInt64() == edit_request.operation_version);
        CHECK(settled["deadline_unix_ms"].getInt64() == edit_request.deadline_unix_ms);
        require_instance_binding(finished, binding);
        require_instance_binding(finished["before"], binding);
        CHECK(compiled["receipt_id"].getString() == edit_result.response->receipt_id);
        CHECK(finished["receipt_id"].getString() == edit_result.response->receipt_id);
        CHECK(finished["before"]["receipt_id"].getString() == edit_result.response->receipt_id);
        CHECK(intervening["receipt_id"].getString() == edit_result.response->receipt_id);
        CHECK(finished["terminal_state"].getString() == settled["state"].getString());
        CHECK(finished["before"]["state_generation"].getInt64() == state_generation);
        CHECK(finished["state_generation"].getInt64() == state_generation + 1);
        CHECK(finished["catalog_generation"].getInt64() ==
              before_state["catalog_generation"].getInt64());
        CHECK(finished["coefficient"].getFloat64() == intervening["coefficient"].getFloat64());
        CHECK(finished["normalized"].getFloat64() == intervening["normalized"].getFloat64());
        CHECK(finished["before"]["graph_generation"].getInt64() == graph_generation);
        CHECK(finished["graph_generation"].getInt64() ==
              graph_generation + (kind == "allow" ? 1 : 0));
        CHECK(finished["prepared_checkpoint_reached"].getBool());
        CHECK(finished["checkpoint_count"].getInt64() == 2);
        CHECK(finished["continuous_blocks"].getInt64() > compiled["continuous_blocks"].getInt64());
        if (kind == "allow") {
            REQUIRE(edit_result.response->state == ControlReceiptState::Completed);
            CHECK(settled["state"].getString() == "completed");
            CHECK_FALSE(settled["cancellation_requested"].getBool());
            CHECK(finished["observed_checkpoint"].getString() == "continue");
            ++appended_delays;
        } else {
            CHECK(settled["state"].getString() == "cancelled");
            CHECK(settled["result_code"].getString() == "cancelled");
            CHECK(settled["cancellation_requested"].getBool());
            CHECK(settled["cancellation_reason"].getString() ==
                  (kind == "cancel" ? "prepared candidate cancellation" : "grant-revoked"));
            CHECK(finished["observed_checkpoint"].getString() == "cancelled");
            CHECK(finished["graph_sha256"].getString() ==
                  finished["before"]["graph_sha256"].getString());
            const auto replay = editor.request(edit_request, 5s);
            if (kind == "cancel") {
                REQUIRE(replay.response);
                CHECK(replay.response->receipt_id == edit_result.response->receipt_id);
                CHECK(replay.response->state == ControlReceiptState::Cancelled);
            } else {
                CHECK_FALSE(replay.response);
                CHECK(replay.error_code == "admission-denied");
                CHECK(replay.explanation == "permission-denied");
            }
        }
        const auto intervening_audio = require_audio(
            root, false, finished["graph_generation"].getInt64(), state_generation + 1,
            appended_delays, intervening["coefficient"].getFloat64(), false, host);
        require_render_binding(intervening_audio, binding);
        const auto after_graph = choc::json::parse(
            call(editor, "dev.pulp.graph/sample-region.read@1", "{}", persistent_grant)
                .detail_json);
        const auto after_state = choc::json::parse(
            call(editor, "dev.pulp.state/read@1", "{}", persistent_grant).detail_json);
        CHECK(after_graph["graph_generation"].getInt64() ==
              graph_generation + (kind == "allow" ? 1 : 0));
        CHECK(after_state["state_generation"].getInt64() == state_generation + 2);
        CHECK(after_state["catalog_generation"].getInt64() ==
              before_state["catalog_generation"].getInt64());
        CHECK(std::abs(after_state["parameters"][0]["normalized"].getFloat64() - normalized) <
              1.0e-6);
        const auto after_audio =
            require_audio(root, false, after_graph["graph_generation"].getInt64(),
                          after_state["state_generation"].getInt64(), appended_delays,
                          after_state["parameters"][0]["value"].getFloat64(), true, host);
        require_render_binding(after_audio, binding);
    }
    std::ofstream(barriers / "stop") << "stop\n";
    const auto exited = wait_json(barriers / "exited.json");
    require_instance_binding(exited, binding);
    require_render_binding(exited, binding);
    CHECK(exited["success"].getBool());
    CHECK(exited["stop_requested"].getBool());
    CHECK(exited["host_ready_before_stop"].getBool());
    CHECK_FALSE(exited["host_ready_after_stop"].getBool());
    CHECK_FALSE(exited["renderer_failed"].getBool());
    CHECK_FALSE(exited["capture_failed"].getBool());
    wait_for_instance_removal(primary, identity);
    bool original_process_exited = false;
    std::string exit_observation;
    const auto exit_deadline = std::chrono::steady_clock::now() + 15s;
    do {
        siginfo_t child_exit{};
        if (::waitid(P_PID, static_cast<id_t>(host_pid), &child_exit,
                     WEXITED | WNOHANG | WNOWAIT) == 0 && child_exit.si_pid == host_pid) {
            REQUIRE(child_exit.si_code == CLD_EXITED);
            REQUIRE(child_exit.si_status == 0);
            original_process_exited = true;
            exit_observation = "child_exited_zero_without_reaping";
            break;
        }
        proc_bsdinfo process{};
        if (proc_pidinfo(host_pid, PROC_PIDTBSDINFO, 0, &process, sizeof(process)) == sizeof(process)) {
            const auto current_start = std::to_string(process.pbi_start_tvsec) + ":" +
                                       std::to_string(process.pbi_start_tvusec);
            if (current_start != host_start || process.pbi_status == SZOMB) {
                original_process_exited = true;
                exit_observation = current_start != host_start ? "pid_reused" : "zombie";
                break;
            }
        }
        errno = 0;
        if (::kill(host_pid, 0) == -1 && errno == ESRCH) {
            original_process_exited = true;
            exit_observation = "pid_absent";
            break;
        }
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < exit_deadline);
    REQUIRE(original_process_exited);
    auto process_exit = choc::value::createObject("SampleRegionProcessExit");
    process_exit.setMember("process_id", static_cast<std::int64_t>(host_pid));
    process_exit.setMember("process_start_id", host_start);
    process_exit.setMember("observation", exit_observation);
    process_exit.setMember("original_process_exited", original_process_exited);
    std::ofstream(barriers / "process-exit.json") << choc::json::toString(process_exit);
    primary.disconnect();
    authority.disconnect();
    parameter.disconnect();
    daemon.stop();
}
#endif

#endif // PULP_SAMPLE_REGION_BROKER_PROOF_HOST
