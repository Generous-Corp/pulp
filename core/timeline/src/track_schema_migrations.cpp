#include "track_schema_migrations.hpp"

#include "track_schema_policy.hpp"

#include <algorithm>
#include <array>
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

bool valid_track_data_shape(JsonValue& data, std::uint32_t version) noexcept {
    const auto* id = mutable_member(data, "id");
    const auto* name = mutable_member(data, "name");
    const auto* clips = mutable_member(data, "clips");
    const auto* devices = mutable_member(data, "device_chain");
    const auto* automation = mutable_member(data, "automation_lanes");
    return id && id->kind == JsonValue::Kind::String && name &&
           name->kind == JsonValue::Kind::String && clips && clips->kind == JsonValue::Kind::Array &&
           (track_schema_policy.requires_device_chain(version) ? devices && devices->kind == JsonValue::Kind::Array
                                            : !devices) &&
           (track_schema_policy.requires_automation(version)
                ? automation && automation->kind == JsonValue::Kind::Array
                : !automation);
}

struct RawEdit {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::string_view replacement;
};

bool valid_raw_edits(std::string_view source, std::array<RawEdit, 2>& edits) {
    std::sort(edits.begin(), edits.end(),
              [](const RawEdit& lhs, const RawEdit& rhs) { return lhs.begin < rhs.begin; });
    std::size_t cursor = 0;
    for (const auto& edit : edits) {
        if (edit.begin < cursor || edit.end < edit.begin || edit.end > source.size())
            return false;
        cursor = edit.end;
    }
    return true;
}

