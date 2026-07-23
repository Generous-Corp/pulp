#include <pulp/timeline/serialize.hpp>

#include "asset_schema_policy.hpp"
#include "project_state_access.hpp"
#include "serialize_asset_loop_decode.hpp"
#include "serialize_automation_decode.hpp"
#include "serialize_decode_support.hpp"
#include "serialize_internal.hpp"
#include "track_schema_policy.hpp"

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
    if (value == "device_placement")
        return runtime::Ok(ItemKind::DevicePlacement);
    if (value == "automation_lane")
        return runtime::Ok(ItemKind::AutomationLane);
    if (value == "automation_point")
        return runtime::Ok(ItemKind::AutomationPoint);
    if (value == "take_lane")
        return runtime::Ok(ItemKind::TakeLane);
    if (value == "take")
        return runtime::Ok(ItemKind::Take);
    return fail<ItemKind>(PersistenceErrorCode::InvalidSchema, std::move(path));
}

struct DecodeCounts {
    std::size_t assets = 0;
    std::size_t audio_loop_points = 0;
    std::size_t audio_loop_tags = 0;
    std::size_t sequences = 0;
    std::size_t tracks = 0;
    std::size_t clips = 0;
    std::size_t notes = 0;
    std::size_t device_placements = 0;
    std::size_t automation_lanes = 0;
    std::size_t automation_points = 0;
    std::size_t take_lanes = 0;
    std::size_t takes = 0;
    std::size_t take_comp_segments = 0;
};

runtime::Result<const JsonValue*, PersistenceError>
required(const JsonValue& object_value, std::string_view name, std::string path) {
    return detail::required_decode_member(object_value, name, std::move(path));
}

runtime::Result<std::string, PersistenceError>
string_field(const JsonValue& object_value, std::string_view name, std::string path) {
    return detail::decode_string_field(object_value, name, std::move(path));
}

struct StructuralData {
    const JsonValue* data = nullptr;
    std::uint32_t version = 0;
};

runtime::Result<StructuralData, PersistenceError>
data_for_versions(const JsonValue& value, std::string_view expected_type,
                  std::uint32_t minimum_version, std::uint32_t maximum_version, std::string path) {
    auto type = string_field(value, "type_name", path);
    auto version = required(value, "version", path);
    auto data = required(value, "data", path);
    if (!type)
        return fail<StructuralData>(type.error().code, type.error().path, type.error().byte_offset);
    if (!version)
        return fail<StructuralData>(version.error().code, version.error().path,
                                    version.error().byte_offset);
    if (!data)
        return fail<StructuralData>(data.error().code, data.error().path, data.error().byte_offset);
    auto decoded_version = parse_u32_number(*version.value(), path + "/version");
    if (type.value() != expected_type)
        return fail<StructuralData>(PersistenceErrorCode::UnsupportedStructuralType,
                                    std::move(path), value.begin);
    if (!decoded_version || decoded_version.value() < minimum_version ||
        decoded_version.value() > maximum_version)
        return fail<StructuralData>(PersistenceErrorCode::UnsupportedSchemaVersion, std::move(path),
                                    value.begin);
    if (data.value()->kind != JsonValue::Kind::Object)
        return fail<StructuralData>(PersistenceErrorCode::UnexpectedType, path + "/data",
                                    data.value()->begin);
    return runtime::Ok(StructuralData{data.value(), decoded_version.value()});
}

runtime::Result<const JsonValue*, PersistenceError>
data_for(const JsonValue& value, std::string_view expected_type, std::string path) {
    auto decoded = data_for_versions(value, expected_type, 1, 1, std::move(path));
    if (!decoded)
        return fail<const JsonValue*>(decoded.error().code, decoded.error().path,
                                      decoded.error().byte_offset);
    return runtime::Ok(decoded.value().data);
}

runtime::Result<timebase::RationalRate, PersistenceError> decode_rate(const JsonValue& value,
                                                                      std::string path) {
    return detail::decode_rational_rate(value, std::move(path));
}

runtime::Result<timebase::TempoMap, PersistenceError> decode_tempo_map(const JsonValue& value,
                                                                       std::string path) {
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
        points.push_back(
            {{decoded_tick.value()}, std::bit_cast<double>(decoded_bits.value()), decoded_curve});
    }
    auto created = timebase::TempoMap::create(points);
    if (!created)
        return fail<timebase::TempoMap>(PersistenceErrorCode::InvalidSchema, std::move(path));
    return runtime::Ok(std::move(created).value());
}

