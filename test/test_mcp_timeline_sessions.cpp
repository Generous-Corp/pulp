#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/audio_file.hpp>
#include <pulp/timeline/document_session.hpp>
#include <pulp/timeline/model.hpp>
#include <pulp/tools/timeline/agent.hpp>
#include <pulp/tools/timeline/writer_profile.hpp>

#include <algorithm>
#include <bit>
#include <filesystem>
#include <fstream>
#include <limits>
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

/// The unrestricted, unquotaed authority the store's accounting tests measured
/// before writer profiles existed. Named once so a byte-budget assertion is
/// never perturbed by an authority change it is not testing.
const pulp::tools::timeline::WriterProfile& store_test_profile() {
    static const auto profile = pulp::tools::timeline::trusted_writer_profile();
    return profile;
}

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
    const auto first = store.open(project, store_test_profile(), error);
    const auto second = store.open(project, store_test_profile(), error);
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
    const auto first = store.open(project, store_test_profile(), error);
    const auto second = store.open(project, store_test_profile(), error);
    const auto third = store.open(project, store_test_profile(), error);
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
    const auto probe_id = probe.open(project, store_test_profile(), error);
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
    const auto id = store.open(project, store_test_profile(), error);
    REQUIRE(id);
    REQUIRE(store.admission_charge_for_testing() == initial_bytes);
    REQUIRE(store.apply(*id, commands));
    REQUIRE(store.admission_charge_for_testing() == changed_bytes);
    REQUIRE(store.undo(*id));
    REQUIRE(store.admission_charge_for_testing() == undone_bytes);
    REQUIRE(store.redo(*id));
    REQUIRE(store.admission_charge_for_testing() == redone_bytes);

    TimelineSessionStore evicting({3, initial_bytes + changed_bytes - 1, 1024 * 1024, 64 * 1024});
    const auto oldest = evicting.open(project, store_test_profile(), error);
    const auto updated = evicting.open(project, store_test_profile(), error);
    REQUIRE(oldest);
    REQUIRE(updated);
    REQUIRE(evicting.apply(*updated, commands));
    require_contains(evicting.diff(*oldest).json, "unknown or expired timeline session");
    require_contains(evicting.diff(*updated).json, R"JSON("revision":"1")JSON");
    REQUIRE(evicting.admission_charge_for_testing() == changed_bytes);

    TimelineSessionStore refusing_eviction(
        {3, initial_bytes + changed_bytes - 1, 1024 * 1024, 64 * 1024});
    const auto refusal_oldest = refusing_eviction.open(project, store_test_profile(), error);
    const auto refusal_target = refusing_eviction.open(project, store_test_profile(), error);
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
    const auto id = store.open(project, store_test_profile(), error);
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
    const auto open_probe_id = open_probe.open(project, store_test_profile(), error);
    REQUIRE(open_probe_id);
    auto complete_open_result = pulp::tools::timeline::project_open(project);
    REQUIRE(complete_open_result);
    // An opened session also reports the authority its writer was admitted
    // under, so the payload this measures has to carry it. Spliced by hand
    // rather than through the production builder, so a boundary that stopped
    // reporting the authority would still fail here.
    complete_open_result.json.insert(
        complete_open_result.json.size() - 1,
        ",\"session_id\":" + pulp::timeline::quote_json_string(*open_probe_id) +
            ",\"writer_profile\":" +
            pulp::timeline::quote_json_string(
                pulp::tools::timeline::writer_profile_name(store_test_profile().kind)));
    complete_open_result.json.insert(
        1, "\"capabilities\":" +
               pulp::tools::timeline::writer_capability_json(store_test_profile().mask) + ",");
    const auto exact_open_wire_bytes = json_tool_payload_size(complete_open_result.json);
    REQUIRE(exact_open_wire_bytes == json_tool_payload(complete_open_result.json).size());

    TimelineSessionStore open_fitting({1, 1024 * 1024, exact_open_wire_bytes, 64 * 1024});
    REQUIRE(open_fitting.open(project, store_test_profile(), error));
    TimelineSessionStore open_refusing({1, 1024 * 1024, exact_open_wire_bytes - 1, 64 * 1024});
    REQUIRE_FALSE(open_refusing.open(project, store_test_profile(), error));
    REQUIRE(error == "opened project exceeds the timeline session output limit");
    REQUIRE(open_refusing.admission_charge_for_testing() == 0);

    TimelineSessionStore probe({1, 1024 * 1024, 1024 * 1024, 64 * 1024});
    const auto probe_id = probe.open(project, store_test_profile(), error);
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

    // Opening reports the writer's capabilities alongside the project, so the
    // opened payload is larger than the mutation payload bounded here. The
    // limit is therefore tightened onto a live session, keeping this a
    // measurement of the mutation preflight at the same exact byte boundary
    // rather than of the open preflight.
    TimelineSessionStore fitting({1, 1024 * 1024, 1024 * 1024, 64 * 1024});
    const auto fitting_id = fitting.open(project, store_test_profile(), error);
    REQUIRE(fitting_id);
    fitting.set_max_output_bytes_for_testing(conservative_wire_limit);
    const auto fitting_result = fitting.apply(*fitting_id, commands);
    REQUIRE(fitting_result);
    REQUIRE(json_tool_payload_size(fitting_result.json) <= conservative_wire_limit);

    TimelineSessionStore refusing({1, 1024 * 1024, 1024 * 1024, 64 * 1024});
    const auto refusing_id = refusing.open(project, store_test_profile(), error);
    REQUIRE(refusing_id);
    const auto before = refusing.diff(*refusing_id).json;
    refusing.set_max_output_bytes_for_testing(conservative_wire_limit - 1);
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
    const auto id = store.open(project, store_test_profile(), error);
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

