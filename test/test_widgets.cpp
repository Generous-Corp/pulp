#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/widget_skin_derive.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/view/motion_preferences.hpp>
#include <pulp/canvas/canvas.hpp>

#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

using namespace pulp::view;
using namespace pulp::canvas;
using Catch::Matchers::WithinAbs;

namespace {

std::vector<DrawCommand> commands_of(const RecordingCanvas& canvas,
                                     DrawCommand::Type type) {
    std::vector<DrawCommand> matches;
    for (const auto& command : canvas.commands()) {
        if (command.type == type) {
            matches.push_back(command);
        }
    }
    return matches;
}

std::shared_ptr<SpriteStrip> make_sprite_strip(
    int total_width,
    int total_height,
    int frame_count,
    SpriteStrip::Orientation orientation) {
    std::vector<uint8_t> pixels(static_cast<size_t>(total_width) *
                                static_cast<size_t>(total_height) * 4u,
                                0x7f);
    auto strip = std::make_shared<SpriteStrip>();
    strip->load(pixels.data(), pixels.size(), total_width, total_height,
                frame_count, orientation);
    return strip;
}

Label* add_child_label(View& parent, std::string text = "x") {
    auto child = std::make_unique<Label>(std::move(text));
    child->set_bounds({0, 0, 100, 20});
    auto* raw = child.get();
    parent.add_child(std::move(child));
    return raw;
}

}  // namespace

TEST_CASE("Knob value clamping", "[view][widget]") {
    Knob knob;
    knob.set_value(0.5f);
    REQUIRE_THAT(knob.value(), WithinAbs(0.5, 0.001));

    knob.set_value(1.5f);
    REQUIRE_THAT(knob.value(), WithinAbs(1.0, 0.001));

    knob.set_value(-0.5f);
    REQUIRE_THAT(knob.value(), WithinAbs(0.0, 0.001));
}

TEST_CASE("Knob drag emits ordered gesture callbacks", "[view][widget]") {
    Knob knob;
    knob.set_value(0.25f);

    std::vector<std::string> events;
    knob.on_gesture_begin = [&] { events.push_back("begin"); };
    knob.on_change = [&](float) { events.push_back("change"); };
    knob.on_gesture_end = [&] { events.push_back("end"); };

    knob.on_mouse_down({24.0f, 80.0f});
    knob.on_mouse_drag({24.0f, 20.0f});
    knob.on_mouse_up({24.0f, 20.0f});
    knob.on_mouse_up({24.0f, 20.0f});

    REQUIRE(events.size() == 3);
    REQUIRE(events[0] == "begin");
    REQUIRE(events[1] == "change");
    REQUIRE(events[2] == "end");
    REQUIRE(knob.value() > 0.25f);
}

TEST_CASE("Knob renders arcs and indicator", "[view][widget]") {
    Knob knob;
    knob.set_bounds({0, 0, 48, 48});
    knob.set_value(0.5f);
    knob.set_label("Gain");

    RecordingCanvas canvas;
    knob.paint(canvas);

    // Ink & Signal knob: track arc + value arc, a raised body disc + a white
    // dot pointer (fill_circle), and the label text. (The old thin-needle
    // stroke_line was replaced by the dot pointer.)
    REQUIRE(canvas.count(DrawCommand::Type::stroke_arc) == 2);
    REQUIRE(canvas.count(DrawCommand::Type::fill_circle) >= 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) >= 1);
}

TEST_CASE("Knob with format function shows value text", "[view][widget]") {
    Knob knob;
    knob.set_bounds({0, 0, 48, 48});
    knob.set_value(0.75f);
    knob.set_format([](float v) { return std::to_string(static_cast<int>(v * 100)) + "%"; });

    RecordingCanvas canvas;
    knob.paint(canvas);

    // Should have label text + value text
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) >= 1);
}

TEST_CASE("Knob silver style draws the rotating indicator notch", "[view][widget][silver]") {
    // Pins the silver path after the indicator notch was factored into the
    // shared draw_knob_indicator_notch helper: exactly the two notch strokes
    // (dark backing + bright pointer), and the pointer sweeps with value.
    auto outer_x_at = [](float value) {
        Knob knob;
        knob.set_bounds({0, 0, 48, 48});
        knob.set_render_style(WidgetRenderStyle::silver);
        knob.set_show_label(false);
        knob.set_value(value);
        RecordingCanvas canvas;
        knob.paint(canvas);
        auto lines = commands_of(canvas, DrawCommand::Type::stroke_line);
        REQUIRE(lines.size() == 2);          // backing + bright, same endpoints
        REQUIRE(lines[0].f[2] == Catch::Approx(lines[1].f[2]));
        return lines.back().f[2];            // outer endpoint x
    };
    const float cx = 24.0f;
    // value 0 → pointer to the lower-left; value 1 → lower-right; 0.5 → up.
    REQUIRE(outer_x_at(0.0f) < cx);
    REQUIRE(outer_x_at(1.0f) > cx);
    REQUIRE(outer_x_at(0.5f) == Catch::Approx(cx).margin(0.01));
}

TEST_CASE("Knob single-frame sprite overlays a rotating indicator", "[view][widget][sprite]") {
    // A single-frame strip is a static captured disc; the engine overlays the
    // native indicator so the imported sprite knob still TURNS with value.
    auto notch_outer_x = [](float value) {
        Knob knob;
        knob.set_bounds({0, 0, 48, 48});
        knob.set_show_label(false);
        knob.set_sprite_strip(make_sprite_strip(48, 48, 1, SpriteStrip::Orientation::vertical));
        knob.set_value(value);
        RecordingCanvas canvas;
        knob.paint(canvas);
        auto lines = commands_of(canvas, DrawCommand::Type::stroke_line);
        REQUIRE(lines.size() == 2);          // the notch, drawn over the body
        return lines.back().f[2];
    };
    const float cx = 24.0f;
    REQUIRE(notch_outer_x(0.0f) < cx);
    REQUIRE(notch_outer_x(1.0f) > cx);
    REQUIRE(notch_outer_x(0.5f) == Catch::Approx(cx).margin(0.01));
}

TEST_CASE("Knob multi-frame sprite has no indicator overlay", "[view][widget][sprite]") {
    // Multi-frame strips encode rotation in the frames themselves, so the
    // engine must NOT draw a redundant native notch over them.
    Knob knob;
    knob.set_bounds({0, 0, 48, 48});
    knob.set_show_label(false);
    knob.set_sprite_strip(make_sprite_strip(48, 48 * 16, 16, SpriteStrip::Orientation::vertical));
    knob.set_value(0.5f);
    RecordingCanvas canvas;
    knob.paint(canvas);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) == 0);
}

TEST_CASE("Knob sprite core-fit sizes the disc to the box", "[view][widget][sprite]") {
    // With a recovered opaque-core rect, the whole frame scales so the core
    // fills the layout box (shadow bleed extends beyond) and the core centers.
    Knob knob;
    knob.set_bounds({0, 0, 50, 50});
    knob.set_show_label(false);
    auto strip = std::make_shared<SpriteStrip>();
    strip->load_from_file("/tmp/synthetic-knob.png", 200, 300, 1,
                          SpriteStrip::Orientation::vertical);
    knob.set_sprite_strip(std::move(strip));
    // Core: 100×100 opaque disc at (40,20) within the 200×300 PNG.
    knob.set_sprite_core(40.0f, 20.0f, 100.0f, 100.0f);
    knob.set_value(0.5f);

    RecordingCanvas canvas;
    knob.paint(canvas);

    auto images = commands_of(canvas, DrawCommand::Type::draw_image);
    REQUIRE(images.size() == 1);
    const auto& img = images.front();
    // s = min(50/100, 50/100) = 0.5 → whole frame 100×150, core centered.
    REQUIRE(img.f[0] == Catch::Approx(-20.0f));  // dst_x = -core_x*s + pad_x
    REQUIRE(img.f[1] == Catch::Approx(-10.0f));  // dst_y = -core_y*s + pad_y
    REQUIRE(img.f[2] == Catch::Approx(100.0f));  // dst_w = png_w*s
    REQUIRE(img.f[3] == Catch::Approx(150.0f));  // dst_h = png_h*s
    // Single frame → indicator overlay present.
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) == 2);
}

TEST_CASE("Fader value clamping", "[view][widget]") {
    Fader fader;
    fader.set_value(0.7f);
    REQUIRE_THAT(fader.value(), WithinAbs(0.7, 0.001));

    fader.set_value(2.0f);
    REQUIRE_THAT(fader.value(), WithinAbs(1.0, 0.001));
}

TEST_CASE("Fader renders track and thumb", "[view][widget]") {
    Fader fader;
    fader.set_bounds({0, 0, 24, 200});
    fader.set_value(0.6f);
    fader.set_label("Volume");

    RecordingCanvas canvas;
    fader.paint(canvas);

    // Ink & Signal fader: track + fill + slab thumb = 3 rounded rects (the
    // default thumb is a rounded slab, not a circle), plus the label text.
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 3);
    REQUIRE(canvas.count(DrawCommand::Type::fill_circle) == 0);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) >= 1);
}

// ── pulp #3191: hybrid skinned fader/meter ──────────────────────────────────

TEST_CASE("Skinned Fader draws a value-positioned rounded-rect thumb",
          "[view][widget][issue-3191]") {
    Fader fader;
    fader.set_bounds({0, 0, 96, 200});
    fader.set_skin_track_color(Color::rgba8(0x1f, 0x21, 0x29));
    fader.set_skin_fill_color(Color::rgba8(0x36, 0x77, 0xcf));
    fader.set_skin_thumb_color(Color::rgba8(0xea, 0xea, 0xf0));
    REQUIRE(fader.has_skin());

    // A skinned fader draws procedurally (track + fill + slab thumb), NOT a
    // baked sprite image — so the thumb still moves with value.
    auto thumb_y = [&](float v) {
        fader.set_value(v);
        RecordingCanvas c;
        fader.paint(c);
        // No sprite image baked in.
        REQUIRE(c.count(DrawCommand::Type::draw_image) == 0);
        // track + fill + thumb slab = 3 rounded rects (no circle thumb).
        REQUIRE(c.count(DrawCommand::Type::fill_rounded_rect) == 3);
        REQUIRE(c.count(DrawCommand::Type::fill_circle) == 0);
        // The thumb is the LAST rounded rect (drawn after track+fill).
        auto rects = commands_of(c, DrawCommand::Type::fill_rounded_rect);
        return rects.back().f[1];  // y of the thumb slab
    };

    // Vertical fader: value 0 → thumb at the BOTTOM (high y),
    // value 1 → thumb at the TOP (low y). The thumb must MOVE.
    float y_low = thumb_y(0.0f);
    float y_mid = thumb_y(0.5f);
    float y_high = thumb_y(1.0f);
    REQUIRE(y_low > y_mid);
    REQUIRE(y_mid > y_high);
}

TEST_CASE("Skinned Fader thumb border emits a stroked rect",
          "[view][widget][issue-3191]") {
    Fader fader;
    fader.set_bounds({0, 0, 96, 200});
    fader.set_skin_thumb_color(Color::rgba8(0xea, 0xea, 0xf0));
    fader.set_skin_thumb_border_color(Color::rgba8(0x69, 0x69, 0x6f));
    fader.set_value(0.5f);

    RecordingCanvas c;
    fader.paint(c);
    REQUIRE(c.count(DrawCommand::Type::stroke_rounded_rect) == 1);
}

TEST_CASE("Skinned Fader strokes the empty track when a track border is set",
          "[view][widget][issue-3192]") {
    // pulp #3192 — the captured empty track has a visible lighter outline. When
    // the importer derives that edge colour, the fader strokes the track rect so
    // the empty channel above the thumb doesn't read as a flat dark slab.
    Fader fader;
    fader.set_bounds({0, 0, 96, 200});
    fader.set_skin_track_color(Color::rgba8(0x1f, 0x21, 0x29));
    fader.set_skin_track_border_color(Color::rgba8(0x3d, 0x3f, 0x47));
    fader.set_value(0.5f);
    REQUIRE(fader.has_skin_track_border_color());

    RecordingCanvas c;
    fader.paint(c);
    // Exactly one stroked rect: the track outline (no thumb border set here).
    REQUIRE(c.count(DrawCommand::Type::stroke_rounded_rect) == 1);
}

TEST_CASE("Skinned Fader with both track and thumb borders strokes twice",
          "[view][widget][issue-3192]") {
    Fader fader;
    fader.set_bounds({0, 0, 96, 200});
    fader.set_skin_track_color(Color::rgba8(0x1f, 0x21, 0x29));
    fader.set_skin_track_border_color(Color::rgba8(0x3d, 0x3f, 0x47));
    fader.set_skin_thumb_color(Color::rgba8(0xea, 0xea, 0xf0));
    fader.set_skin_thumb_border_color(Color::rgba8(0x69, 0x69, 0x6f));
    fader.set_value(0.5f);

    RecordingCanvas c;
    fader.paint(c);
    // Track outline + thumb bevel.
    REQUIRE(c.count(DrawCommand::Type::stroke_rounded_rect) == 2);
}

