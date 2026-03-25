#include <pulp/view/view.hpp>
#include <algorithm>
#include <numeric>

namespace pulp::view {

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
        pos += child_main + gap;
    }
}

} // namespace pulp::view
