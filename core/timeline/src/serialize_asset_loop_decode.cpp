#include "serialize_asset_loop_decode.hpp"

#include <limits>

namespace pulp::timeline::detail {
namespace {

template <typename T>
runtime::Result<T, PersistenceError> fail(PersistenceErrorCode code, std::string path,
                                          std::size_t offset = 0, std::uint64_t actual = 0,
                                          std::uint64_t limit = 0) {
    return runtime::Err(PersistenceError{code, offset, actual, limit, std::move(path)});
}

runtime::Result<const JsonValue*, PersistenceError>
required(const JsonValue& value, std::string_view name, const std::string& path) {
    if (value.kind != JsonValue::Kind::Object)
        return fail<const JsonValue*>(PersistenceErrorCode::UnexpectedType, path, value.begin);
    const auto* member = value.find(name);
    if (!member)
        return fail<const JsonValue*>(PersistenceErrorCode::MissingField,
                                      path + "/" + std::string(name), value.begin);
    return runtime::Ok(member);
}

runtime::Result<std::string, PersistenceError>
string_field(const JsonValue& value, std::string_view name, const std::string& path) {
    auto member = required(value, name, path);
    if (!member)
        return runtime::Err(member.error());
    if (member.value()->kind != JsonValue::Kind::String)
        return fail<std::string>(PersistenceErrorCode::UnexpectedType,
                                 path + "/" + std::string(name), member.value()->begin);
    return runtime::Ok(member.value()->scalar);
}

} // namespace

runtime::Result<AudioLoopInfo, PersistenceError>
decode_audio_loop_info(const JsonValue& value, const DecodeLimits& limits,
                       std::size_t& point_count, std::size_t& tag_count, std::string path) {
    if (value.kind != JsonValue::Kind::Object)
        return fail<AudioLoopInfo>(PersistenceErrorCode::UnexpectedType, std::move(path),
                                   value.begin);
    auto denominator = required(value, "meter_denominator", path);
    auto numerator = required(value, "meter_numerator", path);
    auto one_shot = required(value, "one_shot", path);
    auto points = required(value, "points", path);
    auto tags = required(value, "tags", path);
    if (!denominator || !numerator || !one_shot || !points || !tags)
        return fail<AudioLoopInfo>(PersistenceErrorCode::MissingField, std::move(path));
    if (one_shot.value()->kind != JsonValue::Kind::Boolean ||
        points.value()->kind != JsonValue::Kind::Array ||
        tags.value()->kind != JsonValue::Kind::Array)
        return fail<AudioLoopInfo>(PersistenceErrorCode::UnexpectedType, std::move(path));
    if (point_count > limits.max_audio_loop_points ||
        points.value()->array.size() > limits.max_audio_loop_points - point_count)
        return fail<AudioLoopInfo>(PersistenceErrorCode::LimitExceeded, path + "/points",
                                   points.value()->begin,
                                   point_count + points.value()->array.size(),
                                   limits.max_audio_loop_points);
    if (tag_count > limits.max_audio_loop_tags ||
        tags.value()->array.size() > limits.max_audio_loop_tags - tag_count)
        return fail<AudioLoopInfo>(PersistenceErrorCode::LimitExceeded, path + "/tags",
                                   tags.value()->begin, tag_count + tags.value()->array.size(),
                                   limits.max_audio_loop_tags);
    const auto* in_marker = value.find("in_marker_frame");
    const auto* out_marker = value.find("out_marker_frame");
    const auto* musical_length = value.find("musical_length_ticks");
    const auto* root_note = value.find("root_note");
    if ((in_marker == nullptr) != (out_marker == nullptr) ||
        value.object.size() !=
            5 + (in_marker ? 2 : 0) + (musical_length ? 1 : 0) + (root_note ? 1 : 0))
        return fail<AudioLoopInfo>(PersistenceErrorCode::InvalidSchema, std::move(path));
    auto decoded_denominator =
        parse_u32_number(*denominator.value(), path + "/meter_denominator");
    auto decoded_numerator = parse_u32_number(*numerator.value(), path + "/meter_numerator");
    if (!decoded_denominator || !decoded_numerator ||
        decoded_denominator.value() >
            static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
        decoded_numerator.value() >
            static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))
        return fail<AudioLoopInfo>(PersistenceErrorCode::InvalidNumber, std::move(path));

    AudioLoopInfo decoded;
    decoded.meter = {static_cast<std::int32_t>(decoded_numerator.value()),
                     static_cast<std::int32_t>(decoded_denominator.value())};
    decoded.one_shot = one_shot.value()->boolean;
    if (musical_length) {
        auto length = parse_canonical_i64_string(*musical_length, path + "/musical_length_ticks");
        if (!length)
            return runtime::Err(length.error());
        decoded.musical_length = timebase::TickDuration{length.value()};
    }
    if (root_note) {
        auto note = parse_u32_number(*root_note, path + "/root_note");
        if (!note || note.value() > std::numeric_limits<std::uint8_t>::max())
            return fail<AudioLoopInfo>(PersistenceErrorCode::InvalidNumber, path + "/root_note");
        decoded.root_note = static_cast<std::uint8_t>(note.value());
    }
    if (in_marker) {
        auto start = parse_canonical_u64_string(*in_marker, path + "/in_marker_frame");
        auto end = parse_canonical_u64_string(*out_marker, path + "/out_marker_frame");
        if (!start || !end)
            return fail<AudioLoopInfo>(PersistenceErrorCode::InvalidNumber, std::move(path));
        decoded.active_range = AudioFrameRange{start.value(), end.value()};
    }
    decoded.points.reserve(points.value()->array.size());
    for (std::size_t index = 0; index < points.value()->array.size(); ++index) {
        const auto& point = points.value()->array[index];
        const auto point_path = path + "/points/" + std::to_string(index);
        if (point.kind != JsonValue::Kind::Object || point.object.size() != 2)
            return fail<AudioLoopInfo>(PersistenceErrorCode::InvalidSchema, point_path);
        auto frame = required(point, "frame", point_path);
        auto kind = string_field(point, "kind", point_path);
        if (!frame || !kind)
            return fail<AudioLoopInfo>(PersistenceErrorCode::MissingField, point_path);
        auto decoded_frame = parse_canonical_u64_string(*frame.value(), point_path + "/frame");
        if (!decoded_frame)
            return runtime::Err(decoded_frame.error());
        const auto decoded_kind =
            kind.value() == "manual"      ? std::optional{AudioLoopPointKind::Manual}
            : kind.value() == "automatic" ? std::optional{AudioLoopPointKind::Automatic}
                                          : std::nullopt;
        if (!decoded_kind)
            return fail<AudioLoopInfo>(PersistenceErrorCode::InvalidSchema, point_path + "/kind");
        decoded.points.push_back({decoded_frame.value(), *decoded_kind});
    }
    point_count += decoded.points.size();
    decoded.tags.reserve(tags.value()->array.size());
    for (std::size_t index = 0; index < tags.value()->array.size(); ++index) {
        const auto& tag = tags.value()->array[index];
        if (tag.kind != JsonValue::Kind::String)
            return fail<AudioLoopInfo>(PersistenceErrorCode::UnexpectedType,
                                       path + "/tags/" + std::to_string(index), tag.begin);
        decoded.tags.push_back(tag.scalar);
    }
    tag_count += decoded.tags.size();
    return runtime::Ok(std::move(decoded));
}

} // namespace pulp::timeline::detail
