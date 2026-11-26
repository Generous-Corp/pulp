#include <catch2/catch_test_macros.hpp>
#include <pulp/vellum_components/knob_panel.hpp>

#include <map>
#include <string>

using namespace pulp::vellum_components::overlay;

namespace {

// Every character is 6 wide, so a segment is 6 * length + 12.
float fixed_width(const std::string& text) {
    return 6.0f * static_cast<float>(text.size());
}

KnobSchema sample_schema() {
    KnobSchema schema;
    KnobSpec slider;
    slider.id = "minimalism";
    slider.label = "Minimalism";
    slider.kind = KnobKind::slider;
    slider.positions = {{"airy", 0.0}, {"balanced", 0.5}, {"dense", 1.0}};
    KnobSpec layout;
    layout.id = "layout";
    layout.kind = KnobKind::enumeration;
    layout.positions = {{"stacked", 0.0}, {"sidebar", 0.0}, {"grid", 0.0}};
    schema.knobs = {slider, layout};
    return schema;
}

} // namespace

TEST_CASE("a slider emits its numeric anchor and other kinds their label",
          "[pulp-vellum-components][knob-panel]") {
    KnobSpec slider;
    slider.kind = KnobKind::slider;
    REQUIRE(knob_position_value(slider, {"airy", 0.0}) == "0");
    REQUIRE(knob_position_value(slider, {"dense", 1.0}) == "1");
    REQUIRE(knob_position_value(slider, {"balanced", 0.5}) == "0.5");
    REQUIRE(knob_position_value(slider, {"third", 0.25}) == "0.25");

    KnobSpec toggle;
    toggle.kind = KnobKind::toggle;
    REQUIRE(knob_position_value(toggle, {"dark", 7.0}) == "dark");
    KnobSpec choice;
    choice.kind = KnobKind::enumeration;
    REQUIRE(knob_position_value(choice, {"grid", 7.0}) == "grid");
}

TEST_CASE("an empty schema with no agent lays out to nothing",
          "[pulp-vellum-components][knob-panel]") {
    const auto layout = layout_knob_panel({}, {}, false, 200.0f, 400.0f, fixed_width);
    REQUIRE(layout.titles.empty());
    REQUIRE(layout.segments.empty());
    REQUIRE_FALSE(layout.has_agent);
    REQUIRE(layout.height == 0.0f);
}

TEST_CASE("each knob is a title over a row of segments, the first position selected by default",
          "[pulp-vellum-components][knob-panel]") {
    const auto layout = layout_knob_panel(sample_schema(), {}, false, 400.0f, 400.0f, fixed_width);
    REQUIRE(layout.titles.size() == 2);
    REQUIRE(layout.titles[0].text == "Minimalism");
    // A knob with no label shows its id.
    REQUIRE(layout.titles[1].text == "layout");
    REQUIRE(layout.titles[1].y > layout.titles[0].y);

    REQUIRE(layout.segments.size() == 6);
    const auto& airy = layout.segments[0];
    REQUIRE(airy.knob_id == "minimalism");
    REQUIRE(airy.value == "0");
    REQUIRE(airy.label == "airy");
    REQUIRE(airy.selected);
    REQUIRE(airy.bounds.width == 6.0f * 4 + 12.0f);
    REQUIRE(airy.bounds.height == 16.0f);
    REQUIRE(airy.bounds.x == 0.0f);
    // Segments run left to right with a 4 px gap and share a row.
    const auto& balanced = layout.segments[1];
    REQUIRE(balanced.bounds.x == airy.bounds.width + 4.0f);
    REQUIRE(balanced.bounds.y == airy.bounds.y);
    REQUIRE_FALSE(balanced.selected);
    // The first segment sits under its title.
    REQUIRE(airy.bounds.y == layout.titles[0].y + 15.0f);
}

TEST_CASE("the selection map decides which segment is selected",
          "[pulp-vellum-components][knob-panel]") {
    const auto layout =
        layout_knob_panel(sample_schema(), {{"minimalism", "1"}, {"layout", "grid"}}, false, 400.0f,
                          400.0f, fixed_width);
    for (const auto& seg : layout.segments) {
        const bool expected = (seg.knob_id == "minimalism" && seg.value == "1") ||
                              (seg.knob_id == "layout" && seg.value == "grid");
        REQUIRE(seg.selected == expected);
    }
}

TEST_CASE("segments wrap to a new row at the right edge", "[pulp-vellum-components][knob-panel]") {
    // Three segments of 24, 54 and 36 wide in a 110 px panel: the third no
    // longer fits beside the first two and starts a second row.
    KnobSchema schema;
    KnobSpec knob;
    knob.id = "k";
    knob.positions = {{"ab", 0}, {"abcdefg", 0}, {"abcd", 0}};
    schema.knobs = {knob};
    const auto layout = layout_knob_panel(schema, {}, false, 110.0f, 400.0f, fixed_width);
    REQUIRE(layout.segments.size() == 3);
    REQUIRE(layout.segments[0].bounds.y == layout.segments[1].bounds.y);
    REQUIRE(layout.segments[2].bounds.x == 0.0f);
    REQUIRE(layout.segments[2].bounds.y == layout.segments[0].bounds.y + 16.0f + 4.0f);
}