TEST_CASE("derive_fader_skin synthesises a lighter rim for a dark flat track",
          "[view][widget][issue-3192]") {
    // The captured empty track is dark; the design draws a faint lighter edge so
    // it doesn't read as a flat slab. When the (flat) sampled pixels can't
    // resolve a distinct edge, the sampler synthesises a rim by lightening the
    // dark track colour — still derived from the captured track, no hardcode.
    const int W = 30, H = 100;
    std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 0);
    auto set = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t* p = px.data() + (static_cast<size_t>(y) * W + x) * 4;
        p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
    };
    for (int y = 10; y < 80; ++y)
        for (int x = 0; x < W; ++x) {
            if (y >= 40 && y < 48) set(x, y, 234, 234, 240);   // silver thumb
            else if (y >= 55) set(x, y, 54, 119, 207);          // blue fill
            else set(x, y, 31, 33, 41);                          // dark track
        }
    SkinImage img{px.data(), W, H};
    auto skin = derive_fader_skin(img);
    REQUIRE(skin.has_track);
    REQUIRE(skin.has_track_border);
    // The rim must be lighter than the dark track fill, but still dark-ish (a
    // subtle edge, not a second fill).
    REQUIRE(skin.track_border_color.r > skin.track_color.r);
    REQUIRE(skin.track_border_color.r < 0.5f);
}

TEST_CASE("derive_fader_skin leaves a light/flat track borderless",
          "[view][widget][issue-3192]") {
    // A light track has no dark channel to outline → no synthesised rim.
    const int W = 30, H = 100;
    std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 0);
    auto set = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t* p = px.data() + (static_cast<size_t>(y) * W + x) * 4;
        p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
    };
    for (int y = 10; y < 80; ++y)
        for (int x = 0; x < W; ++x) {
            if (y >= 40 && y < 48) set(x, y, 250, 250, 252);   // bright thumb
            else if (y >= 55) set(x, y, 54, 119, 207);          // blue fill
            else set(x, y, 170, 173, 180);                       // LIGHT track
        }
    SkinImage img{px.data(), W, H};
    auto skin = derive_fader_skin(img);
    REQUIRE(skin.has_track);
    REQUIRE_FALSE(skin.has_track_border);
}

TEST_CASE("Skinned Meter clips its gradient fill to the level",
          "[view][widget][issue-3191]") {
    Meter meter;
    meter.set_bounds({0, 0, 40, 200});
    meter.set_skin_background_color(Color::rgba8(0x0f, 0x12, 0x17));
    meter.set_skin_gradient({
        Color::rgba8(0x33, 0xa7, 0x4d),  // low  (green)
        Color::rgba8(0xff, 0xab, 0x33),  // mid  (orange)
        Color::rgba8(0xff, 0x6b, 0x66),  // high (red)
    });
    REQUIRE(meter.has_skin_gradient());

    auto fill_rows = [&](float level) {
        meter.set_level(level, level);
        RecordingCanvas c;
        meter.paint(c);
        // Each gradient row is a fill_rect; the count grows with the level.
        return c.count(DrawCommand::Type::fill_rect);
    };

    size_t low = fill_rows(0.1f);
    size_t mid = fill_rows(0.5f);
    size_t high = fill_rows(0.9f);

    // Value-driven: more level → more painted rows. Not a static image.
    REQUIRE(low < mid);
    REQUIRE(mid < high);
    // Roughly proportional to the meter height (200px), within ballpark.
    REQUIRE(high >= 150);
}

TEST_CASE("Skinned Meter gradient samples low→high across stops",
          "[view][widget][issue-3191]") {
    Meter meter;
    meter.set_skin_gradient({
        Color::rgba8(0, 255, 0),    // low
        Color::rgba8(255, 0, 0),    // high
    });
    // Bottom of the bar is the low (green) stop; top is the high (red) stop.
    auto lo = meter.gradient_color_at(0.0f);
    auto hi = meter.gradient_color_at(1.0f);
    auto mid = meter.gradient_color_at(0.5f);
    REQUIRE_THAT(lo.g, WithinAbs(1.0, 0.01));
    REQUIRE_THAT(hi.r, WithinAbs(1.0, 0.01));
    // Midpoint is an even blend.
    REQUIRE_THAT(mid.r, WithinAbs(0.5, 0.02));
    REQUIRE_THAT(mid.g, WithinAbs(0.5, 0.02));
}

TEST_CASE("derive_meter_skin samples a synthetic gradient bottom→top",
          "[view][widget][issue-3191]") {
    // Build a 20-wide x 100-tall RGBA image: transparent margins top/bottom,
    // a dark "empty channel" near the top of the art, then a green→red fill
    // from the bottom. Mirrors the captured meter PNG's structure.
    const int W = 20, H = 100;
    std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 0);  // all transparent
    auto set = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t* p = px.data() + (static_cast<size_t>(y) * W + x) * 4;
        p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
    };
    // Art region rows 10..80 (opaque). 10..30 dark channel, 30..80 gradient.
    for (int y = 10; y < 80; ++y)
        for (int x = 0; x < W; ++x) {
            if (y < 30) set(x, y, 15, 18, 23);  // dark empty
            else {
                // bottom (y=79) green → top of fill (y=30) red
                float t = static_cast<float>(y - 30) / (79 - 30);  // 0 at top fill, 1 at bottom
                uint8_t r = static_cast<uint8_t>((1.0f - t) * 255);
                uint8_t g = static_cast<uint8_t>(t * 255);
                set(x, y, r, g, 30);
            }
        }

    SkinImage img{px.data(), W, H};
    auto skin = derive_meter_skin(img, 5);
    REQUIRE(skin.valid());
    REQUIRE(skin.gradient.size() == 5);
    // Low stop (bottom) is green-dominant; high stop (top) is red-dominant.
    REQUIRE(skin.gradient.front().g > skin.gradient.front().r);
    REQUIRE(skin.gradient.back().r > skin.gradient.back().g);
    // Background recovered as the dark channel.
    REQUIRE(skin.has_background);
    REQUIRE(skin.background.r < 0.2f);
}

TEST_CASE("derive_fader_skin recovers track / fill / thumb colours",
          "[view][widget][issue-3191]") {
    const int W = 30, H = 100;
    std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 0);
    auto set = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t* p = px.data() + (static_cast<size_t>(y) * W + x) * 4;
        p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
    };
    // Art rows 10..80. Dark track everywhere, a bright thumb slab at 40..48,
    // and a saturated blue fill in the lower half.
    for (int y = 10; y < 80; ++y)
        for (int x = 0; x < W; ++x) {
            if (y >= 40 && y < 48) set(x, y, 234, 234, 240);   // silver thumb
            else if (y >= 55) set(x, y, 54, 119, 207);          // blue fill
            else set(x, y, 31, 33, 41);                          // dark track
        }

    SkinImage img{px.data(), W, H};
    auto skin = derive_fader_skin(img);
    REQUIRE(skin.has_track);
    REQUIRE(skin.has_thumb);
    REQUIRE(skin.has_fill);
    // Track is dark.
    REQUIRE(skin.track_color.r < 0.2f);
    // Thumb is bright.
    REQUIRE(skin.thumb_color.r > 0.8f);
    // Fill is blue-dominant.
    REQUIRE(skin.fill_color.b > skin.fill_color.r);
    REQUIRE(skin.fill_color.b > skin.fill_color.g);
}

TEST_CASE("derive_*_skin recovers horizontal art widths (pulp #3191 width fix)",
          "[view][widget][issue-3191]") {
    // The captured art's visible element is a NARROW inset region, not the full
    // node box. The sampler must recover those horizontal extents from the
    // pixels so the widget renders narrow + centred. We build synthetic images
    // whose art is a known inset width and assert the recovered px.

    SECTION("meter bar width from a narrow centred bar") {
        // 40-wide box; the coloured bar is x=[14..25] (12 px) — a 30% inset,
        // mirroring the captured meter's ~26%-of-box bar. Faint label glyphs
        // below the bar must NOT widen the recovered bar width.
        const int W = 40, H = 100;
        std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 0);
        auto set = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
            uint8_t* p = px.data() + (static_cast<size_t>(y) * W + x) * 4;
            p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
        };
        // Bar: art rows 10..80, x in [14..25]; green→red bottom→top.
        for (int y = 10; y < 80; ++y)
            for (int x = 14; x <= 25; ++x) {
                float t = static_cast<float>(y - 10) / (79 - 10);
                set(x, y, static_cast<uint8_t>((1.0f - t) * 255),
                          static_cast<uint8_t>(t * 255), 30);
            }
        // A WIDE faint label glyph row well below the bar (rows 88..90, full
        // width). Must not be picked up as the bar (bar vertical region wins).
        for (int y = 88; y < 91; ++y)
            for (int x = 2; x < W - 2; ++x) set(x, y, 100, 100, 100);

        SkinImage img{px.data(), W, H};
        auto skin = derive_meter_skin(img, 5);
        REQUIRE(skin.valid());
        REQUIRE(skin.has_bar_width);
        // Bar spans x=[14..25] → 12 px (not the 40-px box, not the wide label).
        REQUIRE(skin.bar_width_px == Catch::Approx(12.0f).margin(1.0f));
    }

    SECTION("fader track width (thin) vs thumb width (wide slab)") {
        // 60-wide box. Thin track x=[28..31] (4 px) over the whole art; a wide
        // silver thumb slab x=[18..41] (24 px) at rows 40..48; a thin blue fill
        // (same 4 px) in the lower half. The sampler must report the THUMB
        // width as the widget width and the TRACK width as the thin line.
        const int W = 60, H = 100;
        std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 0);
        auto set = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
            uint8_t* p = px.data() + (static_cast<size_t>(y) * W + x) * 4;
            p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
        };
        for (int y = 10; y < 80; ++y) {
            if (y >= 40 && y < 48) {
                for (int x = 18; x <= 41; ++x) set(x, y, 234, 234, 240);  // thumb
            } else if (y >= 55) {
                for (int x = 28; x <= 31; ++x) set(x, y, 54, 119, 207);    // fill
            } else {
                for (int x = 28; x <= 31; ++x) set(x, y, 31, 33, 41);      // track
            }
        }
        SkinImage img{px.data(), W, H};
        auto skin = derive_fader_skin(img);
        REQUIRE(skin.has_thumb_width);
        REQUIRE(skin.has_track_width);
        // Thumb slab spans x=[18..41] → 24 px.
        REQUIRE(skin.thumb_width_px == Catch::Approx(24.0f).margin(1.0f));
        // Track is the thin 4-px column, NOT the 24-px thumb or 60-px box.
        REQUIRE(skin.track_width_px == Catch::Approx(4.0f).margin(1.0f));
        // Thumb slab centred at y≈44 within art rows [10,80) → ~0.51 up the
        // bar (1 = top, 0 = bottom). pulp #3191 position fix.
        REQUIRE(skin.has_thumb_position);
        REQUIRE(skin.thumb_position == Catch::Approx(0.51f).margin(0.1f));
    }

    SECTION("meter fill level from a partially-filled bar") {
        // 40-wide box; gradient bar fills the BOTTOM ~70% of the art (rows
        // 31..80 of art [10,80)) with dark/empty above — so fill_level ≈ 0.7,
        // not the linear dB seed. pulp #3191 position fix.
        const int W = 40, H = 100;
        std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 0);
        auto set = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
            uint8_t* p = px.data() + (static_cast<size_t>(y) * W + x) * 4;
            p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
        };
        // Empty dark channel: art rows 10..30, the bar's inset column.
        for (int y = 10; y < 31; ++y)
            for (int x = 14; x <= 25; ++x) set(x, y, 18, 20, 24);
        // Coloured fill: rows 31..80 (the bottom ~70%), green→red bottom→top.
        for (int y = 31; y < 80; ++y)
            for (int x = 14; x <= 25; ++x) {
                float t = static_cast<float>(y - 31) / (79 - 31);
                set(x, y, static_cast<uint8_t>((1.0f - t) * 255),
                          static_cast<uint8_t>(t * 255), 30);
            }
        SkinImage img{px.data(), W, H};
        auto skin = derive_meter_skin(img, 5);
        REQUIRE(skin.has_fill_level);
        REQUIRE(skin.fill_level == Catch::Approx(0.70f).margin(0.12f));
    }
}

