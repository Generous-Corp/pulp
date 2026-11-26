#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <pulp/vellum_components/inspector_overlay.hpp>

#include <vellum/canvas/recording_canvas.hpp>
#include <vellum/view/view.hpp>

#include "render_helpers.hpp"

#include <cstdint>
#include <memory>

using namespace pulp::vellum_components;
using namespace pulp::vellum_components::testing;
using ::vellum::canvas::Color;
using ::vellum::canvas::DrawCommand;
using ::vellum::canvas::RecordingCanvas;
using ::vellum::view::KeyCode;
using ::vellum::view::KeyEvent;
using ::vellum::view::MouseEvent;

namespace {

// A recording canvas that can read pixels back with Skia's contract: a source
// rect that leaves the surface fails. Every pixel reads as (64, 128, 192).
class ReadbackCanvas : public RecordingCanvas {
  public:
    ReadbackCanvas(int w, int h) : surface_w_(w), surface_h_(h) {}

    bool read_pixels(int x, int y, int width, int height, std::uint8_t* out) override {
        if (!out || width <= 0 || height <= 0)
            return false;
        if (x < 0 || y < 0 || x + width > surface_w_ || y + height > surface_h_)
            return false;
        last_x = x;
        last_y = y;
        last_w = width;
        last_h = height;
        ++reads;
        for (int i = 0; i < width * height; ++i) {
            out[i * 4 + 0] = 64;
            out[i * 4 + 1] = 128;
            out[i * 4 + 2] = 192;
            out[i * 4 + 3] = 255;
        }
        return true;
    }

    int last_x = -1, last_y = -1, last_w = -1, last_h = -1;
    int reads = 0;

  private:
    int surface_w_, surface_h_;
};

MouseEvent move_to(float x, float y) {
    MouseEvent e;
    e.position = {x, y};
    e.is_down = false;
    return e;
}

KeyEvent z_key() {
    KeyEvent e;
    e.key = KeyCode::z;
    e.is_down = true;
    return e;
}

// Pin a view to a rectangle so the layout pass a render runs keeps it there.
void place(View& v, float x, float y, float w, float h) {
    v.set_position(View::Position::absolute);
    v.set_left(x);
    v.set_top(y);
    v.flex().preferred_width = w;
    v.flex().preferred_height = h;
    v.set_bounds({x, y, w, h});
}

} // namespace

TEST_CASE("Z toggles the loupe while the overlay is active and only then",
          "[pulp-vellum-components][inspector-overlay][loupe]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorOverlay inspector(root);
    inspector.set_active(true);
    REQUIRE_FALSE(inspector.zoom_active());

    REQUIRE(inspector.handle_key_event(z_key()));
    REQUIRE(inspector.zoom_active());
    REQUIRE(inspector.handle_key_event(z_key()));
    REQUIRE_FALSE(inspector.zoom_active());

    inspector.set_active(false);
    REQUIRE_FALSE(inspector.handle_key_event(z_key()));
    REQUIRE_FALSE(inspector.zoom_active());

    // A modified Z is not the toggle.
    inspector.set_active(true);
    KeyEvent chord = z_key();
    chord.modifiers = ::vellum::view::kModShift;
    REQUIRE_FALSE(inspector.handle_key_event(chord));
    REQUIRE_FALSE(inspector.zoom_active());
}

TEST_CASE("dismissing the overlay closes the loupe",
          "[pulp-vellum-components][inspector-overlay][loupe]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorOverlay inspector(root);
    inspector.set_active(true);
    inspector.set_zoom_active(true);
    REQUIRE(inspector.zoom_active());
    inspector.set_active(false);
    REQUIRE_FALSE(inspector.zoom_active());
}

TEST_CASE("opening the loupe seeds the sample point at the middle of the root",
          "[pulp-vellum-components][inspector-overlay][loupe]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorOverlay inspector(root);
    inspector.set_active(true);
    inspector.set_zoom_active(true);
    REQUIRE(inspector.zoom_sample_center().x == Catch::Approx(200.0f));
    REQUIRE(inspector.zoom_sample_center().y == Catch::Approx(150.0f));
}

TEST_CASE("the loupe adds a magnified grid of cells on top of the highlights",
          "[pulp-vellum-components][inspector-overlay][loupe]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorOverlay inspector(root);
    inspector.set_active(true);

    RecordingCanvas baseline;
    inspector.paint(baseline);

    inspector.set_zoom_active(true);
    RecordingCanvas with_loupe;
    inspector.paint(with_loupe);
    REQUIRE(with_loupe.command_count() > baseline.command_count());
    // An 11 x 11 block of filled cells, so well over a hundred rectangles.
    REQUIRE(with_loupe.count(DrawCommand::Type::fill_rect) > 100);
}

