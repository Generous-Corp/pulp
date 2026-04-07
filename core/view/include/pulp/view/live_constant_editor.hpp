#pragma once

// LiveConstantEditor — debug overlay for tweaking numeric constants at runtime.
// Register constants with PULP_LIVE_CONSTANT(value, min, max), then open the
// editor overlay to adjust them without recompiling.

#include <pulp/view/view.hpp>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <mutex>

namespace pulp::view {

/// A single live-editable constant
struct LiveConstant {
    std::string name;
    std::string file;
    int line = 0;
    float value = 0;
    float min_value = 0;
    float max_value = 1;
    float default_value = 0;
};

/// Registry of live constants — thread-safe singleton
class LiveConstantRegistry {
public:
    static LiveConstantRegistry& instance();

    /// Register a constant. Returns a reference to the stored value.
    float& register_constant(std::string_view name, std::string_view file, int line,
                              float default_value, float min_val, float max_val);

    /// Get all registered constants
    std::vector<LiveConstant> all() const;

    /// Reset all constants to defaults
    void reset_all();

    /// Reset a specific constant
    void reset(std::string_view name);

    /// Set a constant value by name
    void set(std::string_view name, float value);

    /// Get a constant value by name
    float get(std::string_view name) const;

    /// Number of registered constants
    int count() const;

    /// Called when any constant changes
    std::function<void(const LiveConstant&)> on_change;

private:
    LiveConstantRegistry() = default;
    mutable std::mutex mutex_;
    std::map<std::string, LiveConstant, std::less<>> constants_;
};

/// Debug overlay that shows sliders for all live constants
class LiveConstantEditor : public View {
public:
    LiveConstantEditor();

    /// Toggle visibility
    void toggle() { set_visible(!visible()); }

    /// Whether the editor is showing
    bool is_showing() const { return visible(); }

    /// Slider height per constant
    void set_row_height(float h) { row_height_ = h; }

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_drag(Point pos) override;

private:
    float row_height_ = 28.0f;
    int dragging_index_ = -1;

    int hit_test_row(float y) const;
};

}  // namespace pulp::view

// Macro for declaring live constants
// Usage: float radius = PULP_LIVE_CONSTANT(8.0f, 0.0f, 50.0f);
#define PULP_LIVE_CONSTANT(default_val, min_val, max_val) \
    (::pulp::view::LiveConstantRegistry::instance().register_constant( \
        #default_val, __FILE__, __LINE__, default_val, min_val, max_val))
