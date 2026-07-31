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
    auto sequence = take(Sequence::create({5}, "root", TickDuration{100}, std::nullopt, {track},
                                          {SequenceMarker{{14}, "verse", {0}}},
                                          {SequenceRegion{{15}, "chorus", {0}, {50}}}));
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
    const auto& marker = member(member(sequence, "data"), "markers").array[0];
    const auto& region = member(member(sequence, "data"), "regions").array[0];
    const auto& groove = member(member(sequence, "data"), "groove");
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
    const std::string note =
        R"({"channel":0,"duration_ticks":"25","id":"10","pitch":60,"start_ticks":"0","velocity":32768})";
    const std::string transformed_note =
        R"({"channel":0,"duration_ticks":"25","id":"10","pitch":72,"start_ticks":"0","velocity":32768})";

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
        envelope("pulp.timeline.command.replace_note_content",
                 R"({"clip_id":"7","expected":[)" + note + R"(],"replacement":[)" +
                     transformed_note + R"(],"sequence_id":"5","track_id":"6"})"),
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
        envelope("pulp.timeline.command.insert_marker",
                 "{\"marker\":" + std::string(parsed->raw(marker)) + R"(,"sequence_id":"5"})"),
        envelope("pulp.timeline.command.remove_marker", R"({"marker_id":"14","sequence_id":"5"})"),
        envelope("pulp.timeline.command.insert_region",
                 "{\"region\":" + std::string(parsed->raw(region)) + R"(,"sequence_id":"5"})"),
        envelope("pulp.timeline.command.remove_region", R"({"region_id":"15","sequence_id":"5"})"),
        envelope(
            "pulp.timeline.command.set_chord_scale_lane",
            R"({"expected":[],"replacement":[{"chord_quality":"minor7","chord_root":9,"position":"0","scale_mode":"dorian","scale_root":9}],"sequence_id":"5"})"),
        envelope("pulp.timeline.command.set_groove",
                 "{\"expected\":" + std::string(parsed->raw(groove)) + ",\"replacement\":" +
                     std::string(parsed->raw(groove)) + R"(,"sequence_id":"5"})"),
        envelope(
            "pulp.timeline.command.insert_scene",
            R"({"before_scene_id":"32","scene":{"data":{"id":"30","name":"launch","slots":[]},"type_name":"pulp.timeline.scene","version":1},"sequence_id":"5"})"),
        envelope("pulp.timeline.command.remove_scene", R"({"scene_id":"30","sequence_id":"5"})"),
        envelope(
            "pulp.timeline.command.insert_slot",
            R"({"before_slot_id":"33","scene_id":"30","sequence_id":"5","slot":{"data":{"clip_id":"7","follow":{"choices":[],"grid":"0","repetitions":1},"id":"31","launch_quantize":{"grid":"0","phase":"0"}},"type_name":"pulp.timeline.slot","version":1}})"),
        envelope("pulp.timeline.command.remove_slot",
                 R"({"scene_id":"30","sequence_id":"5","slot_id":"31"})"),
        envelope("pulp.timeline.command.insert_sequence",
                 "{\"sequence\":" + std::string(parsed->raw(sequence)) + "}"),
        envelope("pulp.timeline.command.clone_sequence",
                 R"({"cloned_sequence_id":"30","id_remap":[{"new_id":"30","old_id":"5"},{"new_id":"31","old_id":"6"},{"new_id":"32","old_id":"7"},{"new_id":"33","old_id":"8"},{"new_id":"34","old_id":"9"},{"new_id":"35","old_id":"10"},{"new_id":"36","old_id":"11"},{"new_id":"37","old_id":"12"},{"new_id":"38","old_id":"13"},{"new_id":"39","old_id":"14"},{"new_id":"40","old_id":"15"}],"source_sequence_id":"5"})"),
        envelope("pulp.timeline.command.remove_sequence", R"({"sequence_id":"30"})"),
        envelope(
            "pulp.timeline.command.set_clip_sequence_ref",
            R"({"clip_id":"7","expected":{"sequence_id":"30","source_start":"0"},"replacement":{"sequence_id":"30","source_start":"100"},"sequence_id":"5","track_id":"6"})"),
        envelope(
            "pulp.timeline.command.set_track_mixer",
            R"({"expected":{"gain_linear_bits":"1065353216","pan_bits":"0"},)"
            R"("replacement":{"gain_linear_bits":"1056964608","pan_bits":"0"},)"
            R"("sequence_id":"5","track_id":"6"})"),
        envelope("pulp.timeline.command.insert_track",
                 R"({"before_track_id":"34","sequence_id":"5","track":)" +
                     std::string(parsed->raw(track)) + "}"),
        envelope("pulp.timeline.command.remove_track", R"({"sequence_id":"5","track_id":"6"})"),
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
    REQUIRE(std::holds_alternative<ReplaceNoteContent>(commands[6]));
    REQUIRE(std::holds_alternative<SetClipPlaybackProperties>(commands[7]));
    REQUIRE(std::holds_alternative<SetTempoMap>(commands[8]));
    REQUIRE(std::holds_alternative<SetMeterMap>(commands[9]));
    REQUIRE(std::holds_alternative<CreateAsset>(commands[10]));
    REQUIRE(std::holds_alternative<RemoveAsset>(commands[11]));
    REQUIRE(std::holds_alternative<InsertTakeLane>(commands[12]));
    REQUIRE(std::holds_alternative<RemoveTakeLane>(commands[13]));
    REQUIRE(std::holds_alternative<InsertTake>(commands[14]));
    REQUIRE(std::holds_alternative<RemoveTake>(commands[15]));
    REQUIRE(std::holds_alternative<SetRecordArm>(commands[16]));
    REQUIRE(std::holds_alternative<SetActiveTakeLane>(commands[17]));
    REQUIRE(std::holds_alternative<SetTakeComp>(commands[18]));
    REQUIRE(std::holds_alternative<SetTrackFreeze>(commands[19]));
    REQUIRE(std::holds_alternative<InsertMarker>(commands[20]));
    REQUIRE(std::holds_alternative<RemoveMarker>(commands[21]));
    REQUIRE(std::holds_alternative<InsertRegion>(commands[22]));
    REQUIRE(std::holds_alternative<RemoveRegion>(commands[23]));
    REQUIRE(std::holds_alternative<SetChordScaleLane>(commands[24]));
    REQUIRE(std::holds_alternative<SetGroove>(commands[25]));
    REQUIRE(std::holds_alternative<InsertScene>(commands[26]));
    REQUIRE(std::get<InsertScene>(commands[26]).before_scene_id == ItemId{32});
    REQUIRE(std::holds_alternative<RemoveScene>(commands[27]));
    REQUIRE(std::holds_alternative<InsertSlot>(commands[28]));
    REQUIRE(std::get<InsertSlot>(commands[28]).before_slot_id == ItemId{33});
    REQUIRE(std::holds_alternative<RemoveSlot>(commands[29]));
    REQUIRE(std::holds_alternative<InsertSequence>(commands[30]));
    REQUIRE(std::holds_alternative<CloneSequence>(commands[31]));
    REQUIRE(std::holds_alternative<RemoveSequence>(commands[32]));
    REQUIRE(std::holds_alternative<SetClipSequenceRef>(commands[33]));
    REQUIRE(std::holds_alternative<SetTrackMixer>(commands[34]));
    REQUIRE(std::get<SetTrackMixer>(commands[34]).replacement == TrackMixer{0.5f, 0.0f});
    REQUIRE(std::holds_alternative<InsertTrack>(commands[35]));
    REQUIRE(std::get<InsertTrack>(commands[35]).before_track_id == ItemId{34});
    REQUIRE(std::get<InsertTrack>(commands[35]).track.id() == ItemId{6});
    REQUIRE(std::holds_alternative<RemoveTrack>(commands[36]));

    DecodeLimits no_scenes;
    no_scenes.max_scenes = 0;
    auto scene_limited = deserialize_commands(batch, registry, no_scenes);
    REQUIRE_FALSE(scene_limited);
    REQUIRE(scene_limited.error().code == PersistenceErrorCode::LimitExceeded);

    DecodeLimits no_slots;
    no_slots.max_slots = 0;
    auto slot_limited = deserialize_commands(batch, registry, no_slots);
    REQUIRE_FALSE(slot_limited);
    REQUIRE(slot_limited.error().code == PersistenceErrorCode::LimitExceeded);
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

