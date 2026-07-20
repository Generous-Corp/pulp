#include <pulp/timeline/schema_json.hpp>

#include <charconv>

namespace pulp::timeline {
namespace {

template <typename T>
runtime::Result<T, PersistenceError> fail(PersistenceErrorCode code, std::size_t offset = 0,
                                          std::uint64_t actual = 0,
                                          std::uint64_t limit = 0,
                                          std::string path = {}) {
    return runtime::Result<T, PersistenceError>(runtime::Err(
        PersistenceError{code, offset, actual, limit, std::move(path), std::nullopt}));
}

template <typename T>
runtime::Result<T, PersistenceError> parse_wide(const JsonValue& value, std::string path,
                                                bool allow_negative) {
    if (value.kind != JsonValue::Kind::String || value.scalar.empty())
        return fail<T>(PersistenceErrorCode::UnexpectedType, value.begin, 0, 0, std::move(path));
    const auto text = std::string_view(value.scalar);
    if (text == "-0" || text.front() == '+' ||
        (text.front() == '0' && text.size() != 1) ||
        (text.front() == '-' && (!allow_negative || text.size() == 1 ||
                                 (text.size() > 2 && text[1] == '0'))))
        return fail<T>(PersistenceErrorCode::InvalidNumber, value.begin, 0, 0, std::move(path));
    T result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        return fail<T>(PersistenceErrorCode::InvalidNumber, value.begin, 0, 0, std::move(path));
    return runtime::Result<T, PersistenceError>(runtime::Ok(result));
}

} // namespace

DecodeLimits DecodeLimits::web_defaults() noexcept {
    auto limits = DecodeLimits{};
    limits.max_input_bytes = 256ull * 1024ull * 1024ull;
    limits.max_total_values = 8'000'000;
    limits.max_array_elements = 2'000'000;
    limits.max_opaque_bytes = 16ull * 1024ull * 1024ull;
    limits.max_notes = 1'000'000;
    limits.max_automation_lanes = 25'000;
    limits.max_automation_points = 1'000'000;
    limits.max_sequence_markers = 25'000;
    limits.max_sequence_regions = 25'000;
    return limits;
}

runtime::Result<const JsonValue*, PersistenceError>
validate_exact_envelope(const JsonValue& value, std::string_view expected_type,
                        std::uint32_t expected_version, std::string path,
                        PersistenceErrorCode failure_code) {
    if (value.kind != JsonValue::Kind::Object || value.object.size() != 3)
        return fail<const JsonValue*>(failure_code, value.begin, 0, 0,
                                      std::move(path));
    const auto* data = value.find("data");
    const auto* type = value.find("type_name");
    const auto* version = value.find("version");
    if (!data || data->kind != JsonValue::Kind::Object || !type ||
        type->kind != JsonValue::Kind::String || type->scalar != expected_type ||
        !version)
        return fail<const JsonValue*>(failure_code, value.begin, 0, 0,
                                      std::move(path));
    auto decoded_version = parse_u32_number(*version, path + "/version");
    if (!decoded_version || decoded_version.value() != expected_version)
        return fail<const JsonValue*>(failure_code, value.begin, 0, 0,
                                      std::move(path));
    return runtime::Result<const JsonValue*, PersistenceError>(runtime::Ok(data));
}


runtime::Result<std::uint64_t, PersistenceError>
parse_canonical_u64_string(const JsonValue& value, std::string path) {
    return parse_wide<std::uint64_t>(value, std::move(path), false);
}

runtime::Result<std::int64_t, PersistenceError>
parse_canonical_i64_string(const JsonValue& value, std::string path) {
    return parse_wide<std::int64_t>(value, std::move(path), true);
}

runtime::Result<std::uint32_t, PersistenceError>
parse_u32_number(const JsonValue& value, std::string path) {
    if (value.kind != JsonValue::Kind::Number || value.scalar.empty() || value.scalar.front() == '-' ||
        value.scalar.find_first_of(".eE") != std::string::npos)
        return fail<std::uint32_t>(PersistenceErrorCode::UnexpectedType, value.begin, 0, 0,
                                   std::move(path));
    std::uint32_t result = 0;
    const auto parsed = std::from_chars(value.scalar.data(),
                                        value.scalar.data() + value.scalar.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.scalar.data() + value.scalar.size())
        return fail<std::uint32_t>(PersistenceErrorCode::InvalidNumber, value.begin, 0, 0,
                                   std::move(path));
    return runtime::Result<std::uint32_t, PersistenceError>(runtime::Ok(result));
}

} // namespace pulp::timeline
