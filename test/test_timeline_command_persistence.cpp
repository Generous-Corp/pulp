#include "support/timeline_persistence_test_support.hpp"

#include <pulp/timeline/document_session.hpp>

#include <array>
#include <string_view>

namespace {

Project command_payload_project() {
    auto clip = take(Clip::create({7}, {0}, {100}, EmptyContent{}));
    auto curve = take(AutomationCurve::create({{{10}, {0}, 0.25f}, {{11}, {50}, 0.75f}}));
    auto automation =
        take(AutomationLane::create({9}, DeviceParameterTarget{{8}, 42}, std::move(curve)));
    auto recording =
        take(Take::create({13}, MediaRef{{20}, {0}, 100}, {0}, RationalRate{48'000, 1}));
    auto take_lane = take(TakeLane::create({12}, "recording", {recording},
                                           {{.take_id = {13}, .range = {{0}, 50, {48'000, 1}}}}));
    TrackFreeze freeze{
        MediaRef{{20}, {0}, 100},
        {0},
        {48'000, 1},
        hash('f'),
    };
    auto track = take(Track::create(TrackInput{
        .id = {6},
        .name = "authored",
        .clips = {clip},
        .device_chain = {{{8}}},
        .automation_lanes = {automation},
        .take_lanes = {take_lane},
        .record_armed = true,
        .active_take_lane_id = {12},
        .freeze = freeze,
    }));
    auto sequence = take(Sequence::create({5}, "root", TickDuration{100}, {track}));
    MediaAsset asset{{20}, "take.wav", 1'000, {48'000, 1}, hash('d'), AssetStoragePolicy::External,
                     {},   {},         {}};
    return take(
        Project::create(ProjectInput{{1}, "command payloads", 21, {5}, {asset}, {sequence}}));
}

const JsonValue& member(const JsonValue& value, std::string_view name) {
    const auto* found = value.find(name);
    REQUIRE(found != nullptr);
    return *found;
}

std::string envelope(std::string_view type, std::string data) {
    return "{\"data\":" + std::move(data) + ",\"type_name\":\"" + std::string(type) +
           "\",\"version\":1}";
}

} // namespace

TEST_CASE("Typed command JSON decodes every registered mutation variant") {
    const auto registry = builtins();
    const auto snapshot = take(serialize_project(command_payload_project(), registry)).json;
    const auto parsed = take(parse_json(snapshot));
    const auto& project_data = member(parsed->root(), "data");
    const auto& asset = member(project_data, "assets").array[0];
    const auto& sequence = member(project_data, "sequences").array[0];
    const auto& track = member(member(sequence, "data"), "tracks").array[0];
    const auto& track_data = member(track, "data");
    const auto& clip = member(track_data, "clips").array[0];
    const auto& automation = member(track_data, "automation_lanes").array[0];
    const auto& take_lane = member(track_data, "take_lanes").array[0];
    const auto& take_value = member(member(take_lane, "data"), "takes").array[0];
    const auto& freeze = member(track_data, "freeze");
    const auto& tempo_map = member(project_data, "tempo_map");
    const auto& meter_map = member(project_data, "meter_map");

    const std::string musical_range =
        R"({"duration_ticks":"100","kind":"musical","start_ticks":"0"})";
    const std::string moved_range =
        R"({"duration_ticks":"100","kind":"musical","start_ticks":"100"})";
    const std::string playback =
        R"({"fade_in_duration":"0","fade_out_duration":"0","gain_linear_bits":"1065353216"})";
    const std::string quieter =
        R"({"fade_in_duration":"0","fade_out_duration":"0","gain_linear_bits":"1056964608"})";
    const std::string comp =
        R"([{"sample_count":"50","sample_rate":{"denominator":"1","numerator":"48000"},"start":"0","take_id":"13"}])";

    std::vector<std::string> encoded{
        envelope("pulp.timeline.command.insert_clip",
                 "{\"clip\":" + std::string(parsed->raw(clip)) +
                     R"(,"sequence_id":"5","track_id":"6"})"),
        envelope("pulp.timeline.command.remove_clip",
                 R"({"clip_id":"7","sequence_id":"5","track_id":"6"})"),
        envelope("pulp.timeline.command.insert_automation_lane",
                 "{\"lane\":" + std::string(parsed->raw(automation)) +
                     R"(,"sequence_id":"5","track_id":"6"})"),
        envelope("pulp.timeline.command.remove_automation_lane",
                 R"({"lane_id":"9","sequence_id":"5","track_id":"6"})"),
        envelope("pulp.timeline.command.move_clip", R"({"clip_id":"7","expected_range":)" +
                                                        musical_range + R"(,"replacement_range":)" +
                                                        moved_range +
                                                        R"(,"sequence_id":"5","track_id":"6"})"),
        envelope(
            "pulp.timeline.command.set_note_velocity",
            R"({"clip_id":"7","expected_velocity":32768,"note_id":"10","replacement_velocity":49152,"sequence_id":"5","track_id":"6"})"),
        envelope("pulp.timeline.command.set_clip_playback_properties",
                 R"({"clip_id":"7","expected":)" + playback + R"(,"replacement":)" + quieter +
                     R"(,"sequence_id":"5","track_id":"6"})"),
        envelope("pulp.timeline.command.set_tempo_map",
                 "{\"expected\":" + std::string(parsed->raw(tempo_map)) +
                     ",\"replacement\":" + std::string(parsed->raw(tempo_map)) + "}"),
        envelope("pulp.timeline.command.set_meter_map",
                 "{\"expected\":" + std::string(parsed->raw(meter_map)) +
                     ",\"replacement\":" + std::string(parsed->raw(meter_map)) + "}"),
        envelope("pulp.timeline.command.create_asset",
                 "{\"asset\":" + std::string(parsed->raw(asset)) + "}"),
        envelope("pulp.timeline.command.remove_asset", R"({"asset_id":"20"})"),
        envelope("pulp.timeline.command.insert_take_lane",
                 "{\"lane\":" + std::string(parsed->raw(take_lane)) +
                     R"(,"sequence_id":"5","track_id":"6"})"),
        envelope("pulp.timeline.command.remove_take_lane",
                 R"({"lane_id":"12","sequence_id":"5","track_id":"6"})"),
        envelope("pulp.timeline.command.insert_take",
                 R"({"lane_id":"12","sequence_id":"5","take":)" +
                     std::string(parsed->raw(take_value)) + R"(,"track_id":"6"})"),
        envelope("pulp.timeline.command.remove_take",
                 R"({"lane_id":"12","sequence_id":"5","take_id":"13","track_id":"6"})"),
        envelope("pulp.timeline.command.set_record_arm",
                 R"({"expected":true,"replacement":false,"sequence_id":"5","track_id":"6"})"),
        envelope(
            "pulp.timeline.command.set_active_take_lane",
            R"({"expected_lane_id":"12","replacement_lane_id":"0","sequence_id":"5","track_id":"6"})"),
        envelope("pulp.timeline.command.set_take_comp",
                 R"({"expected":)" + comp +
                     R"(,"lane_id":"12","replacement":[],"sequence_id":"5","track_id":"6"})"),
        envelope("pulp.timeline.command.set_track_freeze",
                 "{\"expected\":" + std::string(parsed->raw(freeze)) +
                     R"(,"sequence_id":"5","track_id":"6"})"),
    };
    std::string batch = "[";
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        if (index != 0)
            batch += ",";
        batch += encoded[index];
    }
    batch += "]";

    const auto commands = take(deserialize_commands(batch, registry));
    REQUIRE(commands.size() == std::variant_size_v<Command>);
    REQUIRE(std::holds_alternative<InsertClip>(commands[0]));
    REQUIRE(std::holds_alternative<RemoveClip>(commands[1]));
    REQUIRE(std::holds_alternative<InsertAutomationLane>(commands[2]));
    REQUIRE(std::holds_alternative<RemoveAutomationLane>(commands[3]));
    REQUIRE(std::holds_alternative<MoveClip>(commands[4]));
    REQUIRE(std::holds_alternative<SetNoteVelocity>(commands[5]));
    REQUIRE(std::holds_alternative<SetClipPlaybackProperties>(commands[6]));
    REQUIRE(std::holds_alternative<SetTempoMap>(commands[7]));
    REQUIRE(std::holds_alternative<SetMeterMap>(commands[8]));
    REQUIRE(std::holds_alternative<CreateAsset>(commands[9]));
    REQUIRE(std::holds_alternative<RemoveAsset>(commands[10]));
    REQUIRE(std::holds_alternative<InsertTakeLane>(commands[11]));
    REQUIRE(std::holds_alternative<RemoveTakeLane>(commands[12]));
    REQUIRE(std::holds_alternative<InsertTake>(commands[13]));
    REQUIRE(std::holds_alternative<RemoveTake>(commands[14]));
    REQUIRE(std::holds_alternative<SetRecordArm>(commands[15]));
    REQUIRE(std::holds_alternative<SetActiveTakeLane>(commands[16]));
    REQUIRE(std::holds_alternative<SetTakeComp>(commands[17]));
    REQUIRE(std::holds_alternative<SetTrackFreeze>(commands[18]));
}

TEST_CASE("Typed command JSON rejects unknown types and invalid scalar widths") {
    const auto registry = builtins();
    auto empty = deserialize_commands("[]", registry);
    REQUIRE_FALSE(empty);
    REQUIRE(empty.error().code == PersistenceErrorCode::InvalidSchema);

    auto unknown = deserialize_commands(
        R"([{"data":{},"type_name":"pulp.timeline.command.unknown","version":1}])", registry);
    REQUIRE_FALSE(unknown);
    REQUIRE(unknown.error().code == PersistenceErrorCode::UnsupportedStructuralType);

    auto velocity = deserialize_commands(
        R"([{"data":{"clip_id":"3","expected_velocity":65536,"note_id":"4","replacement_velocity":1,"sequence_id":"1","track_id":"2"},"type_name":"pulp.timeline.command.set_note_velocity","version":1}])",
        registry);
    REQUIRE_FALSE(velocity);
    REQUIRE(velocity.error().code == PersistenceErrorCode::InvalidNumber);
}

TEST_CASE("Decoded command batch reduces through the authoritative document session") {
    const auto registry = builtins();
    auto commands = take(deserialize_commands(
        R"([{"data":{"expected":true,"replacement":false,"sequence_id":"5","track_id":"6"},"type_name":"pulp.timeline.command.set_record_arm","version":1}])",
        registry));
    auto session = take(DocumentSession::create(command_payload_project()));
    auto writer = take(session->register_writer());
    Transaction transaction;
    transaction.id = writer.allocate_transaction_id();
    transaction.expected_revision = session->revision();
    transaction.commands.push_back({writer.allocate_command_id(), std::move(commands[0])});

    REQUIRE(session->submit(writer, std::move(transaction)));
    REQUIRE_FALSE(session->snapshot()->find_sequence({5})->find_track({6})->record_armed());
}
