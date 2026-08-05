#include <pulp/timeline/serialize.hpp>

#include "serialize_automation_decode.hpp"
#include "serialize_decode_context.hpp"
#include "serialize_decode_support.hpp"

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

runtime::Result<std::optional<ItemId>, PersistenceError>
decode_optional_command_item_id(const JsonValue& data, std::string_view name,
                                const std::string& path) {
    const auto* value = data.find(name);
    if (!value)
        return runtime::Ok(std::optional<ItemId>{});
    auto decoded = parse_canonical_u64_string(*value, path + "/" + std::string(name));
    if (!decoded)
        return fail<std::optional<ItemId>>(decoded.error().code, decoded.error().path,
                                           decoded.error().byte_offset);
    return runtime::Ok(std::optional<ItemId>{ItemId{decoded.value()}});
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
    // Optional, unlike the document field. Commands are authored input with no
    // version-gated migration path of their own, so an omitted shape has to keep
    // meaning what it meant before the field existed.
    ClipFadeShape fade_shape = ClipFadeShape::Linear;
    if (const auto* shape = value.find("fade_shape")) {
        if (shape->kind != JsonValue::Kind::String)
            return fail<ClipPlaybackProperties>(PersistenceErrorCode::UnexpectedType,
                                                path + "/fade_shape");
        if (shape->scalar == "equal_power")
            fade_shape = ClipFadeShape::EqualPower;
        else if (shape->scalar != "linear")
            return fail<ClipPlaybackProperties>(PersistenceErrorCode::InvalidSchema,
                                                path + "/fade_shape");
    }
    return runtime::Ok(ClipPlaybackProperties{
        std::bit_cast<float>(static_cast<std::uint32_t>(decoded_gain.value())),
        decoded_fade_in.value(), decoded_fade_out.value(), fade_shape});
}

