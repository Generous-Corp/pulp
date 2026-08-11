#pragma once

/// @file hit_test.hpp
/// Pointer-neutral hit testing for projected timeline items.

#include <pulp/timeline/item_id.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace pulp::timeline_editor {

/** @addtogroup timeline_editing
 * @{
 */

/// One timeline item after a front-end projects it into pixel space.
///
/// Bounds are half-open: the left and top edges belong to the candidate, while
/// the right and bottom edges do not. Candidates are supplied in paint order,
/// from back to front, so an exact geometric tie resolves to the later item.
struct ProjectedHitCandidate {
    timeline::ItemId item{};
    float left_px = 0.0f;
    float top_px = 0.0f;
    float right_px = 0.0f;
    float bottom_px = 0.0f;

    constexpr bool operator==(const ProjectedHitCandidate&) const = default;
};

/// The horizontal part of a projected item selected by a hit test.
enum class ProjectedHitRegion : std::uint8_t {
    Body,
    LeadingEdge,  ///< Left/start edge in projected timeline space.
    TrailingEdge, ///< Right/end edge in projected timeline space.
};

/// The identity and region selected from a projected candidate set.
struct ProjectedHit {
    timeline::ItemId item{};
    ProjectedHitRegion region = ProjectedHitRegion::Body;

    constexpr bool operator==(const ProjectedHit&) const = default;
};

namespace hit_test_detail {

inline bool valid_candidate(const ProjectedHitCandidate& candidate) noexcept {
    return candidate.item.valid() && std::isfinite(candidate.left_px) &&
           std::isfinite(candidate.top_px) && std::isfinite(candidate.right_px) &&
           std::isfinite(candidate.bottom_px) && candidate.left_px < candidate.right_px &&
           candidate.top_px < candidate.bottom_px;
}

inline bool inside_expanded_half_open(float value, float start, float end,
                                      float tolerance) noexcept {
    const auto wide_value = static_cast<double>(value);
    const auto wide_tolerance = static_cast<double>(tolerance);
    return wide_value >= static_cast<double>(start) - wide_tolerance &&
           wide_value < static_cast<double>(end) + wide_tolerance;
}

inline bool inside_half_open(const ProjectedHitCandidate& candidate, float x, float y) noexcept {
    return x >= candidate.left_px && x < candidate.right_px && y >= candidate.top_px &&
           y < candidate.bottom_px;
}

inline double distance_to_half_open_span(float value, float start, float end) noexcept {
    if (value < start)
        return static_cast<double>(start) - static_cast<double>(value);
    if (value >= end)
        return static_cast<double>(value) - static_cast<double>(end);
    return 0.0;
}

inline ProjectedHitRegion region_at(const ProjectedHitCandidate& candidate, float x,
                                    float tolerance) noexcept {
    if (x < candidate.left_px)
        return ProjectedHitRegion::LeadingEdge;
    if (x >= candidate.right_px)
        return ProjectedHitRegion::TrailingEdge;

    const auto leading = std::abs(static_cast<double>(x) - static_cast<double>(candidate.left_px));
    const auto trailing =
        std::abs(static_cast<double>(x) - static_cast<double>(candidate.right_px));
    const auto width =
        static_cast<double>(candidate.right_px) - static_cast<double>(candidate.left_px);
    const auto edge_band = std::min(static_cast<double>(tolerance), width / 3.0);

    if (leading <= edge_band)
        return ProjectedHitRegion::LeadingEdge;
    if (trailing <= edge_band)
        return ProjectedHitRegion::TrailingEdge;
    return ProjectedHitRegion::Body;
}

} // namespace hit_test_detail

/// Selects one projected timeline item using already-resolved pixel tolerance.
///
/// This lower-rung kernel deliberately does not name `view::HitMetrics` or any
/// view-layer geometry type. A front-end resolves its pointer policy to
/// `tolerance_px`, projects model items into candidates, and passes only those
/// values here.
///
/// Tolerance expands each half-open rectangle on all sides. When expanded
/// rectangles overlap, the candidate with the shortest Euclidean distance to
/// its unexpanded rectangle wins. Containment wins a zero-distance boundary
/// tie, preserving half-open ownership; otherwise an exact distance tie selects
/// the later, frontmost candidate. An acquired point outside the original
/// horizontal span selects that side's edge. Inside the span, each edge band is
/// capped at one third of the item width so even a narrow item retains a body
/// band for move gestures.
///
/// Invalid identities, non-finite or inverted candidate bounds are ignored.
/// A non-finite point, non-finite tolerance, or negative tolerance fails closed.
[[nodiscard]] inline std::optional<ProjectedHit>
hit_test_projected_items(std::span<const ProjectedHitCandidate> back_to_front, float x_px,
                         float y_px, float tolerance_px) noexcept {
    if (!std::isfinite(x_px) || !std::isfinite(y_px) || !std::isfinite(tolerance_px) ||
        tolerance_px < 0.0f)
        return std::nullopt;

    std::optional<ProjectedHit> best;
    auto best_distance_squared = std::numeric_limits<double>::infinity();
    auto best_contains = false;

    for (const auto& candidate : back_to_front) {
        if (!hit_test_detail::valid_candidate(candidate) ||
            !hit_test_detail::inside_expanded_half_open(x_px, candidate.left_px, candidate.right_px,
                                                        tolerance_px) ||
            !hit_test_detail::inside_expanded_half_open(y_px, candidate.top_px, candidate.bottom_px,
                                                        tolerance_px))
            continue;

        const auto dx = hit_test_detail::distance_to_half_open_span(x_px, candidate.left_px,
                                                                    candidate.right_px);
        const auto dy = hit_test_detail::distance_to_half_open_span(y_px, candidate.top_px,
                                                                    candidate.bottom_px);
        const auto distance_squared = dx * dx + dy * dy;
        const auto contains = hit_test_detail::inside_half_open(candidate, x_px, y_px);

        const auto distance_tie = distance_squared == best_distance_squared;
        const auto wins_boundary_tie = distance_tie && contains && !best_contains;
        const auto wins_paint_order_tie = distance_tie && contains == best_contains;
        if (!best || distance_squared < best_distance_squared || wins_boundary_tie ||
            wins_paint_order_tie) {
            best_distance_squared = distance_squared;
            best_contains = contains;
            best = ProjectedHit{candidate.item,
                                hit_test_detail::region_at(candidate, x_px, tolerance_px)};
        }
    }

    return best;
}

/// @}

} // namespace pulp::timeline_editor
