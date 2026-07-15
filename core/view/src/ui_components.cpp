#include <pulp/view/ui_components.hpp>
#include <pulp/view/widget_painter.hpp>
#include <pulp/view/animation.hpp>
#include <algorithm>

namespace pulp::view {

const std::string ComboBox::empty_string_;

namespace {
// Rows whose text starts with "---" are non-selectable visual separators.
bool is_separator_item(const std::string& item) {
    return item.size() >= 3 && item.substr(0, 3) == "---";
}
}  // namespace

float fit_combo_label(std::string& text, float avail, float base, float min,
                      const std::function<float(const std::string&, float)>& width_at) {
    if (avail <= 0.0f) return base;
    float font = base;
    // Shrink the font (in 0.5px steps) until the FULL text fits or we hit `min`.
    while (font > min && width_at(text, font) > avail) {
        font = std::max(min, font - 0.5f);
    }
    // Still overflowing at the floor → ellipsize, trimming until "text..." fits.
    if (width_at(text, font) > avail && text.size() > 1) {
        while (text.size() > 1) {
            text.pop_back();
            if (width_at(text + "...", font) <= avail) { text += "..."; break; }
        }
    }
    return font;
}

// ── ComboBox ─────────────────────────────────────────────────────────────

void ComboBox::set_selected_impl(int index, Notify notify) {
    if (index >= 0 && index < static_cast<int>(items_.size()) && index != selected_) {
        selected_ = index;
        set_access_value(items_[static_cast<size_t>(index)]);
        if (notify == Notify::sync) {
            if (on_change) on_change(index);
        } else if (notify == Notify::async && on_change) {
            auto fn = on_change;
            const int i = index;
            queue_async_notification([fn, i] { fn(i); });
        }
    }
}

void ComboBox::set_selected(int index) {
    set_selected_impl(index, Notify::sync);
}

void ComboBox::set_selected(int index, Notify notify) {
    set_selected_impl(index, notify);
}

void ComboBox::set_selected_silent(int index) {
    set_selected_impl(index, Notify::none);
}

const std::string& ComboBox::selected_text() const {
    if (selected_ >= 0 && selected_ < static_cast<int>(items_.size()))
        return items_[static_cast<size_t>(selected_)];
    return empty_string_;
}

float ComboBox::dropdown_width_hint() const {
    float width = bounds().width;
    for (const auto& item : items_) {
        if (item.size() >= 3 && item.substr(0, 3) == "---") {
            continue;
        }
        width = std::max(width, static_cast<float>(item.size()) * 7.0f + 34.0f);
    }
    return width;
}

void ComboBox::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();

    // Paint delegate first refusal (widget_painter.hpp). Covers the CLOSED field
    // only — the open dropdown is a menu surface and goes through the menu hooks.
    bool skinned = false;
    if (auto* p = effective_painter()) {
        ComboBoxPaintState ps;
        ps.bounds = b;
        ps.enabled = enabled();
        ps.hovered = is_hovered();
        ps.pressed = open_;
        ps.focused = has_focus();
        ps.popup_open = open_;
        ps.text = selected_text();
        const float bh = std::min(b.height, 28.0f);
        ps.arrow_zone = Rect{b.width - 24.0f, 0.0f, 24.0f, bh};
        skinned = p->paint_combo_box(canvas, ps, *this);
    }
    auto bg = resolve_color("bg.surface", canvas::Color::rgba8(30, 30, 46));
    auto border_c = resolve_color("control.border", canvas::Color::rgba8(80, 80, 100));
    auto text_c = resolve_color("text.primary", canvas::Color::rgba8(220, 220, 230));

    float base_h = std::min(b.height, 28.0f);

    // Background
    if (!skinned) {
        canvas.set_fill_color(bg);
        canvas.fill_rounded_rect(0, 0, b.width, base_h, 4);
        canvas.set_stroke_color(border_c);
        canvas.set_line_width(1);
        canvas.stroke_rounded_rect(0, 0, b.width, base_h, 4);
    }

    // Selected text. The box width often comes straight from a design (a Figma
    // dropdown sized tight to its own label), so prefer SHRINKING the font to fit
    // the full text over truncating it — a short label like "1/4 Delay" should
    // read in full, just a touch smaller, not "1/4 De…". fit_combo_label measures
    // with the font actually used (the old inline code measured in the default
    // font, so truncation was wrong at any window size) and only ellipsizes as a
    // last resort once the font hits its floor.
    std::string display_text = selected_text();
    // Reserve the full chevron zone, not just an approximation: the arrow is drawn
    // at ax = width-16 with arms spanning [ax-3, ax+3] = [width-19, width-13], so its
    // left arm starts at width-19. Text begins at x=8; cap its right edge at width-22
    // (a 3px gap before the arrow) → avail = (width-22) - 8 = width-30. The old
    // width-22 reservation let a box-filling label (e.g. "Forward", "Ping-Pong")
    // run to width-14 and overlap the chevron.
    const float avail = std::max(0.0f, b.width - 30.0f);  // 8px left pad + arrow + gap
    const float font_size = fit_combo_label(
        display_text, avail, /*base=*/12.0f, /*min=*/9.0f,
        [&canvas](const std::string& s, float f) {
            canvas.set_font("Inter", f);
            return canvas.measure_text(s);
        });
    if (!skinned) {
        canvas.set_font("Inter", font_size);
        canvas.set_fill_color(text_c);
        canvas.set_text_align(canvas::TextAlign::left);
        canvas.fill_text(display_text, 8, base_h * 0.5f + font_size * 0.34f);
    }

    // Dropdown chevron — vertically centered on the field so it lines up with
    // the field text's optical center (which sits at base_h*0.5). The V's
    // bounding box is symmetric about `ay`; lift it 1px to compensate for the
    // downward point's optical weight, which otherwise reads as sitting low.
    float ax = b.width - 16;
    float ay = base_h * 0.5f - 1.0f;
    auto arrow_c = resolve_color("text.secondary", canvas::Color::rgba8(150, 150, 170));
    if (!skinned) {
        canvas.set_stroke_color(arrow_c);
        canvas.set_line_width(1.5f);
        canvas.stroke_line(ax - 3, ay - 2, ax, ay + 2);
        canvas.stroke_line(ax, ay + 2, ax + 3, ay - 2);
    }

