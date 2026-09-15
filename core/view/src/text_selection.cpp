#include <pulp/view/text_selection.hpp>

#include <pulp/view/pointer_dispatch.hpp>
#include <pulp/platform/clipboard.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace pulp::view {
namespace {

void collect(View& v, std::vector<SelectableText*>& out) {
    if (auto* s = v.as_selectable_text()) out.push_back(s);
    for (std::size_t i = 0; i < v.child_count(); ++i)
        if (auto* c = v.child_at(i)) collect(*c, out);
}

/// Index of `target` in `order`, or -1.
int index_of(const std::vector<SelectableText*>& order, const SelectableText* target) {
    for (std::size_t i = 0; i < order.size(); ++i)
        if (order[i] == target) return static_cast<int>(i);
    return -1;
}

/// The byte range covering ALL of one widget.
///
/// When the widget reports geometry, its own first/last boundary is
/// authoritative — that is what is actually on screen, which for a clamped or
/// ellipsised Label is less than its full string. When it reports none
/// (`measured == false`: never painted, or scrolled out of view), fall back to
/// the whole string. That fallback is the point: an intervening widget the user
/// never dragged over must be fully selected even if it has never painted, or a
/// drag from A to C would copy a hole where B is.
void full_range(SelectableText& w, int& lo, int& hi) {
    const auto layout = w.selectable_layout();
    const int first = selectable_first_index(layout);
    const int last = selectable_last_index(layout);
    if (first >= 0 && last >= first) {
        lo = first;
        hi = last;
        return;
    }
    lo = 0;
    hi = static_cast<int>(w.selectable_text().size());
}

struct Endpoints {
    SelectableText* lo_widget = nullptr;
    SelectableText* hi_widget = nullptr;
    int lo_offset = 0;
    int hi_offset = 0;
    int lo_index = -1;
    int hi_index = -1;
};

/// Resolve the stored anchor/focus into document-ordered endpoints.
/// `lo_index < 0` means there is no usable selection.
Endpoints resolve(View& root, const std::vector<SelectableText*>& order) {
    Endpoints e;
    auto& sel = root.interaction().text_selection;
    View* a = sel.anchor.live_in(root);
    View* f = sel.focus.live_in(root);
    if (a == nullptr || f == nullptr) return e;
    auto* as = a->as_selectable_text();
    auto* fs = f->as_selectable_text();
    if (as == nullptr || fs == nullptr) return e;

    const int ai = index_of(order, as);
    const int fi = index_of(order, fs);
    if (ai < 0 || fi < 0) return e;

    const bool anchor_first =
        ai < fi || (ai == fi && sel.anchor_utf8 <= sel.focus_utf8);
    e.lo_index = anchor_first ? ai : fi;
    e.hi_index = anchor_first ? fi : ai;
    e.lo_widget = anchor_first ? as : fs;
    e.hi_widget = anchor_first ? fs : as;
    e.lo_offset = anchor_first ? sel.anchor_utf8 : sel.focus_utf8;
    e.hi_offset = anchor_first ? sel.focus_utf8 : sel.anchor_utf8;
    return e;
}

/// The byte range widget `i` contributes, given ordered endpoints.
void range_for(SelectableText& w, int i, const Endpoints& e, int& lo, int& hi) {
    if (i < e.lo_index || i > e.hi_index) {
        lo = hi = 0;
        return;
    }
    int wlo = 0, whi = 0;
    full_range(w, wlo, whi);
    if (e.lo_index == e.hi_index) {
        lo = std::min(e.lo_offset, e.hi_offset);
        hi = std::max(e.lo_offset, e.hi_offset);
    } else if (i == e.lo_index) {
        lo = e.lo_offset;
        hi = whi;
    } else if (i == e.hi_index) {
        lo = wlo;
        hi = e.hi_offset;
    } else {
        // The intervening case — the widget the pointer never visited.
        lo = wlo;
        hi = whi;
    }
    lo = std::clamp(lo, wlo, whi);
    hi = std::clamp(hi, wlo, whi);
    if (hi < lo) std::swap(lo, hi);
}

}  // namespace

std::vector<SelectableText*> selectable_widgets_in_document_order(View& root) {
    std::vector<SelectableText*> out;
    collect(root, out);
    return out;
}

SelectionHit selection_hit_test(View& scope, Point root_pos) {
    SelectionHit hit;
    const auto order = selectable_widgets_in_document_order(scope);
    if (order.empty()) return hit;

    // `root_pos` is TREE-root space (a MouseEvent's `window_position`), so the
    // conversion has to start at the tree root even though the WALK is scoped
    // to the content region. Converting relative to the region instead drops
    // every bounds offset between it and the root, which is invisible for a
    // region that happens to sit at the origin and wrong by exactly the
    // region's offset for every other one.
    View* tree = &scope;
    while (tree->parent() != nullptr) tree = tree->parent();

    SelectableText* best = nullptr;
    Point best_local{};
    float best_dy = 0.0f;
    bool best_contains = false;

    for (std::size_t i = 0; i < order.size(); ++i) {
        View* v = order[i]->selectable_view();
        const Point local = point_to_local(root_pos, v, tree);
        const Rect lb = v->local_bounds();
        // Signed vertical miss distance: 0 when the point is on this widget's
        // band, negative above it, positive below.
        float dy = 0.0f;
        if (local.y < lb.y) dy = local.y - lb.y;
        else if (local.y > lb.y + lb.height) dy = local.y - (lb.y + lb.height);
        const bool contains = dy == 0.0f && local.x >= lb.x &&
                              local.x <= lb.x + lb.width;

        if (best == nullptr) {
            best = order[i];
            best_local = local;
            best_dy = dy;
            best_contains = contains;
            continue;
        }
        // Containment wins outright. Otherwise take the smaller vertical miss;
        // on a tie prefer the LAST widget when the point is below them all and
        // the FIRST when it is above, so a drag off the bottom lands at the end
        // of the document rather than on whichever sibling happened to be
        // enumerated first.
        const bool better =
            (contains && !best_contains) ||
            (contains == best_contains &&
             (std::abs(dy) < std::abs(best_dy) ||
              (std::abs(dy) == std::abs(best_dy) && dy > 0.0f)));
        if (better) {
            best = order[i];
            best_local = local;
            best_dy = dy;
            best_contains = contains;
        }
    }
    if (best == nullptr) return hit;

    hit.target = best;
    const auto layout = best->selectable_layout();
    const int idx = selectable_index_at_point(layout, best_local);
    if (idx >= 0) {
        hit.offset = idx;
    } else {
        // Unmeasured widget (never painted). Clamp to an end rather than
        // guessing an interior offset from advances we do not have.
        hit.offset = best_dy > 0.0f
                         ? static_cast<int>(best->selectable_text().size())
                         : 0;
    }
    return hit;
}

