#include "project_schema_migrations.hpp"

#include "project_schema_policy.hpp"

#include <algorithm>
#include <array>
#include <charconv>

namespace pulp::timeline::detail {
namespace {

// The session origin and the tuning are both optional members, so every
// direction only moves the version number: an upgraded payload simply gains
// permission to carry the member, and a downgrade is refused outright when one
// is present rather than silently discarding where the session sits on the
// house clock, or what the document is tuned to.
// Everything outside the version span is copied through byte for byte, which is
// what keeps opaque extension content inside the project intact.
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

// The project envelope carries genuinely optional members (`identities`,
// `tempo_map`, `meter_map`), so the shape check cannot be an exact member list:
// it verifies the required members are present and that canonical ascending
// order holds, which is what any offset-based reasoning depends on.
bool canonical_project_data(const JsonValue& data) noexcept {
    if (data.kind != JsonValue::Kind::Object || data.object.empty())
        return false;
    for (std::size_t index = 1; index < data.object.size(); ++index)
        if (!(data.object[index - 1].first < data.object[index].first))
            return false;
    for (const auto* required :
         {"assets", "id", "name", "next_item_id", "root_sequence_id", "sequences"})
        if (std::find_if(data.object.begin(), data.object.end(), [required](const auto& entry) {
                return entry.first == required;
            }) == data.object.end())
            return false;
    return true;
}

struct RawEdit {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::string_view replacement;
};

bool apply_edits(std::string_view source, std::array<RawEdit, 1> edits, BoundedJsonSink& output) {
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

} // namespace

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_project_v1_to_v2(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return fail();
    auto root = parsed.value()->root();
    auto* data = member(root, "data");
    auto* version = member(root, "version");
    if (!data || !version || !version_is(*version, 1) || !canonical_project_data(*data) ||
        member(*data, "session_start") || version->begin >= version->end)
        return fail();
    return finish(output,
                  apply_edits(source, {RawEdit{version->begin, version->end, "2"}}, output));
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_project_v2_to_v1(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return fail();
    auto root = parsed.value()->root();
    auto* data = member(root, "data");
    auto* version = member(root, "version");
    if (!data || !version || !version_is(*version, 2) || !canonical_project_data(*data) ||
        member(*data, "session_start") || version->begin >= version->end)
        return fail();
    return finish(output,
                  apply_edits(source, {RawEdit{version->begin, version->end, "1"}}, output));
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_project_v2_to_v3(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return fail();
    auto root = parsed.value()->root();
    auto* data = member(root, "data");
    auto* version = member(root, "version");
    if (!data || !version || !version_is(*version, 2) || !canonical_project_data(*data) ||
        member(*data, "tuning") || version->begin >= version->end)
        return fail();
    return finish(output,
                  apply_edits(source, {RawEdit{version->begin, version->end, "3"}}, output));
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_project_v3_to_v2(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return fail();
    auto root = parsed.value()->root();
    auto* data = member(root, "data");
    auto* version = member(root, "version");
    // A tuning decides what every pitch in the document sounds like. A v2
    // reader that silently dropped it would play the whole project in whatever
    // its default is, so a project that states one refuses to downgrade.
    if (!data || !version || !version_is(*version, 3) || !canonical_project_data(*data) ||
        member(*data, "tuning") || version->begin >= version->end)
        return fail();
    return finish(output,
                  apply_edits(source, {RawEdit{version->begin, version->end, "2"}}, output));
}

} // namespace pulp::timeline::detail