    // Dropdown menu: deferred to overlay queue so it paints on top of everything
    if (open_ && !items_.empty()) {
        // On-screen position, peeling off ScrollView scroll (the overlay paints
        // at the root with no scroll transform).
        float abs_x = 0, abs_y = 0, viewport_h = 0, viewport_w = 0;
        overlay_anchor_(abs_x, abs_y, viewport_h, viewport_w);

        float item_h = 24.0f;
        // Flip/clamp/scroll-aware geometry — shared with hit_test, hover, mouse
        // and the platform host's click routing so they always agree.
        float top_local = 0.0f, dd_h = 0.0f;
        int first = 0, visible = 0;
        dropdown_metrics(top_local, dd_h, first, visible);
        float dd_top = abs_y + top_local;

        // Single width source shared with dropdown_window_rect() (mac click
        // routing) and hit_test(), so what is painted is exactly what is
        // clickable — no phantom/dead click zone on the menu's right side.
        float dd_w = dropdown_width_hint();
        if (viewport_w > 0.0f) {  // keep a little breathing room from the right edge
            constexpr float pad = 14.0f;
            if (abs_x + dd_w > viewport_w - pad)
                dd_w = std::max(b.width, viewport_w - pad - abs_x);
        }

        const int n = static_cast<int>(items_.size());
        const bool more_above = first > 0;
        const bool more_below = first + visible < n;
        int sel = selected_;
        int* hover_ptr = &hover_index_;  // live pointer for dynamic hover tracking
        auto items_copy = items_;
        auto dropdown_bg = resolve_color("bg.elevated", canvas::Color::rgba8(45, 45, 60));
        auto accent_c = resolve_color("accent.primary", canvas::Color::rgba8(100, 150, 255));
        auto border = border_c;
        auto text = text_c;

        overlay_queue().push_back({[=](canvas::Canvas& c) {
            c.save();
            c.set_fill_color(dropdown_bg);
            c.fill_rounded_rect(abs_x, dd_top, dd_w, dd_h, 4);
            c.set_stroke_color(border);
            c.set_line_width(1);
            c.stroke_rounded_rect(abs_x, dd_top, dd_w, dd_h, 4);

            c.set_font("Inter", 12);
            // Only the visible window [first, first+visible) is drawn; row r maps
            // to item index (first + r).
            for (int r = 0; r < visible; ++r) {
                const int i = first + r;
                if (i < 0 || i >= static_cast<int>(items_copy.size())) continue;
                float iy = dd_top + static_cast<float>(r) * item_h;
                const auto& item = items_copy[static_cast<size_t>(i)];

                if (item.size() >= 3 && item.substr(0, 3) == "---") {
                    c.set_stroke_color(border);
                    c.set_line_width(0.5f);
                    c.stroke_line(abs_x + 4, iy + item_h * 0.5f,
                                  abs_x + dd_w - 4, iy + item_h * 0.5f);
                    continue;
                }

                int hov = *hover_ptr;
                // The SELECTED row is marked only by its checkmark; the row
                // background highlight is reserved for HOVER.
                if (i == hov) {
                    c.set_fill_color(accent_c);
                    c.fill_rect(abs_x + 1, iy, dd_w - 2, item_h);
                }
                if (i == sel) {
                    auto check_color = (i == hov) ? canvas::Color::rgba8(255, 255, 255)
                                                  : accent_c;
                    c.set_fill_color(check_color);
                    c.fill_text("\xe2\x9c\x93", abs_x + 6, iy + 16);
                }

                c.set_fill_color(text);
                c.set_text_align(canvas::TextAlign::left);
                c.fill_text(item, abs_x + 22, iy + 16);
            }

            // Scroll affordance carets when the list overflows the visible window.
            c.set_stroke_color(text);
            c.set_line_width(1.2f);
            if (more_above) {
                float cx = abs_x + dd_w - 12.0f, cy = dd_top + 6.0f;
                c.stroke_line(cx - 3, cy + 2, cx, cy - 2);
                c.stroke_line(cx, cy - 2, cx + 3, cy + 2);
            }
            if (more_below) {
                float cx = abs_x + dd_w - 12.0f, cy = dd_top + dd_h - 6.0f;
                c.stroke_line(cx - 3, cy - 2, cx, cy + 2);
                c.stroke_line(cx, cy + 2, cx + 3, cy - 2);
            }
            c.restore();
        }, this});
    }
}

ComboBox* ComboBox::active_popup_ = nullptr;

void ComboBox::close_active_popup() {
    if (active_popup_) {
        active_popup_->close_dropdown();
    }
}

void ComboBox::notify_global_click(View* target) {
    if (!active_popup_) return;
    // Check if click target is the active popup or a child of it
    View* v = target;
    while (v) {
        if (v == active_popup_) return;  // click is inside popup
        v = v->parent();
    }
    close_active_popup();
}

void ComboBox::open_dropdown() {
    if (open_) return;
    close_active_popup();  // close any other open dropdown first
    set_overflow(Overflow::visible);
    open_ = true;
    hover_index_ = selected_;  // highlight the current selection on open so
                               // keyboard navigation has a visible starting row
    active_popup_ = this;
    // Scroll only enough to reveal the current selection, and only when it sits
    // BELOW the visible window — so the top items stay visible whenever the
    // selection is already in the first page (a menu that fully fits never
    // scrolls, and opening on a late item never hides earlier ones needlessly).
    dropdown_scroll_ = 0;
    float top = 0.0f, h = 0.0f;
    int first = 0, visible = 0;
    dropdown_metrics(top, h, first, visible);
    const int n = static_cast<int>(items_.size());
    if (visible > 0 && visible < n && selected_ >= visible)
        dropdown_scroll_ = std::clamp(selected_ - visible + 1, 0, n - visible);
}

void ComboBox::close_dropdown() {
    if (!open_) return;
    open_ = false;
    set_overflow(Overflow::hidden);
    if (active_popup_ == this) active_popup_ = nullptr;
}

void ComboBox::overlay_anchor_(float& out_x, float& out_y, float& out_viewport_h,
                              float& out_viewport_w) const {
    float x = 0.0f, y = 0.0f, viewport_h = 0.0f, viewport_w = 0.0f;
    const View* v = this;
    while (v) {
        // A ScrollView paints its children shifted by -scroll; `this` lives in
        // that scrolled content, so peel the offset off to get the on-screen
        // position. Track the nearest scroll viewport's size for flip/clamp logic.
        if (auto* sv = dynamic_cast<const ScrollView*>(v)) {
            x -= sv->scroll_x();
            y -= sv->scroll_y();
            if (viewport_h <= 0.0f) viewport_h = sv->bounds().height;
            if (viewport_w <= 0.0f) viewport_w = sv->bounds().width;
        }
        x += v->bounds().x;
        y += v->bounds().y;
        if (!v->parent() && v != this) {
            // Root ANCESTOR: its size is the window viewport when no ScrollView
            // was seen. Guard `v != this` so a parentless/standalone ComboBox does
            // NOT treat its own field height as the viewport — that would clamp
            // the menu to ~0 rows. Leaving the viewport at 0 makes dropdown_metrics
            // fall back to the unbounded (show-all-items-below) path.
            if (viewport_h <= 0.0f) viewport_h = v->bounds().height;
            if (viewport_w <= 0.0f) viewport_w = v->bounds().width;
        }
        v = v->parent();
    }
    out_x = x;
    out_y = y;
    out_viewport_h = viewport_h;
    out_viewport_w = viewport_w;
}

void ComboBox::dropdown_metrics(float& top_local, float& height,
                                int& first_row, int& visible_rows) const {
    constexpr float item_h = 24.0f;
    const float base_h = std::min(local_bounds().height, 28.0f);
    const int n = static_cast<int>(items_.size());
    const float content_h = static_cast<float>(n) * item_h;

    float abs_x = 0.0f, abs_y = 0.0f, viewport_h = 0.0f, viewport_w = 0.0f;
    overlay_anchor_(abs_x, abs_y, viewport_h, viewport_w);
    constexpr float pad = 4.0f;

    // Room within the visible viewport below and above the field.
    float avail_below = (viewport_h > 0.0f) ? (viewport_h - (abs_y + base_h + 2.0f) - pad)
                                            : content_h;
    float avail_above = (viewport_h > 0.0f) ? (abs_y - 2.0f - pad) : content_h;
    if (avail_below < 0.0f) avail_below = 0.0f;
    if (avail_above < 0.0f) avail_above = 0.0f;

    // Prefer below if the whole list fits; else flip up if it fits; else take the
    // side with more room and scroll the overflow.
    bool below;
    float avail;
    if (viewport_h <= 0.0f || content_h <= avail_below) { below = true;  avail = content_h; }
    else if (content_h <= avail_above)                  { below = false; avail = avail_above; }
    else { below = (avail_below >= avail_above); avail = below ? avail_below : avail_above; }

    int max_rows = (viewport_h > 0.0f) ? static_cast<int>(avail / item_h) : n;
    if (max_rows < 1) max_rows = 1;
    visible_rows = std::min(n < 1 ? 0 : n, max_rows);
    height = static_cast<float>(visible_rows) * item_h;

    first_row = std::clamp(dropdown_scroll_, 0, std::max(0, n - visible_rows));
    top_local = below ? (base_h + 2.0f) : -(height + 2.0f);
}

