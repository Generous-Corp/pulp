#include "serialize_sequence_decode.hpp"

#include "sequence_schema_policy.hpp"

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
    return runtime::Err(PersistenceError{PersistenceErrorCode::ModelRejected, 0, 0, 0,
                                         std::move(path), error});
}

runtime::Result<const JsonValue*, PersistenceError>
required(const JsonValue& object, std::string_view name, std::string path) {
    if (object.kind != JsonValue::Kind::Object)
        return fail<const JsonValue*>(PersistenceErrorCode::UnexpectedType, std::move(path),
                                      object.begin);
    const auto* value = object.find(name);
    return value ? runtime::Result<const JsonValue*, PersistenceError>(runtime::Ok(value))
                 : fail<const JsonValue*>(PersistenceErrorCode::MissingField,
                                          path + "/" + std::string(name), object.begin);
}

runtime::Result<std::string, PersistenceError>
string_field(const JsonValue& object, std::string_view name, std::string path) {
    auto value = required(object, name, path);
    if (!value)
        return fail<std::string>(value.error().code, value.error().path,
                                 value.error().byte_offset);
    if (value.value()->kind != JsonValue::Kind::String)
        return fail<std::string>(PersistenceErrorCode::UnexpectedType,
                                 path + "/" + std::string(name), value.value()->begin);
    return runtime::Ok(value.value()->scalar);
}

struct StructuralData {
    const JsonValue* data = nullptr;
    std::uint32_t version = 0;
};

runtime::Result<StructuralData, PersistenceError>
data_for_versions(const JsonValue& value, std::string_view expected_type, std::uint32_t minimum,
                  std::uint32_t maximum, std::string path) {
    auto type = string_field(value, "type_name", path);
    auto version = required(value, "version", path);
    auto data = required(value, "data", path);
    if (!type || !version || !data)
        return fail<StructuralData>(PersistenceErrorCode::MissingField, std::move(path));
    auto decoded_version = parse_u32_number(*version.value(), path + "/version");
    if (type.value() != expected_type)
        return fail<StructuralData>(PersistenceErrorCode::UnsupportedStructuralType,
                                    std::move(path), value.begin);
    if (!decoded_version || decoded_version.value() < minimum || decoded_version.value() > maximum)
        return fail<StructuralData>(PersistenceErrorCode::UnsupportedSchemaVersion,
                                    std::move(path), value.begin);
    if (data.value()->kind != JsonValue::Kind::Object)
        return fail<StructuralData>(PersistenceErrorCode::UnexpectedType, path + "/data",
                                    data.value()->begin);
    return runtime::Ok(StructuralData{data.value(), decoded_version.value()});
}

runtime::Result<const JsonValue*, PersistenceError>
data_for(const JsonValue& value, std::string_view type, std::string path) {
    auto envelope = data_for_versions(value, type, 1, 1, std::move(path));
    return envelope ? runtime::Result<const JsonValue*, PersistenceError>(
                          runtime::Ok(envelope.value().data))
                    : fail<const JsonValue*>(envelope.error().code, envelope.error().path,
                                             envelope.error().byte_offset);
}

runtime::Result<timebase::RationalRate, PersistenceError>
decode_rate(const JsonValue& value, std::string path) {
    auto numerator = required(value, "numerator", path);
    auto denominator = required(value, "denominator", path);
    if (!numerator || !denominator)
        return fail<timebase::RationalRate>(PersistenceErrorCode::MissingField, std::move(path));
    auto n = parse_canonical_u64_string(*numerator.value(), path + "/numerator");
    auto d = parse_canonical_u64_string(*denominator.value(), path + "/denominator");
    if (!n || !d)
        return fail<timebase::RationalRate>(PersistenceErrorCode::InvalidNumber, std::move(path));
    const timebase::RationalRate rate{n.value(), d.value()};
    if (!rate.valid() || rate.normalized() != rate)
        return fail<timebase::RationalRate>(PersistenceErrorCode::InvalidNumber, std::move(path));
    return runtime::Ok(rate);
}

