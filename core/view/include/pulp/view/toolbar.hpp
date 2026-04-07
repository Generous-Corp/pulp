#pragma once

// Toolbar — horizontal or vertical bar of configurable items.
// Items can be buttons, toggles, separators, or custom views.

#include <pulp/view/view.hpp>
#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace pulp::view {

/// Toolbar item types
enum class ToolbarItemType {
    Button,     // Clickable button with icon/text
    Toggle,     // On/off toggle
    Separator,  // Visual separator
    Spacer,     // Flexible space
    Custom      // Custom view
};

/// Single toolbar item
struct ToolbarItem {
    std::string id;
    std::string label;
    std::string tooltip;
    ToolbarItemType type = ToolbarItemType::Button;
    bool enabled = true;
    bool toggled = false;
    std::function<void()> on_click;
    std::function<void(bool)> on_toggle;
    std::unique_ptr<View> custom_view;
};

/// Toolbar container
class Toolbar : public View {
public:
    enum class Orientation { horizontal, vertical };

    Toolbar() = default;

    /// Add a button item
    void add_button(std::string id, std::string label, std::function<void()> on_click);

    /// Add a toggle item
    void add_toggle(std::string id, std::string label, std::function<void(bool)> on_toggle);

    /// Add a separator
    void add_separator();

    /// Add flexible spacer
    void add_spacer();

    /// Add a custom view
    void add_custom(std::string id, std::unique_ptr<View> view);

    /// Remove an item by ID
    void remove_item(std::string_view id);

    /// Set orientation
    void set_orientation(Orientation o) { orientation_ = o; }
    Orientation orientation() const { return orientation_; }

    /// Item spacing
    void set_spacing(float s) { spacing_ = s; }

    /// Item size (buttons)
    void set_item_size(float s) { item_size_ = s; }

    /// Get/set toggle state by ID
    bool is_toggled(std::string_view id) const;
    void set_toggled(std::string_view id, bool state);

    /// Enable/disable an item by ID
    void set_enabled(std::string_view id, bool enabled);

    int item_count() const { return static_cast<int>(items_.size()); }

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;

    float intrinsic_height() const override {
        return orientation_ == Orientation::horizontal ? item_size_ + 8.0f : 0;
    }

private:
    std::vector<ToolbarItem> items_;
    Orientation orientation_ = Orientation::horizontal;
    float spacing_ = 4.0f;
    float item_size_ = 28.0f;

    int hit_test_item(Point pos) const;
};

}  // namespace pulp::view
