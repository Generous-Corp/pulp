#include <pulp/view/live_constant_editor.hpp>
#include <pulp/canvas/canvas.hpp>
#include <algorithm>
#include <sstream>

namespace pulp::view {

// ── LiveConstantRegistry ────────────────────────────────────────────────

LiveConstantRegistry& LiveConstantRegistry::instance() {
    static LiveConstantRegistry registry;
    return registry;
}

float& LiveConstantRegistry::register_constant(std::string_view name, std::string_view file,
                                                int line, float default_value,
                                                float min_val, float max_val) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string key(name);

    auto it = constants_.find(key);
    if (it != constants_.end())
        return it->second.value;

    LiveConstant c;
    c.name = key;
    c.file = std::string(file);
    c.line = line;
    c.value = default_value;
    c.default_value = default_value;
    c.min_value = min_val;
    c.max_value = max_val;

    auto [iter, _] = constants_.emplace(key, std::move(c));
    return iter->second.value;
}

std::vector<LiveConstant> LiveConstantRegistry::all() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LiveConstant> result;
    for (auto& [_, c] : constants_)
        result.push_back(c);
    return result;
}

void LiveConstantRegistry::reset_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [_, c] : constants_)
        c.value = c.default_value;
}

void LiveConstantRegistry::reset(std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = constants_.find(name);
    if (it != constants_.end())
        it->second.value = it->second.default_value;
}

void LiveConstantRegistry::set(std::string_view name, float value) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = constants_.find(name);
    if (it != constants_.end()) {
        it->second.value = std::clamp(value, it->second.min_value, it->second.max_value);
        if (on_change) on_change(it->second);
    }
}

float LiveConstantRegistry::get(std::string_view name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = constants_.find(name);
    return it != constants_.end() ? it->second.value : 0.0f;
}

int LiveConstantRegistry::count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(constants_.size());
}

// ── LiveConstantEditor ──────────────────────────────────────────────────

LiveConstantEditor::LiveConstantEditor() {
    set_visible(false);
}

int LiveConstantEditor::hit_test_row(float y) const {
    if (y < 30.0f) return -1;  // Header
    return static_cast<int>((y - 30.0f) / row_height_);
}

void LiveConstantEditor::paint(canvas::Canvas& canvas) {
    float w = bounds().width, h = bounds().height;

    // Semi-transparent overlay background
    canvas.set_fill_color(canvas::Color::rgba(20, 20, 28, 230));
    canvas.fill_rect(0, 0, w, h);

    // Header
    canvas.set_fill_color(canvas::Color::rgba(255, 200, 50));
    canvas.set_font("system", 14.0f);
    canvas.fill_text("Live Constants", 10, 22);

    auto constants = LiveConstantRegistry::instance().all();
    float y = 30.0f;

    for (auto& c : constants) {
        // Name
        canvas.set_fill_color(canvas::Color::rgba(180, 180, 195));
        canvas.set_font("system", 11.0f);
        canvas.fill_text(c.name, 10, y + row_height_ * 0.6f);

        // Slider track
        float slider_x = w * 0.4f;
        float slider_w = w * 0.4f;
        float slider_y = y + row_height_ * 0.4f;

        canvas.set_fill_color(canvas::Color::rgba(50, 50, 60));
        canvas.fill_rounded_rect(slider_x, slider_y, slider_w, 6, 3);

        // Slider fill
        float norm = (c.max_value > c.min_value)
            ? (c.value - c.min_value) / (c.max_value - c.min_value) : 0;
        canvas.set_fill_color(canvas::Color::rgba(100, 160, 255));
        canvas.fill_rounded_rect(slider_x, slider_y, slider_w * norm, 6, 3);

        // Value text
        std::ostringstream ss;
        ss.precision(3);
        ss << std::fixed << c.value;
        canvas.set_fill_color(canvas::Color::rgba(220, 220, 230));
        canvas.fill_text(ss.str(), w * 0.85f, y + row_height_ * 0.6f);

        y += row_height_;
    }
}

void LiveConstantEditor::on_mouse_down(Point pos) {
    dragging_index_ = hit_test_row(pos.y);
}

void LiveConstantEditor::on_mouse_drag(Point pos) {
    if (dragging_index_ < 0) return;

    auto constants = LiveConstantRegistry::instance().all();
    if (dragging_index_ >= static_cast<int>(constants.size())) return;

    float slider_x = bounds().width * 0.4f;
    float slider_w = bounds().width * 0.4f;
    float norm = std::clamp((pos.x - slider_x) / slider_w, 0.0f, 1.0f);

    auto& c = constants[dragging_index_];
    float new_val = c.min_value + norm * (c.max_value - c.min_value);
    LiveConstantRegistry::instance().set(c.name, new_val);
}

}  // namespace pulp::view