runtime::Result<timebase::MeterMap, PersistenceError> decode_meter_map(const JsonValue& value,
                                                                       std::string path) {
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
        points.push_back(
            {{t.value()},
             {static_cast<std::int32_t>(n.value()), static_cast<std::int32_t>(d.value())}});
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
    auto envelope = data_for_versions(value, detail::asset_schema_policy.type_name,
                                      detail::asset_schema_policy.oldest_readable_version,
                                      detail::asset_schema_policy.current_version, path);
    if (!envelope)
        return fail<MediaAsset>(envelope.error().code, envelope.error().path,
                                envelope.error().byte_offset);
    const auto* data = envelope.value().data;
    auto id = required(*data, "id", path + "/data");
    auto name = string_field(*data, "name", path + "/data");
    auto frame_count = required(*data, "frame_count", path + "/data");
    auto rate = required(*data, "sample_rate", path + "/data");
    auto hash = string_field(*data, "content_hash", path + "/data");
    auto policy = string_field(*data, "storage_policy", path + "/data");
    auto locators = required(*data, "locators", path + "/data");
    auto representations = required(*data, "representations", path + "/data");
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
            return runtime::Err(decoded.error());
        decoded_representations.push_back(std::move(decoded).value());
    }
    std::optional<AudioLoopInfo> decoded_loop_info;
    if (const auto* loop_info = data->find("loop_info")) {
        if (!detail::asset_schema_policy.supports_loop_info(envelope.value().version))
            return fail<MediaAsset>(PersistenceErrorCode::InvalidSchema, path + "/data/loop_info");
        auto decoded = detail::decode_audio_loop_info(
            *loop_info, limits, counts.audio_loop_points, counts.audio_loop_tags,
            path + "/data/loop_info");
        if (!decoded)
            return runtime::Err(decoded.error());
        decoded_loop_info = std::move(decoded).value();
    }
    return runtime::Result<MediaAsset, PersistenceError>(
        runtime::Ok(MediaAsset{ItemId{decoded_id.value()}, std::move(name).value(),
                               decoded_frames.value(), decoded_rate.value(), *decoded_hash,
                               decoded_policy.value(), std::move(decoded_locators).value(),
                               std::move(decoded_representations), std::move(decoded_loop_info)}));
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

runtime::Result<Take, PersistenceError> decode_take(const JsonValue& value,
                                                    const DecodeLimits& limits,
                                                    DecodeCounts& counts, std::string path) {
    if (++counts.takes > limits.max_takes)
        return fail<Take>(PersistenceErrorCode::LimitExceeded, path, value.begin, counts.takes,
                          limits.max_takes);
    auto data = data_for(value, "pulp.timeline.take", path);
    if (!data)
        return fail<Take>(data.error().code, data.error().path, data.error().byte_offset);
    auto asset = required(*data.value(), "asset_id", path + "/data");
    auto frames = required(*data.value(), "frame_count", path + "/data");
    auto id = required(*data.value(), "id", path + "/data");
    auto placement = required(*data.value(), "placement_start", path + "/data");
    auto rate = required(*data.value(), "sample_rate", path + "/data");
    auto source = required(*data.value(), "source_start", path + "/data");
    if (!asset || !frames || !id || !placement || !rate || !source)
        return fail<Take>(PersistenceErrorCode::MissingField, std::move(path));
    auto decoded_asset = parse_canonical_u64_string(*asset.value(), path + "/data/asset_id");
    auto decoded_frames = parse_canonical_u64_string(*frames.value(), path + "/data/frame_count");
    auto decoded_id = parse_canonical_u64_string(*id.value(), path + "/data/id");
    auto decoded_placement =
        parse_canonical_i64_string(*placement.value(), path + "/data/placement_start");
    auto decoded_source = parse_canonical_i64_string(*source.value(), path + "/data/source_start");
    if (!decoded_asset || !decoded_frames || !decoded_id || !decoded_placement || !decoded_source)
        return fail<Take>(PersistenceErrorCode::InvalidNumber, std::move(path));
    auto decoded_rate = decode_rate(*rate.value(), path + "/data/sample_rate");
    if (!decoded_rate)
        return fail<Take>(decoded_rate.error().code, decoded_rate.error().path,
                          decoded_rate.error().byte_offset);
    auto created = Take::create(
        ItemId{decoded_id.value()},
        MediaRef{ItemId{decoded_asset.value()}, timebase::SamplePosition{decoded_source.value()},
                 decoded_frames.value()},
        timebase::SamplePosition{decoded_placement.value()}, decoded_rate.value());
    if (!created)
        return model_fail<Take>(created.error(), std::move(path));
    return runtime::Result<Take, PersistenceError>(runtime::Ok(std::move(created).value()));
}