TEST_CASE("the loupe re-centres on every mouse event without consuming a hover",
          "[pulp-vellum-components][inspector-overlay][loupe]") {
    View root;
    root.set_bounds({0, 0, 900, 400});
    InspectorOverlay inspector(root);
    inspector.set_active(true);
    inspector.set_zoom_active(true);

    REQUIRE_FALSE(inspector.handle_mouse_event(move_to(137, 92)));
    REQUIRE(inspector.zoom_sample_center().x == Catch::Approx(137.0f));
    REQUIRE(inspector.zoom_sample_center().y == Catch::Approx(92.0f));

    inspector.handle_mouse_event(move_to(210, 40));
    REQUIRE(inspector.zoom_sample_center().x == Catch::Approx(210.0f));
    REQUIRE(inspector.zoom_sample_center().y == Catch::Approx(40.0f));

    // A press moves it too, and is consumed like any press.
    MouseEvent press = move_to(300, 200);
    press.is_down = true;
    REQUIRE(inspector.handle_mouse_event(press));
    REQUIRE(inspector.zoom_sample_center().x == Catch::Approx(300.0f));

    // With the loupe closed the point stays where it was.
    inspector.set_zoom_active(false);
    inspector.handle_mouse_event(move_to(10, 10));
    REQUIRE(inspector.zoom_sample_center().x == Catch::Approx(300.0f));
}

TEST_CASE("without readback the loupe falls back to the colour a view declares",
          "[pulp-vellum-components][inspector-overlay][loupe]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    auto panel = std::make_unique<View>();
    panel->set_bounds({50, 50, 200, 150});
    const Color kPanelColor = Color::rgba(0.2f, 0.6f, 0.9f, 1.0f);
    panel->set_background_color(kPanelColor);
    root.add_child(std::move(panel));

    InspectorOverlay inspector(root);
    inspector.set_active(true);
    inspector.set_zoom_active(true);

    inspector.handle_mouse_event(move_to(120, 110));
    RecordingCanvas canvas;
    inspector.paint(canvas);
    auto c = inspector.zoom_center_color();
    REQUIRE(c.r == Catch::Approx(kPanelColor.r));
    REQUIRE(c.g == Catch::Approx(kPanelColor.g));
    REQUIRE(c.b == Catch::Approx(kPanelColor.b));

    // Outside any view with a background: transparent.
    inspector.handle_mouse_event(move_to(10, 10));
    inspector.paint(canvas);
    REQUIRE(inspector.zoom_center_color().a == Catch::Approx(0.0f));
}

TEST_CASE("the deepest background under the point wins, matching paint order",
          "[pulp-vellum-components][inspector-overlay][loupe]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    auto outer = std::make_unique<View>();
    outer->set_bounds({20, 20, 300, 200});
    outer->set_background_color(Color::rgba(1.0f, 0.0f, 0.0f, 1.0f));
    auto inner = std::make_unique<View>();
    inner->set_bounds({40, 40, 100, 100}); // (60, 60) .. (160, 160) in root space
    inner->set_background_color(Color::rgba(0.0f, 1.0f, 0.0f, 1.0f));
    outer->add_child(std::move(inner));
    root.add_child(std::move(outer));

    InspectorOverlay inspector(root);
    inspector.set_active(true);
    inspector.set_zoom_active(true);
    RecordingCanvas canvas;

    inspector.handle_mouse_event(move_to(100, 100));
    inspector.paint(canvas);
    REQUIRE(inspector.zoom_center_color().g == Catch::Approx(1.0f));
    REQUIRE(inspector.zoom_center_color().r == Catch::Approx(0.0f));

    inspector.handle_mouse_event(move_to(30, 30));
    inspector.paint(canvas);
    REQUIRE(inspector.zoom_center_color().r == Catch::Approx(1.0f));
}

TEST_CASE("the zoom factor clamps to a usable range",
          "[pulp-vellum-components][inspector-overlay][loupe]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorOverlay inspector(root);
    REQUIRE(inspector.zoom_factor() == 20);
    inspector.set_zoom_factor(8);
    REQUIRE(inspector.zoom_factor() == 8);
    inspector.set_zoom_factor(1);
    REQUIRE(inspector.zoom_factor() == 4);
    inspector.set_zoom_factor(1000);
    REQUIRE(inspector.zoom_factor() == 40);
}