float ComboBox::dropdown_local_top() const {
    float top = 0.0f, height = 0.0f;
    int first = 0, visible = 0;
    dropdown_metrics(top, height, first, visible);
    return top;
}

bool ComboBox::dropdown_window_rect(float& x, float& y, float& w, float& h) const {
    if (!open_ || items_.empty()) return false;
    float abs_x = 0.0f, abs_y = 0.0f, viewport_h = 0.0f, viewport_w = 0.0f;
    overlay_anchor_(abs_x, abs_y, viewport_h, viewport_w);
    float top_local = 0.0f, height = 0.0f;
    int first = 0, visible = 0;
    dropdown_metrics(top_local, height, first, visible);
    float dd_w = dropdown_width_hint();
    if (viewport_w > 0.0f) {  // keep a little breathing room from the right edge
        constexpr float pad = 14.0f;
        if (abs_x + dd_w > viewport_w - pad)
            dd_w = std::max(bounds().width, viewport_w - pad - abs_x);
    }
    x = abs_x;
    y = abs_y + top_local;
    w = dd_w;
    h = height;
    return true;
}

View* ComboBox::hit_test(Point local_point) {
    // When open, the menu overlay lives outside this view's own bounds (below, or above when
    // flipped). Claim hits over it so hover/click reach us regardless of flip direction.
    if (open_ && !items_.empty()) {
        float top = 0.0f, dd_h = 0.0f;
        int first = 0, visible = 0;
        dropdown_metrics(top, dd_h, first, visible);
        // Claim the full menu width (same source as paint + routing), not just
        // the header width, so hover and non-mac hit-testing cover the whole menu.
        if (local_point.x >= 0.0f && local_point.x <= dropdown_width_hint() &&
            local_point.y >= std::min(top, 0.0f) &&
            local_point.y <= std::max(top + dd_h, local_bounds().height))
            return this;
    }
    return View::hit_test(local_point);
}

void ComboBox::move_hover(int delta) {
    if (items_.empty()) return;
    const int n = static_cast<int>(items_.size());
    int idx = (hover_index_ < 0) ? selected_ : hover_index_;
    for (int step = 0; step < n; ++step) {
        int next = std::clamp(idx + delta, 0, n - 1);
        if (next == idx) break;  // hit an end
        idx = next;
        const auto& it = items_[static_cast<size_t>(idx)];
        if (it.size() < 3 || it.substr(0, 3) != "---") break;  // landed on a real item
    }
    hover_index_ = idx;
    // Keep the highlighted row inside the (possibly clamped) visible window so
    // keyboard navigation never highlights — or commits — a row scrolled out of
    // view.
    float top = 0.0f, h = 0.0f;
    int first = 0, visible = 0;
    dropdown_metrics(top, h, first, visible);
    if (visible > 0 && visible < n) {
        if (idx < dropdown_scroll_)
            dropdown_scroll_ = idx;
        else if (idx >= dropdown_scroll_ + visible)
            dropdown_scroll_ = idx - visible + 1;
        dropdown_scroll_ = std::clamp(dropdown_scroll_, 0, n - visible);
    }
    request_repaint();
}

void ComboBox::on_hover_move(Point local_pos) {
    // The platform host dispatches hover samples through on_hover_move (NOT
    // on_mouse_event with is_down=false), so the open dropdown's row highlight
    // is updated here. hit_test() already claims the dropdown region, so this
    // fires for hovers over the menu even though it's outside our own bounds.
    if (!open_ || items_.empty()) return;
    float top = 0.0f, dd_h = 0.0f;
    int first = 0, visible = 0;
    dropdown_metrics(top, dd_h, first, visible);
    int next = -1;
    if (local_pos.y >= top && local_pos.y < top + dd_h) {
        const int row = static_cast<int>((local_pos.y - top) / 24.0f);
        const int i = first + row;
        if (row >= 0 && row < visible && i >= 0 && i < static_cast<int>(items_.size())) next = i;
    }
    if (next != hover_index_) { hover_index_ = next; request_repaint(); }
}

void ComboBox::on_mouse_event(const MouseEvent& event) {
    // Menu geometry in local coords — shared with paint/hit_test so hover/click
    // line up with what's drawn, including the flipped-up and scrolled cases.
    float top = 0.0f, dd_h = 0.0f;
    int first = 0, visible = 0;
    dropdown_metrics(top, dd_h, first, visible);
    const int n = static_cast<int>(items_.size());
    const bool in_menu = event.position.y >= top && event.position.y < top + dd_h;
    auto row_to_index = [&](float y) -> int {
        const int row = static_cast<int>((y - top) / 24.0f);
        const int i = first + row;
        return (row >= 0 && row < visible && i >= 0 && i < n) ? i : -1;
    };

    // Wheel scrolls the item list when it's taller than the visible window.
    if (open_ && event.is_wheel) {
        if (visible < n) {
            const int step = (event.scroll_delta_y > 0.0f) ? 1
                           : (event.scroll_delta_y < 0.0f ? -1 : 0);
            dropdown_scroll_ = std::clamp(dropdown_scroll_ + step, 0, n - visible);
            request_repaint();
        }
        return;
    }

    // Track hover on mouse move (even without button down).
    if (open_ && !event.is_down) {
        hover_index_ = in_menu ? row_to_index(event.position.y) : -1;
        request_repaint();
        return;
    }

    if (!event.is_down) return;

    if (open_) {
        if (in_menu) {
            const int index = row_to_index(event.position.y);
            if (index >= 0) {
                const auto& item = items_[static_cast<size_t>(index)];
                if (item.size() < 3 || item.substr(0, 3) != "---") set_selected(index);
            }
        }
        close_dropdown();  // a click on a row commits; a click on the header/outside cancels
    } else {
        open_dropdown();
    }
    request_repaint();
}


bool ComboBox::on_key_event(const KeyEvent& event) {
    if (!event.is_down) return false;

    if (!open_) {
        // Closed: Up/Down step the value like a stepper; Enter/Space opens the menu.
        // Step OVER "---" separators so the stepper can never commit a separator
        // index (which would fire on_change with an invalid/empty value).
        auto step_to_selectable = [&](int delta) -> bool {
            const int n = static_cast<int>(items_.size());
            int idx = selected_;
            for (int s = 0; s < n; ++s) {
                const int next = idx + delta;
                if (next < 0 || next >= n) return false;  // ran off the end
                idx = next;
                if (!is_separator_item(items_[static_cast<size_t>(idx)])) {
                    set_selected(idx);
                    return true;
                }
            }
            return false;
        };
        if (event.key == KeyCode::up) return step_to_selectable(-1);
        if (event.key == KeyCode::down) return step_to_selectable(+1);
        if (event.key == KeyCode::enter || event.key == KeyCode::space) {
            open_dropdown();
            request_repaint();
            return true;
        }
        return false;
    }

    // Open: arrows move the highlight (like hover), Enter/Space commits, Esc cancels.
    switch (event.key) {
        case KeyCode::up:   move_hover(-1); return true;
        case KeyCode::down: move_hover(+1); return true;
        case KeyCode::escape:
            close_dropdown();  // cancel — selection unchanged
            request_repaint();
            return true;
        case KeyCode::enter:
        case KeyCode::space:
            if (hover_index_ >= 0 && hover_index_ < static_cast<int>(items_.size()) &&
                !is_separator_item(items_[static_cast<size_t>(hover_index_)]))
                set_selected(hover_index_);  // never commit a "---" separator row
            close_dropdown();
            request_repaint();
            return true;
        default:
            return false;
    }
}

void ComboBox::on_text_input(const TextInputEvent& event) {
    if (event.text.empty()) return;
    char ch = std::tolower(static_cast<unsigned char>(event.text[0]));
    // Search from current selection + 1, wrapping around
    int start = selected_ + 1;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        int idx = (start + i) % static_cast<int>(items_.size());
        const auto& item = items_[static_cast<size_t>(idx)];
        if (!item.empty() && std::tolower(static_cast<unsigned char>(item[0])) == ch) {
            set_selected(idx);
            if (!open_) open_dropdown();
            return;
        }
    }
}

