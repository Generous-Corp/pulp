#include "take_lane_schema_migrations.hpp"

#include <algorithm>
#include <array>
#include <charconv>

namespace pulp::timeline::detail {
namespace {

runtime::Result<SchemaWriteSuccess, PersistenceError> fail() {
    return runtime::Err(PersistenceError{PersistenceErrorCode::MigrationFailed});
}

JsonValue* member(JsonValue& object, std::string_view name) noexcept {
    if (object.kind != JsonValue::Kind::Object)
        return nullptr;
    const auto found = std::find_if(object.object.begin(), object.object.end(),
                                    [name](const auto& entry) { return entry.first == name; });
    return found == object.object.end() ? nullptr : &found->second;
}

bool version_is(const JsonValue& value, std::uint32_t expected) noexcept {
    if (value.kind != JsonValue::Kind::Number)
        return false;
    std::uint32_t decoded = 0;
    const auto parsed =
        std::from_chars(value.scalar.data(), value.scalar.data() + value.scalar.size(), decoded);
    return parsed.ec == std::errc{} && parsed.ptr == value.scalar.data() + value.scalar.size() &&
           decoded == expected;
}

bool common_shape(JsonValue& data) noexcept {
    const auto* id = member(data, "id");
    const auto* name = member(data, "name");
    const auto* takes = member(data, "takes");
    return data.kind == JsonValue::Kind::Object && id && id->kind == JsonValue::Kind::String &&
           name && name->kind == JsonValue::Kind::String && takes &&
           takes->kind == JsonValue::Kind::Array;
}

struct RawEdit {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::string_view replacement;
};

bool apply_edits(std::string_view source, std::array<RawEdit, 2> edits,
                 BoundedJsonSink& output) {
    std::sort(edits.begin(), edits.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.begin < rhs.begin; });
    std::size_t cursor = 0;
    for (const auto& edit : edits) {
        if (edit.begin < cursor || edit.end < edit.begin || edit.end > source.size())
            return false;
        if (!output.append(source.substr(cursor, edit.begin - cursor)) ||
            !output.append(edit.replacement))
            return false;
        cursor = edit.end;
    }
    return output.append(source.substr(cursor));
}

} // namespace

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_take_lane_v1_to_v2(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return fail();
    auto root = parsed.value()->root();
    auto* data = member(root, "data");
    auto* version = member(root, "version");
    if (!data || !version || !version_is(*version, 1) || !common_shape(*data) ||
        member(*data, "comp_segments") || data->begin >= data->end ||
        version->begin >= version->end)
        return fail();
    if (!apply_edits(
            source,
            {RawEdit{data->begin + 1, data->begin + 1, "\"comp_segments\":[],"},
             RawEdit{version->begin, version->end, "2"}},
            output))
        return output.failed()
                   ? runtime::Result<SchemaWriteSuccess, PersistenceError>(
                         runtime::Err(output.error()))
                   : fail();
    return runtime::Ok(SchemaWriteSuccess{});
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_take_lane_v2_to_v1(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return fail();
    auto root = parsed.value()->root();
    auto* data = member(root, "data");
    auto* version = member(root, "version");
    auto* comp = data ? member(*data, "comp_segments") : nullptr;
    if (!data || !version || !version_is(*version, 2) || !common_shape(*data) || !comp ||
        comp->kind != JsonValue::Kind::Array || !comp->array.empty() ||
        version->begin >= version->end)
        return fail();
    const auto found =
        std::find_if(data->object.begin(), data->object.end(),
                     [](const auto& entry) { return entry.first == "comp_segments"; });
    if (found == data->object.end())
        return fail();
    const auto index = static_cast<std::size_t>(found - data->object.begin());
    std::size_t erase_begin = data->begin + 1;
    std::size_t erase_end = comp->end;
    if (index == 0) {
        const auto comma = source.find(',', comp->end);
        if (comma == std::string_view::npos)
            return fail();
        erase_end = comma + 1;
    } else {
        erase_begin = source.find(',', data->object[index - 1].second.end);
        if (erase_begin == std::string_view::npos || erase_begin >= comp->begin)
            return fail();
    }
    if (!apply_edits(source,
                     {RawEdit{erase_begin, erase_end, {}},
                      RawEdit{version->begin, version->end, "1"}},
                     output))
        return output.failed()
                   ? runtime::Result<SchemaWriteSuccess, PersistenceError>(
                         runtime::Err(output.error()))
                   : fail();
    return runtime::Ok(SchemaWriteSuccess{});
}

} // namespace pulp::timeline::detail
