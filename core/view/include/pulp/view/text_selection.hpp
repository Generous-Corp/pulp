#pragma once

// Document-level text selection — a selection that spans widgets.
//
// A single `TextEditor` has always been able to select within itself. This is
// the layer above: a press in one text-bearing widget, a drag across others,
// and a Cmd-C that yields all of it. The selection is owned by the tree ROOT
// (`View::RootInteractionState::text_selection`), the same place focus lives,
// so two plugin editors in one host process cannot share one selection.
//
// Every widget that takes part implements `SelectableText`
// (`pulp/view/selectable_text.hpp`) and advertises itself through
// `View::as_selectable_text()`.
//
// ── Document order ──────────────────────────────────────────────────────────
//
// The selection between two endpoints covers everything BETWEEN them, which
// means the walk has to be over the tree, not over what the pointer touched:
// dragging from widget A to widget C must fully select the widget B in the
// middle, and the pointer never visits B. Document order here is depth-first
// pre-order over `child_at(0..child_count())` — tree order, deliberately NOT
// the z-order `hit_test` walks, because z-index reorders paint and hit
// priority, not reading order.

#include <pulp/view/selectable_text.hpp>
#include <pulp/view/view.hpp>

#include <string>
#include <vector>

namespace pulp::view {

/// Every `SelectableText` under `root` (inclusive), in document order.
///
/// A widget is included only when `View::as_selectable_text()` returns
/// non-null, so an editable `TextEditor` and a Label that has not opted in are
/// invisible to the walk rather than silently swallowed by a drag across them.
std::vector<SelectableText*> selectable_widgets_in_document_order(View& root);

/// The selectable widget at a root-space point, with the byte offset in it.
///
/// `target` is null only when the tree holds no selectable widget at all. A
/// point OUTSIDE every widget still resolves — to the vertically nearest one,
/// clamped to its first or last offset — because that is what a drag past the
/// end of the text has to mean. Requiring containment would make a drag that
/// leaves the text stop extending, which reads as the selection being stuck.
struct SelectionHit {
    SelectableText* target = nullptr;
    int offset = 0;
};
/// `root_pos` is TREE-root space (a `MouseEvent::window_position`), while the
/// walk is scoped to `scope` — typically the content region. The two differ
/// whenever the region is not at the tree origin.
SelectionHit selection_hit_test(View& scope, Point root_pos);

/// Start a selection. Clears any previous one and marks a drag in flight.
void selection_begin(View& root, SelectableText& at, int utf8_offset);

/// Move the live end of the selection. Safe to call when nothing is selected
/// (it does nothing), so a stray drag cannot invent a selection.
void selection_extend(View& root, SelectableText& to, int utf8_offset);

/// End the drag. The selection itself stays — this only stops tracking.
void selection_end_drag(View& root);

/// Drop the selection and clear every widget's highlight.
void selection_clear(View& root);

/// Select every selectable widget under `root`, from the first byte of the
/// first to the last byte of the last.
void selection_select_all(View& root);

/// True when the live selection covers bytes in more than one widget. The
/// discriminator a widget's own Cmd-C handler needs: a read-only `TextEditor`
/// inside a document selection must copy the WHOLE selection, not the slice it
/// happens to own.
bool selection_spans_multiple_widgets(View& root);

/// True while a press-drag is in flight.
bool selection_is_dragging(View& root);

/// True when the selection covers at least one byte.
bool selection_has_range(View& root);

/// The selected text, in document order, with a `\n` between widgets.
///
/// Newline-joining is a decision, not a detail: a Label is a block, so two
/// adjacent Labels are two lines and pasting them run together would be wrong.
/// A widget that contributes an empty slice still contributes its separator,
/// so a blank line in the source survives the round trip.
std::string selection_text(View& root);

/// Copy `selection_text()` to the system clipboard. False when the selection
/// is empty or the platform clipboard refused.
bool selection_copy(View& root);

/// Re-apply the current selection to every widget's highlight. Called for you
/// by begin/extend/clear; call it directly after mutating the tree under a
/// live selection.
void selection_refresh_highlights(View& root);

}  // namespace pulp::view
