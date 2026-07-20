#include "sequence_schema_migrations.hpp"

#include "sequence_schema_policy.hpp"

#include <algorithm>

namespace pulp::timeline::detail {
namespace {

template <typename T> runtime::Result<T, PersistenceError> failure() {
    return runtime::Err(PersistenceError{PersistenceErrorCode::MigrationFailed});
}

JsonValue* member(JsonValue& value, std::string_view name) noexcept {
    if (value.kind != JsonValue::Kind::Object)
        return nullptr;
    const auto found = std::find_if(value.object.begin(), value.object.end(),
                                    [name](const auto& entry) { return entry.first == name; });
    return found == value.object.end() ? nullptr : &found->second;
}

bool version_is(const JsonValue& value, std::string_view expected) noexcept {
    return value.kind == JsonValue::Kind::Number && value.scalar == expected;
}

bool base_shape(JsonValue& data) noexcept {
    const auto* absolute = member(data, "absolute_duration");
    const auto* id = member(data, "id");
    const auto* musical = member(data, "musical_duration");
    const auto* name = member(data, "name");
    const auto* tracks = member(data, "tracks");
    return absolute && id && id->kind == JsonValue::Kind::String && musical && name &&
           name->kind == JsonValue::Kind::String && tracks && tracks->kind == JsonValue::Kind::Array;
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
write(JsonValue root, BoundedJsonSink& output) noexcept {
    auto canonical = canonicalize_json(root);
    if (!canonical || !output.append(canonical.value()) || output.failed())
        return failure<SchemaWriteSuccess>();
    return runtime::Ok(SchemaWriteSuccess{});
}

} // namespace

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_sequence_v1_to_v2(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return failure<SchemaWriteSuccess>();
    auto root = parsed.value()->root();
    auto* data = member(root, "data");
    auto* version = member(root, "version");
    if (!data || !version || !version_is(*version, "1") || !base_shape(*data) ||
        member(*data, "markers") || member(*data, "regions"))
        return failure<SchemaWriteSuccess>();
    data->object.emplace_back("markers", JsonValue{.kind = JsonValue::Kind::Array});
    data->object.emplace_back("regions", JsonValue{.kind = JsonValue::Kind::Array});
    version->scalar = "2";
    return write(std::move(root), output);
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_sequence_v2_to_v1(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return failure<SchemaWriteSuccess>();
    auto root = parsed.value()->root();
    auto* data = member(root, "data");
    auto* version = member(root, "version");
    auto* markers = data ? member(*data, "markers") : nullptr;
    auto* regions = data ? member(*data, "regions") : nullptr;
    if (!data || !version || !version_is(*version, "2") || !base_shape(*data) || !markers ||
        markers->kind != JsonValue::Kind::Array || !markers->array.empty() || !regions ||
        regions->kind != JsonValue::Kind::Array || !regions->array.empty())
        return failure<SchemaWriteSuccess>();
    std::erase_if(data->object, [](const auto& entry) {
        return entry.first == "markers" || entry.first == "regions";
    });
    version->scalar = "1";
    return write(std::move(root), output);
}

} // namespace pulp::timeline::detail