// ── Tooltip ──────────────────────────────────────────────────────────────

void Tooltip::show_at(Point position) {
    set_bounds({position.x, position.y - 24, 200, 22});
    set_visible(true);
    float dur = resolve_dimension("motion.duration.normal", 0.15f);
    opacity_.animate_to(1.0f, dur, easing::ease_out_quad);
}

void Tooltip::hide() {
    float dur = resolve_dimension("motion.duration.normal", 0.15f);
    opacity_.animate_to(0.0f, dur, easing::ease_in_quad);
    // Note: caller should check opacity_.value() <= 0 to actually set_visible(false)
}

void Tooltip::advance_animations(float dt) {
    opacity_.advance(dt);
    if (!opacity_.animating() && opacity_.value() <= 0.01f && visible()) {
        set_visible(false);
    }
}

void Tooltip::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();
    auto alpha = static_cast<uint8_t>(255 * opacity_.value());
    canvas.set_fill_color(canvas::Color::rgba8(0x33, 0x33, 0x44, alpha));
    canvas.fill_rounded_rect(b.x, b.y, b.width, b.height, 4);
    canvas.set_font("system", 11);
    canvas.set_fill_color(canvas::Color::rgba8(0xe0, 0xe0, 0xe0, alpha));
    // Center the label on the bubble's vertical midline (GlyphCenter anchors
    // on the glyph's optical center, not the baseline) with an 8px left pad,
    // instead of a fixed baseline that left the text sitting low.
    canvas.set_text_align(canvas::TextAlign::left);
    canvas.fill_text_anchored(text_, b.x + 8, b.y + b.height * 0.5f,
                              canvas::Canvas::TextAnchor::GlyphCenter);
}

// ── ThemeModeControl ──────────────────────────────────────────────────────
using canvas::Color;  // local convenience for the control's token fallbacks

int ThemeModeControl::segment_at_(Point pos) const {
    auto b = local_bounds();
    if (pos.y < 0 || pos.y > b.height || pos.x < 0 || pos.x > b.width) return -1;
    float seg_w = b.width / 3.0f;
    int s = static_cast<int>(pos.x / seg_w);
    return std::clamp(s, 0, 2);
}

void ThemeModeControl::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();
    float seg_w = b.width / 3.0f, h = b.height;
    float r = std::min(8.0f, h * 0.3f);

    // Track background.
    canvas.set_fill_color(resolve_color("bg.surface", Color::rgba8(30, 30, 46)));
    canvas.fill_rounded_rect(0, 0, b.width, h, r);
    canvas.set_stroke_color(resolve_color("control.border", Color::rgba8(80, 80, 100)));
    canvas.set_line_width(1.0f);
    canvas.stroke_rounded_rect(0, 0, b.width, h, r);

    int active = mode_ == ThemeMode::system ? 0 : (mode_ == ThemeMode::light ? 1 : 2);
    auto accent = resolve_color("accent.primary", Color::rgba8(22, 218, 194));
    auto fg = resolve_color("text.primary", Color::rgba8(220, 220, 230));
    auto dim = resolve_color("text.secondary", Color::rgba8(140, 145, 155));

    for (int s = 0; s < 3; ++s) {
        float sx = s * seg_w, scx = sx + seg_w * 0.5f, cy = h * 0.5f;
        float ic = std::min(seg_w, h) * 0.30f;  // icon radius

        // Active pill highlight.
        if (s == active) {
            canvas.set_fill_color(Color::rgba(accent.r, accent.g, accent.b, 0.18f));  // token-lint:allow (accent tint)
            canvas.fill_rounded_rect(sx + 2, 2, seg_w - 4, h - 4, r - 1);
        }
        auto col = (s == active) ? accent : (s == hover_seg_ ? fg : dim);
        canvas.set_stroke_color(col);
        canvas.set_fill_color(col);
        canvas.set_line_width(1.6f);

        if (s == 0) {
            // System: split disc (left filled = auto-follow).
            canvas.stroke_circle(scx, cy, ic);
            canvas.fill_circle(scx, cy, ic);            // full disc…
            canvas.set_fill_color(resolve_color("bg.surface", Color::rgba8(30, 30, 46)));
            canvas.fill_rect(scx, cy - ic - 1, ic + 2, ic * 2 + 2);  // …carve right half
            canvas.set_stroke_color(col);
            canvas.stroke_circle(scx, cy, ic);
        } else if (s == 1) {
            // Light: sun (disc + rays).
            canvas.fill_circle(scx, cy, ic * 0.6f);
            for (int k = 0; k < 8; ++k) {
                float a = k * 0.7853982f;  // 45°
                float x0 = scx + std::cos(a) * ic * 0.85f, y0 = cy + std::sin(a) * ic * 0.85f;
                float x1 = scx + std::cos(a) * ic * 1.15f, y1 = cy + std::sin(a) * ic * 1.15f;
                canvas.stroke_line(x0, y0, x1, y1);
            }
        } else {
            // Dark: crescent moon (disc minus an offset disc). The carve must
            // use the OPAQUE color actually behind the icon so the crescent
            // shows — on the active segment that's the accent-tinted pill
            // (accent @0.18 over the surface) composited to a solid color, not
            // the transparent tint itself.
            auto surface = resolve_color("bg.surface", Color::rgba8(30, 30, 46));
            Color carve = surface;
            if (s == active) {
                carve = Color::rgba(surface.r * 0.82f + accent.r * 0.18f,
                                    surface.g * 0.82f + accent.g * 0.18f,
                                    surface.b * 0.82f + accent.b * 0.18f, 1.0f);  // token-lint:allow (pill composite)
            }
            canvas.fill_circle(scx, cy, ic);
            canvas.set_fill_color(carve);
            canvas.fill_circle(scx + ic * 0.45f, cy - ic * 0.25f, ic * 0.85f);
        }
    }

    // Hover name (tooltip-style) centered under the hovered segment.
    if (hover_seg_ >= 0) {
        const char* names[] = {"System", "Light", "Dark"};
        canvas.set_font("Inter", 10.0f);
        canvas.set_fill_color(dim);
        canvas.set_text_align(canvas::TextAlign::center);
        canvas.fill_text(names[hover_seg_], hover_seg_ * seg_w + seg_w * 0.5f, h + 12.0f);
        canvas.set_text_align(canvas::TextAlign::left);
    }
}

void ThemeModeControl::on_mouse_down(Point pos) {
    int s = segment_at_(pos);
    if (s < 0) return;
    ThemeMode m = s == 0 ? ThemeMode::system : (s == 1 ? ThemeMode::light : ThemeMode::dark);
    if (m != mode_) {
        mode_ = m;
        request_repaint();
        if (on_mode_change) on_mode_change(mode_);
    }
}

void ThemeModeControl::on_mouse_event(const MouseEvent& event) {
    if (event.is_down || event.is_wheel) return;
    int s = segment_at_(event.position);
    if (s != hover_seg_) { hover_seg_ = s; request_repaint(); }
}

// ── ProgressBar ──────────────────────────────────────────────────────────

void ProgressBar::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();

    // Track
    canvas.set_fill_color(resolve_color("progress.track", canvas::Color::hex(0x2a2a4a)));
    canvas.fill_rounded_rect(b.x, b.y, b.width, b.height, b.height / 2);

    // Fill
    if (progress_ >= 0) {
        float fill_w = b.width * std::clamp(progress_, 0.0f, 1.0f);
        canvas.set_fill_color(resolve_color("progress.fill", canvas::Color::hex(0xe94560)));
        canvas.fill_rounded_rect(b.x, b.y, fill_w, b.height, b.height / 2);
    }

    // Label
    if (!label_.empty()) {
        canvas.set_font("system", 10);
        canvas.set_text_align(canvas::TextAlign::center);
        canvas.set_fill_color(resolve_color("text.primary", canvas::Color::hex(0xe0e0e0)));
        canvas.fill_text_anchored(label_, b.x + b.width / 2, b.y + b.height / 2,
                                  canvas::Canvas::TextAnchor::GlyphCenter);
    }
}

