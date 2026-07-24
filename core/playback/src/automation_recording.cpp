#include <pulp/playback/automation_recording.hpp>

#include <pulp/runtime/exceptions.hpp>
#include <pulp/timeline/model.hpp>

#include <cmath>
#include <utility>

namespace pulp::playback {

bool AutomationRecorder::prepare(std::size_t maximum_points) {
    release();
    if (maximum_points == 0 || maximum_points > points_.max_size())
        return false;
    PULP_TRY {
        points_.reserve(maximum_points);
    }
    PULP_CATCH_ALL {
        return false;
    }
    maximum_points_ = maximum_points;
    prepared_ = true;
    return true;
}

void AutomationRecorder::release() noexcept {
    points_.clear();
    maximum_points_ = 0;
    latched_value_ = 0.0f;
    gesture_was_active_ = false;
    latch_started_ = false;
    recording_ = false;
    prepared_ = false;
    recording_snapshot_.store(false, std::memory_order_release);
    dropped_points_.store(0, std::memory_order_relaxed);
}

bool AutomationRecorder::begin(AutomationRecordMode mode) noexcept {
    if (!prepared_ || recording_)
        return false;
    points_.clear();
    mode_ = mode;
    latched_value_ = 0.0f;
    gesture_was_active_ = false;
    latch_started_ = false;
    recording_ = true;
    recording_snapshot_.store(true, std::memory_order_release);
    return true;
}

AutomationRecordError AutomationRecorder::append(timebase::TickPosition position,
                                                 float value) noexcept {
    if (!std::isfinite(value))
        return AutomationRecordError::InvalidValue;
    if (!points_.empty()) {
        if (position < points_.back().position)
            return AutomationRecordError::NonMonotonicPosition;
        if (position == points_.back().position) {
            points_.back().value = value;
            return AutomationRecordError::None;
        }
    }
    if (points_.size() >= maximum_points_) {
        dropped_points_.fetch_add(1, std::memory_order_relaxed);
        return AutomationRecordError::CapacityExceeded;
    }
    points_.push_back({position, value});
    return AutomationRecordError::None;
}

AutomationRecordError AutomationRecorder::record(timebase::TickPosition position, float value,
                                                 bool gesture_active) noexcept {
    if (!prepared_)
        return AutomationRecordError::NotPrepared;
    if (!recording_)
        return AutomationRecordError::NotRecording;
    if (!std::isfinite(value))
        return AutomationRecordError::InvalidValue;

    AutomationRecordError result = AutomationRecordError::None;
    switch (mode_) {
    case AutomationRecordMode::Write:
        result = append(position, value);
        break;
    case AutomationRecordMode::Touch:
        if (gesture_active || gesture_was_active_)
            result = append(position, value);
        break;
    case AutomationRecordMode::Latch:
        if (gesture_active) {
            latch_started_ = true;
            latched_value_ = value;
        }
        if (latch_started_)
            result = append(position, latched_value_);
        break;
    }
    gesture_was_active_ = gesture_active;
    return result;
}

bool AutomationRecorder::end() noexcept {
    if (!recording_)
        return false;
    recording_ = false;
    gesture_was_active_ = false;
    latch_started_ = false;
    recording_snapshot_.store(false, std::memory_order_release);
    return true;
}

std::span<const RecordedAutomationPoint> AutomationRecorder::points() const noexcept {
    return points_;
}

bool AutomationRecorder::recording() const noexcept {
    return recording_snapshot_.load(std::memory_order_acquire);
}

std::uint64_t AutomationRecorder::dropped_points() const noexcept {
    return dropped_points_.load(std::memory_order_relaxed);
}

runtime::Result<MaterializedAutomationCurve, AutomationCurveMaterializationError>
AutomationRecorder::materialize_curve(std::uint64_t next_item_id) const {
    using Result =
        runtime::Result<MaterializedAutomationCurve, AutomationCurveMaterializationError>;
    if (recording_ || points_.empty())
        return Result(runtime::Err(AutomationCurveMaterializationError::NoPoints));
    timeline::ItemIdAllocator ids(next_item_id);
    std::vector<timeline::AutomationPoint> points;
    points.reserve(points_.size());
    for (const auto& recorded : points_) {
        auto id = ids.allocate();
        if (!id)
            return Result(runtime::Err(AutomationCurveMaterializationError::IdentityExhausted));
        points.push_back({
            std::move(id).value(),
            recorded.position,
            recorded.value,
            timeline::AutomationInterpolation::Continuous,
            0.0f,
        });
    }
    auto curve = timeline::AutomationCurve::create(std::move(points));
    if (!curve)
        return Result(runtime::Err(AutomationCurveMaterializationError::InvalidCurve));
    return Result(runtime::Ok(MaterializedAutomationCurve{
        std::move(curve).value(),
        ids.next_value(),
    }));
}

} // namespace pulp::playback
