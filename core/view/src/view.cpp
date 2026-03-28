#include <pulp/view/view.hpp>
#include <algorithm>
#include <numeric>

namespace pulp::view {

void View::paint_all(canvas::Canvas& canvas) {
    if (!visible_) return;

    canvas.save();
    canvas.translate(bounds_.x, bounds_.y);

    // CSS transform: scale() — scale around center
    if (scale_ != 1.0f) {
        float cx = bounds_.width * 0.5f;
        float cy = bounds_.height * 0.5f;
        canvas.translate(cx, cy);
        canvas.scale(scale_, scale_);
        canvas.translate(-cx, -cy);
    }

    // Clip only if overflow is hidden (default)
    if (overflow_ == Overflow::hidden)
        canvas.clip_rect(0, 0, bounds_.width, bounds_.height);

    // Apply opacity as layer alpha
    if (opacity_ < 1.0f)
        canvas.set_opacity(opacity_);

    // Paint box shadow (before background, extends outside bounds)
    if (has_shadow_) {
        float sx = shadow_.offset_x;
        float sy = shadow_.offset_y;
        float blur = shadow_.blur;
        float spread = shadow_.spread;
        // Approximate blur with multiple translucent rects at increasing offsets
        int steps = std::max(1, static_cast<int>(blur / 2));
        uint8_t base_alpha = shadow_.color.a;
        for (int i = steps; i >= 0; --i) {
            float t = static_cast<float>(i) / static_cast<float>(steps);
            float expand = spread + blur * t;
            auto alpha = static_cast<uint8_t>(base_alpha * (1.0f - t) * (1.0f - t));
            canvas.set_fill_color({shadow_.color.r, shadow_.color.g, shadow_.color.b, alpha});
            canvas.fill_rounded_rect(-expand + sx, -expand + sy,
                                     bounds_.width + expand * 2,
                                     bounds_.height + expand * 2,
                                     corner_radius_ + expand * 0.5f);
        }
    }

    // Paint background if set
    if (has_bg_) {
        canvas.set_fill_color(bg_color_);
        if (corner_radius_ > 0)
            canvas.fill_rounded_rect(0, 0, bounds_.width, bounds_.height, corner_radius_);
        else
            canvas.fill_rect(0, 0, bounds_.width, bounds_.height);
    }

    // Paint border if set
    if (has_border_ && border_width_ > 0) {
        canvas.set_stroke_color(border_color_);
        canvas.set_line_width(border_width_);
        if (corner_radius_ > 0)
            canvas.stroke_rounded_rect(0, 0, bounds_.width, bounds_.height, corner_radius_);
        else
            canvas.stroke_rect(0, 0, bounds_.width, bounds_.height);
    }

    // Widget-specific painting
    paint(canvas);

    // Paint children
    for (auto& child : children_) {
        child->paint_all(canvas);
    }

    // Focus ring — only show on text input widgets, not sliders/toggles/meters
    // Matches CSS :focus-visible behavior (keyboard focus on text inputs)
    if (has_focus_ && focusable_ &&
        access_role_ != AccessRole::slider &&
        access_role_ != AccessRole::toggle &&
        access_role_ != AccessRole::meter) {
        // TextEditor handles its own focus border, so skip the generic ring
        // for views that paint their own focus state
    }

    // Reset opacity
    if (opacity_ < 1.0f)
        canvas.set_opacity(1.0f);

    canvas.restore();
}

void View::simulate_click(Point root_pos) {
    auto* target = hit_test(root_pos);
    if (!target) return;

    // Convert to target's local coordinates
    Point local = root_pos;
    View* v = target;
    while (v && v != this) {
        local.x -= v->bounds().x;
        local.y -= v->bounds().y;
        v = v->parent();
    }

    target->on_mouse_down(local);
    target->on_mouse_up(local);
    if (target->on_click) target->on_click();
}

void View::simulate_drag(Point start, Point end, int steps) {
    auto* target = hit_test(start);
    if (!target) return;

    target->on_mouse_down(start);
    for (int i = 1; i <= steps; ++i) {
        float t = static_cast<float>(i) / steps;
        Point p = {start.x + (end.x - start.x) * t,
                   start.y + (end.y - start.y) * t};
        target->on_mouse_drag(p);
    }
    target->on_mouse_up(end);
}

static void collect_focusable(View& root, std::vector<View*>& out) {
    if (root.focusable()) out.push_back(&root);
    for (size_t i = 0; i < root.child_count(); ++i)
        collect_focusable(*root.child_at(i), out);
}

View* View::focus_next(View& root, View* current) {
    std::vector<View*> focusable;
    collect_focusable(root, focusable);
    if (focusable.empty()) return nullptr;

    if (!current) {
        focusable[0]->set_focus(true);
        return focusable[0];
    }

    current->set_focus(false);
    for (size_t i = 0; i < focusable.size(); ++i) {
        if (focusable[i] == current) {
            auto* next = focusable[(i + 1) % focusable.size()];
            next->set_focus(true);
            return next;
        }
    }
    focusable[0]->set_focus(true);
    return focusable[0];
}

View* View::focus_prev(View& root, View* current) {
    std::vector<View*> focusable;
    collect_focusable(root, focusable);
    if (focusable.empty()) return nullptr;

    if (!current) {
        focusable.back()->set_focus(true);
        return focusable.back();
    }

    current->set_focus(false);
    for (size_t i = 0; i < focusable.size(); ++i) {
        if (focusable[i] == current) {
            auto* prev = focusable[(i + focusable.size() - 1) % focusable.size()];
            prev->set_focus(true);
            return prev;
        }
    }
    focusable.back()->set_focus(true);
    return focusable.back();
}

void View::set_bounds(Rect r) {
    if (bounds_ == r) return;
    bounds_ = r;
    on_resized();
}

void View::add_child(std::unique_ptr<View> child) {
    child->parent_ = this;
    children_.push_back(std::move(child));
    children_.back()->on_attached();
}

std::unique_ptr<View> View::remove_child(View* child) {
    auto it = std::find_if(children_.begin(), children_.end(),
        [child](const auto& p) { return p.get() == child; });
    if (it == children_.end()) return nullptr;

    child->on_detached();
    child->parent_ = nullptr;
    auto owned = std::move(*it);
    children_.erase(it);
    return owned;
}

View* View::hit_test(Point local_point) {
    if (!visible_) return nullptr;

    // Check children in reverse order (topmost first)
    for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
        auto& child = *it;
        if (!child->visible_) continue;

        Point child_point = {local_point.x - child->bounds_.x,
                            local_point.y - child->bounds_.y};

        // For overflow:visible, expand the hit area to include content
        // that extends beyond the child's bounds (e.g., dropdown menus)
        bool in_bounds = child->local_bounds().contains(child_point);
        if (!in_bounds && child->overflow() == Overflow::visible) {
            // Allow hit testing up to 500px below the child (for dropdowns)
            auto lb = child->local_bounds();
            in_bounds = child_point.x >= lb.x && child_point.x <= lb.x + lb.width &&
                       child_point.y >= lb.y && child_point.y <= lb.y + lb.height + 500;
        }

        if (in_bounds) {
            auto* hit = child->hit_test(child_point);
            if (hit) return hit;
        }
    }