void apply_raw_edits(std::string_view source, const std::array<RawEdit, 2>& edits,
                     BoundedJsonSink& output) {
    std::size_t cursor = 0;
    for (const auto& edit : edits) {
        output.append(source.substr(cursor, edit.begin - cursor));
        output.append(edit.replacement);
        cursor = edit.end;
    }
    output.append(source.substr(cursor));
}

} // namespace

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_track_v1_to_v2(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return migration_fail<SchemaWriteSuccess>();
    auto root = parsed.value()->root();
    auto* data = mutable_member(root, "data");
    auto* version = mutable_member(root, "version");
    if (!data || !version || data->kind != JsonValue::Kind::Object ||
        !valid_version(*version, track_schema_policy.oldest_readable_version) ||
        !valid_track_data_shape(*data, track_schema_policy.oldest_readable_version) ||
        data->end == 0 || version->begin >= version->end)
        return migration_fail<SchemaWriteSuccess>();
    std::array edits{RawEdit{data->end - 1, data->end - 1, ",\"device_chain\":[]"},
                     RawEdit{version->begin, version->end, "2"}};
    if (!valid_raw_edits(source, edits))
        return migration_fail<SchemaWriteSuccess>();
    apply_raw_edits(source, edits, output);
    return runtime::Ok(SchemaWriteSuccess{});
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_track_v2_to_v1(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return migration_fail<SchemaWriteSuccess>();
    auto root = parsed.value()->root();
    auto* data = mutable_member(root, "data");
    auto* version = mutable_member(root, "version");
    auto* chain = data ? mutable_member(*data, "device_chain") : nullptr;
    if (!data || !version || !valid_version(*version, track_schema_policy.device_chain_introduced_version) ||
        !valid_track_data_shape(*data, track_schema_policy.device_chain_introduced_version) || !chain ||
        chain->kind != JsonValue::Kind::Array || !chain->array.empty())
        return migration_fail<SchemaWriteSuccess>();
    const auto found =
        std::find_if(data->object.begin(), data->object.end(),
                     [](const auto& member) { return member.first == "device_chain"; });
    const auto index = static_cast<std::size_t>(found - data->object.begin());
    std::size_t erase_begin = data->begin + 1;
    std::size_t erase_end = chain->end;
    if (index != 0) {
        erase_begin = source.find(',', data->object[index - 1].second.end);
        if (erase_begin == std::string_view::npos || erase_begin >= chain->begin)
            return migration_fail<SchemaWriteSuccess>();
    } else {
        while (erase_begin < chain->begin &&
               (source[erase_begin] == ' ' || source[erase_begin] == '\t' ||
                source[erase_begin] == '\n' || source[erase_begin] == '\r'))
            ++erase_begin;
        const auto comma = source.find(',', chain->end);
        if (comma == std::string_view::npos || comma >= data->object[1].second.begin)
            return migration_fail<SchemaWriteSuccess>();
        erase_end = comma + 1;
        while (erase_end < data->object[1].second.begin &&
               (source[erase_end] == ' ' || source[erase_end] == '\t' ||
                source[erase_end] == '\n' || source[erase_end] == '\r'))
            ++erase_end;
    }
    if (version->begin >= version->end)
        return migration_fail<SchemaWriteSuccess>();
    std::array edits{RawEdit{erase_begin, erase_end, {}},
                     RawEdit{version->begin, version->end, "1"}};
    if (!valid_raw_edits(source, edits))
        return migration_fail<SchemaWriteSuccess>();
    apply_raw_edits(source, edits, output);
    return runtime::Ok(SchemaWriteSuccess{});
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_track_v2_to_v3(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return migration_fail<SchemaWriteSuccess>();
    auto root = parsed.value()->root();
    auto* data = mutable_member(root, "data");
    auto* version = mutable_member(root, "version");
    auto* chain = data ? mutable_member(*data, "device_chain") : nullptr;
    if (!data || !version || data->kind != JsonValue::Kind::Object ||
        !valid_version(*version, track_schema_policy.device_chain_introduced_version) ||
        !valid_track_data_shape(*data, track_schema_policy.device_chain_introduced_version) || !chain ||
        chain->kind != JsonValue::Kind::Array || data->end == 0 ||
        version->begin >= version->end)
        return migration_fail<SchemaWriteSuccess>();
    std::array edits{RawEdit{data->end - 1, data->end - 1, ",\"automation_lanes\":[]"},
                     RawEdit{version->begin, version->end, "3"}};
    if (!valid_raw_edits(source, edits))
        return migration_fail<SchemaWriteSuccess>();
    apply_raw_edits(source, edits, output);
    return runtime::Ok(SchemaWriteSuccess{});
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_track_v3_to_v2(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return migration_fail<SchemaWriteSuccess>();
    auto root = parsed.value()->root();
    auto* data = mutable_member(root, "data");
    auto* version = mutable_member(root, "version");
    auto* lanes = data ? mutable_member(*data, "automation_lanes") : nullptr;
    auto* chain = data ? mutable_member(*data, "device_chain") : nullptr;
    if (!data || !version || !valid_version(*version, track_schema_policy.automation_introduced_version) ||
        !valid_track_data_shape(*data, track_schema_policy.automation_introduced_version) || !chain ||
        chain->kind != JsonValue::Kind::Array || !lanes ||
        lanes->kind != JsonValue::Kind::Array || !lanes->array.empty())
        return migration_fail<SchemaWriteSuccess>();
    const auto found =
        std::find_if(data->object.begin(), data->object.end(),
                     [](const auto& member) { return member.first == "automation_lanes"; });
    const auto index = static_cast<std::size_t>(found - data->object.begin());
    std::size_t erase_begin = data->begin + 1;
    std::size_t erase_end = lanes->end;
    if (index != 0) {
        erase_begin = source.find(',', data->object[index - 1].second.end);
        if (erase_begin == std::string_view::npos || erase_begin >= lanes->begin)
            return migration_fail<SchemaWriteSuccess>();
    } else {
        const auto comma = source.find(',', lanes->end);
        if (comma == std::string_view::npos || data->object.size() < 2 ||
            comma >= data->object[1].second.begin)
            return migration_fail<SchemaWriteSuccess>();
        erase_end = comma + 1;
    }
    std::array edits{RawEdit{erase_begin, erase_end, {}},
                     RawEdit{version->begin, version->end, "2"}};
    if (!valid_raw_edits(source, edits))
        return migration_fail<SchemaWriteSuccess>();
    apply_raw_edits(source, edits, output);
    return runtime::Ok(SchemaWriteSuccess{});
}

} // namespace pulp::timeline::detail