runtime::Result<TakeLane, PersistenceError> decode_take_lane(const JsonValue& value,
                                                             const DecodeLimits& limits,
                                                             DecodeCounts& counts,
                 std::string path) {
    if (++counts.take_lanes > limits.max_take_lanes)
        return fail<TakeLane>(PersistenceErrorCode::LimitExceeded, path, value.begin,
                              counts.take_lanes, limits.max_take_lanes);
    auto structural = data_for_versions(value, "pulp.timeline.take_lane", 1, 2, path);
    if (!structural)
        return fail<TakeLane>(structural.error().code, structural.error().path,
                              structural.error().byte_offset);
    const auto* data = structural.value().data;
    auto id = required(*data, "id", path + "/data");
    auto name = string_field(*data, "name", path + "/data");
    auto takes = required(*data, "takes", path + "/data");
    if (!id || !name || !takes || takes.value()->kind != JsonValue::Kind::Array)
        return fail<TakeLane>(PersistenceErrorCode::MissingField, std::move(path));
    auto decoded_id = parse_canonical_u64_string(*id.value(), path + "/data/id");
    if (!decoded_id)
        return fail<TakeLane>(decoded_id.error().code, decoded_id.error().path,
                              decoded_id.error().byte_offset);
    std::vector<Take> decoded_takes;
    decoded_takes.reserve(takes.value()->array.size());
    for (std::size_t index = 0; index < takes.value()->array.size(); ++index) {
        auto decoded = decode_take(takes.value()->array[index], limits, counts,
                                   path + "/data/takes/" + std::to_string(index));
        if (!decoded)
            return runtime::Err(decoded.error());
        decoded_takes.push_back(std::move(decoded).value());
    }
    std::vector<TakeCompSegment> decoded_comp;
    if (structural.value().version == 2) {
        auto comp = required(*data, "comp_segments", path + "/data");
        if (!comp || comp.value()->kind != JsonValue::Kind::Array)
            return fail<TakeLane>(PersistenceErrorCode::MissingField,
                                  path + "/data/comp_segments");
        if (comp.value()->array.size() >
            limits.max_take_comp_segments - std::min(counts.take_comp_segments,
                                                     limits.max_take_comp_segments))
            return fail<TakeLane>(PersistenceErrorCode::LimitExceeded,
                                  path + "/data/comp_segments", comp.value()->begin,
                                  counts.take_comp_segments + comp.value()->array.size(),
                                  limits.max_take_comp_segments);
        counts.take_comp_segments += comp.value()->array.size();
        decoded_comp.reserve(comp.value()->array.size());
        for (std::size_t index = 0; index < comp.value()->array.size(); ++index) {
            const auto& encoded = comp.value()->array[index];
            const auto item_path = path + "/data/comp_segments/" + std::to_string(index);
            auto count = required(encoded, "sample_count", item_path);
            auto rate = required(encoded, "sample_rate", item_path);
            auto start = required(encoded, "start", item_path);
            auto take_id = required(encoded, "take_id", item_path);
            if (!count || !rate || !start || !take_id)
                return fail<TakeLane>(PersistenceErrorCode::MissingField, item_path);
            auto decoded_count =
                parse_canonical_u64_string(*count.value(), item_path + "/sample_count");
            auto decoded_rate = decode_rate(*rate.value(), item_path + "/sample_rate");
            auto decoded_start =
                parse_canonical_i64_string(*start.value(), item_path + "/start");
            auto decoded_take_id =
                parse_canonical_u64_string(*take_id.value(), item_path + "/take_id");
            if (!decoded_count || !decoded_rate || !decoded_start || !decoded_take_id)
                return fail<TakeLane>(PersistenceErrorCode::InvalidNumber, item_path);
            decoded_comp.push_back(
                {.take_id = ItemId{decoded_take_id.value()},
                 .range = {timebase::SamplePosition{decoded_start.value()},
                           decoded_count.value(), decoded_rate.value()}});
        }
    }
    auto created = TakeLane::create(ItemId{decoded_id.value()}, std::move(name).value(),
                                    std::move(decoded_takes), std::move(decoded_comp));
    if (!created)
        return model_fail<TakeLane>(created.error(), std::move(path));
    return runtime::Result<TakeLane, PersistenceError>(runtime::Ok(std::move(created).value()));
}

