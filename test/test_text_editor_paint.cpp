#include <catch2/catch_test_macros.hpp>
#include "support/text_editor_test_utils.hpp"

#include <pulp/view/frame_clock.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/screenshot_compare.hpp>
#include <pulp/view/theme.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace pulp::view;
using namespace pulp::canvas;
using namespace pulp::test;

namespace {

std::filesystem::path text_editor_validation_dir() {
    auto dir = std::filesystem::path("/tmp/pulp-text-editor-interaction-validation");
    std::filesystem::create_directories(dir);
    return dir;
}

void write_png_artifact(const std::vector<std::uint8_t>& png, std::string name) {
    if (png.empty()) return;
    const auto path = text_editor_validation_dir() / std::move(name);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
}

} // namespace

TEST_CASE("TextEditor caret_rect has a fallback before first paint",
          "[view][text_editor][coverage]") {
    TextEditor editor;
    editor.set_bounds({0, 0, 120, 24});
    editor.set_text("abc");

    auto rect = editor.caret_rect();
    REQUIRE(rect.x >= 9.0f);
    REQUIRE(rect.y == 2.0f);
    REQUIRE(rect.width == 1.5f);
    REQUIRE(rect.height >= 13.0f);
}

TEST_CASE("TextEditor caret X comes from shaped offsets, not summed glyph widths",
          "[view][text_editor][paint]") {
    // Regression guard for the caret/selection-X fix: paint() must populate the layout's
    // x_offsets from canvas.text_x_for_byte() (which a real shaper kerns), not from summing
    // isolated measure_text() advances. ShapedOffsetCanvas makes the two paths diverge by
    // ~500 px so the caret position reveals which one fed it.
    TextEditor editor;
    editor.on_focus_changed(true);              // so set_text leaves the caret at the end
    editor.set_bounds({0, 0, 800, 24});
    editor.set_text("AV");                      // caret now at byte 2

    ShapedOffsetCanvas canvas;
    editor.paint(canvas);

    const auto rect = editor.caret_rect();
    // Shaped offset for byte 2 is 502; a glyph-sum path would put it near 16.
    REQUIRE(rect.x > 400.0f);
}

TEST_CASE("TextEditor paint never measures split UTF-8 byte prefixes",
          "[view][text_editor][paint][utf8]") {
    TextEditor editor;
    editor.on_focus_changed(true);
    editor.set_bounds({0, 0, 800, 30});
    editor.set_text("cafe\xCC\x81 synth");

    StrictUtf8MeasureCanvas canvas;
    REQUIRE_NOTHROW(editor.paint(canvas));
    REQUIRE_NOTHROW(canvas.text_x_for_byte(editor.text(), 5));
}

TEST_CASE("TextEditor multi-line offsets never measure split UTF-8 bytes",
          "[view][text_editor][paint][utf8]") {
    TextEditor editor;
    editor.multi_line = true;
    editor.on_focus_changed(true);
    editor.set_bounds({0, 0, 800, 80});
    editor.set_text("cafe\xCC\x81\nsynth");

    StrictUtf8MeasureCanvas canvas;
    REQUIRE_NOTHROW(editor.paint(canvas));
}

TEST_CASE("TextEditor multi-line paint renders placeholder when unfocused",
          "[view][text_editor][paint][issue-493]") {
    TextEditor editor;
    editor.multi_line = true;
    editor.placeholder = "Type notes";
    editor.set_bounds({0, 0, 180, 64});

    RecordingCanvas canvas;
    editor.paint(canvas);

    bool found = false;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == DrawCommand::Type::fill_text && cmd.text == "Type notes") {
            found = true;
            break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("TextEditor paint produces draw commands", "[view][text_editor]") {
    TextEditor editor;
    editor.set_bounds({0, 0, 200, 30});
    editor.set_text("Paint test");

    RecordingCanvas canvas;
    editor.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) > 0);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) > 0);
}

