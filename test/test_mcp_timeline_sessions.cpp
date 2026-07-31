#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/audio_file.hpp>
#include <pulp/timeline/model.hpp>
#include <pulp/tools/timeline/agent.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../tools/mcp/mcp_json.hpp"
#include "../tools/mcp/mcp_tools.hpp"
#include "../tools/mcp/timeline_session_store.hpp"
#include "mcp_server_test_support.hpp"
#include "mcp_timeline_test_support.hpp"

namespace {

using namespace mcp_test;
using namespace mcp_timeline_test;
using namespace pulp_mcp;

TEST_CASE("timeline MCP sessions expose exact diff and journaled undo redo",
          "[mcp][tools][timeline][iteration]") {
    TempDir temp;
    pulp::audio::AudioFileData source;
    source.sample_rate = 48'000;
    source.channels = {std::vector<float>(32, 0.8f)};
    const auto source_path = temp.path / "source.wav";
    REQUIRE(pulp::audio::write_wav_file(source_path.string(), source,
                                        pulp::audio::WavBitDepth::Float32));

    const auto project = make_timeline_project_json(source_path);
    const auto opened = handle_timeline_project_open(
        "{\"project\":" + pulp::timeline::quote_json_string(project) + "}");
    const auto session_id = timeline_string_from_response(opened, "session_id");
    REQUIRE_FALSE(session_id.empty());

    const auto session_argument = "\"session_id\":" + pulp::timeline::quote_json_string(session_id);
    const auto empty_undo = handle_timeline_undo("{" + session_argument + "}");
    require_contains(empty_undo, R"JSON("isError":true)JSON");
    require_contains(empty_undo, R"JSON("conflict_code":"nothing_to_undo")JSON");
    const auto empty_redo = handle_timeline_redo("{" + session_argument + "}");
    require_contains(empty_redo, R"JSON("isError":true)JSON");
    require_contains(empty_redo, R"JSON("conflict_code":"nothing_to_redo")JSON");

    const std::string command =
        R"JSON([{"data":{"clip_id":"4","expected":{"fade_in_duration":"0","fade_out_duration":"0","gain_linear_bits":"1065353216"},"replacement":{"fade_in_duration":"0","fade_out_duration":"0","gain_linear_bits":"1056964608"},"sequence_id":"2","track_id":"3"},"type_name":"pulp.timeline.command.set_clip_playback_properties","version":1}])JSON";
    const auto applied =
        handle_timeline_command_apply("{\"commands\":" + command + "," + session_argument + "}");
    require_contains(applied, R"JSON("revision":"1")JSON");
    require_contains(applied, R"JSON("item_id":"4")JSON");
    require_contains(applied, R"JSON("flag_bits":4)JSON");
    require_contains(applied, R"JSON("can_undo":true)JSON");
    require_contains(applied, R"JSON("before_revision":"0")JSON");
    require_contains(applied, R"JSON("after_revision":"1")JSON");
    const auto changed_project = timeline_project_from_response(applied);
    REQUIRE(changed_project != project);

    const auto applied_status = handle_timeline_diff("{" + session_argument + "}");
    const auto rejected_duplicate =
        handle_timeline_command_apply("{\"commands\":" + command + "," + session_argument + "}");
    require_contains(rejected_duplicate, R"JSON("isError":true)JSON");
    REQUIRE(handle_timeline_diff("{" + session_argument + "}") == applied_status);

    const auto diff = handle_timeline_diff("{" + session_argument + "}");
    require_contains(diff, R"JSON("flags":["content"])JSON");
    require_contains(diff, R"JSON("owner_track_id":"3")JSON");

    const auto undone = handle_timeline_undo("{" + session_argument + "}");
    REQUIRE(timeline_project_from_response(undone) == project);
    require_contains(undone, R"JSON("can_redo":true)JSON");
    require_contains(undone, R"JSON("flags":["content"])JSON");
    require_contains(undone, R"JSON("item_id":"4")JSON");

    const auto redone = handle_timeline_redo("{" + session_argument + "}");
    REQUIRE(timeline_project_from_response(redone) == changed_project);
    require_contains(redone, R"JSON("can_undo":true)JSON");
    require_contains(redone, R"JSON("flags":["content"])JSON");
    require_contains(redone, R"JSON("item_id":"4")JSON");

    REQUIRE(timeline_project_from_response(handle_timeline_undo("{" + session_argument + "}")) ==
            project);
    const std::string divergent_command =
        R"JSON([{"data":{"marker":{"data":{"id":"8","name":"divergent-marker","position":"0"},"type_name":"pulp.timeline.marker","version":1},"sequence_id":"2"},"type_name":"pulp.timeline.command.insert_marker","version":1}])JSON";
    const auto divergent = handle_timeline_command_apply("{\"commands\":" + divergent_command +
                                                         "," + session_argument + "}");
    require_contains(divergent, "divergent-marker");
    require_contains(divergent, R"JSON("can_redo":false)JSON");
    const auto invalidated_redo = handle_timeline_redo("{" + session_argument + "}");
    require_contains(invalidated_redo, R"JSON("conflict_code":"nothing_to_redo")JSON");

    const auto missing = handle_timeline_diff(R"JSON({"session_id":"timeline-missing"})JSON");
    require_contains(missing, R"JSON("isError":true)JSON");
    require_contains(missing, "unknown or expired timeline session");
}

TEST_CASE("timeline MCP sessions isolate simultaneous mutation and dirty state",
          "[mcp][tools][timeline][iteration]") {
    std::ifstream fixture(std::filesystem::path(PULP_SOURCE_DIR) /
                          "test/fixtures/timeline/v4/sequence-markers.json");
    REQUIRE(fixture);
    const std::string project{std::istreambuf_iterator<char>(fixture),
                              std::istreambuf_iterator<char>()};
    TimelineSessionStore store({2, 1024 * 1024, 1024 * 1024, 64 * 1024});
    std::string error;
    const auto first = store.open(project, error);
    const auto second = store.open(project, error);
    REQUIRE(first);
    REQUIRE(second);
    const auto second_before = store.diff(*second).json;
    const std::string command =
        R"JSON([{"data":{"marker":{"data":{"id":"8","name":"isolated-marker","position":"0"},"type_name":"pulp.timeline.marker","version":1},"sequence_id":"2"},"type_name":"pulp.timeline.command.insert_marker","version":1}])JSON";
    REQUIRE(store.apply(*first, command));
    require_contains(store.diff(*first).json, R"JSON("revision":"1")JSON");
    REQUIRE(store.diff(*second).json == second_before);
    require_contains(second_before, R"JSON("before_revision":"0")JSON");
    require_contains(second_before, R"JSON("after_revision":"0")JSON");
    require_contains(second_before, R"JSON("items":[])JSON");
}

TEST_CASE("timeline MCP apply rejects project and session together at runtime",
          "[mcp][tools][timeline][iteration]") {
    const auto response = handle_timeline_command_apply(
        R"JSON({"commands":[{}],"project":"{}","session_id":"timeline-any"})JSON");
    require_contains(response, R"JSON("isError":true)JSON");
    require_contains(response, "exactly one of project or session_id is required");
}

TEST_CASE("timeline MCP session store retains its count cap with deterministic eviction",
          "[mcp][tools][timeline][iteration]") {
    std::ifstream fixture(std::filesystem::path(PULP_SOURCE_DIR) /
                          "test/fixtures/timeline/v1/minimal.json");
    REQUIRE(fixture);
    const std::string project{std::istreambuf_iterator<char>(fixture),
                              std::istreambuf_iterator<char>()};
    TimelineSessionStore store({2, 1024 * 1024, 1024 * 1024, 64 * 1024});
    std::string error;
    const auto first = store.open(project, error);
    const auto second = store.open(project, error);
    const auto third = store.open(project, error);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(third);
    require_contains(store.diff(*first).json, "unknown or expired timeline session");
    require_contains(store.diff(*second).json, R"JSON("ok":true)JSON");
    require_contains(store.diff(*third).json, R"JSON("ok":true)JSON");
}

TEST_CASE("timeline MCP store accounts apply undo redo under a small byte budget",
          "[mcp][tools][timeline][iteration]") {
    std::ifstream fixture(std::filesystem::path(PULP_SOURCE_DIR) /
                          "test/fixtures/timeline/v4/sequence-markers.json");
    REQUIRE(fixture);
    const std::string project{std::istreambuf_iterator<char>(fixture),
                              std::istreambuf_iterator<char>()};
    const std::string commands =
        R"JSON([{"data":{"marker":{"data":{"id":"8","name":"budget-marker","position":"0"},"type_name":"pulp.timeline.marker","version":1},"sequence_id":"2"},"type_name":"pulp.timeline.command.insert_marker","version":1}])JSON";

    TimelineSessionStore probe({3, 1024 * 1024, 1024 * 1024, 64 * 1024});
    std::string error;
    const auto probe_id = probe.open(project, error);
    REQUIRE(probe_id);
    const auto initial_bytes = probe.admission_charge_for_testing();
    REQUIRE(probe.apply(*probe_id, commands));
    const auto changed_bytes = probe.admission_charge_for_testing();
    REQUIRE(changed_bytes > initial_bytes);
    REQUIRE(probe.undo(*probe_id));
    const auto undone_bytes = probe.admission_charge_for_testing();
    REQUIRE(probe.redo(*probe_id));
    const auto redone_bytes = probe.admission_charge_for_testing();

    const auto peak_bytes = std::max({changed_bytes, undone_bytes, redone_bytes});
    TimelineSessionStore store({3, peak_bytes, 1024 * 1024, 64 * 1024});
    const auto id = store.open(project, error);
    REQUIRE(id);
    REQUIRE(store.admission_charge_for_testing() == initial_bytes);
    REQUIRE(store.apply(*id, commands));
    REQUIRE(store.admission_charge_for_testing() == changed_bytes);
    REQUIRE(store.undo(*id));
    REQUIRE(store.admission_charge_for_testing() == undone_bytes);
    REQUIRE(store.redo(*id));
    REQUIRE(store.admission_charge_for_testing() == redone_bytes);

    TimelineSessionStore evicting({3, initial_bytes + changed_bytes - 1, 1024 * 1024, 64 * 1024});
    const auto oldest = evicting.open(project, error);
    const auto updated = evicting.open(project, error);
    REQUIRE(oldest);
    REQUIRE(updated);
    REQUIRE(evicting.apply(*updated, commands));
    require_contains(evicting.diff(*oldest).json, "unknown or expired timeline session");
    require_contains(evicting.diff(*updated).json, R"JSON("revision":"1")JSON");
    REQUIRE(evicting.admission_charge_for_testing() == changed_bytes);

    TimelineSessionStore refusing_eviction(
        {3, initial_bytes + changed_bytes - 1, 1024 * 1024, 64 * 1024});
    const auto refusal_oldest = refusing_eviction.open(project, error);
    const auto refusal_target = refusing_eviction.open(project, error);
    REQUIRE(refusal_oldest);
    REQUIRE(refusal_target);
    const auto refusal_before = refusing_eviction.diff(*refusal_target).json;
    const auto refusal_charge = refusing_eviction.admission_charge_for_testing();
    refusing_eviction.set_max_output_bytes_for_testing(
        json_tool_payload_size(refusal_before));
    REQUIRE_FALSE(refusing_eviction.apply(*refusal_target, commands));
    refusing_eviction.set_max_output_bytes_for_testing(1024 * 1024);
    require_contains(refusing_eviction.diff(*refusal_oldest).json, R"JSON("ok":true)JSON");
    REQUIRE(refusing_eviction.diff(*refusal_target).json == refusal_before);
    REQUIRE(refusing_eviction.admission_charge_for_testing() == refusal_charge);
}

TEST_CASE("timeline MCP history reservation bounds constant-size edit growth",
          "[mcp][tools][timeline][iteration]") {
    TempDir temp;
    pulp::audio::AudioFileData source;
    source.sample_rate = 48'000;
    source.channels = {std::vector<float>(32, 0.8f)};
    const auto source_path = temp.path / "history-source.wav";
    REQUIRE(pulp::audio::write_wav_file(source_path.string(), source,
                                        pulp::audio::WavBitDepth::Float32));
    const auto project = make_timeline_project_json(source_path);
    constexpr std::size_t store_budget = 1024 * 1024;
    constexpr std::size_t history_reservation = 4096;
    TimelineSessionStore store({1, store_budget, 1024 * 1024, history_reservation});
    std::string error;
    const auto id = store.open(project, error);
    REQUIRE(id);
    REQUIRE(store.admission_charge_for_testing() >= history_reservation);
    const std::string lower =
        R"JSON([{"data":{"clip_id":"4","expected":{"fade_in_duration":"0","fade_out_duration":"0","gain_linear_bits":"1065353216"},"replacement":{"fade_in_duration":"0","fade_out_duration":"0","gain_linear_bits":"1056964608"},"sequence_id":"2","track_id":"3"},"type_name":"pulp.timeline.command.set_clip_playback_properties","version":1}])JSON";
    const std::string raise =
        R"JSON([{"data":{"clip_id":"4","expected":{"fade_in_duration":"0","fade_out_duration":"0","gain_linear_bits":"1056964608"},"replacement":{"fade_in_duration":"0","fade_out_duration":"0","gain_linear_bits":"1065353216"},"sequence_id":"2","track_id":"3"},"type_name":"pulp.timeline.command.set_clip_playback_properties","version":1}])JSON";

    bool refused = false;
    for (std::size_t edit = 0; edit < 128; ++edit) {
        const auto before = store.diff(*id).json;
        const auto result = store.apply(*id, edit % 2 == 0 ? lower : raise);
        REQUIRE(store.admission_charge_for_testing() <= store_budget);
        if (!result) {
            REQUIRE(store.diff(*id).json == before);
            refused = true;
            break;
        }
    }
    REQUIRE(refused);
}

TEST_CASE("timeline MCP session output limit covers the complete encoded payload",
          "[mcp][tools][timeline][iteration]") {
    REQUIRE(json_tool_payload_size("{}") == json_tool_payload("{}").size());
    REQUIRE(json_tool_payload_size("{\n\r\t\"\\\x01}") ==
            json_tool_payload("{\n\r\t\"\\\x01}").size());
    std::ifstream fixture(std::filesystem::path(PULP_SOURCE_DIR) /
                          "test/fixtures/timeline/v4/sequence-markers.json");
    REQUIRE(fixture);
    const std::string project{std::istreambuf_iterator<char>(fixture),
                              std::istreambuf_iterator<char>()};
    const std::string commands =
        R"JSON([{"data":{"marker":{"data":{"id":"8","name":"wire-marker","position":"0"},"type_name":"pulp.timeline.marker","version":1},"sequence_id":"2"},"type_name":"pulp.timeline.command.insert_marker","version":1}])JSON";
    std::string error;

    TimelineSessionStore open_probe({1, 1024 * 1024, 1024 * 1024, 64 * 1024});
    const auto open_probe_id = open_probe.open(project, error);
    REQUIRE(open_probe_id);
    auto complete_open_result = pulp::tools::timeline::project_open(project);
    REQUIRE(complete_open_result);
    complete_open_result.json.insert(complete_open_result.json.size() - 1,
                                     ",\"session_id\":" +
                                         pulp::timeline::quote_json_string(*open_probe_id));
    const auto exact_open_wire_bytes = json_tool_payload_size(complete_open_result.json);
    REQUIRE(exact_open_wire_bytes == json_tool_payload(complete_open_result.json).size());

    TimelineSessionStore open_fitting({1, 1024 * 1024, exact_open_wire_bytes, 64 * 1024});
    REQUIRE(open_fitting.open(project, error));
    TimelineSessionStore open_refusing({1, 1024 * 1024, exact_open_wire_bytes - 1, 64 * 1024});
    REQUIRE_FALSE(open_refusing.open(project, error));
    REQUIRE(error == "opened project exceeds the timeline session output limit");
    REQUIRE(open_refusing.admission_charge_for_testing() == 0);

    TimelineSessionStore probe({1, 1024 * 1024, 1024 * 1024, 64 * 1024});
    const auto probe_id = probe.open(project, error);
    REQUIRE(probe_id);
    const auto probe_result = probe.apply(*probe_id, commands);
    REQUIRE(probe_result);
    const auto exact_wire_bytes = json_tool_payload_size(probe_result.json);
    REQUIRE(exact_wire_bytes == json_tool_payload(probe_result.json).size());
    // Mutation preflight deliberately assumes the longest boolean spelling for
    // undo/redo state. The successful structured response is one byte shorter
    // because can_undo is true rather than false; MCP repeats that byte in both
    // structuredContent and the encoded text representation.
    const auto conservative_wire_limit = exact_wire_bytes + 2;

    TimelineSessionStore fitting({1, 1024 * 1024, conservative_wire_limit, 64 * 1024});
    const auto fitting_id = fitting.open(project, error);
    REQUIRE(fitting_id);
    const auto fitting_result = fitting.apply(*fitting_id, commands);
    REQUIRE(fitting_result);
    REQUIRE(json_tool_payload_size(fitting_result.json) <= conservative_wire_limit);

    TimelineSessionStore refusing({1, 1024 * 1024, conservative_wire_limit - 1, 64 * 1024});
    const auto refusing_id = refusing.open(project, error);
    REQUIRE(refusing_id);
    const auto before = refusing.diff(*refusing_id).json;
    REQUIRE_FALSE(refusing.apply(*refusing_id, commands));
    REQUIRE(refusing.diff(*refusing_id).json == before);
}

TEST_CASE("timeline MCP serialization refusal leaves apply undo redo state unchanged",
          "[mcp][tools][timeline][iteration]") {
    std::ifstream fixture(std::filesystem::path(PULP_SOURCE_DIR) /
                          "test/fixtures/timeline/v4/sequence-markers.json");
    REQUIRE(fixture);
    const std::string project{std::istreambuf_iterator<char>(fixture),
                              std::istreambuf_iterator<char>()};
    const std::string commands =
        R"JSON([{"data":{"marker":{"data":{"id":"8","name":"atomic-marker","position":"0"},"type_name":"pulp.timeline.marker","version":1},"sequence_id":"2"},"type_name":"pulp.timeline.command.insert_marker","version":1}])JSON";

    TimelineSessionStore store({2, 1024 * 1024, 1024 * 1024, 64 * 1024});
    std::string error;
    const auto id = store.open(project, error);
    REQUIRE(id);
    const auto initial_bytes = store.admission_charge_for_testing();
    const auto initial_status = store.diff(*id).json;

    store.set_max_output_bytes_for_testing(json_tool_payload_size(initial_status));
    REQUIRE_FALSE(store.apply(*id, commands));
    REQUIRE(store.diff(*id).json == initial_status);
    REQUIRE(store.admission_charge_for_testing() == initial_bytes);

    store.set_max_output_bytes_for_testing(1024 * 1024);
    REQUIRE(store.apply(*id, commands));
    const auto changed_bytes = store.admission_charge_for_testing();
    const auto applied_status = store.diff(*id).json;
    store.set_max_output_bytes_for_testing(json_tool_payload_size(applied_status));
    REQUIRE_FALSE(store.undo(*id));
    REQUIRE(store.diff(*id).json == applied_status);
    REQUIRE(store.admission_charge_for_testing() == changed_bytes);

    store.set_max_output_bytes_for_testing(1024 * 1024);
    REQUIRE(store.undo(*id));
    const auto undone_status = store.diff(*id).json;
    store.set_max_output_bytes_for_testing(json_tool_payload_size(undone_status));
    REQUIRE_FALSE(store.redo(*id));
    REQUIRE(store.diff(*id).json == undone_status);
    REQUIRE(store.admission_charge_for_testing() > initial_bytes);
}

TEST_CASE("timeline MCP dirty JSON preserves combined flags contexts and null owners",
          "[mcp][tools][timeline][iteration]") {
    using namespace pulp::timeline;
    const DirtySet dirty(
        {DirtyItem{{9}, {}, {2}, DirtyFlags::Structure | DirtyFlags::Context | DirtyFlags::Added}},
        {DirtyContext{{2}, CompileContextKind::Groove}});
    const auto json = timeline_dirty_set_json(dirty);
    require_contains(json, R"JSON("kind":"groove")JSON");
    require_contains(json, R"JSON("kind_id":1)JSON");
    require_contains(json, R"JSON("owner_sequence_id":"2")JSON");
    require_contains(json, R"JSON("flag_bits":1041)JSON");
    require_contains(json, R"JSON("flags":["structure","added","context"])JSON");
    require_contains(json, R"JSON("owner_track_id":null)JSON");
}

} // namespace
