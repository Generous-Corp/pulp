#pragma once

#include <pulp/view/view.hpp>
#include <functional>
#include <string>
#include <variant>
#include <vector>

namespace pulp::view {

// ── PropertyList ────────────────────────────────────────────────────────────
// Key-value property editor for inspector panels.

class PropertyList : public View {
public:
    /// Property value types.
    using PropertyValue = std::variant<std::string, float, int, bool, Color>;

    struct Property {
        std::string key;
        std::string label;          // Display name
        PropertyValue value;
        bool read_only = false;
        std::string category;       // Optional grouping category
    };

    PropertyList();

    /// Set the list of properties.
    void set_properties(std::vector<Property> props);
    const std::vector<Property>& properties() const { return properties_; }

    /// Update a single property value by key.
    void set_value(const std::string& key, PropertyValue value);

    /// Get a property value by key. Returns nullptr if not found.
    const Property* find_property(const std::string& key) const;

    /// Callback when a property value changes.
    std::function<void(const std::string& key, PropertyValue new_value)> on_change;

    /// Display options
    void set_row_height(float h) { row_height_ = h; }
    float row_height() const { return row_height_; }

    void set_label_width_fraction(float f) { label_width_ = f; }
    float label_width_fraction() const { return label_width_; }

    void set_show_categories(bool show) { show_categories_ = show; }
    bool show_categories() const { return show_categories_; }

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    bool wants_mouse_input() const override { return true; }

    float intrinsic_height() const override;

private:
    std::vector<Property> properties_;
    float row_height_ = 28.0f;
    float label_width_ = 0.4f;
    bool show_categories_ = true;
    int editing_index_ = -1;
};

} // namespace pulp::view