TEST_CASE("TextEditor paint clamps shell radius and insets the inner fill", "[view][text_editor]") {
    TextEditor editor;
    editor.set_bounds({0, 0, 120, 20});
    editor.set_border(Color::hex(0xFFFFFF), 2.0f, 20.0f);
    editor.set_text("Paint test");

    RecordingCanvas canvas;
    editor.paint(canvas);

    std::vector<DrawCommand> rounded;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == DrawCommand::Type::fill_rounded_rect) rounded.push_back(cmd);
    }

    REQUIRE(rounded.size() >= 2);
    const auto& outer = rounded[0];
    const auto& inner = rounded[1];

    REQUIRE(outer.f[4] <= 9.5f);
    REQUIRE(inner.f[0] == 2.0f);
    REQUIRE(inner.f[1] == 2.0f);
    REQUIRE(inner.f[2] == 116.0f);
    REQUIRE(inner.f[3] == 16.0f);
    REQUIRE(inner.f[4] < outer.f[4]);
}

TEST_CASE("TextEditor paint renders a visible selection highlight and split text", "[view][text_editor]") {
    TextEditor editor;
    editor.set_bounds({0, 0, 180, 28});
    editor.set_text("Select me");
    editor.select_all();

    RecordingCanvas canvas;
    editor.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) >= 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) >= 1);
}

TEST_CASE("TextEditor paint keeps unfocused single-line text anchored to the start", "[view][text_editor]") {
    TextEditor editor;
    editor.set_bounds({0, 0, 120, 26});
    editor.set_text("Some text");

    RecordingCanvas canvas;
    editor.paint(canvas);

    bool found = false;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type != DrawCommand::Type::fill_text || cmd.text != "Some text") continue;
        REQUIRE(cmd.f[0] >= 6.0f);
        REQUIRE(cmd.f[0] <= 20.0f);
        found = true;
        break;
    }
    REQUIRE(found);
}

TEST_CASE("TextEditor paint resets canvas text alignment before drawing", "[view][text_editor]") {
    TextEditor editor;
    editor.set_bounds({0, 0, 120, 26});
    editor.set_text("Some text");

    RecordingCanvas canvas;
    canvas.set_text_align(TextAlign::center);
    editor.paint(canvas);

    bool found = false;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type != DrawCommand::Type::fill_text || cmd.text != "Some text") continue;
        REQUIRE(cmd.f[0] >= 6.0f);
        REQUIRE(cmd.f[0] <= 20.0f);
        found = true;
        break;
    }
    REQUIRE(found);
}

TEST_CASE("TextEditor password mode masks text", "[view][text_editor]") {
    TextEditor editor;
    editor.password_mode = true;
    editor.password_char = '*';
    editor.set_text("secret");
    editor.set_bounds({0, 0, 200, 30});

    RecordingCanvas canvas;
    editor.paint(canvas);

    // Should have rendered but with masked characters
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) > 0);
    bool found_masked = false;
    bool found_plain = false;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.text == "******") found_masked = true;
        if (cmd.text == "secret") found_plain = true;
    }
    REQUIRE(found_masked);
    REQUIRE_FALSE(found_plain);
}

TEST_CASE("TextEditor caret-blink subscription is removed even after detach", "[view][text_editor]") {
    // Regression: the caret-blink frame-clock subscription must be torn down on
    // destruction even when the editor was removed from the view tree first.
    // frame_clock() walks parent_, so a detached editor can't find its clock; the
    // editor caches the clock pointer at subscribe time so it can always
    // unsubscribe. If it doesn't, the clock holds a callback capturing a destroyed
    // `this` and the next tick() is a use-after-free.
    FrameClock clock;

    auto parent = std::make_unique<View>();
    parent->set_frame_clock(&clock);

    auto editor_owned = std::make_unique<TextEditor>();
    TextEditor* editor = editor_owned.get();
    parent->add_child(std::move(editor_owned));

    editor->on_focus_changed(true);                 // subscribes to the clock
    REQUIRE(clock.has_active_subscribers());

    // Detach from the tree BEFORE destruction — now frame_clock() would return null.
    auto detached = parent->remove_child(editor);
    REQUIRE(detached != nullptr);
    detached.reset();                               // destroy the editor

    REQUIRE_FALSE(clock.has_active_subscribers());  // subscription cleaned up
    clock.tick(0.016f);                             // must not touch freed memory
    SUCCEED("tick after destruction did not use freed memory");
}

