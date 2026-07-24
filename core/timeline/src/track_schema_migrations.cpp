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
    const auto parsed =
        std::from_chars(value.scalar.data(), value.scalar.data() + value.scalar.size(), actual);
    return parsed.ec == std::errc{} && parsed.ptr == value.scalar.data() + value.scalar.size() &&
           actual == expected;
}

bool valid_track_data_shape(JsonValue& data, std::uint32_t version) noexcept {
    const auto* id = mutable_member(data, "id");
    const auto* name = mutable_member(data, "name");
    const auto* clips = mutable_member(data, "clips");
    const auto* devices = mutable_member(data, "device_chain");
    const auto* automation = mutable_member(data, "automation_lanes");
    const auto* takes = mutable_member(data, "take_lanes");
    const auto* record = mutable_member(data, "record_armed");
    const auto* active = mutable_member(data, "active_take_lane_id");
    const auto* freeze = mutable_member(data, "freeze");
    return id && id->kind == JsonValue::Kind::String && name &&
           name->kind == JsonValue::Kind::String && clips &&
           clips->kind == JsonValue::Kind::Array &&
           (track_schema_policy.requires_device_chain(version)
                ? devices && devices->kind == JsonValue::Kind::Array
                : !devices) &&
           (track_schema_policy.requires_automation(version)
                ? automation && automation->kind == JsonValue::Kind::Array
                : !automation) &&
           (track_schema_policy.requires_takes(version)
                ? takes && takes->kind == JsonValue::Kind::Array && record &&
                      record->kind == JsonValue::Kind::Boolean
                : !takes && !record) &&
           (track_schema_policy.requires_active_take_lane(version)
                ? active && active->kind == JsonValue::Kind::String
                : !active) &&
           (track_schema_policy.supports_freeze(version)
                ? !freeze || freeze->kind == JsonValue::Kind::Object
                : !freeze);
}

struct RawEdit {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::string_view replacement;
};

