#include <catch2/catch_test_macros.hpp>
#include <pulp/vellum_components/inspector_overlay.hpp>
#include <pulp/vellum_components/knob_panel.hpp>

#include <vellum/canvas/recording_canvas.hpp>
#include <vellum/view/view.hpp>
#include <vellum/view/widgets.hpp>

#include "render_helpers.hpp"

#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace pulp::vellum_components;
using namespace pulp::vellum_components::testing;
using overlay::KnobKind;
using overlay::KnobSchema;
using overlay::KnobSpec;
using ::vellum::canvas::RecordingCanvas;
using ::vellum::view::KeyCode;
using ::vellum::view::KeyEvent;
using ::vellum::view::kModShift;
using ::vellum::view::MouseEvent;
using ::vellum::view::Point;
using ::vellum::view::TextInputEvent;

namespace {

KnobSchema make_schema() {
    KnobSchema schema;
    KnobSpec slider;
    slider.id = "minimalism";
    slider.label = "Minimalism";
    slider.kind = KnobKind::slider;
    slider.positions = {{"airy", 0.0}, {"balanced", 0.5}, {"dense", 1.0}};
    KnobSpec layout;
    layout.id = "layout";
    layout.label = "Layout";
    layout.kind = KnobKind::enumeration;
    layout.positions = {{"stacked", 0.0}, {"sidebar", 0.0}, {"grid", 0.0}};
    KnobSpec appearance;
    appearance.id = "appearance";
    appearance.label = "Appearance";
    appearance.kind = KnobKind::toggle;
    appearance.positions = {{"light", 0.0}, {"dark", 0.0}};
    schema.knobs = {slider, layout, appearance};
    return schema;
}

// An overlay with the knob panel up, recording every apply and every request.
struct KnobScene {
    View root;
    InspectorOverlay overlay{root};
    RecordingCanvas canvas;
    std::optional<InspectorOverlay::KnobApplyEdit> last_knob;
    int knob_applies = 0;
    std::vector<std::string> agent_texts;

    explicit KnobScene(bool wire_sinks = true) {
        root.set_bounds({0, 0, 500, 500});
        overlay.set_active(true);
        overlay.set_knob_schema(make_schema());
        overlay.set_knob_panel_visible(true);
        if (wire_sinks) {
            overlay.set_knob_apply_sink([this](const InspectorOverlay::KnobApplyEdit& e) {
                last_knob = e;
                ++knob_applies;
            });
            overlay.set_agent_request_sink([this](const InspectorOverlay::AgentRequestEdit& e) {
                agent_texts.push_back(e.text);
            });
        }
        repaint();
    }

    void repaint() {
        canvas.clear();
        overlay.paint(canvas);
    }

    Point knob_center(const std::string& id, const std::string& value) const {
        for (const auto& h : overlay.knob_hits())
            if (h.knob_id == id && h.value == value)
                return {h.bounds.x + h.bounds.width / 2, h.bounds.y + h.bounds.height / 2};
        return {-1, -1};
    }

    bool click(Point p) {
        MouseEvent e;
        e.position = p;
        e.is_down = true;
        return overlay.handle_mouse_event(e);
    }

    void focus_field() {
        const auto fb = overlay.agent_field_bounds();
        click({fb.x + fb.width / 2, fb.y + fb.height / 2});
    }

    bool type(const std::string& text) {
        TextInputEvent e;
        e.text = text;
        return overlay.handle_text_input(e);
    }

    bool key(KeyCode code, std::uint16_t modifiers = 0) {
        KeyEvent e;
        e.key = code;
        e.modifiers = modifiers;
        e.is_down = true;
        return overlay.handle_key_event(e);
    }
};

} // namespace

TEST_CASE("paint fills the segment and agent hit rectangles", "[pulp-vellum-components][knobs]") {
    KnobScene s;
    // Three slider positions, three enum positions and two toggle positions.
    REQUIRE(s.overlay.knob_hits().size() == 8);
    REQUIRE(s.overlay.agent_field_bounds().width > 0.0f);
    REQUIRE(s.overlay.agent_send_bounds().width > 0.0f);
    const auto panel = s.overlay.knob_panel_bounds();
    REQUIRE(panel.width > 0.0f);
    // Docked to the bottom right with a 12 px margin.
    REQUIRE(panel.x + panel.width == 500.0f - 12.0f);
    REQUIRE(panel.y + panel.height == 500.0f - 12.0f);
    // Every control lies inside the panel.
    for (const auto& h : s.overlay.knob_hits()) {
        REQUIRE(h.bounds.x >= panel.x);
        REQUIRE(h.bounds.x + h.bounds.width <= panel.x + panel.width);
        REQUIRE(h.bounds.y >= panel.y);
        REQUIRE(h.bounds.y + h.bounds.height <= panel.y + panel.height);
    }
}

