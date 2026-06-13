#pragma once

#include <catch2/catch_test_macros.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/canvas/text_utf8.hpp>
#include <pulp/platform/clipboard.hpp>
#include <pulp/view/text_editor.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

namespace pulp::test {

inline view::KeyEvent key_event(view::KeyCode key, uint16_t modifiers = 0) {
    view::KeyEvent event;
    event.key = key;
    event.modifiers = modifiers;
    event.is_down = true;
    return event;
}

inline uint16_t main_modifier() {
#ifdef __APPLE__
    return view::kModCmd;
#else
    return view::kModCtrl;
#endif
}

inline uint16_t word_modifier() {
#ifdef __APPLE__
    return view::kModAlt;
#else
    return view::kModCtrl;
#endif
}

inline bool seed_system_clipboard_text(std::string_view text) {
    std::string value(text);
    if (!platform::Clipboard::set_text(value)) return false;
    auto roundtrip = platform::Clipboard::get_text();
    return roundtrip.has_value() && *roundtrip == value;
}

inline void require_system_clipboard_text(std::string_view text) {
    if (!seed_system_clipboard_text(text)) {
        SKIP("platform clipboard text backend unavailable");
    }
}

// A canvas whose shaped text_x_for_byte() deliberately diverges from the sum of
// isolated per-glyph measure_text() widths. This catches regressions where
// editor carets/selections are positioned by glyph sums instead of shaped byte
// offsets from the canvas.
struct ShapedOffsetCanvas : canvas::RecordingCanvas {
    float measure_text(const std::string& text) override {
        return 8.0f * static_cast<float>(text.size());
    }

    float text_x_for_byte(const std::string& text, std::size_t byte_index) override {
        return 500.0f + static_cast<float>(std::min(byte_index, text.size()));
    }
};

struct StrictUtf8MeasureCanvas : canvas::RecordingCanvas {
    float measure_text(const std::string& text) override {
        REQUIRE(canvas::safe_utf8_prefix_size(text, text.size()) == text.size());
        return canvas::RecordingCanvas::measure_text(text);
    }

    float text_x_for_byte(const std::string& text, std::size_t byte_index) override {
        const auto prefix_len = canvas::safe_utf8_prefix_size(text, byte_index);
        return measure_text(text.substr(0, prefix_len));
    }
};

inline size_t count_caret_strokes(const canvas::RecordingCanvas& canvas) {
    size_t count = 0;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type != canvas::DrawCommand::Type::stroke_line) continue;
        if (std::abs(cmd.f[0] - cmd.f[2]) < 0.01f && cmd.f[3] > cmd.f[1])
            ++count;
    }
    return count;
}

} // namespace pulp::test
