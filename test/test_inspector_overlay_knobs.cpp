// Inspector overlay: semantic-knob controls + send-to-agent text field.
//
// The tweaks panel's top region renders declared knobs (each a bundle of writes
// behind one control) and a free-text "ask agent" field. Choosing a knob value
// emits (knob_id, value) through a sink; the field emits its text through
// another. The overlay owns no files — it only resolves clicks/keys to intents —
// so the whole surface is driven headless here: paint into a RecordingCanvas to
// populate the hit-rects, then feed mouse/key/text events through the public
// entry points and assert the sinks fired.

#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/inspector_overlay.hpp>
#include <pulp/inspect/tweak_store.hpp>
#include <pulp/design/design_knobs.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/canvas/canvas.hpp>

#include <optional>
#include <string>
#include <vector>

using namespace pulp::view;
using namespace pulp::inspect;

namespace {

// A minimal design's authored knobs: a "minimalism" slider (numeric anchors) and
// a "layout" variant enum. The builtin theme-flip is appended in the scene.
constexpr const char* kKnobJson = R"({
  "knobs": [
    {"id":"minimalism","label":"Minimalism","kind":"slider","positions":[
      {"label":"airy","at":0,"writes":[{"key":"spacing.scale","value":"1.0"}]},
      {"label":"balanced","at":0.5,"writes":[{"key":"spacing.scale","value":"0.8"}]},
      {"label":"dense","at":1,"writes":[{"key":"spacing.scale","value":"0.6"}]}
    ]},
    {"id":"layout","label":"Layout","kind":"enum","positions":[
      {"label":"stacked","writes":[{"key":"layout.variant","value":"\"stacked\""}]},
      {"label":"sidebar","writes":[{"key":"layout.variant","value":"\"sidebar\""}]},
      {"label":"grid","writes":[{"key":"layout.variant","value":"\"grid\""}]}
    ]}
  ]
})";

struct KnobScene {
    View root;
    TweakStore store;
    InspectorOverlay overlay{root};
    pulp::canvas::RecordingCanvas canvas;

    std::optional<InspectorOverlay::KnobApplyEdit> last_knob;
    std::vector<std::string> agent_texts;

    explicit KnobScene(bool wire_sinks = true) {
        // Tall root so the tweaks-panel bottom third has room for every control.
        root.set_bounds({0, 0, 500, 1000});

        auto schema = pulp::design::parse_knob_schema(kKnobJson);
        REQUIRE(schema.has_value());
        schema->knobs.push_back(pulp::design::builtin_theme_flip());

        overlay.set_active(true);
        overlay.set_tweak_store(&store);
        overlay.set_tweaks_panel_visible(true);
        overlay.set_knob_schema(*schema);

        if (wire_sinks) {
            overlay.set_knob_apply_sink(
                [this](const InspectorOverlay::KnobApplyEdit& e) { last_knob = e; });
            overlay.set_agent_request_sink(
                [this](const InspectorOverlay::AgentRequestEdit& e) {
                    agent_texts.push_back(e.text);
                });
        }
        repaint();
    }

    void repaint() {
        canvas.clear();
        overlay.paint(canvas);
    }

    // Center of a knob segment matching (id, value), or {-1,-1} when absent.
    Point knob_center(const std::string& id, const std::string& value) const {
        for (const auto& h : overlay.knob_hits())
            if (h.knob_id == id && h.value == value)
                return {h.bounds.x + h.bounds.width / 2, h.bounds.y + h.bounds.height / 2};
        return {-1, -1};
    }

    void click(Point p) {
        MouseEvent e;
        e.position = p;
        e.is_down = true;
        overlay.handle_mouse_event(e);
    }

    void type(const std::string& text) {
        TextInputEvent e;
        e.text = text;
        overlay.handle_text_input(e);
    }

    bool key(KeyCode k) {
        KeyEvent e;
        e.key = k;
        e.is_down = true;
        return overlay.handle_key_event(e);
    }
};

}  // namespace

TEST_CASE("overlay knobs: paint populates knob + agent hit-rects", "[inspect][overlay][knobs]") {
    KnobScene s;
    REQUIRE_FALSE(s.overlay.knob_hits().empty());
    // Every authored position + the two theme-flip positions have a hit-rect.
    REQUIRE(s.overlay.knob_hits().size() >= 3 + 3 + 2);
    REQUIRE(s.overlay.agent_field_bounds().width > 0.0f);
    REQUIRE(s.overlay.agent_send_bounds().width > 0.0f);
}

TEST_CASE("overlay knobs: clicking a slider segment emits its numeric anchor",
          "[inspect][overlay][knobs]") {
    KnobScene s;
    // The slider emits the position's numeric anchor (what select_position
    // resolves), not the label.
    Point dense = s.knob_center("minimalism", "1");
    REQUIRE(dense.x >= 0.0f);
    s.click(dense);

    REQUIRE(s.last_knob.has_value());
    REQUIRE(s.last_knob->knob_id == "minimalism");
    REQUIRE(s.last_knob->value == "1");
    REQUIRE(s.overlay.knob_selection("minimalism") == "1");

    // Switching to another anchor updates the selection + re-emits.
    s.repaint();
    Point airy = s.knob_center("minimalism", "0");
    REQUIRE(airy.x >= 0.0f);
    s.click(airy);
    REQUIRE(s.last_knob->value == "0");
    REQUIRE(s.overlay.knob_selection("minimalism") == "0");
}

