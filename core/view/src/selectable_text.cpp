#include <pulp/view/selectable_text.hpp>

#include <pulp/view/view.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace pulp::view {
namespace {

/// The row whose vertical band is NEAREST `y`.
///
/// One rule, deliberately, rather than a containment test plus a clamp plus a
/// fallback. Containment is distance zero, so a point on a row wins outright;
/// a point above the first row, below the last, or in a GAP between two bands
/// resolves to the nearest band by the same arithmetic.
///
/// The earlier three-branch version had a fourth case that no branch owned —
/// its guards only established `front().top < y < last bottom`, which a gap
/// between bands or a zero-height row satisfies while matching no row. That
/// path fell off the end of a non-void function. Nearest-band has no such
/// path: the loop always assigns, because distance is defined for every row.
///
/// Resolving to the nearest band rather than rejecting is also what lets a
/// drag that has left the widget vertically still resolve to its first or last
/// line — the behaviour a cross-widget drag depends on, since the pointer
/// spends most of such a drag outside the widget it is selecting into.
///
/// Ties resolve DOWNWARD (`<=`), which keeps the half-open band convention the
/// containment test used to have: a point exactly on the seam between two rows
/// belongs to the lower one.
std::size_t row_for_y(const std::vector<SelectableLine>& lines, float y) {
    std::size_t best = 0;
    float best_distance = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto& row = lines[i];
        float distance = 0.0f;
        if (y < row.top)
            distance = row.top - y;
        else if (y > row.top + row.height)
            distance = y - (row.top + row.height);
        if (distance <= best_distance) {
            best_distance = distance;
            best = i;
        }
    }
    return best;
}

/// Nearest cluster boundary to `x` on one row.
int boundary_for_x(const SelectableLine& row, float x) {
    if (row.byte_offsets.empty())
        return row.start_utf8;
    std::size_t best = 0;
    float best_dist = std::abs(x - row.x_offsets[0]);
    for (std::size_t j = 1; j < row.x_offsets.size(); ++j) {
        const float d = std::abs(x - row.x_offsets[j]);
        // Strictly-less keeps the LEFTMOST boundary on an exact tie, so a
        // click on a zero-advance cluster boundary is stable rather than
        // dependent on iteration order.
        if (d < best_dist) {
            best_dist = d;
            best = j;
        }
    }
    return row.byte_offsets[best];
}

bool usable(const SelectableLayout& layout) {
    return layout.measured && !layout.lines.empty();
}

} // namespace

View* enclosing_text_selection_region(View& from) {
    for (View* v = &from; v != nullptr; v = v->parent())
        if (v->text_selection_region())
            return v;
    return nullptr;
}

int selectable_index_at_point(const SelectableLayout& layout, Point local) {
    if (!usable(layout))
        return -1;
    const auto& row = layout.lines[row_for_y(layout.lines, local.y)];
    return boundary_for_x(row, local.x);
}

int selectable_first_index(const SelectableLayout& layout) {
    if (!usable(layout))
        return -1;
    return layout.lines.front().start_utf8;
}

int selectable_last_index(const SelectableLayout& layout) {
    if (!usable(layout))
        return -1;
    const auto& last = layout.lines.back();
    if (!last.byte_offsets.empty())
        return last.byte_offsets.back();
    return last.end_utf8;
}

std::vector<Rect> selectable_rects_for_range(const SelectableLayout& layout, int start_utf8,
                                             int end_utf8) {
    std::vector<Rect> out;
    if (!usable(layout))
        return out;
    if (start_utf8 > end_utf8)
        std::swap(start_utf8, end_utf8);
    if (start_utf8 == end_utf8)
        return out;

    for (const auto& row : layout.lines) {
        if (row.byte_offsets.empty())
            continue;
        // A line's selectable span runs to its last recorded boundary, which
        // for a soft-wrapped line is the boundary before the break. Using
        // `end_utf8` of the line instead would include the break character
        // and paint a band past the last glyph.
        const int row_lo = row.byte_offsets.front();
        const int row_hi = row.byte_offsets.back();
        const int lo = std::max(start_utf8, row_lo);
        const int hi = std::min(end_utf8, row_hi);
        if (hi <= lo)
            continue;

        // Map a byte offset to its x by finding the boundary at or before it;
        // an offset that lands mid-cluster (a caller slicing on bytes rather
        // than clusters) snaps left rather than being dropped.
        auto x_at = [&](int byte) {
            std::size_t idx = 0;
            for (std::size_t j = 0; j < row.byte_offsets.size(); ++j) {
                if (row.byte_offsets[j] <= byte)
                    idx = j;
                else
                    break;
            }
            return row.x_offsets[idx];
        };
        const float x0 = x_at(lo);
        const float x1 = x_at(hi);
        if (x1 <= x0)
            continue;
        out.push_back(Rect{x0, row.top, x1 - x0, row.height});
    }
    return out;
}

} // namespace pulp::view
