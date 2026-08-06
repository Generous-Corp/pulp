#include "sequence_schema_migrations.hpp"

#include "sequence_schema_policy.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

bool apply_edits(std::string_view source, std::span<RawEdit> edits, BoundedJsonSink& output) {
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
constexpr std::string_view v3_members[] = {"absolute_duration", "chord_scale_lane", "id",
                                           "markers",           "musical_duration", "name",
                                           "regions",           "tracks"};
constexpr std::string_view v4_members[] = {
    "absolute_duration", "chord_scale_lane", "groove", "id",    "markers",
    "musical_duration",  "name",             "regions", "tracks"};
constexpr std::string_view v5_members[] = {
    "absolute_duration", "chord_scale_lane", "groove",  "id",    "markers",
    "musical_duration",  "name",             "regions", "scenes", "tracks"};
constexpr std::string_view v6_members[] = {
    "absolute_duration", "chord_scale_lane", "groove", "id",          "markers", "musical_duration",
    "name",              "regions",          "scenes", "track_order", "tracks"};
constexpr std::string_view groove_members[] = {
    "name",            "step",            "steps",           "swing_denominator", "swing_grid",
    "swing_numerator", "timing_strength", "velocity_strength"};

bool scalar_is(const JsonValue& value, JsonValue::Kind kind, std::string_view expected) noexcept {
    return value.kind == kind && value.scalar == expected;
}

// Whether the groove is the straight feel and nothing else. Every member is
// checked, including the strengths that are inert without a feel to attenuate:
// they are still data the user authored, and a downgrade may not drop authored
// data quietly just because dropping it is inaudible.
bool states_no_feel(const JsonValue& groove) noexcept {
    if (!has_exact_members(groove, groove_members))
        return false;
    const auto& steps = groove.object[2].second;
    return scalar_is(groove.object[0].second, JsonValue::Kind::String, "") &&
           scalar_is(groove.object[1].second, JsonValue::Kind::String, "0") &&
           steps.kind == JsonValue::Kind::Array && steps.array.empty() &&
           scalar_is(groove.object[3].second, JsonValue::Kind::String, "2") &&
           scalar_is(groove.object[4].second, JsonValue::Kind::String, "0") &&
           scalar_is(groove.object[5].second, JsonValue::Kind::String, "1") &&
           scalar_is(groove.object[6].second, JsonValue::Kind::Number, "1000") &&
           scalar_is(groove.object[7].second, JsonValue::Kind::Number, "1000");
}

// Whether the authored order says the same thing as the track list read
// top to bottom. An empty order already means the identity order, so both
// spellings of "nothing was reordered" survive a downgrade; anything else is
// authored intent a v5 reader would resolve to identity order instead.
bool states_identity_track_order(const JsonValue& order, const JsonValue& tracks) noexcept {
    if (order.kind != JsonValue::Kind::Array || tracks.kind != JsonValue::Kind::Array)
        return false;
    if (order.array.empty())
        return true;
    if (order.array.size() != tracks.array.size())
        return false;
    for (std::size_t index = 0; index < order.array.size(); ++index) {
        const auto& named = order.array[index];
        const auto* data = tracks.array[index].find("data");
        const auto* id = data ? data->find("id") : nullptr;
        if (!id || named.kind != JsonValue::Kind::String || id->kind != JsonValue::Kind::String ||
            named.scalar != id->scalar)
            return false;
    }
    return true;
}


// v7 adds no top-level sequence member: the chord detail lives on each event
// and the section role inside each region envelope, so the v6 key list is also
// the v7 one. Deliberately aliased rather than duplicated, since two identical
// lists can only ever drift apart.
constexpr auto& v7_members = v6_members;
// The three members a v7 chord event carries beyond a v6 one, and the defaults
// that make an upgraded event mean exactly what the v6 one meant. They are
// written as one canonical run so the upgrade and the downgrade cannot spell
// them differently.
constexpr std::string_view kChordDetailPrefix = "\"chord_bass\":null,\"chord_extensions\":0,";
constexpr std::string_view kChordDetailSuffix = ",\"voicing\":null";
constexpr std::string_view kDefaultSectionRole = ",\"role\":\"unspecified\"";
constexpr std::string_view v6_chord_event_members[] = {"chord_quality", "chord_root", "position",
                                                       "scale_mode", "scale_root"};
constexpr std::string_view v7_chord_event_members[] = {
    "chord_bass", "chord_extensions", "chord_quality", "chord_root",
    "position",   "scale_mode",       "scale_root",    "voicing"};

// A region's data carries an optional color, so its shape cannot be an exact
// member list. Ascending key order plus the required members is what any
// offset-based reasoning below depends on.
bool canonical_region_data(const JsonValue& data, bool expect_role) noexcept {
    if (data.kind != JsonValue::Kind::Object)
        return false;
    for (std::size_t index = 1; index < data.object.size(); ++index)
        if (!(data.object[index - 1].first < data.object[index].first))
            return false;
    for (const auto required : {"duration", "id", "name", "position"})
        if (std::find_if(data.object.begin(), data.object.end(), [required](const auto& entry) {
                return entry.first == required;
            }) == data.object.end())
            return false;
    const auto& last = data.object.back();
    return expect_role ? last.first == "role" && last.second.kind == JsonValue::Kind::String
                       : last.first == "position";
}

const JsonValue* region_data(const JsonValue& region) noexcept {
    if (region.kind != JsonValue::Kind::Object)
        return nullptr;
    const auto* data = region.find("data");
    return data && data->kind == JsonValue::Kind::Object ? data : nullptr;
}

// Whether a v7 chord event says exactly what a v6 one could have said. A bass,
// any extension, or a voicing hint is authored harmony with no v6 spelling.
bool states_no_chord_detail(const JsonValue& event) noexcept {
    if (!has_exact_members(event, v7_chord_event_members))
        return false;
    return event.object[0].second.kind == JsonValue::Kind::Null &&
           scalar_is(event.object[1].second, JsonValue::Kind::Number, "0") &&
           event.object[7].second.kind == JsonValue::Kind::Null;
}

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
    std::array edits{RawEdit{id_value.end, id_value.end, ",\"markers\":[]"},
                     RawEdit{name_value.end, name_value.end, ",\"regions\":[]"},
                     RawEdit{version->begin, version->end, "2"}};
    return finish(output, apply_edits(source, edits, output));
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
    std::array edits{RawEdit{markers_comma, markers.end, {}},
                     RawEdit{regions_comma, regions.end, {}},
                     RawEdit{version->begin, version->end, "1"}};
    return finish(output, apply_edits(source, edits, output));
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_sequence_v2_to_v3(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return fail();
    auto root = parsed.value()->root();
    auto* data = member(root, "data");
    auto* version = member(root, "version");
    if (!data || !version || !version_is(*version, 2) || !has_exact_members(*data, v2_members) ||
        version->begin >= version->end)
        return fail();
    // The lane sorts immediately after absolute_duration in canonical order, and
    // has_exact_members already proved that order, so this offset is trustworthy.
    const auto& absolute_value = data->object[0].second;
    if (absolute_value.begin >= absolute_value.end)
        return fail();
    std::array edits{
        RawEdit{absolute_value.end, absolute_value.end, ",\"chord_scale_lane\":[]"},
        RawEdit{version->begin, version->end, "3"}};
    return finish(output, apply_edits(source, edits, output));
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_sequence_v3_to_v2(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return fail();
    auto root = parsed.value()->root();
    auto* data = member(root, "data");
    auto* version = member(root, "version");
    if (!data || !version || !version_is(*version, 3) || !has_exact_members(*data, v3_members) ||
        version->begin >= version->end)
        return fail();
    const auto& lane = data->object[1].second;
    // A v2 reader has nowhere to put authored harmony. Writing the document
    // without it would silently retune every generator that follows the lane, so
    // only the empty array a v2 reader would have produced can be dropped.
    if (lane.kind != JsonValue::Kind::Array || !lane.array.empty())
        return fail();
    const auto lane_comma = source.find(',', data->object[0].second.end);
    if (lane_comma == std::string_view::npos || lane_comma >= lane.begin)
        return fail();
    std::array edits{RawEdit{lane_comma, lane.end, {}},
                     RawEdit{version->begin, version->end, "2"}};
    return finish(output, apply_edits(source, edits, output));
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_sequence_v3_to_v4(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return fail();
    auto root = parsed.value()->root();
    auto* data = member(root, "data");
    auto* version = member(root, "version");
    if (!data || !version || !version_is(*version, 3) || !has_exact_members(*data, v3_members) ||
        version->begin >= version->end)
        return fail();
    // The groove sorts immediately after chord_scale_lane in canonical order,
    // and has_exact_members already proved that order, so this offset is
    // trustworthy.
    const auto& lane_value = data->object[1].second;
    if (lane_value.begin >= lane_value.end)
        return fail();
    std::string inserted = ",\"groove\":";
    inserted += kStraightGrooveJson;
    std::array edits{RawEdit{lane_value.end, lane_value.end, inserted},
                     RawEdit{version->begin, version->end, "4"}};
    return finish(output, apply_edits(source, edits, output));
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_sequence_v4_to_v3(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return fail();
    auto root = parsed.value()->root();
    auto* data = member(root, "data");
    auto* version = member(root, "version");
    if (!data || !version || !version_is(*version, 4) || !has_exact_members(*data, v4_members) ||
        version->begin >= version->end)
        return fail();
    const auto& groove = data->object[2].second;
    // A v3 reader has nowhere to put a feel. Writing the document without it
    // would move every note in the sequence while reporting success, so only the
    // straight groove a v3 reader would have produced can be dropped.
    if (!states_no_feel(groove))
        return fail();
    const auto groove_comma = source.find(',', data->object[1].second.end);
    if (groove_comma == std::string_view::npos || groove_comma >= groove.begin)
        return fail();
    std::array edits{RawEdit{groove_comma, groove.end, {}},
                     RawEdit{version->begin, version->end, "3"}};
    return finish(output, apply_edits(source, edits, output));
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_sequence_v4_to_v5(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return fail();
    auto root = parsed.value()->root();
    auto* data = member(root, "data");
    auto* version = member(root, "version");
    if (!data || !version || !version_is(*version, 4) || !has_exact_members(*data, v4_members) ||
        version->begin >= version->end)
        return fail();
    const auto& regions = data->object[7].second;
    if (regions.begin >= regions.end)
        return fail();
    std::array edits{RawEdit{regions.end, regions.end, ",\"scenes\":[]"},
                     RawEdit{version->begin, version->end, "5"}};
    return finish(output, apply_edits(source, edits, output));
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_sequence_v5_to_v4(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return fail();
    auto root = parsed.value()->root();
    auto* data = member(root, "data");
    auto* version = member(root, "version");
    if (!data || !version || !version_is(*version, 5) || !has_exact_members(*data, v5_members) ||
        version->begin >= version->end)
        return fail();
    const auto& scenes = data->object[8].second;
    if (scenes.kind != JsonValue::Kind::Array || !scenes.array.empty())
        return fail();
    const auto scenes_comma = source.find(',', data->object[7].second.end);
    if (scenes_comma == std::string_view::npos || scenes_comma >= scenes.begin)
        return fail();
    std::array edits{RawEdit{scenes_comma, scenes.end, {}},
                     RawEdit{version->begin, version->end, "4"}};
    return finish(output, apply_edits(source, edits, output));
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_sequence_v5_to_v6(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return fail();
    auto root = parsed.value()->root();
    auto* data = member(root, "data");
    auto* version = member(root, "version");
    if (!data || !version || !version_is(*version, 5) || !has_exact_members(*data, v5_members) ||
        version->begin >= version->end)
        return fail();
    // An empty order records no authored order, so the upgraded document adopts
    // the identity order of its track list -- exactly what a v5 reader saw.
    const auto& scenes = data->object[8].second;
    if (scenes.begin >= scenes.end)
        return fail();
    std::array edits{RawEdit{scenes.end, scenes.end, ",\"track_order\":[]"},
                     RawEdit{version->begin, version->end, "6"}};
    return finish(output, apply_edits(source, edits, output));
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_sequence_v6_to_v5(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return fail();
    auto root = parsed.value()->root();
    auto* data = member(root, "data");
    auto* version = member(root, "version");
    if (!data || !version || !version_is(*version, 6) || !has_exact_members(*data, v6_members) ||
        version->begin >= version->end)
        return fail();
    const auto& track_order = data->object[9].second;
    if (!states_identity_track_order(track_order, data->object[10].second))
        return fail();
    const auto order_comma = source.find(',', data->object[8].second.end);
    if (order_comma == std::string_view::npos || order_comma >= track_order.begin)
        return fail();
    std::array edits{RawEdit{order_comma, track_order.end, {}},
                     RawEdit{version->begin, version->end, "5"}};
    return finish(output, apply_edits(source, edits, output));
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_sequence_v6_to_v7(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return fail();
    auto root = parsed.value()->root();
    auto* data = member(root, "data");
    auto* version = member(root, "version");
    if (!data || !version || !version_is(*version, 6) || !has_exact_members(*data, v6_members) ||
        version->begin >= version->end)
        return fail();
    const auto& lane = data->object[1].second;
    const auto& regions = data->object[7].second;
    if (lane.kind != JsonValue::Kind::Array || regions.kind != JsonValue::Kind::Array)
        return fail();
    std::vector<RawEdit> edits;
    edits.reserve(lane.array.size() * 2 + regions.array.size() + 1);
    edits.push_back(RawEdit{version->begin, version->end, "7"});
    // Every event gains the same defaults, which is what makes the upgraded
    // lane state exactly the harmony the v6 lane stated.
    for (const auto& event : lane.array) {
        if (!has_exact_members(event, v6_chord_event_members) || event.end <= event.begin + 1)
            return fail();
        edits.push_back(RawEdit{event.begin + 1, event.begin + 1, kChordDetailPrefix});
        edits.push_back(RawEdit{event.end - 1, event.end - 1, kChordDetailSuffix});
    }
    // A region that stated no part upgrades to one that states Unspecified,
    // which is the same claim.
    for (const auto& region : regions.array) {
        const auto* region_body = region_data(region);
        if (!region_body || !canonical_region_data(*region_body, false) ||
            region_body->end <= region_body->begin + 1)
            return fail();
        edits.push_back(
            RawEdit{region_body->end - 1, region_body->end - 1, kDefaultSectionRole});
    }
    return finish(output, apply_edits(source, edits, output));
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
migrate_sequence_v7_to_v6(std::string_view source, BoundedJsonSink& output, const void*) noexcept {
    auto parsed = parse_json(source);
    if (!parsed)
        return fail();
    auto root = parsed.value()->root();
    auto* data = member(root, "data");
    auto* version = member(root, "version");
    if (!data || !version || !version_is(*version, 7) || !has_exact_members(*data, v7_members) ||
        version->begin >= version->end)
        return fail();
    const auto& lane = data->object[1].second;
    const auto& regions = data->object[7].second;
    if (lane.kind != JsonValue::Kind::Array || regions.kind != JsonValue::Kind::Array)
        return fail();
    std::vector<RawEdit> edits;
    edits.reserve(lane.array.size() * 2 + regions.array.size() + 1);
    edits.push_back(RawEdit{version->begin, version->end, "6"});
    for (const auto& event : lane.array) {
        // Refuse rather than drop: a v6 reader that lost a slash bass or an
        // added ninth would state a different chord, not a less annotated one.
        if (!states_no_chord_detail(event))
            return fail();
        const auto quality_key = source.find("\"chord_quality\"", event.object[1].second.end);
        const auto voicing_comma = source.find(',', event.object[6].second.end);
        const auto& voicing = event.object[7].second;
        if (quality_key == std::string_view::npos || quality_key <= event.begin ||
            voicing_comma == std::string_view::npos || voicing_comma >= voicing.begin)
            return fail();
        edits.push_back(RawEdit{event.begin + 1, quality_key, {}});
        edits.push_back(RawEdit{voicing_comma, voicing.end, {}});
    }
    for (const auto& region : regions.array) {
        // Drop rather than refuse: the region survives with the same identity,
        // name, position, and span, so a v6 reader sees the same span of music
        // with one fewer label on it.
        const auto* region_body = region_data(region);
        if (!region_body || !canonical_region_data(*region_body, true))
            return fail();
        const auto& position = region_body->object[region_body->object.size() - 2].second;
        const auto& role = region_body->object.back().second;
        const auto role_comma = source.find(',', position.end);
        if (role_comma == std::string_view::npos || role_comma >= role.begin)
            return fail();
        edits.push_back(RawEdit{role_comma, role.end, {}});
    }
    return finish(output, apply_edits(source, edits, output));
}

} // namespace pulp::timeline::detail
