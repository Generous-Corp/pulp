#include <pulp/timeline/serialize.hpp>

#include "project_state_access.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <limits>

namespace pulp::timeline {
namespace {

template <typename T>
runtime::Result<T, PersistenceError> fail(PersistenceErrorCode code, std::string path = {},
                                          std::size_t byte_offset = 0, std::uint64_t actual = 0,
                                          std::uint64_t limit = 0) {
    return runtime::Result<T, PersistenceError>(runtime::Err(
        PersistenceError{code, byte_offset, actual, limit, std::move(path), std::nullopt}));
}

template <typename T>
runtime::Result<T, PersistenceError> model_fail(ModelError error, std::string path) {
    PersistenceError failure;
    failure.code = PersistenceErrorCode::ModelRejected;
    failure.path = std::move(path);
    failure.model_error = error;
    return runtime::Result<T, PersistenceError>(runtime::Err(std::move(failure)));
}

class JsonWriter {
  public:
    explicit JsonWriter(std::size_t maximum) : maximum_(maximum) {}

    bool append(std::string_view text) {
        if (failed_)
            return false;
        if (text.size() > maximum_ - std::min(maximum_, output_.size())) {
            failed_ = true;
            const auto room = std::numeric_limits<std::uint64_t>::max() - output_.size();
            actual_ = text.size() > room ? std::numeric_limits<std::uint64_t>::max()
                                         : output_.size() + text.size();
            return false;
        }
        output_.append(text);
        return true;
    }

    bool character(char value) {
        return append(std::string_view(&value, 1));
    }

    bool quoted(std::string_view value) {
        constexpr char digits[] = "0123456789abcdef";
        if (!character('"'))
            return false;
        for (const auto raw : value) {
            const auto byte = static_cast<unsigned char>(raw);
            switch (raw) {
            case '"':
                if (!append("\\\""))
                    return false;
                break;
            case '\\':
                if (!append("\\\\"))
                    return false;
                break;
            case '\b':
                if (!append("\\b"))
                    return false;
                break;
            case '\f':
                if (!append("\\f"))
                    return false;
                break;
            case '\n':
                if (!append("\\n"))
                    return false;
                break;
            case '\r':
                if (!append("\\r"))
                    return false;
                break;
            case '\t':
                if (!append("\\t"))
                    return false;
                break;
            default:
                if (byte < 0x20) {
                    char escaped[] = {'\\', 'u', '0', '0', digits[byte >> 4], digits[byte & 0x0f]};
                    if (!append(std::string_view(escaped, sizeof(escaped))))
                        return false;
                } else if (!character(raw)) {
                    return false;
                }
            }
        }
        return character('"');
    }

    bool u64(std::uint64_t value, bool quoted_value = false) {
        char buffer[32];
        const auto encoded = std::to_chars(buffer, buffer + sizeof(buffer), value);
        return (!quoted_value || character('"')) &&
               append(std::string_view(buffer, encoded.ptr - buffer)) &&
               (!quoted_value || character('"'));
    }

    bool i64(std::int64_t value, bool quoted_value = false) {
        char buffer[32];
        const auto encoded = std::to_chars(buffer, buffer + sizeof(buffer), value);
        return (!quoted_value || character('"')) &&
               append(std::string_view(buffer, encoded.ptr - buffer)) &&
               (!quoted_value || character('"'));
    }

    bool failed() const noexcept {
        return failed_;
    }
    std::size_t remaining() const noexcept {
        return maximum_ - output_.size();
    }
    PersistenceError error() const {
        return PersistenceError{PersistenceErrorCode::OutputLimitExceeded, 0, actual_, maximum_,
                                "/"};
    }
    std::string take() {
        return std::move(output_);
    }