TEST_CASE("Typed InsertScene command enforces the slot quota before decoding elements") {
    const auto registry = builtins();
    const std::string slot =
        R"({"data":{"clip_id":"7","follow":{"choices":[],"grid":"0","repetitions":1},"id":")";
    const auto command =
        R"([{"data":{"scene":{"data":{"id":"30","name":"launch","slots":[)" + slot + "31" +
        R"(","launch_quantize":{"grid":"0","phase":"0"}},"type_name":"pulp.timeline.slot","version":1},)" +
        slot + "32" +
        R"(","launch_quantize":{"grid":"0","phase":"0"}},"type_name":"pulp.timeline.slot","version":1},)" +
        slot + "33" +
        R"(","launch_quantize":{"grid":"0","phase":"0"}},"type_name":"pulp.timeline.slot","version":1}]},"type_name":"pulp.timeline.scene","version":1},"sequence_id":"5"},"type_name":"pulp.timeline.command.insert_scene","version":1}])";
    DecodeLimits limits;
    limits.max_slots = 2;

    auto rejected = deserialize_commands(command, registry, limits);

    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == PersistenceErrorCode::LimitExceeded);
    REQUIRE(rejected.error().path == "/0/data/scene/data/slots");
    REQUIRE(rejected.error().actual == 3);
    REQUIRE(rejected.error().limit == 2);
}

