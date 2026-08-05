#pragma once

/// @file gesture_budget.hpp
/// How many steps a coalescing gesture can commit before undo refuses it.

#include <pulp/timeline/undo.hpp>

#include <cstddef>
#include <span>

namespace pulp::timeline_editor {

/** @addtogroup timeline_editing
 * @{
 */

/// What one open gesture may spend against the undo budget.
///
/// A gesture that streams `Update` steps coalesces them all into a single undo
/// group, and `DocumentSession` charges each step `retained_size(forward) +
/// retained_size(inverse)` on top of what the group already holds. The group
/// stays open until its `End`, and the eviction loop advances only over groups
/// that are `closed`, so nothing can be reclaimed to make room for it. Past the
/// budget the next step is refused with `ConflictCode::UndoFull` — a drag that
/// stops responding partway through rather than one that degrades.
///
/// Knowing `steps` up front is what lets an editor pick a strategy before it
/// opens a gesture instead of discovering the ceiling mid-drag: stream while the
/// count is comfortable, and hold the edit locally and commit one
/// `GesturePhase::Single` on release when it is not. A `Single` edit is closed
/// on admission and therefore immediately evictable, so that path has no step
/// ceiling at all.
struct UndoGestureBudget {
    /// What one step charges: its forward commands plus its inverse.
    std::size_t step_bytes = 0;
    /// Steps admitted into one open group, counting the opening `Begin`.
    ///
    /// Zero means the gesture cannot be opened at all: one step already exceeds
    /// the whole budget, and an open group has nothing to evict to make room.
    /// `SIZE_MAX` means nothing is charged, which no real command manages —
    /// every `retained_size` arm includes `sizeof(T)` — but an empty step does.
    std::size_t steps = 0;
};

/// Returns what one open gesture of uniform per-step cost may spend.
///
/// It sits at the editor rung rather than beside `UndoLimits` for the reason the
/// edit verbs do: the document model *enforces* the ceiling and needs no help
/// planning around it, while choosing between streaming a gesture and committing
/// one edit on release is an editor's decision alone. A headless importer, a
/// `.pulpgraph` loader, and a plugin that wants only commands should not carry
/// it.
///
/// Pure, and deliberately takes limits rather than a session: the answer does
/// not depend on what the undo stack already holds. `open_gesture` is one
/// `optional` per session, so every group present when a gesture opens is
/// closed, the eviction loop reclaims all of them, and the whole
/// `max_retained_bytes` is available to the open group alone. `max_groups`
/// cannot bind either: only the opening `Begin` adds a group, and the same
/// eviction makes room for it, while every step after it coalesces into that
/// group rather than adding another.
///
/// **This is the undo ceiling only**, and which ceiling actually binds depends
/// on the payload rather than being fixed. `JournalLimits` bounds the same
/// gesture independently and has no automatic eviction, only `checkpoint()`.
/// Its byte ceiling never binds first: a step charges the journal roughly its
/// forward commands and the undo stack roughly forward *plus* inverse, so at
/// twice the charge against half the bytes (8 MiB against 16 MiB) undo is about
/// four times the tighter of the two. Its flat `max_transactions` cap is the
/// axis that can, since a step is one transaction however small.
///
/// So the two swap around a payload size: above it undo binds and this number
/// is the session's real ceiling; below it the journal's flat transaction cap
/// binds and this number is optimistic. The crossover is where a step's undo
/// charge reaches `max_retained_bytes / max_transactions`, which at the defaults
/// is a whole-content note edit over a clip of roughly sixty notes. A caller
/// below that gets the flat cap — 1024 steps, some seventeen seconds of
/// continuous dragging at 60 Hz — which is longer than a real gesture, and a
/// caller above it is in the regime where the undo ceiling is the problem in the
/// first place. `test_timeline_gesture_budget.cpp` pins both sides.
///
/// The step is taken to be representative: `steps` is the count at THIS cost.
/// That holds for a drag that moves or reshapes a fixed set of notes, whose
/// arrays keep their size across the gesture. A gesture whose payload grows as
/// it runs should be priced on the largest step it will reach, since a count
/// derived from the first one overestimates.
///
/// @param limits The session's undo ceilings.
/// @param step_forward Commands one step submits.
/// @param step_inverse The inverse the reducer derives for that step.
/// @return The per-step charge and the number of steps one open gesture admits.
UndoGestureBudget undo_gesture_budget(const timeline::UndoLimits& limits,
                                      std::span<const timeline::Command> step_forward,
                                      std::span<const timeline::Command> step_inverse) noexcept;

/// @}

} // namespace pulp::timeline_editor