  private:
    std::size_t maximum_ = 0;
    std::string output_;
    bool failed_ = false;
    std::uint64_t actual_ = 0;
};

struct EncodeContext {
    JsonWriter writer;
    const SchemaRegistry& registry;
    bool opaque = false;
    std::optional<PersistenceError> failure;
};

template <typename DataFn>
bool write_envelope(EncodeContext& context, std::string_view type_name, std::uint32_t version,
                    DataFn&& write_data) {
    return context.writer.append("{\"data\":") && write_data() &&
           context.writer.append(",\"type_name\":") && context.writer.quoted(type_name) &&
           context.writer.append(",\"version\":") && context.writer.u64(version) &&
           context.writer.character('}');
}

const char* storage_name(AssetStoragePolicy value) noexcept {
    switch (value) {
    case AssetStoragePolicy::External:
        return "external";
    case AssetStoragePolicy::Embedded:
        return "embedded";
    case AssetStoragePolicy::PreferEmbedded:
        return "prefer_embedded";
    }
    return "external";
}

const char* locator_name(AssetLocatorKind value) noexcept {
    return value == AssetLocatorKind::PackageRelative ? "package_relative" : "external_uri";
}

const char* item_kind_name(ItemKind value) noexcept {
    switch (value) {
    case ItemKind::Project:
        return "project";
    case ItemKind::Asset:
        return "asset";
    case ItemKind::Sequence:
        return "sequence";
    case ItemKind::Track:
        return "track";
    case ItemKind::Clip:
        return "clip";
    case ItemKind::Note:
        return "note";
    }
    return "project";
}

runtime::Result<ItemKind, PersistenceError> decode_item_kind(std::string_view value,
                                                             std::string path) {
    if (value == "project")
        return runtime::Ok(ItemKind::Project);
    if (value == "asset")
        return runtime::Ok(ItemKind::Asset);
    if (value == "sequence")
        return runtime::Ok(ItemKind::Sequence);
    if (value == "track")
        return runtime::Ok(ItemKind::Track);
    if (value == "clip")
        return runtime::Ok(ItemKind::Clip);
    if (value == "note")
        return runtime::Ok(ItemKind::Note);
    return fail<ItemKind>(PersistenceErrorCode::InvalidSchema, std::move(path));
}

bool write_rate(EncodeContext& context, timebase::RationalRate rate) {
    return context.writer.append("{\"denominator\":") &&
           context.writer.u64(rate.denominator, true) && context.writer.append(",\"numerator\":") &&
           context.writer.u64(rate.numerator, true) && context.writer.character('}');
}

bool write_tempo_map(EncodeContext& context, const timebase::TempoMap& map) {
    if (!context.writer.character('['))
        return false;
    for (std::size_t index = 0; index < map.points().size(); ++index) {
        const auto& point = map.points()[index];
        if ((index != 0 && !context.writer.character(',')) ||
            !context.writer.append("{\"bpm_bits\":") ||
            !context.writer.u64(std::bit_cast<std::uint64_t>(point.bpm), true) ||
            !context.writer.append(",\"curve\":") ||
            !context.writer.quoted(point.curve_to_next == timebase::TempoCurve::LinearInTicks
                                       ? "linear_in_ticks"
                                       : "constant") ||
            !context.writer.append(",\"tick\":") ||
            !context.writer.i64(point.tick.value, true) || !context.writer.character('}'))
            return false;
    }
    return context.writer.character(']');
}

bool write_meter_map(EncodeContext& context, const timebase::MeterMap& map) {
    if (!context.writer.character('['))
        return false;
    for (std::size_t index = 0; index < map.points().size(); ++index) {
        const auto& point = map.points()[index];
        if ((index != 0 && !context.writer.character(',')) ||
            !context.writer.append("{\"denominator\":") ||
            !context.writer.u64(static_cast<std::uint32_t>(point.signature.denominator)) ||
            !context.writer.append(",\"numerator\":") ||
            !context.writer.u64(static_cast<std::uint32_t>(point.signature.numerator)) ||
            !context.writer.append(",\"tick\":") ||
            !context.writer.i64(point.tick.value, true) || !context.writer.character('}'))
            return false;
    }
    return context.writer.character(']');
}

bool write_locator(EncodeContext& context, const AssetLocator& locator) {
    return context.writer.append("{\"hint\":") && context.writer.quoted(locator.hint) &&
           context.writer.append(",\"kind\":") &&
           context.writer.quoted(locator_name(locator.kind)) && context.writer.character('}');
}

bool write_representation(EncodeContext& context, const AssetRepresentation& representation) {
    return write_envelope(context, "pulp.timeline.asset_representation", 1, [&] {
        if (!context.writer.append("{\"content_hash\":") ||
            !context.writer.quoted(representation.content_hash.to_hex()) ||
            !context.writer.append(",\"locators\":["))
            return false;
        for (std::size_t index = 0; index < representation.locators.size(); ++index) {
            if ((index != 0 && !context.writer.character(',')) ||
                !write_locator(context, representation.locators[index]))
                return false;
        }
        return context.writer.append("],\"role\":") && context.writer.quoted(representation.role) &&
               context.writer.append(",\"storage_policy\":") &&
               context.writer.quoted(storage_name(representation.storage_policy)) &&
               context.writer.character('}');
    });
}

bool write_asset(EncodeContext& context, const MediaAsset& asset) {
    return write_envelope(context, "pulp.timeline.asset", 1, [&] {
        if (!context.writer.append("{\"content_hash\":") ||
            !context.writer.quoted(asset.content_hash.to_hex()) ||
            !context.writer.append(",\"frame_count\":") ||
            !context.writer.u64(asset.frame_count, true) || !context.writer.append(",\"id\":") ||
            !context.writer.u64(asset.id.value, true) || !context.writer.append(",\"locators\":["))
            return false;
        for (std::size_t index = 0; index < asset.locators.size(); ++index) {
            if ((index != 0 && !context.writer.character(',')) ||
                !write_locator(context, asset.locators[index]))
                return false;
        }
        if (!context.writer.append("],\"name\":") || !context.writer.quoted(asset.name) ||
            !context.writer.append(",\"representations\":["))
            return false;
        for (std::size_t index = 0; index < asset.representations.size(); ++index) {
            if ((index != 0 && !context.writer.character(',')) ||
                !write_representation(context, asset.representations[index]))
                return false;
        }
        return context.writer.append("],\"sample_rate\":") &&
               write_rate(context, asset.sample_rate) &&
               context.writer.append(",\"storage_policy\":") &&
               context.writer.quoted(storage_name(asset.storage_policy)) &&
               context.writer.character('}');
    });
}

bool write_content(EncodeContext& context, const ClipContent& content) {
    if (std::holds_alternative<EmptyContent>(content))
        return write_envelope(context, "pulp.timeline.content.empty", 1,
                              [&] { return context.writer.append("{}"); });
    if (const auto* media = std::get_if<MediaRef>(&content)) {
        return write_envelope(context, "pulp.timeline.content.media", 1, [&] {
            return context.writer.append("{\"asset_id\":") &&
                   context.writer.u64(media->asset_id.value, true) &&
                   context.writer.append(",\"frame_count\":") &&
                   context.writer.u64(media->frame_count, true) &&
                   context.writer.append(",\"source_start\":") &&
                   context.writer.i64(media->source_start.value, true) &&
                   context.writer.character('}');
        });
    }
    if (const auto* note_content = std::get_if<NoteContent>(&content)) {
        return write_envelope(context, "pulp.timeline.content.notes", 1, [&] {
            if (!context.writer.append("{\"notes\":["))
                return false;
            for (std::size_t index = 0; index < note_content->notes().size(); ++index) {
                const auto& note = note_content->notes()[index];
                if ((index != 0 && !context.writer.character(',')) ||
                    !context.writer.append("{\"channel\":") || !context.writer.u64(note.channel) ||
                    !context.writer.append(",\"duration_ticks\":") ||
                    !context.writer.i64(note.duration.value, true) ||
                    !context.writer.append(",\"id\":") ||
                    !context.writer.u64(note.id.value, true) ||
                    !context.writer.append(",\"pitch\":") || !context.writer.u64(note.pitch) ||
                    !context.writer.append(",\"start_ticks\":") ||
                    !context.writer.i64(note.start.value, true) ||
                    !context.writer.append(",\"velocity\":") ||
                    !context.writer.u64(note.velocity) || !context.writer.character('}'))
                    return false;
            }
            return context.writer.append("]}");
        });
    }
    if (const auto* registered = std::get_if<RegisteredContent>(&content)) {
        return write_envelope(
            context, registered->schema().type_name, registered->schema().version,
            [&] { return context.writer.append(registered->canonical_payload_json()); });
    }
    const auto& unknown = std::get<OpaqueContent>(content);
    context.opaque = true;
    return context.writer.append(unknown.raw_json());
}

bool write_clip(EncodeContext& context, const Clip& clip) {
    return write_envelope(context, "pulp.timeline.clip", 1, [&] {
        const auto playback = clip.playback_properties();
        if (!context.writer.append("{\"content\":") || !write_content(context, clip.content()) ||
            !context.writer.append(",\"fade_in_duration\":") ||
            !context.writer.u64(playback.fade_in_duration, true) ||
            !context.writer.append(",\"fade_out_duration\":") ||
            !context.writer.u64(playback.fade_out_duration, true) ||
            !context.writer.append(",\"gain_linear_bits\":") ||
            !context.writer.u64(std::bit_cast<std::uint32_t>(playback.gain_linear), true) ||
            !context.writer.append(",\"id\":") || !context.writer.u64(clip.id().value, true) ||
            !context.writer.append(",\"time_range\":"))
            return false;
        if (clip.time_anchor() == ClipTimeAnchor::Musical)
            return context.writer.append("{\"duration_ticks\":") &&
                   context.writer.i64(clip.duration().value, true) &&
                   context.writer.append(",\"kind\":\"musical\",\"start_ticks\":") &&
                   context.writer.i64(clip.start().value, true) && context.writer.append("}}");
        return context.writer.append("{\"kind\":\"absolute\",\"sample_count\":") &&
               context.writer.u64(clip.absolute_duration_samples(), true) &&
               context.writer.append(",\"sample_rate\":") &&
               write_rate(context, clip.absolute_sample_rate()) &&
               context.writer.append(",\"start_sample\":") &&
               context.writer.i64(clip.absolute_start().value, true) && context.writer.append("}}");
    });
}

bool write_track(EncodeContext& context, const Track& track) {
    return write_envelope(context, "pulp.timeline.track", 1, [&] {
        if (!context.writer.append("{\"clips\":["))
            return false;
        for (std::size_t index = 0; index < track.clips().size(); ++index)
            if ((index != 0 && !context.writer.character(',')) ||
                !write_clip(context, track.clips()[index]))
                return false;
        return context.writer.append("],\"id\":") && context.writer.u64(track.id().value, true) &&
               context.writer.append(",\"name\":") && context.writer.quoted(track.name()) &&
               context.writer.character('}');
    });
}

bool write_sequence(EncodeContext& context, const Sequence& sequence) {
    return write_envelope(context, "pulp.timeline.sequence", 1, [&] {
        if (!context.writer.append("{\"absolute_duration\":"))
            return false;
        if (sequence.absolute_duration()) {
            if (!context.writer.append("{\"sample_count\":") ||
                !context.writer.u64(sequence.absolute_duration()->sample_count, true) ||
                !context.writer.append(",\"sample_rate\":") ||
                !write_rate(context, sequence.absolute_duration()->sample_rate) ||
                !context.writer.character('}'))
                return false;
        } else if (!context.writer.append("null"))
            return false;
        if (!context.writer.append(",\"id\":") || !context.writer.u64(sequence.id().value, true) ||
            !context.writer.append(",\"musical_duration\":"))
            return false;
        if (sequence.duration()) {
            if (!context.writer.i64(sequence.duration()->value, true))
                return false;
        } else if (!context.writer.append("null"))
            return false;
        if (!context.writer.append(",\"name\":") || !context.writer.quoted(sequence.name()) ||
            !context.writer.append(",\"tracks\":["))
            return false;
        for (std::size_t index = 0; index < sequence.tracks().size(); ++index)
            if ((index != 0 && !context.writer.character(',')) ||
                !write_track(context, sequence.tracks()[index]))
                return false;
        return context.writer.append("]}");
    });
}

struct DecodeCounts {
    std::size_t assets = 0;
    std::size_t sequences = 0;
    std::size_t tracks = 0;
    std::size_t clips = 0;
    std::size_t notes = 0;
};

runtime::Result<const JsonValue*, PersistenceError>
required(const JsonValue& object_value, std::string_view name, std::string path) {
    if (object_value.kind != JsonValue::Kind::Object)
        return fail<const JsonValue*>(PersistenceErrorCode::UnexpectedType, std::move(path),
                                      object_value.begin);
    const auto* value = object_value.find(name);
    if (!value)
        return fail<const JsonValue*>(PersistenceErrorCode::MissingField,
                                      path + "/" + std::string(name), object_value.begin);
    return runtime::Result<const JsonValue*, PersistenceError>(runtime::Ok(value));
}

runtime::Result<std::string, PersistenceError>
string_field(const JsonValue& object_value, std::string_view name, std::string path) {
    auto value = required(object_value, name, path);
    if (!value)
        return fail<std::string>(value.error().code, value.error().path, value.error().byte_offset);
    if (value.value()->kind != JsonValue::Kind::String)
        return fail<std::string>(PersistenceErrorCode::UnexpectedType,
                                 path + "/" + std::string(name), value.value()->begin);
    return runtime::Result<std::string, PersistenceError>(runtime::Ok(value.value()->scalar));
}

runtime::Result<const JsonValue*, PersistenceError>
data_for(const JsonValue& value, std::string_view expected_type, std::string path) {
    auto type = string_field(value, "type_name", path);
    auto version = required(value, "version", path);
    auto data = required(value, "data", path);
    if (!type)
        return fail<const JsonValue*>(type.error().code, type.error().path,
                                      type.error().byte_offset);
    if (!version)
        return version;
    if (!data)
        return data;
    auto decoded_version = parse_u32_number(*version.value(), path + "/version");
    if (type.value() != expected_type)
        return fail<const JsonValue*>(PersistenceErrorCode::UnsupportedStructuralType,
                                      std::move(path), value.begin);
    if (!decoded_version || decoded_version.value() != 1)
        return fail<const JsonValue*>(PersistenceErrorCode::UnsupportedSchemaVersion,
                                      std::move(path), value.begin);
    if (data.value()->kind != JsonValue::Kind::Object)
        return fail<const JsonValue*>(PersistenceErrorCode::UnexpectedType, path + "/data",
                                      data.value()->begin);
    return data;
}

runtime::Result<timebase::RationalRate, PersistenceError> decode_rate(const JsonValue& value,
                                                                      std::string path) {
    auto numerator = required(value, "numerator", path);
    auto denominator = required(value, "denominator", path);
    if (!numerator)
        return fail<timebase::RationalRate>(numerator.error().code, numerator.error().path,
                                            numerator.error().byte_offset);
    if (!denominator)
        return fail<timebase::RationalRate>(denominator.error().code, denominator.error().path,
                                            denominator.error().byte_offset);
    auto n = parse_canonical_u64_string(*numerator.value(), path + "/numerator");
    auto d = parse_canonical_u64_string(*denominator.value(), path + "/denominator");
    if (!n)
        return fail<timebase::RationalRate>(n.error().code, n.error().path, n.error().byte_offset);
    if (!d)
        return fail<timebase::RationalRate>(d.error().code, d.error().path, d.error().byte_offset);
    const timebase::RationalRate rate{n.value(), d.value()};
    if (!rate.valid() || rate.normalized() != rate)
        return fail<timebase::RationalRate>(PersistenceErrorCode::InvalidNumber, std::move(path),
                                            value.begin);
    return runtime::Result<timebase::RationalRate, PersistenceError>(runtime::Ok(rate));
}

runtime::Result<timebase::TempoMap, PersistenceError>
decode_tempo_map(const JsonValue& value, std::string path) {
    if (value.kind != JsonValue::Kind::Array)
        return fail<timebase::TempoMap>(PersistenceErrorCode::UnexpectedType, std::move(path));
    std::vector<timebase::TempoPoint> points;
    points.reserve(value.array.size());
    for (std::size_t index = 0; index < value.array.size(); ++index) {
        const auto item_path = path + "/" + std::to_string(index);
        auto bits = required(value.array[index], "bpm_bits", item_path);
        auto curve = string_field(value.array[index], "curve", item_path);
        auto tick = required(value.array[index], "tick", item_path);
        if (!bits || !curve || !tick)
            return fail<timebase::TempoMap>(PersistenceErrorCode::MissingField, item_path);
        auto decoded_bits = parse_canonical_u64_string(*bits.value(), item_path + "/bpm_bits");
        auto decoded_tick = parse_canonical_i64_string(*tick.value(), item_path + "/tick");
        if (!decoded_bits || !decoded_tick)
            return fail<timebase::TempoMap>(PersistenceErrorCode::InvalidNumber, item_path);
        timebase::TempoCurve decoded_curve;
        if (curve.value() == "constant")
            decoded_curve = timebase::TempoCurve::Constant;
        else if (curve.value() == "linear_in_ticks")
            decoded_curve = timebase::TempoCurve::LinearInTicks;
        else
            return fail<timebase::TempoMap>(PersistenceErrorCode::InvalidSchema,
                                            item_path + "/curve");
        points.push_back({{decoded_tick.value()}, std::bit_cast<double>(decoded_bits.value()),
                          decoded_curve});
    }
    auto created = timebase::TempoMap::create(points);
    if (!created)
        return fail<timebase::TempoMap>(PersistenceErrorCode::InvalidSchema, std::move(path));
    return runtime::Ok(std::move(created).value());
}

runtime::Result<timebase::MeterMap, PersistenceError>
decode_meter_map(const JsonValue& value, std::string path) {
    if (value.kind != JsonValue::Kind::Array)
        return fail<timebase::MeterMap>(PersistenceErrorCode::UnexpectedType, std::move(path));
    std::vector<timebase::MeterPoint> points;
    points.reserve(value.array.size());
    for (std::size_t index = 0; index < value.array.size(); ++index) {
        const auto item_path = path + "/" + std::to_string(index);
        auto denominator = required(value.array[index], "denominator", item_path);
        auto numerator = required(value.array[index], "numerator", item_path);
        auto tick = required(value.array[index], "tick", item_path);
        if (!denominator || !numerator || !tick)
            return fail<timebase::MeterMap>(PersistenceErrorCode::MissingField, item_path);
        auto d = parse_u32_number(*denominator.value(), item_path + "/denominator");
        auto n = parse_u32_number(*numerator.value(), item_path + "/numerator");
        auto t = parse_canonical_i64_string(*tick.value(), item_path + "/tick");
        if (!d || !n || !t || d.value() > std::numeric_limits<std::int32_t>::max() ||
            n.value() > std::numeric_limits<std::int32_t>::max())
            return fail<timebase::MeterMap>(PersistenceErrorCode::InvalidNumber, item_path);
        points.push_back({{t.value()}, {static_cast<std::int32_t>(n.value()),
                                        static_cast<std::int32_t>(d.value())}});
    }
    auto created = timebase::MeterMap::create(points);
    if (!created)
        return fail<timebase::MeterMap>(PersistenceErrorCode::InvalidSchema, std::move(path));
    return runtime::Ok(std::move(created).value());
}

runtime::Result<AssetStoragePolicy, PersistenceError> decode_storage(std::string_view value,
                                                                     std::string path) {
    if (value == "external")
        return runtime::Result<AssetStoragePolicy, PersistenceError>(
            runtime::Ok(AssetStoragePolicy::External));
    if (value == "embedded")
        return runtime::Result<AssetStoragePolicy, PersistenceError>(
            runtime::Ok(AssetStoragePolicy::Embedded));
    if (value == "prefer_embedded")
        return runtime::Result<AssetStoragePolicy, PersistenceError>(
            runtime::Ok(AssetStoragePolicy::PreferEmbedded));
    return fail<AssetStoragePolicy>(PersistenceErrorCode::InvalidSchema, std::move(path));
}

runtime::Result<std::vector<AssetLocator>, PersistenceError> decode_locators(const JsonValue& value,
                                                                             std::string path) {
    if (value.kind != JsonValue::Kind::Array)
        return fail<std::vector<AssetLocator>>(PersistenceErrorCode::UnexpectedType,
                                               std::move(path), value.begin);
    std::vector<AssetLocator> result;
    result.reserve(value.array.size());
    for (std::size_t index = 0; index < value.array.size(); ++index) {
        const auto member_path = path + "/" + std::to_string(index);
        auto kind = string_field(value.array[index], "kind", member_path);
        auto hint = string_field(value.array[index], "hint", member_path);
        if (!kind)
            return fail<std::vector<AssetLocator>>(kind.error().code, kind.error().path,
                                                   kind.error().byte_offset);
        if (!hint)
            return fail<std::vector<AssetLocator>>(hint.error().code, hint.error().path,
                                                   hint.error().byte_offset);
        AssetLocatorKind decoded_kind;
        if (kind.value() == "package_relative")
            decoded_kind = AssetLocatorKind::PackageRelative;
        else if (kind.value() == "external_uri")
            decoded_kind = AssetLocatorKind::ExternalUri;
        else
            return fail<std::vector<AssetLocator>>(PersistenceErrorCode::InvalidSchema,
                                                   member_path + "/kind");
        result.push_back({decoded_kind, std::move(hint).value()});
    }
    return runtime::Result<std::vector<AssetLocator>, PersistenceError>(
        runtime::Ok(std::move(result)));
}

runtime::Result<AssetRepresentation, PersistenceError> decode_representation(const JsonValue& value,
                                                                             std::string path) {
    auto data = data_for(value, "pulp.timeline.asset_representation", path);
    if (!data)
        return fail<AssetRepresentation>(data.error().code, data.error().path,
                                         data.error().byte_offset);
    auto role = string_field(*data.value(), "role", path + "/data");
    auto hash = string_field(*data.value(), "content_hash", path + "/data");
    auto policy = string_field(*data.value(), "storage_policy", path + "/data");
    auto locators = required(*data.value(), "locators", path + "/data");
    if (!role || !hash || !policy || !locators)
        return fail<AssetRepresentation>(PersistenceErrorCode::MissingField, std::move(path));
    auto decoded_hash = ContentHash::from_hex(hash.value());
    auto decoded_policy = decode_storage(policy.value(), path + "/data/storage_policy");
    auto decoded_locators = decode_locators(*locators.value(), path + "/data/locators");
    if (!decoded_hash || !decoded_policy || !decoded_locators)
        return fail<AssetRepresentation>(PersistenceErrorCode::InvalidSchema, std::move(path));
    return runtime::Result<AssetRepresentation, PersistenceError>(runtime::Ok(
        AssetRepresentation{std::move(role).value(), *decoded_hash, decoded_policy.value(),
                            std::move(decoded_locators).value()}));
}

runtime::Result<MediaAsset, PersistenceError> decode_asset(const JsonValue& value, std::string path,
                                                           DecodeCounts& counts,
                                                           const DecodeLimits& limits) {
    if (++counts.assets > limits.max_assets)
        return fail<MediaAsset>(PersistenceErrorCode::LimitExceeded, path, value.begin,
                                counts.assets, limits.max_assets);
    auto data = data_for(value, "pulp.timeline.asset", path);
    if (!data)
        return fail<MediaAsset>(data.error().code, data.error().path, data.error().byte_offset);
    auto id = required(*data.value(), "id", path + "/data");
    auto name = string_field(*data.value(), "name", path + "/data");
    auto frame_count = required(*data.value(), "frame_count", path + "/data");
    auto rate = required(*data.value(), "sample_rate", path + "/data");
    auto hash = string_field(*data.value(), "content_hash", path + "/data");
    auto policy = string_field(*data.value(), "storage_policy", path + "/data");
    auto locators = required(*data.value(), "locators", path + "/data");
    auto representations = required(*data.value(), "representations", path + "/data");
    if (!id || !name || !frame_count || !rate || !hash || !policy || !locators || !representations)
        return fail<MediaAsset>(PersistenceErrorCode::MissingField, std::move(path));
    auto decoded_id = parse_canonical_u64_string(*id.value(), path + "/data/id");
    auto decoded_frames =
        parse_canonical_u64_string(*frame_count.value(), path + "/data/frame_count");
    auto decoded_rate = decode_rate(*rate.value(), path + "/data/sample_rate");
    auto decoded_hash = ContentHash::from_hex(hash.value());
    auto decoded_policy = decode_storage(policy.value(), path + "/data/storage_policy");
    auto decoded_locators = decode_locators(*locators.value(), path + "/data/locators");
    if (!decoded_id || !decoded_frames || !decoded_rate || !decoded_hash || !decoded_policy ||
        !decoded_locators || representations.value()->kind != JsonValue::Kind::Array)
        return fail<MediaAsset>(PersistenceErrorCode::InvalidSchema, std::move(path));
    std::vector<AssetRepresentation> decoded_representations;
    for (std::size_t index = 0; index < representations.value()->array.size(); ++index) {
        auto decoded =
            decode_representation(representations.value()->array[index],
                                  path + "/data/representations/" + std::to_string(index));
        if (!decoded)
            return fail<MediaAsset>(decoded.error().code, decoded.error().path,
                                    decoded.error().byte_offset);
        decoded_representations.push_back(std::move(decoded).value());
    }
    return runtime::Result<MediaAsset, PersistenceError>(runtime::Ok(
        MediaAsset{ItemId{decoded_id.value()}, std::move(name).value(), decoded_frames.value(),
                   decoded_rate.value(), *decoded_hash, decoded_policy.value(),
                   std::move(decoded_locators).value(), std::move(decoded_representations)}));
}

runtime::Result<ClipContent, PersistenceError>
decode_content(const std::shared_ptr<const ParsedJson>& document, const JsonValue& value,
               const SchemaRegistry& registry, const DecodeLimits& limits, DecodeCounts& counts,
               std::string path) {
    auto type = string_field(value, "type_name", path);
    auto version_value = required(value, "version", path);
    if (!type || !version_value)
        return fail<ClipContent>(PersistenceErrorCode::MissingField, std::move(path));
    auto version = parse_u32_number(*version_value.value(), path + "/version");
    if (!version)
        return fail<ClipContent>(version.error().code, version.error().path,
                                 version.error().byte_offset);
    const auto* schema = registry.find(SchemaDomain::Content, type.value());
    if (!schema || version.value() > schema->current_version) {
        if (!value.find("data"))
            return fail<ClipContent>(PersistenceErrorCode::MissingField, path + "/data",
                                     value.begin);
        const auto raw = document->raw(value);
        if (raw.size() > limits.max_opaque_bytes)
            return fail<ClipContent>(PersistenceErrorCode::LimitExceeded, std::move(path),
                                     value.begin, raw.size(), limits.max_opaque_bytes);
        const OpaqueContentLimits opaque_limits{
            limits.max_input_bytes,    limits.max_depth,          limits.max_total_values,
            limits.max_array_elements, limits.max_object_members, limits.max_string_bytes,
            limits.max_opaque_bytes};
        auto opaque =
            OpaqueContent::create({type.value(), version.value()}, std::string(raw), opaque_limits);
        if (!opaque)
            return model_fail<ClipContent>(opaque.error(), std::move(path));
        return runtime::Result<ClipContent, PersistenceError>(
            runtime::Ok(ClipContent(std::move(opaque).value())));
    }
    if (version.value() < schema->current_version) {
        auto migrated = registry.migrate(SchemaDomain::Content, type.value(), version.value(),
                                         schema->current_version, document->raw(value), limits);
        if (!migrated)
            return fail<ClipContent>(migrated.error().code, std::move(path));
        auto parsed = parse_json(migrated.value(), limits);
        if (!parsed)
            return fail<ClipContent>(parsed.error().code, std::move(path));
        return decode_content(parsed.value(), parsed.value()->root(), registry, limits, counts,
                              std::move(path));
    }
    auto data = required(value, "data", path);
    if (!data || data.value()->kind != JsonValue::Kind::Object)
        return fail<ClipContent>(PersistenceErrorCode::UnexpectedType, path + "/data");
    if (type.value() == "pulp.timeline.content.empty")
        return runtime::Result<ClipContent, PersistenceError>(
            runtime::Ok(ClipContent(EmptyContent{})));
    if (type.value() == "pulp.timeline.content.media") {
        auto asset = required(*data.value(), "asset_id", path + "/data");
        auto source = required(*data.value(), "source_start", path + "/data");
        auto frames = required(*data.value(), "frame_count", path + "/data");
        if (!asset || !source || !frames)
            return fail<ClipContent>(PersistenceErrorCode::MissingField, std::move(path));
        auto decoded_asset = parse_canonical_u64_string(*asset.value(), path + "/data/asset_id");
        auto decoded_source =
            parse_canonical_i64_string(*source.value(), path + "/data/source_start");
        auto decoded_frames =
            parse_canonical_u64_string(*frames.value(), path + "/data/frame_count");
        if (!decoded_asset || !decoded_source || !decoded_frames)
            return fail<ClipContent>(PersistenceErrorCode::InvalidNumber, std::move(path));
        return runtime::Result<ClipContent, PersistenceError>(runtime::Ok(ClipContent(
            MediaRef{ItemId{decoded_asset.value()},
                     timebase::SamplePosition{decoded_source.value()}, decoded_frames.value()})));
    }
    if (type.value() == "pulp.timeline.content.notes") {
        auto notes = required(*data.value(), "notes", path + "/data");
        if (!notes || notes.value()->kind != JsonValue::Kind::Array)
            return fail<ClipContent>(PersistenceErrorCode::UnexpectedType, path + "/data/notes");
        if (counts.notes > limits.max_notes ||
            notes.value()->array.size() > limits.max_notes - counts.notes)
            return fail<ClipContent>(PersistenceErrorCode::LimitExceeded, path + "/data/notes",
                                     notes.value()->begin,
                                     counts.notes + notes.value()->array.size(), limits.max_notes);
        std::vector<NoteEvent> events;
        events.reserve(notes.value()->array.size());
        for (std::size_t index = 0; index < notes.value()->array.size(); ++index) {
            const auto note_path = path + "/data/notes/" + std::to_string(index);
            const auto& note = notes.value()->array[index];
            auto id = required(note, "id", note_path);
            auto start = required(note, "start_ticks", note_path);
            auto duration = required(note, "duration_ticks", note_path);
            auto velocity = required(note, "velocity", note_path);
            auto pitch = required(note, "pitch", note_path);
            auto channel = required(note, "channel", note_path);
            if (!id || !start || !duration || !velocity || !pitch || !channel)
                return fail<ClipContent>(PersistenceErrorCode::MissingField, note_path);
            auto decoded_id = parse_canonical_u64_string(*id.value(), note_path + "/id");
            auto decoded_start =
                parse_canonical_i64_string(*start.value(), note_path + "/start_ticks");
            auto decoded_duration =
                parse_canonical_i64_string(*duration.value(), note_path + "/duration_ticks");
            auto decoded_velocity = parse_u32_number(*velocity.value(), note_path + "/velocity");
            auto decoded_pitch = parse_u32_number(*pitch.value(), note_path + "/pitch");
            auto decoded_channel = parse_u32_number(*channel.value(), note_path + "/channel");
            if (!decoded_id || !decoded_start || !decoded_duration || !decoded_velocity ||
                !decoded_pitch || !decoded_channel ||
                decoded_velocity.value() > std::numeric_limits<std::uint16_t>::max() ||
                decoded_pitch.value() > std::numeric_limits<std::uint8_t>::max() ||
                decoded_channel.value() > std::numeric_limits<std::uint8_t>::max())
                return fail<ClipContent>(PersistenceErrorCode::InvalidNumber, note_path);
            events.push_back({ItemId{decoded_id.value()},
                              {decoded_start.value()},
                              {decoded_duration.value()},
                              static_cast<std::uint16_t>(decoded_velocity.value()),
                              static_cast<std::uint8_t>(decoded_pitch.value()),
                              static_cast<std::uint8_t>(decoded_channel.value())});
        }
        counts.notes += events.size();
        auto created = NoteContent::create(std::move(events));
        if (!created)
            return model_fail<ClipContent>(created.error(), std::move(path));
        return runtime::Result<ClipContent, PersistenceError>(
            runtime::Ok(ClipContent(std::move(created).value())));
    }
    if (!schema->codec.decode)
        return fail<ClipContent>(PersistenceErrorCode::InvalidSchema, std::move(path));
    auto payload = schema->codec.decode(*data.value(), schema->codec.context.get());
    if (!payload)
        return fail<ClipContent>(payload.error().code, std::move(path));
    auto registered =
        registry.create_registered_no_owned_ids({schema->type_name, schema->current_version},
                                                std::move(payload).value(), limits.max_input_bytes);
    if (!registered)
        return fail<ClipContent>(registered.error().code, std::move(path));
    return runtime::Result<ClipContent, PersistenceError>(
        runtime::Ok(ClipContent(std::move(registered).value())));
}

runtime::Result<Clip, PersistenceError>
decode_clip(const std::shared_ptr<const ParsedJson>& document, const JsonValue& value,
            const SchemaRegistry& registry, const DecodeLimits& limits, DecodeCounts& counts,
            std::string path) {
    if (++counts.clips > limits.max_clips)
        return fail<Clip>(PersistenceErrorCode::LimitExceeded, path, value.begin, counts.clips,
                          limits.max_clips);
    auto data = data_for(value, "pulp.timeline.clip", path);
    if (!data)
        return fail<Clip>(data.error().code, data.error().path, data.error().byte_offset);
    auto id = required(*data.value(), "id", path + "/data");
    auto range = required(*data.value(), "time_range", path + "/data");
    auto content_value = required(*data.value(), "content", path + "/data");
    if (!id || !range || !content_value)
        return fail<Clip>(PersistenceErrorCode::MissingField, std::move(path));
    auto decoded_id = parse_canonical_u64_string(*id.value(), path + "/data/id");
    auto kind = string_field(*range.value(), "kind", path + "/data/time_range");
    auto content = decode_content(document, *content_value.value(), registry, limits, counts,
                                  path + "/data/content");
    if (!decoded_id)
        return fail<Clip>(decoded_id.error().code, decoded_id.error().path,
                          decoded_id.error().byte_offset);
    if (!kind)
        return fail<Clip>(kind.error().code, kind.error().path, kind.error().byte_offset);
    if (!content)
        return runtime::Result<Clip, PersistenceError>(runtime::Err(content.error()));
    ClipPlaybackProperties playback;
    const auto* gain = data.value()->find("gain_linear_bits");
    const auto* fade_in = data.value()->find("fade_in_duration");
    const auto* fade_out = data.value()->find("fade_out_duration");
    const bool has_any_playback = gain || fade_in || fade_out;
    if (has_any_playback && (!gain || !fade_in || !fade_out))
        return fail<Clip>(PersistenceErrorCode::MissingField, path + "/data");
    if (has_any_playback) {
        auto decoded_gain = parse_canonical_u64_string(*gain, path + "/data/gain_linear_bits");
        auto decoded_fade_in =
            parse_canonical_u64_string(*fade_in, path + "/data/fade_in_duration");
        auto decoded_fade_out =
            parse_canonical_u64_string(*fade_out, path + "/data/fade_out_duration");
        if (!decoded_gain || !decoded_fade_in || !decoded_fade_out ||
            decoded_gain.value() > std::numeric_limits<std::uint32_t>::max())
            return fail<Clip>(PersistenceErrorCode::InvalidNumber, path + "/data");
        playback = {std::bit_cast<float>(static_cast<std::uint32_t>(decoded_gain.value())),
                    decoded_fade_in.value(), decoded_fade_out.value()};
    }
    runtime::Result<Clip, ModelError> created(runtime::Err(ModelError{}));
    if (kind.value() == "musical") {
        auto start = required(*range.value(), "start_ticks", path + "/data/time_range");
        auto duration = required(*range.value(), "duration_ticks", path + "/data/time_range");
        if (!start || !duration)
            return fail<Clip>(PersistenceErrorCode::MissingField, path);
        auto decoded_start =
            parse_canonical_i64_string(*start.value(), path + "/data/time_range/start_ticks");
        auto decoded_duration =
            parse_canonical_i64_string(*duration.value(), path + "/data/time_range/duration_ticks");
        if (!decoded_start || !decoded_duration)
            return fail<Clip>(PersistenceErrorCode::InvalidNumber, path);
        created = Clip::create({decoded_id.value()}, {decoded_start.value()},
                               {decoded_duration.value()}, std::move(content).value(), playback);
    } else if (kind.value() == "absolute") {
        auto start = required(*range.value(), "start_sample", path + "/data/time_range");
        auto count = required(*range.value(), "sample_count", path + "/data/time_range");
        auto rate = required(*range.value(), "sample_rate", path + "/data/time_range");
        if (!start || !count || !rate)
            return fail<Clip>(PersistenceErrorCode::MissingField, path);
        auto decoded_start =
            parse_canonical_i64_string(*start.value(), path + "/data/time_range/start_sample");
        auto decoded_count =
            parse_canonical_u64_string(*count.value(), path + "/data/time_range/sample_count");
        auto decoded_rate = decode_rate(*rate.value(), path + "/data/time_range/sample_rate");
        if (!decoded_start || !decoded_count || !decoded_rate)
            return fail<Clip>(PersistenceErrorCode::InvalidNumber, path);
        created = Clip::create_absolute({decoded_id.value()}, {decoded_start.value()},
                                        decoded_count.value(), decoded_rate.value(),
                                        std::move(content).value(), playback);
    } else {
        return fail<Clip>(PersistenceErrorCode::InvalidSchema, path + "/data/time_range/kind");
    }
    if (!created)
        return model_fail<Clip>(created.error(), std::move(path));
    return runtime::Result<Clip, PersistenceError>(runtime::Ok(std::move(created).value()));
}

runtime::Result<Track, PersistenceError>
decode_track(const std::shared_ptr<const ParsedJson>& document, const JsonValue& value,
             const SchemaRegistry& registry, const DecodeLimits& limits, DecodeCounts& counts,
             std::string path) {
    if (++counts.tracks > limits.max_tracks)
        return fail<Track>(PersistenceErrorCode::LimitExceeded, path, value.begin, counts.tracks,
                           limits.max_tracks);
    auto data = data_for(value, "pulp.timeline.track", path);
    if (!data)
        return fail<Track>(data.error().code, data.error().path, data.error().byte_offset);
    auto id = required(*data.value(), "id", path + "/data");
    auto name = string_field(*data.value(), "name", path + "/data");
    auto clips = required(*data.value(), "clips", path + "/data");
    if (!id || !name || !clips || clips.value()->kind != JsonValue::Kind::Array)
        return fail<Track>(PersistenceErrorCode::MissingField, std::move(path));
    auto decoded_id = parse_canonical_u64_string(*id.value(), path + "/data/id");
    if (!decoded_id)
        return fail<Track>(decoded_id.error().code, decoded_id.error().path,
                           decoded_id.error().byte_offset);
    std::vector<Clip> decoded_clips;
    for (std::size_t index = 0; index < clips.value()->array.size(); ++index) {
        auto decoded = decode_clip(document, clips.value()->array[index], registry, limits, counts,
                                   path + "/data/clips/" + std::to_string(index));
        if (!decoded)
            return fail<Track>(decoded.error().code, decoded.error().path,
                               decoded.error().byte_offset);
        decoded_clips.push_back(std::move(decoded).value());
    }
    auto created =
        Track::create({decoded_id.value()}, std::move(name).value(), std::move(decoded_clips));
    if (!created)
        return model_fail<Track>(created.error(), std::move(path));
    return runtime::Result<Track, PersistenceError>(runtime::Ok(std::move(created).value()));
}

runtime::Result<Sequence, PersistenceError>
decode_sequence(const std::shared_ptr<const ParsedJson>& document, const JsonValue& value,
                const SchemaRegistry& registry, const DecodeLimits& limits, DecodeCounts& counts,
                std::string path) {
    if (++counts.sequences > limits.max_sequences)
        return fail<Sequence>(PersistenceErrorCode::LimitExceeded, path, value.begin,
                              counts.sequences, limits.max_sequences);
    auto data = data_for(value, "pulp.timeline.sequence", path);
    if (!data)
        return fail<Sequence>(data.error().code, data.error().path, data.error().byte_offset);
    auto id = required(*data.value(), "id", path + "/data");
    auto name = string_field(*data.value(), "name", path + "/data");
    auto tracks = required(*data.value(), "tracks", path + "/data");
    auto musical = required(*data.value(), "musical_duration", path + "/data");
    auto absolute = required(*data.value(), "absolute_duration", path + "/data");
    if (!id || !name || !tracks || !musical || !absolute ||
        tracks.value()->kind != JsonValue::Kind::Array)
        return fail<Sequence>(PersistenceErrorCode::MissingField, std::move(path));
    auto decoded_id = parse_canonical_u64_string(*id.value(), path + "/data/id");
    if (!decoded_id)
        return fail<Sequence>(decoded_id.error().code, decoded_id.error().path,
                              decoded_id.error().byte_offset);
    std::optional<timebase::TickDuration> decoded_musical;
    if (musical.value()->kind != JsonValue::Kind::Null) {
        auto parsed = parse_canonical_i64_string(*musical.value(), path + "/data/musical_duration");
        if (!parsed)
            return fail<Sequence>(parsed.error().code, parsed.error().path,
                                  parsed.error().byte_offset);
        decoded_musical = timebase::TickDuration{parsed.value()};
    }
    std::optional<AbsoluteTimelineDuration> decoded_absolute;
    if (absolute.value()->kind != JsonValue::Kind::Null) {
        auto count = required(*absolute.value(), "sample_count", path + "/data/absolute_duration");
        auto rate = required(*absolute.value(), "sample_rate", path + "/data/absolute_duration");
        if (!count || !rate)
            return fail<Sequence>(PersistenceErrorCode::MissingField, path);
        auto decoded_count = parse_canonical_u64_string(
            *count.value(), path + "/data/absolute_duration/sample_count");
        auto decoded_rate =
            decode_rate(*rate.value(), path + "/data/absolute_duration/sample_rate");
        if (!decoded_count || !decoded_rate)
            return fail<Sequence>(PersistenceErrorCode::InvalidNumber, path);
        decoded_absolute = AbsoluteTimelineDuration{decoded_count.value(), decoded_rate.value()};
    }
    std::vector<Track> decoded_tracks;
    for (std::size_t index = 0; index < tracks.value()->array.size(); ++index) {
        auto decoded = decode_track(document, tracks.value()->array[index], registry, limits,
                                    counts, path + "/data/tracks/" + std::to_string(index));
        if (!decoded)
            return fail<Sequence>(decoded.error().code, decoded.error().path,
                                  decoded.error().byte_offset);
        decoded_tracks.push_back(std::move(decoded).value());
    }
    auto created = Sequence::create({decoded_id.value()}, std::move(name).value(), decoded_musical,
                                    decoded_absolute, std::move(decoded_tracks));
    if (!created)
        return model_fail<Sequence>(created.error(), std::move(path));
    return runtime::Result<Sequence, PersistenceError>(runtime::Ok(std::move(created).value()));
}

std::optional<PersistenceErrorCode>
validate_structural_registry(const SchemaRegistry& registry) noexcept {
    struct ExpectedField {
        std::string_view name;
        SchemaValueKind kind;
        bool required = true;
    };
    struct RequiredSchema {
        SchemaDomain domain;
        std::string_view type_name;
        std::span<const ExpectedField> fields;
    };

    static constexpr ExpectedField project_fields[] = {
        {"assets", SchemaValueKind::Array},
        {"id", SchemaValueKind::U64String},
        {"identities", SchemaValueKind::Array, false},
        {"meter_map", SchemaValueKind::Array, false},
        {"name", SchemaValueKind::String},
        {"next_item_id", SchemaValueKind::U64String},
        {"root_sequence_id", SchemaValueKind::U64String},
        {"sequences", SchemaValueKind::Array},
        {"tempo_map", SchemaValueKind::Array, false},
    };
    static constexpr ExpectedField asset_fields[] = {
        {"content_hash", SchemaValueKind::String}, {"frame_count", SchemaValueKind::U64String},
        {"id", SchemaValueKind::U64String},        {"locators", SchemaValueKind::Array},
        {"name", SchemaValueKind::String},         {"representations", SchemaValueKind::Array},
        {"sample_rate", SchemaValueKind::Object},  {"storage_policy", SchemaValueKind::String},
    };
    static constexpr ExpectedField representation_fields[] = {
        {"content_hash", SchemaValueKind::String},
        {"locators", SchemaValueKind::Array},
        {"role", SchemaValueKind::String},
        {"storage_policy", SchemaValueKind::String},
    };
    static constexpr ExpectedField sequence_fields[] = {
        {"absolute_duration", SchemaValueKind::Object},
        {"id", SchemaValueKind::U64String},
        {"musical_duration", SchemaValueKind::I64String},
        {"name", SchemaValueKind::String},
        {"tracks", SchemaValueKind::Array},
    };
    static constexpr ExpectedField track_fields[] = {
        {"clips", SchemaValueKind::Array},
        {"id", SchemaValueKind::U64String},
        {"name", SchemaValueKind::String},
    };
    static constexpr ExpectedField clip_fields[] = {
        {"content", SchemaValueKind::Object},
        {"fade_in_duration", SchemaValueKind::U64String, false},
        {"fade_out_duration", SchemaValueKind::U64String, false},
        {"gain_linear_bits", SchemaValueKind::U64String, false},
        {"id", SchemaValueKind::U64String},
        {"time_range", SchemaValueKind::Object},
    };
    static constexpr ExpectedField media_fields[] = {
        {"asset_id", SchemaValueKind::U64String},
        {"frame_count", SchemaValueKind::U64String},
        {"source_start", SchemaValueKind::I64String},
    };
    static constexpr ExpectedField notes_fields[] = {
        {"notes", SchemaValueKind::Array},
    };
    constexpr RequiredSchema required[] = {
        {SchemaDomain::Document, "pulp.timeline.project", project_fields},
        {SchemaDomain::Document, "pulp.timeline.asset", asset_fields},
        {SchemaDomain::AssetRepresentation, "pulp.timeline.asset_representation",
         representation_fields},
        {SchemaDomain::Document, "pulp.timeline.sequence", sequence_fields},
        {SchemaDomain::Document, "pulp.timeline.track", track_fields},
        {SchemaDomain::Document, "pulp.timeline.clip", clip_fields},
        {SchemaDomain::Content, "pulp.timeline.content.empty", {}},
        {SchemaDomain::Content, "pulp.timeline.content.media", media_fields},
        {SchemaDomain::Content, "pulp.timeline.content.notes", notes_fields},
    };
    for (const auto& expected : required) {
        const auto* schema = registry.find(expected.domain, expected.type_name);
        if (!schema)
            return PersistenceErrorCode::UnsupportedStructuralType;
        if (schema->current_version != 1)
            return PersistenceErrorCode::UnsupportedSchemaVersion;
        if (schema->fields.size() != expected.fields.size())
            return PersistenceErrorCode::InvalidSchema;
        for (std::size_t index = 0; index < expected.fields.size(); ++index) {
            const auto& actual = schema->fields[index];
            const auto& field = expected.fields[index];
            if (actual.name != field.name || actual.kind != field.kind ||
                actual.required != field.required || !actual.referenced_type.empty())
                return PersistenceErrorCode::InvalidSchema;
        }
    }
    return std::nullopt;
}

} // namespace

runtime::Result<SerializedSnapshot, PersistenceError>
serialize_project(const Project& project, const SchemaRegistry& registry,
                  const SerializeOptions& options) {
    if (const auto invalid = validate_structural_registry(registry))
        return fail<SerializedSnapshot>(*invalid, "/");
    if (!is_valid_utf8(project.name()))
        return fail<SerializedSnapshot>(PersistenceErrorCode::InvalidUtf8, "/data/name");
    for (std::size_t asset_index = 0; asset_index < project.assets().size(); ++asset_index) {
        const auto& asset = project.assets()[asset_index];
        const auto asset_path = "/data/assets/" + std::to_string(asset_index) + "/data";
        if (!is_valid_utf8(asset.name))
            return fail<SerializedSnapshot>(PersistenceErrorCode::InvalidUtf8,
                                            asset_path + "/name");
        for (std::size_t index = 0; index < asset.locators.size(); ++index)
            if (!is_valid_utf8(asset.locators[index].hint))
                return fail<SerializedSnapshot>(PersistenceErrorCode::InvalidUtf8,
                                                asset_path + "/locators/" + std::to_string(index) +
                                                    "/hint");
        for (std::size_t representation_index = 0;
             representation_index < asset.representations.size(); ++representation_index) {
            const auto& representation = asset.representations[representation_index];
            const auto representation_path =
                asset_path + "/representations/" + std::to_string(representation_index) + "/data";
            if (!is_valid_utf8(representation.role))
                return fail<SerializedSnapshot>(PersistenceErrorCode::InvalidUtf8,
                                                representation_path + "/role");
            for (std::size_t index = 0; index < representation.locators.size(); ++index)
                if (!is_valid_utf8(representation.locators[index].hint))
                    return fail<SerializedSnapshot>(PersistenceErrorCode::InvalidUtf8,
                                                    representation_path + "/locators/" +
                                                        std::to_string(index) + "/hint");
        }
    }
    for (std::size_t sequence_index = 0; sequence_index < project.sequences().size();
         ++sequence_index) {
        const auto& sequence = project.sequences()[sequence_index];
        const auto sequence_path = "/data/sequences/" + std::to_string(sequence_index) + "/data";
        if (!is_valid_utf8(sequence.name()))
            return fail<SerializedSnapshot>(PersistenceErrorCode::InvalidUtf8,
                                            sequence_path + "/name");
        for (std::size_t track_index = 0; track_index < sequence.tracks().size(); ++track_index)
            if (!is_valid_utf8(sequence.tracks()[track_index].name()))
                return fail<SerializedSnapshot>(PersistenceErrorCode::InvalidUtf8,
                                                sequence_path + "/tracks/" +
                                                    std::to_string(track_index) + "/data/name");
    }
    EncodeContext context{JsonWriter(options.max_output_bytes), registry, false, std::nullopt};
    const auto wrote = write_envelope(context, "pulp.timeline.project", 1, [&] {
        if (!context.writer.append("{\"assets\":["))
            return false;
        for (std::size_t index = 0; index < project.assets().size(); ++index)
            if ((index != 0 && !context.writer.character(',')) ||
                !write_asset(context, project.assets()[index]))
                return false;
        if (!context.writer.append("],\"id\":") || !context.writer.u64(project.id().value, true) ||
            !context.writer.append(",\"identities\":["))
            return false;
        const auto identities = detail::ProjectStateAccess::identity_entries(project);
        for (std::size_t index = 0; index < identities.size(); ++index) {
            const auto& identity = identities[index];
            const auto& location = identity.location;
            if ((index != 0 && !context.writer.character(',')) ||
                !context.writer.append("{\"active\":") ||
                !context.writer.append(location.active ? "true" : "false") ||
                !context.writer.append(",\"clip_id\":") ||
                !context.writer.u64(location.clip_id.value, true) ||
                !context.writer.append(",\"id\":") ||
                !context.writer.u64(identity.item.value, true) ||
                !context.writer.append(",\"kind\":") ||
                !context.writer.quoted(item_kind_name(location.kind)) ||
                !context.writer.append(",\"sequence_id\":") ||
                !context.writer.u64(location.sequence_id.value, true) ||
                !context.writer.append(",\"track_id\":") ||
                !context.writer.u64(location.track_id.value, true) ||
                !context.writer.character('}'))
                return false;
        }
        if (!context.writer.append("],\"meter_map\":") ||
            !write_meter_map(context, project.meter_map()) ||
            !context.writer.append(",\"name\":") || !context.writer.quoted(project.name()) ||
            !context.writer.append(",\"next_item_id\":") ||
            !context.writer.u64(project.next_item_id(), true) ||
            !context.writer.append(",\"root_sequence_id\":") ||
            !context.writer.u64(project.root_sequence_id().value, true) ||
            !context.writer.append(",\"sequences\":["))
            return false;
        for (std::size_t index = 0; index < project.sequences().size(); ++index)
            if ((index != 0 && !context.writer.character(',')) ||
                !write_sequence(context, project.sequences()[index]))
                return false;
        return context.writer.append("],\"tempo_map\":") &&
               write_tempo_map(context, project.tempo_map()) && context.writer.character('}');
    });
    if (!wrote) {
        if (context.failure)
            return runtime::Result<SerializedSnapshot, PersistenceError>(
                runtime::Err(std::move(*context.failure)));
        return runtime::Result<SerializedSnapshot, PersistenceError>(
            runtime::Err(context.writer.error()));
    }
    auto json = context.writer.take();
    return runtime::Result<SerializedSnapshot, PersistenceError>(
        runtime::Ok(SerializedSnapshot{std::move(json), context.opaque}));
}

runtime::Result<Project, PersistenceError> deserialize_project(std::string_view json,
                                                               const SchemaRegistry& registry,
                                                               const DecodeLimits& limits) {
    if (const auto invalid = validate_structural_registry(registry))
        return fail<Project>(*invalid, "/");
    auto structural = preflight_timeline_structure(json, limits);
    if (!structural)
        return runtime::Result<Project, PersistenceError>(runtime::Err(structural.error()));
    auto parsed = parse_json(json, limits);
    if (!parsed)
        return fail<Project>(parsed.error().code, parsed.error().path, parsed.error().byte_offset,
                             parsed.error().actual, parsed.error().limit);
    auto data = data_for(parsed.value()->root(), "pulp.timeline.project", "");
    if (!data)
        return fail<Project>(data.error().code, data.error().path, data.error().byte_offset);
    auto id = required(*data.value(), "id", "/data");
    auto name = string_field(*data.value(), "name", "/data");
    auto next = required(*data.value(), "next_item_id", "/data");
    auto root = required(*data.value(), "root_sequence_id", "/data");
    auto assets = required(*data.value(), "assets", "/data");
    auto sequences = required(*data.value(), "sequences", "/data");
    const auto* identities = data.value()->find("identities");
    const auto* tempo_map_value = data.value()->find("tempo_map");
    const auto* meter_map_value = data.value()->find("meter_map");
    if (!id || !name || !next || !root || !assets || !sequences ||
        assets.value()->kind != JsonValue::Kind::Array ||
        sequences.value()->kind != JsonValue::Kind::Array)
        return fail<Project>(PersistenceErrorCode::MissingField, "/data");
    auto decoded_id = parse_canonical_u64_string(*id.value(), "/data/id");
    auto decoded_next = parse_canonical_u64_string(*next.value(), "/data/next_item_id");
    auto decoded_root = parse_canonical_u64_string(*root.value(), "/data/root_sequence_id");
    if (!decoded_id || !decoded_next || !decoded_root)
        return fail<Project>(PersistenceErrorCode::InvalidNumber, "/data");
    DecodeCounts counts;
    std::vector<MediaAsset> decoded_assets;
    for (std::size_t index = 0; index < assets.value()->array.size(); ++index) {
        auto decoded = decode_asset(assets.value()->array[index],
                                    "/data/assets/" + std::to_string(index), counts, limits);
        if (!decoded)
            return fail<Project>(decoded.error().code, decoded.error().path,
                                 decoded.error().byte_offset);
        decoded_assets.push_back(std::move(decoded).value());
    }
    std::vector<Sequence> decoded_sequences;
    for (std::size_t index = 0; index < sequences.value()->array.size(); ++index) {
        auto decoded = decode_sequence(parsed.value(), sequences.value()->array[index], registry,
                                       limits, counts, "/data/sequences/" + std::to_string(index));
        if (!decoded)
            return fail<Project>(decoded.error().code, decoded.error().path,
                                 decoded.error().byte_offset);
        decoded_sequences.push_back(std::move(decoded).value());
    }
    std::vector<detail::IdentityRecord> decoded_identities;
    if (identities) {
        if (identities->kind != JsonValue::Kind::Array)
            return fail<Project>(PersistenceErrorCode::UnexpectedType, "/data/identities");
        decoded_identities.reserve(identities->array.size());
        for (std::size_t index = 0; index < identities->array.size(); ++index) {
            const auto path = "/data/identities/" + std::to_string(index);
            const auto& value = identities->array[index];
            auto id_value = required(value, "id", path);
            auto kind_value = string_field(value, "kind", path);
            auto sequence_value = required(value, "sequence_id", path);
            auto track_value = required(value, "track_id", path);
            auto clip_value = required(value, "clip_id", path);
            auto active_value = required(value, "active", path);
            if (!id_value || !kind_value || !sequence_value || !track_value || !clip_value ||
                !active_value || active_value.value()->kind != JsonValue::Kind::Boolean)
                return fail<Project>(PersistenceErrorCode::MissingField, path);
            auto item = parse_canonical_u64_string(*id_value.value(), path + "/id");
            auto kind = decode_item_kind(kind_value.value(), path + "/kind");
            auto sequence_id =
                parse_canonical_u64_string(*sequence_value.value(), path + "/sequence_id");
            auto track_id = parse_canonical_u64_string(*track_value.value(), path + "/track_id");
            auto clip_id = parse_canonical_u64_string(*clip_value.value(), path + "/clip_id");
            if (!item || !kind || !sequence_id || !track_id || !clip_id)
                return fail<Project>(PersistenceErrorCode::InvalidNumber, path);
            decoded_identities.push_back({{item.value()},
                                          {kind.value(),
                                           {sequence_id.value()},
                                           {track_id.value()},
                                           {clip_id.value()},
                                           active_value.value()->boolean}});
        }
    }
    timebase::TempoMap decoded_tempo_map;
    if (tempo_map_value) {
        auto decoded = decode_tempo_map(*tempo_map_value, "/data/tempo_map");
        if (!decoded)
            return runtime::Err(decoded.error());
        decoded_tempo_map = std::move(decoded).value();
    }
    timebase::MeterMap decoded_meter_map;
    if (meter_map_value) {
        auto decoded = decode_meter_map(*meter_map_value, "/data/meter_map");
        if (!decoded)
            return runtime::Err(decoded.error());
        decoded_meter_map = std::move(decoded).value();
    }
    auto created = Project::create(ProjectInput{{decoded_id.value()},
                                                std::move(name).value(),
                                                decoded_next.value(),
                                                {decoded_root.value()},
                                                std::move(decoded_assets),
                                                std::move(decoded_sequences),
                                                std::move(decoded_tempo_map),
                                                std::move(decoded_meter_map)});
    if (!created)
        return model_fail<Project>(created.error(), "/");
    if (identities) {
        auto restored = detail::ProjectStateAccess::restore_identities(
            std::move(created).value(), std::move(decoded_identities));
        if (!restored)
            return model_fail<Project>(restored.error(), "/data/identities");
        created = std::move(restored);
    }
    return runtime::Result<Project, PersistenceError>(runtime::Ok(std::move(created).value()));
}

} // namespace pulp::timeline