TEST_CASE("timeline MCP defaults to the proposal writer and names typed refusals",
          "[mcp][tools][timeline][capability]") {
    TempDir temp;
    pulp::audio::AudioFileData source;
    source.sample_rate = 48'000;
    source.channels = {std::vector<float>(32, 0.8f)};
    const auto source_path = temp.path / "source.wav";
    REQUIRE(pulp::audio::write_wav_file(source_path.string(), source,
                                        pulp::audio::WavBitDepth::Float32));

    const auto project = make_timeline_project_json(source_path);
    const auto project_argument = "\"project\":" + pulp::timeline::quote_json_string(project);
    const std::string remove_clip =
        R"JSON([{"data":{"clip_id":"4","sequence_id":"2","track_id":"3"},"type_name":"pulp.timeline.command.remove_clip","version":1}])JSON";
    const std::string modify_clip =
        R"JSON([{"data":{"clip_id":"4","expected":{"fade_in_duration":"0","fade_out_duration":"0","gain_linear_bits":"1065353216"},"replacement":{"fade_in_duration":"0","fade_out_duration":"0","gain_linear_bits":"1056964608"},"sequence_id":"2","track_id":"3"},"type_name":"pulp.timeline.command.set_clip_playback_properties","version":1}])JSON";

    // Absent selection is the non-destructive proposal authority, reported by
    // name and expanded into named class/intent pairs rather than a bit mask.
    const auto opened = handle_timeline_project_open("{" + project_argument + "}");
    require_contains(opened, R"JSON("writer_profile":"proposal")JSON");
    require_contains(opened, R"JSON({"class":"clip","intents":["create","modify"]})JSON");
    const auto session_id = timeline_string_from_response(opened, "session_id");
    REQUIRE_FALSE(session_id.empty());
    const auto session_argument = "\"session_id\":" + pulp::timeline::quote_json_string(session_id);

    // Positive control at the same target: a modify the proposal profile allows,
    // so the refusal below is the denied intent and not a dead session.
    const auto modified = handle_timeline_command_apply("{\"commands\":" + modify_clip + "," +
                                                        session_argument + "}");
    require_contains(modified, R"JSON("revision":"1")JSON");
    require_contains(modified, R"JSON("writer_profile":"proposal")JSON");
    const auto after_modify = handle_timeline_diff("{" + session_argument + "}");

    const auto refused = handle_timeline_command_apply("{\"commands\":" + remove_clip + "," +
                                                       session_argument + "}");
    require_contains(refused, R"JSON("isError":true)JSON");
    require_contains(refused, R"JSON("conflict_code":"capability_denied")JSON");
    require_contains(refused,
                     R"JSON("required_capability":{"class":"clip","intent":"remove"})JSON");
    require_contains(refused, R"JSON("command":{"sequence":)JSON");
    // The refusal left the document byte-identical.
    REQUIRE(handle_timeline_diff("{" + session_argument + "}") == after_modify);

    // Negative control: the identical removal under the selectable editor
    // profile commits, so the refusal above is the authority and not the payload.
    const auto editor_open = handle_timeline_project_open(
        "{" + project_argument + ",\"writer_profile\":\"editor\"}");
    require_contains(editor_open, R"JSON("writer_profile":"editor")JSON");
    require_contains(editor_open, R"JSON("intents":["create","modify","remove"])JSON");
    const auto editor_id = timeline_string_from_response(editor_open, "session_id");
    REQUIRE_FALSE(editor_id.empty());
    const auto editor_argument = "\"session_id\":" + pulp::timeline::quote_json_string(editor_id);
    const auto removed = handle_timeline_command_apply("{\"commands\":" + remove_clip + "," +
                                                       editor_argument + "}");
    require_contains(removed, R"JSON("revision":"1")JSON");
    require_contains(removed, R"JSON("writer_profile":"editor")JSON");

    // An unrecognized name is a usage error, never a fallback to a wider authority.
    const auto unknown = handle_timeline_project_open(
        "{" + project_argument + ",\"writer_profile\":\"bogus\"}");
    require_contains(unknown, R"JSON("isError":true)JSON");
    require_contains(unknown, "unknown writer_profile");
    require_contains(unknown, "proposal|editor|trusted");
}