TEST_CASE("clicking a slider segment reports its numeric anchor",
          "[pulp-vellum-components][knobs]") {
    KnobScene s;
    const auto dense = s.knob_center("minimalism", "1");
    REQUIRE(dense.x >= 0.0f);
    REQUIRE(s.click(dense));
    REQUIRE(s.last_knob.has_value());
    REQUIRE(s.last_knob->knob_id == "minimalism");
    REQUIRE(s.last_knob->value == "1");
    REQUIRE(s.overlay.knob_selection("minimalism") == "1");

    s.repaint();
    const auto airy = s.knob_center("minimalism", "0");
    s.click(airy);
    REQUIRE(s.last_knob->value == "0");
    REQUIRE(s.overlay.knob_selection("minimalism") == "0");

    s.repaint();
    s.click(s.knob_center("minimalism", "0.5"));
    REQUIRE(s.last_knob->value == "0.5");
}

TEST_CASE("clicking an enum segment reports its label", "[pulp-vellum-components][knobs]") {
    KnobScene s;
    const auto grid = s.knob_center("layout", "grid");
    REQUIRE(grid.x >= 0.0f);
    s.click(grid);
    REQUIRE(s.last_knob.has_value());
    REQUIRE(s.last_knob->knob_id == "layout");
    REQUIRE(s.last_knob->value == "grid");
    REQUIRE(s.overlay.knob_selection("layout") == "grid");
}

TEST_CASE("a toggle reports light or dark", "[pulp-vellum-components][knobs]") {
    KnobScene s;
    s.click(s.knob_center("appearance", "dark"));
    REQUIRE(s.last_knob.has_value());
    REQUIRE(s.last_knob->knob_id == "appearance");
    REQUIRE(s.last_knob->value == "dark");
}

TEST_CASE("the segment chosen last is the one painted selected",
          "[pulp-vellum-components][knobs]") {
    KnobScene s;
    s.click(s.knob_center("layout", "sidebar"));
    s.repaint();
    // The orange selected fill is set once for each selected segment: the
    // knobs' defaults (airy, stacked, light) and the sidebar choice replacing
    // stacked leave three.
    int selected_fills = 0;
    for (const auto& cmd : s.canvas.commands())
        if (cmd.type == ::vellum::canvas::DrawCommand::Type::set_fill_color && cmd.color.r > 0.9f &&
            cmd.color.g > 0.4f && cmd.color.g < 0.6f && cmd.color.b < 0.1f && cmd.color.a < 0.2f)
            ++selected_fills;
    REQUIRE(selected_fills == 3);
}

TEST_CASE("send to agent: focus, type, Enter", "[pulp-vellum-components][knobs]") {
    KnobScene s;
    s.focus_field();
    REQUIRE(s.overlay.agent_field_active());
    REQUIRE(s.type("make it airy"));
    REQUIRE(s.overlay.agent_text_buffer() == "make it airy");
    REQUIRE(s.key(KeyCode::enter));
    REQUIRE(s.agent_texts.size() == 1);
    REQUIRE(s.agent_texts[0] == "make it airy");
    REQUIRE(s.overlay.agent_text_buffer().empty());
    REQUIRE_FALSE(s.overlay.agent_field_active());
}

TEST_CASE("the Send button sends the draft", "[pulp-vellum-components][knobs]") {
    KnobScene s;
    s.focus_field();
    s.type("tighten the header");
    s.repaint();
    const auto sb = s.overlay.agent_send_bounds();
    REQUIRE(s.click({sb.x + sb.width / 2, sb.y + sb.height / 2}));
    REQUIRE(s.agent_texts.size() == 1);
    REQUIRE(s.agent_texts[0] == "tighten the header");
    REQUIRE(s.overlay.agent_text_buffer().empty());
}

TEST_CASE("an empty send is a no-op", "[pulp-vellum-components][knobs]") {
    KnobScene s;
    s.focus_field();
    REQUIRE(s.overlay.agent_field_active());
    REQUIRE(s.key(KeyCode::enter));
    REQUIRE(s.agent_texts.empty());
}

TEST_CASE("Escape defocuses the field and keeps the draft", "[pulp-vellum-components][knobs]") {
    KnobScene s;
    s.focus_field();
    s.type("wip draft");
    REQUIRE(s.key(KeyCode::escape));
    REQUIRE_FALSE(s.overlay.agent_field_active());
    REQUIRE(s.overlay.agent_text_buffer() == "wip draft");
    REQUIRE(s.agent_texts.empty());
}