runtime::Result<Track, PersistenceError>
decode_track(const std::shared_ptr<const ParsedJson>& document, const JsonValue& value,
             const SchemaRegistry& registry, const DecodeLimits& limits, DecodeCounts& counts,
             std::string path) {
    if (++counts.tracks > limits.max_tracks)
        return fail<Track>(PersistenceErrorCode::LimitExceeded, path, value.begin, counts.tracks,
                           limits.max_tracks);
    auto envelope = data_for_versions(value, detail::track_schema_policy.type_name,
                                      detail::track_schema_policy.oldest_readable_version,
                                      detail::track_schema_policy.current_version, path);
    if (!envelope)
        return fail<Track>(envelope.error().code, envelope.error().path,
                           envelope.error().byte_offset);
    const auto& data = *envelope.value().data;
    auto id = required(data, "id", path + "/data");
    auto name = string_field(data, "name", path + "/data");
    auto clips = required(data, "clips", path + "/data");
    const auto* active_take_lane = data.find("active_take_lane_id");
    const auto* freeze = data.find("freeze");
    const auto* devices = data.find("device_chain");
    const auto* automation = data.find("automation_lanes");
    const auto* take_lanes = data.find("take_lanes");
    const auto* record = data.find("record_armed");
    const auto requires_devices =
        detail::track_schema_policy.requires_device_chain(envelope.value().version);
    const auto requires_automation =
        detail::track_schema_policy.requires_automation(envelope.value().version);
    const auto requires_takes =
        detail::track_schema_policy.requires_takes(envelope.value().version);
    const auto requires_active_take_lane =
        detail::track_schema_policy.requires_active_take_lane(envelope.value().version);
    const auto supports_freeze =
        detail::track_schema_policy.supports_freeze(envelope.value().version);
    if (!id || !name || !clips || clips.value()->kind != JsonValue::Kind::Array ||
        (!requires_devices && devices) ||
        (requires_devices && (!devices || devices->kind != JsonValue::Kind::Array)) ||
        (!requires_automation && automation) ||
        (requires_automation && (!automation || automation->kind != JsonValue::Kind::Array)) ||
        (!requires_takes && (take_lanes || record)) ||
        (requires_takes && (!take_lanes || take_lanes->kind != JsonValue::Kind::Array || !record ||
                            record->kind != JsonValue::Kind::Boolean)) ||
        (!requires_active_take_lane && active_take_lane) ||
        (requires_active_take_lane &&
         (!active_take_lane || active_take_lane->kind != JsonValue::Kind::String)) ||
        (!supports_freeze && freeze) || (freeze && freeze->kind != JsonValue::Kind::Object))
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
            return runtime::Err(decoded.error());
        decoded_clips.push_back(std::move(decoded).value());
    }
    std::vector<DevicePlacement> decoded_devices;
    decoded_devices.reserve(devices ? devices->array.size() : 0);
    for (std::size_t index = 0; devices && index < devices->array.size(); ++index) {
        const auto item_path = path + "/data/device_chain/" + std::to_string(index);
        if (++counts.device_placements > limits.max_device_placements)
            return fail<Track>(PersistenceErrorCode::LimitExceeded, item_path,
                               devices->array[index].begin, counts.device_placements,
                               limits.max_device_placements);
        auto device_data =
            data_for(devices->array[index], "pulp.timeline.device_placement", item_path);
        if (!device_data)
            return fail<Track>(device_data.error().code, device_data.error().path,
                               device_data.error().byte_offset);
        auto device_id = required(*device_data.value(), "id", item_path + "/data");
        if (!device_id)
            return fail<Track>(device_id.error().code, device_id.error().path,
                               device_id.error().byte_offset);
        auto decoded_device_id =
            parse_canonical_u64_string(*device_id.value(), item_path + "/data/id");
        if (!decoded_device_id)
            return fail<Track>(decoded_device_id.error().code, decoded_device_id.error().path,
                               decoded_device_id.error().byte_offset);
        decoded_devices.push_back(DevicePlacement{{decoded_device_id.value()}});
    }
    std::vector<AutomationLane> decoded_automation;
    if (automation) {
        auto decoded = detail::decode_automation_lanes(*automation, limits, counts.automation_lanes,
                                                       counts.automation_points,
                                                       path + "/data/automation_lanes");
        if (!decoded)
            return runtime::Err(decoded.error());
        decoded_automation = std::move(decoded).value();
    }
    std::vector<TakeLane> decoded_take_lanes;
    if (take_lanes) {
        decoded_take_lanes.reserve(take_lanes->array.size());
        for (std::size_t index = 0; index < take_lanes->array.size(); ++index) {
            auto decoded = decode_take_lane(take_lanes->array[index], limits, counts,
                                            path + "/data/take_lanes/" + std::to_string(index));
            if (!decoded)
                return runtime::Err(decoded.error());
            decoded_take_lanes.push_back(std::move(decoded).value());
        }
    }
    const bool decoded_record_armed = record && record->boolean;
    ItemId decoded_active_take_lane;
    if (active_take_lane) {
        auto parsed =
            parse_canonical_u64_string(*active_take_lane, path + "/data/active_take_lane_id");
        if (!parsed)
            return fail<Track>(parsed.error().code, parsed.error().path,
                               parsed.error().byte_offset);
        decoded_active_take_lane = {parsed.value()};
    }
    auto decoded_freeze = detail::decode_track_freeze(freeze, path);
    if (!decoded_freeze)
        return runtime::Err(decoded_freeze.error());
    auto created = Track::create(TrackInput{.id = {decoded_id.value()},
                                            .name = std::move(name).value(),
                                            .clips = std::move(decoded_clips),
                                            .device_chain = std::move(decoded_devices),
                                            .automation_lanes = std::move(decoded_automation),
                                            .take_lanes = std::move(decoded_take_lanes),
                                            .record_armed = decoded_record_armed,
                                            .active_take_lane_id = decoded_active_take_lane,
                                            .freeze = std::move(decoded_freeze).value()});
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
            return runtime::Err(decoded.error());
        decoded_tracks.push_back(std::move(decoded).value());
    }
    auto created = Sequence::create({decoded_id.value()}, std::move(name).value(), decoded_musical,
                                    decoded_absolute, std::move(decoded_tracks));
    if (!created)
        return model_fail<Sequence>(created.error(), std::move(path));
    return runtime::Result<Sequence, PersistenceError>(runtime::Ok(std::move(created).value()));
}

runtime::Result<ItemId, PersistenceError>
decode_command_item_id(const JsonValue& data, std::string_view name, const std::string& path) {
    auto value = required(data, name, path);
    if (!value)
        return fail<ItemId>(value.error().code, value.error().path, value.error().byte_offset);
    auto decoded = parse_canonical_u64_string(*value.value(), path + "/" + std::string(name));
    if (!decoded)
        return fail<ItemId>(decoded.error().code, decoded.error().path,
                            decoded.error().byte_offset);
    return runtime::Ok(ItemId{decoded.value()});
}

runtime::Result<bool, PersistenceError>
decode_command_bool(const JsonValue& data, std::string_view name, const std::string& path) {
    auto value = required(data, name, path);
    if (!value)
        return fail<bool>(value.error().code, value.error().path, value.error().byte_offset);
    if (value.value()->kind != JsonValue::Kind::Boolean)
        return fail<bool>(PersistenceErrorCode::UnexpectedType, path + "/" + std::string(name),
                          value.value()->begin);
    return runtime::Ok(value.value()->boolean);
}

