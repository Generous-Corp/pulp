#pragma once

#include <pulp/audio/rt_safety_contract.hpp>
#include <pulp/runtime/result.hpp>
#include <pulp/timebase/tick.hpp>
#include <pulp/timeline/automation_curve.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace pulp::playback {

enum class AutomationRecordMode : std::uint8_t {
    Touch,
    Latch,
    Write,
};

struct RecordedAutomationPoint {
    timebase::TickPosition position;
    float value = 0.0f;
    constexpr bool operator==(const RecordedAutomationPoint&) const = default;
};

enum class AutomationRecordError : std::uint8_t {
    None,
    NotPrepared,
    NotRecording,
    InvalidValue,
    NonMonotonicPosition,
    CapacityExceeded,
};

enum class AutomationCurveMaterializationError : std::uint8_t {
    NoPoints,
    IdentityExhausted,
    InvalidCurve,
};

struct MaterializedAutomationCurve {
    timeline::AutomationCurve curve;
    std::uint64_t next_item_id = 1;
};

/// Captures parameter gestures into a bounded point stream. All state-mutating
/// methods belong to one callback thread; materialize_curve() runs only after
/// end() while that owner is quiescent.
class AutomationRecorder {
  public:
    static constexpr audio::RtSafetyClass record_rt_safety_class =
        audio::RtSafetyClass::AudioCallbackSafeAfterPrepare;

    bool prepare(std::size_t maximum_points);
    void release() noexcept;
    bool begin(AutomationRecordMode mode) noexcept;
    AutomationRecordError record(timebase::TickPosition position, float value,
                                 bool gesture_active) noexcept;
    bool end() noexcept;

    std::span<const RecordedAutomationPoint> points() const noexcept;
    bool recording() const noexcept;
    std::uint64_t dropped_points() const noexcept;

    runtime::Result<MaterializedAutomationCurve, AutomationCurveMaterializationError>
    materialize_curve(std::uint64_t next_item_id) const;

  private:
    AutomationRecordError append(timebase::TickPosition position, float value) noexcept;

    std::vector<RecordedAutomationPoint> points_;
    std::size_t maximum_points_ = 0;
    AutomationRecordMode mode_ = AutomationRecordMode::Touch;
    float latched_value_ = 0.0f;
    bool gesture_was_active_ = false;
    bool latch_started_ = false;
    bool recording_ = false;
    bool prepared_ = false;
    std::atomic<bool> recording_snapshot_{false};
    std::atomic<std::uint64_t> dropped_points_{0};
};

} // namespace pulp::playback