TEST_CASE("a segment wider than the panel still gets its own row start",
          "[pulp-vellum-components][knob-panel]") {
    KnobSchema schema;
    KnobSpec knob;
    knob.id = "k";
    knob.positions = {{"a-very-long-segment-label", 0}, {"b", 0}};
    schema.knobs = {knob};
    const auto layout = layout_knob_panel(schema, {}, false, 60.0f, 400.0f, fixed_width);
    REQUIRE(layout.segments.size() == 2);
    // The first sits at the left edge even though it overflows, and does not
    // wrap off an empty row.
    REQUIRE(layout.segments[0].bounds.x == 0.0f);
    REQUIRE(layout.segments[0].bounds.y == layout.titles[0].y + 15.0f);
    REQUIRE(layout.segments[1].bounds.x == 0.0f);
    REQUIRE(layout.segments[1].bounds.y > layout.segments[0].bounds.y);
}

TEST_CASE("an unlabelled position shows its value", "[pulp-vellum-components][knob-panel]") {
    KnobSchema schema;
    KnobSpec knob;
    knob.id = "k";
    knob.kind = KnobKind::slider;
    knob.positions = {{"", 0.5}};
    schema.knobs = {knob};
    const auto layout = layout_knob_panel(schema, {}, false, 200.0f, 400.0f, fixed_width);
    REQUIRE(layout.segments[0].label == "0.5");
    REQUIRE(layout.segments[0].value == "0.5");
}

TEST_CASE("the agent field and Send button follow the knobs",
          "[pulp-vellum-components][knob-panel]") {
    const auto layout = layout_knob_panel(sample_schema(), {}, true, 200.0f, 400.0f, fixed_width);
    REQUIRE(layout.has_agent);
    const auto& last = layout.segments.back();
    REQUIRE(layout.agent_title_y > last.bounds.y);
    REQUIRE(layout.agent_field.y == layout.agent_title_y + 15.0f);
    REQUIRE(layout.agent_send.y == layout.agent_field.y);
    // Send is as wide as its label plus padding and is flush with the right
    // edge; the field fills what is left, less a 4 px gap.
    REQUIRE(layout.agent_send.width == 6.0f * 4 + 12.0f);
    REQUIRE(layout.agent_send.x + layout.agent_send.width == 200.0f);
    REQUIRE(layout.agent_field.x == 0.0f);
    REQUIRE(layout.agent_field.width == layout.agent_send.x - 4.0f);
    REQUIRE(layout.height > layout.agent_send.y + layout.agent_send.height);
}

TEST_CASE("an agent field alone lays out with no knobs", "[pulp-vellum-components][knob-panel]") {
    const auto layout = layout_knob_panel({}, {}, true, 200.0f, 400.0f, fixed_width);
    REQUIRE(layout.titles.empty());
    REQUIRE(layout.has_agent);
    REQUIRE(layout.agent_field.width > 0.0f);
}

TEST_CASE("what does not fit is left out whole knob first, and the field last",
          "[pulp-vellum-components][knob-panel]") {
    const auto full = layout_knob_panel(sample_schema(), {}, true, 400.0f, 1000.0f, fixed_width);
    REQUIRE(full.has_agent);

    // Tall enough for the first knob but not the second.
    const auto first_only =
        layout_knob_panel(sample_schema(), {}, true, 400.0f, 45.0f, fixed_width);
    REQUIRE(first_only.titles.size() == 1);
    REQUIRE(first_only.segments.size() == 3);
    REQUIRE_FALSE(first_only.has_agent);
    REQUIRE(first_only.height <= 45.0f);

    // Too short for any knob.
    const auto none = layout_knob_panel(sample_schema(), {}, false, 400.0f, 20.0f, fixed_width);
    REQUIRE(none.titles.empty());
    REQUIRE(none.segments.empty());
}

TEST_CASE("the agent request carries the text, the context and the host's timestamp",
          "[pulp-vellum-components][knob-panel]") {
    AgentRequestContext context;
    context.design = "synth-ui";
    context.screen = "main";
    context.editmode_state = R"({"appearance":"dark"})";
    context.screenshot_path = "/tmp/shot.png";
    const auto payload = build_agent_request("make it airy", context, "2026-07-06T18:00:00Z");
    REQUIRE(payload.text == "make it airy");
    REQUIRE(payload.design == "synth-ui");
    REQUIRE(payload.screen == "main");
    REQUIRE(payload.editmode_state == R"({"appearance":"dark"})");
    REQUIRE(payload.screenshot_path == "/tmp/shot.png");
    REQUIRE(payload.created_at == "2026-07-06T18:00:00Z");

    // Context and timestamp are optional.
    const auto bare = build_agent_request("hello", {}, {});
    REQUIRE(bare.text == "hello");
    REQUIRE(bare.design.empty());
    REQUIRE(bare.created_at.empty());
}
