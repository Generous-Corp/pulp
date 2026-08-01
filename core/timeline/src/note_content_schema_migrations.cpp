#include "note_content_schema_migrations.hpp"

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
    const auto* notes = member(data, "notes");
    return data.kind == JsonValue::Kind::Object && notes && notes->kind == JsonValue::Kind::Array;
}

struct RawEdit {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::string_view replacement;
};

bool apply_edits(std::string_view source, std::array<RawEdit, 2> edits, BoundedJsonSink& output) {
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

runtime::Result<SchemaWriteSuccess, PersistenceError> finish(bool applied,
                                                             BoundedJsonSink& output) {
    if (!applied)
        return output.failed() ? runtime::Result<SchemaWriteSuccess, PersistenceError>(
                                     runtime::Err(output.error()))
                               : fail();
    return runtime::Ok(SchemaWriteSuccess{});
}

} // namespace

// A v1 note content authored no modifiers, so the upgrade is the identity on
// how the clip plays: an empty companion array and a zero seed. The inserted
// members sort before "notes", which keeps the result canonical.
runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_note_content_v1_to_v2(std::string_view source, BoundedJsonSink& output,
                              const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return fail();
    auto root = parsed.value()->root();
    auto* data = member(root, "data");
    auto* version = member(root, "version");
    if (!data || !version || !version_is(*version, 1) || !common_shape(*data) ||
        member(*data, "modifiers") || member(*data, "modifier_seed") || data->begin >= data->end ||
        version->begin >= version->end)
        return fail();
    return finish(apply_edits(source,
                              {RawEdit{data->begin + 1, data->begin + 1,
                                       "\"modifier_seed\":\"0\",\"modifiers\":[],"},
                               RawEdit{version->begin, version->end, "2"}},
                              output),
                  output);
}

// The downgrade is only offered when it loses nothing: a document that authors
// a modifier or a non-zero seed has no v1 spelling, and silently dropping
// either would change how it plays. Refusing is the honest answer.
runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_note_content_v2_to_v1(std::string_view source, BoundedJsonSink& output,
                              const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return fail();
    auto root = parsed.value()->root();
    auto* data = member(root, "data");
    auto* version = member(root, "version");
    auto* modifiers = data ? member(*data, "modifiers") : nullptr;
    auto* seed = data ? member(*data, "modifier_seed") : nullptr;
    if (!data || !version || !version_is(*version, 2) || !common_shape(*data) || !modifiers ||
        !seed || modifiers->kind != JsonValue::Kind::Array || !modifiers->array.empty() ||
        seed->kind != JsonValue::Kind::String || seed->scalar != "0" ||
        version->begin >= version->end)
        return fail();
    // Both members are the first two in canonical order, so the erase runs from
    // just inside the object to the separator that follows "modifiers".
    if (data->object.size() < 3 || data->object[0].first != "modifier_seed" ||
        data->object[1].first != "modifiers")
        return fail();
    const auto comma = source.find(',', modifiers->end);
    if (comma == std::string_view::npos || comma >= data->end)
        return fail();
    return finish(apply_edits(source,
                              {RawEdit{data->begin + 1, comma + 1, {}},
                               RawEdit{version->begin, version->end, "1"}},
                              output),
                  output);
}

// A v2 note content authored no controller or expression streams, so it
// upgrades to the degenerate case: an empty lane array, leaving every note,
// modifier, and the seed byte-identical. "lanes" sorts before every other
// member, so the insertion lands immediately inside the object and the result
// stays canonical.
runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_note_content_v2_to_v3(std::string_view source, BoundedJsonSink& output,
                              const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return fail();
    auto root = parsed.value()->root();
    auto* data = member(root, "data");
    auto* version = member(root, "version");
    if (!data || !version || !version_is(*version, 2) || !common_shape(*data) ||
        member(*data, "lanes") || !member(*data, "modifiers") || !member(*data, "modifier_seed") ||
        data->begin >= data->end || version->begin >= version->end)
        return fail();
    return finish(
        apply_edits(source,
                    {RawEdit{data->begin + 1, data->begin + 1, "\"lanes\":[],"},
                     RawEdit{version->begin, version->end, "3"}},
                    output),
        output);
}

// The downgrade is offered only when it loses nothing: a document that authors
// a lane has no v2 spelling, and dropping the lane would silently change what
// the clip's controllers say. Refusing is the honest answer.
runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_note_content_v3_to_v2(std::string_view source, BoundedJsonSink& output,
                              const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return fail();
    auto root = parsed.value()->root();
    auto* data = member(root, "data");
    auto* version = member(root, "version");
    auto* lanes = data ? member(*data, "lanes") : nullptr;
    if (!data || !version || !version_is(*version, 3) || !common_shape(*data) || !lanes ||
        lanes->kind != JsonValue::Kind::Array || !lanes->array.empty() ||
        version->begin >= version->end)
        return fail();
    // "lanes" is the first member in canonical order, so the erase runs from
    // just inside the object to the separator that follows it.
    if (data->object.size() < 2 || data->object[0].first != "lanes")
        return fail();
    const auto comma = source.find(',', lanes->end);
    if (comma == std::string_view::npos || comma >= data->end)
        return fail();
    return finish(apply_edits(source,
                              {RawEdit{data->begin + 1, comma + 1, {}},
                               RawEdit{version->begin, version->end, "2"}},
                              output),
                  output);
}

} // namespace pulp::timeline::detail