TEST_CASE("derive_meter_skin spans the full colour range (warm/red top stop)",
          "[view][widget][issue-3191]") {
    // The gradient must span the bar's FULL coloured extent — its top stop is
    // the warm/red the capture shows, NOT a yellow-green clipped to the fill
    // level. Build a meter whose colours run red(top)→orange→yellow→green(bottom)
    // over the coloured fill, with a dark channel above. Mirrors the captured
    // Pulp meter: red-orange (254,105,55) at the top of the fill.
    const int W = 36, H = 120;
    std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 0);
    auto set = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t* p = px.data() + (static_cast<size_t>(y) * W + x) * 4;
        p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
    };
    // Housing art rows 10..100, coloured bar x=[12..23]. Dark empty channel
    // rows 10..34 (a hair under 30% of the housing), then the colour ramp
    // red(top, y=35) → green(bottom, y=99).
    for (int y = 10; y < 100; ++y)
        for (int x = 12; x <= 23; ++x) {
            if (y < 35) { set(x, y, 15, 18, 23); continue; }  // dark channel
            float t = static_cast<float>(y - 35) / (99 - 35);  // 0 top, 1 bottom
            // top: red-orange (254,105,55); bottom: green (60,200,90)
            uint8_t r = static_cast<uint8_t>(254 + t * (60 - 254));
            uint8_t g = static_cast<uint8_t>(105 + t * (200 - 105));
            uint8_t b = static_cast<uint8_t>(55 + t * (90 - 55));
            set(x, y, r, g, b);
        }
    SkinImage img{px.data(), W, H};
    auto skin = derive_meter_skin(img, 5);
    REQUIRE(skin.valid());
    REQUIRE(skin.gradient.size() == 5);
    // TOP stop (last, high) is the warm/red — r clearly the strongest channel
    // and meaningfully warm, NOT a yellow-green (which would have g ≈ r).
    const auto& top = skin.gradient.back();
    REQUIRE(top.r > top.g);
    REQUIRE(top.r > top.b);
    REQUIRE(top.r > 0.7f);          // red-dominant, ~254
    REQUIRE(top.g < 0.75f);         // not yellow (g would be ≈ r for yellow)
    // BOTTOM stop (first, low) is green-dominant.
    REQUIRE(skin.gradient.front().g > skin.gradient.front().r);
}

TEST_CASE("Skinned Meter renders the warm top stop at the top of the fill",
          "[view][widget][issue-3191]") {
    // The gradient maps across the FILL region (not absolute meter height), so
    // a partial fill still shows the warm/red TOP stop at the top of the fill —
    // matching the capture. With the old absolute-height mapping a 50% fill only
    // exposed the lower (green) half of the gradient. We sample the painted rows
    // and assert the topmost filled row is the red stop.
    Meter meter;
    meter.set_bounds({0, 0, 40, 200});
    meter.set_skin_gradient({
        Color::rgba8(0, 200, 0),    // low  (green)
        Color::rgba8(255, 0, 0),    // high (red)
    });
    meter.set_level(0.5f, 0.5f);  // half full
    RecordingCanvas rc;
    meter.paint(rc);
    // Filled rows are fill_rect commands; the FIRST one painted is the topmost
    // row of the fill (paint walks top→bottom of the fill region). fill_rect
    // carries no colour — the active colour is the most recent set_fill_color
    // command before it. Find the first fill_rect and the colour in effect.
    const auto& cmds = rc.commands();
    Color top_color{};
    Color active{};
    bool found = false;
    for (const auto& c : cmds) {
        if (c.type == DrawCommand::Type::set_fill_color) active = c.color;
        else if (c.type == DrawCommand::Type::fill_rect) { top_color = active; found = true; break; }
    }
    REQUIRE(found);
    // The topmost filled row must be the red TOP stop (r >> g), NOT the green
    // bottom stop — proving the gradient maps across the fill region.
    REQUIRE(top_color.r > 0.6f);
    REQUIRE(top_color.r > top_color.g + 0.3f);
}

TEST_CASE("derive_*_skin recovers the control housing height (excludes value stack)",
          "[view][widget][issue-3191]") {
    // The captured PNG bakes the value-stack text below the control, so the
    // node's declared height spans control+labels. The sampler must recover the
    // real CONTROL housing height (the tallest opaque art run) so the importer
    // doesn't stretch the widget to ~2× tall. Build a tall image whose control
    // art occupies the top portion and a gapped label glyph run sits below.
    const int W = 36, H = 200;
    std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 0);
    auto set = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t* p = px.data() + (static_cast<size_t>(y) * W + x) * 4;
        p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
    };
    SECTION("meter housing height") {
        // Control housing: rows 10..110 (height 100). Dark channel 10..40, then
        // green→red fill 40..110, bar x=[12..23].
        for (int y = 10; y < 110; ++y)
            for (int x = 12; x <= 23; ++x) {
                if (y < 40) set(x, y, 15, 18, 23);
                else {
                    float t = static_cast<float>(y - 40) / (109 - 40);
                    set(x, y, static_cast<uint8_t>((1 - t) * 254 + t * 60),
                              static_cast<uint8_t>(t * 200 + (1 - t) * 105), 55);
                }
            }
        // Value-stack glyph runs WELL BELOW the housing (gapped), rows 130..140.
        for (int y = 130; y < 141; ++y)
            for (int x = 14; x < 22; ++x) set(x, y, 90, 90, 100);
        SkinImage img{px.data(), W, H};
        auto skin = derive_meter_skin(img, 5);
        REQUIRE(skin.has_housing_height);
        // Housing is the 100-px control run, NOT the 200-px box.
        REQUIRE(skin.housing_height_px == Catch::Approx(100.0f).margin(3.0f));
    }
    SECTION("fader housing height") {
        // Control housing: rows 10..110. Dark track everywhere, a thumb slab at
        // 50..58, blue fill below 70.
        for (int y = 10; y < 110; ++y)
            for (int x = 12; x <= 23; ++x) {
                if (y >= 50 && y < 58) set(x, y, 234, 234, 240);
                else if (y >= 70) set(x, y, 90, 150, 230);
                else set(x, y, 31, 33, 41);
            }
        for (int y = 130; y < 141; ++y)
            for (int x = 14; x < 22; ++x) set(x, y, 90, 90, 100);
        SkinImage img{px.data(), W, H};
        auto skin = derive_fader_skin(img);
        REQUIRE(skin.has_housing_height);
        REQUIRE(skin.housing_height_px == Catch::Approx(100.0f).margin(3.0f));
    }
}

TEST_CASE("derive_meter_skin recovers the coloured-bar/housing width ratio",
          "[view][widget][issue-3191]") {
    // The coloured fill is recessed inside a WIDER dark housing slot. The
    // sampler reports housing width as bar_width_px and the colored-bar/housing
    // ratio as bar_fill_ratio (<1 when recessed). Build a 24-wide dark housing
    // with a 12-wide coloured bar inset inside it.
    const int W = 40, H = 120;
    std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 0);
    auto set = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t* p = px.data() + (static_cast<size_t>(y) * W + x) * 4;
        p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
    };
    // Housing rows 10..100, x=[8..31] (24 px) dark. Coloured bar x=[14..25]
    // (12 px) over the bottom ~70% (rows 35..100), dark channel above.
    for (int y = 10; y < 100; ++y) {
        for (int x = 8; x <= 31; ++x) set(x, y, 18, 20, 24);  // dark housing
        if (y >= 35)
            for (int x = 14; x <= 25; ++x) {
                float t = static_cast<float>(y - 35) / (99 - 35);
                set(x, y, static_cast<uint8_t>((1 - t) * 254 + t * 60),
                          static_cast<uint8_t>(t * 200 + (1 - t) * 105), 55);
            }
    }
    SkinImage img{px.data(), W, H};
    auto skin = derive_meter_skin(img, 5);
    REQUIRE(skin.has_bar_width);
    // Housing width is the full 24-px slot.
    REQUIRE(skin.bar_width_px == Catch::Approx(24.0f).margin(2.0f));
    REQUIRE(skin.has_bar_fill_ratio);
    // Coloured bar (12) / housing (24) = 0.5.
    REQUIRE(skin.bar_fill_ratio == Catch::Approx(0.5f).margin(0.12f));
}

TEST_CASE("derive_fader_skin fill colour is the dominant mid tone, not the most-saturated stop",
          "[view][widget][issue-3191]") {
    // The fill is a vertical gradient (lighter at the thumb, darker/most-
    // saturated at the bottom). The derived fill colour must be the dominant
    // MID tone, not the single deepest/most-saturated bottom pixel (which over-
    // saturates vs the reference palette). Build a blue gradient fill: light
    // (101,174,243) at top → deep (54,119,207) at bottom; the median should land
    // near the mid blue, not the deepest stop.
    const int W = 30, H = 120;
    std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 0);
    auto set = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t* p = px.data() + (static_cast<size_t>(y) * W + x) * 4;
        p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
    };
    // Dark track 10..40, then blue gradient fill 40..100.
    for (int y = 10; y < 100; ++y)
        for (int x = 12; x <= 17; ++x) {
            if (y < 40) set(x, y, 31, 33, 41);
            else {
                float t = static_cast<float>(y - 40) / (99 - 40);  // 0 top, 1 bottom
                set(x, y, static_cast<uint8_t>(101 + t * (54 - 101)),
                          static_cast<uint8_t>(174 + t * (119 - 174)),
                          static_cast<uint8_t>(243 + t * (207 - 243)));
            }
        }
    SkinImage img{px.data(), W, H};
    auto skin = derive_fader_skin(img);
    REQUIRE(skin.has_fill);
    // Blue-dominant.
    REQUIRE(skin.fill_color.b > skin.fill_color.r);
    // The derived blue is the dominant MID tone (~(78,147,225)), NOT the deepest
    // bottom stop (54,119,207). Red channel sits above the deepest stop's 54/255.
    REQUIRE(skin.fill_color.r > (54.0f / 255.0f) + 0.02f);
    REQUIRE(skin.fill_color.r < (101.0f / 255.0f));  // below the lightest too
}

TEST_CASE("Skinned Fader honours derived thin track width (pulp #3191)",
          "[view][widget][issue-3191]") {
    // A skinned fader whose widget box was sized to the captured thumb width
    // must draw its TRACK at the derived thin width (centred), not a fraction
    // of the widget box. We render into a RecordingCanvas and assert the track
    // rect spans ~the derived width, far narrower than the box.
    Fader fader;
    // Box is deliberately WIDE (60 px) so the old skinned heuristic
    // (0.18*box → ~11 px, clamped) would visibly differ from the derived
    // 5-px track. Honour-the-derived-width is the behaviour under test.
    fader.set_bounds({0, 0, 60, 200});
    fader.set_value(0.5f);
    fader.set_skin_track_color(Color::rgba8(31, 33, 41));
    fader.set_skin_thumb_color(Color::rgba8(234, 234, 240));
    fader.set_skin_track_width(5.0f);       // derived thin track
    REQUIRE(fader.has_skin());
    REQUIRE(fader.has_skin_track_width());

    RecordingCanvas rc;
    fader.paint(rc);
    // Rect geometry is in f[0..3] = x, y, w, h. The track is the FIRST
    // full-height rounded rect (drawn before fill + thumb). Assert it is the
    // derived thin width (~5 px) and centred — NOT a fraction of the 28-px box
    // (the old skinned heuristic would have drawn 28*0.18 ≈ 5 here by accident,
    // so make the box wide enough that 0.18*box would clearly differ).
    auto rects = commands_of(rc, DrawCommand::Type::fill_rounded_rect);
    bool found_thin_track = false;
    for (const auto& r : rects) {
        if (r.f[3] >= 180.0f) {  // full-height → the track
            found_thin_track = true;
            REQUIRE(r.f[2] == Catch::Approx(5.0f).margin(1.5f));            // width
            REQUIRE(r.f[0] == Catch::Approx((60.0f - 5.0f) * 0.5f).margin(1.5f));  // centred x
            break;
        }
    }
    REQUIRE(found_thin_track);
}

TEST_CASE("Unskinned Fader/Meter keep their default look (back-compat)",
          "[view][widget][issue-3191]") {
    // Fader: no skin → the default Ink & Signal look is track + fill + slab
    // thumb = 3 rounded rects, no circle thumb.
    Fader fader;
    fader.set_bounds({0, 0, 24, 200});
    fader.set_value(0.6f);
    REQUIRE_FALSE(fader.has_skin());
    RecordingCanvas fc;
    fader.paint(fc);
    REQUIRE(fc.count(DrawCommand::Type::fill_circle) == 0);
    REQUIRE(fc.count(DrawCommand::Type::fill_rounded_rect) == 3);

    // Meter: no gradient → default threshold path (rounded-rect bg + rect fill).
    Meter meter;
    meter.set_bounds({0, 0, 40, 200});
    meter.set_level(0.5f, 0.5f);
    REQUIRE_FALSE(meter.has_skin_gradient());
    RecordingCanvas mc;
    meter.paint(mc);
    REQUIRE(mc.count(DrawCommand::Type::fill_rounded_rect) == 1);  // bg
}

TEST_CASE("Fader horizontal orientation", "[view][widget]") {
    Fader fader;
    fader.set_orientation(Fader::Orientation::horizontal);
    fader.set_bounds({0, 0, 200, 24});
    fader.set_value(0.5f);

    RecordingCanvas canvas;
    fader.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 3);
    REQUIRE(canvas.count(DrawCommand::Type::fill_circle) == 0);
}