    // No child was hit — return this view if the point is within bounds
    if (local_bounds().contains(local_point))
        return this;

    return nullptr;
}

// ── Overlay paint queue ──────────────────────────────────────────────────────

std::vector<View::OverlayRequest>& View::overlay_queue() {
    static std::vector<OverlayRequest> queue;
    return queue;
}

void View::paint_overlays(canvas::Canvas& canvas) {
    auto& queue = overlay_queue();
    for (auto& req : queue) {
        if (req.paint_fn) req.paint_fn(canvas);
    }
    queue.clear();
}

Color View::resolve_color(const std::string& name, Color fallback) const {
    auto c = theme_.color(name);
    if (c.has_value()) return c.value();
    if (parent_) return parent_->resolve_color(name, fallback);
    return fallback;
}

float View::resolve_dimension(const std::string& name, float fallback) const {
    auto d = theme_.dimension(name);
    if (d.has_value()) return d.value();
    if (parent_) return parent_->resolve_dimension(name, fallback);
    return fallback;
}

void View::set_hovered(bool h) {
    if (hovered_ == h) return;
    hovered_ = h;
    if (h) {
        on_mouse_enter();
        if (on_hover_enter) on_hover_enter();
    } else {
        on_mouse_leave();
        if (on_hover_leave) on_hover_leave();
    }
}

FrameClock* View::frame_clock() const {
    if (frame_clock_) return frame_clock_;
    if (parent_) return parent_->frame_clock();
    return nullptr;
}

void View::simulate_hover(Point root_pos) {
    // Clear hover on all children first via a simple recursive walk
    std::function<void(View*)> clear_hover = [&](View* v) {
        if (v->hovered_) v->set_hovered(false);
        for (size_t i = 0; i < v->child_count(); ++i)
            clear_hover(v->child_at(i));
    };
    clear_hover(this);

    // Set hover on the hit target
    auto* target = hit_test(root_pos);
    if (target) target->set_hovered(true);
}

