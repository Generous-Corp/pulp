#include "device_placement_schema_migrations.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <string>

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

bool number_is(const JsonValue& value, std::uint32_t expected) noexcept {
    return version_is(value, expected);
}

bool string_is(const JsonValue& value, std::string_view expected) noexcept {
    return value.kind == JsonValue::Kind::String && value.scalar == expected;
}

struct RawEdit {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::string_view replacement;
};

template <std::size_t N>
bool apply_edits(std::string_view source, std::array<RawEdit, N> edits,
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
migrate_device_placement_v1_to_v2(std::string_view source, BoundedJsonSink& output,
                                  const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return fail();
    auto root = parsed.value()->root();
    auto* data = member(root, "data");
    auto* version = member(root, "version");
    auto* id = data ? member(*data, "id") : nullptr;
    if (!data || !version || !version_is(*version, 1) || data->kind != JsonValue::Kind::Object ||
        data->object.size() != 1 || !id || id->kind != JsonValue::Kind::String ||
        data->begin >= data->end || version->begin >= version->end)
        return fail();
    std::string canonical_data =
        "{\"binding_key\":\"\",\"bypassed\":false,\"device_kind\":\"unresolved\",\"id\":";
    canonical_data.append(source.substr(id->begin, id->end - id->begin));
    canonical_data.append(
        ",\"position\":\"pre_fader\",\"slot_kind\":\"audio_to_audio\","
        "\"wet_dry_bits\":1065353216}");
    if (!apply_edits(source,
                     std::array{RawEdit{data->begin, data->end, canonical_data},
                                RawEdit{version->begin, version->end, "2"}},
                     output))
        return output.failed()
                   ? runtime::Result<SchemaWriteSuccess, PersistenceError>(
                         runtime::Err(output.error()))
                   : fail();
    return runtime::Ok(SchemaWriteSuccess{});
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_device_placement_v2_to_v1(std::string_view source, BoundedJsonSink& output,
                                  const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return fail();
    auto root = parsed.value()->root();
    auto* data = member(root, "data");
    auto* version = member(root, "version");
    auto* id = data ? member(*data, "id") : nullptr;
    auto* binding = data ? member(*data, "binding_key") : nullptr;
    auto* bypassed = data ? member(*data, "bypassed") : nullptr;
    auto* kind = data ? member(*data, "device_kind") : nullptr;
    auto* position = data ? member(*data, "position") : nullptr;
    auto* slot = data ? member(*data, "slot_kind") : nullptr;
    auto* wet = data ? member(*data, "wet_dry_bits") : nullptr;
    if (!data || !version || !version_is(*version, 2) || data->kind != JsonValue::Kind::Object ||
        data->object.size() != 7 || !id || id->kind != JsonValue::Kind::String || !binding ||
        !string_is(*binding, "") || !bypassed || bypassed->kind != JsonValue::Kind::Boolean ||
        bypassed->boolean || !kind || !string_is(*kind, "unresolved") || !position ||
        !string_is(*position, "pre_fader") || !slot || !string_is(*slot, "audio_to_audio") ||
        !wet || !number_is(*wet, 1065353216) || member(*data, "state_ref") ||
        version->begin >= version->end)
        return fail();
    std::string legacy_data = "{\"id\":";
    legacy_data.append(source.substr(id->begin, id->end - id->begin));
    legacy_data.push_back('}');
    if (!apply_edits(source,
                     std::array{RawEdit{data->begin, data->end, legacy_data},
                                RawEdit{version->begin, version->end, "1"}},
                     output))
        return output.failed()
                   ? runtime::Result<SchemaWriteSuccess, PersistenceError>(
                         runtime::Err(output.error()))
                   : fail();
    return runtime::Ok(SchemaWriteSuccess{});
}

} // namespace pulp::timeline::detail
