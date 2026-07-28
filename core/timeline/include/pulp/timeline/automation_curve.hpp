#pragma once

#include <pulp/runtime/result.hpp>
#include <pulp/timebase/tick.hpp>
#include <pulp/timeline/item_id.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace pulp::timeline {

/** @addtogroup timeline_model
 * @{
 */

/// Interpolation applied from an automation point to its successor.
enum class AutomationInterpolation : std::uint8_t {
    Hold,
    Continuous,
};

/// One identity-bearing automation value at a canonical musical tick.
///
/// `value` is in the target's plain parameter domain. `curvature` controls a
/// continuous outgoing segment and must be finite and in the admitted range.
struct AutomationPoint {
    ItemId id;
    timebase::TickPosition position;
    float value = 0.0f;
    AutomationInterpolation interpolation = AutomationInterpolation::Continuous;
    float curvature = 0.0f;

    constexpr bool operator==(const AutomationPoint&) const = default;
};

/// Canonical ordering for authored automation points and curve storage.
struct AutomationPointPositionLess {
    /// Orders by position and then stable point identity.
    constexpr bool operator()(const AutomationPoint& lhs,
                              const AutomationPoint& rhs) const noexcept {
        if (lhs.position != rhs.position)
            return lhs.position < rhs.position;
        return lhs.id < rhs.id;
    }
};

/// Evaluates one continuous segment and clamps outside it. Reversed or empty
/// intervals resolve to their start value before `end` and end value otherwise.
/// @param position Position to evaluate, in canonical ticks.
/// @param start Inclusive start of the segment.
/// @param end End of the segment.
/// @param start_value Value at `start`.
/// @param end_value Value at `end`.
/// @param curvature Signed quadratic curvature; zero is linear.
/// @return The deterministically evaluated value.
float evaluate_continuous_automation_segment(timebase::TickPosition position,
                                             timebase::TickPosition start,
                                             timebase::TickPosition end, float start_value,
                                             float end_value, float curvature) noexcept;

/// Validation failures returned by AutomationCurve factories and edits.
enum class AutomationCurveErrorCode : std::uint8_t {
    InvalidPointId,
    DuplicatePointId,
    DuplicatePosition,
    InvalidInterpolation,
    NonFiniteValue,
    InvalidCurvature,
    MissingPoint,
};

/// Automation curve failure with the offending and, when relevant, conflicting identities.
struct AutomationCurveError {
    AutomationCurveErrorCode code = AutomationCurveErrorCode::InvalidPointId;
    ItemId point;
    ItemId related_point;
};

/// Immutable, position-ordered automation points. A point's interpolation and
/// curvature describe the segment from that point to the next point. Dormant
/// metadata (the final point's segment fields and curvature on a hold segment)
/// is retained so later edits preserve the author's settings. Continuous
/// segments use a monotonic quadratic blend: zero is linear, positive values
/// ease in, and negative values ease out.
class AutomationCurve {
  public:
    /// Validates, canonically orders, and immutably stores `points`.
    ///
    /// Point identities and positions must be unique; numeric fields must be
    /// finite and interpolation values valid.
    static runtime::Result<AutomationCurve, AutomationCurveError>
    create(std::vector<AutomationPoint> points);

    /// Returns points in canonical position-then-identity order.
    ///
    /// The span remains valid while this curve snapshot remains alive.
    std::span<const AutomationPoint> points() const noexcept {
        return *points_;
    }
    /// Finds a point by identity, or returns `nullptr` when absent.
    ///
    /// The returned pointer is owned by this immutable curve snapshot.
    const AutomationPoint* find_point(ItemId id) const noexcept;

    /// Random-access evaluation for control-thread and compile-time use. An
    /// empty curve has no value; positions outside a non-empty curve clamp to
    /// its first or last value. Evaluation uses IEEE double arithmetic with
    /// explicit fused multiply-add operations, followed by one float conversion.
    /// The audio thread consumes a compiled automation cursor instead.
    std::optional<float> value_at(timebase::TickPosition position) const noexcept;

    /// Returns a new curve containing `point`; this snapshot remains unchanged.
    ///
    /// Fails when the point is invalid or conflicts by identity or position.
    runtime::Result<AutomationCurve, AutomationCurveError>
    insert_point(AutomationPoint point) const;
    /// Returns a new curve replacing the point with the same identity.
    ///
    /// Fails when the identity is absent or the replacement is invalid or
    /// collides with another point's position.
    runtime::Result<AutomationCurve, AutomationCurveError>
    replace_point(AutomationPoint point) const;
    /// Returns a new curve without `id`, or `MissingPoint` when absent.
    runtime::Result<AutomationCurve, AutomationCurveError> erase_point(ItemId id) const;

  private:
    explicit AutomationCurve(std::shared_ptr<const std::vector<AutomationPoint>> points)
        : points_(std::move(points)) {}

    std::shared_ptr<const std::vector<AutomationPoint>> points_;
};

/// @}

} // namespace pulp::timeline
