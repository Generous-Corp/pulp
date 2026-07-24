#include <pulp/timeline/serialize.hpp>

#include "serialize_automation_decode.hpp"
#include "serialize_decode_context.hpp"

#include <array>
#include <bit>
#include <limits>

namespace pulp::timeline::detail {

runtime::Result<ItemId, PersistenceError>
decode_command_item_id(const JsonValue& data, std::string_view name, const std::string& path) {
    auto value = required(data, name, path);
    if (!value)
        return fail<ItemId>(value.error().code, value.error().path, value.error().byte_offset);
    auto decoded = parse_canonical_u64_string(*value.value(), path + "/" + std::string(name));
    if (!decoded)
        return fail<ItemId>(decoded.error().code, decoded.error().path,
                            decoded.error().byte_offset);
    return runtime::Ok(ItemId{decoded.value()});
}

runtime::Result<bool, PersistenceError>
decode_command_bool(const JsonValue& data, std::string_view name, const std::string& path) {
    auto value = required(data, name, path);
    if (!value)
        return fail<bool>(value.error().code, value.error().path, value.error().byte_offset);
    if (value.value()->kind != JsonValue::Kind::Boolean)
        return fail<bool>(PersistenceErrorCode::UnexpectedType, path + "/" + std::string(name),
                          value.value()->begin);
    return runtime::Ok(value.value()->boolean);
}

runtime::Result<ClipPlaybackProperties, PersistenceError>
decode_command_playback_properties(const JsonValue& value, std::string path) {
    auto gain = required(value, "gain_linear_bits", path);
    auto fade_in = required(value, "fade_in_duration", path);
    auto fade_out = required(value, "fade_out_duration", path);
    if (!gain || !fade_in || !fade_out)
        return fail<ClipPlaybackProperties>(PersistenceErrorCode::MissingField, std::move(path));
    auto decoded_gain = parse_canonical_u64_string(*gain.value(), path + "/gain_linear_bits");
    auto decoded_fade_in = parse_canonical_u64_string(*fade_in.value(), path + "/fade_in_duration");
    auto decoded_fade_out =
        parse_canonical_u64_string(*fade_out.value(), path + "/fade_out_duration");
    if (!decoded_gain || !decoded_fade_in || !decoded_fade_out ||
        decoded_gain.value() > std::numeric_limits<std::uint32_t>::max())
        return fail<ClipPlaybackProperties>(PersistenceErrorCode::InvalidNumber, std::move(path));
    return runtime::Ok(ClipPlaybackProperties{
        std::bit_cast<float>(static_cast<std::uint32_t>(decoded_gain.value())),
        decoded_fade_in.value(), decoded_fade_out.value()});
}

runtime::Result<ClipTimeRange, PersistenceError> decode_command_clip_range(const JsonValue& value,
                                                                           std::string path) {
    auto kind = string_field(value, "kind", path);
    if (!kind)
        return fail<ClipTimeRange>(kind.error().code, kind.error().path, kind.error().byte_offset);
    if (kind.value() == "musical") {
        auto start = required(value, "start_ticks", path);
        auto duration = required(value, "duration_ticks", path);
        if (!start || !duration)
            return fail<ClipTimeRange>(PersistenceErrorCode::MissingField, std::move(path));
        auto decoded_start = parse_canonical_i64_string(*start.value(), path + "/start_ticks");
        auto decoded_duration =
            parse_canonical_i64_string(*duration.value(), path + "/duration_ticks");
        if (!decoded_start || !decoded_duration)
            return fail<ClipTimeRange>(PersistenceErrorCode::InvalidNumber, std::move(path));
        return runtime::Ok(
            ClipTimeRange(MusicalTimeRange{{decoded_start.value()}, {decoded_duration.value()}}));
    }
    if (kind.value() == "absolute") {
        auto start = required(value, "start_sample", path);
        auto count = required(value, "sample_count", path);
        auto rate = required(value, "sample_rate", path);
        if (!start || !count || !rate)
            return fail<ClipTimeRange>(PersistenceErrorCode::MissingField, std::move(path));
        auto decoded_start = parse_canonical_i64_string(*start.value(), path + "/start_sample");
        auto decoded_count = parse_canonical_u64_string(*count.value(), path + "/sample_count");
        auto decoded_rate = decode_rate(*rate.value(), path + "/sample_rate");
        if (!decoded_start || !decoded_count || !decoded_rate)
            return fail<ClipTimeRange>(PersistenceErrorCode::InvalidNumber, std::move(path));
        return runtime::Ok(ClipTimeRange(AbsoluteTimeRange{
            {decoded_start.value()}, decoded_count.value(), decoded_rate.value()}));
    }
    return fail<ClipTimeRange>(PersistenceErrorCode::InvalidSchema, path + "/kind");
}

runtime::Result<std::vector<TakeCompSegment>, PersistenceError>
decode_command_take_comp(const JsonValue& value, DecodeContext& context, std::string path) {
    const auto& limits = context.limits;
    auto& total_count = context.counts.take_comp_segments;
    if (value.kind != JsonValue::Kind::Array)
        return fail<std::vector<TakeCompSegment>>(PersistenceErrorCode::UnexpectedType,
                                                  std::move(path), value.begin);
    if (value.array.size() >
        limits.max_take_comp_segments - std::min(total_count, limits.max_take_comp_segments))
        return fail<std::vector<TakeCompSegment>>(
            PersistenceErrorCode::LimitExceeded, std::move(path), value.begin,
            total_count + value.array.size(), limits.max_take_comp_segments);
    total_count += value.array.size();
    std::vector<TakeCompSegment> result;
    result.reserve(value.array.size());
    for (std::size_t index = 0; index < value.array.size(); ++index) {
        const auto item_path = path + "/" + std::to_string(index);
        auto count = required(value.array[index], "sample_count", item_path);
        auto rate = required(value.array[index], "sample_rate", item_path);
        auto start = required(value.array[index], "start", item_path);
        auto take_id = required(value.array[index], "take_id", item_path);
        if (!count || !rate || !start || !take_id)
            return fail<std::vector<TakeCompSegment>>(PersistenceErrorCode::MissingField,
                                                      item_path);
        auto decoded_count =
            parse_canonical_u64_string(*count.value(), item_path + "/sample_count");
        auto decoded_rate = decode_rate(*rate.value(), item_path + "/sample_rate");
        auto decoded_start = parse_canonical_i64_string(*start.value(), item_path + "/start");
        auto decoded_take_id = parse_canonical_u64_string(*take_id.value(), item_path + "/take_id");
        if (!decoded_count || !decoded_rate || !decoded_start || !decoded_take_id)
            return fail<std::vector<TakeCompSegment>>(PersistenceErrorCode::InvalidNumber,
                                                      item_path);
        result.push_back({ItemId{decoded_take_id.value()},
                          {{decoded_start.value()}, decoded_count.value(), decoded_rate.value()}});
    }
    return runtime::Ok(std::move(result));
}

runtime::Result<Command, PersistenceError>
decode_command(const std::shared_ptr<const ParsedJson>& document, const JsonValue& value,
               const SchemaRegistry& registry, DecodeContext& context, std::string path) {
    const auto& limits = context.limits;
    auto& counts = context.counts;
    auto type = string_field(value, "type_name", path);
    auto version = required(value, "version", path);
    if (!type || !version)
        return fail<Command>(PersistenceErrorCode::MissingField, std::move(path));
    auto decoded_version = parse_u32_number(*version.value(), path + "/version");
    if (!decoded_version)
        return fail<Command>(decoded_version.error().code, decoded_version.error().path,
                             decoded_version.error().byte_offset);
    const auto* schema = registry.find(SchemaDomain::Command, type.value());
    if (!schema)
        return fail<Command>(PersistenceErrorCode::UnsupportedStructuralType, std::move(path),
                             value.begin);
    if (decoded_version.value() != schema->current_version)
        return fail<Command>(PersistenceErrorCode::UnsupportedSchemaVersion, std::move(path),
                             value.begin);
    auto data = required(value, "data", path);
    if (!data || data.value()->kind != JsonValue::Kind::Object)
        return fail<Command>(PersistenceErrorCode::UnexpectedType, path + "/data");
    const auto& command = *data.value();
    const auto data_path = path + "/data";
    auto ids = [&]() -> runtime::Result<std::array<ItemId, 3>, PersistenceError> {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto track = decode_command_item_id(command, "track_id", data_path);
        auto clip = decode_command_item_id(command, "clip_id", data_path);
        if (!sequence || !track || !clip)
            return fail<std::array<ItemId, 3>>(PersistenceErrorCode::MissingField, data_path);
        return runtime::Ok(std::array{sequence.value(), track.value(), clip.value()});
    };

    if (type.value() == "pulp.timeline.command.insert_clip") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto track = decode_command_item_id(command, "track_id", data_path);
        auto clip = required(command, "clip", data_path);
        if (!sequence || !track || !clip)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        auto decoded = decode_clip(document, *clip.value(), registry, context, data_path + "/clip");
        if (!decoded)
            return runtime::Err(decoded.error());
        return runtime::Ok(
            Command(InsertClip{sequence.value(), track.value(), std::move(decoded).value()}));
    }
    if (type.value() == "pulp.timeline.command.remove_clip") {
        auto decoded = ids();
        if (!decoded)
            return runtime::Err(decoded.error());
        return runtime::Ok(
            Command(RemoveClip{decoded.value()[0], decoded.value()[1], decoded.value()[2]}));
    }
    if (type.value() == "pulp.timeline.command.insert_automation_lane") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto track = decode_command_item_id(command, "track_id", data_path);
        auto lane = required(command, "lane", data_path);
        if (!sequence || !track || !lane)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        JsonValue lanes;
        lanes.kind = JsonValue::Kind::Array;
        lanes.array.push_back(*lane.value());
        auto decoded = detail::decode_automation_lanes(
            lanes, limits, counts.automation_lanes, counts.automation_points, data_path + "/lane");
        if (!decoded || decoded.value().size() != 1)
            return fail<Command>(decoded ? PersistenceErrorCode::InvalidSchema
                                         : decoded.error().code,
                                 data_path + "/lane");
        return runtime::Ok(Command(
            InsertAutomationLane{sequence.value(), track.value(), std::move(decoded).value()[0]}));
    }
    if (type.value() == "pulp.timeline.command.remove_automation_lane") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto track = decode_command_item_id(command, "track_id", data_path);
        auto lane = decode_command_item_id(command, "lane_id", data_path);
        if (!sequence || !track || !lane)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        return runtime::Ok(
            Command(RemoveAutomationLane{sequence.value(), track.value(), lane.value()}));
    }
    if (type.value() == "pulp.timeline.command.move_clip") {
        auto decoded = ids();
        auto expected = required(command, "expected_range", data_path);
        auto replacement = required(command, "replacement_range", data_path);
        if (!decoded || !expected || !replacement)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        auto decoded_expected =
            decode_command_clip_range(*expected.value(), data_path + "/expected_range");
        auto decoded_replacement =
            decode_command_clip_range(*replacement.value(), data_path + "/replacement_range");
        if (!decoded_expected || !decoded_replacement)
            return fail<Command>(PersistenceErrorCode::InvalidSchema, data_path);
        return runtime::Ok(Command(MoveClip{decoded.value()[0], decoded.value()[1],
                                            decoded.value()[2], std::move(decoded_expected).value(),
                                            std::move(decoded_replacement).value()}));
    }
    if (type.value() == "pulp.timeline.command.set_note_velocity") {
        auto decoded = ids();
        auto note = decode_command_item_id(command, "note_id", data_path);
        auto expected = required(command, "expected_velocity", data_path);
        auto replacement = required(command, "replacement_velocity", data_path);
        if (!decoded || !note || !expected || !replacement)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        auto decoded_expected =
            parse_u32_number(*expected.value(), data_path + "/expected_velocity");
        auto decoded_replacement =
            parse_u32_number(*replacement.value(), data_path + "/replacement_velocity");
        if (!decoded_expected || !decoded_replacement ||
            decoded_expected.value() > std::numeric_limits<std::uint16_t>::max() ||
            decoded_replacement.value() > std::numeric_limits<std::uint16_t>::max())
            return fail<Command>(PersistenceErrorCode::InvalidNumber, data_path);
        return runtime::Ok(Command(
            SetNoteVelocity{decoded.value()[0], decoded.value()[1], decoded.value()[2],
                            note.value(), static_cast<std::uint16_t>(decoded_expected.value()),
                            static_cast<std::uint16_t>(decoded_replacement.value())}));
    }
    if (type.value() == "pulp.timeline.command.set_clip_playback_properties") {
        auto decoded = ids();
        auto expected = required(command, "expected", data_path);
        auto replacement = required(command, "replacement", data_path);
        if (!decoded || !expected || !replacement)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        auto decoded_expected =
            decode_command_playback_properties(*expected.value(), data_path + "/expected");
        auto decoded_replacement =
            decode_command_playback_properties(*replacement.value(), data_path + "/replacement");
        if (!decoded_expected || !decoded_replacement)
            return fail<Command>(PersistenceErrorCode::InvalidSchema, data_path);
        return runtime::Ok(Command(
            SetClipPlaybackProperties{decoded.value()[0], decoded.value()[1], decoded.value()[2],
                                      decoded_expected.value(), decoded_replacement.value()}));
    }
    if (type.value() == "pulp.timeline.command.set_tempo_map" ||
        type.value() == "pulp.timeline.command.set_meter_map") {
        auto expected = required(command, "expected", data_path);
        auto replacement = required(command, "replacement", data_path);
        if (!expected || !replacement)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        if (type.value() == "pulp.timeline.command.set_tempo_map") {
            auto decoded_expected = decode_tempo_map(*expected.value(), data_path + "/expected");
            auto decoded_replacement =
                decode_tempo_map(*replacement.value(), data_path + "/replacement");
            if (!decoded_expected || !decoded_replacement)
                return fail<Command>(PersistenceErrorCode::InvalidSchema, data_path);
            return runtime::Ok(Command(SetTempoMap{std::move(decoded_expected).value(),
                                                   std::move(decoded_replacement).value()}));
        }
        auto decoded_expected = decode_meter_map(*expected.value(), data_path + "/expected");
        auto decoded_replacement =
            decode_meter_map(*replacement.value(), data_path + "/replacement");
        if (!decoded_expected || !decoded_replacement)
            return fail<Command>(PersistenceErrorCode::InvalidSchema, data_path);
        return runtime::Ok(Command(SetMeterMap{std::move(decoded_expected).value(),
                                               std::move(decoded_replacement).value()}));
    }
    if (type.value() == "pulp.timeline.command.create_asset") {
        auto asset = required(command, "asset", data_path);
        if (!asset)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        auto decoded = decode_asset(*asset.value(), data_path + "/asset", context);
        if (!decoded)
            return runtime::Err(decoded.error());
        return runtime::Ok(Command(CreateAsset{std::move(decoded).value()}));
    }
    if (type.value() == "pulp.timeline.command.remove_asset") {
        auto asset = decode_command_item_id(command, "asset_id", data_path);
        if (!asset)
            return runtime::Err(asset.error());
        return runtime::Ok(Command(RemoveAsset{asset.value()}));
    }
    if (type.value() == "pulp.timeline.command.insert_take_lane") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto track = decode_command_item_id(command, "track_id", data_path);
        auto lane = required(command, "lane", data_path);
        if (!sequence || !track || !lane)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        auto decoded = decode_take_lane(*lane.value(), context, data_path + "/lane");
        if (!decoded)
            return runtime::Err(decoded.error());
        return runtime::Ok(
            Command(InsertTakeLane{sequence.value(), track.value(), std::move(decoded).value()}));
    }
    if (type.value() == "pulp.timeline.command.remove_take_lane") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto track = decode_command_item_id(command, "track_id", data_path);
        auto lane = decode_command_item_id(command, "lane_id", data_path);
        if (!sequence || !track || !lane)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        return runtime::Ok(Command(RemoveTakeLane{sequence.value(), track.value(), lane.value()}));
    }
    if (type.value() == "pulp.timeline.command.insert_take") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto track = decode_command_item_id(command, "track_id", data_path);
        auto lane = decode_command_item_id(command, "lane_id", data_path);
        auto take = required(command, "take", data_path);
        if (!sequence || !track || !lane || !take)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        auto decoded = decode_take(*take.value(), context, data_path + "/take");
        if (!decoded)
            return runtime::Err(decoded.error());
        return runtime::Ok(Command(
            InsertTake{sequence.value(), track.value(), lane.value(), std::move(decoded).value()}));
    }
    if (type.value() == "pulp.timeline.command.remove_take") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto track = decode_command_item_id(command, "track_id", data_path);
        auto lane = decode_command_item_id(command, "lane_id", data_path);
        auto take = decode_command_item_id(command, "take_id", data_path);
        if (!sequence || !track || !lane || !take)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        return runtime::Ok(
            Command(RemoveTake{sequence.value(), track.value(), lane.value(), take.value()}));
    }
    if (type.value() == "pulp.timeline.command.set_record_arm") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto track = decode_command_item_id(command, "track_id", data_path);
        auto expected = decode_command_bool(command, "expected", data_path);
        auto replacement = decode_command_bool(command, "replacement", data_path);
        if (!sequence || !track || !expected || !replacement)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        return runtime::Ok(Command(
            SetRecordArm{sequence.value(), track.value(), expected.value(), replacement.value()}));
    }
    if (type.value() == "pulp.timeline.command.set_active_take_lane") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto track = decode_command_item_id(command, "track_id", data_path);
        auto expected = decode_command_item_id(command, "expected_lane_id", data_path);
        auto replacement = decode_command_item_id(command, "replacement_lane_id", data_path);
        if (!sequence || !track || !expected || !replacement)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        return runtime::Ok(Command(SetActiveTakeLane{sequence.value(), track.value(),
                                                     expected.value(), replacement.value()}));
    }
    if (type.value() == "pulp.timeline.command.set_take_comp") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto track = decode_command_item_id(command, "track_id", data_path);
        auto lane = decode_command_item_id(command, "lane_id", data_path);
        auto expected = required(command, "expected", data_path);
        auto replacement = required(command, "replacement", data_path);
        if (!sequence || !track || !lane || !expected || !replacement)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        auto decoded_expected =
            decode_command_take_comp(*expected.value(), context, data_path + "/expected");
        auto decoded_replacement =
            decode_command_take_comp(*replacement.value(), context, data_path + "/replacement");
        if (!decoded_expected || !decoded_replacement)
            return fail<Command>(PersistenceErrorCode::InvalidSchema, data_path);
        return runtime::Ok(Command(SetTakeComp{sequence.value(), track.value(), lane.value(),
                                               std::move(decoded_expected).value(),
                                               std::move(decoded_replacement).value()}));
    }
    if (type.value() == "pulp.timeline.command.set_track_freeze") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto track = decode_command_item_id(command, "track_id", data_path);
        if (!sequence || !track)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        auto expected =
            detail::decode_track_freeze(command.find("expected"), data_path + "/expected");
        auto replacement =
            detail::decode_track_freeze(command.find("replacement"), data_path + "/replacement");
        if (!expected || !replacement)
            return fail<Command>(PersistenceErrorCode::InvalidSchema, data_path);
        return runtime::Ok(
            Command(SetTrackFreeze{sequence.value(), track.value(), std::move(expected).value(),
                                   std::move(replacement).value()}));
    }
    return fail<Command>(PersistenceErrorCode::UnsupportedStructuralType, std::move(path),
                         value.begin);
}

} // namespace pulp::timeline::detail