runtime::Result<ClipPlaybackProperties, PersistenceError>
decode_command_playback_properties(const JsonValue& value, std::string path) {
    auto gain = required(value, "gain_linear_bits", path);
    auto fade_in = required(value, "fade_in_duration", path);
    auto fade_out = required(value, "fade_out_duration", path);
    if (!gain || !fade_in || !fade_out)
        return fail<ClipPlaybackProperties>(PersistenceErrorCode::MissingField, std::move(path));
    auto decoded_gain = parse_canonical_u64_string(*gain.value(), path + "/gain_linear_bits");
    auto decoded_fade_in = parse_canonical_u64_string(*fade_in.value(), path + "/fade_in_duration");
    auto decoded_fade_out =
        parse_canonical_u64_string(*fade_out.value(), path + "/fade_out_duration");
    if (!decoded_gain || !decoded_fade_in || !decoded_fade_out ||
        decoded_gain.value() > std::numeric_limits<std::uint32_t>::max())
        return fail<ClipPlaybackProperties>(PersistenceErrorCode::InvalidNumber, std::move(path));
    return runtime::Ok(ClipPlaybackProperties{
        std::bit_cast<float>(static_cast<std::uint32_t>(decoded_gain.value())),
        decoded_fade_in.value(), decoded_fade_out.value()});
}

runtime::Result<ClipTimeRange, PersistenceError> decode_command_clip_range(const JsonValue& value,
                                                                           std::string path) {
    auto kind = string_field(value, "kind", path);
    if (!kind)
        return fail<ClipTimeRange>(kind.error().code, kind.error().path, kind.error().byte_offset);
    if (kind.value() == "musical") {
        auto start = required(value, "start_ticks", path);
        auto duration = required(value, "duration_ticks", path);
        if (!start || !duration)
            return fail<ClipTimeRange>(PersistenceErrorCode::MissingField, std::move(path));
        auto decoded_start = parse_canonical_i64_string(*start.value(), path + "/start_ticks");
        auto decoded_duration =
            parse_canonical_i64_string(*duration.value(), path + "/duration_ticks");
        if (!decoded_start || !decoded_duration)
            return fail<ClipTimeRange>(PersistenceErrorCode::InvalidNumber, std::move(path));
        return runtime::Ok(
            ClipTimeRange(MusicalTimeRange{{decoded_start.value()}, {decoded_duration.value()}}));
    }
    if (kind.value() == "absolute") {
        auto start = required(value, "start_sample", path);
        auto count = required(value, "sample_count", path);
        auto rate = required(value, "sample_rate", path);
        if (!start || !count || !rate)
            return fail<ClipTimeRange>(PersistenceErrorCode::MissingField, std::move(path));
        auto decoded_start = parse_canonical_i64_string(*start.value(), path + "/start_sample");
        auto decoded_count = parse_canonical_u64_string(*count.value(), path + "/sample_count");
        auto decoded_rate = decode_rate(*rate.value(), path + "/sample_rate");
        if (!decoded_start || !decoded_count || !decoded_rate)
            return fail<ClipTimeRange>(PersistenceErrorCode::InvalidNumber, std::move(path));
        return runtime::Ok(ClipTimeRange(AbsoluteTimeRange{
            {decoded_start.value()}, decoded_count.value(), decoded_rate.value()}));
    }
    return fail<ClipTimeRange>(PersistenceErrorCode::InvalidSchema, path + "/kind");
}

runtime::Result<std::vector<TakeCompSegment>, PersistenceError>
decode_command_take_comp(const JsonValue& value, const DecodeLimits& limits,
                         std::size_t& total_count, std::string path) {
    if (value.kind != JsonValue::Kind::Array)
        return fail<std::vector<TakeCompSegment>>(PersistenceErrorCode::UnexpectedType,
                                                  std::move(path), value.begin);
    if (value.array.size() >
        limits.max_take_comp_segments - std::min(total_count, limits.max_take_comp_segments))
        return fail<std::vector<TakeCompSegment>>(
            PersistenceErrorCode::LimitExceeded, std::move(path), value.begin,
            total_count + value.array.size(), limits.max_take_comp_segments);
    total_count += value.array.size();
    std::vector<TakeCompSegment> result;
    result.reserve(value.array.size());
    for (std::size_t index = 0; index < value.array.size(); ++index) {
        const auto item_path = path + "/" + std::to_string(index);
        auto count = required(value.array[index], "sample_count", item_path);
        auto rate = required(value.array[index], "sample_rate", item_path);
        auto start = required(value.array[index], "start", item_path);
        auto take_id = required(value.array[index], "take_id", item_path);
        if (!count || !rate || !start || !take_id)
            return fail<std::vector<TakeCompSegment>>(PersistenceErrorCode::MissingField,
                                                      item_path);
        auto decoded_count =
            parse_canonical_u64_string(*count.value(), item_path + "/sample_count");
        auto decoded_rate = decode_rate(*rate.value(), item_path + "/sample_rate");
        auto decoded_start = parse_canonical_i64_string(*start.value(), item_path + "/start");
        auto decoded_take_id = parse_canonical_u64_string(*take_id.value(), item_path + "/take_id");
        if (!decoded_count || !decoded_rate || !decoded_start || !decoded_take_id)
            return fail<std::vector<TakeCompSegment>>(PersistenceErrorCode::InvalidNumber,
                                                      item_path);
        result.push_back({ItemId{decoded_take_id.value()},
                          {{decoded_start.value()}, decoded_count.value(), decoded_rate.value()}});
    }
    return runtime::Ok(std::move(result));
}

