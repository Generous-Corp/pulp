#include "serialize_sequence_decode.hpp"

#include "sequence_schema_policy.hpp"
#include "serialize_decode_support.hpp"

namespace pulp::timeline::detail {
namespace {

template <typename T>
runtime::Result<T, PersistenceError> fail(PersistenceErrorCode code, std::string path = {},
                                          std::size_t byte_offset = 0, std::uint64_t actual = 0,
                                          std::uint64_t limit = 0) {
    return runtime::Err(
        PersistenceError{code, byte_offset, actual, limit, std::move(path), std::nullopt});
}

template <typename T>
runtime::Result<T, PersistenceError> model_fail(ModelError error, std::string path) {
    return runtime::Err(
        PersistenceError{PersistenceErrorCode::ModelRejected, 0, 0, 0, std::move(path), error});
}

runtime::Result<SequencePoint, PersistenceError> decode_point(const JsonValue& value,
                                                              std::string path) {
    auto kind = string_field(value, "kind", path);
    if (!kind)
        return runtime::Err(kind.error());
    if (kind.value() == "musical") {
        auto position = required(value, "position_ticks", path);
        if (!position)
            return runtime::Err(position.error());
        auto decoded = parse_canonical_i64_string(*position.value(), path + "/position_ticks");
        if (!decoded)
            return runtime::Err(decoded.error());
        return runtime::Ok(SequencePoint{MusicalSequencePoint{{decoded.value()}}});
    }
    if (kind.value() != "absolute")
        return fail<SequencePoint>(PersistenceErrorCode::InvalidSchema, path + "/kind");
    auto position = required(value, "position_sample", path);
    auto rate = required(value, "sample_rate", path);
    if (!position)
        return runtime::Err(position.error());
    if (!rate)
        return runtime::Err(rate.error());
    auto decoded_position =
        parse_canonical_i64_string(*position.value(), path + "/position_sample");
    auto decoded_rate = decode_rate(*rate.value(), path + "/sample_rate");
    if (!decoded_position)
        return runtime::Err(decoded_position.error());
    if (!decoded_rate)
        return runtime::Err(decoded_rate.error());
    return runtime::Ok(
        SequencePoint{AbsoluteSequencePoint{{decoded_position.value()}, decoded_rate.value()}});
}

runtime::Result<SequenceRange, PersistenceError> decode_range(const JsonValue& value,
                                                              std::string path) {
    auto kind = string_field(value, "kind", path);
    if (!kind)
        return runtime::Err(kind.error());
    if (kind.value() == "musical") {
        auto start = required(value, "start_ticks", path);
        auto duration = required(value, "duration_ticks", path);
        if (!start)
            return runtime::Err(start.error());
        if (!duration)
            return runtime::Err(duration.error());
        auto decoded_start = parse_canonical_i64_string(*start.value(), path + "/start_ticks");
        auto decoded_duration =
            parse_canonical_i64_string(*duration.value(), path + "/duration_ticks");
        if (!decoded_start)
            return runtime::Err(decoded_start.error());
        if (!decoded_duration)
            return runtime::Err(decoded_duration.error());
        return runtime::Ok(SequenceRange{
            MusicalSequenceRange{{decoded_start.value()}, {decoded_duration.value()}}});
    }
    if (kind.value() != "absolute")
        return fail<SequenceRange>(PersistenceErrorCode::InvalidSchema, path + "/kind");
    auto start = required(value, "start_sample", path);
    auto count = required(value, "sample_count", path);
    auto rate = required(value, "sample_rate", path);
    if (!start)
        return runtime::Err(start.error());
    if (!count)
        return runtime::Err(count.error());
    if (!rate)
        return runtime::Err(rate.error());
    auto decoded_start = parse_canonical_i64_string(*start.value(), path + "/start_sample");
    auto decoded_count = parse_canonical_u64_string(*count.value(), path + "/sample_count");
    auto decoded_rate = decode_rate(*rate.value(), path + "/sample_rate");
    if (!decoded_start)
        return runtime::Err(decoded_start.error());
    if (!decoded_count)
        return runtime::Err(decoded_count.error());
    if (!decoded_rate)
        return runtime::Err(decoded_rate.error());
    return runtime::Ok(SequenceRange{AbsoluteSequenceRange{
        {decoded_start.value()}, decoded_count.value(), decoded_rate.value()}});
}

} // namespace

runtime::Result<Sequence, PersistenceError>
decode_sequence(const std::shared_ptr<const ParsedJson>& document, const JsonValue& value,
                const SchemaRegistry& registry, const DecodeLimits& limits, DecodeCounts& counts,
                std::string path, TrackDecodeFn decode_track) {
    if (++counts.sequences > limits.max_sequences)
        return fail<Sequence>(PersistenceErrorCode::LimitExceeded, path, value.begin,
                              counts.sequences, limits.max_sequences);
    auto envelope = data_for_versions(value, sequence_schema_policy.type_name,
                                      sequence_schema_policy.oldest_readable_version,
                                      sequence_schema_policy.current_version, path);
    if (!envelope)
        return runtime::Err(envelope.error());
    const auto& data = *envelope.value().data;
    auto id = required(data, "id", path + "/data");
    auto name = string_field(data, "name", path + "/data");
    auto tracks = required(data, "tracks", path + "/data");
    auto musical = required(data, "musical_duration", path + "/data");
    auto absolute = required(data, "absolute_duration", path + "/data");
    const auto* markers = data.find("markers");
    const auto* regions = data.find("regions");
    const bool has_annotations = sequence_has_annotations(envelope.value().version);
    if (!id)
        return runtime::Err(id.error());
    if (!name)
        return runtime::Err(name.error());
    if (!tracks)
        return runtime::Err(tracks.error());
    if (!musical)
        return runtime::Err(musical.error());
    if (!absolute)
        return runtime::Err(absolute.error());
    if (tracks.value()->kind != JsonValue::Kind::Array)
        return fail<Sequence>(PersistenceErrorCode::UnexpectedType, path + "/data/tracks",
                              tracks.value()->begin);
    if (has_annotations && !markers)
        return fail<Sequence>(PersistenceErrorCode::MissingField, path + "/data/markers",
                              data.begin);
    if (has_annotations && markers->kind != JsonValue::Kind::Array)
        return fail<Sequence>(PersistenceErrorCode::UnexpectedType, path + "/data/markers",
                              markers->begin);
    if (has_annotations && !regions)
        return fail<Sequence>(PersistenceErrorCode::MissingField, path + "/data/regions",
                              data.begin);
    if (has_annotations && regions->kind != JsonValue::Kind::Array)
        return fail<Sequence>(PersistenceErrorCode::UnexpectedType, path + "/data/regions",
                              regions->begin);
    if (!has_annotations && (markers || regions))
        return fail<Sequence>(PersistenceErrorCode::InvalidSchema, path + "/data", data.begin);
    auto decoded_id = parse_canonical_u64_string(*id.value(), path + "/data/id");
    if (!decoded_id)
        return runtime::Err(decoded_id.error());
    std::optional<timebase::TickDuration> decoded_musical;
    if (musical.value()->kind != JsonValue::Kind::Null) {
        auto decoded =
            parse_canonical_i64_string(*musical.value(), path + "/data/musical_duration");
        if (!decoded)
            return runtime::Err(decoded.error());
        decoded_musical = timebase::TickDuration{decoded.value()};
    }
    std::optional<AbsoluteTimelineDuration> decoded_absolute;
    if (absolute.value()->kind != JsonValue::Kind::Null) {
        auto count = required(*absolute.value(), "sample_count", path + "/data/absolute_duration");
        auto rate = required(*absolute.value(), "sample_rate", path + "/data/absolute_duration");
        if (!count)
            return runtime::Err(count.error());
        if (!rate)
            return runtime::Err(rate.error());
        auto decoded_count = parse_canonical_u64_string(
            *count.value(), path + "/data/absolute_duration/sample_count");
        auto decoded_rate =
            decode_rate(*rate.value(), path + "/data/absolute_duration/sample_rate");
        if (!decoded_count)
            return runtime::Err(decoded_count.error());
        if (!decoded_rate)
            return runtime::Err(decoded_rate.error());
        decoded_absolute = AbsoluteTimelineDuration{decoded_count.value(), decoded_rate.value()};
    }
    std::vector<SequenceMarker> decoded_markers;
    decoded_markers.reserve(markers ? markers->array.size() : 0);
    for (std::size_t index = 0; markers && index < markers->array.size(); ++index) {
        const auto item_path = path + "/data/markers/" + std::to_string(index);
        if (++counts.sequence_markers > limits.max_sequence_markers)
            return fail<Sequence>(PersistenceErrorCode::LimitExceeded, item_path,
                                  markers->array[index].begin, counts.sequence_markers,
                                  limits.max_sequence_markers);
        auto marker_data =
            data_for(markers->array[index], "pulp.timeline.sequence_marker", item_path);
        if (!marker_data)
            return runtime::Err(marker_data.error());
        auto marker_id = required(*marker_data.value(), "id", item_path + "/data");
        auto marker_name = string_field(*marker_data.value(), "name", item_path + "/data");
        auto marker_type = string_field(*marker_data.value(), "type", item_path + "/data");
        auto point = required(*marker_data.value(), "point", item_path + "/data");
        if (!marker_id)
            return runtime::Err(marker_id.error());
        if (!marker_name)
            return runtime::Err(marker_name.error());
        if (!marker_type)
            return runtime::Err(marker_type.error());
        if (!point)
            return runtime::Err(point.error());
        auto parsed_id = parse_canonical_u64_string(*marker_id.value(), item_path + "/data/id");
        auto parsed_type = MarkerTypeId::create(std::move(marker_type).value());
        auto parsed_point = decode_point(*point.value(), item_path + "/data/point");
        if (!parsed_id)
            return runtime::Err(parsed_id.error());
        if (!parsed_type)
            return fail<Sequence>(PersistenceErrorCode::InvalidSchema, item_path + "/data/type",
                                  marker_data.value()->find("type")->begin);
        if (!parsed_point)
            return runtime::Err(parsed_point.error());
        decoded_markers.push_back({{parsed_id.value()},
                                   std::move(parsed_type).value(),
                                   std::move(marker_name).value(),
                                   std::move(parsed_point).value()});
    }
    std::vector<SequenceRegion> decoded_regions;
    decoded_regions.reserve(regions ? regions->array.size() : 0);
    for (std::size_t index = 0; regions && index < regions->array.size(); ++index) {
        const auto item_path = path + "/data/regions/" + std::to_string(index);
        if (++counts.sequence_regions > limits.max_sequence_regions)
            return fail<Sequence>(PersistenceErrorCode::LimitExceeded, item_path,
                                  regions->array[index].begin, counts.sequence_regions,
                                  limits.max_sequence_regions);
        auto region_data =
            data_for(regions->array[index], "pulp.timeline.sequence_region", item_path);
        if (!region_data)
            return runtime::Err(region_data.error());
        auto region_id = required(*region_data.value(), "id", item_path + "/data");
        auto region_name = string_field(*region_data.value(), "name", item_path + "/data");
        auto range = required(*region_data.value(), "range", item_path + "/data");
        if (!region_id)
            return runtime::Err(region_id.error());
        if (!region_name)
            return runtime::Err(region_name.error());
        if (!range)
            return runtime::Err(range.error());
        auto parsed_id = parse_canonical_u64_string(*region_id.value(), item_path + "/data/id");
        auto parsed_range = decode_range(*range.value(), item_path + "/data/range");
        if (!parsed_id)
            return runtime::Err(parsed_id.error());
        if (!parsed_range)
            return runtime::Err(parsed_range.error());
        decoded_regions.push_back(
            {{parsed_id.value()}, std::move(region_name).value(), std::move(parsed_range).value()});
    }
    std::vector<Track> decoded_tracks;
    decoded_tracks.reserve(tracks.value()->array.size());
    for (std::size_t index = 0; index < tracks.value()->array.size(); ++index) {
        auto decoded = decode_track(document, tracks.value()->array[index], registry, limits,
                                    counts, path + "/data/tracks/" + std::to_string(index));
        if (!decoded)
            return runtime::Err(decoded.error());
        decoded_tracks.push_back(std::move(decoded).value());
    }
    auto created = Sequence::create(SequenceInput{{decoded_id.value()},
                                                  std::move(name).value(),
                                                  decoded_musical,
                                                  decoded_absolute,
                                                  std::move(decoded_tracks),
                                                  std::move(decoded_markers),
                                                  std::move(decoded_regions)});
    return created ? runtime::Result<Sequence, PersistenceError>(
                         runtime::Ok(std::move(created).value()))
                   : model_fail<Sequence>(created.error(), std::move(path));
}

} // namespace pulp::timeline::detail