TEST_CASE("TextEditor caret blink is clock-driven and movement briefly holds solid",
          "[view][text_editor][caret]") {
    FrameClock clock;
    auto parent = std::make_unique<View>();
    parent->set_frame_clock(&clock);

    auto editor_owned = std::make_unique<TextEditor>();
    TextEditor* editor = editor_owned.get();
    editor->set_bounds({0, 0, 200, 30});
    parent->add_child(std::move(editor_owned));

    editor->on_focus_changed(true);
    editor->set_text("blink");

    RecordingCanvas canvas;
    editor->paint(canvas);
    REQUIRE(count_caret_strokes(canvas) == 1);

    clock.tick(0.90f);
    canvas.clear();
    editor->paint(canvas);
    REQUIRE(count_caret_strokes(canvas) == 0);

    REQUIRE(editor->on_key_event(key_event(KeyCode::left)));
    canvas.clear();
    editor->paint(canvas);
    REQUIRE(count_caret_strokes(canvas) == 1);

    clock.tick(0.20f);
    canvas.clear();
    editor->paint(canvas);
    REQUIRE(count_caret_strokes(canvas) == 1);

    clock.tick(0.20f);
    canvas.clear();
    editor->paint(canvas);
    REQUIRE(count_caret_strokes(canvas) == 1);

    clock.tick(0.54f);
    canvas.clear();
    editor->paint(canvas);
    REQUIRE(count_caret_strokes(canvas) == 0);
}

// Regression: a selection must recolor glyphs in place — never move them.
//
// The painter used to draw a selection as three independently-shaped runs
// (before / selected / after). Re-shaping a substring in isolation loses the
// kerning/left-side-bearing context it had inside the full string, so the
// selected glyphs landed at the wrong x — the "gap between the letters in the
// 2nd word" a user sees when dragging a selection across a space and into a
// word. The fix paints the whole string as ONE shaped run, then overlays the
// selected color clipped to the selection rect, so a glyph cannot move just
// because it became selected.
//
// The invariant under test is text-engine-level and design-agnostic. We
// neutralize every selection color (selection fill and selected-text color both
// resolve to colors that paint identically over the background) so that the
// ONLY thing that could change pixels between the unselected and selected
// renders is a glyph moving. A correct painter therefore yields two identical
// frames; the old three-run painter shifts the mid-word selected glyphs and the
// frames diverge.
TEST_CASE("TextEditor selecting mid-word recolors in place without moving glyphs",
          "[view][text_editor][selection][svg]") {
    const std::string kText = "WAVE table mix";   // strong W-A kern; spaces
    constexpr int kW = 240, kH = 30;
    constexpr float kScale = 2.0f;

    // White page, black text. selected-text color resolves from "bg.primary"
    // (black, == text) and the selection fill from "accent.primary" (white, ==
    // page, alpha-blended to a no-op). So selection changes no color — only a
    // moved glyph can change a pixel.
    Theme neutral;
    neutral.colors["text.primary"]   = Color::rgba8(0, 0, 0, 255);
    neutral.colors["bg.primary"]     = Color::rgba8(0, 0, 0, 255);
    neutral.colors["accent.primary"] = Color::rgba8(255, 255, 255, 255);

    auto render = [&](bool select_mid_word) {
        TextEditor editor;
        editor.set_theme(neutral);
        editor.set_background_color(Color::rgba8(255, 255, 255, 255));
        editor.set_bounds({0, 0, float(kW), float(kH)});
        editor.set_text(kText);
        if (select_mid_word) {
            editor.on_focus_changed(true);
            editor.on_key_event(key_event(KeyCode::home));         // caret -> 0
            editor.on_key_event(key_event(KeyCode::right));        // caret -> 1 (after 'W')
            auto shift_right = key_event(KeyCode::right, kModShift);
            for (int i = 0; i < 3; ++i) editor.on_key_event(shift_right);  // select "AVE"
            editor.on_focus_changed(false);   // drop the caret; selection persists
        }
        // Both frames render unfocused: no caret, identical background.
        return render_to_png(editor, kW, kH, kScale, ScreenshotBackend::skia);
    };

    const auto base = render(false);
    if (base.empty()) SKIP("Skia raster screenshot backend unavailable");
    const auto selected = render(true);
    REQUIRE_FALSE(selected.empty());

    // Some partial-Skia lanes no-op raster text; skip rather than false-fail.
    const auto stats = analyze_screenshot_content(base);
    if (!stats.passes_content_floor()) SKIP("native raster unavailable in this build");

    // With colors neutralized, the only source of difference is a moving glyph.
    // Concentrate the signal on the first word ("WAVE", left ~25% of the strip)
    // where the mid-word selection lives; the global frame is mostly the stable
    // tail, which dilutes the metric.
    const uint32_t png_w = static_cast<uint32_t>(kW * kScale);
    const uint32_t png_h = static_cast<uint32_t>(kH * kScale);
    const uint32_t band_w = png_w / 4;   // left quarter: comfortably covers "WAVE"
    const auto base_band = crop_png(base, 0, 0, band_w, png_h);
    const auto sel_band = crop_png(selected, 0, 0, band_w, png_h);
    REQUIRE_FALSE(base_band.empty());
    REQUIRE_FALSE(sel_band.empty());
    const auto cmp = compare_screenshots(base_band, sel_band, /*tolerance=*/4);
    REQUIRE(cmp.valid);
    INFO("band similarity = " << cmp.similarity);
    // A correct painter tiles the width with disjoint clips, so neutralized
    // selection leaves the band pixel-identical. The old three-run painter
    // shifted the selected glyphs, dropping the band well below this floor
    // (~0.95 at the time of the fix).
    CHECK(cmp.similarity >= 0.99f);
}