TEST_CASE("timeline writer profiles bound quota and project every refusal by name",
          "[mcp][tools][timeline][capability]") {
    using namespace pulp::timeline;
    namespace tools = pulp::tools::timeline;

    // The selectable profiles differ in authority and in quota, and only the
    // trusted profile is unbounded. This is the shape D-1 requires of a default.
    const auto proposal = tools::proposal_writer_profile();
    const auto editor = tools::editor_writer_profile();
    const auto trusted = tools::trusted_writer_profile();
    REQUIRE(proposal.mask.max_transaction_retained_bytes <
            std::numeric_limits<std::size_t>::max());
    REQUIRE(editor.mask.max_transaction_retained_bytes < std::numeric_limits<std::size_t>::max());
    REQUIRE(editor.mask.max_session_retained_bytes < std::numeric_limits<std::size_t>::max());
    REQUIRE(trusted.mask.max_transaction_retained_bytes ==
            std::numeric_limits<std::size_t>::max());
    REQUIRE(trusted.mask.max_session_retained_bytes == std::numeric_limits<std::size_t>::max());
    REQUIRE_FALSE(
        pulp::timeline::allows(proposal.mask, {CommandClass::Clip, CommandIntent::Remove}));
    REQUIRE(pulp::timeline::allows(editor.mask, {CommandClass::Clip, CommandIntent::Remove}));

    TempDir temp;
    pulp::audio::AudioFileData source;
    source.sample_rate = 48'000;
    source.channels = {std::vector<float>(32, 0.8f)};
    const auto source_path = temp.path / "source.wav";
    REQUIRE(pulp::audio::write_wav_file(source_path.string(), source,
                                        pulp::audio::WavBitDepth::Float32));
    auto registry = require_timeline_result(make_builtin_timeline_registry());
    auto document = require_timeline_result(deserialize_project(
        make_timeline_project_json(source_path), registry));

    const auto modify = [](WriterToken& writer, DocumentRevision expected) {
        Transaction transaction;
        transaction.id = writer.allocate_transaction_id();
        transaction.expected_revision = expected;
        transaction.commands.push_back(
            {writer.allocate_command_id(),
             Command(SetClipPlaybackProperties{{2},
                                               {3},
                                               {4},
                                               {.gain_linear = 1.0f},
                                               {.gain_linear = 0.5f}})});
        return transaction;
    };

    SECTION("an exhausted quota is named, and the same work commits unquotaed") {
        auto session = require_timeline_result(
            DocumentSession::create(Project(document), SessionLimits{}));
        WriterCapabilityMask starved = unrestricted_capabilities();
        starved.max_transaction_retained_bytes = 0;
        auto writer = require_timeline_result(session->register_writer(starved));
        auto transaction = modify(writer, session->revision());
        const auto authorities = tools::capture_command_authorities(transaction);
        auto refused = session->submit(writer, std::move(transaction));
        REQUIRE_FALSE(refused);
        REQUIRE(refused.error().code == ConflictCode::WriterQuotaExhausted);
        const auto json = tools::transaction_refusal_json(refused.error(), "session", authorities);
        REQUIRE(json.find(R"JSON("conflict_code":"writer_quota_exhausted")JSON") !=
                std::string::npos);

        // Positive control at the same target: the identical transaction under
        // an unbounded quota commits, so the refusal is the quota.
        auto unbounded = require_timeline_result(session->register_writer(trusted.mask));
        REQUIRE(session->submit(unbounded, modify(unbounded, session->revision())));
    }

    SECTION("a stale revision is named with both revisions") {
        auto session = require_timeline_result(
            DocumentSession::create(Project(document), SessionLimits{}));
        auto writer = require_timeline_result(session->register_writer(trusted.mask));
        auto stale = modify(writer, DocumentRevision{7});
        const auto authorities = tools::capture_command_authorities(stale);
        auto refused = session->submit(writer, std::move(stale));
        REQUIRE_FALSE(refused);
        REQUIRE(refused.error().code == ConflictCode::StaleRevision);
        const auto json = tools::transaction_refusal_json(refused.error(), "session", authorities);
        REQUIRE(json.find(R"JSON("conflict_code":"stale_revision")JSON") != std::string::npos);
        REQUIRE(json.find(R"JSON("expected_revision":7)JSON") != std::string::npos);
        REQUIRE(json.find(R"JSON("current_revision":0)JSON") != std::string::npos);

        // Positive control: the same transaction at the true revision commits.
        REQUIRE(session->submit(writer, modify(writer, session->revision())));
    }
}


