#pragma once

// Web-compat test helpers — shared utilities for CSS/layout/event/visual testing

#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/runtime/system.hpp>
#include <pulp/state/store.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace pulp::test {

using namespace pulp::view;
using namespace pulp::canvas;

// ── Test environment ─────────────────────────────────────────────────────────

struct TestEnvironment {
    View root;
    ScriptEngine engine;
    state::StateStore store;
    std::unique_ptr<WidgetBridge> bridge;

    TestEnvironment(float w = 400, float h = 300) {
        root.set_bounds({0, 0, w, h});
        root.set_theme(Theme::dark());
        bridge = std::make_unique<WidgetBridge>(engine, root, store);
    }

    // Execute JS and run layout
    void run(const std::string& js) {
        bridge->load_script(js);
        root.layout_children();
    }

    // Just execute JS (no layout)
    void eval(const std::string& js) {
        bridge->load_script(js);
    }

    // Get widget by ID
    View* widget(const std::string& id) { return bridge->widget(id); }
};

// ── View factory helpers ─────────────────────────────────────────────────────

inline std::unique_ptr<View> make_box(float w, float h) {
    auto v = std::make_unique<View>();
    v->flex().preferred_width = w;
    v->flex().preferred_height = h;
    return v;
}

inline std::unique_ptr<View> make_flex_row(float w, float h) {
    auto v = std::make_unique<View>();
    v->set_bounds({0, 0, w, h});
    v->flex().direction = FlexDirection::row;
    return v;
}

inline std::unique_ptr<View> make_flex_col(float w, float h) {
    auto v = std::make_unique<View>();
    v->set_bounds({0, 0, w, h});
    v->flex().direction = FlexDirection::column;
    return v;
}

// ── Image comparison ─────────────────────────────────────────────────────────

enum class Tolerance {
    exact,   // 0 pixels different
    tight,   // < 0.1% different, max channel delta 2
    loose,   // < 1% different, max channel delta 5
};

struct CompareResult {
    bool passed = false;
    float diff_percent = 0;      // % of pixels that differ
    int max_channel_delta = 0;   // Largest single-channel difference
    std::vector<uint8_t> diff_image; // Red overlay on differences (PNG)
};

// Compare two PNG buffers
inline CompareResult compare_images(
    const std::vector<uint8_t>& actual,
    const std::vector<uint8_t>& expected,
    Tolerance tol = Tolerance::tight
) {
    CompareResult result;

    // Both empty = trivially pass
    if (actual.empty() && expected.empty()) {
        result.passed = true;
        return result;
    }

    // Size mismatch = fail
    if (actual.size() != expected.size()) {
        result.diff_percent = 100.0f;
        return result;
    }

    // Compare raw bytes (PNG-level comparison — works for identical render paths)
    size_t diff_count = 0;
    for (size_t i = 0; i < actual.size(); ++i) {
        int delta = std::abs(static_cast<int>(actual[i]) - static_cast<int>(expected[i]));
        if (delta > 0) diff_count++;
        result.max_channel_delta = std::max(result.max_channel_delta, delta);
    }

    result.diff_percent = (static_cast<float>(diff_count) / static_cast<float>(actual.size())) * 100.0f;

    switch (tol) {
        case Tolerance::exact:
            result.passed = (diff_count == 0);
            break;
        case Tolerance::tight:
            result.passed = (result.diff_percent < 0.1f && result.max_channel_delta <= 2);
            break;
        case Tolerance::loose:
            result.passed = (result.diff_percent < 1.0f && result.max_channel_delta <= 5);
            break;
    }

    return result;
}

// ── Screenshot helpers ───────────────────────────────────────────────────────

inline std::vector<uint8_t> render_view(View& root, uint32_t w, uint32_t h) {
    return render_to_png(root, w, h, 1.0f);
}

inline bool save_screenshot(View& root, uint32_t w, uint32_t h, const std::string& path) {
    return render_to_file(root, w, h, path, 1.0f);
}

// ── Baseline management ──────────────────────────────────────────────────────

inline std::filesystem::path baseline_dir() {
    // Platform-aware baseline directory
#if defined(__aarch64__) || defined(__arm64__)
    return std::filesystem::path(PULP_TEST_SOURCE_DIR) / "reftests" / "baselines" / "macos-arm64";
#elif defined(__APPLE__)
    return std::filesystem::path(PULP_TEST_SOURCE_DIR) / "reftests" / "baselines" / "macos-x64";
#elif defined(__linux__)
    return std::filesystem::path(PULP_TEST_SOURCE_DIR) / "reftests" / "baselines" / "linux-x64";
#else
    return std::filesystem::path(PULP_TEST_SOURCE_DIR) / "reftests" / "baselines" / "unknown";
#endif
}

inline std::vector<uint8_t> load_baseline(const std::string& name) {
    auto path = baseline_dir() / (name + ".png");
    if (!std::filesystem::exists(path)) return {};

    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

inline bool save_baseline(const std::string& name, const std::vector<uint8_t>& data) {
    auto dir = baseline_dir();
    std::filesystem::create_directories(dir);
    auto path = dir / (name + ".png");

    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return file.good();
}

// ── Regression check ─────────────────────────────────────────────────────────

struct RegressionResult {
    bool passed = false;
    bool baseline_missing = false;
    CompareResult compare;
};

inline RegressionResult regression_check(const std::string& name, const std::vector<uint8_t>& screenshot) {
    RegressionResult result;
    auto expected = load_baseline(name);

    if (expected.empty()) {
        result.baseline_missing = true;
        // Auto-save if env var set
        if (auto update = runtime::get_env("PULP_UPDATE_BASELINES"); update && *update == "1") {
            save_baseline(name, screenshot);
            result.passed = true;
        }
        return result;
    }

    result.compare = compare_images(screenshot, expected, Tolerance::tight);
    result.passed = result.compare.passed;
    return result;
}

// ── Layout assertion helpers ─────────────────────────────────────────────────

// Check bounds within tolerance (default 1px for rounding)
inline bool bounds_match(const Rect& actual, float x, float y, float w, float h, float tol = 1.0f) {
    return std::abs(actual.x - x) <= tol &&
           std::abs(actual.y - y) <= tol &&
           std::abs(actual.width - w) <= tol &&
           std::abs(actual.height - h) <= tol;
}

// ── Event simulation helpers ─────────────────────────────────────────────────

inline void simulate_mouse_enter(View& root, float x, float y) {
    // Find the view at that position and call on_mouse_enter
    View* target = root.hit_test({x, y});
    if (target) target->on_mouse_enter();
}

inline void simulate_mouse_leave(View& root, float x, float y) {
    View* target = root.hit_test({x, y});
    if (target) target->on_mouse_leave();
}

inline void simulate_key(View& target, int key_code, bool ctrl = false, bool shift = false, bool alt = false) {
    KeyEvent ke;
    ke.key = static_cast<KeyCode>(key_code);
    ke.modifiers = 0;
    if (ctrl)  ke.modifiers |= kModCtrl;
    if (shift) ke.modifiers |= kModShift;
    if (alt)   ke.modifiers |= kModAlt;
    ke.is_down = true;
    target.on_key_event(ke);
}

} // namespace pulp::test
