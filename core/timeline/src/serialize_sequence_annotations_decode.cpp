#include "serialize_decode_context.hpp"

#include "bounded_increment.hpp"

#include <algorithm>
#include <limits>
#include <optional>

namespace pulp::timeline::detail {
namespace {

runtime::Result<std::optional<std::uint32_t>, PersistenceError>
decode_annotation_color(const JsonValue& data, const std::string& path) {
    const auto* color = data.find("color");
    if (!color)
        return runtime::Ok(std::optional<std::uint32_t>{});
    auto decoded = parse_u32_number(*color, path + "/color");
    if (!decoded)
        return fail<std::optional<std::uint32_t>>(decoded.error().code, decoded.error().path,
                                                  decoded.error().byte_offset);
    return runtime::Ok(std::optional<std::uint32_t>{decoded.value()});
}

runtime::Result<FollowActionKind, PersistenceError>
decode_follow_action_kind(std::string_view value, std::string path) {
    if (value == "none")
        return runtime::Ok(FollowActionKind::None);
    if (value == "stop")
        return runtime::Ok(FollowActionKind::Stop);
    if (value == "again")
        return runtime::Ok(FollowActionKind::Again);
    if (value == "previous")
        return runtime::Ok(FollowActionKind::Previous);
    if (value == "next")
        return runtime::Ok(FollowActionKind::Next);
    if (value == "first")
        return runtime::Ok(FollowActionKind::First);
    if (value == "last")
        return runtime::Ok(FollowActionKind::Last);
    if (value == "any")
        return runtime::Ok(FollowActionKind::Any);
    if (value == "other")
        return runtime::Ok(FollowActionKind::Other);
    if (value == "jump")
        return runtime::Ok(FollowActionKind::Jump);
    return fail<FollowActionKind>(PersistenceErrorCode::InvalidSchema, std::move(path));
}

} // namespace

runtime::Result<SequenceMarker, PersistenceError>
decode_marker(const JsonValue& value, DecodeContext& context, std::string path) {
    const auto increment = bounded_increment(context.counts.markers, context.limits.max_markers);
    if (!increment)
        return fail<SequenceMarker>(PersistenceErrorCode::LimitExceeded, path, value.begin,
                                    increment.actual, context.limits.max_markers);
    auto data = data_for(value, "pulp.timeline.marker", path);
    if (!data)
        return fail<SequenceMarker>(data.error().code, data.error().path, data.error().byte_offset);
    auto id = required(*data.value(), "id", path + "/data");
    auto name = string_field(*data.value(), "name", path + "/data");
    auto position = required(*data.value(), "position", path + "/data");
    if (!id || !name || !position)
        return fail<SequenceMarker>(PersistenceErrorCode::MissingField, std::move(path));
    auto decoded_id = parse_canonical_u64_string(*id.value(), path + "/data/id");
    auto decoded_position = parse_canonical_i64_string(*position.value(), path + "/data/position");
    if (!decoded_id || !decoded_position)
        return fail<SequenceMarker>(PersistenceErrorCode::InvalidNumber, std::move(path));
    auto decoded_color = decode_annotation_color(*data.value(), path + "/data");
    if (!decoded_color)
        return runtime::Err(decoded_color.error());
    return runtime::Ok(SequenceMarker{ItemId{decoded_id.value()}, std::move(name).value(),
                                      timebase::TickPosition{decoded_position.value()},
                                      decoded_color.value()});
}

runtime::Result<SequenceRegion, PersistenceError>
decode_region(const JsonValue& value, DecodeContext& context, std::string path) {
    const auto increment = bounded_increment(context.counts.regions, context.limits.max_regions);
    if (!increment)
        return fail<SequenceRegion>(PersistenceErrorCode::LimitExceeded, path, value.begin,
                                    increment.actual, context.limits.max_regions);
    auto data = data_for(value, "pulp.timeline.region", path);
    if (!data)
        return fail<SequenceRegion>(data.error().code, data.error().path, data.error().byte_offset);
    auto id = required(*data.value(), "id", path + "/data");
    auto name = string_field(*data.value(), "name", path + "/data");
    auto position = required(*data.value(), "position", path + "/data");
    auto duration = required(*data.value(), "duration", path + "/data");
    if (!id || !name || !position || !duration)
        return fail<SequenceRegion>(PersistenceErrorCode::MissingField, std::move(path));
    auto decoded_id = parse_canonical_u64_string(*id.value(), path + "/data/id");
    auto decoded_position = parse_canonical_i64_string(*position.value(), path + "/data/position");
    auto decoded_duration = parse_canonical_i64_string(*duration.value(), path + "/data/duration");
    if (!decoded_id || !decoded_position || !decoded_duration)
        return fail<SequenceRegion>(PersistenceErrorCode::InvalidNumber, std::move(path));
    auto decoded_color = decode_annotation_color(*data.value(), path + "/data");
    if (!decoded_color)
        return runtime::Err(decoded_color.error());
    return runtime::Ok(SequenceRegion{ItemId{decoded_id.value()}, std::move(name).value(),
                                      timebase::TickPosition{decoded_position.value()},
                                      timebase::TickDuration{decoded_duration.value()},
                                      decoded_color.value()});
}

runtime::Result<Slot, PersistenceError> decode_slot(const JsonValue& value, DecodeContext& context,
                                                    std::string path) {
    const auto increment = bounded_increment(context.counts.slots, context.limits.max_slots);
    if (!increment)
        return fail<Slot>(PersistenceErrorCode::LimitExceeded, path, value.begin, increment.actual,
                          context.limits.max_slots);
    auto data = data_for(value, "pulp.timeline.slot", path);
    if (!data)
        return fail<Slot>(data.error().code, data.error().path, data.error().byte_offset);
    auto id = required(*data.value(), "id", path + "/data");
    auto clip = required(*data.value(), "clip_id", path + "/data");
    auto follow = required(*data.value(), "follow", path + "/data");
    auto launch = required(*data.value(), "launch_quantize", path + "/data");
    if (!id || !clip || !follow || !launch || follow.value()->kind != JsonValue::Kind::Object ||
        launch.value()->kind != JsonValue::Kind::Object)
        return fail<Slot>(PersistenceErrorCode::MissingField, std::move(path));
    auto decoded_id = parse_canonical_u64_string(*id.value(), path + "/data/id");
    auto decoded_clip = parse_canonical_u64_string(*clip.value(), path + "/data/clip_id");
    auto choices = required(*follow.value(), "choices", path + "/data/follow");
    auto grid = required(*follow.value(), "grid", path + "/data/follow");
    auto repetitions = required(*follow.value(), "repetitions", path + "/data/follow");
    auto launch_grid = required(*launch.value(), "grid", path + "/data/launch_quantize");
    auto phase = required(*launch.value(), "phase", path + "/data/launch_quantize");
    if (!decoded_id)
        return runtime::Err(decoded_id.error());
    if (!decoded_clip)
        return runtime::Err(decoded_clip.error());
    if (!choices)
        return runtime::Err(choices.error());
    if (!grid)
        return runtime::Err(grid.error());
    if (!repetitions)
        return runtime::Err(repetitions.error());
    if (!launch_grid)
        return runtime::Err(launch_grid.error());
    if (!phase)
        return runtime::Err(phase.error());
    if (choices.value()->kind != JsonValue::Kind::Array ||
        choices.value()->array.size() > FollowActionSet::kMaxChoices)
        return fail<Slot>(PersistenceErrorCode::InvalidSchema, path + "/data/follow/choices");
    auto decoded_grid = parse_canonical_i64_string(*grid.value(), path + "/data/follow/grid");
    auto decoded_repetitions =
        parse_u32_number(*repetitions.value(), path + "/data/follow/repetitions");
    auto decoded_launch_grid =
        parse_canonical_i64_string(*launch_grid.value(), path + "/data/launch_quantize/grid");
    auto decoded_phase =
        parse_canonical_i64_string(*phase.value(), path + "/data/launch_quantize/phase");
    if (!decoded_grid)
        return runtime::Err(decoded_grid.error());
    if (!decoded_repetitions)
        return runtime::Err(decoded_repetitions.error());
    if (!decoded_launch_grid)
        return runtime::Err(decoded_launch_grid.error());
    if (!decoded_phase)
        return runtime::Err(decoded_phase.error());
    FollowActionSet decoded_follow;
    decoded_follow.choice_count = static_cast<std::uint8_t>(choices.value()->array.size());
    decoded_follow.grid = timebase::TickDuration{decoded_grid.value()};
    decoded_follow.repetitions = static_cast<std::uint32_t>(decoded_repetitions.value());
    for (std::size_t index = 0; index < choices.value()->array.size(); ++index) {
        const auto choice_path = path + "/data/follow/choices/" + std::to_string(index);
        const auto& choice = choices.value()->array[index];
        auto kind = string_field(choice, "kind", choice_path);
        auto target = required(choice, "target", choice_path);
        auto weight = required(choice, "weight", choice_path);
        if (!kind || !target || !weight)
            return fail<Slot>(PersistenceErrorCode::MissingField, choice_path);
        auto decoded_kind = decode_follow_action_kind(kind.value(), choice_path + "/kind");
        auto decoded_target = parse_canonical_u64_string(*target.value(), choice_path + "/target");
        auto decoded_weight = parse_u32_number(*weight.value(), choice_path + "/weight");
        if (!decoded_kind)
            return runtime::Err(decoded_kind.error());
        if (!decoded_target)
            return runtime::Err(decoded_target.error());
        if (!decoded_weight)
            return runtime::Err(decoded_weight.error());
        if (decoded_weight.value() > std::numeric_limits<std::uint16_t>::max())
            return fail<Slot>(PersistenceErrorCode::InvalidNumber, choice_path + "/weight");
        decoded_follow.choices[index] =
            FollowAction{decoded_kind.value(), ItemId{decoded_target.value()},
                         static_cast<std::uint16_t>(decoded_weight.value())};
    }
    return runtime::Ok(Slot{ItemId{decoded_id.value()}, ItemId{decoded_clip.value()},
                            LaunchQuantize{timebase::TickDuration{decoded_launch_grid.value()},
                                           timebase::TickPosition{decoded_phase.value()}},
                            decoded_follow});
}

runtime::Result<Scene, PersistenceError> decode_scene(const JsonValue& value,
                                                      DecodeContext& context, std::string path) {
    const auto increment = bounded_increment(context.counts.scenes, context.limits.max_scenes);
    if (!increment)
        return fail<Scene>(PersistenceErrorCode::LimitExceeded, path, value.begin, increment.actual,
                           context.limits.max_scenes);
    auto data = data_for(value, "pulp.timeline.scene", path);
    if (!data)
        return fail<Scene>(data.error().code, data.error().path, data.error().byte_offset);
    auto id = required(*data.value(), "id", path + "/data");
    auto name = string_field(*data.value(), "name", path + "/data");
    auto slots = required(*data.value(), "slots", path + "/data");
    if (!id || !name || !slots || slots.value()->kind != JsonValue::Kind::Array)
        return fail<Scene>(PersistenceErrorCode::MissingField, std::move(path));
    auto decoded_id = parse_canonical_u64_string(*id.value(), path + "/data/id");
    if (!decoded_id)
        return fail<Scene>(decoded_id.error().code, decoded_id.error().path,
                           decoded_id.error().byte_offset);
    const auto slot_count = slots.value()->array.size();
    const auto remaining_slots =
        context.limits.max_slots - std::min(context.counts.slots, context.limits.max_slots);
    if (slot_count > remaining_slots)
        return fail<Scene>(PersistenceErrorCode::LimitExceeded, path + "/data/slots",
                           slots.value()->begin, context.counts.slots + slot_count,
                           context.limits.max_slots);
    std::vector<Slot> decoded_slots;
    decoded_slots.reserve(slot_count);
    for (std::size_t index = 0; index < slot_count; ++index) {
        auto decoded = decode_slot(slots.value()->array[index], context,
                                   path + "/data/slots/" + std::to_string(index));
        if (!decoded)
            return runtime::Err(decoded.error());
        decoded_slots.push_back(std::move(decoded).value());
    }
    return runtime::Ok(
        Scene{ItemId{decoded_id.value()}, std::move(name).value(), std::move(decoded_slots)});
}

} // namespace pulp::timeline::detail
