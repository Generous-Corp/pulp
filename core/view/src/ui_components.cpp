#include <pulp/view/ui_components.hpp>
#include <pulp/view/animation.hpp>
#include <algorithm>

namespace pulp::view {

const std::string ComboBox::empty_string_;

// ── ComboBox ─────────────────────────────────────────────────────────────

void ComboBox::set_selected_impl(int index, bool notify) {
    if (index >= 0 && index < static_cast<int>(items_.size()) && index != selected_) {
        selected_ = index;
        set_access_value(items_[static_cast<size_t>(index)]);
        if (notify && on_change) on_change(index);
    }
}

void ComboBox::set_selected(int index) {
    set_selected_impl(index, true);
}

void ComboBox::set_selected_silent(int index) {
    set_selected_impl(index, false);
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
    auto bg = resolve_color("bg.surface", canvas::Color::rgba8(30, 30, 46));
    auto border_c = resolve_color("control.border", canvas::Color::rgba8(80, 80, 100));
    auto text_c = resolve_color("text.primary", canvas::Color::rgba8(220, 220, 230));

    float base_h = std::min(b.height, 28.0f);

    // Background
    canvas.set_fill_color(bg);
    canvas.fill_rounded_rect(0, 0, b.width, base_h, 4);
    canvas.set_stroke_color(border_c);
    canvas.set_line_width(1);
    canvas.stroke_rounded_rect(0, 0, b.width, base_h, 4);

    // Selected text
    std::string display_text = selected_text();
    float avail = std::max(0.0f, b.width - 30.0f);
    if (canvas.measure_text(display_text) > avail && display_text.size() > 3) {
        while (display_text.size() > 1) {
            display_text.pop_back();
            if (canvas.measure_text(display_text + "...") <= avail) {
                display_text += "...";
                break;
            }
        }
    }
    canvas.set_font("Inter", 12);
    canvas.set_fill_color(text_c);
    canvas.set_text_align(canvas::TextAlign::left);
    canvas.fill_text(display_text, 8, base_h * 0.5f + 4);

    // Dropdown arrow
    float ax = b.width - 16;
    float ay = base_h / 2;
    auto arrow_c = resolve_color("text.secondary", canvas::Color::rgba8(150, 150, 170));
    canvas.set_stroke_color(arrow_c);
    canvas.set_line_width(1.5f);
    canvas.stroke_line(ax - 3, ay - 2, ax, ay + 2);
    canvas.stroke_line(ax, ay + 2, ax + 3, ay - 2);

    // Dropdown menu: deferred to overlay queue so it paints on top of everything
    if (open_ && !items_.empty()) {
        // Compute absolute position by walking up the parent chain
        float abs_x = 0, abs_y = 0;
        View* v = this;
        while (v) {
            abs_x += v->bounds().x;
            abs_y += v->bounds().y;
            v = v->parent();
        }

        float item_h = 24.0f;
        float dd_top = abs_y + base_h + 2;
        float dd_w = b.width;
        float dd_h = static_cast<float>(items_.size()) * item_h;
        int sel = selected_;
        int* hover_ptr = &hover_index_;  // live pointer for dynamic hover tracking
        auto items_copy = items_;
        auto dropdown_bg = resolve_color("bg.elevated", canvas::Color::rgba8(45, 45, 60));
        auto accent_c = resolve_color("accent.primary", canvas::Color::rgba8(100, 150, 255));
        auto border = border_c;
        auto text = text_c;
        canvas.set_font("Inter", 12);
        for (const auto& item : items_copy) {
            if (item.size() >= 3 && item.substr(0, 3) == "---") {
                continue;
            }
            dd_w = std::max(dd_w, canvas.measure_text(item) + 34.0f);
        }

        overlay_queue().push_back({[=](canvas::Canvas& c) {
            c.save();
            c.set_fill_color(dropdown_bg);
            c.fill_rounded_rect(abs_x, dd_top, dd_w, dd_h, 4);
            c.set_stroke_color(border);
            c.set_line_width(1);
            c.stroke_rounded_rect(abs_x, dd_top, dd_w, dd_h, 4);

            c.set_font("Inter", 12);
            for (int i = 0; i < static_cast<int>(items_copy.size()); ++i) {
                float iy = dd_top + static_cast<float>(i) * item_h;
                const auto& item = items_copy[static_cast<size_t>(i)];

                if (item.size() >= 3 && item.substr(0, 3) == "---") {
                    c.set_stroke_color(border);
                    c.set_line_width(0.5f);
                    c.stroke_line(abs_x + 4, iy + item_h * 0.5f,
                                  abs_x + dd_w - 4, iy + item_h * 0.5f);
                    continue;
                }

                int hov = *hover_ptr;
                // Highlight: hover takes priority for background, check marks selected
                if (i == hov) {
                    c.set_fill_color(accent_c);
                    c.fill_rect(abs_x + 1, iy, dd_w - 2, item_h);
                }
                // Check glyph only for the selected item
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
    active_popup_ = this;
}

void ComboBox::close_dropdown() {
    if (!open_) return;
    open_ = false;
    set_overflow(Overflow::hidden);
    if (active_popup_ == this) active_popup_ = nullptr;
}

void ComboBox::on_mouse_event(const MouseEvent& event) {
    auto b = local_bounds();
    float header_h = std::min(b.height, 28.0f);

    // Track hover on mouse move (even without button down)
    if (open_ && !event.is_down && !event.is_wheel) {
        float dropdown_top = header_h + 2;
        if (event.position.y >= dropdown_top) {
            hover_index_ = static_cast<int>((event.position.y - dropdown_top) / 24.0f);
            if (hover_index_ >= static_cast<int>(items_.size())) hover_index_ = -1;
        } else {
            hover_index_ = -1;
        }
        return;
    }

    if (!event.is_down) return;

    if (open_) {
        float dropdown_top = header_h + 2;
        if (event.position.y >= dropdown_top) {
            int index = static_cast<int>((event.position.y - dropdown_top) / 24.0f);
            if (index >= 0 && index < static_cast<int>(items_.size())) {
                const auto& item = items_[static_cast<size_t>(index)];
                if (item.size() < 3 || item.substr(0, 3) != "---") {
                    set_selected(index);
                }
            }
        }
        close_dropdown();
    } else {
        open_dropdown();
    }
}


bool ComboBox::on_key_event(const KeyEvent& event) {
    if (!event.is_down) return false;
    if (event.key == KeyCode::up && selected_ > 0) {
        set_selected(selected_ - 1);
        return true;
    }
    if (event.key == KeyCode::down && selected_ < static_cast<int>(items_.size()) - 1) {
        set_selected(selected_ + 1);
        return true;
    }
    if (event.key == KeyCode::escape && open_) {
        close_dropdown();
        return true;
    }
    if ((event.key == KeyCode::enter || event.key == KeyCode::space) && !open_) {
        open_dropdown();
        return true;
    }
    if ((event.key == KeyCode::enter || event.key == KeyCode::space) && open_) {
        close_dropdown();
        return true;
    }
    return false;
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
    canvas.fill_text(text_, b.x + 6, b.y + 15);
}

// ── ProgressBar ──────────────────────────────────────────────────────────

void ProgressBar::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();

    // Track
    canvas.set_fill_color(resolve_color("progress_track", canvas::Color::hex(0x2a2a4a)));
    canvas.fill_rounded_rect(b.x, b.y, b.width, b.height, b.height / 2);

    // Fill
    if (progress_ >= 0) {
        float fill_w = b.width * std::clamp(progress_, 0.0f, 1.0f);
        canvas.set_fill_color(resolve_color("accent", canvas::Color::hex(0xe94560)));
        canvas.fill_rounded_rect(b.x, b.y, fill_w, b.height, b.height / 2);
    }

    // Label
    if (!label_.empty()) {
        canvas.set_font("system", 10);
        canvas.set_text_align(canvas::TextAlign::center);
        canvas.set_fill_color(resolve_color("text", canvas::Color::hex(0xe0e0e0)));
        canvas.fill_text(label_, b.x + b.width / 2, b.y + b.height / 2 + 3);
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
    canvas.set_fill_color(resolve_color("callout_bg", canvas::Color::hex(0x16213e)));
    canvas.fill_rounded_rect(b.x, b.y, b.width, b.height, 8);
    canvas.set_stroke_color(resolve_color("border", canvas::Color::hex(0x3a3a5a)));
    canvas.set_line_width(1);
    canvas.stroke_rounded_rect(b.x, b.y, b.width, b.height, 8);

    canvas.set_font("system", 13);
    canvas.set_fill_color(resolve_color("text", canvas::Color::hex(0xe0e0e0)));
    canvas.fill_text(message_, b.x + 16, b.y + b.height / 2 + 4);
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

void TabPanel::set_active_tab(int index) {
    if (index == active_ || index < 0 || index >= static_cast<int>(tabs_.size())) return;

    // Hide old, show new
    if (active_ < static_cast<int>(child_count()))
        child_at(static_cast<size_t>(active_))->set_visible(false);
    active_ = index;
    if (active_ < static_cast<int>(child_count()))
        child_at(static_cast<size_t>(active_))->set_visible(true);

    if (on_tab_change) on_tab_change(index);
}

void TabPanel::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();

    // Tab bar background
    canvas.set_fill_color(resolve_color("tab_bar_bg", canvas::Color::hex(0x16213e)));
    canvas.fill_rect(b.x, b.y, b.width, tab_height_);

    // Draw tabs
    float tab_w = tabs_.empty() ? 0 : b.width / static_cast<float>(tabs_.size());
    for (int i = 0; i < static_cast<int>(tabs_.size()); ++i) {
        float tx = b.x + static_cast<float>(i) * tab_w;
        if (i == active_) {
            canvas.set_fill_color(resolve_color("tab_active_bg", canvas::Color::hex(0x1a1a2e)));
            canvas.fill_rect(tx, b.y, tab_w, tab_height_);
            canvas.set_fill_color(resolve_color("accent", canvas::Color::hex(0xe94560)));
            canvas.fill_rect(tx, b.y + tab_height_ - 2, tab_w, 2);
        }
        canvas.set_font("system", 12);
        canvas.set_text_align(canvas::TextAlign::center);
        auto text_color = i == active_
            ? resolve_color("text", canvas::Color::hex(0xe0e0e0))
            : resolve_color("text_muted", canvas::Color::hex(0x808090));
        canvas.set_fill_color(text_color);
        canvas.fill_text(tabs_[static_cast<size_t>(i)].title,
                         tx + tab_w / 2, b.y + tab_height_ / 2 + 4);
    }
}

void TabPanel::on_mouse_event(const MouseEvent& event) {
    if (!event.is_down) return;
    if (event.position.y > tab_height_) return; // click below tab bar

    float tab_w = tabs_.empty() ? 0 : local_bounds().width / static_cast<float>(tabs_.size());
    int index = static_cast<int>(event.position.x / tab_w);
    set_active_tab(index);
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

void ScrollView::scroll_by(float dx, float dy) {
    target_scroll_x_ += dx;
    target_scroll_y_ += dy;
    clamp_scroll_targets();
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
    // Let the base paint handle bg/border via paint() call path
    // Actually just call paint() for scrollbar drawing, and handle children ourselves

    // Paint background + border via a minimal approach
    // (View::paint_all does this but also paints children without scroll offset)
    paint(canvas);  // This draws scrollbars + any ScrollView-specific visuals

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

    auto b = local_bounds();
    float bar_w = bar_width_.value();
    bool in_v_bar = direction_ != Direction::horizontal &&
                    content_size_.height > b.height &&
                    local_point.x >= b.x + b.width - bar_w - 6;
    bool in_h_bar = direction_ != Direction::vertical &&
                    content_size_.width > b.width &&
                    local_point.y >= b.y + b.height - bar_w - 6;
    if (in_v_bar || in_h_bar)
        return this;

    float sx = smooth_scroll_x_.value();
    float sy = smooth_scroll_y_.value();

    for (size_t i = child_count(); i > 0; --i) {
        auto* child = child_at(i - 1);
        if (!child->visible()) continue;

        Point child_point = {local_point.x + sx - child->bounds().x,
                             local_point.y + sy - child->bounds().y};

        bool in_bounds = child->local_bounds().contains(child_point);
        if (!in_bounds && child->overflow() == Overflow::visible) {
            auto lb = child->local_bounds();
            in_bounds = child_point.x >= lb.x && child_point.x <= lb.x + lb.width &&
                        child_point.y >= lb.y && child_point.y <= lb.y + lb.height + 500;
        }

        if (in_bounds) {
            if (auto* hit = child->hit_test(child_point))
                return hit;
        }
    }

    return this;
}

void ScrollView::on_mouse_event(const MouseEvent& event) {
    if (event.is_wheel) {
        // macOS trackpad provides pixel-level deltas
        float dx = event.scroll_delta_x;
        float dy = event.scroll_delta_y;
        // Restrict to configured scroll direction
        if (direction_ == Direction::vertical) dx = 0;
        if (direction_ == Direction::horizontal) dy = 0;
        scroll_by(dx, dy);
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

void ListBox::set_selected(int index) {
    if (index != selected_) {
        selected_ = index;
        if (on_select) on_select(index);
    }
}

void ListBox::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();

    canvas.save();
    canvas.clip_rect(b.x, b.y, b.width, b.height);

    int visible_start = static_cast<int>(scroll_offset_ / row_height_);
    int visible_count = static_cast<int>(b.height / row_height_) + 2;

    for (int i = visible_start; i < std::min(visible_start + visible_count, static_cast<int>(items_.size())); ++i) {
        float y = b.y + static_cast<float>(i) * row_height_ - scroll_offset_;

        if (i == selected_) {
            canvas.set_fill_color(resolve_color("list_selected_bg", canvas::Color::hex(0x0f3460)));
            canvas.fill_rect(b.x, y, b.width, row_height_);
        }

        canvas.set_font("system", 13);
        canvas.set_fill_color(resolve_color("text", canvas::Color::hex(0xe0e0e0)));
        canvas.fill_text(items_[static_cast<size_t>(i)], b.x + 8, y + row_height_ * 0.65f);
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