runtime::Result<SequencePoint, PersistenceError>
decode_point(const JsonValue& value, std::string path) {
    auto kind = string_field(value, "kind", path);
    if (!kind)
        return fail<SequencePoint>(kind.error().code, kind.error().path);
    if (kind.value() == "musical") {
        auto position = required(value, "position_ticks", path);
        if (!position)
            return fail<SequencePoint>(position.error().code, position.error().path);
        auto decoded = parse_canonical_i64_string(*position.value(), path + "/position_ticks");
        return decoded ? runtime::Result<SequencePoint, PersistenceError>(
                             runtime::Ok(SequencePoint{MusicalSequencePoint{{decoded.value()}}}))
                       : fail<SequencePoint>(decoded.error().code, decoded.error().path);
    }
    if (kind.value() != "absolute")
        return fail<SequencePoint>(PersistenceErrorCode::InvalidSchema, path + "/kind");
    auto position = required(value, "position_sample", path);
    auto rate = required(value, "sample_rate", path);
    if (!position || !rate)
        return fail<SequencePoint>(PersistenceErrorCode::MissingField, std::move(path));
    auto decoded_position = parse_canonical_i64_string(*position.value(), path + "/position_sample");
    auto decoded_rate = decode_rate(*rate.value(), path + "/sample_rate");
    if (!decoded_position || !decoded_rate)
        return fail<SequencePoint>(PersistenceErrorCode::InvalidNumber, std::move(path));
    return runtime::Ok(SequencePoint{
        AbsoluteSequencePoint{{decoded_position.value()}, decoded_rate.value()}});
}

