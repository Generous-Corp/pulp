#pragma once

#include <pulp/state/sequencer_state_channel.hpp>
#include <pulp/timeline/schema_registry.hpp>

#include <optional>

namespace pulp::examples::timeline_phase1 {

inline constexpr const char* kStepPatternSchemaName =
    "pulp.examples.timeline.step_pattern";
inline constexpr std::uint32_t kStepPatternSchemaVersion = 1;

struct StepPatternDocument {
    state::Snapshot snapshot;
};

/// Capacity outside active_pattern_count/active_lane_count is wire padding,
/// not pattern state. Canonical documents require that padding to retain the
/// default PatternState/StepCell values so encoding never silently discards a
/// second, hidden meaning.
bool step_pattern_snapshot_is_canonical(const state::Snapshot& snapshot) noexcept;

std::optional<timeline::SchemaRegistry> make_step_pattern_registry();
std::optional<timeline::RegisteredContent>
make_registered_step_pattern(const state::Snapshot& snapshot,
                             const timeline::SchemaRegistry& registry);

} // namespace pulp::examples::timeline_phase1
