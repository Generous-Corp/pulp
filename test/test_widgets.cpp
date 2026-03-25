#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/canvas/canvas.hpp>

using namespace pulp::view;
using namespace pulp::canvas;
using Catch::Matchers::WithinAbs;

TEST_CASE("Label renders text", "[view][widget]") {
    Label label("Gain");
    label.set_bounds({0, 0, 100, 20});

    RecordingCanvas canvas;
    label.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_text) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::set_font) == 1);
}

TEST_CASE("Label text can be changed", "[view][widget]") {
    Label label("Initial");
    REQUIRE(label.text() == "Initial");

    label.set_text("Changed");
    REQUIRE(label.text() == "Changed");
}

TEST_CASE("Knob value clamping", "[view][widget]") {
    Knob knob;
    knob.set_value(0.5f);
    REQUIRE_THAT(knob.value(), WithinAbs(0.5, 0.001));

    knob.set_value(1.5f);
    REQUIRE_THAT(knob.value(), WithinAbs(1.0, 0.001));

    knob.set_value(-0.5f);
    REQUIRE_THAT(knob.value(), WithinAbs(0.0, 0.001));
}

TEST_CASE("Knob renders arcs and indicator", "[view][widget]") {
    Knob knob;
    knob.set_bounds({0, 0, 48, 48});
    knob.set_value(0.5f);
    knob.set_label("Gain");

    RecordingCanvas canvas;
    knob.paint(canvas);

    // Should draw: track arc, value arc, thumb line, label text
    REQUIRE(canvas.count(DrawCommand::Type::stroke_arc) == 2);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) == 1);
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

    // Track + fill = 2 rounded rects, thumb = 1 circle, label text
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 2);
    REQUIRE(canvas.count(DrawCommand::Type::fill_circle) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) >= 1);
}

TEST_CASE("Fader horizontal orientation", "[view][widget]") {
    Fader fader;
    fader.set_orientation(Fader::Orientation::horizontal);
    fader.set_bounds({0, 0, 200, 24});
    fader.set_value(0.5f);

    RecordingCanvas canvas;
    fader.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 2);
    REQUIRE(canvas.count(DrawCommand::Type::fill_circle) == 1);
}

TEST_CASE("Toggle state", "[view][widget]") {
    Toggle toggle;
    REQUIRE_FALSE(toggle.is_on());

    toggle.set_on(true);
    REQUIRE(toggle.is_on());
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