TEST_CASE("Backspace trims one character, a whole code point for a multibyte one",
          "[pulp-vellum-components][knobs]") {
    KnobScene s;
    s.focus_field();
    s.type("abc");
    REQUIRE(s.key(KeyCode::backspace));
    REQUIRE(s.overlay.agent_text_buffer() == "ab");
    s.type("\xC3\xA9");
    REQUIRE(s.key(KeyCode::backspace));
    REQUIRE(s.overlay.agent_text_buffer() == "ab");
}

TEST_CASE("Left and Right move the caret and typing inserts there",
          "[pulp-vellum-components][knobs]") {
    KnobScene s;
    s.focus_field();
    s.type("ac");
    REQUIRE(s.key(KeyCode::left));
    s.type("b");
    REQUIRE(s.overlay.agent_text_buffer() == "abc");
    REQUIRE(s.key(KeyCode::right));
    s.type("d");
    REQUIRE(s.overlay.agent_text_buffer() == "abcd");
    REQUIRE(s.key(KeyCode::left));
    REQUIRE(s.key(KeyCode::backspace));
    REQUIRE(s.overlay.agent_text_buffer() == "abd");
}

TEST_CASE("a focused field declines other keys and flips no mode",
          "[pulp-vellum-components][knobs]") {
    KnobScene s;
    s.focus_field();
    REQUIRE_FALSE(s.key(KeyCode::z));
    REQUIRE_FALSE(s.key(KeyCode::m));
    REQUIRE_FALSE(s.key(KeyCode::p));
    REQUIRE_FALSE(s.overlay.zoom_active());
    REQUIRE_FALSE(s.overlay.pass_viewer_enabled());
    REQUIRE(s.overlay.agent_text_buffer().empty());
}

TEST_CASE("text input is declined while the field has no focus",
          "[pulp-vellum-components][knobs]") {
    KnobScene s;
    REQUIRE_FALSE(s.type("hello"));
    REQUIRE(s.overlay.agent_text_buffer().empty());
}

TEST_CASE("with no sinks attached the knobs still track a choice and the field is absent",
          "[pulp-vellum-components][knobs]") {
    KnobScene s(false);
    const auto grid = s.knob_center("layout", "grid");
    REQUIRE(grid.x >= 0.0f);
    s.click(grid);
    REQUIRE(s.overlay.knob_selection("layout") == "grid");
    REQUIRE(s.overlay.agent_field_bounds().width == 0.0f);
    REQUIRE(s.overlay.agent_send_bounds().width == 0.0f);
    REQUIRE_FALSE(s.overlay.has_agent_request_sink());
}

TEST_CASE("a press on the panel's background is taken and defocuses the field",
          "[pulp-vellum-components][knobs]") {
    KnobScene s;
    auto view = std::make_unique<View>();
    view->set_bounds({0, 0, 500, 500});
    View* behind = view.get();
    s.root.add_child(std::move(view));
    s.focus_field();
    REQUIRE(s.overlay.agent_field_active());

    const auto panel = s.overlay.knob_panel_bounds();
    // The top-left padding corner holds no control.
    REQUIRE(s.click({panel.x + 2, panel.y + 2}));
    REQUIRE_FALSE(s.overlay.agent_field_active());
    // And nothing was selected behind it.
    REQUIRE(s.overlay.selected_view() != behind);
    REQUIRE(s.overlay.selected_view() == nullptr);
}

TEST_CASE("a press outside the panel selects as usual", "[pulp-vellum-components][knobs]") {
    KnobScene s;
    auto view = std::make_unique<View>();
    view->set_bounds({10, 10, 100, 100});
    View* target = view.get();
    s.root.add_child(std::move(view));
    REQUIRE(s.click({50, 50}));
    REQUIRE(s.overlay.selected_view() == target);
}

TEST_CASE("Shift+T shows and hides the panel, and a hidden panel takes no press",
          "[pulp-vellum-components][knobs]") {
    KnobScene s;
    REQUIRE(s.overlay.knob_panel_visible());
    REQUIRE(s.key(KeyCode::t, kModShift));
    REQUIRE_FALSE(s.overlay.knob_panel_visible());
    s.repaint();
    // Nothing is clickable while it is hidden.
    REQUIRE(s.overlay.knob_hits().empty());
    REQUIRE(s.overlay.knob_panel_bounds().width == 0.0f);
    REQUIRE(s.overlay.agent_field_bounds().width == 0.0f);
    REQUIRE(s.knob_applies == 0);
    // A bare T is the Text tool, not the panel.
    REQUIRE(s.key(KeyCode::t));
    REQUIRE_FALSE(s.overlay.knob_panel_visible());
    REQUIRE(s.key(KeyCode::t, kModShift));
    REQUIRE(s.overlay.knob_panel_visible());
}