// ── CallOutBox ───────────────────────────────────────────────────────────

std::unique_ptr<CallOutBox> CallOutBox::confirm(
    const std::string& message,
    std::function<void()> on_ok,
    std::function<void()> on_cancel)
{
    auto box = std::make_unique<CallOutBox>();
    box->set_message(message);
    box->on_confirm = std::move(on_ok);
    box->on_cancel = std::move(on_cancel);
    return box;
}

std::unique_ptr<CallOutBox> CallOutBox::notify(
    const std::string& message, float duration)
{
    auto box = std::make_unique<CallOutBox>();
    box->set_message(message);
    box->auto_dismiss_seconds = duration;
    return box;
}

void CallOutBox::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();
    canvas.set_fill_color(resolve_color("modal.bg", canvas::Color::hex(0x16213e)));
    canvas.fill_rounded_rect(b.x, b.y, b.width, b.height, 8);
    canvas.set_stroke_color(resolve_color("modal.border", canvas::Color::hex(0x3a3a5a)));
    canvas.set_line_width(1);
    canvas.stroke_rounded_rect(b.x, b.y, b.width, b.height, 8);

    canvas.set_font("system", 13);
    canvas.set_fill_color(resolve_color("text.primary", canvas::Color::hex(0xe0e0e0)));
    canvas.fill_text_anchored(message_, b.x + 16, b.y + b.height / 2,
                              canvas::Canvas::TextAnchor::GlyphCenter);
}

bool CallOutBox::on_key_event(const KeyEvent& event) {
    if (event.is_down && event.key == KeyCode::escape) {
        if (on_cancel) on_cancel();
        return true;
    }
    if (event.is_down && event.key == KeyCode::enter) {
        if (on_confirm) on_confirm();
        return true;
    }
    return false;
}

// ── TabPanel ─────────────────────────────────────────────────────────────

void TabPanel::add_tab(std::string title, std::unique_ptr<View> content) {
    content->set_visible(tabs_.empty()); // first tab visible
    add_child(std::move(content)); // transfer ownership to view tree
    tabs_.push_back({std::move(title), nullptr}); // content ptr managed by parent
}

void TabPanel::set_active_tab(int index, Notify notify) {
    if (index == active_ || index < 0 || index >= static_cast<int>(tabs_.size())) return;

    // Hide old, show new
    if (active_ < static_cast<int>(child_count()))
        child_at(static_cast<size_t>(active_))->set_visible(false);
    active_ = index;
    if (active_ < static_cast<int>(child_count()))
        child_at(static_cast<size_t>(active_))->set_visible(true);

    if (notify == Notify::sync) {
        if (on_tab_change) on_tab_change(index);
    } else if (notify == Notify::async && on_tab_change) {
        auto fn = on_tab_change;
        const int i = index;
        queue_async_notification([fn, i] { fn(i); });
    }
}

std::string_view TabPanel::tab_title(int index) const {
    if (index < 0 || index >= static_cast<int>(tabs_.size())) return {};
    return tabs_[static_cast<size_t>(index)].title;
}

int TabPanel::find_tab(std::string_view title) const {
    for (int i = 0; i < static_cast<int>(tabs_.size()); ++i) {
        if (tabs_[static_cast<size_t>(i)].title == title) return i;
    }
    return -1;
}

bool TabPanel::set_active_tab(std::string_view title, Notify notify) {
    const int index = find_tab(title);
    if (index < 0) return false;
    set_active_tab(index, notify);
    return true;
}

void TabPanel::paint(canvas::Canvas& canvas) {
    if (!show_tab_bar_) return;  // card-stack mode: no tab bar, content fills the panel
    auto b = local_bounds();

    const bool underline = (tab_bar_style_ == TabBarStyle::underline);

    // Tab bar background. The `filled` style paints a `bg.secondary` strip
    // behind the row (historic look). The `underline` style omits the strip —
    // titles sit directly on the panel — and instead draws a faint full-width
    // divider rule under the whole row (Ink & Signal navigation treatment).
    if (!underline) {
        canvas.set_fill_color(resolve_color("bg.secondary", canvas::Color::hex(0x16213e)));
        canvas.fill_rect(b.x, b.y, b.width, tab_height_);
    } else {
        canvas.set_stroke_color(resolve_color("divider", canvas::Color::hex(0x2a3550)));
        canvas.set_line_width(1.0f);
        canvas.stroke_line(b.x, b.y + tab_height_ - 0.5f,
                           b.x + b.width, b.y + tab_height_ - 0.5f);
    }

    // Draw tabs — active tab is marked by a teal underline only (matching the
    // Ink & Signal design language; no filled active block).
    float tab_w = tabs_.empty() ? 0 : b.width / static_cast<float>(tabs_.size());
    for (int i = 0; i < static_cast<int>(tabs_.size()); ++i) {
        float tx = b.x + static_cast<float>(i) * tab_w;
        if (i == active_) {
            canvas.set_fill_color(resolve_color("tab.active", canvas::Color::hex(0x14b8a6)));
            canvas.fill_rect(tx, b.y + tab_height_ - 2, tab_w, 2);
        }
        canvas.set_font("system", 12);
        canvas.set_text_align(canvas::TextAlign::center);
        auto text_color = i == active_
            ? resolve_color("text.primary", canvas::Color::hex(0xe0e0e0))
            : resolve_color("tab.inactive", canvas::Color::hex(0x808090));
        canvas.set_fill_color(text_color);
        canvas.fill_text(tabs_[static_cast<size_t>(i)].title,
                         tx + tab_w / 2, b.y + tab_height_ / 2 + 4);
    }
}

void TabPanel::on_mouse_event(const MouseEvent& event) {
    if (!show_tab_bar_) return;  // card-stack mode: nothing to click in the (absent) tab bar
    if (!event.is_down) return;
    if (tabs_.empty()) return;
    if (event.position.y > tab_height_) return; // click below tab bar

    float tab_w = tabs_.empty() ? 0 : local_bounds().width / static_cast<float>(tabs_.size());
    if (tab_w <= 0.0f) return;
    int index = static_cast<int>(event.position.x / tab_w);
    set_active_tab(index);
}

// ── SegmentedControl ───────────────────────────────────────────────────────

void SegmentedControl::set_selected(int index) {
    set_selected(index, Notify::sync);
}

void SegmentedControl::set_selected(int index, Notify notify) {
    if (segments_.empty()) return;
    index = std::clamp(index, 0, static_cast<int>(segments_.size()) - 1);
    if (index == selected_) return;
    selected_ = index;
    request_repaint();
    if (notify == Notify::sync) {
        if (on_change) on_change(selected_);
    } else if (notify == Notify::async && on_change) {
        auto fn = on_change;
        const int i = selected_;
        queue_async_notification([fn, i] { fn(i); });
    }
}

void SegmentedControl::set_selected_silent(int index) {
    if (segments_.empty()) return;
    selected_ = std::clamp(index, 0, static_cast<int>(segments_.size()) - 1);
    request_repaint();
}

int SegmentedControl::segment_at_(Point pos) const {
    auto b = local_bounds();
    if (segments_.empty() || b.width <= 0) return -1;
    if (pos.x < b.x || pos.x >= b.x + b.width) return -1;
    if (pos.y < b.y || pos.y >= b.y + b.height) return -1;
    float seg_w = b.width / static_cast<float>(segments_.size());
    int i = static_cast<int>((pos.x - b.x) / seg_w);
    return std::clamp(i, 0, static_cast<int>(segments_.size()) - 1);
}

