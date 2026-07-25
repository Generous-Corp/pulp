#include <pulp/audio/sample_bank.hpp>

#include <pulp/runtime/crypto.hpp>

#include <choc/text/choc_JSON.h>
#include <choc/text/choc_UTF8.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace pulp::audio {
namespace {

using Value = choc::value::ValueView;
using namespace std::string_view_literals;

struct Parser {
    SampleBankParseResult result;

    bool fail(SampleBankStatus status, std::string path) {
        result.status = status;
        result.field_path = std::move(path);
        return false;
    }

    template <std::size_t N>
    bool audit(Value object, std::string_view path, const std::array<std::string_view, N>& allowed,
               std::span<const std::string_view> required = {}) {
        if (!object.isObject())
            return fail(SampleBankStatus::wrong_type, std::string(path));
        for (std::uint32_t i = 0; i < object.size(); ++i) {
            const auto member = object.getObjectMemberAt(i);
            if (std::find(allowed.begin(), allowed.end(), member.name) == allowed.end())
                return fail(SampleBankStatus::unknown_field,
                            std::string(path) + "." + std::string(member.name));
            for (std::uint32_t earlier = 0; earlier < i; ++earlier) {
                if (object.getObjectMemberAt(earlier).name == member.name)
                    return fail(SampleBankStatus::duplicate_field,
                                std::string(path) + "." + std::string(member.name));
            }
        }
        for (const auto field : required) {
            if (!object.hasObjectMember(field))
                return fail(SampleBankStatus::missing_field,
                            std::string(path) + "." + std::string(field));
        }
        return true;
    }

    bool text(Value object, std::string_view field, const std::string& path, std::string& out) {
        const auto value = object[field];
        if (!value.isString())
            return fail(SampleBankStatus::wrong_type, path);
        out = value.getString();
        return true;
    }

    template <typename T>
    bool integer(Value object, std::string_view field, const std::string& path,
                 std::int64_t minimum, std::int64_t maximum, T& out) {
        const auto value = object[field];
        if (!value.isInt())
            return fail(SampleBankStatus::wrong_type, path);
        const auto parsed = value.get<std::int64_t>();
        if (parsed < minimum || parsed > maximum)
            return fail(SampleBankStatus::invalid_zone, path);
        out = static_cast<T>(parsed);
        return true;
    }

    bool number(Value object, std::string_view field, const std::string& path, double& out) {
        const auto value = object[field];
        if (!value.isInt() && !value.isFloat())
            return fail(SampleBankStatus::wrong_type, path);
        out = value.isInt() ? static_cast<double>(value.get<std::int64_t>()) : value.get<double>();
        if (!std::isfinite(out))
            return fail(SampleBankStatus::invalid_zone, path);
        return true;
    }

    bool optional_integer(Value object, std::string_view field, const std::string& path,
                          std::int64_t minimum, std::int64_t maximum, auto& out) {
        return !object.hasObjectMember(field) ||
               integer(object, field, path, minimum, maximum, out);
    }

    bool optional_number(Value object, std::string_view field, const std::string& path,
                         double& out) {
        return !object.hasObjectMember(field) || number(object, field, path, out);
    }
};

bool safe_identifier(std::string_view value) {
    if (value.empty() || value == "." || value == "..")
        return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '_' || c == '-';
    });
}

bool safe_relative_path(std::string_view value) {
    if (value.empty() || value.front() == '/' || value.find('\\') != value.npos ||
        value.find(':') != value.npos || value.find('\0') != value.npos)
        return false;
    std::filesystem::path path(value);
    if (path.is_absolute())
        return false;
    for (const auto& component : path) {
        const auto text = component.string();
        if (text.empty() || text == "." || text == "..")
            return false;
    }
    return path.generic_string() == value;
}

/// choc's JSON parser rejects malformed UTF-8 outright, but the writer passes
/// bytes >= 0x20 through unescaped. Without this check the writer can emit a
/// manifest its own parser refuses to read.
bool valid_utf8(std::string_view value) {
    return choc::text::findInvalidUTF8Data(value.data(), value.size()) == nullptr;
}

