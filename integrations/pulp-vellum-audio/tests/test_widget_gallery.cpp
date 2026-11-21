#include <catch2/catch_test_macros.hpp>
#include <pulp/vellum_audio/audio_widgets.hpp>
#include <pulp/vellum_audio/widget_gallery.hpp>
#include <vellum/canvas/canvas.hpp>
#include <vellum/view/gap_widgets.hpp>
#include <vellum/view/input_events.hpp>
#include <vellum/view/text_editor.hpp>
#include <vellum/view/theme_presets.hpp>
#include <vellum/view/ui_components.hpp>
#include <vellum/view/widgets.hpp>

#include <cstddef>
#include <vector>

using namespace pulp::vellum_audio;
using namespace vellum::view;

namespace {

Theme ink_signal(bool dark) {
    const auto* preset = find_preset("ink-signal");
    REQUIRE(preset != nullptr);
    return theme_from_preset(*preset, dark);
}

template <typename Widget> std::vector<Widget*> specimens(View& board) {
    std::vector<Widget*> result;
    for (std::size_t i = 0; i < board.child_count(); ++i) {
        if (auto* widget = dynamic_cast<Widget*>(board.child_at(i)))
            result.push_back(widget);
    }
    return result;
}

} // namespace

TEST_CASE("Vellum widget gallery fits its themed catalogue on the board", "[gallery][board]") {
    for (bool dark : {false, true}) {
        auto board = build_widget_gallery(ink_signal(dark));
        REQUIRE(board->bounds().width == GALLERY_WIDTH);
        REQUIRE(board->bounds().height > 600.0f);
        REQUIRE(specimens<Badge>(*board).size() == 5);
        REQUIRE(specimens<ChannelStrip>(*board).size() == 3);
        REQUIRE(specimens<TextEditor>(*board).size() == 2);
        REQUIRE(specimens<XYPad>(*board).size() == 1);
        for (std::size_t i = 0; i < board->child_count(); ++i) {
            const auto bounds = board->child_at(i)->bounds();
            CHECK(bounds.x >= 0.0f);
            CHECK(bounds.y >= 0.0f);
            CHECK(bounds.width > 0.0f);
            CHECK(bounds.height > 0.0f);
            CHECK(bounds.x + bounds.width <= board->bounds().width);
            CHECK(bounds.y + bounds.height <= board->bounds().height);
        }
    }
}

TEST_CASE("Vellum widget gallery shows the chart shader presets on knobs", "[gallery][knobs]") {
    auto board = build_widget_gallery(ink_signal(true));
    std::size_t charted = 0;
    for (const auto* knob : specimens<Knob>(*board)) {
        if (knob->chart_shader().empty())
            continue;
        ++charted;
        const auto error = vellum::canvas::Canvas::compile_sdf_chart_sksl(
            vellum::canvas::Canvas::SDFShape::flat_arc, knob->chart_shader());
        INFO(error);
        CHECK(error.empty());
    }
    REQUIRE(charted == 7);
}

TEST_CASE("Vellum widget gallery shows chart shader presets on fader tracks", "[gallery][faders]") {
    auto board = build_widget_gallery(ink_signal(true));
    int vertical = 0;
    int horizontal = 0;
    for (const auto* fader : specimens<Fader>(*board)) {
        if (fader->chart_shader().empty())
            continue;
        const auto shape = widget_chart_shape(*fader);
        REQUIRE(shape == vellum::canvas::Canvas::SDFShape::stadium);
        const auto error =
            vellum::canvas::Canvas::compile_sdf_chart_sksl(*shape, fader->chart_shader());
        INFO(error);
        CHECK(error.empty());
        (fader->orientation() == Fader::Orientation::vertical ? vertical : horizontal)++;
    }
    REQUIRE(vertical == 3);
    REQUIRE(horizontal == 2);
}

TEST_CASE("Vellum widget gallery provides seeded level meters and a playable keyboard",
          "[gallery][audio-controls]") {
    auto board = build_widget_gallery(ink_signal(true));
    const auto meters = specimens<LevelMeter>(*board);
    REQUIRE(meters.size() == 2);
    CHECK(meters[0]->rms() == 0.55f);
    CHECK(meters[0]->peak() == 0.82f);
    CHECK(meters[1]->rms() == 0.38f);
    CHECK(meters[1]->peak() == 0.64f);

    const auto keyboards = specimens<MidiKeyboard>(*board);
    REQUIRE(keyboards.size() == 1);
    auto& keyboard = *keyboards.front();
    CHECK(keyboard.first_note() == 48);
    CHECK(keyboard.last_note() == 72);
    int pressed = -1;
    int released = -1;
    keyboard.on_note_on = [&](int note, float) { pressed = note; };
    keyboard.on_note_off = [&](int note) { released = note; };
    const Point first_white_key{1.0f, keyboard.bounds().height - 1.0f};
    keyboard.on_mouse_down(first_white_key);
    CHECK(pressed == 48);
    CHECK(keyboard.is_note_on(48));
    keyboard.on_mouse_up(first_white_key);
    CHECK(released == 48);
    CHECK_FALSE(keyboard.is_note_on(48));
}

TEST_CASE("Vellum widget gallery scrolls to the full board in either axis", "[gallery][scroll]") {
    constexpr float viewport_width = 480.0f;
    constexpr float viewport_height = 600.0f;
    auto root = build_scrolling_widget_gallery(ink_signal(true), viewport_width, viewport_height);
    auto* scroll = dynamic_cast<ScrollView*>(root.get());
    REQUIRE(scroll != nullptr);
    REQUIRE(scroll->child_count() == 1);
    CHECK(scroll->bounds().width == viewport_width);
    CHECK(scroll->bounds().height == viewport_height);
    CHECK(scroll->content_size().width == scroll->child_at(0)->bounds().width);
    CHECK(scroll->content_size().height == scroll->child_at(0)->bounds().height);

    MouseEvent wheel{};
    wheel.is_wheel = true;
    wheel.scroll_delta_x = 1e6f;
    wheel.scroll_delta_y = 1e6f;
    scroll->on_mouse_event(wheel);
    CHECK(scroll->scroll_x() == scroll->content_size().width - viewport_width);
    CHECK(scroll->scroll_y() == scroll->content_size().height - viewport_height);
}
