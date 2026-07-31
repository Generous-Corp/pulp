#pragma once

/// @file hit_metrics.hpp
/// Device-appropriate target sizing shared by pointer-driven hit tests.

#include <pulp/view/input_events.hpp>

#include <algorithm>

namespace pulp::view {

/// Minimum hit-target sizing for one pointer type.
///
/// A 44pt touch target and a 4px mouse target are the SAME hit test evaluated
/// with different metrics, not two hit tests with two code paths. Callers that
/// used to hard-code a `tolerance_px` now carry a `HitMetrics` and project it,
/// so widening touch targets never means duplicating the geometry.
///
/// `min_target_pt` is the full edge length the target must present to this
/// pointer type. Hit tests measure distance from a target's anchor, so the
/// usable tolerance is half that extent on each side: a mouse asking for an 8pt
/// target accepts anything within 4px on a 1x surface, which is the value the
/// waveform handle hit test carried literally before this existed.
struct HitMetrics {
    PointerType pointer_type = PointerType::mouse;
    /// Full minimum target extent in points. Zero selects the per-type default.
    float min_target_pt = 0.0f;

    /// Minimum comfortable target extent for a pointer type, in points.
    ///
    /// Touch uses the 44pt figure Apple's HIG specifies for finger targets. Pen
    /// sits between the two: more precise than a finger, less than a cursor.
    static constexpr float default_min_target_pt(PointerType type) noexcept {
        switch (type) {
        case PointerType::touch: return 44.0f;
        case PointerType::pen:   return 12.0f;
        case PointerType::mouse: break;
        }
        return 8.0f;
    }

    /// Metrics carrying this pointer type's default target extent.
    static constexpr HitMetrics for_pointer(PointerType type) noexcept {
        return HitMetrics{type, default_min_target_pt(type)};
    }

    /// Effective target extent in points, resolving 0 to the type default.
    [[nodiscard]] constexpr float effective_target_pt() const noexcept {
        return min_target_pt > 0.0f ? min_target_pt : default_min_target_pt(pointer_type);
    }

    /// One-sided tolerance in pixels for a distance-from-anchor hit test.
    ///
    /// `pixels_per_point` converts the point-space target to the surface's pixel
    /// space; it is 1.0 on a 1x surface, where the mouse default yields 4px.
    [[nodiscard]] constexpr float tolerance_px(float pixels_per_point = 1.0f) const noexcept {
        const float scale = pixels_per_point > 0.0f ? pixels_per_point : 1.0f;
        return effective_target_pt() * 0.5f * scale;
    }
};

} // namespace pulp::view
