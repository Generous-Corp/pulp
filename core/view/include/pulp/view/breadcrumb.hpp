#pragma once

#include <pulp/view/view.hpp>
#include <functional>
#include <string>
#include <vector>

namespace pulp::view {

// ── Breadcrumb ──────────────────────────────────────────────────────────────
// Navigation breadcrumb trail.

class Breadcrumb : public View {
public:
    struct Item {
        std::string label;
        std::string id;       // Optional identifier for callback
    };

    Breadcrumb();

    /// Set the full breadcrumb path.
    void set_items(std::vector<Item> items);
    const std::vector<Item>& items() const { return items_; }

    /// Push a new item onto the trail.
    void push(Item item);

    /// Pop the last item. Returns it, or empty if trail is empty.
    Item pop();

    /// Pop back to a specific index (inclusive).
    void pop_to(size_t index);

    /// Callback when a breadcrumb item is clicked.
    std::function<void(size_t index, const Item& item)> on_navigate;

    /// Separator string between items.
    void set_separator(std::string sep) { separator_ = std::move(sep); }
    const std::string& separator() const { return separator_; }

    /// Whether to paint the rounded `bg.surface` card behind the trail.
    /// `true` (default) is the historic look. Set `false` for the flat Ink &
    /// Signal treatment where crumbs sit directly on the surrounding panel
    /// (Figma 227:1830). Opt-in; never changes the default.
    void set_show_background(bool show) { show_background_ = show; }
    bool show_background() const { return show_background_; }

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    bool wants_mouse_input() const override { return true; }

    float intrinsic_height() const override { return 32.0f; }

private:
    std::vector<Item> items_;
    std::string separator_ = "/";
    bool show_background_ = true;

    int item_at_position(Point pos) const;
};

} // namespace pulp::view
