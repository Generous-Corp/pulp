#include <pulp/view/context_menu.hpp>

#include <algorithm>

namespace pulp::view {

namespace {

bool is_selectable(const ContextMenu::Item& it) {
    return !it.separator && it.enabled;
}

}  // namespace

float ContextMenu::measured_width() const {
    return cached_width_;
}

Rect ContextMenu::menu_box(const canvas::Canvas* canvas) const {
    // Width: max measured label width + padding, clamped to a minimum. When a
    // canvas is available (during paint) measure with the real font; otherwise
    // fall back to the cached width so headless hit-testing is stable.
    float width = kMinWidth;
    if (canvas) {
        // const_cast: measure_text/set_font are non-const on the canvas, but
        // measuring text is logically a read for our geometry. We never mutate
        // pixels here.
        auto* c = const_cast<canvas::Canvas*>(canvas);
        c->set_font("Inter", 12);
        for (const auto& it : items_) {
            if (it.separator) continue;
            width = std::max(width, c->measure_text(it.label) + kHPad);
        }
        cached_width_ = width;
    } else {
        width = cached_width_;
    }

    float height = static_cast<float>(items_.size()) * kRowHeight;

    auto b = local_bounds();
    float x = anchor_.x;
    float y = anchor_.y;
    // Flip left / up if the box would spill past the overlay bounds.
    if (b.width > 0.0f && x + width > b.width) x = anchor_.x - width;
    if (b.height > 0.0f && y + height > b.height) y = anchor_.y - height;
    x = std::max(0.0f, x);
    y = std::max(0.0f, y);
    return {x, y, width, height};
}

int ContextMenu::row_at(Point local_point, const Rect& box) const {
    if (local_point.x < box.x || local_point.x > box.x + box.width ||
        local_point.y < box.y || local_point.y > box.y + box.height)
        return -1;
    int idx = static_cast<int>((local_point.y - box.y) / kRowHeight);
    if (idx < 0 || idx >= static_cast<int>(items_.size())) return -1;
    return idx;
}

void ContextMenu::move_hover(int delta) {
    if (items_.empty()) return;
    const int n = static_cast<int>(items_.size());
    int idx = hover_index_;
    for (int step = 0; step < n; ++step) {
        int next = idx + delta;
        if (next < 0 || next >= n) {
            // No selectable row found yet but we ran off the end: if we have no
            // current hover, seed from the appropriate end on the next pass.
            if (idx < 0) {
                next = (delta > 0) ? 0 : n - 1;
            } else {
                break;  // stay put at the edge
            }
        }
        idx = next;
        if (is_selectable(items_[static_cast<size_t>(idx)])) {
            hover_index_ = idx;
            request_repaint();
            return;
        }
    }
}

void ContextMenu::fire_close(std::optional<int> result) {
    if (closed_) return;
    closed_ = true;
    if (on_close) on_close(result);  // may delete `this` (removes from root)
}

void ContextMenu::paint(canvas::Canvas& canvas) {
    if (items_.empty()) return;

    const Rect box = menu_box(&canvas);

    auto bg = resolve_color("bg.elevated", canvas::Color::rgba8(45, 45, 60));
    auto border_c = resolve_color("control.border", canvas::Color::rgba8(80, 80, 100));
    auto text_c = resolve_color("text.primary", canvas::Color::rgba8(220, 220, 230));
    auto text_dim = resolve_color("text.secondary", canvas::Color::rgba8(150, 150, 170));
    auto accent_c = resolve_color("accent.primary", canvas::Color::rgba8(100, 150, 255));

    canvas.save();

    // Menu box background + 1px rounded border.
    canvas.set_fill_color(bg);
    canvas.fill_rounded_rect(box.x, box.y, box.width, box.height, kRadius);
    canvas.set_stroke_color(border_c);
    canvas.set_line_width(1);
    canvas.stroke_rounded_rect(box.x, box.y, box.width, box.height, kRadius);

    canvas.set_font("Inter", 12);
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const auto& it = items_[static_cast<size_t>(i)];
        float iy = box.y + static_cast<float>(i) * kRowHeight;

        if (it.separator) {
            canvas.set_stroke_color(border_c);
            canvas.set_line_width(0.5f);
            canvas.stroke_line(box.x + 4, iy + kRowHeight * 0.5f,
                               box.x + box.width - 4, iy + kRowHeight * 0.5f);
            continue;
        }

        // Hover highlight only on enabled rows.
        if (i == hover_index_ && it.enabled) {
            canvas.set_fill_color(accent_c);
            canvas.fill_rect(box.x + 1, iy, box.width - 2, kRowHeight);
        }

        // Checkmark glyph for checked items.
        if (it.checked) {
            auto check_color = (i == hover_index_ && it.enabled)
                                   ? canvas::Color::rgba8(255, 255, 255)
                                   : accent_c;
            canvas.set_fill_color(check_color);
            canvas.fill_text("\xe2\x9c\x93", box.x + 6, iy + 16);
        }

        // Dim disabled rows; white text on the hovered row.
        canvas::Color row_text = text_c;
        if (!it.enabled) row_text = text_dim;
        else if (i == hover_index_) row_text = canvas::Color::rgba8(255, 255, 255);
        canvas.set_fill_color(row_text);
        canvas.set_text_align(canvas::TextAlign::left);
        canvas.fill_text(it.label, box.x + 22, iy + 16);
    }