TEST_CASE("TextEditor interaction harness captures mouse keyboard and text-input states",
          "[view][text_editor][interaction][screenshot]") {
    constexpr uint32_t kW = 420;
    constexpr uint32_t kH = 120;
    constexpr float kScale = 2.0f;

    TextEditor editor;
    editor.on_focus_changed(true);
    editor.set_bounds({0, 0, static_cast<float>(kW), static_cast<float>(kH)});
    editor.set_text("lo-fi house beat, 90 BPM");

    auto capture = [&](const std::string& name) {
        auto result = capture_view(editor, kW, kH, kScale);
        write_png_artifact(result.png, name + ".png");
        INFO(name << " artifact: " << (text_editor_validation_dir() / (name + ".png")).string());
        return result;
    };

    const auto baseline = capture("01-baseline");
    if (!baseline.ok) SKIP("screenshot capture unavailable: " << baseline.reason);
    REQUIRE(analyze_screenshot_content(baseline.png).passes_content_floor());

    auto x_for_byte = [&](int byte_offset) {
        editor.set_caret_pos(byte_offset);
        auto result = capture_view(editor, kW, kH, kScale);
        REQUIRE(result.ok);
        return editor.caret_rect().x;
    };
    auto x_between = [&](int left_byte, int right_byte) {
        return (x_for_byte(left_byte) + x_for_byte(right_byte)) * 0.5f;
    };

    MouseEvent double_click;
    double_click.position = {x_between(12, 13), 18.0f};
    double_click.click_count = 2;
    double_click.is_down = true;
    editor.on_mouse_event(double_click);
    editor.on_mouse_drag({x_between(21, 22), 18.0f});
    REQUIRE(editor.selected_text() == "beat, 90 BPM");

    const auto mouse_selection = capture("02-mouse-word-drag-selection");
    REQUIRE(mouse_selection.ok);
    REQUIRE(analyze_screenshot_content(mouse_selection.png).passes_content_floor());
    auto mouse_diff = compare_screenshots(baseline.png, mouse_selection.png, 8);
    REQUIRE(mouse_diff.valid);
    CHECK(mouse_diff.similarity < 0.995f);

    TextInputEvent replacement;
    replacement.text = "garage";
    editor.on_text_input(replacement);
    REQUIRE(editor.text() == "lo-fi house garage");
    REQUIRE_FALSE(editor.has_selection());

    const auto typed = capture("03-typed-replacement");
    REQUIRE(typed.ok);
    REQUIRE(analyze_screenshot_content(typed.png).passes_content_floor());

    editor.multi_line = true;
    editor.set_text("abcd\nef\nghij");
    editor.set_caret_pos(static_cast<int>(editor.text().size()));
    REQUIRE(editor.on_key_event(key_event(KeyCode::up, kModShift)));
    REQUIRE(editor.selected_text() == "\nghij");

    const auto shift_up = capture("04-shift-up-selection");
    REQUIRE(shift_up.ok);
    REQUIRE(analyze_screenshot_content(shift_up.png).passes_content_floor());
}