runtime::Result<std::vector<NoteEvent>, PersistenceError>
decode_command_notes(const JsonValue& value, DecodeContext& context, std::string path) {
    if (value.kind != JsonValue::Kind::Array)
        return fail<std::vector<NoteEvent>>(PersistenceErrorCode::UnexpectedType, std::move(path),
                                            value.begin);
    auto& count = context.counts.notes;
    if (count > context.limits.max_notes || value.array.size() > context.limits.max_notes - count)
        return fail<std::vector<NoteEvent>>(PersistenceErrorCode::LimitExceeded, std::move(path),
                                            value.begin, count + value.array.size(),
                                            context.limits.max_notes);
    std::vector<NoteEvent> notes;
    notes.reserve(value.array.size());
    for (std::size_t index = 0; index < value.array.size(); ++index) {
        const auto item_path = path + "/" + std::to_string(index);
        const auto& item = value.array[index];
        auto id = required(item, "id", item_path);
        auto start = required(item, "start_ticks", item_path);
        auto duration = required(item, "duration_ticks", item_path);
        auto velocity = required(item, "velocity", item_path);
        auto pitch = required(item, "pitch", item_path);
        auto channel = required(item, "channel", item_path);
        if (!id || !start || !duration || !velocity || !pitch || !channel)
            return fail<std::vector<NoteEvent>>(PersistenceErrorCode::MissingField, item_path);
        auto decoded_id = parse_canonical_u64_string(*id.value(), item_path + "/id");
        auto decoded_start = parse_canonical_i64_string(*start.value(), item_path + "/start_ticks");
        auto decoded_duration =
            parse_canonical_i64_string(*duration.value(), item_path + "/duration_ticks");
        auto decoded_velocity = parse_u32_number(*velocity.value(), item_path + "/velocity");
        auto decoded_pitch = parse_u32_number(*pitch.value(), item_path + "/pitch");
        auto decoded_channel = parse_u32_number(*channel.value(), item_path + "/channel");
        if (!decoded_id || !decoded_start || !decoded_duration || !decoded_velocity ||
            !decoded_pitch || !decoded_channel ||
            decoded_velocity.value() > std::numeric_limits<std::uint16_t>::max() ||
            decoded_pitch.value() > std::numeric_limits<std::uint8_t>::max() ||
            decoded_channel.value() > std::numeric_limits<std::uint8_t>::max())
            return fail<std::vector<NoteEvent>>(PersistenceErrorCode::InvalidNumber, item_path);
        notes.push_back({ItemId{decoded_id.value()},
                         {decoded_start.value()},
                         {decoded_duration.value()},
                         static_cast<std::uint16_t>(decoded_velocity.value()),
                         static_cast<std::uint8_t>(decoded_pitch.value()),
                         static_cast<std::uint8_t>(decoded_channel.value())});
    }
    count += notes.size();
    auto validated = MidiContent::create(std::move(notes));
    if (!validated)
        return model_fail<std::vector<NoteEvent>>(validated.error(), std::move(path));
    return runtime::Ok(std::vector<NoteEvent>(validated->notes().begin(),
                                              validated->notes().end()));
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
    const auto decode_sequence_ref =
        [&](const JsonValue& object,
            const std::string& object_path) -> runtime::Result<SequenceRef, PersistenceError> {
        auto sequence = decode_command_item_id(object, "sequence_id", object_path);
        auto source = required(object, "source_start", object_path);
        if (!sequence || !source)
            return fail<SequenceRef>(PersistenceErrorCode::MissingField, object_path);
        auto decoded_source =
            parse_canonical_i64_string(*source.value(), object_path + "/source_start");
        if (!decoded_source)
            return fail<SequenceRef>(decoded_source.error().code,
                                     decoded_source.error().path,
                                     decoded_source.error().byte_offset);
        return runtime::Ok(
            SequenceRef{sequence.value(), timebase::TickPosition{decoded_source.value()}});
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
    if (type.value() == "pulp.timeline.command.replace_note_content") {
        auto decoded = ids();
        auto expected = required(command, "expected", data_path);
        auto replacement = required(command, "replacement", data_path);
        if (!decoded || !expected || !replacement)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        auto decoded_expected =
            decode_command_notes(*expected.value(), context, data_path + "/expected");
        if (!decoded_expected)
            return runtime::Err(decoded_expected.error());
        auto decoded_replacement =
            decode_command_notes(*replacement.value(), context, data_path + "/replacement");
        if (!decoded_replacement)
            return runtime::Err(decoded_replacement.error());
        // Optional, like the fade shape above and for the same reason: a
        // command carries no migration path, so an envelope written before
        // these fields existed has to keep decoding to what it meant then —
        // omitted arrays, and a reducer that derives the surviving modifiers
        // from the clip.
        std::vector<NoteModifier> expected_modifiers;
        std::vector<NoteModifier> replacement_modifiers;
        if (const auto* value = command.find("expected_modifiers")) {
            auto modifiers = decode_note_modifiers(*value, decoded_expected.value().size(),
                                                   data_path + "/expected_modifiers");
            if (!modifiers)
                return runtime::Err(modifiers.error());
            expected_modifiers = std::move(modifiers).value();
        }
        if (const auto* value = command.find("replacement_modifiers")) {
            auto modifiers = decode_note_modifiers(*value, decoded_replacement.value().size(),
                                                   data_path + "/replacement_modifiers");
            if (!modifiers)
                return runtime::Err(modifiers.error());
            replacement_modifiers = std::move(modifiers).value();
        }
        return runtime::Ok(Command(ReplaceNoteContent{
            decoded.value()[0], decoded.value()[1], decoded.value()[2],
            std::move(decoded_expected).value(), std::move(decoded_replacement).value(),
            std::move(expected_modifiers), std::move(replacement_modifiers)}));
    }
    if (type.value() == "pulp.timeline.command.set_note_events") {
        auto decoded = ids();
        auto expected = required(command, "expected", data_path);
        auto replacement = required(command, "replacement", data_path);
        if (!decoded || !expected || !replacement)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        // Both arrays are note arrays, decoded exactly as a clip's notes are, so
        // a malformed note is rejected here rather than at the reducer. Whether
        // the two arrays pair up is a reduction question, not a decode one: it
        // needs the clip they name.
        auto decoded_expected =
            decode_command_notes(*expected.value(), context, data_path + "/expected");
        if (!decoded_expected)
            return runtime::Err(decoded_expected.error());
        auto decoded_replacement =
            decode_command_notes(*replacement.value(), context, data_path + "/replacement");
        if (!decoded_replacement)
            return runtime::Err(decoded_replacement.error());
        return runtime::Ok(Command(SetNoteEvents{decoded.value()[0], decoded.value()[1],
                                                 decoded.value()[2],
                                                 std::move(decoded_expected).value(),
                                                 std::move(decoded_replacement).value()}));
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
    if (type.value() == "pulp.timeline.command.set_chord_scale_lane") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto expected = required(command, "expected", data_path);
        auto replacement = required(command, "replacement", data_path);
        if (!sequence || !expected || !replacement ||
            expected.value()->kind != JsonValue::Kind::Array ||
            replacement.value()->kind != JsonValue::Kind::Array)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        auto decoded_expected =
            decode_chord_scale_lane(expected.value(), MemberPolicy::Optional, context,
                                    data_path + "/expected");
        auto decoded_replacement =
            decode_chord_scale_lane(replacement.value(), MemberPolicy::Optional, context,
                                    data_path + "/replacement");
        if (!decoded_expected)
            return runtime::Err(decoded_expected.error());
        if (!decoded_replacement)
            return runtime::Err(decoded_replacement.error());
        return runtime::Ok(
            Command(SetChordScaleLane{sequence.value(), std::move(decoded_expected).value(),
                                      std::move(decoded_replacement).value()}));
    }
    if (type.value() == "pulp.timeline.command.insert_marker") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto marker = required(command, "marker", data_path);
        if (!sequence || !marker)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        auto decoded = decode_marker(*marker.value(), context, data_path + "/marker");
        if (!decoded)
            return runtime::Err(decoded.error());
        return runtime::Ok(Command(InsertMarker{sequence.value(), std::move(decoded).value()}));
    }
    if (type.value() == "pulp.timeline.command.remove_marker") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto marker = decode_command_item_id(command, "marker_id", data_path);
        if (!sequence || !marker)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        return runtime::Ok(Command(RemoveMarker{sequence.value(), marker.value()}));
    }
    if (type.value() == "pulp.timeline.command.insert_region") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto region = required(command, "region", data_path);
        if (!sequence || !region)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        auto decoded = decode_region(*region.value(), MemberPolicy::Optional, context,
                                     data_path + "/region");
        if (!decoded)
            return runtime::Err(decoded.error());
        return runtime::Ok(Command(InsertRegion{sequence.value(), std::move(decoded).value()}));
    }
    if (type.value() == "pulp.timeline.command.remove_region") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto region = decode_command_item_id(command, "region_id", data_path);
        if (!sequence || !region)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        return runtime::Ok(Command(RemoveRegion{sequence.value(), region.value()}));
    }
    if (type.value() == "pulp.timeline.command.set_groove") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto expected = required(command, "expected", data_path);
        auto replacement = required(command, "replacement", data_path);
        if (!sequence || !expected || !replacement ||
            expected.value()->kind != JsonValue::Kind::Object ||
            replacement.value()->kind != JsonValue::Kind::Object)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        auto decoded_expected = decode_groove(expected.value(), context, data_path + "/expected");
        auto decoded_replacement =
            decode_groove(replacement.value(), context, data_path + "/replacement");
        if (!decoded_expected)
            return runtime::Err(decoded_expected.error());
        if (!decoded_replacement)
            return runtime::Err(decoded_replacement.error());
        return runtime::Ok(Command(SetGroove{sequence.value(), std::move(decoded_expected).value(),
                                             std::move(decoded_replacement).value()}));
    }
    if (type.value() == "pulp.timeline.command.insert_scene") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto before = decode_optional_command_item_id(command, "before_scene_id", data_path);
        auto scene = required(command, "scene", data_path);
        if (!sequence)
            return runtime::Err(sequence.error());
        if (!before)
            return runtime::Err(before.error());
        if (!scene)
            return runtime::Err(scene.error());
        auto decoded = decode_scene(*scene.value(), context, data_path + "/scene");
        if (!decoded)
            return runtime::Err(decoded.error());
        return runtime::Ok(
            Command(InsertScene{sequence.value(), std::move(decoded).value(), before.value()}));
    }
    if (type.value() == "pulp.timeline.command.remove_scene") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto scene = decode_command_item_id(command, "scene_id", data_path);
        if (!sequence || !scene)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        return runtime::Ok(Command(RemoveScene{sequence.value(), scene.value()}));
    }
    if (type.value() == "pulp.timeline.command.insert_slot") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto scene = decode_command_item_id(command, "scene_id", data_path);
        auto before = decode_optional_command_item_id(command, "before_slot_id", data_path);
        auto slot = required(command, "slot", data_path);
        if (!sequence)
            return runtime::Err(sequence.error());
        if (!scene)
            return runtime::Err(scene.error());
        if (!before)
            return runtime::Err(before.error());
        if (!slot)
            return runtime::Err(slot.error());
        auto decoded = decode_slot(*slot.value(), context, data_path + "/slot");
        if (!decoded)
            return runtime::Err(decoded.error());
        return runtime::Ok(Command(InsertSlot{sequence.value(), scene.value(),
                                              std::move(decoded).value(), before.value()}));
    }
    if (type.value() == "pulp.timeline.command.remove_slot") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto scene = decode_command_item_id(command, "scene_id", data_path);
        auto slot = decode_command_item_id(command, "slot_id", data_path);
        if (!sequence || !scene || !slot)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        return runtime::Ok(Command(RemoveSlot{sequence.value(), scene.value(), slot.value()}));
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
    if (type.value() == "pulp.timeline.command.insert_track") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto before = decode_optional_command_item_id(command, "before_track_id", data_path);
        auto track = required(command, "track", data_path);
        if (!sequence)
            return runtime::Err(sequence.error());
        if (!before)
            return runtime::Err(before.error());
        if (!track)
            return runtime::Err(track.error());
        auto decoded =
            decode_track(document, *track.value(), registry, context, data_path + "/track");
        if (!decoded)
            return runtime::Err(decoded.error());
        return runtime::Ok(
            Command(InsertTrack{sequence.value(), std::move(decoded).value(), before.value()}));
    }
    if (type.value() == "pulp.timeline.command.remove_track") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto track = decode_command_item_id(command, "track_id", data_path);
        if (!sequence || !track)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        return runtime::Ok(Command(RemoveTrack{sequence.value(), track.value()}));
    }
    if (type.value() == "pulp.timeline.command.set_track_name") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto track = decode_command_item_id(command, "track_id", data_path);
        if (!sequence || !track)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        auto expected = detail::decode_string_field(command, "expected", data_path);
        auto replacement = detail::decode_string_field(command, "replacement", data_path);
        if (!expected)
            return runtime::Err(expected.error());
        if (!replacement)
            return runtime::Err(replacement.error());
        return runtime::Ok(Command(SetTrackName{sequence.value(), track.value(),
                                                std::move(expected).value(),
                                                std::move(replacement).value()}));
    }
    if (type.value() == "pulp.timeline.command.move_track") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto track = decode_command_item_id(command, "track_id", data_path);
        if (!sequence || !track)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        // Both endpoints are absent when a track moves from last to last, so an
        // absent member is a position rather than a missing field.
        auto expected =
            decode_optional_command_item_id(command, "expected_before_track_id", data_path);
        auto replacement =
            decode_optional_command_item_id(command, "replacement_before_track_id", data_path);
        if (!expected)
            return runtime::Err(expected.error());
        if (!replacement)
            return runtime::Err(replacement.error());
        return runtime::Ok(Command(MoveTrack{sequence.value(), track.value(), expected.value(),
                                             replacement.value()}));
    }
    if (type.value() == "pulp.timeline.command.insert_sequence") {
        auto sequence = required(command, "sequence", data_path);
        if (!sequence)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        auto decoded = decode_sequence(document, *sequence.value(), registry, context,
                                       data_path + "/sequence");
        if (!decoded)
            return runtime::Err(decoded.error());
        return runtime::Ok(Command(InsertSequence{std::move(decoded).value()}));
    }
    if (type.value() == "pulp.timeline.command.clone_sequence") {
        auto source = decode_command_item_id(command, "source_sequence_id", data_path);
        auto cloned = decode_command_item_id(command, "cloned_sequence_id", data_path);
        auto remap = required(command, "id_remap", data_path);
        if (!source || !cloned || !remap)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        if (remap.value()->kind != JsonValue::Kind::Array)
            return fail<Command>(PersistenceErrorCode::UnexpectedType,
                                 data_path + "/id_remap");
        std::vector<std::pair<ItemId, ItemId>> entries;
        entries.reserve(remap.value()->array.size());
        for (std::size_t index = 0; index < remap.value()->array.size(); ++index) {
            const auto entry_path =
                data_path + "/id_remap/" + std::to_string(index);
            const auto& entry = remap.value()->array[index];
            auto old_id = decode_command_item_id(entry, "old_id", entry_path);
            auto new_id = decode_command_item_id(entry, "new_id", entry_path);
            if (!old_id || !new_id)
                return fail<Command>(PersistenceErrorCode::MissingField, entry_path);
            entries.emplace_back(old_id.value(), new_id.value());
        }
        return runtime::Ok(Command(
            CloneSequence{source.value(), cloned.value(), std::move(entries)}));
    }
    if (type.value() == "pulp.timeline.command.remove_sequence") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        if (!sequence)
            return runtime::Err(sequence.error());
        return runtime::Ok(Command(RemoveSequence{sequence.value()}));
    }
    if (type.value() == "pulp.timeline.command.set_clip_sequence_ref") {
        auto decoded = ids();
        auto expected = required(command, "expected", data_path);
        auto replacement = required(command, "replacement", data_path);
        if (!decoded || !expected || !replacement)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        auto decoded_expected =
            decode_sequence_ref(*expected.value(), data_path + "/expected");
        auto decoded_replacement =
            decode_sequence_ref(*replacement.value(), data_path + "/replacement");
        if (!decoded_expected || !decoded_replacement)
            return fail<Command>(PersistenceErrorCode::InvalidSchema, data_path);
        return runtime::Ok(Command(SetClipSequenceRef{
            decoded.value()[0], decoded.value()[1], decoded.value()[2],
            decoded_expected.value(), decoded_replacement.value()}));
    }
    if (type.value() == "pulp.timeline.command.set_track_mixer") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto track = decode_command_item_id(command, "track_id", data_path);
        const auto* expected_value = command.find("expected");
        const auto* replacement_value = command.find("replacement");
        // Absence means "the default mixer" on a track, but on this command it
        // would silently assert an expectation the author never wrote, so both
        // sides of the optimistic gate are required here.
        if (!sequence || !track || !expected_value || !replacement_value)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        auto expected = detail::decode_track_mixer(expected_value, data_path + "/expected");
        auto replacement =
            detail::decode_track_mixer(replacement_value, data_path + "/replacement");
        if (!expected || !replacement)
            return fail<Command>(PersistenceErrorCode::InvalidSchema, data_path);
        return runtime::Ok(Command(SetTrackMixer{sequence.value(), track.value(), expected.value(),
                                                 replacement.value()}));
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