    canvas.restore();
}

void ContextMenu::on_mouse_event(const MouseEvent& event) {
    if (closed_ || items_.empty()) return;
    if (event.is_wheel) return;

    const Rect box = menu_box();  // headless geometry (cached width)
    const int row = row_at(event.position, box);

    if (!event.is_down) {
        // Hover: only over enabled, non-separator rows.
        if (row >= 0 && is_selectable(items_[static_cast<size_t>(row)]))
            hover_index_ = row;
        else
            hover_index_ = -1;
        request_repaint();
        return;
    }

    // Mouse-down.
    if (row < 0) {
        // Click outside the menu box → dismiss.
        fire_close(std::nullopt);
        return;
    }
    const auto& it = items_[static_cast<size_t>(row)];
    if (is_selectable(it)) {
        fire_close(it.id);  // commit selection
    }
    // Click on a disabled row or separator: ignore (menu stays open).
}

bool ContextMenu::on_key_event(const KeyEvent& event) {
    if (!event.is_down || closed_) return false;
    switch (event.key) {
        case KeyCode::up:   move_hover(-1); return true;
        case KeyCode::down: move_hover(+1); return true;
        case KeyCode::escape:
            fire_close(std::nullopt);
            return true;
        case KeyCode::enter:
        case KeyCode::space:
            if (hover_index_ >= 0 &&
                hover_index_ < static_cast<int>(items_.size()) &&
                is_selectable(items_[static_cast<size_t>(hover_index_)]))
                fire_close(items_[static_cast<size_t>(hover_index_)].id);
            return true;
        default:
            return false;
    }
}

View* ContextMenu::hit_test(Point local_point) {
    // The whole overlay is hit-testable so clicks anywhere (inside or outside
    // the menu box) reach us — outside clicks must dismiss.
    if (!visible() || !enabled() || !hit_testable()) return nullptr;
    auto b = local_bounds();
    if (local_point.x >= 0.0f && local_point.x <= b.width &&
        local_point.y >= 0.0f && local_point.y <= b.height)
        return this;
    return nullptr;
}

ContextMenu* ContextMenu::show(View* root, Point pos, std::vector<Item> items,
                               std::function<void(std::optional<int>)> on_close) {
    if (!root) return nullptr;

    auto menu = std::make_unique<ContextMenu>();
    ContextMenu* raw = menu.get();
    raw->set_items(std::move(items));
    raw->set_anchor(pos);
    raw->set_bounds(root->local_bounds());
    raw->set_focusable(true);

    // Wrap the caller's callback so it ALSO removes the menu from root. The
    // user callback fires first (so it can read state), then we detach. Capture
    // `root` and `raw` by value; `raw` stays valid until remove_child runs.
    raw->on_close = [root, raw, cb = std::move(on_close)](std::optional<int> result) {
        if (cb) cb(result);
        // Detach from the tree; this destroys the ContextMenu (and its closure,
        // so nothing else may touch `raw` after this line).
        root->remove_child(raw);
    };

    root->add_child(std::move(menu));  // last child → painted/hit-tested on top
    raw->set_focus(true);
    raw->claim_input_focus();
    return raw;
}

}  // namespace pulp::view