TEST_CASE("overlay knobs: clicking an enum segment emits its label",
          "[inspect][overlay][knobs]") {
    KnobScene s;
    Point grid = s.knob_center("layout", "grid");
    REQUIRE(grid.x >= 0.0f);
    s.click(grid);
    REQUIRE(s.last_knob.has_value());
    REQUIRE(s.last_knob->knob_id == "layout");
    REQUIRE(s.last_knob->value == "grid");
    REQUIRE(s.overlay.knob_selection("layout") == "grid");
}

TEST_CASE("overlay knobs: builtin theme flip emits light/dark", "[inspect][overlay][knobs]") {
    KnobScene s;
    const auto flip = pulp::design::builtin_theme_flip();
    Point dark = s.knob_center(flip.id, "dark");
    REQUIRE(dark.x >= 0.0f);
    s.click(dark);
    REQUIRE(s.last_knob.has_value());
    REQUIRE(s.last_knob->knob_id == flip.id);
    REQUIRE(s.last_knob->value == "dark");
}

TEST_CASE("overlay knobs: send-to-agent field focus, type, Enter", "[inspect][overlay][knobs]") {
    KnobScene s;
    Rect fb = s.overlay.agent_field_bounds();
    s.click({fb.x + fb.width / 2, fb.y + fb.height / 2});
    REQUIRE(s.overlay.agent_field_active());

    s.type("make it airy");
    REQUIRE(s.overlay.agent_text_buffer() == "make it airy");

    REQUIRE(s.key(KeyCode::enter));
    REQUIRE(s.agent_texts.size() == 1);
    REQUIRE(s.agent_texts[0] == "make it airy");
    REQUIRE(s.overlay.agent_text_buffer().empty());  // cleared on submit
    REQUIRE_FALSE(s.overlay.agent_field_active());    // defocused
}

TEST_CASE("overlay knobs: Send button submits", "[inspect][overlay][knobs]") {
    KnobScene s;
    Rect fb = s.overlay.agent_field_bounds();
    s.click({fb.x + fb.width / 2, fb.y + fb.height / 2});
    s.type("tighten the header");
    s.repaint();  // Send button lights up once the buffer is non-empty

    Rect sb = s.overlay.agent_send_bounds();
    s.click({sb.x + sb.width / 2, sb.y + sb.height / 2});
    REQUIRE(s.agent_texts.size() == 1);
    REQUIRE(s.agent_texts[0] == "tighten the header");
    REQUIRE(s.overlay.agent_text_buffer().empty());
}

TEST_CASE("overlay knobs: empty send is a no-op", "[inspect][overlay][knobs]") {
    KnobScene s;
    Rect fb = s.overlay.agent_field_bounds();
    s.click({fb.x + fb.width / 2, fb.y + fb.height / 2});
    REQUIRE(s.overlay.agent_field_active());
    REQUIRE(s.key(KeyCode::enter));  // nothing typed
    REQUIRE(s.agent_texts.empty());
}

TEST_CASE("overlay knobs: Esc defocuses but keeps the draft", "[inspect][overlay][knobs]") {
    KnobScene s;
    Rect fb = s.overlay.agent_field_bounds();
    s.click({fb.x + fb.width / 2, fb.y + fb.height / 2});
    s.type("wip draft");
    REQUIRE(s.key(KeyCode::escape));
    REQUIRE_FALSE(s.overlay.agent_field_active());
    REQUIRE(s.overlay.agent_text_buffer() == "wip draft");  // retained
    REQUIRE(s.agent_texts.empty());
}

TEST_CASE("overlay knobs: Backspace trims one character", "[inspect][overlay][knobs]") {
    KnobScene s;
    Rect fb = s.overlay.agent_field_bounds();
    s.click({fb.x + fb.width / 2, fb.y + fb.height / 2});
    s.type("abc");
    REQUIRE(s.key(KeyCode::backspace));
    REQUIRE(s.overlay.agent_text_buffer() == "ab");
}

TEST_CASE("overlay knobs: caret moves with Left/Right and inserts mid-string",
          "[inspect][overlay][knobs]") {
    KnobScene s;
    Rect fb = s.overlay.agent_field_bounds();
    s.click({fb.x + fb.width / 2, fb.y + fb.height / 2});
    s.type("ac");                       // "ac", caret at end
    REQUIRE(s.key(KeyCode::left));       // caret between a|c
    s.type("b");                         // insert at caret → "abc"
    REQUIRE(s.overlay.agent_text_buffer() == "abc");
    REQUIRE(s.key(KeyCode::right));       // caret past c
    s.type("d");                         // append → "abcd"
    REQUIRE(s.overlay.agent_text_buffer() == "abcd");
    // Backspace mid-string removes the char before the caret, not the tail.
    REQUIRE(s.key(KeyCode::left));        // caret between c|d
    REQUIRE(s.key(KeyCode::backspace));   // removes 'c' → "abd"
    REQUIRE(s.overlay.agent_text_buffer() == "abd");
}

TEST_CASE("overlay knobs: no sinks attached is safe", "[inspect][overlay][knobs]") {
    KnobScene s(/*wire_sinks=*/false);
    // Knobs render from the schema and track selection with no knob sink, never
    // crashing on a click.
    Point grid = s.knob_center("layout", "grid");
    REQUIRE(grid.x >= 0.0f);
    s.click(grid);
    REQUIRE(s.overlay.knob_selection("layout") == "grid");

    // The send-to-agent affordance is gated on an attached sink: with none, the
    // field + Send button are not rendered at all (a control that goes nowhere
    // would mislead), so there is nothing to focus.
    REQUIRE(s.overlay.agent_field_bounds().width == 0.0f);
    REQUIRE(s.overlay.agent_send_bounds().width == 0.0f);
}