namespace pulp::timeline {

using detail::decode_command;
using detail::DecodeContext;
using detail::fail;

runtime::Result<std::vector<Command>, PersistenceError>
deserialize_commands(std::string_view json, const SchemaRegistry& registry,
                     const DecodeLimits& limits) {
    auto parsed = parse_json(json, limits);
    if (!parsed)
        return fail<std::vector<Command>>(parsed.error().code, parsed.error().path,
                                          parsed.error().byte_offset, parsed.error().actual,
                                          parsed.error().limit);
    if (parsed.value()->root().kind != JsonValue::Kind::Array)
        return fail<std::vector<Command>>(PersistenceErrorCode::UnexpectedType, "/",
                                          parsed.value()->root().begin);
    if (parsed.value()->root().array.empty())
        return fail<std::vector<Command>>(PersistenceErrorCode::InvalidSchema, "/",
                                          parsed.value()->root().begin);
    if (parsed.value()->root().array.size() > limits.max_array_elements)
        return fail<std::vector<Command>>(
            PersistenceErrorCode::LimitExceeded, "/", parsed.value()->root().begin,
            parsed.value()->root().array.size(), limits.max_array_elements);
    std::vector<Command> commands;
    commands.reserve(parsed.value()->root().array.size());
    DecodeContext context(limits);
    for (std::size_t index = 0; index < parsed.value()->root().array.size(); ++index) {
        auto decoded = decode_command(parsed.value(), parsed.value()->root().array[index], registry,
                                      context, "/" + std::to_string(index));
        if (!decoded)
            return runtime::Err(decoded.error());
        commands.push_back(std::move(decoded).value());
    }
    return runtime::Ok(std::move(commands));
}

} // namespace pulp::timeline
