#include "sequence_schema_migrations.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <span>
#include <string_view>

namespace pulp::timeline::detail {
namespace {

// The sequence envelope carries whole track subtrees, and a track may hold
// opaque extension content that must survive re-save byte for byte. So both
// directions splice the raw source rather than re-serializing a parsed tree:
// every byte outside the edited spans is copied through untouched.
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

// Both splices depend on canonical member order, so the exact key sequence is
// checked before any offset is trusted. A reordered payload fails closed rather
// than emitting a member in the wrong slot.
bool has_exact_members(const JsonValue& data, std::span<const std::string_view> names) noexcept {
    if (data.kind != JsonValue::Kind::Object || data.object.size() != names.size())
        return false;
    for (std::size_t index = 0; index < names.size(); ++index)
        if (data.object[index].first != names[index])
            return false;
    return true;
}

struct RawEdit {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::string_view replacement;
};

bool apply_edits(std::string_view source, std::array<RawEdit, 3> edits, BoundedJsonSink& output) {
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

runtime::Result<SchemaWriteSuccess, PersistenceError> finish(BoundedJsonSink& output, bool wrote) {
    if (wrote)
        return runtime::Ok(SchemaWriteSuccess{});
    return output.failed()
               ? runtime::Result<SchemaWriteSuccess, PersistenceError>(runtime::Err(output.error()))
               : fail();
}

constexpr std::string_view v1_members[] = {"absolute_duration", "id", "musical_duration", "name",
                                           "tracks"};
constexpr std::string_view v2_members[] = {
    "absolute_duration", "id", "markers", "musical_duration", "name", "regions", "tracks"};

} // namespace

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_sequence_v1_to_v2(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return fail();
    auto root = parsed.value()->root();
    auto* data = member(root, "data");
    auto* version = member(root, "version");
    if (!data || !version || !version_is(*version, 1) || !has_exact_members(*data, v1_members) ||
        version->begin >= version->end)
        return fail();
    const auto& id_value = data->object[1].second;
    const auto& name_value = data->object[3].second;
    if (id_value.begin >= id_value.end || name_value.begin >= name_value.end)
        return fail();
    return finish(output, apply_edits(source,
                                      {RawEdit{id_value.end, id_value.end, ",\"markers\":[]"},
                                       RawEdit{name_value.end, name_value.end, ",\"regions\":[]"},
                                       RawEdit{version->begin, version->end, "2"}},
                                      output));
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_sequence_v2_to_v1(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return fail();
    auto root = parsed.value()->root();
    auto* data = member(root, "data");
    auto* version = member(root, "version");
    if (!data || !version || !version_is(*version, 2) || !has_exact_members(*data, v2_members) ||
        version->begin >= version->end)
        return fail();
    const auto& markers = data->object[2].second;
    const auto& regions = data->object[5].second;
    // A downgrade never discards authored annotations: only the empty arrays a
    // v1 reader would have produced can be dropped.
    if (markers.kind != JsonValue::Kind::Array || !markers.array.empty() ||
        regions.kind != JsonValue::Kind::Array || !regions.array.empty())
        return fail();
    const auto markers_comma = source.find(',', data->object[1].second.end);
    const auto regions_comma = source.find(',', data->object[4].second.end);
    if (markers_comma == std::string_view::npos || markers_comma >= markers.begin ||
        regions_comma == std::string_view::npos || regions_comma >= regions.begin)
        return fail();
    return finish(output, apply_edits(source,
                                      {RawEdit{markers_comma, markers.end, {}},
                                       RawEdit{regions_comma, regions.end, {}},
                                       RawEdit{version->begin, version->end, "1"}},
                                      output));
}

} // namespace pulp::timeline::detail
