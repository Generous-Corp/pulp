#include "serialize_decode_context.hpp"

#include <bit>
#include <limits>

namespace pulp::timeline::detail {

runtime::Result<ItemKind, PersistenceError> decode_item_kind(std::string_view value,
                                                             std::string path) {
    if (value == "project")
        return runtime::Ok(ItemKind::Project);
    if (value == "asset")
        return runtime::Ok(ItemKind::Asset);
    if (value == "sequence")
        return runtime::Ok(ItemKind::Sequence);
    if (value == "track")
        return runtime::Ok(ItemKind::Track);
    if (value == "clip")
        return runtime::Ok(ItemKind::Clip);
    if (value == "note")
        return runtime::Ok(ItemKind::Note);
    if (value == "device_placement")
        return runtime::Ok(ItemKind::DevicePlacement);
    if (value == "automation_lane")
        return runtime::Ok(ItemKind::AutomationLane);
    if (value == "automation_point")
        return runtime::Ok(ItemKind::AutomationPoint);
    if (value == "take_lane")
        return runtime::Ok(ItemKind::TakeLane);
    if (value == "take")
        return runtime::Ok(ItemKind::Take);
    return fail<ItemKind>(PersistenceErrorCode::InvalidSchema, std::move(path));
}

runtime::Result<const JsonValue*, PersistenceError>
required(const JsonValue& object_value, std::string_view name, std::string path) {
    return detail::required_decode_member(object_value, name, std::move(path));
}

runtime::Result<std::string, PersistenceError>
string_field(const JsonValue& object_value, std::string_view name, std::string path) {
    return detail::decode_string_field(object_value, name, std::move(path));
}

runtime::Result<StructuralData, PersistenceError>
data_for_versions(const JsonValue& value, std::string_view expected_type,
                  std::uint32_t minimum_version, std::uint32_t maximum_version, std::string path) {
    auto type = string_field(value, "type_name", path);
    auto version = required(value, "version", path);
    auto data = required(value, "data", path);
    if (!type)
        return fail<StructuralData>(type.error().code, type.error().path, type.error().byte_offset);
    if (!version)
        return fail<StructuralData>(version.error().code, version.error().path,
                                    version.error().byte_offset);
    if (!data)
        return fail<StructuralData>(data.error().code, data.error().path, data.error().byte_offset);
    auto decoded_version = parse_u32_number(*version.value(), path + "/version");
    if (type.value() != expected_type)
        return fail<StructuralData>(PersistenceErrorCode::UnsupportedStructuralType,
                                    std::move(path), value.begin);
    if (!decoded_version || decoded_version.value() < minimum_version ||
        decoded_version.value() > maximum_version)
        return fail<StructuralData>(PersistenceErrorCode::UnsupportedSchemaVersion, std::move(path),
                                    value.begin);
    if (data.value()->kind != JsonValue::Kind::Object)
        return fail<StructuralData>(PersistenceErrorCode::UnexpectedType, path + "/data",
                                    data.value()->begin);
    return runtime::Ok(StructuralData{data.value(), decoded_version.value()});
}

runtime::Result<const JsonValue*, PersistenceError>
data_for(const JsonValue& value, std::string_view expected_type, std::string path) {
    auto decoded = data_for_versions(value, expected_type, 1, 1, std::move(path));
    if (!decoded)
        return fail<const JsonValue*>(decoded.error().code, decoded.error().path,
                                      decoded.error().byte_offset);
    return runtime::Ok(decoded.value().data);
}

runtime::Result<timebase::RationalRate, PersistenceError> decode_rate(const JsonValue& value,
                                                                      std::string path) {
    return detail::decode_rational_rate(value, std::move(path));
}

runtime::Result<timebase::TempoMap, PersistenceError> decode_tempo_map(const JsonValue& value,
                                                                       std::string path) {
    if (value.kind != JsonValue::Kind::Array)
        return fail<timebase::TempoMap>(PersistenceErrorCode::UnexpectedType, std::move(path));
    std::vector<timebase::TempoPoint> points;
    points.reserve(value.array.size());
    for (std::size_t index = 0; index < value.array.size(); ++index) {
        const auto item_path = path + "/" + std::to_string(index);
        auto bits = required(value.array[index], "bpm_bits", item_path);
        auto curve = string_field(value.array[index], "curve", item_path);
        auto tick = required(value.array[index], "tick", item_path);
        if (!bits || !curve || !tick)
            return fail<timebase::TempoMap>(PersistenceErrorCode::MissingField, item_path);
        auto decoded_bits = parse_canonical_u64_string(*bits.value(), item_path + "/bpm_bits");
        auto decoded_tick = parse_canonical_i64_string(*tick.value(), item_path + "/tick");
        if (!decoded_bits || !decoded_tick)
            return fail<timebase::TempoMap>(PersistenceErrorCode::InvalidNumber, item_path);
        timebase::TempoCurve decoded_curve;
        if (curve.value() == "constant")
            decoded_curve = timebase::TempoCurve::Constant;
        else if (curve.value() == "linear_in_ticks")
            decoded_curve = timebase::TempoCurve::LinearInTicks;
        else
            return fail<timebase::TempoMap>(PersistenceErrorCode::InvalidSchema,
                                            item_path + "/curve");
        points.push_back(
            {{decoded_tick.value()}, std::bit_cast<double>(decoded_bits.value()), decoded_curve});
    }
    auto created = timebase::TempoMap::create(points);
    if (!created)
        return fail<timebase::TempoMap>(PersistenceErrorCode::InvalidSchema, std::move(path));
    return runtime::Ok(std::move(created).value());
}

runtime::Result<timebase::MeterMap, PersistenceError> decode_meter_map(const JsonValue& value,
                                                                       std::string path) {
    if (value.kind != JsonValue::Kind::Array)
        return fail<timebase::MeterMap>(PersistenceErrorCode::UnexpectedType, std::move(path));
    std::vector<timebase::MeterPoint> points;
    points.reserve(value.array.size());
    for (std::size_t index = 0; index < value.array.size(); ++index) {
        const auto item_path = path + "/" + std::to_string(index);
        auto denominator = required(value.array[index], "denominator", item_path);
        auto numerator = required(value.array[index], "numerator", item_path);
        auto tick = required(value.array[index], "tick", item_path);
        if (!denominator || !numerator || !tick)
            return fail<timebase::MeterMap>(PersistenceErrorCode::MissingField, item_path);
        auto d = parse_u32_number(*denominator.value(), item_path + "/denominator");
        auto n = parse_u32_number(*numerator.value(), item_path + "/numerator");
        auto t = parse_canonical_i64_string(*tick.value(), item_path + "/tick");
        if (!d || !n || !t || d.value() > std::numeric_limits<std::int32_t>::max() ||
            n.value() > std::numeric_limits<std::int32_t>::max())
            return fail<timebase::MeterMap>(PersistenceErrorCode::InvalidNumber, item_path);
        points.push_back(
            {{t.value()},
             {static_cast<std::int32_t>(n.value()), static_cast<std::int32_t>(d.value())}});
    }
    auto created = timebase::MeterMap::create(points);
    if (!created)
        return fail<timebase::MeterMap>(PersistenceErrorCode::InvalidSchema, std::move(path));
    return runtime::Ok(std::move(created).value());
}

} // namespace pulp::timeline::detail