TEST_CASE("the loupe keeps its read window on the surface at every corner",
          "[pulp-vellum-components][inspector-overlay][loupe]") {
    constexpr int kW = 400, kH = 300;
    View root;
    root.set_bounds({0, 0, static_cast<float>(kW), static_cast<float>(kH)});
    InspectorOverlay inspector(root);
    inspector.set_active(true);
    inspector.set_zoom_active(true);

    const Color kReadback = Color::rgba(64.0f / 255.0f, 128.0f / 255.0f, 192.0f / 255.0f, 1.0f);
    const float corners[4][2] = {{0, 0}, {kW, 0}, {0, kH}, {kW, kH}};
    for (const auto& corner : corners) {
        inspector.handle_mouse_event(move_to(corner[0], corner[1]));
        ReadbackCanvas canvas(kW, kH);
        inspector.paint(canvas);

        // The block read must have succeeded, which means it was clamped in.
        REQUIRE(canvas.reads >= 1);
        REQUIRE(canvas.last_x >= 0);
        REQUIRE(canvas.last_y >= 0);
        REQUIRE(canvas.last_x + canvas.last_w <= kW);
        REQUIRE(canvas.last_y + canvas.last_h <= kH);

        // The centre readout agrees with the block: real readback, not the
        // fallback.
        const auto c = inspector.zoom_center_color();
        REQUIRE(c.r == Catch::Approx(kReadback.r));
        REQUIRE(c.g == Catch::Approx(kReadback.g));
        REQUIRE(c.b == Catch::Approx(kReadback.b));

        // Every grid cell carries the read colour, not the checkerboard.
        int cells = 0;
        for (const auto& cmd : canvas.commands()) {
            if (cmd.type == DrawCommand::Type::set_fill_color &&
                cmd.color.r == Catch::Approx(kReadback.r) &&
                cmd.color.g == Catch::Approx(kReadback.g) &&
                cmd.color.b == Catch::Approx(kReadback.b))
                ++cells;
        }
        REQUIRE(cells >= 100);
    }
}

TEST_CASE("a surface that cannot read back says so and draws a checkerboard",
          "[pulp-vellum-components][inspector-overlay][loupe]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorOverlay inspector(root);
    inspector.set_active(true);
    inspector.set_zoom_active(true);

    RecordingCanvas canvas;
    inspector.paint(canvas);
    bool said_so = false;
    for (const auto& cmd : canvas.commands())
        if (cmd.type == DrawCommand::Type::fill_text && cmd.text == "no readback")
            said_so = true;
    REQUIRE(said_so);

    ReadbackCanvas readable(400, 300);
    inspector.paint(readable);
    for (const auto& cmd : readable.commands())
        REQUIRE_FALSE((cmd.type == DrawCommand::Type::fill_text && cmd.text == "no readback"));
}

TEST_CASE("the loupe shows the real pixels under the pointer, magnified",
          "[pulp-vellum-components][inspector-overlay][loupe][pixels]") {
    View root;
    root.set_bounds({0, 0, 300, 340});
    auto red = std::make_unique<View>();
    place(*red, 100, 40, 20, 20);
    red->set_background_color(Color::rgba(0.9f, 0.1f, 0.1f, 1.0f));
    root.add_child(std::move(red));
    auto blue = std::make_unique<View>();
    place(*blue, 120, 40, 20, 20);
    blue->set_background_color(Color::rgba(0.1f, 0.2f, 0.9f, 1.0f));
    root.add_child(std::move(blue));

    InspectorOverlay inspector(root);
    inspector.set_active(true);
    inspector.set_zoom_active(true);
    // The seam between the red and blue squares, 5 px into the red side.
    inspector.handle_mouse_event(move_to(115, 50));
    install_inspector_hooks(inspector);
    const auto frame = render(root, 300, 340);
    uninstall_inspector_hooks();

    // The panel is 11 cells x 20 px plus 6 px padding, 12 px in from the left
    // and sitting 12 px above the bottom. The grid starts at (18, y0).
    const float grid = 11 * 20.0f;
    const float panel_h = grid + 12.0f + 36.0f;
    const float grid_y = 340.0f - panel_h - 12.0f + 6.0f;
    // The sample is x = 115, so the window spans x 110 to 120: cells 0 to 8
    // are red and cell 10 (x = 120) is blue. Cell 9 straddles the seam.
    const auto left_cell =
        frame.at(18 + 2 * 20 + 10, static_cast<std::uint32_t>(grid_y) + 4 * 20 + 6);
    const auto right_cell =
        frame.at(18 + 10 * 20 + 10, static_cast<std::uint32_t>(grid_y) + 4 * 20 + 6);
    CHECK(left_cell.r > 180);
    CHECK(left_cell.b < 100);
    CHECK(right_cell.b > 180);
    CHECK(right_cell.r < 100);

    // The panel's own padding is the near-black backdrop, not a saturated one.
    const auto backdrop = frame.at(14, static_cast<std::uint32_t>(grid_y) + 4 * 20);
    CHECK(backdrop.r < 60);
    CHECK(backdrop.g < 60);
    CHECK(backdrop.b < 60);
}
