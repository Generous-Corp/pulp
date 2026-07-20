#include "serialize_decode_support.hpp"

namespace pulp::timeline::detail {
namespace {

template <typename T>
runtime::Result<T, PersistenceError> fail(PersistenceErrorCode code, std::string path = {},
                                          std::size_t byte_offset = 0) {
    return runtime::Err(PersistenceError{code, byte_offset, 0, 0, std::move(path)});
}

} // namespace

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
        return fail<std::string>(value.error().code, value.error().path, value.error().byte_offset);
    if (value.value()->kind != JsonValue::Kind::String)
        return fail<std::string>(PersistenceErrorCode::UnexpectedType,
                                 path + "/" + std::string(name), value.value()->begin);
    return runtime::Ok(value.value()->scalar);
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
    auto envelope = data_for_versions(value, expected_type, 1, 1, std::move(path));
    if (!envelope)
        return fail<const JsonValue*>(envelope.error().code, envelope.error().path,
                                      envelope.error().byte_offset);
    return runtime::Ok(envelope.value().data);
}

runtime::Result<timebase::RationalRate, PersistenceError> decode_rate(const JsonValue& value,
                                                                      std::string path) {
    auto numerator = required(value, "numerator", path);
    auto denominator = required(value, "denominator", path);
    if (!numerator)
        return fail<timebase::RationalRate>(numerator.error().code, numerator.error().path,
                                            numerator.error().byte_offset);
    if (!denominator)
        return fail<timebase::RationalRate>(denominator.error().code, denominator.error().path,
                                            denominator.error().byte_offset);
    auto n = parse_canonical_u64_string(*numerator.value(), path + "/numerator");
    auto d = parse_canonical_u64_string(*denominator.value(), path + "/denominator");
    if (!n)
        return fail<timebase::RationalRate>(n.error().code, n.error().path, n.error().byte_offset);
    if (!d)
        return fail<timebase::RationalRate>(d.error().code, d.error().path, d.error().byte_offset);
    const timebase::RationalRate rate{n.value(), d.value()};
    if (!rate.valid() || rate.normalized() != rate)
        return fail<timebase::RationalRate>(PersistenceErrorCode::InvalidNumber, std::move(path),
                                            value.begin);
    return runtime::Ok(rate);
}

} // namespace pulp::timeline::detail