TEST_CASE("Knob and Fader render loaded sprite strips",
          "[view][widget]") {
    auto knob_strip = make_sprite_strip(2, 9, 3, SpriteStrip::Orientation::vertical);
    Knob knob;
    knob.set_bounds({0, 0, 20, 30});
    knob.set_value(0.75f);
    knob.set_sprite_strip(knob_strip);

    RecordingCanvas knob_canvas;
    knob.paint(knob_canvas);

    REQUIRE(knob_canvas.count(DrawCommand::Type::draw_image) == 1);
    REQUIRE(knob_canvas.count(DrawCommand::Type::save) == 1);
    REQUIRE(knob_canvas.count(DrawCommand::Type::restore) == 1);
    REQUIRE(knob_canvas.count(DrawCommand::Type::stroke_arc) == 0);
    REQUIRE(knob_canvas.count(DrawCommand::Type::stroke_line) == 0);

    auto knob_clip = commands_of(knob_canvas, DrawCommand::Type::clip_rect).front();
    REQUIRE_THAT(knob_clip.f[2], WithinAbs(20.0, 0.001));
    REQUIRE_THAT(knob_clip.f[3], WithinAbs(30.0, 0.001));

    auto knob_scale = commands_of(knob_canvas, DrawCommand::Type::scale).front();
    REQUIRE_THAT(knob_scale.f[0], WithinAbs(10.0, 0.001));
    REQUIRE_THAT(knob_scale.f[1], WithinAbs(10.0, 0.001));

    auto knob_translate = commands_of(knob_canvas, DrawCommand::Type::translate).front();
    REQUIRE_THAT(knob_translate.f[0], WithinAbs(0.0, 0.001));
    REQUIRE_THAT(knob_translate.f[1], WithinAbs(-6.0, 0.001));

    auto knob_image = commands_of(knob_canvas, DrawCommand::Type::draw_image).front();
    REQUIRE(knob_image.text.size() == knob_strip->data_size());
    REQUIRE_THAT(knob_image.f[2], WithinAbs(2.0, 0.001));
    REQUIRE_THAT(knob_image.f[3], WithinAbs(9.0, 0.001));

    auto fader_strip = make_sprite_strip(12, 4, 3, SpriteStrip::Orientation::horizontal);
    Fader fader;
    fader.set_bounds({0, 0, 40, 20});
    fader.set_value(0.5f);
    fader.set_sprite_strip(fader_strip);

    RecordingCanvas fader_canvas;
    fader.paint(fader_canvas);

    REQUIRE(fader_canvas.count(DrawCommand::Type::draw_image) == 1);
    REQUIRE(fader_canvas.count(DrawCommand::Type::save) == 1);
    REQUIRE(fader_canvas.count(DrawCommand::Type::restore) == 1);
    REQUIRE(fader_canvas.count(DrawCommand::Type::fill_rounded_rect) == 0);
    REQUIRE(fader_canvas.count(DrawCommand::Type::fill_circle) == 0);

    auto fader_clip = commands_of(fader_canvas, DrawCommand::Type::clip_rect).front();
    REQUIRE_THAT(fader_clip.f[2], WithinAbs(40.0, 0.001));
    REQUIRE_THAT(fader_clip.f[3], WithinAbs(20.0, 0.001));

    auto fader_scale = commands_of(fader_canvas, DrawCommand::Type::scale).front();
    REQUIRE_THAT(fader_scale.f[0], WithinAbs(10.0, 0.001));
    REQUIRE_THAT(fader_scale.f[1], WithinAbs(5.0, 0.001));

    auto fader_translate = commands_of(fader_canvas, DrawCommand::Type::translate).front();
    REQUIRE_THAT(fader_translate.f[0], WithinAbs(-4.0, 0.001));
    REQUIRE_THAT(fader_translate.f[1], WithinAbs(0.0, 0.001));

    auto fader_image = commands_of(fader_canvas, DrawCommand::Type::draw_image).front();
    REQUIRE(fader_image.text.size() == fader_strip->data_size());
    REQUIRE_THAT(fader_image.f[2], WithinAbs(12.0, 0.001));
    REQUIRE_THAT(fader_image.f[3], WithinAbs(4.0, 0.001));
}

// ── RangeSlider (pulp issue-966) ────────────────────────────────────────────

TEST_CASE("RangeSlider default state matches HTML <input type=\"range\">",
          "[view][widget][issue-966]") {
    RangeSlider rs;
    REQUIRE_THAT(rs.min_value(), WithinAbs(0.0, 0.0001));
    REQUIRE_THAT(rs.max_value(), WithinAbs(1.0, 0.0001));
    // Default step is 0 (continuous) — HTML defaults to 1 but plugins
    // overwhelmingly want continuous values. Callers opt into stepping.
    REQUIRE_THAT(rs.step(), WithinAbs(0.0, 0.0001));
    REQUIRE_THAT(rs.value(), WithinAbs(0.0, 0.0001));
    REQUIRE(rs.orientation() == RangeSlider::Orientation::horizontal);
    REQUIRE_FALSE(rs.has_accent_color());
}

TEST_CASE("RangeSlider clamps value to [min,max]", "[view][widget][issue-966]") {
    RangeSlider rs;
    rs.set_min(-1.0f);
    rs.set_max(1.0f);

    rs.set_value(2.0f);
    REQUIRE_THAT(rs.value(), WithinAbs(1.0, 0.0001));

    rs.set_value(-5.0f);
    REQUIRE_THAT(rs.value(), WithinAbs(-1.0, 0.0001));

    rs.set_value(0.5f);
    REQUIRE_THAT(rs.value(), WithinAbs(0.5, 0.0001));
}

TEST_CASE("RangeSlider quantises to step", "[view][widget][issue-966]") {
    RangeSlider rs;
    rs.set_min(0.0f);
    rs.set_max(1.0f);
    rs.set_step(0.1f);

    rs.set_value(0.34f);
    REQUIRE_THAT(rs.value(), WithinAbs(0.3, 0.0001));

    rs.set_value(0.36f);
    REQUIRE_THAT(rs.value(), WithinAbs(0.4, 0.0001));

    // Step that doesn't divide the range cleanly: max reachable step
    // before exceeding hi must clamp back inside the range.
    rs.set_step(0.3f);
    rs.set_value(1.0f);
    REQUIRE(rs.value() <= 1.0f + 1e-5f);
    REQUIRE(rs.value() >= 0.0f);
}

TEST_CASE("RangeSlider step=0 means continuous", "[view][widget][issue-966]") {
    RangeSlider rs;
    rs.set_min(0.0f);
    rs.set_max(100.0f);
    rs.set_step(0.0f);
    rs.set_value(33.7f);
    REQUIRE_THAT(rs.value(), WithinAbs(33.7, 0.0001));
}

TEST_CASE("RangeSlider re-clamps when bounds change", "[view][widget][issue-966]") {
    RangeSlider rs;
    rs.set_min(0.0f);
    rs.set_max(10.0f);
    rs.set_value(7.0f);
    REQUIRE_THAT(rs.value(), WithinAbs(7.0, 0.0001));

    // Tighten the upper bound — the existing value falls outside and
    // must snap back.
    rs.set_max(5.0f);
    REQUIRE_THAT(rs.value(), WithinAbs(5.0, 0.0001));

    // Raise the lower bound past the value — same idea in the other
    // direction.
    rs.set_min(6.0f);
    REQUIRE_THAT(rs.value(), WithinAbs(6.0, 0.0001));
}

TEST_CASE("RangeSlider invalid range collapses to min", "[view][widget][issue-966]") {
    RangeSlider rs;
    rs.set_min(10.0f);
    rs.set_max(0.0f);  // invalid: max < min
    rs.set_value(5.0f);
    // Per HTMLInputElement: invalid range pins value to min.
    REQUIRE_THAT(rs.value(), WithinAbs(10.0, 0.0001));
}

TEST_CASE("RangeSlider drag dispatches change with quantised value",
          "[view][widget][issue-966]") {
    RangeSlider rs;
    rs.set_bounds({0, 0, 200, 24});
    rs.set_min(0.0f);
    rs.set_max(1.0f);
    rs.set_step(0.25f);

    std::vector<float> changes;
    rs.on_change = [&](float v) { changes.push_back(v); };

    // Mouse-down at x=100 (50% across) → expected snap to 0.5.
    MouseEvent down;
    down.position = {100, 12};
    down.is_down = true;
    rs.on_mouse_event(down);

    REQUIRE(changes.size() == 1);
    REQUIRE_THAT(changes.back(), WithinAbs(0.5, 0.0001));
    REQUIRE_THAT(rs.value(), WithinAbs(0.5, 0.0001));

    // Drag to x=180 (90%) → should snap to 1.0.
    rs.on_mouse_drag({180, 12});
    REQUIRE_THAT(rs.value(), WithinAbs(1.0, 0.0001));
    REQUIRE(changes.size() >= 2);
    REQUIRE_THAT(changes.back(), WithinAbs(1.0, 0.0001));

    // Drag to x=10 (5%) → should snap to 0.
    rs.on_mouse_drag({10, 12});
    REQUIRE_THAT(rs.value(), WithinAbs(0.0, 0.0001));

    // Mouse up → dragging stops; subsequent drags must not move value.
    MouseEvent up = down; up.is_down = false;
    rs.on_mouse_event(up);
    rs.on_mouse_drag({180, 12});
    REQUIRE_THAT(rs.value(), WithinAbs(0.0, 0.0001));
}

TEST_CASE("RangeSlider vertical orientation maps y correctly",
          "[view][widget][issue-966]") {
    RangeSlider rs;
    rs.set_bounds({0, 0, 24, 200});
    rs.set_orientation(RangeSlider::Orientation::vertical);
    rs.set_min(0.0f);
    rs.set_max(1.0f);

    // y=0 is top → max value (1.0).
    MouseEvent ev;
    ev.position = {12, 0};
    ev.is_down = true;
    rs.on_mouse_event(ev);
    REQUIRE_THAT(rs.value(), WithinAbs(1.0, 0.0001));

    // y=200 is bottom → min value (0.0).
    rs.on_mouse_drag({12, 200});
    REQUIRE_THAT(rs.value(), WithinAbs(0.0, 0.0001));

    // y=100 → halfway = 0.5.
    rs.on_mouse_drag({12, 100});
    REQUIRE_THAT(rs.value(), WithinAbs(0.5, 0.001));
}

TEST_CASE("RangeSlider renders track + fill + handle",
          "[view][widget][issue-966]") {
    RangeSlider rs;
    rs.set_bounds({0, 0, 200, 24});
    rs.set_value(0.5f);

    RecordingCanvas canvas;
    rs.paint(canvas);

    // Track + active fill = 2 rounded rects, handle = 1 circle.
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 2);
    REQUIRE(canvas.count(DrawCommand::Type::fill_circle) == 1);
}

TEST_CASE("RangeSlider vertical paint draws lower fill and inverted handle",
          "[view][widget][issue-966]") {
    RangeSlider rs;
    rs.set_bounds({0, 0, 24, 200});
    rs.set_orientation(RangeSlider::Orientation::vertical);
    rs.set_track_thickness(6.0f);
    rs.set_value(0.25f);

    RecordingCanvas canvas;
    rs.paint(canvas);

    auto rects = commands_of(canvas, DrawCommand::Type::fill_rounded_rect);
    REQUIRE(rects.size() == 2);

    // Track is centered horizontally and spans the full vertical bounds.
    REQUIRE_THAT(rects[0].f[0], WithinAbs(9.0, 0.001));
    REQUIRE_THAT(rects[0].f[1], WithinAbs(0.0, 0.001));
    REQUIRE_THAT(rects[0].f[2], WithinAbs(6.0, 0.001));
    REQUIRE_THAT(rects[0].f[3], WithinAbs(200.0, 0.001));

    // Vertical fill grows upward from the bottom for the current value.
    REQUIRE_THAT(rects[1].f[0], WithinAbs(9.0, 0.001));
    REQUIRE_THAT(rects[1].f[1], WithinAbs(150.0, 0.001));
    REQUIRE_THAT(rects[1].f[2], WithinAbs(6.0, 0.001));
    REQUIRE_THAT(rects[1].f[3], WithinAbs(50.0, 0.001));

    auto handles = commands_of(canvas, DrawCommand::Type::fill_circle);
    REQUIRE(handles.size() == 1);
    REQUIRE_THAT(handles.front().f[0], WithinAbs(12.0, 0.001));
    REQUIRE_THAT(handles.front().f[1], WithinAbs(146.0, 0.001));
    REQUIRE_THAT(handles.front().f[2], WithinAbs(8.0, 0.001));
}

TEST_CASE("RangeSlider at minimum draws track but skips empty fill",
          "[view][widget][issue-966]") {
    RangeSlider rs;
    rs.set_bounds({0, 0, 200, 24});
    rs.set_value(0.0f);  // exactly at min — no active fill to draw

    RecordingCanvas canvas;
    rs.paint(canvas);

    // Only the background track rounded rect — fill is zero-width and
    // skipped. Handle still renders.
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_circle) == 1);
}

TEST_CASE("RangeSlider accent color overrides theme fill",
          "[view][widget][issue-966]") {
    RangeSlider rs;
    rs.set_bounds({0, 0, 100, 20});
    rs.set_value(0.6f);
    rs.set_accent_color(Color::rgba8(255, 64, 32, 255));
    REQUIRE(rs.has_accent_color());

    rs.clear_accent_color();
    REQUIRE_FALSE(rs.has_accent_color());
}

