#include "clip_schema_migrations.hpp"

#include "clip_schema_policy.hpp"

#include <pulp/timeline/schema_json.hpp>

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
    const auto* content = member(data, "content");
    const auto* id = member(data, "id");
    const auto* time_range = member(data, "time_range");
    return data.kind == JsonValue::Kind::Object && content &&
           content->kind == JsonValue::Kind::Object && id && id->kind == JsonValue::Kind::String &&
           time_range && time_range->kind == JsonValue::Kind::Object;
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
        if (edit.begin < cursor || edit.end < edit.begin || edit.end > source.size() ||
            !output.append(source.substr(cursor, edit.begin - cursor)) ||
            !output.append(edit.replacement))
            return false;
        cursor = edit.end;
    }
    return output.append(source.substr(cursor));
}

runtime::Result<SchemaWriteSuccess, PersistenceError> finish(BoundedJsonSink& output, bool wrote) {
    if (wrote)
        return runtime::Ok(SchemaWriteSuccess{});
    return output.failed()
               ? runtime::Result<SchemaWriteSuccess, PersistenceError>(runtime::Err(output.error()))
               : fail();
}

} // namespace

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_clip_v1_to_v2(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return fail();
    auto root = parsed.value()->root();
    auto* data = member(root, "data");
    auto* version = member(root, "version");
    auto* time_range = data ? member(*data, "time_range") : nullptr;
    if (!data || !version || !version_is(*version, clip_schema_policy.oldest_readable_version) ||
        !time_range || !common_shape(*data) || member(*data, "time_conform") ||
        data->object.empty() || version->begin >= version->end)
        return fail();
    // Append after the final parsed top-level value. This retains every authored
    // byte (including member order and whitespace) and cannot be confused by a
    // nested key with the same spelling.
    const auto insertion = data->object.back().second.end;
    return finish(output,
                  apply_edits(source,
                              {RawEdit{insertion, insertion, ",\"time_conform\":\"none\""},
                               RawEdit{version->begin, version->end, "2"}},
                              output));
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_clip_v2_to_v1(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return fail();
    auto root = parsed.value()->root();
    auto* data = member(root, "data");
    auto* version = member(root, "version");
    auto* time_conform = data ? member(*data, "time_conform") : nullptr;
    if (!data || !version ||
        !version_is(*version, clip_schema_policy.time_conform_introduced_version) ||
        !common_shape(*data) || !time_conform || time_conform->kind != JsonValue::Kind::String ||
        time_conform->scalar != "none" || version->begin >= version->end)
        return fail();
    const auto found = std::find_if(data->object.begin(), data->object.end(),
                                    [](const auto& entry) {
                                        return entry.first == "time_conform";
                                    });
    if (found == data->object.end() || data->object.size() < 2)
        return fail();
    const auto index = static_cast<std::size_t>(found - data->object.begin());
    std::size_t erase_begin = data->begin + 1;
    std::size_t erase_end = time_conform->end;
    if (index != 0) {
        // The structural comma between the known adjacent top-level values is
        // the member boundary. Searches are bounded by parsed sibling spans.
        erase_begin = source.find(',', data->object[index - 1].second.end);
        if (erase_begin == std::string_view::npos || erase_begin >= time_conform->begin)
            return fail();
    } else {
        const auto comma = source.find(',', time_conform->end);
        if (comma == std::string_view::npos || comma >= data->object[1].second.begin)
            return fail();
        erase_end = comma + 1;
    }
    return finish(output,
                  apply_edits(source,
                              {RawEdit{erase_begin, erase_end, {}},
                               RawEdit{version->begin, version->end, "1"}},
                              output));
}

} // namespace pulp::timeline::detail
