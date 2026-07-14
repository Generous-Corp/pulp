#include <pulp/view/context_menu.hpp>
#include <pulp/view/widget_painter.hpp>

#include <pulp/canvas/text_shaper.hpp>

#include <algorithm>
#include <vector>

namespace pulp::view {

namespace {

bool is_selectable(const ContextMenu::Item& it) {
    return !it.separator && it.enabled;
}

}  // namespace

ContextMenu::MenuLayout ContextMenu::layout() const {
    MenuLayout lay;

    // ── The sizing delegate (widget_metrics.hpp) ─────────────────────────
    // A menu's panel size is a pure function of its rows, so the delegate is
    // asked three things — the row font, each row's natural size, and the panel
    // border — and the menu does the arithmetic. Each hook may decline, in
    // which case the stock metric below is used for that hook alone.
    WidgetMetrics* metrics = effective_metrics();

    lay.font = FontSpec{"Inter", kFontSize, 400, 0.0f};
    if (metrics != nullptr)
        metrics->menu_font(lay.font, const_cast<ContextMenu&>(*this));

    lay.border = 0.0f;
    if (metrics != nullptr)
        metrics->menu_border(lay.border, const_cast<ContextMenu&>(*this));

    // Measure with the SAME shaper the painter draws with, so a measured width
    // and a drawn width cannot disagree. This runs with no canvas, which is
    // what lets a menu be sized before it is shown.
    auto& shaper = canvas::global_text_shaper();

    float widest = kMinWidth;
    float stacked = 0.0f;
    std::vector<float> heights;
    heights.reserve(items_.size());

    for (const auto& it : items_) {
        BoxSize size;
        bool sized = false;
        if (metrics != nullptr) {
            MenuItemMetricsQuery q;
            q.text = it.label;
            q.separator = it.separator;
            q.header = it.header;
            q.ticked = it.checked;
            q.has_submenu = it.has_submenu;
            q.standard_height = 0.0f;   // no caller-forced row height
            sized = metrics->menu_item_size(q, size, const_cast<ContextMenu&>(*this));
        }
        if (!sized || size.width <= 0.0f) {
            size.width = it.separator
                ? 0.0f
                : shaper.prepare(it.label, lay.font.family, lay.font.size).total_width() + kHPad;
        }
        if (!sized || size.height <= 0.0f)
            size.height = it.separator ? kSeparatorHeight : kRowHeight;

        widest = std::max(widest, size.width);
        heights.push_back(size.height);
        stacked += size.height;
    }

    const float width = widest + lay.border * 2.0f;
    const float height = stacked + lay.border * 2.0f;

    // Position: flip left / up if the box would spill past the overlay bounds.
    auto b = local_bounds();
    float x = anchor_.x;
    float y = anchor_.y;
    if (b.width > 0.0f && x + width > b.width) x = anchor_.x - width;
    if (b.height > 0.0f && y + height > b.height) y = anchor_.y - height;
    x = std::max(0.0f, x);
    y = std::max(0.0f, y);
    lay.box = {x, y, width, height};

    float row_y = y + lay.border;
    lay.rows.reserve(heights.size());
    for (float h : heights) {
        lay.rows.push_back({x + lay.border, row_y, widest, h});
        row_y += h;
    }
    return lay;
}

int ContextMenu::row_at(Point local_point, const MenuLayout& lay) const {
    const Rect& box = lay.box;
    if (local_point.x < box.x || local_point.x > box.x + box.width ||
        local_point.y < box.y || local_point.y > box.y + box.height)
        return -1;
    for (int i = 0; i < static_cast<int>(lay.rows.size()); ++i) {
        const Rect& r = lay.rows[static_cast<size_t>(i)];
        if (local_point.y >= r.y && local_point.y < r.y + r.height) return i;
    }
    return -1;
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

    const MenuLayout lay = layout();
    const Rect& box = lay.box;

    auto bg = resolve_color("bg.elevated", canvas::Color::rgba8(45, 45, 60));
    auto border_c = resolve_color("control.border", canvas::Color::rgba8(80, 80, 100));
    auto text_c = resolve_color("text.primary", canvas::Color::rgba8(220, 220, 230));
    auto text_dim = resolve_color("text.secondary", canvas::Color::rgba8(150, 150, 170));
    auto accent_c = resolve_color("accent.primary", canvas::Color::rgba8(100, 150, 255));

    canvas.save();

    // Paint delegate (widget_painter.hpp): the panel surface and each row are
    // separate hooks, so a skin can restyle rows and keep the stock panel, or
    // the reverse. Each declines by default.
    WidgetPainter* delegate = effective_painter();

    bool panel_skinned = false;
    if (delegate != nullptr) {
        MenuBackgroundPaintState ps;
        ps.bounds = box;
        ps.enabled = enabled();
        panel_skinned = delegate->paint_menu_background(canvas, ps, *this);
    }
    if (!panel_skinned) {
        // Menu box background + 1px rounded border.
        canvas.set_fill_color(bg);
        canvas.fill_rounded_rect(box.x, box.y, box.width, box.height, kRadius);
        canvas.set_stroke_color(border_c);
        canvas.set_line_width(1);
        canvas.stroke_rounded_rect(box.x, box.y, box.width, box.height, kRadius);
    }

    canvas.set_font(lay.font.family, lay.font.size);
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const auto& it = items_[static_cast<size_t>(i)];
        const Rect row = lay.rows[static_cast<size_t>(i)];
        const float iy = row.y;
        const float rh = row.height;

        if (delegate != nullptr) {
            if (it.header) {
                MenuHeaderPaintState hs;
                hs.bounds = row;
                hs.text = it.label;
                if (delegate->paint_menu_section_header(canvas, hs, *this)) continue;
            } else {
                MenuItemPaintState is;
                is.bounds = row;
                is.enabled = it.enabled;
                is.text = it.label;
                is.separator = it.separator;
                is.highlighted = (i == hover_index_ && it.enabled);
                is.hovered = is.highlighted;
                is.ticked = it.checked;
                is.has_submenu = it.has_submenu;
                if (delegate->paint_menu_item(canvas, is, *this)) continue;
            }
        }

        if (it.separator) {
            canvas.set_stroke_color(border_c);
            canvas.set_line_width(0.5f);
            canvas.stroke_line(row.x + 4, iy + rh * 0.5f,
                               row.x + row.width - 4, iy + rh * 0.5f);
            continue;
        }

        // Hover highlight only on enabled rows.
        if (i == hover_index_ && it.enabled) {
            canvas.set_fill_color(accent_c);
            canvas.fill_rect(row.x + 1, iy, row.width - 2, rh);
        }

        // Checkmark glyph for checked items.
        if (it.checked) {
            auto check_color = (i == hover_index_ && it.enabled)
                                   ? canvas::Color::rgba8(255, 255, 255)
                                   : accent_c;
            canvas.set_fill_color(check_color);
            canvas.fill_text("\xe2\x9c\x93", row.x + 6, iy + rh * 0.5f + 4.0f);
        }

        // Dim disabled rows; white text on the hovered row.
        canvas::Color row_text = text_c;
        if (!it.enabled) row_text = text_dim;
        else if (i == hover_index_) row_text = canvas::Color::rgba8(255, 255, 255);
        canvas.set_fill_color(row_text);
        canvas.set_text_align(canvas::TextAlign::left);
        canvas.fill_text(it.label, row.x + 22, iy + rh * 0.5f + 4.0f);
    }

    canvas.restore();
}

void ContextMenu::on_mouse_event(const MouseEvent& event) {
    if (closed_ || items_.empty()) return;
    if (event.is_wheel) return;

    const MenuLayout lay = layout();  // canvas-free geometry
    const int row = row_at(event.position, lay);

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