TEST_CASE("a press at a control's old place after the panel is hidden applies nothing",
          "[pulp-vellum-components][knobs]") {
    KnobScene s;
    const auto grid = s.knob_center("layout", "grid");
    // Hidden by key, with no repaint in between, so the last layout is stale.
    REQUIRE(s.key(KeyCode::t, kModShift));
    s.click(grid);
    REQUIRE(s.knob_applies == 0);
    REQUIRE(s.overlay.knob_selection("layout").empty());
}

TEST_CASE("a focused field paints a translucent orange fill and an orange caret",
          "[pulp-vellum-components][knobs]") {
    KnobScene s;
    s.focus_field();
    s.type("ab");
    s.repaint();
    bool fill = false;
    bool caret = false;
    for (const auto& cmd : s.canvas.commands()) {
        const auto& c = cmd.color;
        const bool orange = std::abs(c.r - 0.95f) < 0.01f && std::abs(c.g - 0.6f) < 0.01f &&
                            std::abs(c.b - 0.2f) < 0.01f;
        if (cmd.type == ::vellum::canvas::DrawCommand::Type::set_fill_color && orange &&
            std::abs(c.a - 0.18f) < 0.01f)
            fill = true;
        if (cmd.type == ::vellum::canvas::DrawCommand::Type::set_stroke_color && orange &&
            std::abs(c.a - 1.0f) < 0.01f)
            caret = true;
    }
    REQUIRE(fill);
    REQUIRE(caret);
}

TEST_CASE("a panel with no schema and no agent paints nothing", "[pulp-vellum-components][knobs]") {
    View root;
    root.set_bounds({0, 0, 500, 500});
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_knob_panel_visible(true);
    RecordingCanvas canvas;
    overlay.paint(canvas);
    REQUIRE(overlay.knob_panel_bounds().width == 0.0f);
    REQUIRE(overlay.knob_hits().empty());
}

TEST_CASE("deactivating the overlay takes the keyboard from the field and keeps the draft",
          "[pulp-vellum-components][knobs]") {
    KnobScene s;
    s.focus_field();
    s.type("draft");
    s.overlay.set_active(false);
    REQUIRE_FALSE(s.overlay.agent_field_active());
    REQUIRE(s.overlay.agent_text_buffer() == "draft");
    s.overlay.set_active(true);
    REQUIRE_FALSE(s.type("more"));
}

TEST_CASE("focusing the field ends an open text edit", "[pulp-vellum-components][knobs]") {
    KnobScene s;
    auto label = std::make_unique<::vellum::view::Label>("copy");
    label->set_anchor_id("a");
    label->set_bounds({10, 10, 80, 20});
    ::vellum::view::Label* label_ptr = label.get();
    s.root.add_child(std::move(label));
    REQUIRE(s.overlay.begin_text_edit(label_ptr));
    s.overlay.handle_text_input(TextInputEvent{"!"});
    s.focus_field();
    REQUIRE_FALSE(s.overlay.text_editing());
    REQUIRE(s.overlay.agent_field_active());
    // The edit was committed, not dropped.
    REQUIRE(label_ptr->text() == "copy!");
}

TEST_CASE("the hand-off builds the payload from the context and the clock",
          "[pulp-vellum-components][knobs]") {
    KnobScene s;
    std::vector<overlay::AgentRequestPayload> handed;
    s.overlay.set_agent_request_handoff(
        [&](const overlay::AgentRequestPayload& p) { handed.push_back(p); },
        [] {
            overlay::AgentRequestContext c;
            c.design = "synth-ui";
            c.screen = "main";
            c.editmode_state = R"({"appearance":"dark"})";
            c.screenshot_path = "/tmp/shot.png";
            return c;
        },
        [] { return std::string("2026-07-06T18:00:00Z"); });
    REQUIRE(s.overlay.has_agent_request_sink());
    s.repaint();
    s.focus_field();
    s.type("make the header tighter");
    REQUIRE(s.key(KeyCode::enter));

    REQUIRE(handed.size() == 1);
    REQUIRE(handed[0].text == "make the header tighter");
    REQUIRE(handed[0].design == "synth-ui");
    REQUIRE(handed[0].screen == "main");
    REQUIRE(handed[0].editmode_state == R"({"appearance":"dark"})");
    REQUIRE(handed[0].screenshot_path == "/tmp/shot.png");
    REQUIRE(handed[0].created_at == "2026-07-06T18:00:00Z");
    REQUIRE(s.overlay.agent_text_buffer().empty());
}