template <std::size_t N>
bool valid_raw_edits(std::string_view source, std::array<RawEdit, N>& edits) {
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

template <std::size_t N>
void apply_raw_edits(std::string_view source, const std::array<RawEdit, N>& edits,
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
    if (!data || !version ||
        !valid_version(*version, track_schema_policy.device_chain_introduced_version) ||
        !valid_track_data_shape(*data, track_schema_policy.device_chain_introduced_version) ||
        !chain || chain->kind != JsonValue::Kind::Array || !chain->array.empty())
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
        !valid_track_data_shape(*data, track_schema_policy.device_chain_introduced_version) ||
        !chain || chain->kind != JsonValue::Kind::Array || data->end == 0 ||
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
    if (!data || !version ||
        !valid_version(*version, track_schema_policy.automation_introduced_version) ||
        !valid_track_data_shape(*data, track_schema_policy.automation_introduced_version) ||
        !chain || chain->kind != JsonValue::Kind::Array || !lanes ||
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

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_track_v3_to_v4(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return migration_fail<SchemaWriteSuccess>();
    auto root = parsed.value()->root();
    auto* data = mutable_member(root, "data");
    auto* version = mutable_member(root, "version");
    if (!data || !version || data->kind != JsonValue::Kind::Object ||
        !valid_version(*version, track_schema_policy.automation_introduced_version) ||
        !valid_track_data_shape(*data, track_schema_policy.automation_introduced_version) ||
        data->end == 0 || version->begin >= version->end)
        return migration_fail<SchemaWriteSuccess>();
    // record_armed and take_lanes sort last, so appending keeps canonical order.
    std::array edits{
        RawEdit{data->end - 1, data->end - 1, ",\"record_armed\":false,\"take_lanes\":[]"},
        RawEdit{version->begin, version->end, "4"}};
    if (!valid_raw_edits(source, edits))
        return migration_fail<SchemaWriteSuccess>();
    apply_raw_edits(source, edits, output);
    return runtime::Ok(SchemaWriteSuccess{});
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_track_v4_to_v3(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return migration_fail<SchemaWriteSuccess>();
    auto root = parsed.value()->root();
    auto* data = mutable_member(root, "data");
    auto* version = mutable_member(root, "version");
    auto* record = data ? mutable_member(*data, "record_armed") : nullptr;
    auto* takes = data ? mutable_member(*data, "take_lanes") : nullptr;
    if (!data || !version ||
        !valid_version(*version, track_schema_policy.takes_introduced_version) ||
        !valid_track_data_shape(*data, track_schema_policy.takes_introduced_version) || !record ||
        record->kind != JsonValue::Kind::Boolean || record->boolean || !takes ||
        takes->kind != JsonValue::Kind::Array || !takes->array.empty())
        return migration_fail<SchemaWriteSuccess>();
    // record_armed and take_lanes are the last two canonical members. Drop both
    // as one contiguous span, from the comma preceding record_armed through the
    // end of take_lanes. A non-default arming or a non-empty take set is not
    // representable at v3, so those cases fail closed above.
    const auto record_it =
        std::find_if(data->object.begin(), data->object.end(),
                     [](const auto& member) { return member.first == "record_armed"; });
    const auto record_index = static_cast<std::size_t>(record_it - data->object.begin());
    if (record_index == 0 || record_index + 1 != data->object.size() - 1 ||
        data->object.back().first != "take_lanes")
        return migration_fail<SchemaWriteSuccess>();
    const auto erase_begin = source.find(',', data->object[record_index - 1].second.end);
    if (erase_begin == std::string_view::npos || erase_begin >= record->begin)
        return migration_fail<SchemaWriteSuccess>();
    if (version->begin >= version->end)
        return migration_fail<SchemaWriteSuccess>();
    std::array edits{RawEdit{erase_begin, takes->end, {}},
                     RawEdit{version->begin, version->end, "3"}};
    if (!valid_raw_edits(source, edits))
        return migration_fail<SchemaWriteSuccess>();
    apply_raw_edits(source, edits, output);
    return runtime::Ok(SchemaWriteSuccess{});
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_track_v4_to_v5(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return migration_fail<SchemaWriteSuccess>();
    auto root = parsed.value()->root();
    auto* data = mutable_member(root, "data");
    auto* version = mutable_member(root, "version");
    if (!data || !version || data->kind != JsonValue::Kind::Object ||
        !valid_version(*version, track_schema_policy.takes_introduced_version) ||
        !valid_track_data_shape(*data, track_schema_policy.takes_introduced_version) ||
        data->end == 0 || version->begin >= version->end)
        return migration_fail<SchemaWriteSuccess>();
    // active_take_lane_id sorts first in the canonical track object.
    std::array edits{RawEdit{data->begin + 1, data->begin + 1, "\"active_take_lane_id\":\"0\","},
                     RawEdit{version->begin, version->end, "5"}};
    if (!valid_raw_edits(source, edits))
        return migration_fail<SchemaWriteSuccess>();
    apply_raw_edits(source, edits, output);
    return runtime::Ok(SchemaWriteSuccess{});
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_track_v5_to_v4(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return migration_fail<SchemaWriteSuccess>();
    auto root = parsed.value()->root();
    auto* data = mutable_member(root, "data");
    auto* version = mutable_member(root, "version");
    auto* active = data ? mutable_member(*data, "active_take_lane_id") : nullptr;
    if (!data || !version ||
        !valid_version(*version, track_schema_policy.active_take_lane_introduced_version) ||
        !valid_track_data_shape(*data, track_schema_policy.active_take_lane_introduced_version) ||
        !active || active->scalar != "0" || data->object.size() < 2 ||
        version->begin >= version->end)
        return migration_fail<SchemaWriteSuccess>();
    const auto found =
        std::find_if(data->object.begin(), data->object.end(),
                     [](const auto& member) { return member.first == "active_take_lane_id"; });
    const auto index = static_cast<std::size_t>(found - data->object.begin());
    std::size_t erase_begin = data->begin + 1;
    std::size_t erase_end = active->end;
    if (index != 0) {
        erase_begin = source.find(',', data->object[index - 1].second.end);
        if (erase_begin == std::string_view::npos || erase_begin >= active->begin)
            return migration_fail<SchemaWriteSuccess>();
    } else {
        const auto comma = source.find(',', active->end);
        if (comma == std::string_view::npos || comma >= data->object[1].second.begin)
            return migration_fail<SchemaWriteSuccess>();
        erase_end = comma + 1;
    }
    std::array edits{RawEdit{erase_begin, erase_end, {}},
                     RawEdit{version->begin, version->end, "4"}};
    if (!valid_raw_edits(source, edits))
        return migration_fail<SchemaWriteSuccess>();
    apply_raw_edits(source, edits, output);
    return runtime::Ok(SchemaWriteSuccess{});
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_track_v5_to_v6(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return migration_fail<SchemaWriteSuccess>();
    auto root = parsed.value()->root();
    auto* data = mutable_member(root, "data");
    auto* version = mutable_member(root, "version");
    if (!data || !version || data->kind != JsonValue::Kind::Object ||
        !valid_version(*version, track_schema_policy.active_take_lane_introduced_version) ||
        !valid_track_data_shape(*data, track_schema_policy.active_take_lane_introduced_version) ||
        version->begin >= version->end)
        return migration_fail<SchemaWriteSuccess>();
    std::array edits{RawEdit{version->begin, version->end, "6"}};
    if (!valid_raw_edits(source, edits))
        return migration_fail<SchemaWriteSuccess>();
    apply_raw_edits(source, edits, output);
    return runtime::Ok(SchemaWriteSuccess{});
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_track_v6_to_v5(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return migration_fail<SchemaWriteSuccess>();
    auto root = parsed.value()->root();
    auto* data = mutable_member(root, "data");
    auto* version = mutable_member(root, "version");
    if (!data || !version ||
        !valid_version(*version, track_schema_policy.freeze_introduced_version) ||
        !valid_track_data_shape(*data, track_schema_policy.freeze_introduced_version) ||
        mutable_member(*data, "freeze") || version->begin >= version->end)
        return migration_fail<SchemaWriteSuccess>();
    std::array edits{RawEdit{version->begin, version->end, "5"}};
    if (!valid_raw_edits(source, edits))
        return migration_fail<SchemaWriteSuccess>();
    apply_raw_edits(source, edits, output);
    return runtime::Ok(SchemaWriteSuccess{});
}

} // namespace pulp::timeline::detail