TEST_CASE("Typed scene commands preserve exact nested field diagnostics") {
    const auto registry = builtins();
    auto invalid_follow = deserialize_commands(
        R"([{"data":{"scene_id":"30","sequence_id":"5","slot":{"data":{"clip_id":"7","follow":null,"id":"31","launch_quantize":{"grid":"0","phase":"0"}},"type_name":"pulp.timeline.slot","version":1}},"type_name":"pulp.timeline.command.insert_slot","version":1}])",
        registry);
    REQUIRE_FALSE(invalid_follow);
    REQUIRE(invalid_follow.error().code == PersistenceErrorCode::InvalidSchema);
    REQUIRE(invalid_follow.error().path == "/0/data/slot/data/follow");

    auto missing_slots = deserialize_commands(
        R"([{"data":{"scene":{"data":{"id":"30","name":"launch"},"type_name":"pulp.timeline.scene","version":1},"sequence_id":"5"},"type_name":"pulp.timeline.command.insert_scene","version":1}])",
        registry);
    REQUIRE_FALSE(missing_slots);
    REQUIRE(missing_slots.error().code == PersistenceErrorCode::MissingField);
    REQUIRE(missing_slots.error().path == "/0/data/scene/data/slots");

    auto invalid_scene_anchor = deserialize_commands(
        R"([{"data":{"before_scene_id":"01","scene":{"data":{"id":"30","name":"launch","slots":[]},"type_name":"pulp.timeline.scene","version":1},"sequence_id":"5"},"type_name":"pulp.timeline.command.insert_scene","version":1}])",
        registry);
    REQUIRE_FALSE(invalid_scene_anchor);
    REQUIRE(invalid_scene_anchor.error().code == PersistenceErrorCode::InvalidNumber);
    REQUIRE(invalid_scene_anchor.error().path == "/0/data/before_scene_id");

    auto invalid_slot_anchor = deserialize_commands(
        R"([{"data":{"before_slot_id":"01","scene_id":"30","sequence_id":"5","slot":{"data":{"clip_id":"7","follow":{"choices":[],"grid":"0","repetitions":1},"id":"31","launch_quantize":{"grid":"0","phase":"0"}},"type_name":"pulp.timeline.slot","version":1}},"type_name":"pulp.timeline.command.insert_slot","version":1}])",
        registry);
    REQUIRE_FALSE(invalid_slot_anchor);
    REQUIRE(invalid_slot_anchor.error().code == PersistenceErrorCode::InvalidNumber);
    REQUIRE(invalid_slot_anchor.error().path == "/0/data/before_slot_id");
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
