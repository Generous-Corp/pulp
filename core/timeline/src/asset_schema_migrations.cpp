#include "asset_schema_migrations.hpp"

#include "asset_schema_policy.hpp"

#include <pulp/timeline/schema_json.hpp>

#include <algorithm>
#include <charconv>

namespace pulp::timeline::detail {
namespace {

template <typename T> runtime::Result<T, PersistenceError> migration_fail() {
    return runtime::Result<T, PersistenceError>(
        runtime::Err(PersistenceError{PersistenceErrorCode::MigrationFailed}));
}

JsonValue* mutable_member(JsonValue& object, std::string_view name) noexcept {
    if (object.kind != JsonValue::Kind::Object)
        return nullptr;
    const auto found = std::find_if(object.object.begin(), object.object.end(),
                                    [name](const auto& member) { return member.first == name; });
    return found == object.object.end() ? nullptr : &found->second;
}

bool valid_version(const JsonValue& value, std::uint32_t expected) noexcept {
    if (value.kind != JsonValue::Kind::Number)
        return false;
    std::uint32_t actual = 0;
    const auto parsed = std::from_chars(value.scalar.data(),
                                        value.scalar.data() + value.scalar.size(), actual);
    return parsed.ec == std::errc{} && parsed.ptr == value.scalar.data() + value.scalar.size() &&
           actual == expected;
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
rewrite_version(std::string_view source, std::uint32_t from, std::uint32_t to,
                bool reject_loop_info, BoundedJsonSink& output) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return migration_fail<SchemaWriteSuccess>();
    auto root = parsed.value()->root();
    auto* data = mutable_member(root, "data");
    auto* version = mutable_member(root, "version");
    if (!data || data->kind != JsonValue::Kind::Object || !version ||
        !valid_version(*version, from) || version->begin >= version->end ||
        (reject_loop_info && mutable_member(*data, "loop_info")))
        return migration_fail<SchemaWriteSuccess>();
    char encoded[16];
    const auto result = std::to_chars(encoded, encoded + sizeof(encoded), to);
    if (result.ec != std::errc{})
        return migration_fail<SchemaWriteSuccess>();
    if (!output.append(source.substr(0, version->begin)) ||
        !output.append(std::string_view(encoded, result.ptr - encoded)) ||
        !output.append(source.substr(version->end)))
        return migration_fail<SchemaWriteSuccess>();
    return runtime::Ok(SchemaWriteSuccess{});
}

} // namespace

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_asset_v1_to_v2(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    return rewrite_version(source, asset_schema_policy.oldest_readable_version,
                           asset_schema_policy.loop_info_introduced_version, true, output);
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_asset_v2_to_v1(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    return rewrite_version(source, asset_schema_policy.loop_info_introduced_version,
                           asset_schema_policy.oldest_readable_version, true, output);
}

} // namespace pulp::timeline::detail
