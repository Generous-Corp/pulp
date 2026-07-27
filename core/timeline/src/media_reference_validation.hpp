#pragma once

#include <pulp/timeline/model.hpp>

namespace pulp::timeline::detail {

std::optional<ModelError> validate_media_reference(std::span<const MediaAsset> assets,
                                                   const MediaRef& media, ItemId owner) noexcept;
std::optional<ModelError> validate_media_reference(const Project& project, const MediaRef& media,
                                                   ItemId owner) noexcept;
std::optional<ModelError> validate_clip_media(std::span<const MediaAsset> assets,
                                              const Clip& clip) noexcept;
std::optional<ModelError> validate_clip_media(const Project& project, const Clip& clip) noexcept;
std::optional<ModelError> validate_track_media(std::span<const MediaAsset> assets,
                                               const Track& track) noexcept;
std::optional<ModelError> validate_track_media(const Project& project, const Track& track) noexcept;
std::optional<ModelError> validate_sequence_media(std::span<const MediaAsset> assets,
                                                  const Sequence& sequence) noexcept;
std::optional<ModelError> validate_sequence_media(const Project& project,
                                                  const Sequence& sequence) noexcept;

} // namespace pulp::timeline::detail
