#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <pulp/runtime/result.hpp>
#include <pulp/timeline/item_id.hpp>

namespace pulp::timeline_editor {

/// Construction and query failures for track-header projection.
enum class TrackHeaderProjectionError : std::uint8_t {
    EmptyTrackOrder,
    InvalidTrackId,
    DuplicateTrackId,
    NonFiniteGeometry,
    NonPositiveRowHeight,
    NonFiniteCoordinate,
    MissingMovingTrack,
};

/// Maps authored track order to header rows and reorder insertion neighbors.
///
/// Row lookup is half-open. Drop lookup compares against row centers and omits
/// the moving track, producing the `before_track_id` value for `MoveTrack`.
class TrackHeaderProjection {
  public:
    /// Empty means append after every remaining track.
    using DropTarget = std::optional<timeline::ItemId>;

    /// Validates and captures the complete authored order and row geometry.
    static runtime::Result<TrackHeaderProjection, TrackHeaderProjectionError>
    create(std::span<const timeline::ItemId> track_order, float origin_y, float row_height) {
        if (!std::isfinite(origin_y) || !std::isfinite(row_height)) {
            return runtime::Err(TrackHeaderProjectionError::NonFiniteGeometry);
        }
        if (row_height <= 0.0f) {
            return runtime::Err(TrackHeaderProjectionError::NonPositiveRowHeight);
        }
        if (track_order.empty()) {
            return runtime::Err(TrackHeaderProjectionError::EmptyTrackOrder);
        }

        std::vector<timeline::ItemId> order(track_order.begin(), track_order.end());
        for (const auto id : order) {
            if (!id.valid()) {
                return runtime::Err(TrackHeaderProjectionError::InvalidTrackId);
            }
        }
        auto sorted = order;
        std::sort(sorted.begin(), sorted.end());
        if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
            return runtime::Err(TrackHeaderProjectionError::DuplicateTrackId);
        }

        const auto bottom = origin_y + row_height * static_cast<float>(order.size());
        if (!std::isfinite(bottom)) {
            return runtime::Err(TrackHeaderProjectionError::NonFiniteGeometry);
        }
        return runtime::Ok(TrackHeaderProjection(std::move(order), origin_y, row_height));
    }

    /// Returns the track owning y, or empty outside the half-open row range.
    [[nodiscard]] std::optional<timeline::ItemId> track_at(float y) const noexcept {
        if (!std::isfinite(y) || y < origin_y_ || y >= bottom_y()) {
            return std::nullopt;
        }
        const auto index =
            std::min(static_cast<std::size_t>((y - origin_y_) / row_height_), order_.size() - 1);
        return order_[index];
    }

    /// Resolves y to a reorder neighbor after removing moving_track logically.
    [[nodiscard]] runtime::Result<DropTarget, TrackHeaderProjectionError>
    before_track_for_drop(float y, timeline::ItemId moving_track) const {
        if (!std::isfinite(y)) {
            return runtime::Err(TrackHeaderProjectionError::NonFiniteCoordinate);
        }
        if (std::find(order_.begin(), order_.end(), moving_track) == order_.end()) {
            return runtime::Err(TrackHeaderProjectionError::MissingMovingTrack);
        }

        for (std::size_t index = 0; index < order_.size(); ++index) {
            const auto candidate = order_[index];
            if (candidate == moving_track) {
                continue;
            }
            const auto center = origin_y_ + (static_cast<float>(index) + 0.5f) * row_height_;
            if (y < center) {
                return runtime::Ok(DropTarget{candidate});
            }
        }
        return runtime::Ok(DropTarget{});
    }

    /// Returns the leading y coordinate of the first row.
    [[nodiscard]] constexpr float origin_y() const noexcept {
        return origin_y_;
    }
    /// Returns the positive height shared by every row.
    [[nodiscard]] constexpr float row_height() const noexcept {
        return row_height_;
    }
    /// Returns the exclusive y coordinate following the final row.
    [[nodiscard]] float bottom_y() const noexcept {
        return origin_y_ + row_height_ * static_cast<float>(order_.size());
    }
    /// Returns the immutable authored order captured at construction.
    [[nodiscard]] std::span<const timeline::ItemId> track_order() const noexcept {
        return order_;
    }

  private:
    TrackHeaderProjection(std::vector<timeline::ItemId> order, float origin_y, float row_height)
        : order_(std::move(order)), origin_y_(origin_y), row_height_(row_height) {}

    std::vector<timeline::ItemId> order_;
    float origin_y_ = 0.0f;
    float row_height_ = 0.0f;
};

} // namespace pulp::timeline_editor