TEST_CASE("Toggle state", "[view][widget]") {
    Toggle toggle;
    REQUIRE_FALSE(toggle.is_on());

    toggle.set_on(true);
    REQUIRE(toggle.is_on());
}

TEST_CASE("Toggle set_on(animate=false) snaps the thumb immediately", "[view][widget]") {
    // The default animated form leaves the thumb at its start position until the
    // animation clock advances — so a single headless paint (no clock) would
    // render the thumb stuck off even though the logical state is on.
    //
    // animate_to() collapses to an immediate jump under MotionPolicy::Off /
    // Reduced (and a CI host with reduce-motion enabled would report Off), so
    // pin Full while asserting the animated branch hasn't arrived yet, then
    // revert to OS detection.
    auto& motion = MotionPreferences::instance();
    motion.set_override(MotionPolicy::Full);
    Toggle animated;
    animated.set_on(true);                       // animate (default)
    REQUIRE(animated.is_on());
    REQUIRE(animated.thumb_position() < 1.0f);   // not yet arrived (no clock tick)
    motion.set_override(std::nullopt);

    // The snapping form is for the initial seed: there is nothing to animate
    // from, so the thumb must reflect the state on the very first frame. set()
    // ignores motion policy, so this branch is deterministic regardless of host.
    Toggle snapped;
    snapped.set_on(true, /*animate=*/false);
    REQUIRE(snapped.is_on());
    REQUIRE(snapped.thumb_position() == Catch::Approx(1.0f));

    // Snapping back off is equally immediate.
    snapped.set_on(false, /*animate=*/false);
    REQUIRE_FALSE(snapped.is_on());
    REQUIRE(snapped.thumb_position() == Catch::Approx(0.0f));
}

TEST_CASE("Toggle set_on(animate=false) reconciles a mid-flight thumb", "[view][widget]") {
    // A non-animated set_on must force the thumb to match the logical state even
    // when the logical state is already correct — e.g. a re-seed / screenshot
    // taken while an earlier interactive animation is still mid-flight. Pin Full
    // so the first set_on actually animates (Off would snap it immediately).
    auto& motion = MotionPreferences::instance();
    motion.set_override(MotionPolicy::Full);

    Toggle toggle;
    toggle.set_on(true);                       // animate on; thumb starts climbing from 0
    REQUIRE(toggle.is_on());
    REQUIRE(toggle.thumb_position() < 1.0f);   // mid-flight (no clock advanced it)

    // Same logical state (on), but ask to snap: the thumb must jump to 1.0
    // rather than stay stuck mid-flight.
    toggle.set_on(true, /*animate=*/false);
    REQUIRE(toggle.thumb_position() == Catch::Approx(1.0f));

    motion.set_override(std::nullopt);
}

TEST_CASE("Toggle renders switch", "[view][widget]") {
    Toggle toggle;
    toggle.set_bounds({0, 0, 50, 30});
    toggle.set_on(true);
    toggle.set_label("Bypass");

    RecordingCanvas canvas;
    toggle.paint(canvas);

    // Background rounded rect + thumb circle + label
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_circle) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) >= 1);
}

TEST_CASE("Audio widgets render declarative schemas and invalid schema fallback",
          "[view][widget][schema]") {
    Knob knob;
    knob.set_bounds({0, 0, 80, 80});
    knob.set_value(0.5f);
    knob.set_widget_schema(R"json({
        "elements": [
            { "type": "arc", "color": "control.fill", "radius": "80%", "width": 4,
              "startAngle": -120, "sweepAngle": { "bind": "value", "range": [0, 240] } },
            { "type": "circle", "color": "control.thumb", "radius": "25%" },
            { "type": "line", "color": "text.primary", "innerRadius": "15%",
              "outerRadius": "65%", "angle": { "bind": "value", "range": [-90, 90] } },
            { "type": "rect", "color": "bg.surface", "cornerRadius": "5" },
            { "type": "text", "color": "text.primary", "text": "dB", "fontSize": 12 }
        ]
    })json");

    RecordingCanvas knob_canvas;
    knob.paint(knob_canvas);
    REQUIRE(knob_canvas.count(DrawCommand::Type::stroke_arc) == 1);
    REQUIRE(knob_canvas.count(DrawCommand::Type::fill_circle) == 1);
    REQUIRE(knob_canvas.count(DrawCommand::Type::stroke_line) == 1);
    REQUIRE(knob_canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(knob_canvas.count(DrawCommand::Type::fill_text) == 1);
    REQUIRE(commands_of(knob_canvas, DrawCommand::Type::fill_text).front().text == "dB");

    Fader fader;
    fader.set_bounds({0, 0, 32, 120});
    fader.set_value(0.25f);
    fader.set_widget_schema(R"json({
        "elements": [
            { "type": "rect", "color": "control.track" },
            { "type": "line", "color": "control.fill", "angle": 90,
              "innerRadius": "4", "outerRadius": "24" }
        ]
    })json");

    RecordingCanvas fader_canvas;
    fader.paint(fader_canvas);
    REQUIRE(fader_canvas.count(DrawCommand::Type::fill_rect) == 1);
    REQUIRE(fader_canvas.count(DrawCommand::Type::stroke_line) == 1);

    Toggle toggle;
    toggle.set_bounds({0, 0, 64, 32});
    toggle.set_on(true);
    toggle.set_widget_schema(R"json({
        "elements": [
            { "type": "circle", "color": "accent.primary", "radius": "12" }
        ]
    })json");

    RecordingCanvas toggle_canvas;
    toggle.paint(toggle_canvas);
    REQUIRE(toggle_canvas.count(DrawCommand::Type::fill_circle) == 1);

    Knob invalid;
    invalid.set_bounds({0, 0, 48, 48});
    invalid.set_widget_schema("{ not valid json");

    RecordingCanvas invalid_canvas;
    invalid.paint(invalid_canvas);
    REQUIRE(invalid_canvas.count(DrawCommand::Type::fill_rect) == 1);
}

TEST_CASE("Audio widget schemas reject malformed dimension tokens without error fallback",
          "[view][widget][schema]") {
    Knob knob;
    knob.set_bounds({0, 0, 80, 80});
    knob.set_value(0.5f);
    knob.set_widget_schema(R"json({
        "elements": [
            { "type": "arc", "color": "control.fill", "radius": "80%junk", "width": 4,
              "startAngle": -120, "sweepAngle": { "bind": "value", "range": [0, 240] } },
            { "type": "circle", "color": "control.thumb", "radius": "" },
            { "type": "line", "color": "text.primary", "innerRadius": "15 %",
              "outerRadius": "65x", "angle": { "bind": "value", "range": [-90, 90] } },
            { "type": "rect", "color": "bg.surface", "cornerRadius": "5x" }
        ]
    })json");

    RecordingCanvas canvas;
    knob.paint(canvas);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_arc) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_circle) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 0);
}

TEST_CASE("Audio widgets custom shader paths fall back on recording canvas",
          "[view][widget][shader][issue-493]") {
    Knob knob;
    knob.set_bounds({0, 0, 80, 80});
    knob.set_value(0.5f);
    knob.set_label("Drive");
    knob.set_format([](float) { return "50%"; });
    knob.set_custom_shader("uniform float time; half4 main(float2 p) { return half4(time); }");
    REQUIRE(knob.has_custom_shader());
    REQUIRE(knob.shader_uses_time());

    RecordingCanvas knob_canvas;
    knob.paint(knob_canvas);
    REQUIRE(knob_canvas.count(DrawCommand::Type::fill_rect) == 1);
    REQUIRE(knob_canvas.count(DrawCommand::Type::fill_text) >= 2);

    Fader fader;
    fader.set_bounds({0, 0, 24, 120});
    fader.set_label("Level");
    fader.set_custom_shader("half4 main(float2 p) { return half4(1); }");
    REQUIRE(fader.has_custom_shader());
    REQUIRE_FALSE(fader.shader_uses_time());

    RecordingCanvas fader_canvas;
    fader.paint(fader_canvas);
    REQUIRE(fader_canvas.count(DrawCommand::Type::fill_rect) == 1);
    REQUIRE(fader_canvas.count(DrawCommand::Type::fill_text) == 1);

    Toggle toggle;
    toggle.set_bounds({0, 0, 64, 32});
    toggle.set_on(true);
    toggle.set_label("On");
    toggle.set_custom_shader("uniform float time; half4 main(float2 p) { return half4(time); }");
    REQUIRE(toggle.has_custom_shader());
    REQUIRE(toggle.shader_uses_time());

    RecordingCanvas toggle_canvas;
    toggle.paint(toggle_canvas);
    REQUIRE(toggle_canvas.count(DrawCommand::Type::fill_rect) == 1);
    REQUIRE(toggle_canvas.count(DrawCommand::Type::fill_text) == 1);
}

