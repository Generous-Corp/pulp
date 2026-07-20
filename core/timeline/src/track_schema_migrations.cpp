#include "track_schema_migrations.hpp"

#include <algorithm>
#include <array>

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

bool valid_track_data_shape(JsonValue& data) noexcept {
    const auto* id = mutable_member(data, "id");
    const auto* name = mutable_member(data, "name");
    const auto* clips = mutable_member(data, "clips");
    return id && id->kind == JsonValue::Kind::String && name &&
           name->kind == JsonValue::Kind::String && clips && clips->kind == JsonValue::Kind::Array;
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
        !valid_track_data_shape(*data) || mutable_member(*data, "device_chain") || data->end == 0 ||
        version->begin >= version->end)
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
    if (!data || !version || !valid_track_data_shape(*data) || !chain ||
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

} // namespace pulp::timeline::detail