void selection_begin(View& root, SelectableText& at, int utf8_offset) {
    auto& sel = root.interaction().text_selection;
    sel.anchor.set(at.selectable_view());
    sel.focus.set(at.selectable_view());
    sel.anchor_utf8 = utf8_offset;
    sel.focus_utf8 = utf8_offset;
    sel.dragging = true;
    selection_refresh_highlights(root);
}

void selection_extend(View& root, SelectableText& to, int utf8_offset) {
    auto& sel = root.interaction().text_selection;
    if (!sel.anchor.has_value()) return;
    sel.focus.set(to.selectable_view());
    sel.focus_utf8 = utf8_offset;
    selection_refresh_highlights(root);
}

void selection_end_drag(View& root) {
    root.interaction().text_selection.dragging = false;
}

void selection_clear(View& root) {
    auto* state = root.existing_interaction();
    if (state == nullptr) return;
    state->text_selection = View::TextSelectionState{};
    for (auto* w : selectable_widgets_in_document_order(root))
        w->set_selection_highlight(0, 0);
}

bool selection_is_dragging(View& root) {
    auto* state = root.existing_interaction();
    return state != nullptr && state->text_selection.dragging;
}

void selection_refresh_highlights(View& root) {
    const auto order = selectable_widgets_in_document_order(root);
    const Endpoints e = resolve(root, order);
    for (std::size_t i = 0; i < order.size(); ++i) {
        int lo = 0, hi = 0;
        if (e.lo_index >= 0) range_for(*order[i], static_cast<int>(i), e, lo, hi);
        order[i]->set_selection_highlight(lo, hi);
    }
}

void selection_select_all(View& root) {
    const auto order = selectable_widgets_in_document_order(root);
    if (order.empty()) return;
    auto& sel = root.interaction().text_selection;
    int lo = 0, hi = 0;
    full_range(*order.front(), lo, hi);
    sel.anchor.set(order.front()->selectable_view());
    sel.anchor_utf8 = lo;
    full_range(*order.back(), lo, hi);
    sel.focus.set(order.back()->selectable_view());
    sel.focus_utf8 = hi;
    sel.dragging = false;
    selection_refresh_highlights(root);
}

bool selection_spans_multiple_widgets(View& root) {
    auto* state = root.existing_interaction();
    if (state == nullptr) return false;
    const auto order = selectable_widgets_in_document_order(root);
    const Endpoints e = resolve(root, order);
    if (e.lo_index < 0) return false;
    int contributing = 0;
    for (std::size_t i = 0; i < order.size(); ++i) {
        int lo = 0, hi = 0;
        range_for(*order[i], static_cast<int>(i), e, lo, hi);
        if (hi > lo && ++contributing > 1) return true;
    }
    return false;
}

bool selection_has_range(View& root) {
    auto* state = root.existing_interaction();
    if (state == nullptr) return false;
    const auto order = selectable_widgets_in_document_order(root);
    const Endpoints e = resolve(root, order);
    if (e.lo_index < 0) return false;
    for (std::size_t i = 0; i < order.size(); ++i) {
        int lo = 0, hi = 0;
        range_for(*order[i], static_cast<int>(i), e, lo, hi);
        if (hi > lo) return true;
    }
    return false;
}

std::string selection_text(View& root) {
    std::string out;
    auto* state = root.existing_interaction();
    if (state == nullptr) return out;
    const auto order = selectable_widgets_in_document_order(root);
    const Endpoints e = resolve(root, order);
    if (e.lo_index < 0) return out;

    for (int i = e.lo_index; i <= e.hi_index; ++i) {
        auto* w = order[static_cast<std::size_t>(i)];
        int lo = 0, hi = 0;
        range_for(*w, i, e, lo, hi);
        const std::string_view text = w->selectable_text();
        const auto n = static_cast<int>(text.size());
        lo = std::clamp(lo, 0, n);
        hi = std::clamp(hi, lo, n);
        if (i > e.lo_index) out.push_back('\n');
        out.append(text.substr(static_cast<std::size_t>(lo),
                               static_cast<std::size_t>(hi - lo)));
    }
    return out;
}

bool selection_copy(View& root) {
    if (!selection_has_range(root)) return false;
    return platform::Clipboard::set_text(selection_text(root));
}

}  // namespace pulp::view