/// Builds a gain-change command whose precondition names the value it replaces.
///
/// Every apply in these tests has to move the gain somewhere new, because the
/// command asserts the value it expects to find. That is what makes a second
/// apply observable: without a retry token the repeat is refused on its own
/// precondition, so a repeat that SUCCEEDS can only be the recorded result.
std::string gain_command(float expected, float replacement) {
    const auto bits = [](float value) {
        return std::to_string(std::bit_cast<std::uint32_t>(value));
    };
    return R"JSON([{"data":{"clip_id":"4","expected":{"fade_in_duration":"0","fade_out_duration":"0","gain_linear_bits":")JSON" +
           bits(expected) +
           R"JSON("},"replacement":{"fade_in_duration":"0","fade_out_duration":"0","gain_linear_bits":")JSON" +
           bits(replacement) +
           R"JSON("},"sequence_id":"2","track_id":"3"},"type_name":"pulp.timeline.command.set_clip_playback_properties","version":1}])JSON";
}

/// Opens a session over a freshly written source and returns its id.
std::string open_retry_session(const std::filesystem::path& source_path) {
    pulp::audio::AudioFileData source;
    source.sample_rate = 48'000;
    source.channels = {std::vector<float>(32, 0.8f)};
    REQUIRE(pulp::audio::write_wav_file(source_path.string(), source,
                                        pulp::audio::WavBitDepth::Float32));
    const auto project = make_timeline_project_json(source_path);
    const auto opened = handle_timeline_project_open(
        "{\"project\":" + pulp::timeline::quote_json_string(project) + "}");
    auto session_id = timeline_string_from_response(opened, "session_id");
    REQUIRE_FALSE(session_id.empty());
    return session_id;
}