void SegmentedControl::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();
    if (segments_.empty()) return;

    const float radius = 6.0f;

    // Inset track — the darker recessed groove the pills sit in.
    canvas.set_fill_color(resolve_color("bg.surface", canvas::Color::hex(0x12161f)));
    canvas.fill_rounded_rect(b.x, b.y, b.width, b.height, radius);

    const float seg_w = b.width / static_cast<float>(segments_.size());
    const auto active_text   = resolve_color("text.primary",  canvas::Color::hex(0xe0e0e0));
    const auto inactive_text = resolve_color("tab.inactive",  canvas::Color::hex(0x808090));

    for (int i = 0; i < static_cast<int>(segments_.size()); ++i) {
        const float sx = b.x + static_cast<float>(i) * seg_w;

        if (i == selected_) {
            // Raised "elevated" pill — lighter fill than the track with a hair
            // border, so the active segment reads as lifted (Figma 227:1763).
            const float pad = 2.0f;
            canvas.set_fill_color(resolve_color("bg.elevated", canvas::Color::hex(0x232a36)));
            canvas.fill_rounded_rect(sx + pad, b.y + pad,
                                     seg_w - 2 * pad, b.height - 2 * pad, radius - 1.0f);
            canvas.set_stroke_color(resolve_color("divider", canvas::Color::hex(0x2a3550)));
            canvas.set_line_width(1.0f);
            canvas.stroke_rounded_rect(sx + pad, b.y + pad,
                                       seg_w - 2 * pad, b.height - 2 * pad, radius - 1.0f);
        }

        canvas.set_font("system", 12);
        canvas.set_text_align(canvas::TextAlign::center);
        canvas.set_fill_color(i == selected_ ? active_text : inactive_text);
        canvas.fill_text(segments_[static_cast<size_t>(i)],
                         sx + seg_w / 2.0f, b.y + b.height / 2.0f + 4.0f);
    }
}

void SegmentedControl::on_mouse_event(const MouseEvent& event) {
    if (event.is_down) {
        int i = segment_at_(event.position);
        if (i >= 0) set_selected(i);
        return;
    }
    // Hover tracking (repaint only on change to avoid churn).
    int h = segment_at_(event.position);
    if (h != hover_) { hover_ = h; request_repaint(); }
}

bool SegmentedControl::on_key_event(const KeyEvent& event) {
    if (!event.is_down) return false;
    if (event.key == KeyCode::left && selected_ > 0) {
        set_selected(selected_ - 1);
        return true;
    }
    if (event.key == KeyCode::right &&
        selected_ < static_cast<int>(segments_.size()) - 1) {
        set_selected(selected_ + 1);
        return true;
    }
    return false;
}

// ── ScrollView ───────────────────────────────────────────────────────────

void ScrollView::clamp_scroll_targets() {
    auto b = local_bounds();
    target_scroll_x_ = std::clamp(target_scroll_x_, 0.0f, std::max(0.0f, content_size_.width - b.width));
    target_scroll_y_ = std::clamp(target_scroll_y_, 0.0f, std::max(0.0f, content_size_.height - b.height));
}

void ScrollView::set_scroll(float x, float y) {
    target_scroll_x_ = x;
    target_scroll_y_ = y;
    clamp_scroll_targets();
    smooth_scroll_x_.set(target_scroll_x_);
    smooth_scroll_y_.set(target_scroll_y_);
}

void ScrollView::scroll_by(float dx, float dy, bool animate) {
    // A scroll gesture dismisses any open dropdown — the anchored menu would
    // otherwise drift away from its field as the page moves under it. Matches
    // native menu behavior (scrolling the backdrop closes the menu).
    if ((dx != 0.0f || dy != 0.0f) && ComboBox::active_popup_)
        ComboBox::close_active_popup();
    target_scroll_x_ += dx;
    target_scroll_y_ += dy;
    clamp_scroll_targets();

    // pulp #1737 — honor CSS `scroll-behavior`. CSS
    // default is `auto` (instant); Pulp's historic default has been
    // smooth, so we treat empty / `smooth` / anything-else as smooth
    // (preserves the existing scroll feel for callers that don't set
    // the slot) and only fast-path to instant when the author
    // explicitly sets `scroll-behavior: auto`.
    const std::string& sb = scroll_behavior();
    if (sb == "auto" || !animate) {
        // Wheel/trackpad scroll passes animate=false so
        // the offset jumps instantly. A trackpad sends a continuous delta
        // stream; the OS already smooths/inerts it, so animating each delta
        // lags behind the fingers. Programmatic scroll (animate defaults to
        // true) still eases below.
        smooth_scroll_x_.set(target_scroll_x_);
        smooth_scroll_y_.set(target_scroll_y_);
        return;
    }

    auto duration = resolve_dimension("motion.duration.fast", 0.10f);
    if (std::abs(smooth_scroll_x_.value() - target_scroll_x_) < 0.01f)
        smooth_scroll_x_.set(target_scroll_x_);
    else
        smooth_scroll_x_.animate_to(target_scroll_x_, duration, easing::ease_out_quad);

    if (std::abs(smooth_scroll_y_.value() - target_scroll_y_) < 0.01f)
        smooth_scroll_y_.set(target_scroll_y_);
    else
        smooth_scroll_y_.animate_to(target_scroll_y_, duration, easing::ease_out_quad);
}

void ScrollView::on_mouse_enter() {
    float dur = resolve_dimension("motion.duration.normal", 0.15f);
    bar_opacity_.animate_to(1.0f, dur, easing::ease_out_quad);
    bar_width_.animate_to(8.0f, dur, easing::ease_out_quad);
}

void ScrollView::on_mouse_leave() {
    float dur = resolve_dimension("motion.duration.normal", 0.15f);
    bar_opacity_.animate_to(0.3f, dur, easing::ease_in_quad);
    bar_width_.animate_to(4.0f, dur, easing::ease_in_quad);
}

void ScrollView::advance_animations(float dt) {
    smooth_scroll_x_.advance(dt);
    smooth_scroll_y_.advance(dt);
    bar_opacity_.advance(dt);
    bar_width_.advance(dt);
}

void ScrollView::layout_children() {
    // Temporarily expand bounds to content_size for child layout,
    // then restore. This prevents children from squishing when the
    // ScrollView is smaller than its content.
    auto saved = bounds();
    auto content_bounds = saved;
    if (content_size_.width > 0) content_bounds.width = std::max(saved.width, content_size_.width);
    if (content_size_.height > 0) content_bounds.height = std::max(saved.height, content_size_.height);
    set_bounds(content_bounds);
    View::layout_children();  // call base with expanded bounds
    set_bounds(saved);  // restore actual bounds for painting/clipping
}

void ScrollView::paint_all(canvas::Canvas& canvas) {
    if (!visible()) return;

    // Call base View::paint_all which handles background, border, opacity.
    // But we override to inject scroll offset for children.
    // Since View::paint_all paints children without offset, we need custom logic.

    auto b = bounds();
    canvas.save();
    canvas.translate(b.x, b.y);

    // Clip to viewport
    canvas.clip_rect(0, 0, b.width, b.height);

    if (opacity() < 1.0f)
        canvas.set_opacity(opacity());

    // Background (re-implement from View since we can't call base partially)
    if (has_background_color()) {
        // Use the view's internal bg painting
    }
    // Paint children first, then scrollbar on top so it isn't occluded.

    float sx = smooth_scroll_x_.value();
    float sy = smooth_scroll_y_.value();

    // Paint children WITH scroll offset applied
    canvas.save();
    canvas.clip_rect(0, 0, b.width, b.height);
    canvas.translate(-sx, -sy);
    for (size_t i = 0; i < child_count(); ++i) {
        child_at(i)->paint_all(canvas);
    }
    canvas.restore();

    // Paint scrollbar indicators AFTER children so they render on top
    paint(canvas);

    if (opacity() < 1.0f)
        canvas.set_opacity(1.0f);

    canvas.restore();
}