/// `std::to_chars` switches to scientific notation for large magnitudes and
/// emits a `+` in the exponent, which choc's number parser does not accept.
/// Bounding the value keeps canonical output readable by the parser.
bool serializable_number(double value) {
    return std::isfinite(value) && std::abs(value) < 1e15;
}

bool valid_hash(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

bool valid_extensions_json(std::string_view json) {
    try {
        const auto owned = choc::json::parse(json);
        const Value object = owned;
        if (!object.isObject())
            return false;
        for (std::uint32_t i = 0; i < object.size(); ++i) {
            const auto member = object.getObjectMemberAt(i);
            const auto name = std::string_view(member.name);
            if (name.empty() || name.front() == '.' || name.back() == '.' ||
                name.find('.') == name.npos || name.find("..") != name.npos)
                return false;
            for (std::uint32_t earlier = 0; earlier < i; ++earlier) {
                if (object.getObjectMemberAt(earlier).name == member.name)
                    return false;
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::string canonical_extensions_json(std::string_view json) {
    const auto owned = choc::json::parse(json);
    return choc::json::toString(Value(owned), false);
}

SampleBankStatus validate_manifest(const SampleBankManifest& manifest, std::string& field_path) {
    if (manifest.schema != kSampleBankSchema) {
        field_path = "$.schema";
        return SampleBankStatus::unsupported_schema;
    }
    if (manifest.samples.size() > kSampleBankMaxSamples) {
        field_path = "$.samples";
        return SampleBankStatus::resource_limit_exceeded;
    }
    if (manifest.zones.size() > kSampleBankMaxZones) {
        field_path = "$.zones";
        return SampleBankStatus::resource_limit_exceeded;
    }
    if (!safe_identifier(manifest.id)) {
        field_path = "$.id";
        return SampleBankStatus::invalid_identifier;
    }
    if (manifest.name.empty() || !valid_utf8(manifest.name)) {
        field_path = "$.name";
        return SampleBankStatus::invalid_identifier;
    }
    if (!valid_extensions_json(manifest.extensions_json)) {
        field_path = "$.extensions";
        return SampleBankStatus::wrong_type;
    }
    std::unordered_set<std::uint32_t> ids;
    std::unordered_set<std::string> paths;
    for (std::size_t i = 0; i < manifest.samples.size(); ++i) {
        const auto base = "$.samples[" + std::to_string(i) + "]";
        const auto& sample = manifest.samples[i];
        if (sample.id == 0 || sample.id == kInvalidSampleId) {
            field_path = base + ".id";
            return SampleBankStatus::invalid_sample_id;
        }
        if (!ids.insert(sample.id).second) {
            field_path = base + ".id";
            return SampleBankStatus::duplicate_sample_id;
        }
        if (!safe_relative_path(sample.path) || !valid_utf8(sample.path)) {
            field_path = base + ".path";
            return SampleBankStatus::invalid_path;
        }
        if (!valid_utf8(sample.license_ref)) {
            field_path = base + ".license_ref";
            return SampleBankStatus::invalid_identifier;
        }
        if (!paths.insert(sample.path).second) {
            field_path = base + ".path";
            return SampleBankStatus::duplicate_sample_path;
        }
        if (!valid_hash(sample.sha256)) {
            field_path = base + ".sha256";
            return SampleBankStatus::invalid_hash;
        }
        if (!valid_extensions_json(sample.extensions_json)) {
            field_path = base + ".extensions";
            return SampleBankStatus::wrong_type;
        }
    }
    for (std::size_t i = 0; i < manifest.zones.size(); ++i) {
        const auto& bank_zone = manifest.zones[i];
        const auto& zone = bank_zone.zone;
        const auto base = "$.zones[" + std::to_string(i) + "]";
        if (!ids.contains(zone.sample_id)) {
            field_path = base + ".sample_id";
            return SampleBankStatus::missing_sample_reference;
        }
        if (zone.root_note < 0 || zone.root_note > 127 ||
            zone.lowest_note < 0 || zone.lowest_note > 127 ||
            zone.highest_note < 0 || zone.highest_note > 127 ||
            zone.lowest_velocity < 1 || zone.lowest_velocity > 127 ||
            zone.highest_velocity < 1 || zone.highest_velocity > 127 ||
            zone.lowest_note > zone.highest_note ||
            zone.lowest_velocity > zone.highest_velocity ||
            !serializable_number(zone.keytrack_cents_per_key) ||
            !serializable_number(zone.tune_semitones)) {
            field_path = base;
            return SampleBankStatus::invalid_zone;
        }
        if (zone.slice_index != kNoSampleSliceIndex &&
            zone.slice_region.start_frame >= zone.slice_region.end_frame) {
            field_path = base + ".slice";
            return SampleBankStatus::invalid_zone;
        }
        if (zone.has_loop && zone.loop.start_frame >= zone.loop.end_frame) {
            field_path = base + ".loop";
            return SampleBankStatus::invalid_zone;
        }
        // v1 serializes only the slice and loop frame ranges. Rejecting the
        // richer in-memory fields keeps the format honest: a manifest that
        // cannot be written back unchanged is refused rather than silently
        // flattened. Carry this metadata in a namespaced `extensions` key, or
        // widen the serialized set in a later schema version.
        if (zone.slice_index != kNoSampleSliceIndex && zone.slice_region.marker_index != 0) {
            field_path = base + ".slice_marker_index";
            return SampleBankStatus::invalid_zone;
        }
        if (zone.has_loop &&
            (zone.loop.crossfade_frames != 0 ||
             zone.loop.playback_mode != LoopPlaybackMode::Forward ||
             zone.loop.crossfade_curve != LoopCrossfadeCurve::Linear ||
             zone.loop.interpolation != LoopInterpolationMode::Linear ||
             zone.loop.snap_policy != LoopSnapPolicy::ValueDirection ||
             zone.loop.reverse_entry)) {
            field_path = base + ".loop_playback";
            return SampleBankStatus::invalid_zone;
        }
        if (!valid_extensions_json(bank_zone.extensions_json)) {
            field_path = base + ".extensions";
            return SampleBankStatus::wrong_type;
        }
    }
    return SampleBankStatus::ok;
}

void append_escaped(std::string& out, std::string_view value) {
    out.push_back('"');
    for (const unsigned char c : value) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                constexpr char hex[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 0xf]);
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
}

void append_number(std::string& out, double value) {
    std::array<char, 64> text{};
    const auto converted =
        std::to_chars(text.data(), text.data() + text.size(), value, std::chars_format::general,
                      std::numeric_limits<double>::max_digits10);
    out.append(text.data(), converted.ptr);
}

} // namespace

const char* sample_bank_status_name(SampleBankStatus status) noexcept {
    switch (status) {
    case SampleBankStatus::ok:
        return "ok";
    case SampleBankStatus::invalid_json:
        return "invalid_json";
    case SampleBankStatus::root_not_object:
        return "root_not_object";
    case SampleBankStatus::unknown_field:
        return "unknown_field";
    case SampleBankStatus::duplicate_field:
        return "duplicate_field";
    case SampleBankStatus::missing_field:
        return "missing_field";
    case SampleBankStatus::wrong_type:
        return "wrong_type";
    case SampleBankStatus::unsupported_schema:
        return "unsupported_schema";
    case SampleBankStatus::invalid_identifier:
        return "invalid_identifier";
    case SampleBankStatus::invalid_sample_id:
        return "invalid_sample_id";
    case SampleBankStatus::invalid_path:
        return "invalid_path";
    case SampleBankStatus::invalid_hash:
        return "invalid_hash";
    case SampleBankStatus::duplicate_sample_id:
        return "duplicate_sample_id";
    case SampleBankStatus::duplicate_sample_path:
        return "duplicate_sample_path";
    case SampleBankStatus::invalid_zone:
        return "invalid_zone";
    case SampleBankStatus::missing_sample_reference:
        return "missing_sample_reference";
    case SampleBankStatus::filesystem_error:
        return "filesystem_error";
    case SampleBankStatus::path_escape:
        return "path_escape";
    case SampleBankStatus::hash_mismatch:
        return "hash_mismatch";
    case SampleBankStatus::import_failed:
        return "import_failed";
    case SampleBankStatus::preparation_failed:
        return "preparation_failed";
    case SampleBankStatus::resource_limit_exceeded:
        return "resource_limit_exceeded";
    case SampleBankStatus::unsupported_materialization_policy:
        return "unsupported_materialization_policy";
    }
    return "unknown";
}

SampleBankStatus validate_sample_bank_manifest(const SampleBankManifest& manifest,
                                               std::string& field_path) {
    return validate_manifest(manifest, field_path);
}

SampleBankParseResult parse_sample_bank_json(std::string_view json) {
    Parser parser;
    if (json.size() > kSampleBankMaxJsonBytes) {
        parser.fail(SampleBankStatus::resource_limit_exceeded, "$");
        return std::move(parser.result);
    }
    try {
        const auto owned = choc::json::parse(json);
        const Value root = owned;
        if (!root.isObject()) {
            parser.fail(SampleBankStatus::root_not_object, "$");
            return std::move(parser.result);
        }
        constexpr std::array root_fields{"schema"sv,  "id"sv,    "name"sv,
                                         "samples"sv, "zones"sv, "extensions"sv};
        constexpr std::array root_required{"schema"sv, "id"sv, "name"sv, "samples"sv, "zones"sv};
        if (!parser.audit(root, "$", root_fields, root_required))
            return std::move(parser.result);
        if (!parser.text(root, "schema", "$.schema", parser.result.manifest.schema) ||
            !parser.text(root, "id", "$.id", parser.result.manifest.id) ||
            !parser.text(root, "name", "$.name", parser.result.manifest.name))
            return std::move(parser.result);
        if (root.hasObjectMember("extensions")) {
            if (!root["extensions"].isObject()) {
                parser.fail(SampleBankStatus::wrong_type, "$.extensions");
                return std::move(parser.result);
            }
            parser.result.manifest.extensions_json =
                choc::json::toString(root["extensions"], false);
        }
        if (!root["samples"].isArray()) {
            parser.fail(SampleBankStatus::wrong_type, "$.samples");
            return std::move(parser.result);
        }
        const auto samples = root["samples"];
        if (samples.size() > kSampleBankMaxSamples) {
            parser.fail(SampleBankStatus::resource_limit_exceeded, "$.samples");
            return std::move(parser.result);
        }
        parser.result.manifest.samples.resize(samples.size());
        for (std::uint32_t i = 0; i < samples.size(); ++i) {
            const auto base = "$.samples[" + std::to_string(i) + "]";
            const auto value = samples[i];
            constexpr std::array fields{"id"sv, "path"sv, "sha256"sv, "license_ref"sv,
                                        "extensions"sv};
            constexpr std::array required{"id"sv, "path"sv, "sha256"sv};
            auto& sample = parser.result.manifest.samples[i];
            if (!parser.audit(value, base, fields, required) ||
                !parser.integer(value, "id", base + ".id", 0,
                                std::numeric_limits<std::uint32_t>::max(), sample.id) ||
                !parser.text(value, "path", base + ".path", sample.path) ||
                !parser.text(value, "sha256", base + ".sha256", sample.sha256))
                return std::move(parser.result);
            if (value.hasObjectMember("license_ref") &&
                !parser.text(value, "license_ref", base + ".license_ref", sample.license_ref))
                return std::move(parser.result);
            if (value.hasObjectMember("extensions")) {
                if (!value["extensions"].isObject()) {
                    parser.fail(SampleBankStatus::wrong_type, base + ".extensions");
                    return std::move(parser.result);
                }
                sample.extensions_json = choc::json::toString(value["extensions"], false);
            }
        }
        if (!root["zones"].isArray()) {
            parser.fail(SampleBankStatus::wrong_type, "$.zones");
            return std::move(parser.result);
        }
        const auto zones = root["zones"];
        if (zones.size() > kSampleBankMaxZones) {
            parser.fail(SampleBankStatus::resource_limit_exceeded, "$.zones");
            return std::move(parser.result);
        }
        parser.result.manifest.zones.resize(zones.size());
        for (std::uint32_t i = 0; i < zones.size(); ++i) {
            const auto base = "$.zones[" + std::to_string(i) + "]";
            const auto value = zones[i];
            constexpr std::array fields{"sample_id"sv,
                                        "root_note"sv,
                                        "lowest_note"sv,
                                        "highest_note"sv,
                                        "lowest_velocity"sv,
                                        "highest_velocity"sv,
                                        "keytrack_cents_per_key"sv,
                                        "tune_semitones"sv,
                                        "priority"sv,
                                        "round_robin_group"sv,
                                        "voice_group"sv,
                                        "choke_group"sv,
                                        "slice_index"sv,
                                        "slice_start_frame"sv,
                                        "slice_end_frame"sv,
                                        "loop_start_frame"sv,
                                        "loop_end_frame"sv,
                                        "extensions"sv};
            constexpr std::array required{"sample_id"sv};
            auto& bank_zone = parser.result.manifest.zones[i];
            auto& zone = bank_zone.zone;
            if (!parser.audit(value, base, fields, required) ||
                !parser.integer(value, "sample_id", base + ".sample_id", 0,
                                std::numeric_limits<std::uint32_t>::max(), zone.sample_id) ||
                !parser.optional_integer(value, "root_note", base + ".root_note", 0, 127,
                                         zone.root_note) ||
                !parser.optional_integer(value, "lowest_note", base + ".lowest_note", 0, 127,
                                         zone.lowest_note) ||
                !parser.optional_integer(value, "highest_note", base + ".highest_note", 0, 127,
                                         zone.highest_note) ||
                !parser.optional_integer(value, "lowest_velocity", base + ".lowest_velocity", 1,
                                         127, zone.lowest_velocity) ||
                !parser.optional_integer(value, "highest_velocity", base + ".highest_velocity", 1,
                                         127, zone.highest_velocity) ||
                !parser.optional_number(value, "keytrack_cents_per_key",
                                        base + ".keytrack_cents_per_key",
                                        zone.keytrack_cents_per_key) ||
                !parser.optional_number(value, "tune_semitones", base + ".tune_semitones",
                                        zone.tune_semitones) ||
                !parser.optional_integer(value, "priority", base + ".priority", 0,
                                         std::numeric_limits<std::uint32_t>::max(),
                                         zone.priority) ||
                !parser.optional_integer(value, "round_robin_group", base + ".round_robin_group", 0,
                                         std::numeric_limits<std::uint32_t>::max(),
                                         zone.round_robin_group) ||
                !parser.optional_integer(value, "voice_group", base + ".voice_group", 0,
                                         std::numeric_limits<std::uint32_t>::max(),
                                         zone.voice_group) ||
                !parser.optional_integer(value, "choke_group", base + ".choke_group", 0,
                                         std::numeric_limits<std::uint32_t>::max(),
                                         zone.choke_group))
                return std::move(parser.result);
            const bool any_slice = value.hasObjectMember("slice_index") ||
                                   value.hasObjectMember("slice_start_frame") ||
                                   value.hasObjectMember("slice_end_frame");
            const bool all_slice = value.hasObjectMember("slice_index") &&
                                   value.hasObjectMember("slice_start_frame") &&
                                   value.hasObjectMember("slice_end_frame");
            if (any_slice != all_slice) {
                parser.fail(SampleBankStatus::missing_field, base + ".slice");
                return std::move(parser.result);
            }
            if (all_slice &&
                // kNoSampleSliceIndex is the in-memory "no slice" marker, and
                // the writer omits the slice triple for it. Accepting it here
                // would let a manifest round-trip to different JSON.
                (!parser.integer(value, "slice_index", base + ".slice_index", 0,
                                 kNoSampleSliceIndex - 1, zone.slice_index) ||
                 !parser.integer(value, "slice_start_frame", base + ".slice_start_frame", 0,
                                 std::numeric_limits<std::int64_t>::max(),
                                 zone.slice_region.start_frame) ||
                 !parser.integer(value, "slice_end_frame", base + ".slice_end_frame", 0,
                                 std::numeric_limits<std::int64_t>::max(),
                                 zone.slice_region.end_frame)))
                return std::move(parser.result);
            const bool any_loop = value.hasObjectMember("loop_start_frame") ||
                                  value.hasObjectMember("loop_end_frame");
            const bool all_loop = value.hasObjectMember("loop_start_frame") &&
                                  value.hasObjectMember("loop_end_frame");
            if (any_loop != all_loop) {
                parser.fail(SampleBankStatus::missing_field, base + ".loop");
                return std::move(parser.result);
            }
            if (all_loop) {
                zone.has_loop = true;
                if (!parser.integer(value, "loop_start_frame", base + ".loop_start_frame", 0,
                                    std::numeric_limits<std::int64_t>::max(),
                                    zone.loop.start_frame) ||
                    !parser.integer(value, "loop_end_frame", base + ".loop_end_frame", 0,
                                    std::numeric_limits<std::int64_t>::max(), zone.loop.end_frame))
                    return std::move(parser.result);
            }
            if (value.hasObjectMember("extensions")) {
                if (!value["extensions"].isObject()) {
                    parser.fail(SampleBankStatus::wrong_type, base + ".extensions");
                    return std::move(parser.result);
                }
                bank_zone.extensions_json = choc::json::toString(value["extensions"], false);
            }
        }
    } catch (const std::exception& error) {
        const auto message = std::string_view(error.what());
        parser.fail(message.find("already contains a member") != message.npos
                        ? SampleBankStatus::duplicate_field
                        : SampleBankStatus::invalid_json,
                    "$");
        return std::move(parser.result);
    } catch (...) {
        parser.fail(SampleBankStatus::invalid_json, "$");
        return std::move(parser.result);
    }
    parser.result.status = validate_manifest(parser.result.manifest, parser.result.field_path);
    return std::move(parser.result);
}

SampleBankWriteResult write_sample_bank_json(const SampleBankManifest& manifest) {
    SampleBankWriteResult result;
    result.status = validate_sample_bank_manifest(manifest, result.field_path);
    if (result.status != SampleBankStatus::ok)
        return result;
    auto& out = result.json;
    out = "{\"schema\":";
    append_escaped(out, manifest.schema);
    out += ",\"id\":";
    append_escaped(out, manifest.id);
    out += ",\"name\":";
    append_escaped(out, manifest.name);
    if (manifest.extensions_json != "{}")
        out += ",\"extensions\":" + canonical_extensions_json(manifest.extensions_json);
    out += ",\"samples\":[";
    for (std::size_t i = 0; i < manifest.samples.size(); ++i) {
        if (i != 0)
            out.push_back(',');
        const auto& sample = manifest.samples[i];
        out += "{\"id\":" + std::to_string(sample.id) + ",\"path\":";
        append_escaped(out, sample.path);
        out += ",\"sha256\":";
        append_escaped(out, sample.sha256);
        if (!sample.license_ref.empty()) {
            out += ",\"license_ref\":";
            append_escaped(out, sample.license_ref);
        }
        if (sample.extensions_json != "{}")
            out += ",\"extensions\":" + canonical_extensions_json(sample.extensions_json);
        out.push_back('}');
    }
    out += "],\"zones\":[";
    for (std::size_t i = 0; i < manifest.zones.size(); ++i) {
        if (i != 0)
            out.push_back(',');
        const auto& bank_zone = manifest.zones[i];
        const auto& zone = bank_zone.zone;
        out += "{\"sample_id\":" + std::to_string(zone.sample_id);
        out += ",\"root_note\":" + std::to_string(zone.root_note);
        out += ",\"lowest_note\":" + std::to_string(zone.lowest_note);
        out += ",\"highest_note\":" + std::to_string(zone.highest_note);
        out += ",\"lowest_velocity\":" + std::to_string(zone.lowest_velocity);
        out += ",\"highest_velocity\":" + std::to_string(zone.highest_velocity);
        out += ",\"keytrack_cents_per_key\":";
        append_number(out, zone.keytrack_cents_per_key);
        out += ",\"tune_semitones\":";
        append_number(out, zone.tune_semitones);
        out += ",\"priority\":" + std::to_string(zone.priority);
        out += ",\"round_robin_group\":" + std::to_string(zone.round_robin_group);
        out += ",\"voice_group\":" + std::to_string(zone.voice_group);
        out += ",\"choke_group\":" + std::to_string(zone.choke_group);
        if (zone.slice_index != kNoSampleSliceIndex) {
            out += ",\"slice_index\":" + std::to_string(zone.slice_index);
            out += ",\"slice_start_frame\":" + std::to_string(zone.slice_region.start_frame);
            out += ",\"slice_end_frame\":" + std::to_string(zone.slice_region.end_frame);
        }
        if (zone.has_loop) {
            out += ",\"loop_start_frame\":" + std::to_string(zone.loop.start_frame);
            out += ",\"loop_end_frame\":" + std::to_string(zone.loop.end_frame);
        }
        if (bank_zone.extensions_json != "{}")
            out += ",\"extensions\":" + canonical_extensions_json(bank_zone.extensions_json);
        out.push_back('}');
    }
    out += "]}";
    return result;
}

SampleBankContentValidationResult validate_sample_bank_content(
    const std::filesystem::path& content_root,
    std::span<const std::string> exported_bank_paths,
    std::uint64_t max_encoded_bytes_per_sample) {
    SampleBankContentValidationResult result;
    namespace fs = std::filesystem;
    // A pack that exports no banks has nothing to verify. Returning before the
    // root check keeps packs that carry only presets/themes/samples valid even
    // when the caller has no content directory to point at.
    if (exported_bank_paths.empty()) return result;
    std::error_code error;
    const auto canonical_root = fs::weakly_canonical(content_root, error);
    auto add_issue = [&](SampleBankStatus status, std::string manifest_path,
                         std::string field_path) {
        result.issues.push_back(
            {status, std::move(manifest_path), std::move(field_path)});
    };
    if (error || !fs::is_directory(canonical_root)) {
        add_issue(SampleBankStatus::filesystem_error, {}, "$");
        return result;
    }
    auto within_root = [&](const fs::path& path) {
        const auto normalized_path = path.lexically_normal();
        const auto normalized_root = canonical_root.lexically_normal();
        auto path_it = normalized_path.begin();
        const auto path_end = normalized_path.end();
        for (auto root_it = normalized_root.begin(); root_it != normalized_root.end();
             ++root_it, ++path_it) {
            if (path_it == path_end || *path_it != *root_it)
                return false;
        }
        return true;
    };
    auto contains_symlink = [&](const fs::path& relative) {
        auto current = canonical_root;
        for (const auto& component : relative) {
            current /= component;
            const auto status = fs::symlink_status(current, error);
            if (error) {
                // A component that simply does not exist is not a symlink; the
                // caller's existence check reports it as a filesystem error.
                // Any other failure (permissions, IO) stays suspicious.
                const bool missing = error == std::errc::no_such_file_or_directory;
                error.clear();
                return !missing;
            }
            if (fs::is_symlink(status)) return true;
        }
        return false;
    };

    // Bank manifests are named `*.bank` or `*.bank.json`. A directory export
    // may also carry the audio it references plus ordinary companion files,
    // and those must not be parsed as manifests. An explicitly exported file
    // is always treated as a manifest so a misnamed one still reports a parse
    // error rather than being silently ignored.
    auto is_bank_manifest = [](const fs::path& path) {
        const auto name = path.filename().string();
        const auto has_suffix = [&name](std::string_view suffix) {
            return name.size() > suffix.size() &&
                   std::string_view(name).substr(name.size() - suffix.size()) == suffix;
        };
        return has_suffix(".bank") || has_suffix(".bank.json");
    };

    std::vector<fs::path> manifests;
    for (const auto& exported : exported_bank_paths) {
        if (!safe_relative_path(exported)) {
            add_issue(SampleBankStatus::invalid_path, exported, "$exports");
            continue;
        }
        const fs::path relative(exported);
        if (contains_symlink(relative)) {
            add_issue(SampleBankStatus::path_escape, exported, "$exports");
            continue;
        }
        const auto path = fs::weakly_canonical(canonical_root / relative, error);
        if (error || !within_root(path)) {
            add_issue(SampleBankStatus::path_escape, exported, "$exports");
            error.clear();
            continue;
        }
        if (fs::is_directory(path, error)) {
            for (fs::recursive_directory_iterator it(path, error), end;
                 !error && it != end; it.increment(error)) {
                const auto entry_relative =
                    fs::relative(it->path(), canonical_root, error);
                if (error || contains_symlink(entry_relative)) {
                    add_issue(SampleBankStatus::path_escape,
                              entry_relative.generic_string(), "$exports");
                    error.clear();
                    continue;
                }
                if (it->is_regular_file(error) && is_bank_manifest(it->path()))
                    manifests.push_back(it->path());
            }
            if (error) {
                add_issue(SampleBankStatus::filesystem_error, exported,
                          "$exports");
                error.clear();
            }
        } else if (fs::is_regular_file(path, error)) {
            manifests.push_back(path);
        } else {
            add_issue(SampleBankStatus::filesystem_error, exported, "$exports");
        }
        error.clear();
    }

    std::sort(manifests.begin(), manifests.end());
    for (const auto& path : manifests) {
        const auto relative_manifest =
            path.lexically_relative(canonical_root).generic_string();
        result.manifest_paths.push_back(relative_manifest);
        const auto size = fs::file_size(path, error);
        if (error || size > kSampleBankMaxJsonBytes) {
            add_issue(error ? SampleBankStatus::filesystem_error
                            : SampleBankStatus::resource_limit_exceeded,
                      relative_manifest, "$");
            error.clear();
            continue;
        }
        std::ifstream input(path, std::ios::binary);
        std::string text(static_cast<std::size_t>(size), '\0');
        input.read(text.data(), static_cast<std::streamsize>(size));
        if (!input) {
            add_issue(SampleBankStatus::filesystem_error, relative_manifest, "$");
            continue;
        }
        const auto parsed = parse_sample_bank_json(text);
        if (!parsed.valid()) {
            add_issue(parsed.status, relative_manifest, parsed.field_path);
            continue;
        }
        for (const auto& sample : parsed.manifest.samples) {
            const fs::path sample_relative(sample.path);
            if (contains_symlink(sample_relative)) {
                add_issue(SampleBankStatus::path_escape, relative_manifest,
                          sample.path);
                continue;
            }
            const auto sample_path =
                fs::weakly_canonical(canonical_root / sample_relative, error);
            if (error || !within_root(sample_path)) {
                // Confinement is decided before existence so an escaping path
                // is never softened into a missing-file report.
                add_issue(error ? SampleBankStatus::filesystem_error
                                : SampleBankStatus::path_escape,
                          relative_manifest, sample.path);
                error.clear();
                continue;
            }
            if (!fs::is_regular_file(sample_path)) {
                add_issue(SampleBankStatus::filesystem_error, relative_manifest,
                          sample.path);
                continue;
            }
            const auto hash = runtime::sha256_file_hex(
                sample_path, max_encoded_bytes_per_sample);
            if (!hash) {
                add_issue(SampleBankStatus::resource_limit_exceeded,
                          relative_manifest, sample.path);
                continue;
            }
            if (*hash != sample.sha256) {
                add_issue(SampleBankStatus::hash_mismatch, relative_manifest,
                          sample.path);
                continue;
            }
            if (std::find(result.sample_paths.begin(), result.sample_paths.end(),
                          sample.path) == result.sample_paths.end())
                result.sample_paths.push_back(sample.path);
        }
    }
    return result;
}

} // namespace pulp::audio
