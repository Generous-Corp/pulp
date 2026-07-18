#include <pulp/timeline/serialize.hpp>

#include <algorithm>
#include <limits>

namespace pulp::timeline {
namespace {

template <typename T>
runtime::Result<T, PersistenceError> fail(PersistenceErrorCode code,
                                          std::string path = {},
                                          std::size_t byte_offset = 0,
                                          std::uint64_t actual = 0,
                                          std::uint64_t limit = 0) {
    return runtime::Result<T, PersistenceError>(runtime::Err(PersistenceError{
        code, byte_offset, actual, limit, std::move(path), std::nullopt}));
}

template <typename T>
runtime::Result<T, PersistenceError> model_fail(ModelError error, std::string path) {
    PersistenceError failure;
    failure.code = PersistenceErrorCode::ModelRejected;
    failure.path = std::move(path);
    failure.model_error = error;
    return runtime::Result<T, PersistenceError>(runtime::Err(std::move(failure)));
}

std::string object(std::initializer_list<std::pair<std::string_view, std::string>> fields) {
    std::vector<std::pair<std::string_view, std::string>> sorted(fields.begin(), fields.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first < rhs.first;
    });
    std::string result = "{";
    for (std::size_t index = 0; index < sorted.size(); ++index) {
        if (index != 0) result.push_back(',');
        result += quote_json_string(sorted[index].first);
        result.push_back(':');
        result += sorted[index].second;
    }
    result.push_back('}');
    return result;
}

std::string array(const std::vector<std::string>& elements) {
    std::string result = "[";
    for (std::size_t index = 0; index < elements.size(); ++index) {
        if (index != 0) result.push_back(',');
        result += elements[index];
    }
    result.push_back(']');
    return result;
}

std::string wide(std::uint64_t value) { return quote_json_string(std::to_string(value)); }
std::string wide(std::int64_t value) { return quote_json_string(std::to_string(value)); }
std::string small(std::uint32_t value) { return std::to_string(value); }

std::string envelope(std::string data, std::string_view type_name, std::uint32_t version) {
    return object({{"data", std::move(data)},
                   {"type_name", quote_json_string(type_name)},
                   {"version", small(version)}});
}

const char* storage_name(AssetStoragePolicy value) noexcept {
    switch (value) {
        case AssetStoragePolicy::External: return "external";
        case AssetStoragePolicy::Embedded: return "embedded";
        case AssetStoragePolicy::PreferEmbedded: return "prefer_embedded";
    }
    return "external";
}

const char* locator_name(AssetLocatorKind value) noexcept {
    return value == AssetLocatorKind::PackageRelative ? "package_relative" : "external_uri";
}

std::string encode_rate(timebase::RationalRate rate) {
    return object({{"denominator", wide(rate.denominator)},
                   {"numerator", wide(rate.numerator)}});
}

std::string encode_locator(const AssetLocator& locator) {
    return object({{"hint", quote_json_string(locator.hint)},
                   {"kind", quote_json_string(locator_name(locator.kind))}});
}

std::string encode_representation(const AssetRepresentation& representation) {
    std::vector<std::string> locators;
    locators.reserve(representation.locators.size());
    for (const auto& locator : representation.locators)
        locators.push_back(encode_locator(locator));
    const auto data = object({{"content_hash", quote_json_string(representation.content_hash.to_hex())},
                              {"locators", array(locators)},
                              {"role", quote_json_string(representation.role)},
                              {"storage_policy", quote_json_string(
                                                     storage_name(representation.storage_policy))}});
    return envelope(data, "pulp.timeline.asset_representation", 1);
}

std::string encode_asset(const MediaAsset& asset) {
    std::vector<std::string> locators;
    for (const auto& locator : asset.locators) locators.push_back(encode_locator(locator));
    std::vector<std::string> representations;
    for (const auto& representation : asset.representations)
        representations.push_back(encode_representation(representation));
    const auto data = object({{"content_hash", quote_json_string(asset.content_hash.to_hex())},
                              {"frame_count", wide(asset.frame_count)},
                              {"id", wide(asset.id.value)},
                              {"locators", array(locators)},
                              {"name", quote_json_string(asset.name)},
                              {"representations", array(representations)},
                              {"sample_rate", encode_rate(asset.sample_rate)},
                              {"storage_policy", quote_json_string(
                                                     storage_name(asset.storage_policy))}});
    return envelope(data, "pulp.timeline.asset", 1);
}

runtime::Result<std::string, PersistenceError>
encode_content(const ClipContent& content, const SchemaRegistry& registry, bool& opaque) {
    if (std::holds_alternative<EmptyContent>(content))
        return runtime::Result<std::string, PersistenceError>(runtime::Ok(
            envelope("{}", "pulp.timeline.content.empty", 1)));
    if (const auto* media = std::get_if<MediaRef>(&content)) {
        const auto data = object({{"asset_id", wide(media->asset_id.value)},
                                  {"frame_count", wide(media->frame_count)},
                                  {"source_start", wide(media->source_start.value)}});
        return runtime::Result<std::string, PersistenceError>(runtime::Ok(
            envelope(data, "pulp.timeline.content.media", 1)));
    }
    if (const auto* note_content = std::get_if<NoteContent>(&content)) {
        std::vector<std::string> notes;
        notes.reserve(note_content->notes().size());
        for (const auto& note : note_content->notes()) {
            notes.push_back(object({{"channel", small(note.channel)},
                                    {"duration_ticks", wide(note.duration.value)},
                                    {"id", wide(note.id.value)},
                                    {"pitch", small(note.pitch)},
                                    {"start_ticks", wide(note.start.value)},
                                    {"velocity", small(note.velocity)}}));
        }
        return runtime::Result<std::string, PersistenceError>(runtime::Ok(
            envelope(object({{"notes", array(notes)}}), "pulp.timeline.content.notes", 1)));
    }
    if (const auto* registered = std::get_if<RegisteredContent>(&content)) {
        const auto* schema = registry.find(SchemaDomain::Content,
                                           registered->schema().type_name);
        if (!schema || !schema->codec.encode ||
            registered->schema().version != schema->current_version)
            return fail<std::string>(PersistenceErrorCode::InvalidSchema, "/content");
        auto data = schema->codec.encode(registered->value(), schema->codec.context.get());
        if (!data) return data;
        DecodeLimits validation_limits;
        validation_limits.max_input_bytes = data.value().size();
        auto parsed = parse_json(data.value(), validation_limits);
        if (!parsed || parsed.value()->root().kind != JsonValue::Kind::Object)
            return fail<std::string>(PersistenceErrorCode::InvalidSchema, "/content/data");
        auto canonical = canonicalize_json(parsed.value()->root());
        if (!canonical) return canonical;
        return runtime::Result<std::string, PersistenceError>(runtime::Ok(
            envelope(std::move(canonical).value(), schema->type_name, schema->current_version)));
    }
    const auto& unknown = std::get<OpaqueContent>(content);
    DecodeLimits validation_limits;
    validation_limits.max_input_bytes = unknown.raw_json().size();
    validation_limits.max_opaque_bytes = unknown.raw_json().size();
    auto parsed = parse_json(unknown.raw_json(), validation_limits);
    if (!parsed || parsed.value()->root().kind != JsonValue::Kind::Object)
        return fail<std::string>(PersistenceErrorCode::InvalidSchema, "/content");
    const auto* type = parsed.value()->root().find("type_name");
    const auto* version = parsed.value()->root().find("version");
    const auto* data = parsed.value()->root().find("data");
    if (!type || type->kind != JsonValue::Kind::String ||
        type->scalar != unknown.schema().type_name || !version || !data)
        return fail<std::string>(PersistenceErrorCode::InvalidSchema, "/content");
    auto parsed_version = parse_u32_number(*version, "/content/version");
    if (!parsed_version || parsed_version.value() != unknown.schema().version)
        return fail<std::string>(PersistenceErrorCode::InvalidSchema, "/content/version");
    opaque = true;
    return runtime::Result<std::string, PersistenceError>(
        runtime::Ok(unknown.raw_json()));
}

runtime::Result<std::string, PersistenceError>
encode_clip(const Clip& clip, const SchemaRegistry& registry, bool& opaque) {
    auto content = encode_content(clip.content(), registry, opaque);
    if (!content) return content;
    std::string range;
    if (clip.time_anchor() == ClipTimeAnchor::Musical) {
        range = object({{"duration_ticks", wide(clip.duration().value)},
                        {"kind", quote_json_string("musical")},
                        {"start_ticks", wide(clip.start().value)}});
    } else {
        range = object({{"kind", quote_json_string("absolute")},
                        {"sample_count", wide(clip.absolute_duration_samples())},
                        {"sample_rate", encode_rate(clip.absolute_sample_rate())},
                        {"start_sample", wide(clip.absolute_start().value)}});
    }
    return runtime::Result<std::string, PersistenceError>(runtime::Ok(envelope(
        object({{"content", std::move(content).value()},
                {"id", wide(clip.id().value)}, {"time_range", std::move(range)}}),
        "pulp.timeline.clip", 1)));
}

runtime::Result<std::string, PersistenceError>
encode_track(const Track& track, const SchemaRegistry& registry, bool& opaque) {
    std::vector<std::string> clips;
    clips.reserve(track.clips().size());
    for (const auto& clip : track.clips()) {
        auto encoded = encode_clip(clip, registry, opaque);
        if (!encoded) return encoded;
        clips.push_back(std::move(encoded).value());
    }
    return runtime::Result<std::string, PersistenceError>(runtime::Ok(envelope(
        object({{"clips", array(clips)}, {"id", wide(track.id().value)},
                {"name", quote_json_string(track.name())}}),
        "pulp.timeline.track", 1)));
}

runtime::Result<std::string, PersistenceError>
encode_sequence(const Sequence& sequence, const SchemaRegistry& registry, bool& opaque) {
    std::vector<std::string> tracks;
    for (const auto& track : sequence.tracks()) {
        auto encoded = encode_track(track, registry, opaque);
        if (!encoded) return encoded;
        tracks.push_back(std::move(encoded).value());
    }
    std::string absolute = "null";
    if (sequence.absolute_duration())
        absolute = object({{"sample_count", wide(sequence.absolute_duration()->sample_count)},
                           {"sample_rate", encode_rate(sequence.absolute_duration()->sample_rate)}});
    const auto musical = sequence.duration() ? wide(sequence.duration()->value) : "null";
    return runtime::Result<std::string, PersistenceError>(runtime::Ok(envelope(
        object({{"absolute_duration", std::move(absolute)},
                {"id", wide(sequence.id().value)},
                {"musical_duration", musical},
                {"name", quote_json_string(sequence.name())},
                {"tracks", array(tracks)}}),
        "pulp.timeline.sequence", 1)));
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
    if (!value) return fail<std::string>(value.error().code, value.error().path,
                                         value.error().byte_offset);
    if (value.value()->kind != JsonValue::Kind::String)
        return fail<std::string>(PersistenceErrorCode::UnexpectedType,
                                 path + "/" + std::string(name), value.value()->begin);
    return runtime::Result<std::string, PersistenceError>(
        runtime::Ok(value.value()->scalar));
}

runtime::Result<const JsonValue*, PersistenceError>
data_for(const JsonValue& value, std::string_view expected_type, std::string path) {
    auto type = string_field(value, "type_name", path);
    auto version = required(value, "version", path);
    auto data = required(value, "data", path);
    if (!type) return fail<const JsonValue*>(type.error().code, type.error().path,
                                             type.error().byte_offset);
    if (!version) return version;
    if (!data) return data;
    auto decoded_version = parse_u32_number(*version.value(), path + "/version");
    if (type.value() != expected_type)
        return fail<const JsonValue*>(PersistenceErrorCode::UnsupportedStructuralType,
                                      std::move(path), value.begin);
    if (!decoded_version || decoded_version.value() != 1)
        return fail<const JsonValue*>(PersistenceErrorCode::UnsupportedSchemaVersion,
                                      std::move(path), value.begin);
    if (data.value()->kind != JsonValue::Kind::Object)
        return fail<const JsonValue*>(PersistenceErrorCode::UnexpectedType,
                                      path + "/data", data.value()->begin);
    return data;
}

runtime::Result<timebase::RationalRate, PersistenceError>
decode_rate(const JsonValue& value, std::string path) {
    auto numerator = required(value, "numerator", path);
    auto denominator = required(value, "denominator", path);
    if (!numerator) return fail<timebase::RationalRate>(numerator.error().code,
                                                        numerator.error().path,
                                                        numerator.error().byte_offset);
    if (!denominator) return fail<timebase::RationalRate>(denominator.error().code,
                                                          denominator.error().path,
                                                          denominator.error().byte_offset);
    auto n = parse_canonical_u64_string(*numerator.value(), path + "/numerator");
    auto d = parse_canonical_u64_string(*denominator.value(), path + "/denominator");
    if (!n) return fail<timebase::RationalRate>(n.error().code, n.error().path,
                                                n.error().byte_offset);
    if (!d) return fail<timebase::RationalRate>(d.error().code, d.error().path,
                                                d.error().byte_offset);
    const timebase::RationalRate rate{n.value(), d.value()};
    if (!rate.valid() || rate.normalized() != rate)
        return fail<timebase::RationalRate>(PersistenceErrorCode::InvalidNumber,
                                            std::move(path), value.begin);
    return runtime::Result<timebase::RationalRate, PersistenceError>(runtime::Ok(rate));
}

runtime::Result<AssetStoragePolicy, PersistenceError>
decode_storage(std::string_view value, std::string path) {
    if (value == "external") return runtime::Result<AssetStoragePolicy, PersistenceError>(
        runtime::Ok(AssetStoragePolicy::External));
    if (value == "embedded") return runtime::Result<AssetStoragePolicy, PersistenceError>(
        runtime::Ok(AssetStoragePolicy::Embedded));
    if (value == "prefer_embedded") return runtime::Result<AssetStoragePolicy, PersistenceError>(
        runtime::Ok(AssetStoragePolicy::PreferEmbedded));
    return fail<AssetStoragePolicy>(PersistenceErrorCode::InvalidSchema, std::move(path));
}

runtime::Result<std::vector<AssetLocator>, PersistenceError>
decode_locators(const JsonValue& value, std::string path) {
    if (value.kind != JsonValue::Kind::Array)
        return fail<std::vector<AssetLocator>>(PersistenceErrorCode::UnexpectedType,
                                               std::move(path), value.begin);
    std::vector<AssetLocator> result;
    result.reserve(value.array.size());
    for (std::size_t index = 0; index < value.array.size(); ++index) {
        const auto member_path = path + "/" + std::to_string(index);
        auto kind = string_field(value.array[index], "kind", member_path);
        auto hint = string_field(value.array[index], "hint", member_path);
        if (!kind) return fail<std::vector<AssetLocator>>(kind.error().code, kind.error().path,
                                                          kind.error().byte_offset);
        if (!hint) return fail<std::vector<AssetLocator>>(hint.error().code, hint.error().path,
                                                          hint.error().byte_offset);
        AssetLocatorKind decoded_kind;
        if (kind.value() == "package_relative") decoded_kind = AssetLocatorKind::PackageRelative;
        else if (kind.value() == "external_uri") decoded_kind = AssetLocatorKind::ExternalUri;
        else return fail<std::vector<AssetLocator>>(PersistenceErrorCode::InvalidSchema,
                                                    member_path + "/kind");
        result.push_back({decoded_kind, std::move(hint).value()});
    }
    return runtime::Result<std::vector<AssetLocator>, PersistenceError>(
        runtime::Ok(std::move(result)));
}

runtime::Result<AssetRepresentation, PersistenceError>
decode_representation(const JsonValue& value, std::string path) {
    auto data = data_for(value, "pulp.timeline.asset_representation", path);
    if (!data) return fail<AssetRepresentation>(data.error().code, data.error().path,
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
    return runtime::Result<AssetRepresentation, PersistenceError>(runtime::Ok(AssetRepresentation{
        std::move(role).value(), *decoded_hash, decoded_policy.value(),
        std::move(decoded_locators).value()}));
}

runtime::Result<MediaAsset, PersistenceError>
decode_asset(const JsonValue& value, std::string path, DecodeCounts& counts,
             const DecodeLimits& limits) {
    if (++counts.assets > limits.max_assets)
        return fail<MediaAsset>(PersistenceErrorCode::LimitExceeded, path, value.begin,
                                counts.assets, limits.max_assets);
    auto data = data_for(value, "pulp.timeline.asset", path);
    if (!data) return fail<MediaAsset>(data.error().code, data.error().path,
                                      data.error().byte_offset);
    auto id = required(*data.value(), "id", path + "/data");
    auto name = string_field(*data.value(), "name", path + "/data");
    auto frame_count = required(*data.value(), "frame_count", path + "/data");
    auto rate = required(*data.value(), "sample_rate", path + "/data");
    auto hash = string_field(*data.value(), "content_hash", path + "/data");
    auto policy = string_field(*data.value(), "storage_policy", path + "/data");
    auto locators = required(*data.value(), "locators", path + "/data");
    auto representations = required(*data.value(), "representations", path + "/data");
    if (!id || !name || !frame_count || !rate || !hash || !policy || !locators ||
        !representations)
        return fail<MediaAsset>(PersistenceErrorCode::MissingField, std::move(path));
    auto decoded_id = parse_canonical_u64_string(*id.value(), path + "/data/id");
    auto decoded_frames = parse_canonical_u64_string(*frame_count.value(),
                                                      path + "/data/frame_count");
    auto decoded_rate = decode_rate(*rate.value(), path + "/data/sample_rate");
    auto decoded_hash = ContentHash::from_hex(hash.value());
    auto decoded_policy = decode_storage(policy.value(), path + "/data/storage_policy");
    auto decoded_locators = decode_locators(*locators.value(), path + "/data/locators");
    if (!decoded_id || !decoded_frames || !decoded_rate || !decoded_hash || !decoded_policy ||
        !decoded_locators || representations.value()->kind != JsonValue::Kind::Array)
        return fail<MediaAsset>(PersistenceErrorCode::InvalidSchema, std::move(path));
    std::vector<AssetRepresentation> decoded_representations;
    for (std::size_t index = 0; index < representations.value()->array.size(); ++index) {
        auto decoded = decode_representation(representations.value()->array[index],
                                             path + "/data/representations/" +
                                                 std::to_string(index));
        if (!decoded) return fail<MediaAsset>(decoded.error().code, decoded.error().path,
                                              decoded.error().byte_offset);
        decoded_representations.push_back(std::move(decoded).value());
    }
    return runtime::Result<MediaAsset, PersistenceError>(runtime::Ok(MediaAsset{
        ItemId{decoded_id.value()}, std::move(name).value(), decoded_frames.value(),
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
        auto opaque = OpaqueContent::create({type.value(), version.value()}, std::string(raw));
        if (!opaque)
            return model_fail<ClipContent>(opaque.error(), std::move(path));
        return runtime::Result<ClipContent, PersistenceError>(
            runtime::Ok(ClipContent(std::move(opaque).value())));
    }
    if (version.value() < schema->current_version) {
        auto migrated = registry.migrate(SchemaDomain::Content, type.value(), version.value(),
                                         schema->current_version, document->raw(value), limits);
        if (!migrated) return fail<ClipContent>(migrated.error().code, std::move(path));
        auto parsed = parse_json(migrated.value(), limits);
        if (!parsed) return fail<ClipContent>(parsed.error().code, std::move(path));
        return decode_content(parsed.value(), parsed.value()->root(), registry, limits, counts,
                              std::move(path));
    }
    auto data = required(value, "data", path);
    if (!data || data.value()->kind != JsonValue::Kind::Object)
        return fail<ClipContent>(PersistenceErrorCode::UnexpectedType, path + "/data");
    if (type.value() == "pulp.timeline.content.empty")
        return runtime::Result<ClipContent, PersistenceError>(runtime::Ok(ClipContent(EmptyContent{})));
    if (type.value() == "pulp.timeline.content.media") {
        auto asset = required(*data.value(), "asset_id", path + "/data");
        auto source = required(*data.value(), "source_start", path + "/data");
        auto frames = required(*data.value(), "frame_count", path + "/data");
        if (!asset || !source || !frames)
            return fail<ClipContent>(PersistenceErrorCode::MissingField, std::move(path));
        auto decoded_asset = parse_canonical_u64_string(*asset.value(), path + "/data/asset_id");
        auto decoded_source = parse_canonical_i64_string(*source.value(), path + "/data/source_start");
        auto decoded_frames = parse_canonical_u64_string(*frames.value(), path + "/data/frame_count");
        if (!decoded_asset || !decoded_source || !decoded_frames)
            return fail<ClipContent>(PersistenceErrorCode::InvalidNumber, std::move(path));
        return runtime::Result<ClipContent, PersistenceError>(runtime::Ok(ClipContent(MediaRef{
            ItemId{decoded_asset.value()}, timebase::SamplePosition{decoded_source.value()},
            decoded_frames.value()})));
    }
    if (type.value() == "pulp.timeline.content.notes") {
        auto notes = required(*data.value(), "notes", path + "/data");
        if (!notes || notes.value()->kind != JsonValue::Kind::Array)
            return fail<ClipContent>(PersistenceErrorCode::UnexpectedType, path + "/data/notes");
        if (counts.notes > limits.max_notes ||
            notes.value()->array.size() > limits.max_notes - counts.notes)
            return fail<ClipContent>(PersistenceErrorCode::LimitExceeded, path + "/data/notes",
                                     notes.value()->begin, counts.notes + notes.value()->array.size(),
                                     limits.max_notes);
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
            auto decoded_start = parse_canonical_i64_string(*start.value(), note_path + "/start_ticks");
            auto decoded_duration = parse_canonical_i64_string(*duration.value(),
                                                                note_path + "/duration_ticks");
            auto decoded_velocity = parse_u32_number(*velocity.value(), note_path + "/velocity");
            auto decoded_pitch = parse_u32_number(*pitch.value(), note_path + "/pitch");
            auto decoded_channel = parse_u32_number(*channel.value(), note_path + "/channel");
            if (!decoded_id || !decoded_start || !decoded_duration || !decoded_velocity ||
                !decoded_pitch || !decoded_channel ||
                decoded_velocity.value() > std::numeric_limits<std::uint16_t>::max() ||
                decoded_pitch.value() > std::numeric_limits<std::uint8_t>::max() ||
                decoded_channel.value() > std::numeric_limits<std::uint8_t>::max())
                return fail<ClipContent>(PersistenceErrorCode::InvalidNumber, note_path);
            events.push_back({ItemId{decoded_id.value()}, {decoded_start.value()},
                              {decoded_duration.value()},
                              static_cast<std::uint16_t>(decoded_velocity.value()),
                              static_cast<std::uint8_t>(decoded_pitch.value()),
                              static_cast<std::uint8_t>(decoded_channel.value())});
        }
        counts.notes += events.size();
        auto created = NoteContent::create(std::move(events));
        if (!created) return model_fail<ClipContent>(created.error(), std::move(path));
        return runtime::Result<ClipContent, PersistenceError>(
            runtime::Ok(ClipContent(std::move(created).value())));
    }
    if (!schema->codec.decode)
        return fail<ClipContent>(PersistenceErrorCode::InvalidSchema, std::move(path));
    auto payload = schema->codec.decode(*data.value(), schema->codec.context.get());
    if (!payload) return fail<ClipContent>(payload.error().code, std::move(path));
    auto registered = RegisteredContent::create_no_owned_ids(
        {schema->type_name, schema->current_version}, std::move(payload).value());
    if (!registered) return model_fail<ClipContent>(registered.error(), std::move(path));
    return runtime::Result<ClipContent, PersistenceError>(
        runtime::Ok(ClipContent(std::move(registered).value())));
}

runtime::Result<Clip, PersistenceError>
decode_clip(const std::shared_ptr<const ParsedJson>& document, const JsonValue& value,
            const SchemaRegistry& registry, const DecodeLimits& limits, DecodeCounts& counts,
            std::string path) {
    if (++counts.clips > limits.max_clips)
        return fail<Clip>(PersistenceErrorCode::LimitExceeded, path, value.begin,
                          counts.clips, limits.max_clips);
    auto data = data_for(value, "pulp.timeline.clip", path);
    if (!data) return fail<Clip>(data.error().code, data.error().path, data.error().byte_offset);
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
    runtime::Result<Clip, ModelError> created(runtime::Err(ModelError{}));
    if (kind.value() == "musical") {
        auto start = required(*range.value(), "start_ticks", path + "/data/time_range");
        auto duration = required(*range.value(), "duration_ticks", path + "/data/time_range");
        if (!start || !duration) return fail<Clip>(PersistenceErrorCode::MissingField, path);
        auto decoded_start = parse_canonical_i64_string(
            *start.value(), path + "/data/time_range/start_ticks");
        auto decoded_duration = parse_canonical_i64_string(
            *duration.value(), path + "/data/time_range/duration_ticks");
        if (!decoded_start || !decoded_duration)
            return fail<Clip>(PersistenceErrorCode::InvalidNumber, path);
        created = Clip::create({decoded_id.value()}, {decoded_start.value()},
                               {decoded_duration.value()}, std::move(content).value());
    } else if (kind.value() == "absolute") {
        auto start = required(*range.value(), "start_sample", path + "/data/time_range");
        auto count = required(*range.value(), "sample_count", path + "/data/time_range");
        auto rate = required(*range.value(), "sample_rate", path + "/data/time_range");
        if (!start || !count || !rate) return fail<Clip>(PersistenceErrorCode::MissingField, path);
        auto decoded_start = parse_canonical_i64_string(
            *start.value(), path + "/data/time_range/start_sample");
        auto decoded_count = parse_canonical_u64_string(
            *count.value(), path + "/data/time_range/sample_count");
        auto decoded_rate = decode_rate(*rate.value(), path + "/data/time_range/sample_rate");
        if (!decoded_start || !decoded_count || !decoded_rate)
            return fail<Clip>(PersistenceErrorCode::InvalidNumber, path);
        created = Clip::create_absolute({decoded_id.value()}, {decoded_start.value()}, decoded_count.value(),
                                        decoded_rate.value(), std::move(content).value());
    } else {
        return fail<Clip>(PersistenceErrorCode::InvalidSchema, path + "/data/time_range/kind");
    }
    if (!created) return model_fail<Clip>(created.error(), std::move(path));
    return runtime::Result<Clip, PersistenceError>(runtime::Ok(std::move(created).value()));
}

runtime::Result<Track, PersistenceError>
decode_track(const std::shared_ptr<const ParsedJson>& document, const JsonValue& value,
             const SchemaRegistry& registry, const DecodeLimits& limits, DecodeCounts& counts,
             std::string path) {
    if (++counts.tracks > limits.max_tracks)
        return fail<Track>(PersistenceErrorCode::LimitExceeded, path, value.begin,
                           counts.tracks, limits.max_tracks);
    auto data = data_for(value, "pulp.timeline.track", path);
    if (!data) return fail<Track>(data.error().code, data.error().path, data.error().byte_offset);
    auto id = required(*data.value(), "id", path + "/data");
    auto name = string_field(*data.value(), "name", path + "/data");
    auto clips = required(*data.value(), "clips", path + "/data");
    if (!id || !name || !clips || clips.value()->kind != JsonValue::Kind::Array)
        return fail<Track>(PersistenceErrorCode::MissingField, std::move(path));
    auto decoded_id = parse_canonical_u64_string(*id.value(), path + "/data/id");
    if (!decoded_id) return fail<Track>(decoded_id.error().code, decoded_id.error().path,
                                        decoded_id.error().byte_offset);
    std::vector<Clip> decoded_clips;
    for (std::size_t index = 0; index < clips.value()->array.size(); ++index) {
        auto decoded = decode_clip(document, clips.value()->array[index], registry, limits, counts,
                                   path + "/data/clips/" + std::to_string(index));
        if (!decoded) return fail<Track>(decoded.error().code, decoded.error().path,
                                         decoded.error().byte_offset);
        decoded_clips.push_back(std::move(decoded).value());
    }
    auto created = Track::create({decoded_id.value()}, std::move(name).value(),
                                 std::move(decoded_clips));
    if (!created) return model_fail<Track>(created.error(), std::move(path));
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
    if (!data) return fail<Sequence>(data.error().code, data.error().path,
                                     data.error().byte_offset);
    auto id = required(*data.value(), "id", path + "/data");
    auto name = string_field(*data.value(), "name", path + "/data");
    auto tracks = required(*data.value(), "tracks", path + "/data");
    auto musical = required(*data.value(), "musical_duration", path + "/data");
    auto absolute = required(*data.value(), "absolute_duration", path + "/data");
    if (!id || !name || !tracks || !musical || !absolute ||
        tracks.value()->kind != JsonValue::Kind::Array)
        return fail<Sequence>(PersistenceErrorCode::MissingField, std::move(path));
    auto decoded_id = parse_canonical_u64_string(*id.value(), path + "/data/id");
    if (!decoded_id) return fail<Sequence>(decoded_id.error().code, decoded_id.error().path,
                                           decoded_id.error().byte_offset);
    std::optional<timebase::TickDuration> decoded_musical;
    if (musical.value()->kind != JsonValue::Kind::Null) {
        auto parsed = parse_canonical_i64_string(*musical.value(), path + "/data/musical_duration");
        if (!parsed) return fail<Sequence>(parsed.error().code, parsed.error().path,
                                           parsed.error().byte_offset);
        decoded_musical = timebase::TickDuration{parsed.value()};
    }
    std::optional<AbsoluteTimelineDuration> decoded_absolute;
    if (absolute.value()->kind != JsonValue::Kind::Null) {
        auto count = required(*absolute.value(), "sample_count", path + "/data/absolute_duration");
        auto rate = required(*absolute.value(), "sample_rate", path + "/data/absolute_duration");
        if (!count || !rate) return fail<Sequence>(PersistenceErrorCode::MissingField, path);
        auto decoded_count = parse_canonical_u64_string(
            *count.value(), path + "/data/absolute_duration/sample_count");
        auto decoded_rate = decode_rate(*rate.value(), path + "/data/absolute_duration/sample_rate");
        if (!decoded_count || !decoded_rate) return fail<Sequence>(PersistenceErrorCode::InvalidNumber, path);
        decoded_absolute = AbsoluteTimelineDuration{decoded_count.value(), decoded_rate.value()};
    }
    std::vector<Track> decoded_tracks;
    for (std::size_t index = 0; index < tracks.value()->array.size(); ++index) {
        auto decoded = decode_track(document, tracks.value()->array[index], registry, limits, counts,
                                    path + "/data/tracks/" + std::to_string(index));
        if (!decoded) return fail<Sequence>(decoded.error().code, decoded.error().path,
                                            decoded.error().byte_offset);
        decoded_tracks.push_back(std::move(decoded).value());
    }
    auto created = Sequence::create({decoded_id.value()}, std::move(name).value(), decoded_musical,
                                    decoded_absolute, std::move(decoded_tracks));
    if (!created) return model_fail<Sequence>(created.error(), std::move(path));
    return runtime::Result<Sequence, PersistenceError>(runtime::Ok(std::move(created).value()));
}

} // namespace

runtime::Result<SerializedSnapshot, PersistenceError>
serialize_project(const Project& project, const SchemaRegistry& registry,
                  const SerializeOptions& options) {
    if (!registry.find(SchemaDomain::Document, "pulp.timeline.project"))
        return fail<SerializedSnapshot>(PersistenceErrorCode::InvalidSchema, "/");
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
                                                asset_path + "/locators/" +
                                                    std::to_string(index) + "/hint");
        for (std::size_t representation_index = 0;
             representation_index < asset.representations.size(); ++representation_index) {
            const auto& representation = asset.representations[representation_index];
            const auto representation_path = asset_path + "/representations/" +
                                             std::to_string(representation_index) + "/data";
            if (!is_valid_utf8(representation.role))
                return fail<SerializedSnapshot>(PersistenceErrorCode::InvalidUtf8,
                                                representation_path + "/role");
            for (std::size_t index = 0; index < representation.locators.size(); ++index)
                if (!is_valid_utf8(representation.locators[index].hint))
                    return fail<SerializedSnapshot>(
                        PersistenceErrorCode::InvalidUtf8,
                        representation_path + "/locators/" + std::to_string(index) + "/hint");
        }
    }
    for (std::size_t sequence_index = 0; sequence_index < project.sequences().size();
         ++sequence_index) {
        const auto& sequence = project.sequences()[sequence_index];
        const auto sequence_path =
            "/data/sequences/" + std::to_string(sequence_index) + "/data";
        if (!is_valid_utf8(sequence.name()))
            return fail<SerializedSnapshot>(PersistenceErrorCode::InvalidUtf8,
                                            sequence_path + "/name");
        for (std::size_t track_index = 0; track_index < sequence.tracks().size(); ++track_index)
            if (!is_valid_utf8(sequence.tracks()[track_index].name()))
                return fail<SerializedSnapshot>(
                    PersistenceErrorCode::InvalidUtf8,
                    sequence_path + "/tracks/" + std::to_string(track_index) + "/data/name");
    }
    bool opaque = false;
    std::vector<std::string> assets;
    for (const auto& asset : project.assets()) assets.push_back(encode_asset(asset));
    std::vector<std::string> sequences;
    for (const auto& sequence : project.sequences()) {
        auto encoded = encode_sequence(sequence, registry, opaque);
        if (!encoded) return fail<SerializedSnapshot>(encoded.error().code, encoded.error().path,
                                                       encoded.error().byte_offset);
        sequences.push_back(std::move(encoded).value());
    }
    auto json = envelope(object({{"assets", array(assets)},
                                 {"id", wide(project.id().value)},
                                 {"name", quote_json_string(project.name())},
                                 {"next_item_id", wide(project.next_item_id())},
                                 {"root_sequence_id", wide(project.root_sequence_id().value)},
                                 {"sequences", array(sequences)}}),
                         "pulp.timeline.project", 1);
    if (json.size() > options.max_output_bytes)
        return fail<SerializedSnapshot>(PersistenceErrorCode::OutputLimitExceeded, "/", 0,
                                        json.size(), options.max_output_bytes);
    return runtime::Result<SerializedSnapshot, PersistenceError>(
        runtime::Ok(SerializedSnapshot{std::move(json), opaque}));
}