void ScrollView::paint(canvas::Canvas& canvas) {
    // Only draw scrollbar indicators here — children are painted by paint_all with scroll offset
    auto b = local_bounds();
    float sx = smooth_scroll_x_.value();
    float sy = smooth_scroll_y_.value();

    float opacity = bar_opacity_.value();
    float width = bar_width_.value();

    // Vertical scroll bar
    if (direction_ != Direction::horizontal && content_size_.height > b.height) {
        float ratio = b.height / content_size_.height;
        float bar_h = std::max(20.0f, b.height * ratio);
        float max_scroll = content_size_.height - b.height;
        float bar_y = b.y + (max_scroll > 0 ? (sy / max_scroll) * (b.height - bar_h) : 0);
        auto alpha = static_cast<uint8_t>(255 * opacity * 0.4f);
        canvas.set_fill_color(canvas::Color::rgba8(255, 255, 255, alpha));
        canvas.fill_rounded_rect(b.x + b.width - width - 2, bar_y, width, bar_h, width * 0.5f);
    }

    // Horizontal scroll bar
    if (direction_ != Direction::vertical && content_size_.width > b.width) {
        float ratio = b.width / content_size_.width;
        float bar_w = std::max(20.0f, b.width * ratio);
        float max_scroll = content_size_.width - b.width;
        float bar_x = b.x + (max_scroll > 0 ? (sx / max_scroll) * (b.width - bar_w) : 0);
        auto alpha = static_cast<uint8_t>(255 * opacity * 0.4f);
        canvas.set_fill_color(canvas::Color::rgba8(255, 255, 255, alpha));
        canvas.fill_rounded_rect(bar_x, b.y + b.height - width - 2, bar_w, width, width * 0.5f);
    }
}

View* ScrollView::hit_test(Point local_point) {
    if (!visible() || !enabled() || !hit_testable()) return nullptr;
    if (!local_bounds().contains(local_point)) return nullptr;

    // React Native pointerEvents parity (pulp #1170):
    // ScrollView shadows the base View::hit_test, so without this honor the
    // setPointerEvents(box-only/box-none/none) path silently no-ops on
    // scrollables. Mirror View::hit_test's policy here.
    if (pointer_events() == PointerEvents::none) return nullptr;

    auto b = local_bounds();
    float bar_w = bar_width_.value();
    bool in_v_bar = direction_ != Direction::horizontal &&
                    content_size_.height > b.height &&
                    local_point.x >= b.x + b.width - bar_w - 6;
    bool in_h_bar = direction_ != Direction::vertical &&
                    content_size_.width > b.width &&
                    local_point.y >= b.y + b.height - bar_w - 6;
    // Scrollbar hits target the ScrollView itself. box_none disables
    // self-targeting; box_only still routes scrollbar interactions to
    // self (the chrome belongs to the container, not its children).
    if (in_v_bar || in_h_bar) {
        return pointer_events() == PointerEvents::box_none ? nullptr : this;
    }

    float sx = smooth_scroll_x_.value();
    float sy = smooth_scroll_y_.value();

    if (pointer_events() != PointerEvents::box_only) {
        for (size_t i = child_count(); i > 0; --i) {
            auto* child = child_at(i - 1);
            if (!child->visible()) continue;

            Point child_point = {local_point.x + sx - child->bounds().x,
                                 local_point.y + sy - child->bounds().y};

            // overflow:visible: expand hit area symmetrically on all four
            // sides for popovers that extend in any direction (pulp #1148).
            bool in_bounds = child->local_bounds().contains(child_point);
            if (!in_bounds && child->overflow() == Overflow::visible) {
                auto lb = child->local_bounds();
                in_bounds = child_point.x >= lb.x - 500 &&
                            child_point.x <= lb.x + lb.width + 500 &&
                            child_point.y >= lb.y - 500 &&
                            child_point.y <= lb.y + lb.height + 500;
            }

            if (in_bounds) {
                if (auto* hit = child->hit_test(child_point))
                    return hit;
            }
        }
    }

    // box_none suppresses self-targeting even when no child was hit.
    if (pointer_events() == PointerEvents::box_none) return nullptr;
    return this;
}

ScrollView* find_scroll_view_at(View& root, Point root_point) {
    // Recursive descent that converts the point into each view's local
    // space and tracks the DEEPEST ScrollView whose bounds contain it.
    // Bounds-only (ignores visibility/hit-test gates beyond visibility)
    // so empty background inside a scroll pane still resolves.
    ScrollView* best = nullptr;
    std::function<void(View&, Point)> walk = [&](View& v, Point local) {
        if (!v.visible()) return;
        if (!v.local_bounds().contains(local)) return;
        if (auto* sv = dynamic_cast<ScrollView*>(&v)) best = sv;
        for (size_t i = 0; i < v.child_count(); ++i) {
            View* child = v.child_at(i);
            if (!child) continue;
            Point child_local{local.x - child->bounds().x,
                              local.y - child->bounds().y};
            walk(*child, child_local);
        }
    };
    walk(root, root_point);
    return best;
}

View* find_wheel_scroll_view_at(View& root, Point root_point) {
    View* best = nullptr;
    std::function<void(View&, Point)> walk = [&](View& v, Point local) {
        if (!v.visible() || !v.enabled() || !v.hit_testable()) return;
        if (!v.local_bounds().contains(local)) return;
        if (v.pointer_events() == View::PointerEvents::none) return;
        if (v.wants_wheel_scroll() && v.pointer_events() != View::PointerEvents::box_none)
            best = &v;
        if (v.pointer_events() == View::PointerEvents::box_only) return;
        for (size_t i = 0; i < v.child_count(); ++i) {
            View* child = v.child_at(i);
            if (!child) continue;
            Point child_local{local.x - child->bounds().x,
                              local.y - child->bounds().y};
            walk(*child, child_local);
        }
    };
    walk(root, root_point);
    return best;
}

void ScrollView::on_mouse_event(const MouseEvent& event) {
    if (event.is_wheel) {
        // macOS trackpad provides pixel-level deltas
        float dx = event.scroll_delta_x;
        float dy = event.scroll_delta_y;
        // Restrict to configured scroll direction
        if (direction_ == Direction::vertical) dx = 0;
        if (direction_ == Direction::horizontal) dy = 0;
        // animate=false: wheel/trackpad scrolls instantly (see scroll_by).
        scroll_by(dx, dy, /*animate=*/false);
        // Show scrollbar while scrolling (will fade when we add animation timer)
        bar_opacity_.set(0.6f);
        bar_width_.set(6.0f);
        return;
    }

    auto b = local_bounds();
    float bar_w = bar_width_.value();

    // Vertical scrollbar hit zone (right edge)
    bool in_v_bar = direction_ != Direction::horizontal &&
                    content_size_.height > b.height &&
                    event.position.x >= b.x + b.width - bar_w - 6;

    // Horizontal scrollbar hit zone (bottom edge)
    bool in_h_bar = direction_ != Direction::vertical &&
                    content_size_.width > b.width &&
                    event.position.y >= b.y + b.height - bar_w - 6;

    if (event.is_down && event.button == MouseButton::left) {
        if (in_v_bar) {
            float ratio = b.height / content_size_.height;
            float bar_h = std::max(20.0f, b.height * ratio);
            float max_scroll = content_size_.height - b.height;
            float sy = smooth_scroll_y_.value();
            float bar_y = b.y + (max_scroll > 0 ? (sy / max_scroll) * (b.height - bar_h) : 0);

            if (event.position.y >= bar_y && event.position.y <= bar_y + bar_h) {
                // Clicked on thumb — start drag
                dragging_v_bar_ = true;
                drag_offset_ = event.position.y - bar_y;
            } else {
                // Clicked on track — jump to position
                float click_ratio = (event.position.y - b.y - bar_h * 0.5f) / (b.height - bar_h);
                click_ratio = std::clamp(click_ratio, 0.0f, 1.0f);
                target_scroll_y_ = click_ratio * max_scroll;
                clamp_scroll_targets();
                smooth_scroll_y_.animate_to(target_scroll_y_, 0.1f);
            }
            return;
        }
        if (in_h_bar) {
            float ratio = b.width / content_size_.width;
            float bar_w_px = std::max(20.0f, b.width * ratio);
            float max_scroll = content_size_.width - b.width;
            float sx = smooth_scroll_x_.value();
            float bar_x = b.x + (max_scroll > 0 ? (sx / max_scroll) * (b.width - bar_w_px) : 0);

            if (event.position.x >= bar_x && event.position.x <= bar_x + bar_w_px) {
                dragging_h_bar_ = true;
                drag_offset_ = event.position.x - bar_x;
            } else {
                float click_ratio = (event.position.x - b.x - bar_w_px * 0.5f) / (b.width - bar_w_px);
                click_ratio = std::clamp(click_ratio, 0.0f, 1.0f);
                target_scroll_x_ = click_ratio * max_scroll;
                clamp_scroll_targets();
                smooth_scroll_x_.animate_to(target_scroll_x_, 0.1f);
            }
            return;
        }
    }

    // Mouse up — stop dragging
    if (!event.is_down) {
        dragging_v_bar_ = false;
        dragging_h_bar_ = false;
        return;
    }
}