TEST_CASE("a hand-off with no context or clock still hands the text over",
          "[pulp-vellum-components][knobs]") {
    KnobScene s(false);
    std::vector<overlay::AgentRequestPayload> handed;
    s.overlay.set_agent_request_handoff(
        [&](const overlay::AgentRequestPayload& p) { handed.push_back(p); });
    s.repaint();
    s.focus_field();
    s.type("hello");
    s.key(KeyCode::enter);
    REQUIRE(handed.size() == 1);
    REQUIRE(handed[0].text == "hello");
    REQUIRE(handed[0].design.empty());
    REQUIRE(handed[0].created_at.empty());
}

TEST_CASE("a null hand-off removes the sink and the field with it",
          "[pulp-vellum-components][knobs]") {
    KnobScene s;
    REQUIRE(s.overlay.has_agent_request_sink());
    s.overlay.set_agent_request_handoff({});
    REQUIRE_FALSE(s.overlay.has_agent_request_sink());
    s.repaint();
    REQUIRE(s.overlay.agent_field_bounds().width == 0.0f);
}

TEST_CASE("the panel renders docked at the bottom right with the chosen segment lit",
          "[pulp-vellum-components][knobs][pixels]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_knob_schema(make_schema());
    overlay.set_knob_panel_visible(true);
    overlay.set_agent_request_sink([](const InspectorOverlay::AgentRequestEdit&) {});
    install_inspector_hooks(overlay);

    // A first paint lays the panel out so a segment can be chosen by its place.
    RecordingCanvas warm;
    overlay.paint(warm);
    Point grid{};
    for (const auto& h : overlay.knob_hits())
        if (h.knob_id == "layout" && h.value == "grid")
            grid = {h.bounds.x + h.bounds.width / 2, h.bounds.y + h.bounds.height / 2};
    MouseEvent click;
    click.position = grid;
    click.is_down = true;
    REQUIRE(overlay.handle_mouse_event(click));

    const auto frame = render(root, 400, 300);
    maybe_write_png(root, 400, 300, "inspector-overlay-knob-panel.png");
    uninstall_inspector_hooks();

    const auto panel = overlay.knob_panel_bounds();
    REQUIRE(panel.width == 260.0f);
    // The panel background is dark and the area above-left of it is not.
    const auto inside =
        frame.at(static_cast<std::uint32_t>(panel.x + 2), static_cast<std::uint32_t>(panel.y + 2));
    REQUIRE(inside.r < 60);
    REQUIRE(inside.b < 70);

    // The chosen "grid" segment carries the orange selected fill and stroke
    // (warmer than blue), and a segment that is not chosen ("sidebar") is
    // blue-grey throughout.
    const auto orange_in = [&](const overlay::Rect& r) {
        int n = 0;
        for (auto y = static_cast<std::uint32_t>(r.y);
             y < static_cast<std::uint32_t>(r.y + r.height); ++y)
            for (auto x = static_cast<std::uint32_t>(r.x);
                 x < static_cast<std::uint32_t>(r.x + r.width); ++x) {
                const auto p = frame.at(x, y);
                if (p.r > p.b + 20)
                    ++n;
            }
        return n;
    };
    overlay::Rect grid_rect;
    overlay::Rect sidebar_rect;
    for (const auto& h : overlay.knob_hits()) {
        if (h.knob_id == "layout" && h.value == "grid")
            grid_rect = h.bounds;
        if (h.knob_id == "layout" && h.value == "sidebar")
            sidebar_rect = h.bounds;
    }
    REQUIRE(orange_in(grid_rect) > 100);
    REQUIRE(orange_in(sidebar_rect) == 0);
    // An unselected segment is a translucent blue over the panel, not white.
    int blue = 0;
    for (auto y = static_cast<std::uint32_t>(sidebar_rect.y);
         y < static_cast<std::uint32_t>(sidebar_rect.y + sidebar_rect.height); ++y)
        for (auto x = static_cast<std::uint32_t>(sidebar_rect.x);
             x < static_cast<std::uint32_t>(sidebar_rect.x + sidebar_rect.width); ++x) {
            const auto p = frame.at(x, y);
            if (p.b > p.r + 20 && p.b < 120)
                ++blue;
        }
    REQUIRE(blue > 100);
}
