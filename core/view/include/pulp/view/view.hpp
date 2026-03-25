#pragma once

#include <pulp/view/geometry.hpp>
#include <pulp/view/theme.hpp>
#include <vector>
#include <memory>
#include <string>

namespace pulp::view {

// Forward declarations
class Canvas;

// Base class for all UI elements
// Views form a tree: each view has zero or more children and one optional parent
class View {
public:
    View() = default;
    virtual ~View() = default;

    View(const View&) = delete;
    View& operator=(const View&) = delete;

    // ── Bounds and layout ────────────────────────────────────────────────

    // The view's bounds in its parent's coordinate space
    Rect bounds() const { return bounds_; }
    void set_bounds(Rect r);

    // The view's local bounds (origin at 0,0)
    Rect local_bounds() const { return {0, 0, bounds_.width, bounds_.height}; }

    // Flex layout style
    FlexStyle& flex() { return flex_; }
    const FlexStyle& flex() const { return flex_; }

    // ── Child management ─────────────────────────────────────────────────

    void add_child(std::unique_ptr<View> child);
    std::unique_ptr<View> remove_child(View* child);
    size_t child_count() const { return children_.size(); }
    View* child_at(size_t index) { return children_[index].get(); }
    const View* child_at(size_t index) const { return children_[index].get(); }
    View* parent() const { return parent_; }

    // ── Hit testing ──────────────────────────────────────────────────────

    // Find the deepest child that contains the given point (in local coords)
    View* hit_test(Point local_point);

    // ── Theme ────────────────────────────────────────────────────────────

    void set_theme(const Theme& theme) { theme_ = theme; }
    const Theme& theme() const { return theme_; }

    // Resolve a color: check own theme first, then walk up to parent
    Color resolve_color(const std::string& name, Color fallback = {}) const;

    // ── Visibility ───────────────────────────────────────────────────────

    bool visible() const { return visible_; }
    void set_visible(bool v) { visible_ = v; }

    // ── Layout ───────────────────────────────────────────────────────────

    // Perform flex layout on children
    void layout_children();

    // ── Lifecycle (override in subclasses) ────────────────────────────────

    virtual void on_resized() {}
    virtual void on_attached() {}
    virtual void on_detached() {}

    // ── Identity ─────────────────────────────────────────────────────────

    void set_id(std::string id) { id_ = std::move(id); }
    const std::string& id() const { return id_; }

private:
    Rect bounds_{};
    FlexStyle flex_{};
    Theme theme_;
    View* parent_ = nullptr;
    std::vector<std::unique_ptr<View>> children_;
    std::string id_;
    bool visible_ = true;
};

} // namespace pulp::view