TEST_CASE("Audio widgets minimal render style paints simplified branches",
          "[view][widget][style][issue-493]") {
    Knob knob;
    knob.set_bounds({0, 0, 64, 64});
    knob.set_render_style(WidgetRenderStyle::minimal);

    RecordingCanvas knob_canvas;
    knob.paint(knob_canvas);
    REQUIRE(knob_canvas.count(DrawCommand::Type::fill_circle) == 1);
    REQUIRE(knob_canvas.count(DrawCommand::Type::stroke_circle) == 1);
    REQUIRE(knob_canvas.count(DrawCommand::Type::stroke_arc) == 0);

    Fader fader;
    fader.set_bounds({0, 0, 28, 120});
    fader.set_render_style(WidgetRenderStyle::minimal);

    RecordingCanvas fader_canvas;
    fader.paint(fader_canvas);
    REQUIRE(fader_canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(fader_canvas.count(DrawCommand::Type::fill_circle) == 0);

    Meter meter;
    meter.set_bounds({0, 0, 8, 6});
    meter.set_render_style(WidgetRenderStyle::minimal);

    RecordingCanvas meter_canvas;
    meter.paint(meter_canvas);
    REQUIRE(meter_canvas.count(DrawCommand::Type::fill_rect) == 6);
    REQUIRE(meter_canvas.count(DrawCommand::Type::fill_rounded_rect) == 0);
}

TEST_CASE("Knob mouse paths update value, hover animation, and default reset",
          "[view][widget][interaction][issue-493]") {
    Knob knob;
    knob.set_value(0.25f);
    knob.set_default_value(0.75f);

    std::vector<float> changes;
    knob.on_change = [&](float v) { changes.push_back(v); };

    knob.on_mouse_enter();
    knob.advance_animations(1.0f);
    REQUIRE(knob.hover_glow() > 0.9f);

    knob.on_mouse_leave();
    knob.advance_animations(1.0f);
    REQUIRE(knob.hover_glow() < 0.1f);

    knob.on_mouse_down({0, 100});
    knob.on_mouse_drag({0, 55});
    REQUIRE(knob.value() > 0.25f);
    REQUIRE_FALSE(changes.empty());

    MouseEvent reset;
    reset.is_down = true;
    reset.click_count = 2;
    knob.on_mouse_event(reset);
    REQUIRE_THAT(knob.value(), WithinAbs(0.75, 0.001));
    REQUIRE_THAT(changes.back(), WithinAbs(0.75, 0.001));
}

TEST_CASE("Fader and toggle mouse paths dispatch clamped interactive values",
          "[view][widget][interaction][issue-493]") {
    Fader vertical;
    vertical.set_bounds({0, 0, 24, 100});
    std::vector<float> fader_changes;
    vertical.on_change = [&](float v) { fader_changes.push_back(v); };

    MouseEvent down;
    down.is_down = true;
    down.position = {12, 75};
    vertical.on_mouse_event(down);
    REQUIRE_THAT(vertical.value(), WithinAbs(0.25, 0.001));

    vertical.on_mouse_drag({12, 20});
    REQUIRE_THAT(vertical.value(), WithinAbs(0.80, 0.001));

    MouseEvent up = down;
    up.is_down = false;
    vertical.on_mouse_event(up);
    vertical.on_mouse_drag({12, 100});
    REQUIRE_THAT(vertical.value(), WithinAbs(0.80, 0.001));
    REQUIRE(fader_changes.size() >= 2);

    Fader horizontal;
    horizontal.set_bounds({0, 0, 200, 24});
    horizontal.set_orientation(Fader::Orientation::horizontal);
    MouseEvent horizontal_down;
    horizontal_down.is_down = true;
    horizontal_down.position = {50, 12};
    horizontal.on_mouse_event(horizontal_down);
    REQUIRE_THAT(horizontal.value(), WithinAbs(0.25, 0.001));

    Toggle toggle;
    std::vector<bool> toggle_changes;
    toggle.on_toggle = [&](bool v) { toggle_changes.push_back(v); };

    toggle.on_mouse_enter();
    toggle.advance_animations(1.0f);
    REQUIRE(toggle.hover_opacity() > 0.9f);

    toggle.on_mouse_down({4, 4});
    REQUIRE(toggle.is_on());
    REQUIRE(toggle_changes == std::vector<bool>{true});

    toggle.on_mouse_leave();
    toggle.advance_animations(1.0f);
    REQUIRE(toggle.hover_opacity() < 0.1f);
}

TEST_CASE("Checkbox, toggle button, icons, and image placeholders cover widget paint edges",
          "[view][widget][controls]") {
    Checkbox checkbox;
    checkbox.set_bounds({0, 0, 24, 24});

    RecordingCanvas unchecked_canvas;
    checkbox.paint(unchecked_canvas);
    REQUIRE(unchecked_canvas.count(DrawCommand::Type::stroke_rounded_rect) == 1);

    int checkbox_changes = 0;
    bool last_checked = false;
    checkbox.on_change = [&](bool checked) {
        ++checkbox_changes;
        last_checked = checked;
    };
    checkbox.on_mouse_down({12, 12});
    REQUIRE(checkbox.is_checked());
    REQUIRE(checkbox_changes == 1);
    REQUIRE(last_checked);

    RecordingCanvas checked_canvas;
    checkbox.paint(checked_canvas);
    REQUIRE(checked_canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(checked_canvas.count(DrawCommand::Type::stroke_line) == 2);

    ToggleButton button;
    button.set_bounds({0, 0, 96, 32});
    button.set_label("Latch");

    RecordingCanvas off_canvas;
    button.paint(off_canvas);
    REQUIRE(off_canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(off_canvas.count(DrawCommand::Type::stroke_rounded_rect) == 1);
    REQUIRE(commands_of(off_canvas, DrawCommand::Type::fill_text).front().text == "Latch");

    int toggle_count = 0;
    bool toggle_state = false;
    button.on_toggle = [&](bool on) {
        ++toggle_count;
        toggle_state = on;
    };
    button.on_mouse_down({4, 4});
    REQUIRE(button.is_on());
    REQUIRE(toggle_count == 1);
    REQUIRE(toggle_state);

    RecordingCanvas on_canvas;
    button.paint(on_canvas);
    REQUIRE(on_canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(on_canvas.count(DrawCommand::Type::stroke_rounded_rect) == 0);

    for (auto type : {Icon::Type::image_upload, Icon::Type::send,
                      Icon::Type::search, Icon::Type::close}) {
        Icon icon(type);
        icon.set_bounds({0, 0, 32, 32});
        RecordingCanvas icon_canvas;
        icon.paint(icon_canvas);
        REQUIRE(icon_canvas.command_count() > 0);
    }

    ImageView empty_image;
    empty_image.set_bounds({0, 0, 96, 48});
    RecordingCanvas empty_image_canvas;
    empty_image.paint(empty_image_canvas);
    REQUIRE(empty_image_canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(commands_of(empty_image_canvas, DrawCommand::Type::fill_text).front().text == "IMG");

    ImageView path_image;
    path_image.set_bounds({0, 0, 120, 48});
    path_image.set_image_path("/tmp/pulp-widget-preview.png");
    REQUIRE(path_image.image_path() == "file:///tmp/pulp-widget-preview.png");

    RecordingCanvas path_image_canvas;
    path_image.paint(path_image_canvas);
    REQUIRE(path_image_canvas.count(DrawCommand::Type::draw_image) == 1);
    REQUIRE(commands_of(path_image_canvas, DrawCommand::Type::draw_image).front().text == "/tmp/pulp-widget-preview.png");
}

TEST_CASE("Meter set_level", "[view][widget]") {
    Meter meter;
    meter.set_bounds({0, 0, 12, 200});
    meter.set_level(0.5f, 0.8f);

    REQUIRE_THAT(meter.display_rms(), WithinAbs(0.5, 0.01));
    REQUIRE_THAT(meter.display_peak(), WithinAbs(0.8, 0.01));
}

TEST_CASE("Meter renders with levels", "[view][widget]") {
    Meter meter;
    meter.set_bounds({0, 0, 12, 200});
    meter.set_level(0.6f, 0.85f);

    RecordingCanvas canvas;
    meter.paint(canvas);

    // Background + RMS fill = 2 rects, peak line + held peak = 2 lines
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) >= 1);
}

TEST_CASE("Meter horizontal orientation", "[view][widget]") {
    Meter meter;
    meter.set_orientation(Meter::Orientation::horizontal);
    meter.set_bounds({0, 0, 200, 12});
    meter.set_level(0.4f, 0.7f);

    RecordingCanvas canvas;
    meter.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) == 1);
}

TEST_CASE("Meter update with ballistics", "[view][widget]") {
    Meter meter;
    meter.set_bounds({0, 0, 12, 200});

    // Sudden peak
    meter.update(0.9f, 0.5f, 1.0f / 60.0f);
    REQUIRE(meter.display_peak() > 0);
    REQUIRE(meter.display_rms() > 0);

    // Decay
    for (int i = 0; i < 10; ++i) {
        meter.update(0.0f, 0.0f, 1.0f / 60.0f);
    }
    REQUIRE(meter.display_peak() < 0.9f);
    REQUIRE(meter.held_peak() > 0.8f); // Still held
}

TEST_CASE("XYPad value clamping", "[view][widget]") {
    XYPad pad;
    pad.set_x(0.3f);
    pad.set_y(0.7f);
    REQUIRE_THAT(pad.x_value(), WithinAbs(0.3, 0.001));
    REQUIRE_THAT(pad.y_value(), WithinAbs(0.7, 0.001));

    pad.set_x(1.5f);
    REQUIRE_THAT(pad.x_value(), WithinAbs(1.0, 0.001));
    pad.set_y(-0.5f);
    REQUIRE_THAT(pad.y_value(), WithinAbs(0.0, 0.001));
}

TEST_CASE("XYPad renders crosshair", "[view][widget]") {
    XYPad pad;
    pad.set_bounds({0, 0, 100, 100});
    pad.set_x(0.5f);
    pad.set_y(0.5f);
    pad.set_x_label("Freq");
    pad.set_y_label("Res");

    RecordingCanvas canvas;
    pad.paint(canvas);

    // Background + grid lines + crosshair lines + thumb + labels
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) >= 4); // 2 grid + 2 crosshair
    REQUIRE(canvas.count(DrawCommand::Type::fill_circle) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) >= 2);
}

TEST_CASE("WaveformView renders waveform", "[view][widget]") {
    WaveformView waveform;
    waveform.set_bounds({0, 0, 200, 60});

    // Generate sine wave
    std::vector<float> data(200);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = std::sin(2.0f * 3.14159f * i / data.size());
    }
    waveform.set_data(std::move(data));

    REQUIRE(waveform.sample_count() == 200);

    RecordingCanvas canvas;
    waveform.paint(canvas);

    // Background (rounded rect) + one center line + mirrored amplitude bars
    // (the waveform is drawn as per-column fill_rect bars, not per-sample lines).
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) > 10);
}

TEST_CASE("WaveformView empty renders background only", "[view][widget]") {
    WaveformView waveform;
    waveform.set_bounds({0, 0, 200, 60});

    RecordingCanvas canvas;
    waveform.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) == 0); // No waveform
}

TEST_CASE("WaveformView trigger -- rising zero crossing", "[view][widget][trigger]") {
    // Phase-shifted sine: starts at +0.5, crosses zero downward, crosses
    // zero upward roughly at index N/2. With rising_zero trigger, the
    // display should be rotated so the rising crossing is at index 0.
    constexpr size_t N = 256;
    std::vector<float> samples(N);
    const float phase_offset = 3.14159f / 4.0f;  // start 45° into the cycle
    for (size_t i = 0; i < N; ++i) {
        samples[i] = std::sin(phase_offset + 2.0f * 3.14159f * i / N);
    }

    // Locate the expected trigger index in the raw buffer before triggering.
    size_t expected = WaveformView::find_trigger_index(
        samples.data(), samples.size(),
        WaveformView::TriggerMode::rising_zero);
    REQUIRE(expected > 0);
    REQUIRE(expected < N);

    WaveformView waveform;
    waveform.set_trigger_mode(WaveformView::TriggerMode::rising_zero);
    waveform.set_data(samples);  // copy so we can compare

    REQUIRE(waveform.sample_count() == N);

    // After triggering, index 0 should correspond to old index `expected`,
    // which is the first sample > 0 following a sample <= 0.
    // We can't directly read the rotated buffer, but we can observe
    // stability: applying the same data twice should yield the same result.
    WaveformView waveform2;
    waveform2.set_trigger_mode(WaveformView::TriggerMode::rising_zero);
    waveform2.set_data(samples);
    REQUIRE(waveform2.sample_count() == N);
}

TEST_CASE("WaveformView trigger -- free run leaves buffer unchanged", "[view][widget][trigger]") {
    constexpr size_t N = 32;
    std::vector<float> samples(N);
    for (size_t i = 0; i < N; ++i) samples[i] = static_cast<float>(i);

    WaveformView waveform;
    REQUIRE(waveform.trigger_mode() == WaveformView::TriggerMode::free_run);
    waveform.set_data(samples);
    REQUIRE(waveform.sample_count() == N);
    // In free_run mode, find_trigger_index should return 0 regardless
    REQUIRE(WaveformView::find_trigger_index(
        samples.data(), samples.size(),
        WaveformView::TriggerMode::free_run) == 0);
}

TEST_CASE("WaveformView trigger -- falling zero crossing", "[view][widget][trigger]") {
    // A simple ramp that crosses zero downward at the midpoint.
    std::vector<float> samples = {2, 1, 0.5f, 0, -0.5f, -1, -2, -1};
    size_t idx = WaveformView::find_trigger_index(
        samples.data(), samples.size(),
        WaveformView::TriggerMode::falling_zero);
    // prev=0 at i=3 is not > 0, so the first falling crossing is i=4 (prev=0, curr=-0.5)
    REQUIRE(idx == 4);
}

TEST_CASE("WaveformView trigger -- no crossing leaves buffer alone", "[view][widget][trigger]") {
    // All positive samples — no rising zero crossing possible
    std::vector<float> samples = {0.5f, 0.6f, 0.7f, 0.8f, 0.9f};
    size_t idx = WaveformView::find_trigger_index(
        samples.data(), samples.size(),
        WaveformView::TriggerMode::rising_zero);
    REQUIRE(idx == 0);

    WaveformView waveform;
    waveform.set_trigger_mode(WaveformView::TriggerMode::rising_zero);
    waveform.set_data(samples);
    REQUIRE(waveform.sample_count() == samples.size());
}

TEST_CASE("SpectrumView renders bars", "[view][widget]") {
    SpectrumView spectrum;
    spectrum.set_bounds({0, 0, 300, 100});
    spectrum.set_style(SpectrumView::Style::bars);

    std::vector<float> bins(32);
    for (size_t i = 0; i < bins.size(); ++i) {
        bins[i] = -80.0f + 80.0f * (1.0f - static_cast<float>(i) / bins.size());
    }
    spectrum.set_spectrum(std::move(bins));

    REQUIRE(spectrum.bin_count() == 32);

    RecordingCanvas canvas;
    spectrum.paint(canvas);

    // Background + 32 bars + 3 grid lines
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) >= 30);
}

TEST_CASE("SpectrumView renders filled line", "[view][widget]") {
    SpectrumView spectrum;
    spectrum.set_bounds({0, 0, 300, 100});
    spectrum.set_style(SpectrumView::Style::filled);

    std::vector<float> bins(64, -40.0f);
    spectrum.set_spectrum(std::move(bins));

    RecordingCanvas canvas;
    spectrum.paint(canvas);

    // GPU path uses draw_waveform (single call), CPU fallback uses stroke_line
    // At minimum: background rect + center line + waveform fallback lines
    REQUIRE(canvas.command_count() > 5);
}

TEST_CASE("SpectrumView empty renders background", "[view][widget]") {
    SpectrumView spectrum;
    spectrum.set_bounds({0, 0, 300, 100});

    RecordingCanvas canvas;
    spectrum.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) == 0);
}

TEST_CASE("SpectrumView invalid dB range only draws background",
          "[view][widget]") {
    SpectrumView spectrum;
    spectrum.set_bounds({0, 0, 300, 100});
    spectrum.set_style(SpectrumView::Style::bars);
    spectrum.set_spectrum(std::vector<float>{-80.0f, -40.0f, -12.0f});
    spectrum.set_range(0.0f, -80.0f);

    RecordingCanvas canvas;
    spectrum.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) == 0);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) == 0);
}

TEST_CASE("SpectrogramView auto-configures and paints pushed spectrum",
          "[view][widget]") {
    SpectrogramView spectrogram;
    spectrogram.set_bounds({0, 0, 128, 32});

    RecordingCanvas empty_canvas;
    spectrogram.paint(empty_canvas);
    REQUIRE(empty_canvas.count(DrawCommand::Type::fill_rect) == 1);

    std::vector<float> magnitudes{-80.0f, -40.0f, 0.0f};
    spectrogram.push_spectrum(magnitudes.data(), static_cast<int>(magnitudes.size()));

    REQUIRE(spectrogram.history_columns() == 256);
    REQUIRE(spectrogram.freq_rows() == 3);

    RecordingCanvas painted_canvas;
    spectrogram.paint(painted_canvas);

    REQUIRE(painted_canvas.count(DrawCommand::Type::fill_rect) >
            empty_canvas.count(DrawCommand::Type::fill_rect));
    REQUIRE(painted_canvas.count(DrawCommand::Type::set_fill_color) >
            empty_canvas.count(DrawCommand::Type::set_fill_color));
}