runtime::Result<Command, PersistenceError>
decode_command(const std::shared_ptr<const ParsedJson>& document, const JsonValue& value,
               const SchemaRegistry& registry, const DecodeLimits& limits, DecodeCounts& counts,
               std::string path) {
    auto type = string_field(value, "type_name", path);
    auto version = required(value, "version", path);
    if (!type || !version)
        return fail<Command>(PersistenceErrorCode::MissingField, std::move(path));
    auto decoded_version = parse_u32_number(*version.value(), path + "/version");
    if (!decoded_version)
        return fail<Command>(decoded_version.error().code, decoded_version.error().path,
                             decoded_version.error().byte_offset);
    const auto* schema = registry.find(SchemaDomain::Command, type.value());
    if (!schema)
        return fail<Command>(PersistenceErrorCode::UnsupportedStructuralType, std::move(path),
                             value.begin);
    if (decoded_version.value() != schema->current_version)
        return fail<Command>(PersistenceErrorCode::UnsupportedSchemaVersion, std::move(path),
                             value.begin);
    auto data = required(value, "data", path);
    if (!data || data.value()->kind != JsonValue::Kind::Object)
        return fail<Command>(PersistenceErrorCode::UnexpectedType, path + "/data");
    const auto& command = *data.value();
    const auto data_path = path + "/data";
    auto ids = [&]() -> runtime::Result<std::array<ItemId, 3>, PersistenceError> {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto track = decode_command_item_id(command, "track_id", data_path);
        auto clip = decode_command_item_id(command, "clip_id", data_path);
        if (!sequence || !track || !clip)
            return fail<std::array<ItemId, 3>>(PersistenceErrorCode::MissingField, data_path);
        return runtime::Ok(std::array{sequence.value(), track.value(), clip.value()});
    };

    if (type.value() == "pulp.timeline.command.insert_clip") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto track = decode_command_item_id(command, "track_id", data_path);
        auto clip = required(command, "clip", data_path);
        if (!sequence || !track || !clip)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        auto decoded =
            decode_clip(document, *clip.value(), registry, limits, counts, data_path + "/clip");
        if (!decoded)
            return runtime::Err(decoded.error());
        return runtime::Ok(
            Command(InsertClip{sequence.value(), track.value(), std::move(decoded).value()}));
    }
    if (type.value() == "pulp.timeline.command.remove_clip") {
        auto decoded = ids();
        if (!decoded)
            return runtime::Err(decoded.error());
        return runtime::Ok(
            Command(RemoveClip{decoded.value()[0], decoded.value()[1], decoded.value()[2]}));
    }
    if (type.value() == "pulp.timeline.command.insert_automation_lane") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto track = decode_command_item_id(command, "track_id", data_path);
        auto lane = required(command, "lane", data_path);
        if (!sequence || !track || !lane)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        JsonValue lanes;
        lanes.kind = JsonValue::Kind::Array;
        lanes.array.push_back(*lane.value());
        auto decoded = detail::decode_automation_lanes(
            lanes, limits, counts.automation_lanes, counts.automation_points, data_path + "/lane");
        if (!decoded || decoded.value().size() != 1)
            return fail<Command>(decoded ? PersistenceErrorCode::InvalidSchema
                                         : decoded.error().code,
                                 data_path + "/lane");
        return runtime::Ok(Command(
            InsertAutomationLane{sequence.value(), track.value(), std::move(decoded).value()[0]}));
    }
    if (type.value() == "pulp.timeline.command.remove_automation_lane") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto track = decode_command_item_id(command, "track_id", data_path);
        auto lane = decode_command_item_id(command, "lane_id", data_path);
        if (!sequence || !track || !lane)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        return runtime::Ok(
            Command(RemoveAutomationLane{sequence.value(), track.value(), lane.value()}));
    }
    if (type.value() == "pulp.timeline.command.move_clip") {
        auto decoded = ids();
        auto expected = required(command, "expected_range", data_path);
        auto replacement = required(command, "replacement_range", data_path);
        if (!decoded || !expected || !replacement)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        auto decoded_expected =
            decode_command_clip_range(*expected.value(), data_path + "/expected_range");
        auto decoded_replacement =
            decode_command_clip_range(*replacement.value(), data_path + "/replacement_range");
        if (!decoded_expected || !decoded_replacement)
            return fail<Command>(PersistenceErrorCode::InvalidSchema, data_path);
        return runtime::Ok(Command(MoveClip{decoded.value()[0], decoded.value()[1],
                                            decoded.value()[2], std::move(decoded_expected).value(),
                                            std::move(decoded_replacement).value()}));
    }
    if (type.value() == "pulp.timeline.command.set_note_velocity") {
        auto decoded = ids();
        auto note = decode_command_item_id(command, "note_id", data_path);
        auto expected = required(command, "expected_velocity", data_path);
        auto replacement = required(command, "replacement_velocity", data_path);
        if (!decoded || !note || !expected || !replacement)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        auto decoded_expected =
            parse_u32_number(*expected.value(), data_path + "/expected_velocity");
        auto decoded_replacement =
            parse_u32_number(*replacement.value(), data_path + "/replacement_velocity");
        if (!decoded_expected || !decoded_replacement ||
            decoded_expected.value() > std::numeric_limits<std::uint16_t>::max() ||
            decoded_replacement.value() > std::numeric_limits<std::uint16_t>::max())
            return fail<Command>(PersistenceErrorCode::InvalidNumber, data_path);
        return runtime::Ok(Command(
            SetNoteVelocity{decoded.value()[0], decoded.value()[1], decoded.value()[2],
                            note.value(), static_cast<std::uint16_t>(decoded_expected.value()),
                            static_cast<std::uint16_t>(decoded_replacement.value())}));
    }
    if (type.value() == "pulp.timeline.command.set_clip_playback_properties") {
        auto decoded = ids();
        auto expected = required(command, "expected", data_path);
        auto replacement = required(command, "replacement", data_path);
        if (!decoded || !expected || !replacement)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        auto decoded_expected =
            decode_command_playback_properties(*expected.value(), data_path + "/expected");
        auto decoded_replacement =
            decode_command_playback_properties(*replacement.value(), data_path + "/replacement");
        if (!decoded_expected || !decoded_replacement)
            return fail<Command>(PersistenceErrorCode::InvalidSchema, data_path);
        return runtime::Ok(Command(
            SetClipPlaybackProperties{decoded.value()[0], decoded.value()[1], decoded.value()[2],
                                      decoded_expected.value(), decoded_replacement.value()}));
    }
    if (type.value() == "pulp.timeline.command.set_tempo_map" ||
        type.value() == "pulp.timeline.command.set_meter_map") {
        auto expected = required(command, "expected", data_path);
        auto replacement = required(command, "replacement", data_path);
        if (!expected || !replacement)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        if (type.value() == "pulp.timeline.command.set_tempo_map") {
            auto decoded_expected = decode_tempo_map(*expected.value(), data_path + "/expected");
            auto decoded_replacement =
                decode_tempo_map(*replacement.value(), data_path + "/replacement");
            if (!decoded_expected || !decoded_replacement)
                return fail<Command>(PersistenceErrorCode::InvalidSchema, data_path);
            return runtime::Ok(Command(SetTempoMap{std::move(decoded_expected).value(),
                                                   std::move(decoded_replacement).value()}));
        }
        auto decoded_expected = decode_meter_map(*expected.value(), data_path + "/expected");
        auto decoded_replacement =
            decode_meter_map(*replacement.value(), data_path + "/replacement");
        if (!decoded_expected || !decoded_replacement)
            return fail<Command>(PersistenceErrorCode::InvalidSchema, data_path);
        return runtime::Ok(Command(SetMeterMap{std::move(decoded_expected).value(),
                                               std::move(decoded_replacement).value()}));
    }
    if (type.value() == "pulp.timeline.command.create_asset") {
        auto asset = required(command, "asset", data_path);
        if (!asset)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        auto decoded = decode_asset(*asset.value(), data_path + "/asset", counts, limits);
        if (!decoded)
            return runtime::Err(decoded.error());
        return runtime::Ok(Command(CreateAsset{std::move(decoded).value()}));
    }
    if (type.value() == "pulp.timeline.command.remove_asset") {
        auto asset = decode_command_item_id(command, "asset_id", data_path);
        if (!asset)
            return runtime::Err(asset.error());
        return runtime::Ok(Command(RemoveAsset{asset.value()}));
    }
    if (type.value() == "pulp.timeline.command.insert_take_lane") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto track = decode_command_item_id(command, "track_id", data_path);
        auto lane = required(command, "lane", data_path);
        if (!sequence || !track || !lane)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        auto decoded = decode_take_lane(*lane.value(), limits, counts, data_path + "/lane");
        if (!decoded)
            return runtime::Err(decoded.error());
        return runtime::Ok(
            Command(InsertTakeLane{sequence.value(), track.value(), std::move(decoded).value()}));
    }
    if (type.value() == "pulp.timeline.command.remove_take_lane") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto track = decode_command_item_id(command, "track_id", data_path);
        auto lane = decode_command_item_id(command, "lane_id", data_path);
        if (!sequence || !track || !lane)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        return runtime::Ok(Command(RemoveTakeLane{sequence.value(), track.value(), lane.value()}));
    }
    if (type.value() == "pulp.timeline.command.insert_take") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto track = decode_command_item_id(command, "track_id", data_path);
        auto lane = decode_command_item_id(command, "lane_id", data_path);
        auto take = required(command, "take", data_path);
        if (!sequence || !track || !lane || !take)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        auto decoded = decode_take(*take.value(), limits, counts, data_path + "/take");
        if (!decoded)
            return runtime::Err(decoded.error());
        return runtime::Ok(Command(
            InsertTake{sequence.value(), track.value(), lane.value(), std::move(decoded).value()}));
    }
    if (type.value() == "pulp.timeline.command.remove_take") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto track = decode_command_item_id(command, "track_id", data_path);
        auto lane = decode_command_item_id(command, "lane_id", data_path);
        auto take = decode_command_item_id(command, "take_id", data_path);
        if (!sequence || !track || !lane || !take)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        return runtime::Ok(
            Command(RemoveTake{sequence.value(), track.value(), lane.value(), take.value()}));
    }
    if (type.value() == "pulp.timeline.command.set_record_arm") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto track = decode_command_item_id(command, "track_id", data_path);
        auto expected = decode_command_bool(command, "expected", data_path);
        auto replacement = decode_command_bool(command, "replacement", data_path);
        if (!sequence || !track || !expected || !replacement)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        return runtime::Ok(Command(
            SetRecordArm{sequence.value(), track.value(), expected.value(), replacement.value()}));
    }
    if (type.value() == "pulp.timeline.command.set_active_take_lane") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto track = decode_command_item_id(command, "track_id", data_path);
        auto expected = decode_command_item_id(command, "expected_lane_id", data_path);
        auto replacement = decode_command_item_id(command, "replacement_lane_id", data_path);
        if (!sequence || !track || !expected || !replacement)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        return runtime::Ok(Command(SetActiveTakeLane{sequence.value(), track.value(),
                                                     expected.value(), replacement.value()}));
    }
    if (type.value() == "pulp.timeline.command.set_take_comp") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto track = decode_command_item_id(command, "track_id", data_path);
        auto lane = decode_command_item_id(command, "lane_id", data_path);
        auto expected = required(command, "expected", data_path);
        auto replacement = required(command, "replacement", data_path);
        if (!sequence || !track || !lane || !expected || !replacement)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        auto decoded_expected = decode_command_take_comp(
            *expected.value(), limits, counts.take_comp_segments, data_path + "/expected");
        auto decoded_replacement = decode_command_take_comp(
            *replacement.value(), limits, counts.take_comp_segments, data_path + "/replacement");
        if (!decoded_expected || !decoded_replacement)
            return fail<Command>(PersistenceErrorCode::InvalidSchema, data_path);
        return runtime::Ok(Command(SetTakeComp{sequence.value(), track.value(), lane.value(),
                                               std::move(decoded_expected).value(),
                                               std::move(decoded_replacement).value()}));
    }
    if (type.value() == "pulp.timeline.command.set_track_freeze") {
        auto sequence = decode_command_item_id(command, "sequence_id", data_path);
        auto track = decode_command_item_id(command, "track_id", data_path);
        if (!sequence || !track)
            return fail<Command>(PersistenceErrorCode::MissingField, data_path);
        auto expected =
            detail::decode_track_freeze(command.find("expected"), data_path + "/expected");
        auto replacement =
            detail::decode_track_freeze(command.find("replacement"), data_path + "/replacement");
        if (!expected || !replacement)
            return fail<Command>(PersistenceErrorCode::InvalidSchema, data_path);
        return runtime::Ok(
            Command(SetTrackFreeze{sequence.value(), track.value(), std::move(expected).value(),
                                   std::move(replacement).value()}));
    }
    return fail<Command>(PersistenceErrorCode::UnsupportedStructuralType, std::move(path),
                         value.begin);
}

} // namespace