runtime::Result<SequenceRange, PersistenceError>
decode_range(const JsonValue& value, std::string path) {
    auto kind = string_field(value, "kind", path);
    if (!kind)
        return fail<SequenceRange>(kind.error().code, kind.error().path);
    if (kind.value() == "musical") {
        auto start = required(value, "start_ticks", path);
        auto duration = required(value, "duration_ticks", path);
        if (!start || !duration)
            return fail<SequenceRange>(PersistenceErrorCode::MissingField, std::move(path));
        auto decoded_start = parse_canonical_i64_string(*start.value(), path + "/start_ticks");
        auto decoded_duration =
            parse_canonical_i64_string(*duration.value(), path + "/duration_ticks");
        if (!decoded_start || !decoded_duration)
            return fail<SequenceRange>(PersistenceErrorCode::InvalidNumber, std::move(path));
        return runtime::Ok(SequenceRange{MusicalSequenceRange{{decoded_start.value()},
                                                               {decoded_duration.value()}}});
    }
    if (kind.value() != "absolute")
        return fail<SequenceRange>(PersistenceErrorCode::InvalidSchema, path + "/kind");
    auto start = required(value, "start_sample", path);
    auto count = required(value, "sample_count", path);
    auto rate = required(value, "sample_rate", path);
    if (!start || !count || !rate)
        return fail<SequenceRange>(PersistenceErrorCode::MissingField, std::move(path));
    auto decoded_start = parse_canonical_i64_string(*start.value(), path + "/start_sample");
    auto decoded_count = parse_canonical_u64_string(*count.value(), path + "/sample_count");
    auto decoded_rate = decode_rate(*rate.value(), path + "/sample_rate");
    if (!decoded_start || !decoded_count || !decoded_rate)
        return fail<SequenceRange>(PersistenceErrorCode::InvalidNumber, std::move(path));
    return runtime::Ok(SequenceRange{AbsoluteSequenceRange{{decoded_start.value()},
                                                            decoded_count.value(),
                                                            decoded_rate.value()}});
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
        return fail<Sequence>(envelope.error().code, envelope.error().path,
                              envelope.error().byte_offset);
    const auto& data = *envelope.value().data;
    auto id = required(data, "id", path + "/data");
    auto name = string_field(data, "name", path + "/data");
    auto tracks = required(data, "tracks", path + "/data");
    auto musical = required(data, "musical_duration", path + "/data");
    auto absolute = required(data, "absolute_duration", path + "/data");
    const auto* markers = data.find("markers");
    const auto* regions = data.find("regions");
    const bool has_annotations = sequence_has_annotations(envelope.value().version);
    if (!id || !name || !tracks || !musical || !absolute ||
        tracks.value()->kind != JsonValue::Kind::Array ||
        (has_annotations && (!markers || markers->kind != JsonValue::Kind::Array || !regions ||
                             regions->kind != JsonValue::Kind::Array)) ||
        (!has_annotations && (markers || regions)))
        return fail<Sequence>(PersistenceErrorCode::MissingField, std::move(path));
    auto decoded_id = parse_canonical_u64_string(*id.value(), path + "/data/id");
    if (!decoded_id)
        return fail<Sequence>(decoded_id.error().code, decoded_id.error().path);
    std::optional<timebase::TickDuration> decoded_musical;
    if (musical.value()->kind != JsonValue::Kind::Null) {
        auto decoded = parse_canonical_i64_string(*musical.value(), path + "/data/musical_duration");
        if (!decoded)
            return fail<Sequence>(decoded.error().code, decoded.error().path);
        decoded_musical = timebase::TickDuration{decoded.value()};
    }
    std::optional<AbsoluteTimelineDuration> decoded_absolute;
    if (absolute.value()->kind != JsonValue::Kind::Null) {
        auto count = required(*absolute.value(), "sample_count", path + "/data/absolute_duration");
        auto rate = required(*absolute.value(), "sample_rate", path + "/data/absolute_duration");
        if (!count || !rate)
            return fail<Sequence>(PersistenceErrorCode::MissingField, path);
        auto decoded_count = parse_canonical_u64_string(
            *count.value(), path + "/data/absolute_duration/sample_count");
        auto decoded_rate = decode_rate(*rate.value(), path + "/data/absolute_duration/sample_rate");
        if (!decoded_count || !decoded_rate)
            return fail<Sequence>(PersistenceErrorCode::InvalidNumber, path);
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
        auto marker_data = data_for(markers->array[index], "pulp.timeline.sequence_marker", item_path);
        if (!marker_data)
            return fail<Sequence>(marker_data.error().code, marker_data.error().path);
        auto marker_id = required(*marker_data.value(), "id", item_path + "/data");
        auto marker_name = string_field(*marker_data.value(), "name", item_path + "/data");
        auto marker_type = string_field(*marker_data.value(), "type", item_path + "/data");
        auto point = required(*marker_data.value(), "point", item_path + "/data");
        if (!marker_id || !marker_name || !marker_type || !point)
            return fail<Sequence>(PersistenceErrorCode::MissingField, item_path);
        auto parsed_id = parse_canonical_u64_string(*marker_id.value(), item_path + "/data/id");
        auto parsed_type = MarkerTypeId::create(std::move(marker_type).value());
        auto parsed_point = decode_point(*point.value(), item_path + "/data/point");
        if (!parsed_id || !parsed_type || !parsed_point)
            return fail<Sequence>(PersistenceErrorCode::InvalidSchema, item_path);
        decoded_markers.push_back({{parsed_id.value()}, std::move(parsed_type).value(),
                                   std::move(marker_name).value(), std::move(parsed_point).value()});
    }
    std::vector<SequenceRegion> decoded_regions;
    decoded_regions.reserve(regions ? regions->array.size() : 0);
    for (std::size_t index = 0; regions && index < regions->array.size(); ++index) {
        const auto item_path = path + "/data/regions/" + std::to_string(index);
        if (++counts.sequence_regions > limits.max_sequence_regions)
            return fail<Sequence>(PersistenceErrorCode::LimitExceeded, item_path,
                                  regions->array[index].begin, counts.sequence_regions,
                                  limits.max_sequence_regions);
        auto region_data = data_for(regions->array[index], "pulp.timeline.sequence_region", item_path);
        if (!region_data)
            return fail<Sequence>(region_data.error().code, region_data.error().path);
        auto region_id = required(*region_data.value(), "id", item_path + "/data");
        auto region_name = string_field(*region_data.value(), "name", item_path + "/data");
        auto range = required(*region_data.value(), "range", item_path + "/data");
        if (!region_id || !region_name || !range)
            return fail<Sequence>(PersistenceErrorCode::MissingField, item_path);
        auto parsed_id = parse_canonical_u64_string(*region_id.value(), item_path + "/data/id");
        auto parsed_range = decode_range(*range.value(), item_path + "/data/range");
        if (!parsed_id || !parsed_range)
            return fail<Sequence>(PersistenceErrorCode::InvalidSchema, item_path);
        decoded_regions.push_back({{parsed_id.value()}, std::move(region_name).value(),
                                   std::move(parsed_range).value()});
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
    auto created = Sequence::create(SequenceInput{{decoded_id.value()}, std::move(name).value(),
                                                  decoded_musical, decoded_absolute,
                                                  std::move(decoded_tracks),
                                                  std::move(decoded_markers),
                                                  std::move(decoded_regions)});
    return created ? runtime::Result<Sequence, PersistenceError>(runtime::Ok(std::move(created).value()))
                   : model_fail<Sequence>(created.error(), std::move(path));
}

} // namespace pulp::timeline::detail
