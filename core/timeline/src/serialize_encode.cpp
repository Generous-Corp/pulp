#include <pulp/timeline/serialize.hpp>

#include "project_state_access.hpp"
#include "schema_json_write_internal.hpp"
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
        return detail::append_quoted_json_string(
            value, [this](std::string_view text) { return append(text); });
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
    case ItemKind::DevicePlacement:
        return "device_placement";
    case ItemKind::AutomationLane:
        return "automation_lane";
    case ItemKind::AutomationPoint:
        return "automation_point";
    case ItemKind::TakeLane:
        return "take_lane";
    case ItemKind::Take:
        return "take";
    }
    return "project";
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
            !context.writer.append(",\"tick\":") || !context.writer.i64(point.tick.value, true) ||
            !context.writer.character('}'))
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
            !context.writer.append(",\"tick\":") || !context.writer.i64(point.tick.value, true) ||
            !context.writer.character('}'))
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

bool write_device_placement(EncodeContext& context, const DevicePlacement& placement) {
    return write_envelope(context, "pulp.timeline.device_placement", 1, [&] {
        return context.writer.append("{\"id\":") && context.writer.u64(placement.id.value, true) &&
               context.writer.character('}');
    });
}

bool write_automation_lane(EncodeContext& context, const AutomationLane& lane) {
    return write_envelope(context, "pulp.timeline.automation_lane", 1, [&] {
        if (!context.writer.append("{\"id\":") || !context.writer.u64(lane.id().value, true) ||
            !context.writer.append(",\"points\":["))
            return false;
        for (std::size_t index = 0; index < lane.curve().points().size(); ++index) {
            const auto& point = lane.curve().points()[index];
            if ((index != 0 && !context.writer.character(',')) ||
                !context.writer.append("{\"curvature_bits\":") ||
                !context.writer.u64(std::bit_cast<std::uint32_t>(point.curvature), true) ||
                !context.writer.append(",\"id\":") || !context.writer.u64(point.id.value, true) ||
                !context.writer.append(",\"interpolation\":") ||
                !context.writer.quoted(
                    point.interpolation == AutomationInterpolation::Hold ? "hold" : "continuous") ||
                !context.writer.append(",\"position_ticks\":") ||
                !context.writer.i64(point.position.value, true) ||
                !context.writer.append(",\"value_bits\":") ||
                !context.writer.u64(std::bit_cast<std::uint32_t>(point.value), true) ||
                !context.writer.character('}'))
                return false;
        }
        const auto& target = std::get<DeviceParameterTarget>(lane.target());
        return context.writer.append("],\"target\":") &&
               write_envelope(
                   context, "pulp.timeline.automation_target.device_parameter", 1,
                   [&] {
                       return context.writer.append("{\"device_placement_id\":") &&
                              context.writer.u64(target.device_placement_id.value, true) &&
                              context.writer.append(",\"parameter_id\":") &&
                              context.writer.u64(target.param_id) && context.writer.character('}');
                   }) &&
               context.writer.character('}');
    });
}

bool write_take(EncodeContext& context, const Take& take) {
    return write_envelope(context, "pulp.timeline.take", 1, [&] {
        return context.writer.append("{\"asset_id\":") &&
               context.writer.u64(take.media().asset_id.value, true) &&
               context.writer.append(",\"frame_count\":") &&
               context.writer.u64(take.media().frame_count, true) &&
               context.writer.append(",\"id\":") && context.writer.u64(take.id().value, true) &&
               context.writer.append(",\"placement_start\":") &&
               context.writer.i64(take.placement_start().value, true) &&
               context.writer.append(",\"sample_rate\":") &&
               write_rate(context, take.sample_rate()) &&
               context.writer.append(",\"source_start\":") &&
               context.writer.i64(take.media().source_start.value, true) &&
               context.writer.character('}');
    });
}

