#include "media_reference_validation.hpp"

#include <algorithm>
#include <utility>

namespace pulp::timeline::detail {
namespace {

template <typename FindAsset>
std::optional<ModelError> validate_reference(const MediaRef& media, ItemId owner,
                                             FindAsset&& find_asset) noexcept {
    const auto* asset = find_asset(media.asset_id);
    if (!asset)
        return ModelError{ModelErrorCode::MissingAsset, owner, media.asset_id};
    if (media.source_start.value < 0)
        return ModelError{ModelErrorCode::InvalidMediaRange, owner, media.asset_id};
    const auto start = static_cast<std::uint64_t>(media.source_start.value);
    if (start > asset->frame_count || media.frame_count > asset->frame_count - start)
        return ModelError{ModelErrorCode::InvalidMediaRange, owner, media.asset_id};
    return std::nullopt;
}

template <typename FindAsset>
std::optional<ModelError> validate_clip(const Clip& clip, FindAsset&& find_asset) noexcept {
    return std::visit(
        ClipContentCases{
            [](const EmptyContent&) { return std::optional<ModelError>{}; },
            [&](const MediaRef& media) { return validate_reference(media, clip.id(), find_asset); },
            [](const NoteContent&) { return std::optional<ModelError>{}; },
            [](const RegisteredContent&) { return std::optional<ModelError>{}; },
            [](const OpaqueContent&) { return std::optional<ModelError>{}; },
            [](const SequenceRef&) { return std::optional<ModelError>{}; },
        },
        clip.content());
}

template <typename FindAsset>
std::optional<ModelError> validate_track(const Track& track, FindAsset&& find_asset) noexcept {
    for (const auto& clip : track.clips())
        if (const auto error = validate_clip(clip, find_asset))
            return error;
    for (const auto& lane : track.take_lanes())
        for (const auto& take : lane.takes())
            if (const auto error = validate_reference(take.media(), take.id(), find_asset))
                return error;
    if (track.freeze()) {
        if (const auto error = validate_reference(track.freeze()->media, track.id(), find_asset))
            return error;
        const auto* asset = find_asset(track.freeze()->media.asset_id);
        if (!asset || asset->sample_rate.normalized() != track.freeze()->sample_rate.normalized())
            return ModelError{ModelErrorCode::IncompatibleSampleRate, track.id(),
                              track.freeze()->media.asset_id};
    }
    return std::nullopt;
}

template <typename FindAsset>
std::optional<ModelError> validate_sequence(const Sequence& sequence,
                                            FindAsset&& find_asset) noexcept {
    for (const auto& track : sequence.tracks())
        if (const auto error = validate_track(track, find_asset))
            return error;
    return std::nullopt;
}

auto span_lookup(std::span<const MediaAsset> assets) {
    return [assets](ItemId id) -> const MediaAsset* {
        const auto found = std::lower_bound(
            assets.begin(), assets.end(), id,
            [](const MediaAsset& asset, ItemId wanted) { return asset.id < wanted; });
        return found != assets.end() && found->id == id ? &*found : nullptr;
    };
}

auto project_lookup(const Project& project) {
    return [&project](ItemId id) { return project.find_asset(id); };
}

} // namespace

std::optional<ModelError> validate_media_reference(std::span<const MediaAsset> assets,
                                                   const MediaRef& media, ItemId owner) noexcept {
    return validate_reference(media, owner, span_lookup(assets));
}

std::optional<ModelError> validate_media_reference(const Project& project, const MediaRef& media,
                                                   ItemId owner) noexcept {
    return validate_reference(media, owner, project_lookup(project));
}

std::optional<ModelError> validate_clip_media(std::span<const MediaAsset> assets,
                                              const Clip& clip) noexcept {
    return validate_clip(clip, span_lookup(assets));
}

std::optional<ModelError> validate_clip_media(const Project& project, const Clip& clip) noexcept {
    return validate_clip(clip, project_lookup(project));
}

std::optional<ModelError> validate_track_media(std::span<const MediaAsset> assets,
                                               const Track& track) noexcept {
    return validate_track(track, span_lookup(assets));
}

std::optional<ModelError> validate_track_media(const Project& project,
                                               const Track& track) noexcept {
    return validate_track(track, project_lookup(project));
}

std::optional<ModelError> validate_sequence_media(std::span<const MediaAsset> assets,
                                                  const Sequence& sequence) noexcept {
    return validate_sequence(sequence, span_lookup(assets));
}

std::optional<ModelError> validate_sequence_media(const Project& project,
                                                  const Sequence& sequence) noexcept {
    return validate_sequence(sequence, project_lookup(project));
}

} // namespace pulp::timeline::detail
