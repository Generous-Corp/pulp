#pragma once

// SelectableText — the capability that lets a text-bearing widget take part in
// a selection that spans more than one widget.
//
// The shape follows the repo's "expose the pipeline, not just the endpoints"
// rule. Both `Label` and `TextEditor` already compute, during paint, the exact
// intermediate a selection needs: where each painted line sits and where every
// cluster boundary falls inside it. `TextEditor` kept that intermediate private
// in `LayoutSnapshot`; `Label` threw it away entirely. This header names that
// intermediate once, so the hit-test and rect arithmetic are written a single
// time instead of per widget.
//
// Offsets are UTF-8 BYTE offsets into `selectable_text()`, matching the units
// `TextEditor` already uses for its own selection and the units
// `text_accessibility.hpp` reports to assistive technology
// (`selection_start_utf8` / `selection_end_utf8`).

#include <pulp/view/geometry.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace pulp::view {

class View;

/// One painted line of selectable text, in the widget's LOCAL coordinates.
///
/// `x_offsets[i]` is the x of the cluster boundary at `byte_offsets[i]`,
/// measured from the widget's local origin (the line's own indent is already
/// folded in). The two vectors are always the same length, and both are
/// non-empty for a line that reports any geometry at all: a zero-length line
/// still carries the single boundary that is its caret position.
struct SelectableLine {
    int start_utf8 = 0;  ///< First byte of this line in `selectable_text()`.
    int end_utf8 = 0;    ///< One past this line's last byte.
    float top = 0.0f;
    float height = 0.0f;
    std::vector<float> x_offsets;
    std::vector<int> byte_offsets;
};

/// The painted line geometry of one widget.
///
/// `measured == false` means UNKNOWN, not empty — the widget has not painted
/// yet, or it paints through a path this capability does not model. A caller
/// must never read it as "this widget has no text": that is the failure mode
/// `Label::PaintedTextExtents::measured` exists to prevent, and it is the same
/// mistake here.
struct SelectableLayout {
    bool measured = false;
    std::vector<SelectableLine> lines;
};

/// A widget whose painted text can join a document-level selection.
class SelectableText {
public:
    virtual ~SelectableText() = default;

    /// The widget's text, in UTF-8. This is the SOURCE string — what a copy
    /// should yield — not the transformed string the widget paints. A Label
    /// under `text-transform: uppercase` reports its original casing.
    virtual std::string_view selectable_text() const = 0;

    /// Where the glyphs landed at the last paint. See `SelectableLayout`.
    virtual SelectableLayout selectable_layout() const = 0;

    /// Paint this byte range as selected. `start >= end` clears the highlight.
    virtual void set_selection_highlight(int start_utf8, int end_utf8) = 0;

    /// The current highlight. Returns false when nothing is highlighted.
    virtual bool selection_highlight(int& start_utf8, int& end_utf8) const = 0;

    /// The View this capability is mixed into. Never null.
    virtual View* selectable_view() = 0;
    const View* selectable_view() const {
        return const_cast<SelectableText*>(this)->selectable_view();
    }
};

/// The nearest enclosing text-content region of `from` (itself included), or
/// null when it is not inside one.
///
/// A free function rather than a `View` method: the flag is View state, but
/// resolving it is a selection-subsystem question, and keeping the walk here
/// puts it beside the rest of the capability's arithmetic. Walked rather than
/// cached, so a reparent cannot leave a stale answer behind.
View* enclosing_text_selection_region(View& from);

/// Nearest cluster boundary to `local` within `layout`.
///
/// Rows are picked by y (clamped to the first/last row, so a point above or
/// below the text still resolves), then the closest boundary on that row by x
/// — the half-glyph convention `TextEditor::char_index_at_point` already uses.
/// Returns -1 when `layout.measured` is false, so "unknown" stays
/// distinguishable from "offset 0".
int selectable_index_at_point(const SelectableLayout& layout, Point local);

/// The painted rects covering `[start_utf8, end_utf8)`, in local coordinates,
/// one per touched line. A collapsed range yields no rects.
std::vector<Rect> selectable_rects_for_range(const SelectableLayout& layout,
                                             int start_utf8, int end_utf8);

/// First / last selectable byte offset in `layout`. Both return -1 when the
/// layout is unmeasured. `selectable_last_index` is the offset one past the
/// final cluster, i.e. where a caret sits at end-of-text.
int selectable_first_index(const SelectableLayout& layout);
int selectable_last_index(const SelectableLayout& layout);

}  // namespace pulp::view