runtime::Result<Project, PersistenceError>
deserialize_project(std::string_view json, const SchemaRegistry& registry,
                    const DecodeLimits& limits) {
    auto parsed = parse_json(json, limits);
    if (!parsed) return fail<Project>(parsed.error().code, parsed.error().path,
                                      parsed.error().byte_offset, parsed.error().actual,
                                      parsed.error().limit);
    auto data = data_for(parsed.value()->root(), "pulp.timeline.project", "");
    if (!data) return fail<Project>(data.error().code, data.error().path,
                                    data.error().byte_offset);
    if (!registry.find(SchemaDomain::Document, "pulp.timeline.project"))
        return fail<Project>(PersistenceErrorCode::InvalidSchema, "/");
    auto id = required(*data.value(), "id", "/data");
    auto name = string_field(*data.value(), "name", "/data");
    auto next = required(*data.value(), "next_item_id", "/data");
    auto root = required(*data.value(), "root_sequence_id", "/data");
    auto assets = required(*data.value(), "assets", "/data");
    auto sequences = required(*data.value(), "sequences", "/data");
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
        if (!decoded) return fail<Project>(decoded.error().code, decoded.error().path,
                                           decoded.error().byte_offset);
        decoded_assets.push_back(std::move(decoded).value());
    }
    std::vector<Sequence> decoded_sequences;
    for (std::size_t index = 0; index < sequences.value()->array.size(); ++index) {
        auto decoded = decode_sequence(parsed.value(), sequences.value()->array[index], registry,
                                       limits, counts,
                                       "/data/sequences/" + std::to_string(index));
        if (!decoded) return fail<Project>(decoded.error().code, decoded.error().path,
                                           decoded.error().byte_offset);
        decoded_sequences.push_back(std::move(decoded).value());
    }
    auto created = Project::create(ProjectInput{{decoded_id.value()}, std::move(name).value(),
                                                 decoded_next.value(), {decoded_root.value()},
                                                 std::move(decoded_assets),
                                                 std::move(decoded_sequences)});
    if (!created) return model_fail<Project>(created.error(), "/");
    return runtime::Result<Project, PersistenceError>(runtime::Ok(std::move(created).value()));
}

} // namespace pulp::timeline
