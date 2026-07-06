#include <pulp/view/reorder_list.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::view {

int reorder_target_index(int from, float offset, float pitch, int count) {
    if (pitch <= 0.0f || count <= 0) return from;
    const int delta = static_cast<int>(std::lround(offset / pitch));
    return std::clamp(from + delta, 0, count - 1);
}

void ReorderList::add_item(std::unique_ptr<View> item) {
    if (!item) return;
    View* raw = item.get();
    // The row is a drag handle: route pointer events to the container.
    raw->set_pointer_events(PointerEvents::none);
    items_.push_back(raw);
    add_child(std::move(item));
}

int ReorderList::index_of(const View* v) const {
    for (int i = 0; i < item_count(); ++i)
        if (items_[i] == v) return i;
    return -1;
}

void ReorderList::position_item(int display_index) const {
    View* v = items_[display_index];
    const float cross = orientation_ == Orientation::vertical ? bounds().width
                                                              : bounds().height;
    float main = static_cast<float>(display_index) * pitch();

    if (dragging_ >= 0) {
        if (display_index == dragging_) {
            // The lifted item follows the pointer (plus any settle residual).
            main = static_cast<float>(dragging_) * pitch() + drag_offset_;
        } else if (target_ >= 0) {
            // Neighbours between the source and the target slide one slot to open
            // the gap the lifted item will drop into.
            const int lo = std::min(dragging_, target_);
            const int hi = std::max(dragging_, target_);
            if (display_index >= lo && display_index <= hi) {
                if (target_ > dragging_) main -= pitch();  // shift up toward the gap
                else                     main += pitch();  // shift down
            }
        }
    }

    if (orientation_ == Orientation::vertical)
        v->set_bounds({0, main, cross, item_extent_});
    else
        v->set_bounds({main, 0, item_extent_, cross});
}

void ReorderList::layout_children() {
    for (int i = 0; i < item_count(); ++i) position_item(i);
}

void ReorderList::paint(canvas::Canvas& canvas) {
    // Landing glow behind the just-dropped item (cosmetic; only visible while the
    // glow animation is alive, which needs a frame clock — headless previews just
    // skip it).
    if (glow_index_ >= 0 && glow_index_ < item_count() && glow_.value() > 0.001f) {
        View* v = items_[glow_index_];
        const Rect b = v->bounds();
        auto accent = resolve_color("accent.primary", canvas::Color::rgba8(20, 184, 166));
        canvas.set_fill_color(accent.with_alpha(0.35f * glow_.value()));
        canvas.fill_rounded_rect(b.x - 2, b.y - 2, b.width + 4, b.height + 4, 6.0f);
    }
}

void ReorderList::on_mouse_down(Point pos) {
    if (item_count() == 0 || pitch() <= 0.0f) return;
    const int idx = static_cast<int>(main_of(pos) / pitch());
    if (idx < 0 || idx >= item_count()) return;
    dragging_ = idx;
    target_ = idx;
    drag_start_main_ = main_of(pos);
    drag_offset_ = 0.0f;
}

void ReorderList::on_mouse_drag(Point pos) {
    if (dragging_ < 0) return;
    drag_offset_ = main_of(pos) - drag_start_main_;
    target_ = reorder_target_index(dragging_, drag_offset_, pitch(), item_count());
    layout_children();
    request_repaint();
}

void ReorderList::on_mouse_up(Point pos) {
    if (dragging_ < 0) return;
    const int from = dragging_;
    const int to = target_;
    dragging_ = -1;
    target_ = -1;
    drag_offset_ = 0.0f;

    if (to >= 0 && to != from && from < item_count()) {
        // Permute the display order: remove from `from`, insert at `to`.
        View* moved = items_[from];
        items_.erase(items_.begin() + from);
        items_.insert(items_.begin() + to, moved);
        if (on_reorder) on_reorder(from, to);

        // Landing feedback: glow on the item at its new slot + a drop tween that
        // settles any residual visual offset. animate() no-ops (returns -1) when
        // no frame clock is reachable (previews/tests) — the order is already
        // committed above, so correctness never depends on the tween running.
        glow_index_ = to;
        glow_.set(1.0f);
        glow_.animate_to(0.0f, 0.45f, easing::ease_out_cubic);
        settle_.set(1.0f);
        settle_.animate_to(0.0f, 0.18f, easing::ease_out_cubic);
        animate([this](float v) { settle_.set(v); request_repaint(); },
                1.0f, 0.0f, 0.18f, easing::ease_out_cubic, {}, "reorder-settle");
    }
    layout_children();
    request_repaint();
    (void)pos;
}

}  // namespace pulp::view