TEST_CASE("timeline MCP applies honour a caller retry token",
          "[mcp][tools][timeline][idempotency]") {
    TempDir temp;
    const auto session_id = open_retry_session(temp.path / "source.wav");
    const auto session_argument = "\"session_id\":" + pulp::timeline::quote_json_string(session_id);
    const auto command = gain_command(1.0f, 0.5f);

    const auto applied = handle_timeline_command_apply(
        "{\"commands\":" + command + "," + session_argument + ",\"idempotency_key\":\"k1\"}");
    require_contains(applied, R"JSON("revision":"1")JSON");
    REQUIRE(applied.find(R"JSON("isError":true)JSON") == std::string::npos);

    SECTION("the same key and the same commands return the first result") {
        const auto replayed = handle_timeline_command_apply(
            "{\"commands\":" + command + "," + session_argument + ",\"idempotency_key\":\"k1\"}");
        REQUIRE(replayed == applied);

        // Negative control: the identical repeat WITHOUT a key is refused on the
        // command's own precondition, so the replay above cannot be a second
        // apply that happened to look the same.
        const auto unkeyed =
            handle_timeline_command_apply("{\"commands\":" + command + "," + session_argument + "}");
        require_contains(unkeyed, R"JSON("isError":true)JSON");
        require_contains(unkeyed, R"JSON("conflict_code":"expected_value_mismatch")JSON");

        // The document moved exactly once across all three calls.
        require_contains(handle_timeline_diff("{" + session_argument + "}"),
                         R"JSON("revision":"1")JSON");
    }

    SECTION("the same key with different commands is refused as a collision") {
        const auto different = gain_command(0.5f, 0.25f);
        const auto refused = handle_timeline_command_apply(
            "{\"commands\":" + different + "," + session_argument + ",\"idempotency_key\":\"k1\"}");
        require_contains(refused, R"JSON("isError":true)JSON");
        require_contains(refused, R"JSON("conflict_code":"transaction_id_collision")JSON");

        // Positive control: those same commands under a fresh key commit, so the
        // refusal is the reused token and not the commands.
        const auto accepted = handle_timeline_command_apply(
            "{\"commands\":" + different + "," + session_argument + ",\"idempotency_key\":\"k2\"}");
        require_contains(accepted, R"JSON("revision":"2")JSON");
    }

    SECTION("a replayed key whose result has aged out says so instead of reapplying") {
        // Push distinct unkeyed applies through until the session's result cache
        // has turned over. The token's identity outlives the cached result, so
        // the replay resolves to a transaction the document has already passed.
        float current = 0.5f;
        for (int i = 0; i < 12; ++i) {
            const float next = current - 0.01f;
            const auto step = gain_command(current, next);
            const auto ok = handle_timeline_command_apply("{\"commands\":" + step + "," +
                                                          session_argument + "}");
            REQUIRE(ok.find(R"JSON("isError":true)JSON") == std::string::npos);
            current = next;
        }
        const auto expired = handle_timeline_command_apply(
            "{\"commands\":" + command + "," + session_argument + ",\"idempotency_key\":\"k1\"}");
        require_contains(expired, R"JSON("isError":true)JSON");
        require_contains(expired, R"JSON("conflict_code":"already_applied_result_expired")JSON");
    }
}

