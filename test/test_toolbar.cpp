#include <catch2/catch_test_macros.hpp>
#include <pulp/view/toolbar.hpp>
#include <pulp/view/widgets.hpp>   // Label
#include <pulp/canvas/canvas.hpp>

#include <memory>
#include <string>

using namespace pulp::view;
using namespace pulp::canvas;

namespace {
bool has_glyph(const RecordingCanvas& canvas, const std::string& g) {
    for (const auto& c : canvas.commands())
        if (c.type == DrawCommand::Type::fill_text && c.text == g) return true;
    return false;
}
// Minimal custom view that records whether it received a mouse-down.
struct ClickSpy : View {
    bool clicked = false;
    void on_mouse_down(Point) override { clicked = true; }
};
}  // namespace

TEST_CASE("Toolbar paints multibyte glyph labels without splitting a codepoint",
          "[view][toolbar]") {
    Toolbar tb;
    tb.add_button("play", "\xe2\x96\xb6", [] {});  // ▶ (3-byte UTF-8)
    tb.add_button("ascii", "Play", [] {});
    tb.set_bounds({0, 0, 200, 36});

    RecordingCanvas canvas;
    tb.paint(canvas);                              // must not abort on invalid UTF-8
    REQUIRE(has_glyph(canvas, "\xe2\x96\xb6"));    // whole glyph, not a 2-byte split
    REQUIRE(has_glyph(canvas, "Pl"));              // ASCII path unchanged (first 2 chars)
}

TEST_CASE("Toolbar hit-tests items positioned after a wide custom item",
          "[view][toolbar]") {
    Toolbar tb;
    auto wide = std::make_unique<Label>("120 BPM");
    wide->flex().preferred_width = 70.0f;          // wider than the square item_size_
    tb.add_custom("bpm", std::move(wide));
    bool fired = false;
    tb.add_button("next", "N", [&] { fired = true; });
    tb.set_bounds({0, 0, 200, 36});

    // The custom item spans [4, 74); the button therefore starts at ~78. A
    // click at x=85 lands on the button only if paint/hit-test honor the
    // custom item's full width (not the square item_size_ = 28).
    tb.on_mouse_down({85.0f, 18.0f});
    REQUIRE(fired);
}

TEST_CASE("Toolbar forwards clicks to a custom view", "[view][toolbar]") {
    Toolbar tb;
    auto spy = std::make_unique<ClickSpy>();
    ClickSpy* raw = spy.get();
    spy->flex().preferred_width = 70.0f;
    tb.add_custom("spy", std::move(spy));
    tb.set_bounds({0, 0, 200, 36});

    tb.on_mouse_down({20.0f, 18.0f});              // within the custom item
    REQUIRE(raw->clicked);
}

TEST_CASE("Toolbar toggle reports active state", "[view][toolbar]") {
    Toolbar tb;
    tb.add_toggle("loop", "Loop", [](bool) {});
    REQUIRE_FALSE(tb.is_toggled("loop"));
    tb.set_toggled("loop", true);
    REQUIRE(tb.is_toggled("loop"));
}