float View::intrinsic_height() const {
    // Containers: sum visible children's heights + gaps (CSS auto height behavior)
    if (children_.empty()) return 0;

    bool is_col = flex_.direction == FlexDirection::column;
    if (!is_col) return 0;  // Row containers don't auto-height from children

    float total = 0;
    float gap = flex_.effective_gap(flex_.direction);
    int count = 0;
    for (auto& child : children_) {
        if (!child->visible_) continue;
        auto& cf = child->flex();
        float h = cf.preferred_height;
        if (h <= 0) h = child->intrinsic_height();
        total += h + cf.margin_t() + cf.margin_b();
        if (count > 0) total += gap;
        ++count;
    }

    // Add padding
    float pt = flex_.padding_top >= 0 ? flex_.padding_top : flex_.padding;
    float pb = flex_.padding_bottom >= 0 ? flex_.padding_bottom : flex_.padding;
    return total + pt + pb;
}

void View::layout_children() {
    if (children_.empty()) return;

    auto area = local_bounds();

    // Per-side padding
    float pt = flex_.padding_top >= 0 ? flex_.padding_top : flex_.padding;
    float pr = flex_.padding_right >= 0 ? flex_.padding_right : flex_.padding;
    float pb = flex_.padding_bottom >= 0 ? flex_.padding_bottom : flex_.padding;
    float pl = flex_.padding_left >= 0 ? flex_.padding_left : flex_.padding;
    area = {area.x + pl, area.y + pt, area.width - pl - pr, area.height - pt - pb};

    bool is_row = flex_.direction == FlexDirection::row;
    float main_size = is_row ? area.width : area.height;
    float cross_size = is_row ? area.height : area.width;
    float gap = flex_.effective_gap(flex_.direction);

    // ── Collect visible children, sorted by order ────────────────────
    struct ChildEntry { View* view; int order; };
    std::vector<ChildEntry> ordered;
    for (auto& child : children_) {
        if (!child->visible_) continue;
        ordered.push_back({child.get(), child->flex().order});
    }
    // Stable sort by order (preserves source order for equal values)
    std::stable_sort(ordered.begin(), ordered.end(),
        [](const ChildEntry& a, const ChildEntry& b) { return a.order < b.order; });

    int visible_count = static_cast<int>(ordered.size());
    if (visible_count == 0) return;

    // ── Pass 1: Measure children (flex_basis → preferred → intrinsic) ──
    float total_fixed = 0;
    float total_flex_grow = 0;
    float total_flex_shrink = 0;
    float total_margins = 0;

    for (auto& entry : ordered) {
        auto& cf = entry.view->flex();
        // Main-axis margins
        float margin_before = is_row ? cf.margin_l() : cf.margin_t();
        float margin_after = is_row ? cf.margin_r() : cf.margin_b();
        total_margins += margin_before + margin_after;

        if (cf.flex_grow > 0) {
            total_flex_grow += cf.flex_grow;
        } else {
            float basis = cf.basis_or_preferred(is_row);
            if (basis <= 0) basis = is_row ? entry.view->intrinsic_width() : entry.view->intrinsic_height();
            float min_val = is_row ? cf.min_width : cf.min_height;
            float max_val = is_row ? cf.max_width : cf.max_height;
            float size = std::max(basis, min_val);
            if (max_val > 0) size = std::min(size, max_val);
            total_fixed += size;
            total_flex_shrink += cf.flex_shrink;
        }
    }

    float total_gaps = visible_count > 1 ? gap * (visible_count - 1) : 0;
    float remaining = main_size - total_fixed - total_gaps - total_margins;

    // ── Pass 2: Compute child sizes ───────────────────────────────────
    struct ChildLayout { View* view; float main_size; float cross_size;
                         float margin_before; float margin_after;
                         float cross_margin_before; float cross_margin_after; };
    std::vector<ChildLayout> layouts;
    layouts.reserve(static_cast<size_t>(visible_count));

    for (auto& entry : ordered) {
        auto& cf = entry.view->flex();
        float child_main;
        float mb = is_row ? cf.margin_l() : cf.margin_t();
        float ma = is_row ? cf.margin_r() : cf.margin_b();
        float cmb = is_row ? cf.margin_t() : cf.margin_l();
        float cma = is_row ? cf.margin_b() : cf.margin_r();

        if (cf.flex_grow > 0 && remaining > 0) {
            child_main = total_flex_grow > 0 ? remaining * (cf.flex_grow / total_flex_grow) : 0;
        } else if (cf.flex_grow == 0 && remaining < 0 && cf.flex_shrink > 0 && total_flex_shrink > 0) {
            float basis = cf.basis_or_preferred(is_row);
            if (basis <= 0) basis = is_row ? entry.view->intrinsic_width() : entry.view->intrinsic_height();
            float min_val = is_row ? cf.min_width : cf.min_height;
            float base = std::max(basis, min_val);
            float shrink_amount = (-remaining) * (cf.flex_shrink / total_flex_shrink);
            child_main = std::max(min_val, base - shrink_amount);
        } else {
            float basis = cf.basis_or_preferred(is_row);
            if (basis <= 0) basis = is_row ? entry.view->intrinsic_width() : entry.view->intrinsic_height();
            float min_val = is_row ? cf.min_width : cf.min_height;
            child_main = std::max(basis, min_val);
        }

        float max_main = is_row ? cf.max_width : cf.max_height;
        if (max_main > 0) child_main = std::min(child_main, max_main);

        // Cross-axis sizing — respect align_self override
        FlexAlign align = (cf.align_self != FlexAlign::auto_) ? cf.align_self : flex_.align_items;
        float cross_min = is_row ? cf.min_height : cf.min_width;
        float cross_preferred = is_row ? cf.preferred_height : cf.preferred_width;
        float cross_intrinsic = is_row ? entry.view->intrinsic_height() : entry.view->intrinsic_width();
        float cross_max = is_row ? cf.max_height : cf.max_width;
        float avail_cross = cross_size - cmb - cma;
        float child_cross;

        if (align == FlexAlign::stretch) {
            child_cross = avail_cross;
        } else {
            child_cross = cross_preferred > 0 ? cross_preferred : cross_intrinsic;
            child_cross = std::max(child_cross, cross_min);
            if (child_cross <= 0) child_cross = avail_cross;
        }
        if (cross_max > 0) child_cross = std::min(child_cross, cross_max);

        layouts.push_back({entry.view, child_main, child_cross, mb, ma, cmb, cma});
    }

    // ── Pass 3: Justify content ──────────────────────────────────────
    float total_content = 0;
    for (auto& l : layouts)
        total_content += l.main_size + l.margin_before + l.margin_after;

    float free_space = std::max(0.0f, main_size - total_content - total_gaps);
    float pos = is_row ? area.x : area.y;
    float extra_gap = 0;

    switch (flex_.justify_content) {
        case FlexJustify::start: break;
        case FlexJustify::center: pos += free_space * 0.5f; break;
        case FlexJustify::end_: pos += free_space; break;
        case FlexJustify::space_between:
            if (visible_count > 1) extra_gap = free_space / (visible_count - 1);
            break;
        case FlexJustify::space_around:
            if (visible_count > 0) {
                float around = free_space / visible_count;
                pos += around * 0.5f; extra_gap = around;
            }
            break;
        case FlexJustify::space_evenly:
            if (visible_count > 0) {
                float even = free_space / (visible_count + 1);
                pos += even; extra_gap = even;
            }
            break;
    }

    // ── Pass 4: Position children ────────────────────────────────────
    for (size_t i = 0; i < layouts.size(); ++i) {
        auto& l = layouts[i];
        auto& cf = l.view->flex();

        pos += l.margin_before;

        // Cross-axis position — respect align_self
        FlexAlign align = (cf.align_self != FlexAlign::auto_) ? cf.align_self : flex_.align_items;
        float cross_pos = (is_row ? area.y : area.x) + l.cross_margin_before;
        float avail_cross = cross_size - l.cross_margin_before - l.cross_margin_after;

        switch (align) {
            case FlexAlign::start:
            case FlexAlign::stretch:
            case FlexAlign::auto_:
                break;
            case FlexAlign::center:
                cross_pos += (avail_cross - l.cross_size) * 0.5f;
                break;
            case FlexAlign::end:
                cross_pos += avail_cross - l.cross_size;
                break;
        }

        Rect child_bounds;
        if (is_row) {
            child_bounds = {pos, cross_pos, l.main_size, l.cross_size};
        } else {
            child_bounds = {cross_pos, pos, l.cross_size, l.main_size};
        }

        l.view->set_bounds(child_bounds);
        l.view->layout_children();
        pos += l.main_size + l.margin_after + gap + extra_gap;
    }
}

} // namespace pulp::view
