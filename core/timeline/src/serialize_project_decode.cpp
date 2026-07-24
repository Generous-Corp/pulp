#include <pulp/timeline/serialize.hpp>

#include "asset_schema_policy.hpp"
#include "project_state_access.hpp"
#include "serialize_asset_loop_decode.hpp"
#include "serialize_automation_decode.hpp"
#include "serialize_decode_context.hpp"
#include "track_schema_policy.hpp"

#include <algorithm>
#include <bit>
#include <limits>

namespace pulp::timeline::detail {

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
                                                           DecodeContext& context) {
    const auto& limits = context.limits;
    auto& counts = context.counts;
    const auto asset_increment = bounded_increment(counts.assets, limits.max_assets);
    if (!asset_increment)
        return fail<MediaAsset>(PersistenceErrorCode::LimitExceeded, path, value.begin,
                                asset_increment.actual, limits.max_assets);
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
        auto decoded =
            detail::decode_audio_loop_info(*loop_info, limits, context.audio_loop_points,
                                           context.audio_loop_tags, path + "/data/loop_info");
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
               const SchemaRegistry& registry, DecodeContext& context, std::string path) {
    const auto& limits = context.limits;
    auto& counts = context.counts;
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
        return decode_content(parsed.value(), parsed.value()->root(), registry, context,
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
            const SchemaRegistry& registry, DecodeContext& context, std::string path) {
    const auto& limits = context.limits;
    auto& counts = context.counts;
    const auto clip_increment = bounded_increment(counts.clips, limits.max_clips);
    if (!clip_increment)
        return fail<Clip>(PersistenceErrorCode::LimitExceeded, path, value.begin,
                          clip_increment.actual, limits.max_clips);
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
    auto content =
        decode_content(document, *content_value.value(), registry, context, path + "/data/content");
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

runtime::Result<Take, PersistenceError> decode_take(const JsonValue& value, DecodeContext& context,
                                                    std::string path) {
    const auto& limits = context.limits;
    auto& counts = context.counts;
    const auto take_increment = bounded_increment(counts.takes, limits.max_takes);
    if (!take_increment)
        return fail<Take>(PersistenceErrorCode::LimitExceeded, path, value.begin,
                          take_increment.actual, limits.max_takes);
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

runtime::Result<TakeLane, PersistenceError>
decode_take_lane(const JsonValue& value, DecodeContext& context, std::string path) {
    const auto& limits = context.limits;
    auto& counts = context.counts;
    const auto take_lane_increment = bounded_increment(counts.take_lanes, limits.max_take_lanes);
    if (!take_lane_increment)
        return fail<TakeLane>(PersistenceErrorCode::LimitExceeded, path, value.begin,
                              take_lane_increment.actual, limits.max_take_lanes);
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
        auto decoded = decode_take(takes.value()->array[index], context,
                                   path + "/data/takes/" + std::to_string(index));
        if (!decoded)
            return runtime::Err(decoded.error());
        decoded_takes.push_back(std::move(decoded).value());
    }
    std::vector<TakeCompSegment> decoded_comp;
    if (structural.value().version == 2) {
        auto comp = required(*data, "comp_segments", path + "/data");
        if (!comp || comp.value()->kind != JsonValue::Kind::Array)
            return fail<TakeLane>(PersistenceErrorCode::MissingField, path + "/data/comp_segments");
        if (comp.value()->array.size() >
            limits.max_take_comp_segments -
                std::min(counts.take_comp_segments, limits.max_take_comp_segments))
            return fail<TakeLane>(PersistenceErrorCode::LimitExceeded, path + "/data/comp_segments",
                                  comp.value()->begin,
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
            auto decoded_start = parse_canonical_i64_string(*start.value(), item_path + "/start");
            auto decoded_take_id =
                parse_canonical_u64_string(*take_id.value(), item_path + "/take_id");
            if (!decoded_count || !decoded_rate || !decoded_start || !decoded_take_id)
                return fail<TakeLane>(PersistenceErrorCode::InvalidNumber, item_path);
            decoded_comp.push_back({.take_id = ItemId{decoded_take_id.value()},
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
             const SchemaRegistry& registry, DecodeContext& context, std::string path) {
    const auto& limits = context.limits;
    auto& counts = context.counts;
    const auto track_increment = bounded_increment(counts.tracks, limits.max_tracks);
    if (!track_increment)
        return fail<Track>(PersistenceErrorCode::LimitExceeded, path, value.begin,
                           track_increment.actual, limits.max_tracks);
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
        auto decoded = decode_clip(document, clips.value()->array[index], registry, context,
                                   path + "/data/clips/" + std::to_string(index));
        if (!decoded)
            return runtime::Err(decoded.error());
        decoded_clips.push_back(std::move(decoded).value());
    }
    std::vector<DevicePlacement> decoded_devices;
    decoded_devices.reserve(devices ? devices->array.size() : 0);
    for (std::size_t index = 0; devices && index < devices->array.size(); ++index) {
        const auto item_path = path + "/data/device_chain/" + std::to_string(index);
        const auto device_increment =
            bounded_increment(counts.device_placements, limits.max_device_placements);
        if (!device_increment)
            return fail<Track>(PersistenceErrorCode::LimitExceeded, item_path,
                               devices->array[index].begin, device_increment.actual,
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
            auto decoded = decode_take_lane(take_lanes->array[index], context,
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
                const SchemaRegistry& registry, DecodeContext& context, std::string path) {
    const auto& limits = context.limits;
    auto& counts = context.counts;
    const auto sequence_increment = bounded_increment(counts.sequences, limits.max_sequences);
    if (!sequence_increment)
        return fail<Sequence>(PersistenceErrorCode::LimitExceeded, path, value.begin,
                              sequence_increment.actual, limits.max_sequences);
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
        auto decoded = decode_track(document, tracks.value()->array[index], registry, context,
                                    path + "/data/tracks/" + std::to_string(index));
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

} // namespace pulp::timeline::detail

namespace pulp::timeline {

using detail::data_for;
using detail::decode_asset;
using detail::decode_item_kind;
using detail::decode_meter_map;
using detail::decode_sequence;
using detail::decode_tempo_map;
using detail::DecodeContext;
using detail::fail;
using detail::model_fail;
using detail::required;
using detail::string_field;

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
    DecodeContext context(limits);
    std::vector<MediaAsset> decoded_assets;
    for (std::size_t index = 0; index < assets.value()->array.size(); ++index) {
        auto decoded = decode_asset(assets.value()->array[index],
                                    "/data/assets/" + std::to_string(index), context);
        if (!decoded)
            return runtime::Err(decoded.error());
        decoded_assets.push_back(std::move(decoded).value());
    }
    std::vector<Sequence> decoded_sequences;
    for (std::size_t index = 0; index < sequences.value()->array.size(); ++index) {
        auto decoded = decode_sequence(parsed.value(), sequences.value()->array[index], registry,
                                       context, "/data/sequences/" + std::to_string(index));
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

} // namespace pulp::timeline