void ScrollView::on_mouse_drag(Point pos) {
    auto b = local_bounds();

    if (dragging_v_bar_) {
        float ratio = b.height / content_size_.height;
        float bar_h = std::max(20.0f, b.height * ratio);
        float max_scroll = content_size_.height - b.height;
        float track_h = b.height - bar_h;
        if (track_h > 0) {
            float bar_top = pos.y - drag_offset_;
            float scroll_ratio = (bar_top - b.y) / track_h;
            scroll_ratio = std::clamp(scroll_ratio, 0.0f, 1.0f);
            target_scroll_y_ = scroll_ratio * max_scroll;
            clamp_scroll_targets();
            smooth_scroll_y_.set(target_scroll_y_);  // immediate, no animation during drag
        }
    }

    if (dragging_h_bar_) {
        float ratio = b.width / content_size_.width;
        float bar_w_px = std::max(20.0f, b.width * ratio);
        float max_scroll = content_size_.width - b.width;
        float track_w = b.width - bar_w_px;
        if (track_w > 0) {
            float bar_left = pos.x - drag_offset_;
            float scroll_ratio = (bar_left - b.x) / track_w;
            scroll_ratio = std::clamp(scroll_ratio, 0.0f, 1.0f);
            target_scroll_x_ = scroll_ratio * max_scroll;
            clamp_scroll_targets();
            smooth_scroll_x_.set(target_scroll_x_);
        }
    }
}

// ── ListBox ──────────────────────────────────────────────────────────────

void ListBox::set_selected(int index, Notify notify) {
    if (index != selected_) {
        selected_ = index;
        if (notify == Notify::sync) {
            if (on_select) on_select(index);
        } else if (notify == Notify::async && on_select) {
            auto fn = on_select;
            const int i = index;
            queue_async_notification([fn, i] { fn(i); });
        }
    }
}

void ListBox::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();

    canvas.save();
    canvas.clip_rect(b.x, b.y, b.width, b.height);

    int visible_start = static_cast<int>(scroll_offset_ / row_height_);
    int visible_count = static_cast<int>(b.height / row_height_) + 2;

    const bool accent = (selection_style_ == SelectionStyle::accent);

    for (int i = visible_start; i < std::min(visible_start + visible_count, static_cast<int>(items_.size())); ++i) {
        float y = b.y + static_cast<float>(i) * row_height_ - scroll_offset_;
        const bool is_sel = (i == selected_);

        // Selected-row fill. `accent` uses a translucent accent tint plus a
        // teal left-edge bar (Ink & Signal sidebar nav); `standard` keeps the
        // historic opaque elevated fill.
        if (is_sel) {
            if (accent) {
                canvas.set_fill_color(resolve_color("nav.selected.bg", canvas::Color::rgba8(20, 184, 166, 38)));
                canvas.fill_rect(b.x, y, b.width, row_height_);
                canvas.set_fill_color(resolve_color("nav.selected.text", canvas::Color::hex(0x14b8a6)));
                canvas.fill_rect(b.x, y, 3.0f, row_height_);
            } else {
                canvas.set_fill_color(resolve_color("bg.elevated", canvas::Color::hex(0x0f3460)));
                canvas.fill_rect(b.x, y, b.width, row_height_);
            }
        }

        float text_x = b.x + 8;

        // Optional leading icon glyph, when set for this row.
        if (i < static_cast<int>(icons_.size()) && !icons_[static_cast<size_t>(i)].empty()) {
            canvas.set_font("system", 13);
            canvas.set_fill_color(resolve_color("text.secondary", canvas::Color::hex(0x808090)));
            canvas.fill_text(icons_[static_cast<size_t>(i)], text_x, y + row_height_ * 0.65f);
            text_x += 22.0f;
        }

        canvas.set_font("system", 13);
        canvas.set_fill_color(
            (is_sel && accent)
                ? resolve_color("nav.selected.text", canvas::Color::hex(0x14b8a6))
                : resolve_color("text.primary", canvas::Color::hex(0xe0e0e0)));
        canvas.fill_text(items_[static_cast<size_t>(i)], text_x, y + row_height_ * 0.65f);
    }

    canvas.restore();

    // Scroll indicator
    float total_h = static_cast<float>(items_.size()) * row_height_;
    if (total_h > b.height) {
        float ratio = b.height / total_h;
        float bar_h = b.height * ratio;
        float bar_y = b.y + (scroll_offset_ / total_h) * b.height;
        canvas.set_fill_color(canvas::Color::rgba8(255, 255, 255, 40));
        canvas.fill_rounded_rect(b.x + b.width - 6, bar_y, 4, bar_h, 2);
    }
}

void ListBox::on_mouse_event(const MouseEvent& event) {
    // Scroll wheel support
    if (event.is_wheel) {
        float total_h = static_cast<float>(items_.size()) * row_height_;
        float max_scroll = std::max(0.0f, total_h - local_bounds().height);
        scroll_offset_ = std::clamp(scroll_offset_ + event.scroll_delta_y, 0.0f, max_scroll);
        return;
    }

    if (!event.is_down) return;
    int index = static_cast<int>((event.position.y + scroll_offset_) / row_height_);
    if (index >= 0 && index < static_cast<int>(items_.size())) {
        set_selected(index);
        if (event.click_count == 2 && on_activate) {
            on_activate(index);
        }
    }
}

bool ListBox::on_key_event(const KeyEvent& event) {
    if (!event.is_down) return false;
    if (event.key == KeyCode::up && selected_ > 0) {
        set_selected(selected_ - 1);
        ensure_visible(selected_);
        return true;
    }
    if (event.key == KeyCode::down && selected_ < static_cast<int>(items_.size()) - 1) {
        set_selected(selected_ + 1);
        ensure_visible(selected_);
        return true;
    }
    if (event.key == KeyCode::enter && selected_ >= 0 && on_activate) {
        on_activate(selected_);
        return true;
    }
    return false;
}

void ListBox::ensure_visible(int index) {
    if (index < 0) return;
    float item_top = static_cast<float>(index) * row_height_;
    float item_bottom = item_top + row_height_;
    float view_h = local_bounds().height;

    if (item_top < scroll_offset_) {
        scroll_offset_ = item_top;
    } else if (item_bottom > scroll_offset_ + view_h) {
        scroll_offset_ = item_bottom - view_h;
    }
}

} // namespace pulp::view