bool write_take_lane(EncodeContext& context, const TakeLane& lane) {
    return write_envelope(context, "pulp.timeline.take_lane", 2, [&] {
        if (!context.writer.append("{\"comp_segments\":["))
            return false;
        for (std::size_t index = 0; index < lane.comp_segments().size(); ++index) {
            const auto& segment = lane.comp_segments()[index];
            if ((index != 0 && !context.writer.character(',')) ||
                !context.writer.append("{\"sample_count\":") ||
                !context.writer.u64(segment.range.sample_count, true) ||
                !context.writer.append(",\"sample_rate\":") ||
                !write_rate(context, segment.range.sample_rate) ||
                !context.writer.append(",\"start\":") ||
                !context.writer.i64(segment.range.start.value, true) ||
                !context.writer.append(",\"take_id\":") ||
                !context.writer.u64(segment.take_id.value, true) ||
                !context.writer.character('}'))
                return false;
        }
        if (!context.writer.append("],\"id\":") || !context.writer.u64(lane.id().value, true) ||
            !context.writer.append(",\"name\":") || !context.writer.quoted(lane.name()) ||
            !context.writer.append(",\"takes\":["))
            return false;
        for (std::size_t index = 0; index < lane.takes().size(); ++index)
            if ((index != 0 && !context.writer.character(',')) ||
                !write_take(context, lane.takes()[index]))
                return false;
        return context.writer.append("]}");
    });
}

bool write_track(EncodeContext& context, const Track& track) {
    return write_envelope(
        context, detail::track_schema_policy.type_name, detail::track_schema_policy.current_version,
        [&] {
            if (!context.writer.append("{\"active_take_lane_id\":") ||
                !context.writer.u64(track.active_take_lane_id().value, true) ||
                !context.writer.append(",\"automation_lanes\":["))
                return false;
            for (std::size_t index = 0; index < track.automation_lanes().size(); ++index)
                if ((index != 0 && !context.writer.character(',')) ||
                    !write_automation_lane(context, track.automation_lanes()[index]))
                    return false;
            if (!context.writer.append("],\"clips\":["))
                return false;
            for (std::size_t index = 0; index < track.clips().size(); ++index)
                if ((index != 0 && !context.writer.character(',')) ||
                    !write_clip(context, track.clips()[index]))
                    return false;
            if (!context.writer.append("],\"device_chain\":["))
                return false;
            for (std::size_t index = 0; index < track.device_chain().size(); ++index)
                if ((index != 0 && !context.writer.character(',')) ||
                    !write_device_placement(context, track.device_chain()[index]))
                    return false;
            if (!context.writer.append("],\"id\":") ||
                !context.writer.u64(track.id().value, true) ||
                !context.writer.append(",\"name\":") || !context.writer.quoted(track.name()) ||
                !context.writer.append(",\"record_armed\":") ||
                !context.writer.append(track.record_armed() ? "true" : "false") ||
                !context.writer.append(",\"take_lanes\":["))
                return false;
            for (std::size_t index = 0; index < track.take_lanes().size(); ++index)
                if ((index != 0 && !context.writer.character(',')) ||
                    !write_take_lane(context, track.take_lanes()[index]))
                    return false;
            return context.writer.append("]}");
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

} // namespace

runtime::Result<SerializedSnapshot, PersistenceError>
serialize_project(const Project& project, const SchemaRegistry& registry,
                  const SerializeOptions& options) {
    if (const auto invalid = detail::validate_structural_registry(registry))
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
        for (std::size_t track_index = 0; track_index < sequence.tracks().size(); ++track_index) {
            const auto& track = sequence.tracks()[track_index];
            const auto track_path =
                sequence_path + "/tracks/" + std::to_string(track_index) + "/data";
            if (!is_valid_utf8(track.name()))
                return fail<SerializedSnapshot>(PersistenceErrorCode::InvalidUtf8,
                                                track_path + "/name");
            for (std::size_t lane_index = 0; lane_index < track.take_lanes().size(); ++lane_index)
                if (!is_valid_utf8(track.take_lanes()[lane_index].name()))
                    return fail<SerializedSnapshot>(PersistenceErrorCode::InvalidUtf8,
                                                    track_path + "/take_lanes/" +
                                                        std::to_string(lane_index) + "/data/name");
        }
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
                !context.writer.append(",\"parent_id\":") ||
                !context.writer.u64(location.parent_id.value, true) ||
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

} // namespace pulp::timeline
