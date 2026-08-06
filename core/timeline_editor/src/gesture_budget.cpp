#include <pulp/timeline_editor/gesture_budget.hpp>

#include <limits>

namespace pulp::timeline_editor {

using namespace pulp::timeline;

namespace {

/// Mirrors how DocumentSession combines the two halves of a step's charge:
/// `retained_size` already saturates, so summing two saturated values is the
/// one place the total can wrap.
std::size_t saturated_add(std::size_t lhs, std::size_t rhs) noexcept {
    return lhs > std::numeric_limits<std::size_t>::max() - rhs
               ? std::numeric_limits<std::size_t>::max()
               : lhs + rhs;
}

} // namespace

UndoGestureBudget undo_gesture_budget(const UndoLimits& limits,
                                      std::span<const Command> step_forward,
                                      std::span<const Command> step_inverse) noexcept {
    UndoGestureBudget budget;
    budget.step_bytes = saturated_add(retained_size(step_forward), retained_size(step_inverse));
    // Integer division is the exact boundary rather than an estimate: the group
    // holds `steps * step_bytes` after `steps` steps, and the session refuses
    // the step whose projected total first exceeds the budget.
    budget.steps = budget.step_bytes == 0 ? std::numeric_limits<std::size_t>::max()
                                          : limits.max_retained_bytes / budget.step_bytes;
    return budget;
}

} // namespace pulp::timeline_editor