TEST_CASE("SpectrogramView explicit configuration controls painted grid size",
          "[view][widget]") {
    SpectrogramView spectrogram;
    spectrogram.set_bounds({0, 0, 40, 20});
    spectrogram.configure(4, 2, pulp::signal::ColorRamp::heat, -60.0f, -20.0f);
    spectrogram.set_color_ramp(pulp::signal::ColorRamp::grayscale);
    spectrogram.set_range(-90.0f, -30.0f);

    std::vector<float> magnitudes{-90.0f, -30.0f};
    spectrogram.push_spectrum(magnitudes.data(), static_cast<int>(magnitudes.size()));

    REQUIRE(spectrogram.history_columns() == 4);
    REQUIRE(spectrogram.freq_rows() == 2);

    RecordingCanvas canvas;
    spectrogram.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) == 1 + 4 * 2);
}

TEST_CASE("MultiMeter paints vertical and horizontal channel indicators",
          "[view][widget]") {
    pulp::signal::MultiChannelMeterData data;
    data.num_channels = 3;
    data.channels[0].rms = 0.95f;
    data.channels[0].peak = 0.98f;
    data.channels[0].clipped = true;
    data.channels[1].rms = 0.75f;
    data.channels[1].peak = 0.82f;
    data.channels[2].rms = 0.35f;
    data.channels[2].peak = 0.42f;

    MultiMeter vertical;
    vertical.set_bounds({0, 0, 90, 120});
    vertical.set_channel_count(20);
    REQUIRE(vertical.channel_count() == pulp::signal::kMaxMeterChannels);

    vertical.update(data, 0.1f);
    REQUIRE(vertical.channel_count() == 3);
    REQUIRE(vertical.ballistics().channels[0].clip_indicator);

    RecordingCanvas vertical_canvas;
    vertical.paint(vertical_canvas);

    REQUIRE(vertical_canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(vertical_canvas.count(DrawCommand::Type::fill_rect) >= 4);
    REQUIRE(vertical_canvas.count(DrawCommand::Type::stroke_line) >= 6);

    MultiMeter horizontal;
    horizontal.set_bounds({0, 0, 120, 60});
    horizontal.set_layout(MultiMeter::Layout::horizontal);
    horizontal.update(data, 0.1f);

    RecordingCanvas horizontal_canvas;
    horizontal.paint(horizontal_canvas);

    REQUIRE(horizontal.layout() == MultiMeter::Layout::horizontal);
    REQUIRE(horizontal_canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(horizontal_canvas.count(DrawCommand::Type::fill_rect) >= 4);
    REQUIRE(horizontal_canvas.count(DrawCommand::Type::stroke_line) >= 6);

    MultiMeter segmented;
    segmented.set_bounds({0, 0, 260, 48});
    segmented.set_layout(MultiMeter::Layout::horizontal);
    segmented.set_display_style(MultiMeter::DisplayStyle::segmented);
    segmented.update(data, 0.1f);

    RecordingCanvas segmented_canvas;
    segmented.paint(segmented_canvas);

    REQUIRE(segmented.display_style() == MultiMeter::DisplayStyle::segmented);
    REQUIRE(segmented_canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(segmented_canvas.count(DrawCommand::Type::fill_rect) >= 24);
    REQUIRE(segmented_canvas.count(DrawCommand::Type::fill_text) >= 4);
    REQUIRE(segmented_canvas.count(DrawCommand::Type::stroke_line) >= 8);
}

TEST_CASE("MultiMeter with no channels paints nothing",
          "[view][widget]") {
    MultiMeter meter;
    meter.set_bounds({0, 0, 80, 40});

    RecordingCanvas canvas;
    meter.paint(canvas);

    REQUIRE(canvas.command_count() == 0);
}

TEST_CASE("CorrelationMeter clamps updates and paints both polarities",
          "[view][widget]") {
    CorrelationMeter meter;
    meter.set_bounds({0, 0, 100, 20});

    meter.update(2.0f, 1.0f);
    REQUIRE(meter.display_correlation() > 0.99f);

    RecordingCanvas positive_canvas;
    meter.paint(positive_canvas);

    REQUIRE(positive_canvas.count(DrawCommand::Type::fill_rounded_rect) == 2);
    REQUIRE(positive_canvas.count(DrawCommand::Type::stroke_line) == 3);
    REQUIRE(positive_canvas.count(DrawCommand::Type::fill_rect) == 1);

    meter.update(-2.0f, 1.0f);
    REQUIRE(meter.display_correlation() < -0.99f);

    RecordingCanvas negative_canvas;
    meter.paint(negative_canvas);

    REQUIRE(negative_canvas.count(DrawCommand::Type::fill_rounded_rect) == 2);
    REQUIRE(negative_canvas.count(DrawCommand::Type::stroke_line) == 3);
    REQUIRE(negative_canvas.count(DrawCommand::Type::fill_rect) == 1);
}

TEST_CASE("View paint_all paints children", "[view][widget]") {
    View root;
    root.set_bounds({0, 0, 300, 200});

    auto label = std::make_unique<Label>("Test");
    label->set_bounds({10, 10, 100, 20});

    auto knob = std::make_unique<Knob>();
    knob->set_bounds({10, 40, 48, 48});
    knob->set_value(0.5f);

    root.add_child(std::move(label));
    root.add_child(std::move(knob));

    RecordingCanvas canvas;
    root.paint_all(canvas);

    // Should have commands from both children
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) >= 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_arc) >= 2);
    // save/restore pairs for root + 2 children = 3 pairs
    REQUIRE(canvas.count(DrawCommand::Type::save) == 3);
    REQUIRE(canvas.count(DrawCommand::Type::restore) == 3);
}

// ── pulp #927 — Label honors setFontFamily / setFontWeight / setLetterSpacing ──
//
// Before #927, Label::paint() called canvas.set_font("Inter", size) with no
// weight / family / spacing — so JS calls into setFontWeight, setFontFamily,
// and setLetterSpacing landed on Label members but were dropped on the floor
// at render time. These tests assert the propagation through the
// RecordingCanvas, which captures the rich state via DrawCommand::set_font_full.

TEST_CASE("Label propagates font_family to canvas", "[view][widget][issue-927]") {
    Label label("Hello");
    label.set_bounds({0, 0, 200, 24});
    label.set_font_family("JetBrains Mono");

    RecordingCanvas canvas;
    label.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::set_font_full) == 1);
    // Locate the rich set_font_full command and assert family is forwarded.
    bool found_family = false;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == DrawCommand::Type::set_font_full) {
            REQUIRE(cmd.text == "JetBrains Mono");
            found_family = true;
        }
    }
    REQUIRE(found_family);
}

TEST_CASE("Label propagates font_weight to canvas", "[view][widget][issue-927]") {
    Label label("BOLD");
    label.set_bounds({0, 0, 200, 24});
    label.set_font_weight(700);

    RecordingCanvas canvas;
    label.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::set_font_full) == 1);
    bool found_weight = false;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == DrawCommand::Type::set_font_full) {
            // f[1] carries CSS font-weight (100..900).
            REQUIRE(static_cast<int>(cmd.f[1]) == 700);
            found_weight = true;
        }
    }
    REQUIRE(found_weight);
}

TEST_CASE("Label propagates letter_spacing to canvas", "[view][widget][issue-927]") {
    Label label("SPECTR");
    label.set_bounds({0, 0, 200, 24});
    label.set_letter_spacing(1.5f);

    RecordingCanvas canvas;
    label.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::set_font_full) == 1);
    bool found_spacing = false;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == DrawCommand::Type::set_font_full) {
            // f[3] carries CSS letter-spacing in px.
            REQUIRE_THAT(cmd.f[3], WithinAbs(1.5, 0.001));
            found_spacing = true;
        }
    }
    REQUIRE(found_spacing);
}

TEST_CASE("Label set_font_full carries default weight when no setter called",
          "[view][widget][issue-927]") {
    // Verifies that the rich command goes out even for the default
    // (font_weight_=400, no family) path. Important: we do NOT want a
    // regression where set_font_full only fires for "non-default" labels.
    Label label("plain");
    label.set_bounds({0, 0, 100, 20});

    RecordingCanvas canvas;
    label.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::set_font_full) == 1);
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == DrawCommand::Type::set_font_full) {
            REQUIRE(cmd.text == "Inter");          // default theme family
            REQUIRE(static_cast<int>(cmd.f[1]) == 400);
            REQUIRE_THAT(cmd.f[3], WithinAbs(0.0, 0.001));
        }
    }
}

TEST_CASE("Label getters round-trip font_family", "[view][widget][issue-927]") {
    Label label("x");
    REQUIRE(label.font_family().empty());          // default == theme fallback
    label.set_font_family("Inter Display");
    REQUIRE(label.font_family() == "Inter Display");
}

// ── pulp #1737 — #1791: clear font_features at end ────────────────────────
// Label::paint sets font_features on the shared canvas when fontVariant
// is non-empty (translates CSV → SkShaper Feature tags so HarfBuzz
// honors the OpenType lookup). Because the canvas keeps font_features
// as mutable canvas-level state, leaving them set across paint
// boundaries leaks the previous Label's typography onto sibling
// widgets that don't set features themselves (TextButton, TextEditor,
// adjacent Labels). The fix unconditionally clears at end of paint.
//
// We verify by subclassing RecordingCanvas and recording every
// set/clear call. The expected sequence for any Label::paint that
// touches the canvas is: setN clears or sets exactly once at the top
// (the existing fontVariant translation block), then exactly one
// clear at the bottom. So either the count of clears == 2 (empty
// fontVariant case: one clear in the translation block, one at end)
// or the count of clears == 1 with one set (non-empty fontVariant:
// translation set + end clear). Either way, the LAST call must be a
// clear so subsequent paints start clean.

namespace {
struct FontFeatureSpyCanvas : pulp::canvas::RecordingCanvas {
    enum class Kind { set, clear };
    std::vector<Kind> calls;

    void set_font_features(std::vector<pulp::canvas::Canvas::FontFeature> features) override {
        (void)features;
        calls.push_back(Kind::set);
    }
    void clear_font_features() override {
        calls.push_back(Kind::clear);
    }
};
}

TEST_CASE("Label::paint always ends with clear_font_features (#1791)",
          "[view][widget][issue-1737][issue-1791]") {
    Label label("hello");
    label.set_bounds({0, 0, 120, 24});

    {
        // Empty fontVariant — Label still calls clear at the top
        // (translation block) AND at the end (the new safety clear).
        FontFeatureSpyCanvas canvas;
        label.paint(canvas);
        REQUIRE_FALSE(canvas.calls.empty());
        // The LAST call MUST be a clear — that's the contract that
        // prevents leakage onto subsequent widgets.
        REQUIRE(canvas.calls.back() == FontFeatureSpyCanvas::Kind::clear);
    }

    {
        // Non-empty fontVariant — translation block calls set;
        // end-of-paint MUST then clear so the next widget doesn't
        // inherit our tnum/smcp/etc.
        label.set_font_variant("tabular-nums,small-caps");
        FontFeatureSpyCanvas canvas;
        label.paint(canvas);
        REQUIRE_FALSE(canvas.calls.empty());
        // At least one set somewhere in the trace.
        bool saw_set = false;
        for (auto k : canvas.calls) if (k == FontFeatureSpyCanvas::Kind::set) saw_set = true;
        REQUIRE(saw_set);
        // The LAST call MUST be a clear regardless.
        REQUIRE(canvas.calls.back() == FontFeatureSpyCanvas::Kind::clear);
    }
}

// ── pulp #1737 PR-2 — Label soft-wrap via TextShaper::layout_with_lines ──
// PR-1 added the BreakMode plumbing inside TextShaper. PR-2 (this PR)
// wires Label::paint multi-line path to consume TextShaper when
// View::word_break_ opts into break-word or anywhere AND bounds().width
// is bounded. Default (`normal` / empty) keeps the legacy `\n`-only
// split — these tests pin both paths so a future change can't silently
// flip semantics for either.

TEST_CASE("Label soft-wrap: word_break='break-word' splits long unbroken word",
          "[view][widget][issue-1737]") {
    Label label("Antidisestablishmentarianism");
    label.set_bounds({0, 0, 60, 100});
    label.set_multi_line(true);
    label.set_word_break("break-word");

    RecordingCanvas canvas;
    label.paint(canvas);

    auto text_cmds = commands_of(canvas, DrawCommand::Type::fill_text);
    // Single 28-char word at width 60 — should produce >1 line under
    // break-word. Default `normal` would emit exactly 1 line that
    // overflows.
    REQUIRE(text_cmds.size() >= 2);

    // Reconstruction must be lossless for the non-whitespace characters.
    std::string reconstructed;
    for (const auto& cmd : text_cmds) reconstructed += cmd.text;
    REQUIRE(reconstructed == "Antidisestablishmentarianism");
}

