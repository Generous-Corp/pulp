#include <pulp/vellum_components/overlay_geometry.hpp>

#include <vellum/view/view.hpp>

#include <cmath>
#include <functional>

namespace pulp::vellum_components::overlay {

Rect bounds_in_root(const View* view, const View& root) {
    if (!view)
        return {};
    float x = 0, y = 0;
    for (const View* cur = view; cur && cur != &root; cur = cur->parent()) {
        x += cur->bounds().x;
        y += cur->bounds().y;
    }
    return {x, y, view->bounds().width, view->bounds().height};
}

float effective_scale_in_root(const View* view, const View& root) {
    float scale = 1.0f;
    for (const View* cur = view; cur && cur != &root; cur = cur->parent())
        scale *= cur->scale();
    return scale;
}

bool is_self_or_ancestor(const View* ancestor, const View* view) {
    for (const View* cur = view; cur; cur = cur->parent())
        if (cur == ancestor)
            return true;
    return false;
}

const View* containing_block_of(const View* view, const View& root, Rect& block_in_root) {
    for (const View* cur = view ? view->parent() : nullptr; cur && cur != &root;
         cur = cur->parent()) {
        if (cur->position() != View::Position::static_) {
            block_in_root = bounds_in_root(cur, root);
            return cur;
        }
    }
    block_in_root = bounds_in_root(&root, root);
    return &root;
}

DragHandle hit_test_handle(const Rect& r, Point pos) {
    const auto in_box = [&](float cx, float cy) {
        return pos.x >= cx - kHandleGrabRadius && pos.x <= cx + kHandleGrabRadius &&
               pos.y >= cy - kHandleGrabRadius && pos.y <= cy + kHandleGrabRadius;
    };
    const float midx = r.x + r.width * 0.5f;
    const float midy = r.y + r.height * 0.5f;
    // A corner shares its coordinates with the ends of two edges, and a corner
    // press resizes both axes, so corners are tested first.
    if (in_box(r.x, r.y))
        return DragHandle::nw;
    if (in_box(r.x + r.width, r.y))
        return DragHandle::ne;
    if (in_box(r.x, r.y + r.height))
        return DragHandle::sw;
    if (in_box(r.x + r.width, r.y + r.height))
        return DragHandle::se;
    if (in_box(midx, r.y))
        return DragHandle::n;
    if (in_box(midx, r.y + r.height))
        return DragHandle::s;
    if (in_box(r.x + r.width, midy))
        return DragHandle::e;
    if (in_box(r.x, midy))
        return DragHandle::w;
    return DragHandle::none;
}

bool hit_test_body(const Rect& r, Point pos) {
    // A handle press wins over a body press, so a point on a handle is not a
    // body hit.
    if (hit_test_handle(r, pos) != DragHandle::none)
        return false;
    return pos.x >= r.x && pos.x <= r.x + r.width && pos.y >= r.y && pos.y <= r.y + r.height;
}

CursorAffordance affordance_for_handle(DragHandle handle) {
    switch (handle) {
    case DragHandle::nw:
    case DragHandle::se:
        return CursorAffordance::resize_nw_se;
    case DragHandle::ne:
    case DragHandle::sw:
        return CursorAffordance::resize_ne_sw;
    case DragHandle::n:
    case DragHandle::s:
        return CursorAffordance::resize_ns;
    case DragHandle::e:
    case DragHandle::w:
        return CursorAffordance::resize_ew;
    case DragHandle::none:
        break;
    }
    return CursorAffordance::none;
}

CursorAffordance affordance_at(const Rect& selection, Point pos) {
    const DragHandle handle = hit_test_handle(selection, pos);
    if (handle != DragHandle::none)
        return affordance_for_handle(handle);
    if (hit_test_body(selection, pos))
        return CursorAffordance::move;
    return CursorAffordance::none;
}

int cursor_style_value(CursorAffordance affordance) {
    switch (affordance) {
    case CursorAffordance::move:
        // The four-way cursor; macOS maps it to a hand, which reads as grab.
        return static_cast<int>(View::CursorStyle::multi_directional_resize);
    case CursorAffordance::resize_nw_se:
        return static_cast<int>(View::CursorStyle::top_left_resize);
    case CursorAffordance::resize_ne_sw:
        return static_cast<int>(View::CursorStyle::top_right_resize);
    case CursorAffordance::resize_ns:
        return static_cast<int>(View::CursorStyle::vertical_resize);
    case CursorAffordance::resize_ew:
        return static_cast<int>(View::CursorStyle::horizontal_resize);
    case CursorAffordance::none:
        break;
    }
    return -1;
}

std::vector<FlatTreeItem> flatten_tree(const View& root,
                                       const std::unordered_set<const View*>& collapsed) {
    std::vector<FlatTreeItem> items;
    std::function<void(const View*, int)> walk = [&](const View* view, int depth) {
        items.push_back({view, depth});
        if (collapsed.count(view))
            return;
        for (std::size_t i = 0; i < view->child_count(); ++i)
            walk(view->child_at(i), depth + 1);
    };
    walk(&root, 0);
    return items;
}

const FlatTreeItem* tree_item_at(const std::vector<FlatTreeItem>& items, float y,
                                 float row_height) {
    // floor, not truncation: a y above the first row must not land on row 0.
    const int index = static_cast<int>(std::floor(y / row_height));
    if (index >= 0 && index < static_cast<int>(items.size()))
        return &items[index];
    return nullptr;
}

bool point_in_panel(Point p, float root_width, float panel_width, bool manipulate_only) {
    if (manipulate_only)
        return false;
    return p.x >= root_width - panel_width;
}

} // namespace pulp::vellum_components::overlay
