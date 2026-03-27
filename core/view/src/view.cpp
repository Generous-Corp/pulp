#include <pulp/view/view.hpp>
#include <algorithm>
#include <numeric>

namespace pulp::view {

void View::paint_all(canvas::Canvas& canvas) {
    if (!visible_) return;

    canvas.save();
    canvas.translate(bounds_.x, bounds_.y);
    canvas.clip_rect(0, 0, bounds_.width, bounds_.height);

    paint(canvas);

    for (auto& child : children_) {
        child->paint_all(canvas);
    }

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
        if (child->local_bounds().contains(child_point)) {
            auto* hit = child->hit_test(child_point);
            if (hit) return hit;
        }
    }

    // No child was hit — return this view if the point is within bounds
    if (local_bounds().contains(local_point))
        return this;

    return nullptr;
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
    if (h) on_mouse_enter();
    else on_mouse_leave();
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

void View::layout_children() {
    if (children_.empty()) return;

    auto area = local_bounds();
    float padding = flex_.padding;
    area = area.inset(padding);

    bool is_row = flex_.direction == FlexDirection::row;
    float main_size = is_row ? area.width : area.height;
    float cross_size = is_row ? area.height : area.width;
    float gap = flex_.gap;

    // Calculate total fixed size and flex grow
    float total_fixed = 0;
    float total_flex_grow = 0;
    int visible_count = 0;

    for (auto& child : children_) {
        if (!child->visible_) continue;
        ++visible_count;

        auto& cf = child->flex();
        if (cf.flex_grow > 0) {
            total_flex_grow += cf.flex_grow;
        } else {
            float preferred = is_row ? cf.preferred_width : cf.preferred_height;
            float min = is_row ? cf.min_width : cf.min_height;
            total_fixed += std::max(preferred, min);
        }
    }

    float total_gaps = visible_count > 1 ? gap * (visible_count - 1) : 0;
    float remaining = std::max(0.0f, main_size - total_fixed - total_gaps);

    // Position children
    float pos = is_row ? area.x : area.y;

    for (auto& child : children_) {
        if (!child->visible_) continue;

        auto& cf = child->flex();
        float child_main, child_cross;

        if (cf.flex_grow > 0) {
            child_main = total_flex_grow > 0 ? remaining * (cf.flex_grow / total_flex_grow) : 0;
        } else {
            float preferred = is_row ? cf.preferred_width : cf.preferred_height;
            float min = is_row ? cf.min_width : cf.min_height;
            child_main = std::max(preferred, min);
        }

        // Cross-axis sizing
        float cross_min = is_row ? cf.min_height : cf.min_width;
        float cross_preferred = is_row ? cf.preferred_height : cf.preferred_width;

        switch (flex_.align_items) {
            case FlexAlign::stretch:
                child_cross = cross_size;
                break;
            case FlexAlign::start:
            case FlexAlign::end:
            case FlexAlign::center:
                child_cross = std::max(cross_preferred, cross_min);
                if (child_cross <= 0) child_cross = cross_size;
                break;
        }

        // Cross-axis position
        float cross_pos = is_row ? area.y : area.x;
        switch (flex_.align_items) {
            case FlexAlign::start:
            case FlexAlign::stretch:
                break;
            case FlexAlign::center:
                cross_pos += (cross_size - child_cross) * 0.5f;
                break;
            case FlexAlign::end:
                cross_pos += cross_size - child_cross;
                break;
        }

        Rect child_bounds;
        if (is_row) {
            child_bounds = {pos, cross_pos, child_main, child_cross};
        } else {
            child_bounds = {cross_pos, pos, child_cross, child_main};
        }

        child->set_bounds(child_bounds);
        child->layout_children();  // Recurse into nested containers
        pos += child_main + gap;
    }
}

} // namespace pulp::view