TEST_CASE("timeline MCP applies honour a caller expected revision",
          "[mcp][tools][timeline][idempotency]") {
    TempDir temp;
    const auto session_id = open_retry_session(temp.path / "source.wav");
    const auto session_argument = "\"session_id\":" + pulp::timeline::quote_json_string(session_id);
    const auto command = gain_command(1.0f, 0.5f);

    SECTION("an apply against the revision the caller read commits") {
        const auto applied = handle_timeline_command_apply(
            "{\"commands\":" + command + "," + session_argument + ",\"expected_revision\":0}");
        require_contains(applied, R"JSON("revision":"1")JSON");
        REQUIRE(applied.find(R"JSON("isError":true)JSON") == std::string::npos);
    }

    SECTION("an apply against a revision the document has passed is refused") {
        REQUIRE(handle_timeline_command_apply("{\"commands\":" + command + "," + session_argument +
                                              "}")
                    .find(R"JSON("isError":true)JSON") == std::string::npos);
        const auto second = gain_command(0.5f, 0.25f);
        const auto stale = handle_timeline_command_apply(
            "{\"commands\":" + second + "," + session_argument + ",\"expected_revision\":0}");
        require_contains(stale, R"JSON("isError":true)JSON");
        require_contains(stale, R"JSON("conflict_code":"stale_revision")JSON");

        // The refusal left the document where it was.
        require_contains(handle_timeline_diff("{" + session_argument + "}"),
                         R"JSON("revision":"1")JSON");

        // Positive control: the same commands at the true revision commit, so
        // the refusal is the revision the caller named and not the commands.
        const auto accepted = handle_timeline_command_apply(
            "{\"commands\":" + second + "," + session_argument + ",\"expected_revision\":1}");
        require_contains(accepted, R"JSON("revision":"2")JSON");
    }
}

TEST_CASE("timeline MCP refuses retry controls it cannot honour",
          "[mcp][tools][timeline][idempotency]") {
    TempDir temp;
    const auto session_id = open_retry_session(temp.path / "source.wav");
    const auto session_argument = "\"session_id\":" + pulp::timeline::quote_json_string(session_id);
    const auto command = gain_command(1.0f, 0.5f);

    const auto refuse = [&](const std::string& extra) {
        const auto response = handle_timeline_command_apply("{\"commands\":" + command + "," +
                                                            session_argument + "," + extra + "}");
        require_contains(response, R"JSON("isError":true)JSON");
        require_contains(response, R"JSON("stage":"arguments")JSON");
    };

    refuse(R"JSON("idempotency_key":7)JSON");
    refuse(R"JSON("idempotency_key":"")JSON");
    refuse(R"JSON("expected_revision":"1")JSON");
    refuse(R"JSON("expected_revision":-1)JSON");
    refuse(R"JSON("expected_revision":1.5)JSON");

    // Positive control: the same call with well-formed controls commits, so the
    // refusals above are the malformed values and not the surrounding call.
    const auto accepted = handle_timeline_command_apply(
        "{\"commands\":" + command + "," + session_argument +
        ",\"idempotency_key\":\"ok\",\"expected_revision\":0}");
    require_contains(accepted, R"JSON("revision":"1")JSON");
}

TEST_CASE("timeline MCP refuses retry controls on a stateless apply",
          "[mcp][tools][timeline][idempotency]") {
    TempDir temp;
    const auto source_path = temp.path / "source.wav";
    pulp::audio::AudioFileData source;
    source.sample_rate = 48'000;
    source.channels = {std::vector<float>(32, 0.8f)};
    REQUIRE(pulp::audio::write_wav_file(source_path.string(), source,
                                        pulp::audio::WavBitDepth::Float32));
    const auto project = make_timeline_project_json(source_path);
    const auto project_argument = "\"project\":" + pulp::timeline::quote_json_string(project);
    const auto command = gain_command(1.0f, 0.5f);

    // There is no session to record a token against and no revision to compare,
    // so accepting either would report a deduplication or a staleness check that
    // nothing performed.
    for (const std::string extra : {R"JSON("idempotency_key":"k1")JSON",
                                    R"JSON("expected_revision":0)JSON"}) {
        const auto refused = handle_timeline_command_apply("{\"commands\":" + command + "," +
                                                           project_argument + "," + extra + "}");
        require_contains(refused, R"JSON("isError":true)JSON");
        require_contains(refused, "require session_id");
    }

    // Positive control: the same stateless apply without them commits.
    const auto accepted =
        handle_timeline_command_apply("{\"commands\":" + command + "," + project_argument + "}");
    REQUIRE(accepted.find(R"JSON("isError":true)JSON") == std::string::npos);
}

} // namespace
