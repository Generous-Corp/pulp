#pragma once

#include <pulp/audio/rt_safety_contract.hpp>
#include <pulp/playback/automation_program.hpp>
#include <pulp/playback/transport.hpp>

#include <cstdint>
#include <limits>
#include <span>

namespace pulp::playback {

enum class AutomationTransition : std::uint8_t {
    Seed,
    Immediate,
    LinearRamp,
};

struct AutomationBlockEvent {
    std::uint32_t sample_offset = 0;
    float value = 0.0f;
    AutomationTransition transition = AutomationTransition::Seed;
    constexpr bool operator==(const AutomationBlockEvent&) const = default;
};

enum class AutomationCursorCode : std::uint8_t {
    Ok,
    Coalesced,
    AdoptionRejected,
    InvalidTransport,
    TempoMapMismatch,
    InsufficientCapacity,
    WorkCapacityExceeded,
};

enum class AutomationProgramAdoption : std::uint8_t {
    Adopted,
    Unchanged,
    Rejected,
};

struct AutomationCursorResult {
    AutomationCursorCode code = AutomationCursorCode::Ok;
    AutomationProgramAdoption adoption = AutomationProgramAdoption::Unchanged;
    std::uint32_t emitted_events = 0;
    /// Distinct mandatory topology and continuous-refinement positions before
    /// applying the caller's output budget.
    std::uint32_t candidate_points = 0;
    /// Compiled segments intersecting the active transport ranges.
    std::uint32_t intersecting_segments = 0;
};

/// Allocation-free renderer for one immutable AutomationProgram. The caller
/// owns the output budget and routes the resulting plain-domain control points.
/// A non-seed point describes the transition from the preceding emitted point
/// in the same transport range. `process()` may use the span as bounded scratch;
/// only its first `emitted_events` entries are defined when the call returns.
class AutomationCursor {
  public:
    static constexpr audio::RtSafetyClass process_rt_safety_class =
        audio::RtSafetyClass::AudioCallbackSafeWithImmutableInputs;

    AutomationCursorResult process(const AutomationProgram& program,
                                   const TransportSnapshot& transport,
                                   std::span<AutomationBlockEvent> output,
                                   std::uint32_t max_intersecting_segments =
                                       std::numeric_limits<std::uint32_t>::max()) noexcept;
    void reset() noexcept;

    timeline::ItemId active_lane_id() const noexcept {
        return active_key_.item_id;
    }
    ProgramGeneration active_generation() const noexcept {
        return active_key_.generation;
    }

  private:
    RendererProgramKey active_key_;
    AutomationProgramInstanceToken active_instance_token_;
    bool has_block_index_ = false;
    std::uint64_t last_block_index_ = 0;
};

} // namespace pulp::playback