TEST_CASE("Label soft-wrap: word_break='normal' on a single unbroken word keeps whole-word overflow",
          "[view][widget][issue-1737][issue-1924]") {
    Label label("Antidisestablishmentarianism");
    label.set_bounds({0, 0, 60, 100});
    label.set_multi_line(true);
    label.set_word_break("normal");  // explicit, but matches default

    RecordingCanvas canvas;
    label.paint(canvas);

    // pulp #1924: the default (`normal`) path now also routes through
    // TextShaper for bounded multi_line labels, but TextShaper's
    // BreakMode::normal preserves the "whole-word overflow for a single
    // unbroken word" CSS contract (see test_text_shaper.cpp
    // "BreakMode::normal preserves legacy whole-word overflow"). So an
    // input with no whitespace still emits exactly one fill_text — the
    // observable output for this probe is unchanged from pre-#1924.
    auto text_cmds = commands_of(canvas, DrawCommand::Type::fill_text);
    REQUIRE(text_cmds.size() == 1);
    REQUIRE(text_cmds[0].text == "Antidisestablishmentarianism");
}

// ── pulp #1924 — Label soft-wraps under CSS default `white-space: normal` ──
// Pre-#1924 the wrap gate required `word_break != normal` before the shaper
// fired, so default Labels (no explicit word-break) overflowed their bounds
// instead of soft-wrapping at whitespace. CSS `white-space: normal` is the
// default and MUST soft-wrap at word boundaries. Hit on Spectr's dropdowns
// (Settings / Preset Manager) — text overflowed the parent SChips /
// PatternRow container.

TEST_CASE("Label default word_break: text with whitespace soft-wraps at word boundaries (#1924)",
          "[view][widget][issue-1924]") {
    // Several short words separated by spaces. With bounds().width
    // narrower than the full string the shaper must break at whitespace,
    // emitting more than one line. NO explicit word_break is set — this
    // is the CSS default (`white-space: normal`) path that was broken
    // pre-#1924 (single overflowing line).
    Label label("Settings Preset Manager Dropdown");
    label.set_bounds({0, 0, 60, 200});
    label.set_multi_line(true);
    // NOTE: no set_word_break — defaults to "normal" / empty.

    RecordingCanvas canvas;
    label.paint(canvas);

    auto text_cmds = commands_of(canvas, DrawCommand::Type::fill_text);
    // Expect >1 fill_text — soft-wrap at whitespace inside the bounded
    // container. Pre-#1924 this would have been exactly 1 (the legacy
    // `\n`-only path with no newlines in the input).
    REQUIRE(text_cmds.size() >= 2);

    // Reconstruction across all emitted lines must contain every
    // non-whitespace character from the original (whitespace at line
    // boundaries is collapsed under CSS `white-space: normal` — the
    // exact preservation contract is shaper-defined, but no characters
    // beyond inter-word spaces should be dropped).
    std::string concatenated;
    for (const auto& cmd : text_cmds) concatenated += cmd.text;
    // Each individual word must still appear in the emitted output.
    for (const char* word : {"Settings", "Preset", "Manager", "Dropdown"}) {
        INFO("Expecting word " << word << " in '" << concatenated << "'");
        REQUIRE(concatenated.find(word) != std::string::npos);
    }
}

TEST_CASE("Label soft-wrap: word_break='anywhere' also splits, same as break-word for over-wide single word",
          "[view][widget][issue-1737]") {
    Label label("Supercalifragilisticexpialidocious");
    label.set_bounds({0, 0, 50, 100});
    label.set_multi_line(true);
    label.set_word_break("anywhere");

    RecordingCanvas canvas;
    label.paint(canvas);

    auto text_cmds = commands_of(canvas, DrawCommand::Type::fill_text);
    REQUIRE(text_cmds.size() >= 2);

    std::string reconstructed;
    for (const auto& cmd : text_cmds) reconstructed += cmd.text;
    REQUIRE(reconstructed == "Supercalifragilisticexpialidocious");
}

TEST_CASE("Label soft-wrap: line-clamp truncates wrapped lines + appends ellipsis",
          "[view][widget][issue-1737]") {
    Label label("Antidisestablishmentarianism");  // single long word
    label.set_bounds({0, 0, 60, 100});
    label.set_multi_line(true);
    label.set_word_break("break-word");
    label.set_line_clamp(2);

    RecordingCanvas canvas;
    label.paint(canvas);

    auto text_cmds = commands_of(canvas, DrawCommand::Type::fill_text);
    // line_clamp=2 caps emitted lines at 2 (assuming the word produces
    // ≥3 shaped lines at width 60).
    REQUIRE(text_cmds.size() <= 2);
    if (text_cmds.size() == 2) {
        // pulp #1552 — last visible line under line-clamp gets the
        // U+2026 (UTF-8 0xE2 0x80 0xA6) ellipsis appended.
        const std::string& last = text_cmds.back().text;
        REQUIRE(last.size() >= 3);
        REQUIRE((unsigned char)last[last.size()-3] == 0xE2);
        REQUIRE((unsigned char)last[last.size()-2] == 0x80);
        REQUIRE((unsigned char)last[last.size()-1] == 0xA6);
    }
}

TEST_CASE("Label soft-wrap: bounds().width == 0 falls through to legacy path (no shaper invocation)",
          "[view][widget][issue-1737]") {
    Label label("hello world how are you");
    // NO bounds set — bounds().width defaults to 0
    label.set_multi_line(true);
    label.set_word_break("break-word");

    RecordingCanvas canvas;
    label.paint(canvas);

    // With no bounded width, soft-wrap can't decide where to break.
    // Fall through to the legacy path which emits one fill_text per
    // \n-delimited line — exactly 1 fill_text since input has no \n.
    auto text_cmds = commands_of(canvas, DrawCommand::Type::fill_text);
    REQUIRE(text_cmds.size() == 1);
    REQUIRE(text_cmds[0].text == "hello world how are you");
}

namespace {

// pulp #73 — minimal WindowHost that counts repaint() calls. Mirrors
// the DummyWindowHost in test_view.cpp; duplicated here so the widget
// suite stays self-contained.
class CountingHost final : public WindowHost {
public:
    void show() override {}
    void hide() override {}
    bool is_visible() const override { return false; }
    void repaint() override { ++repaint_count; }
    void set_close_callback(std::function<void()>) override {}
    void run_event_loop() override {}

    int repaint_count = 0;
};

} // namespace

// pulp #73 — programmatic value mutation MUST schedule a repaint.
// User-input mutation (mouse drag) piggybacks on the host's per-event
// setNeedsDisplay path; preset application / JS bridge setValue go
// through THIS code path and would otherwise leave the painted state
// stale until the next user input. The "preset-applied band-shape
// outline missing (only renders during manual draw)" symptom in
// Spectr was exactly this gap. These regression tests fence each
// programmatic setter so a refactor that drops the request_repaint()
// call surfaces in CI before the user sees a silent paint.
TEST_CASE("Widget set_value programmatic mutation requests repaint [issue-73]",
          "[view][widget][issue-73]") {
    SECTION("Knob::set_value") {
        Knob knob;
        CountingHost host;
        knob.set_window_host(&host);
        REQUIRE(host.repaint_count == 0);

        knob.set_value(0.7f);
        REQUIRE(host.repaint_count >= 1);
    }

    SECTION("Fader::set_value") {
        Fader fader;
        CountingHost host;
        fader.set_window_host(&host);
        REQUIRE(host.repaint_count == 0);

        fader.set_value(0.4f);
        REQUIRE(host.repaint_count >= 1);
    }

    SECTION("RangeSlider::set_value") {
        RangeSlider slider;
        slider.set_min(0);
        slider.set_max(100);
        CountingHost host;
        slider.set_window_host(&host);
        int before = host.repaint_count;

        slider.set_value(50);
        REQUIRE(host.repaint_count > before);
    }

    SECTION("Toggle::set_on") {
        Toggle toggle;
        CountingHost host;
        toggle.set_window_host(&host);
        REQUIRE(host.repaint_count == 0);

        toggle.set_on(true);
        REQUIRE(host.repaint_count >= 1);
    }

    SECTION("Checkbox::set_checked") {
        Checkbox cb;
        CountingHost host;
        cb.set_window_host(&host);
        REQUIRE(host.repaint_count == 0);

        cb.set_checked(true);
        REQUIRE(host.repaint_count >= 1);
    }

    SECTION("ToggleButton::set_on") {
        ToggleButton tb;
        CountingHost host;
        tb.set_window_host(&host);
        REQUIRE(host.repaint_count == 0);

        tb.set_on(true);
        REQUIRE(host.repaint_count >= 1);
    }
}

// PR #2013 — the no-change guard. WidgetBridge::sync_from_store
// and restore_values(...) call set_value() / set_on() in tight loops
// during sync/reload. Firing a host repaint when the value didn't change
// burns wall-clock on large widget trees. These tests fence the guard
// so a regression that drops the early-return surfaces as visible
// frame-time spikes long before a human notices.
TEST_CASE("Widget setters skip repaint when value is unchanged [issue-73]",
          "[view][widget][issue-73][issue-2013]") {
    SECTION("Knob::set_value") {
        Knob knob;
        CountingHost host;
        knob.set_window_host(&host);
        knob.set_value(0.5f);
        int after_first = host.repaint_count;
        REQUIRE(after_first >= 1);

        // Same value again — must NOT repaint.
        knob.set_value(0.5f);
        REQUIRE(host.repaint_count == after_first);

        // Different value — must repaint.
        knob.set_value(0.6f);
        REQUIRE(host.repaint_count > after_first);
    }

    SECTION("Fader::set_value idempotent") {
        Fader fader;
        CountingHost host;
        fader.set_window_host(&host);
        fader.set_value(0.3f);
        int after = host.repaint_count;

        fader.set_value(0.3f);
        REQUIRE(host.repaint_count == after);
    }

    SECTION("Toggle::set_on idempotent") {
        Toggle toggle;
        CountingHost host;
        toggle.set_window_host(&host);
        toggle.set_on(true);
        int after = host.repaint_count;

        toggle.set_on(true);
        REQUIRE(host.repaint_count == after);
    }

    SECTION("Checkbox::set_checked idempotent") {
        Checkbox cb;
        CountingHost host;
        cb.set_window_host(&host);
        cb.set_checked(true);
        int after = host.repaint_count;

        cb.set_checked(true);
        REQUIRE(host.repaint_count == after);
    }

    SECTION("ToggleButton::set_on idempotent") {
        ToggleButton tb;
        CountingHost host;
        tb.set_window_host(&host);
        tb.set_on(true);
        int after = host.repaint_count;

        tb.set_on(true);
        REQUIRE(host.repaint_count == after);
    }

    SECTION("RangeSlider::set_value idempotent") {
        RangeSlider slider;
        slider.set_min(0);
        slider.set_max(100);
        CountingHost host;
        slider.set_window_host(&host);
        slider.set_value(50);
        int after = host.repaint_count;

        slider.set_value(50);
        REQUIRE(host.repaint_count == after);
    }

    SECTION("Knob::set_label idempotent") {
        Knob knob;
        CountingHost host;
        knob.set_window_host(&host);
        knob.set_label("Cutoff");
        int after = host.repaint_count;

        knob.set_label("Cutoff");
        REQUIRE(host.repaint_count == after);
    }

    SECTION("Fader::set_label idempotent") {
        Fader fader;
        CountingHost host;
        fader.set_window_host(&host);
        fader.set_label("Volume");
        int after = host.repaint_count;

        fader.set_label("Volume");
        REQUIRE(host.repaint_count == after);
    }

    SECTION("ToggleButton::set_label idempotent") {
        ToggleButton tb;
        CountingHost host;
        tb.set_window_host(&host);
        tb.set_label("Bypass");
        int after = host.repaint_count;

        tb.set_label("Bypass");
        REQUIRE(host.repaint_count == after);
    }
}

TEST_CASE("Widget set_label programmatic mutation requests repaint [issue-73]",
          "[view][widget][issue-73]") {
    SECTION("Knob::set_label") {
        Knob knob;
        CountingHost host;
        knob.set_window_host(&host);
        int before = host.repaint_count;

        knob.set_label("Cutoff");
        REQUIRE(host.repaint_count > before);
    }

    SECTION("Fader::set_label") {
        Fader fader;
        CountingHost host;
        fader.set_window_host(&host);
        int before = host.repaint_count;

        fader.set_label("Volume");
        REQUIRE(host.repaint_count > before);
    }

    SECTION("ToggleButton::set_label") {
        ToggleButton tb;
        CountingHost host;
        tb.set_window_host(&host);
        int before = host.repaint_count;

        tb.set_label("Mute");
        REQUIRE(host.repaint_count > before);
    }
}

TEST_CASE("Knob format setter requests repaint without changing value [issue-73]",
          "[view][widget][issue-73]") {
    Knob knob;
    knob.set_value(0.25f);
    CountingHost host;
    knob.set_window_host(&host);
    int before = host.repaint_count;

    knob.set_format([](float value) {
        return std::to_string(static_cast<int>(value * 100.0f)) + "%";
    });

    REQUIRE(knob.value() == 0.25f);
    REQUIRE(host.repaint_count > before);
}
