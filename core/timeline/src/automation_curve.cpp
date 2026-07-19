#include <pulp/timeline/automation_curve.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace pulp::timeline {
namespace {

template <typename T>
runtime::Result<T, AutomationCurveError> fail(AutomationCurveErrorCode code, ItemId point = {},
                                              ItemId related = {}) {
    return runtime::Result<T, AutomationCurveError>(
        runtime::Err(AutomationCurveError{code, point, related}));
}

bool valid_point(const AutomationPoint& point, AutomationCurveErrorCode& code) noexcept {
    if (!point.id.valid()) {
        code = AutomationCurveErrorCode::InvalidPointId;
        return false;
    }
    switch (point.interpolation) {
    case AutomationInterpolation::Hold:
    case AutomationInterpolation::Continuous:
        break;
    default:
        code = AutomationCurveErrorCode::InvalidInterpolation;
        return false;
    }
    if (!std::isfinite(point.value)) {
        code = AutomationCurveErrorCode::NonFiniteValue;
        return false;
    }
    if (!std::isfinite(point.curvature) || point.curvature < -1.0f || point.curvature > 1.0f) {
        code = AutomationCurveErrorCode::InvalidCurvature;
        return false;
    }
    return true;
}

double shape_fraction(double fraction, float curvature) noexcept {
    const double bend = static_cast<double>(curvature);
    const double quadratic =
        bend >= 0.0 ? fraction * fraction : std::fma(-fraction, fraction, 2.0 * fraction);
    return std::fma(std::abs(bend), quadratic - fraction, fraction);
}

} // namespace

float evaluate_continuous_automation_segment(timebase::TickPosition position,
                                             timebase::TickPosition start,
                                             timebase::TickPosition end, float start_value,
                                             float end_value, float curvature) noexcept {
    if (end <= start)
        return position < end ? start_value : end_value;
    if (position <= start)
        return start_value;
    if (position >= end)
        return end_value;
    const auto span =
        static_cast<std::uint64_t>(end.value) - static_cast<std::uint64_t>(start.value);
    const auto offset =
        static_cast<std::uint64_t>(position.value) - static_cast<std::uint64_t>(start.value);
    const double fraction =
        std::clamp(static_cast<double>(offset) / static_cast<double>(span), 0.0, 1.0);
    const double shaped = shape_fraction(fraction, curvature);
    return static_cast<float>(std::fma(static_cast<double>(end_value) - start_value, shaped,
                                       static_cast<double>(start_value)));
}

runtime::Result<AutomationCurve, AutomationCurveError>
AutomationCurve::create(std::vector<AutomationPoint> points) {
    std::unordered_set<ItemId> ids;
    ids.reserve(points.size());
    for (const auto& point : points) {
        AutomationCurveErrorCode code{};
        if (!valid_point(point, code))
            return fail<AutomationCurve>(code, point.id);
        if (!ids.insert(point.id).second)
            return fail<AutomationCurve>(AutomationCurveErrorCode::DuplicatePointId, point.id);
    }

    std::sort(points.begin(), points.end(), AutomationPointPositionLess{});
    for (std::size_t index = 1; index < points.size(); ++index) {
        if (points[index - 1].position == points[index].position)
            return fail<AutomationCurve>(AutomationCurveErrorCode::DuplicatePosition,
                                         points[index].id, points[index - 1].id);
    }
    return runtime::Result<AutomationCurve, AutomationCurveError>(runtime::Ok(
        AutomationCurve(std::make_shared<const std::vector<AutomationPoint>>(std::move(points)))));
}

const AutomationPoint* AutomationCurve::find_point(ItemId id) const noexcept {
    const auto found = std::find_if(points_->begin(), points_->end(),
                                    [id](const auto& point) { return point.id == id; });
    return found == points_->end() ? nullptr : &*found;
}

std::optional<float> AutomationCurve::value_at(timebase::TickPosition position) const noexcept {
    if (points_->empty())
        return std::nullopt;
    const auto right =
        std::upper_bound(points_->begin(), points_->end(), position,
                         [](timebase::TickPosition value, const AutomationPoint& point) {
                             return value < point.position;
                         });
    if (right == points_->begin())
        return right->value;
    const auto& left = *std::prev(right);
    if (right == points_->end() || left.interpolation == AutomationInterpolation::Hold)
        return left.value;

    return evaluate_continuous_automation_segment(position, left.position, right->position,
                                                  left.value, right->value, left.curvature);
}

runtime::Result<AutomationCurve, AutomationCurveError>
AutomationCurve::insert_point(AutomationPoint point) const {
    auto next = *points_;
    next.push_back(point);
    return create(std::move(next));
}

runtime::Result<AutomationCurve, AutomationCurveError>
AutomationCurve::replace_point(AutomationPoint point) const {
    auto next = *points_;
    const auto found = std::find_if(next.begin(), next.end(), [point](const auto& candidate) {
        return candidate.id == point.id;
    });
    if (found == next.end())
        return fail<AutomationCurve>(AutomationCurveErrorCode::MissingPoint, point.id);
    *found = point;
    return create(std::move(next));
}

runtime::Result<AutomationCurve, AutomationCurveError>
AutomationCurve::erase_point(ItemId id) const {
    auto next = *points_;
    const auto found =
        std::find_if(next.begin(), next.end(), [id](const auto& point) { return point.id == id; });
    if (found == next.end())
        return fail<AutomationCurve>(AutomationCurveErrorCode::MissingPoint, id);
    next.erase(found);
    return create(std::move(next));
}

} // namespace pulp::timeline