runtime::Result<Project, PersistenceError> deserialize_project(std::string_view json,
                                                               const SchemaRegistry& registry,
                                                               const DecodeLimits& limits) {
    if (const auto invalid = detail::validate_structural_registry(registry))
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
            return runtime::Err(decoded.error());
        decoded_assets.push_back(std::move(decoded).value());
    }
    std::vector<Sequence> decoded_sequences;
    for (std::size_t index = 0; index < sequences.value()->array.size(); ++index) {
        auto decoded = decode_sequence(parsed.value(), sequences.value()->array[index], registry,
                                       limits, counts, "/data/sequences/" + std::to_string(index));
        if (!decoded)
            return runtime::Err(decoded.error());
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
            const auto* parent_value = value.find("parent_id");
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
            ItemId parent_id;
            if (parent_value) {
                auto parent = parse_canonical_u64_string(*parent_value, path + "/parent_id");
                if (!parent)
                    return fail<Project>(PersistenceErrorCode::InvalidNumber, path + "/parent_id");
                parent_id = {parent.value()};
            } else {
                parent_id =
                    immediate_parent_id(kind.value(), {decoded_id.value()}, {sequence_id.value()},
                                        {track_id.value()}, {clip_id.value()});
            }
            decoded_identities.push_back({{item.value()},
                                          {kind.value(),
                                           parent_id,
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

runtime::Result<std::vector<Command>, PersistenceError>
deserialize_commands(std::string_view json, const SchemaRegistry& registry,
                     const DecodeLimits& limits) {
    auto parsed = parse_json(json, limits);
    if (!parsed)
        return fail<std::vector<Command>>(parsed.error().code, parsed.error().path,
                                          parsed.error().byte_offset, parsed.error().actual,
                                          parsed.error().limit);
    if (parsed.value()->root().kind != JsonValue::Kind::Array)
        return fail<std::vector<Command>>(PersistenceErrorCode::UnexpectedType, "/",
                                          parsed.value()->root().begin);
    if (parsed.value()->root().array.empty())
        return fail<std::vector<Command>>(PersistenceErrorCode::InvalidSchema, "/",
                                          parsed.value()->root().begin);
    if (parsed.value()->root().array.size() > limits.max_array_elements)
        return fail<std::vector<Command>>(
            PersistenceErrorCode::LimitExceeded, "/", parsed.value()->root().begin,
            parsed.value()->root().array.size(), limits.max_array_elements);
    std::vector<Command> commands;
    commands.reserve(parsed.value()->root().array.size());
    DecodeCounts counts;
    for (std::size_t index = 0; index < parsed.value()->root().array.size(); ++index) {
        auto decoded = decode_command(parsed.value(), parsed.value()->root().array[index], registry,
                                      limits, counts, "/" + std::to_string(index));
        if (!decoded)
            return runtime::Err(decoded.error());
        commands.push_back(std::move(decoded).value());
    }
    return runtime::Ok(std::move(commands));
}

} // namespace pulp::timeline
