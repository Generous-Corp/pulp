#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/inspect/inspector_window.hpp>
#include <pulp/view/inspector.hpp>
#include <pulp/view/widgets.hpp>

#include <cstddef>
#include <functional>
#include <string_view>
#include <tuple>
#include <vector>

using namespace pulp::view;
using namespace pulp::inspect;

namespace {

template <typename T>
void collect_views_of_type(View& root, std::vector<T*>& out) {
    if (auto* match = dynamic_cast<T*>(&root))
        out.push_back(match);
    for (size_t i = 0; i < root.child_count(); ++i)
        collect_views_of_type(*root.child_at(i), out);
}

template <typename T>
std::vector<T*> collect_views_of_type(View& root) {
    std::vector<T*> out;
    collect_views_of_type(root, out);
    return out;
}

template <typename T>
T* first_view_of_type(View& root) {
    auto matches = collect_views_of_type<T>(root);
    return matches.empty() ? nullptr : matches.front();
}

bool has_label_containing(View& root, std::string_view text) {
    auto labels = collect_views_of_type<Label>(root);
    for (auto* label : labels) {
        if (label->text().find(text) != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

TEST_CASE("ViewInspector type_name", "[view][inspector]") {
    View v;
    Knob k;
    Fader f;
    Toggle t;
    Label l;
    Meter m;
    XYPad xy;
    WaveformView wf;
    SpectrumView sp;

    REQUIRE(ViewInspector::type_name(v) == "View");
    REQUIRE(ViewInspector::type_name(k) == "Knob");
    REQUIRE(ViewInspector::type_name(f) == "Fader");
    REQUIRE(ViewInspector::type_name(t) == "Toggle");
    REQUIRE(ViewInspector::type_name(l) == "Label");
    REQUIRE(ViewInspector::type_name(m) == "Meter");
    REQUIRE(ViewInspector::type_name(xy) == "XYPad");
    REQUIRE(ViewInspector::type_name(wf) == "WaveformView");
    REQUIRE(ViewInspector::type_name(sp) == "SpectrumView");
}

TEST_CASE("ViewInspector count_views", "[view][inspector]") {
    View root;
    root.add_child(std::make_unique<Knob>());
    root.add_child(std::make_unique<Fader>());

    auto panel = std::make_unique<View>();
    panel->add_child(std::make_unique<Label>("test"));
    root.add_child(std::move(panel));

    REQUIRE(ViewInspector::count_views(root) == 5); // root + 2 + panel + label
}

TEST_CASE("ViewInspector find_by_id", "[view][inspector]") {
    View root;
    root.set_id("root");

    auto knob = std::make_unique<Knob>();
    knob->set_id("gain");
    auto* knob_ptr = knob.get();
    root.add_child(std::move(knob));

    auto fader = std::make_unique<Fader>();
    fader->set_id("volume");
    root.add_child(std::move(fader));

    REQUIRE(ViewInspector::find_by_id(root, "root") == &root);
    REQUIRE(ViewInspector::find_by_id(root, "gain") == knob_ptr);
    REQUIRE(ViewInspector::find_by_id(root, "nonexistent") == nullptr);
}

TEST_CASE("ViewInspector to_json", "[view][inspector]") {
    View root;
    root.set_id("root");
    root.set_bounds({0, 0, 400, 300});

    auto knob = std::make_unique<Knob>();
    knob->set_id("gain");
    knob->set_bounds({10, 10, 48, 48});
    knob->set_value(0.75f);
    knob->set_label("Gain");
    root.add_child(std::move(knob));

    auto toggle = std::make_unique<Toggle>();
    toggle->set_id("bypass");
    toggle->set_on(true);
    root.add_child(std::move(toggle));

    auto json = ViewInspector::to_json(root);
    REQUIRE_FALSE(json.empty());

    // Verify key elements are present in the JSON
    REQUIRE(json.find("\"root\"") != std::string::npos);
    REQUIRE(json.find("\"Knob\"") != std::string::npos);
    REQUIRE(json.find("\"gain\"") != std::string::npos);
    REQUIRE(json.find("\"Toggle\"") != std::string::npos);
    REQUIRE(json.find("\"bypass\"") != std::string::npos);
    REQUIRE(json.find("children") != std::string::npos);
}

TEST_CASE("ViewInspector JSON includes widget values", "[view][inspector]") {
    View root;

    auto fader = std::make_unique<Fader>();
    fader->set_value(0.6f);
    fader->set_orientation(Fader::Orientation::horizontal);
    root.add_child(std::move(fader));

    auto label = std::make_unique<Label>("Hello");
    root.add_child(std::move(label));

    auto json = ViewInspector::to_json(root);
    REQUIRE(json.find("\"horizontal\"") != std::string::npos);
    REQUIRE(json.find("\"Hello\"") != std::string::npos);
}

// ── Protocol encode/decode ──────────────────────────────────────────────────

#include <pulp/inspect/protocol.hpp>

TEST_CASE("Protocol: encode request") {
    auto msg = make_request(1, "DOM.getDocument", R"({"depth":1})");
    auto json = encode_message(msg);
    REQUIRE(json.find("\"id\"") != std::string::npos);
    REQUIRE(json.find("DOM.getDocument") != std::string::npos);
}

TEST_CASE("Protocol: encode response") {
    auto msg = make_response(1, R"({"root":{}})");
    auto json = encode_message(msg);
    REQUIRE(json.find("\"id\"") != std::string::npos);
    REQUIRE(json.find("root") != std::string::npos);
}

TEST_CASE("Protocol: encode error") {
    auto msg = make_error(1, "View not found");
    auto json = encode_message(msg);
    REQUIRE(json.find("error") != std::string::npos);
    REQUIRE(json.find("View not found") != std::string::npos);
}

TEST_CASE("Protocol: encode event has no id") {
    auto msg = make_event("DOM.documentUpdated");
    auto json = encode_message(msg);
    REQUIRE(json.find("DOM.documentUpdated") != std::string::npos);
}

TEST_CASE("Protocol: decode roundtrip") {
    auto original = make_request(42, "State.getParameters", R"({"filter":"gain"})");
    auto json = encode_message(original);

    InspectorMessage decoded;
    REQUIRE(decode_message(json, decoded));
    REQUIRE(decoded.id == 42);
    REQUIRE(decoded.method == "State.getParameters");
}

TEST_CASE("Protocol: decode invalid JSON") {
    InspectorMessage msg;
    REQUIRE_FALSE(decode_message("not json", msg));
}

// ── InspectorOverlay ────────────────────────────────────────────────────────

#include <pulp/inspect/inspector_overlay.hpp>
#include <pulp/render/render_pass.hpp>

TEST_CASE("InspectorOverlay: toggle") {
    View root;
    InspectorOverlay overlay(root);
    REQUIRE_FALSE(overlay.is_active());
    overlay.set_active(true);
    REQUIRE(overlay.is_active());
    overlay.toggle();
    REQUIRE_FALSE(overlay.is_active());
}

TEST_CASE("InspectorOverlay: Cmd+I toggles") {
    View root;
    InspectorOverlay overlay(root);
    KeyEvent ke;
    ke.key = KeyCode::i;
#ifdef __APPLE__
    ke.modifiers = kModCmd;
#else
    ke.modifiers = kModCtrl;  // Ctrl+I on Linux/Windows
#endif
    ke.is_down = true;
    REQUIRE(overlay.handle_key_event(ke));
    REQUIRE(overlay.is_active());
    REQUIRE(overlay.handle_key_event(ke));
    REQUIRE_FALSE(overlay.is_active());
}

TEST_CASE("InspectorOverlay: Escape dismisses") {
    View root;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    KeyEvent ke;
    ke.key = KeyCode::escape;
    ke.is_down = true;
    REQUIRE(overlay.handle_key_event(ke));
    REQUIRE_FALSE(overlay.is_active());
}

TEST_CASE("InspectorOverlay: inactive ignores events") {
    View root;
    InspectorOverlay overlay(root);
    KeyEvent ke;
    ke.key = KeyCode::escape;
    ke.is_down = true;
    REQUIRE_FALSE(overlay.handle_key_event(ke));
    MouseEvent me;
    me.position = {10, 10};
    me.is_down = true;
    REQUIRE_FALSE(overlay.handle_mouse_event(me));
}

TEST_CASE("InspectorOverlay: mouse selection and panel tree interactions", "[inspect][overlay][issue-641]") {
    View root;
    root.set_id("root");
    root.set_bounds({0, 0, 500, 300});

    auto first = std::make_unique<View>();
    first->set_id("first");
    first->set_bounds({10, 10, 80, 40});
    first->flex().padding = 3;
    first->flex().margin = 4;
    auto* first_ptr = first.get();
    root.add_child(std::move(first));

    auto second = std::make_unique<View>();
    second->set_id("second");
    second->set_bounds({120, 10, 80, 40});
    auto* second_ptr = second.get();
    root.add_child(std::move(second));

    InspectorOverlay overlay(root);
    overlay.set_active(true);

    pulp::canvas::RecordingCanvas canvas;
    overlay.paint(canvas);
    REQUIRE(canvas.command_count() > 0);

    MouseEvent hover;
    hover.position = {20, 20};
    hover.is_down = false;
    REQUIRE_FALSE(overlay.handle_mouse_event(hover));
    REQUIRE(overlay.hovered_view() == first_ptr);

    MouseEvent click;
    click.position = {20, 20};
    click.is_down = true;
    REQUIRE(overlay.handle_mouse_event(click));
    REQUIRE(overlay.selected_view() == first_ptr);

    canvas.clear();
    overlay.paint(canvas);
    REQUIRE(canvas.command_count() > 0);

    MouseEvent panel_select;
    panel_select.position = {250, 25};
    panel_select.is_down = true;
    REQUIRE(overlay.handle_mouse_event(panel_select));
    REQUIRE(overlay.selected_view() == first_ptr);

    MouseEvent panel_collapse;
    panel_collapse.position = {205, 5};
    panel_collapse.is_down = true;
    REQUIRE(overlay.handle_mouse_event(panel_collapse));

    canvas.clear();
    overlay.paint(canvas);
    REQUIRE(canvas.command_count() > 0);

    overlay.set_active(false);
    REQUIRE(overlay.selected_view() == nullptr);
    REQUIRE(overlay.hovered_view() == nullptr);

    overlay.set_active(true);
    MouseEvent alt_first;
    alt_first.position = {20, 20};
    alt_first.modifiers = kModAlt;
    alt_first.is_down = true;
    REQUIRE(overlay.handle_mouse_event(alt_first));

    MouseEvent alt_second;
    alt_second.position = {130, 20};
    alt_second.modifiers = kModAlt;
    alt_second.is_down = true;
    REQUIRE(overlay.handle_mouse_event(alt_second));
    REQUIRE(overlay.selected_view() == second_ptr);
}

// Phase 3f — Alt-hover sibling distance (Figma-style spacing reveal).
// Click to select a node, then HOLD Alt while hovering another → overlay
// renders a live distance line from selection to hovered target. Released
// Alt clears the target. Distinct from the Alt+CLICK sticky-anchor mode
// covered above.
TEST_CASE("InspectorOverlay: Alt-hover reveals sibling distance line",
          "[inspect][overlay][phase3f]") {
    View root;
    root.set_id("root");
    root.set_bounds({0, 0, 500, 300});

    auto a = std::make_unique<View>();
    a->set_id("a");
    a->set_bounds({10, 10, 60, 60});
    auto* a_ptr = a.get();
    root.add_child(std::move(a));

    auto b = std::make_unique<View>();
    b->set_id("b");
    b->set_bounds({100, 10, 60, 60});
    auto* b_ptr = b.get();
    root.add_child(std::move(b));

    InspectorOverlay overlay(root);
    overlay.set_active(true);

    // Select a via click (no Alt).
    MouseEvent click_a;
    click_a.position = {20, 20};
    click_a.is_down = true;
    REQUIRE(overlay.handle_mouse_event(click_a));
    REQUIRE(overlay.selected_view() == a_ptr);

    // Hover over b WITHOUT Alt — no distance reveal; selection unchanged.
    MouseEvent hover_b;
    hover_b.position = {130, 30};
    hover_b.is_down = false;
    REQUIRE_FALSE(overlay.handle_mouse_event(hover_b));
    REQUIRE(overlay.hovered_view() == b_ptr);
    REQUIRE(overlay.selected_view() == a_ptr);

    // Paint with no Alt — single highlight, no Alt-hover line.
    pulp::canvas::RecordingCanvas no_alt_canvas;
    overlay.paint(no_alt_canvas);
    auto baseline_cmd_count = no_alt_canvas.command_count();
    REQUIRE(baseline_cmd_count > 0);

    // Now hover over b WITH Alt held → alt-hover target should be b.
    MouseEvent alt_hover_b;
    alt_hover_b.position = {130, 30};
    alt_hover_b.modifiers = kModAlt;
    alt_hover_b.is_down = false;
    REQUIRE_FALSE(overlay.handle_mouse_event(alt_hover_b));
    REQUIRE(overlay.hovered_view() == b_ptr);
    REQUIRE(overlay.selected_view() == a_ptr);  // selection unchanged by hover

    pulp::canvas::RecordingCanvas alt_canvas;
    overlay.paint(alt_canvas);
    // With Alt held + selection + hover, paint_distance_lines() draws an
    // extra line + label between selected and hovered. The exact command
    // count depends on canvas internals, but it MUST be greater than the
    // no-Alt baseline.
    REQUIRE(alt_canvas.command_count() > baseline_cmd_count);

    // Release Alt — alt_hover_target_ clears; render returns to baseline.
    MouseEvent release_alt;
    release_alt.position = {130, 30};
    release_alt.modifiers = 0;  // Alt released
    release_alt.is_down = false;
    REQUIRE_FALSE(overlay.handle_mouse_event(release_alt));

    pulp::canvas::RecordingCanvas after_release_canvas;
    overlay.paint(after_release_canvas);
    REQUIRE(after_release_canvas.command_count() == baseline_cmd_count);
}

TEST_CASE("InspectorOverlay: Alt-hover does nothing without a selection",
          "[inspect][overlay][phase3f]") {
    // Edge case: holding Alt while hovering with no selected_ should
    // NOT set alt_hover_target_. Otherwise we'd render a line from
    // nullptr → confused diagnostics.
    View root;
    root.set_bounds({0, 0, 200, 200});

    auto child = std::make_unique<View>();
    child->set_bounds({10, 10, 60, 60});
    root.add_child(std::move(child));

    InspectorOverlay overlay(root);
    overlay.set_active(true);

    pulp::canvas::RecordingCanvas baseline;
    overlay.paint(baseline);
    auto baseline_count = baseline.command_count();

    MouseEvent alt_hover_no_sel;
    alt_hover_no_sel.position = {30, 30};
    alt_hover_no_sel.modifiers = kModAlt;
    alt_hover_no_sel.is_down = false;
    overlay.handle_mouse_event(alt_hover_no_sel);

    pulp::canvas::RecordingCanvas after_alt_hover;
    overlay.paint(after_alt_hover);
    // No selection → no distance line should be added → command count
    // matches baseline.
    REQUIRE(after_alt_hover.command_count() == baseline_count);
}

// Codex P2 follow-up on #2328: Alt-hover state must clear when the
// cursor enters the inspector panel. Otherwise the live distance line
// keeps drawing from selected_ to a view that's no longer under the
// cursor.
TEST_CASE("InspectorOverlay: Alt-hover target clears when cursor enters panel "
          "(codex P2 #2328 regression)",
          "[inspect][overlay][phase3f][regression]") {
    View root;
    root.set_bounds({0, 0, 600, 400});
    auto a = std::make_unique<View>();
    a->set_bounds({10, 10, 60, 60});
    auto* a_ptr = a.get();
    root.add_child(std::move(a));
    auto b = std::make_unique<View>();
    b->set_bounds({100, 10, 60, 60});
    auto* b_ptr = b.get();
    root.add_child(std::move(b));

    InspectorOverlay overlay(root);
    overlay.set_active(true);

    // Select a + Alt-hover over b → distance line draws.
    MouseEvent click_a;
    click_a.position = {20, 20};
    click_a.is_down = true;
    overlay.handle_mouse_event(click_a);
    REQUIRE(overlay.selected_view() == a_ptr);

    MouseEvent alt_hover_b;
    alt_hover_b.position = {130, 30};
    alt_hover_b.modifiers = kModAlt;
    alt_hover_b.is_down = false;
    overlay.handle_mouse_event(alt_hover_b);
    REQUIRE(overlay.hovered_view() == b_ptr);

    pulp::canvas::RecordingCanvas with_line;
    overlay.paint(with_line);
    auto with_line_count = with_line.command_count();

    // Move cursor INTO the panel (root.bounds().width - panel_width_;
    // default panel_width_ = 300, so panel starts at x=300 for a 600-
    // wide root). Pre-fix: alt_hover_target_ stays set, distance line
    // still draws. Post-fix: alt_hover_target_ clears, distance line
    // disappears, command count drops back to baseline.
    MouseEvent enter_panel;
    enter_panel.position = {450, 50};
    enter_panel.modifiers = kModAlt;
    enter_panel.is_down = false;
    overlay.handle_mouse_event(enter_panel);

    pulp::canvas::RecordingCanvas after_panel_entry;
    overlay.paint(after_panel_entry);
    REQUIRE(after_panel_entry.command_count() < with_line_count);
}

// ── Phase 0b PR-C-1: gesture-tweak emission via TweakStore ────────────────
#include <pulp/inspect/tweak_store.hpp>
#include <choc/containers/choc_Value.h>

TEST_CASE("InspectorOverlay: emit_tweak_for_selection writes to TweakStore",
          "[inspect][overlay][gesture]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    auto child = std::make_unique<View>();
    child->set_anchor_id("figma:0:42");
    child->set_bounds({10, 10, 80, 40});
    auto* child_ptr = child.get();
    root.add_child(std::move(child));

    TweakStore store;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);

    // Simulate selection via the overlay's normal click path.
    MouseEvent click;
    click.position = {20, 20};
    click.is_down = true;
    overlay.handle_mouse_event(click);
    REQUIRE(overlay.selected_view() == child_ptr);

    // Emit a tweak — overlay maps selected_->anchor_id() to the store.
    bool ok = overlay.emit_tweak_for_selection(
        "layout.padding", choc::value::createInt32(12), "drag");
    REQUIRE(ok);
    REQUIRE(store.count() == 1);
    auto v = store.lookup("figma:0:42", "layout.padding");
    REQUIRE(v.has_value());
    REQUIRE(v->getInt32() == 12);
}

TEST_CASE("InspectorOverlay: emit_tweak_for_selection silently no-ops "
          "without a selection",
          "[inspect][overlay][gesture]") {
    View root;
    TweakStore store;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);

    // No view selected.
    REQUIRE(overlay.selected_view() == nullptr);
    bool ok = overlay.emit_tweak_for_selection(
        "paint.bg", choc::value::createString("#abc"), "color-picker");
    REQUIRE_FALSE(ok);
    REQUIRE(store.count() == 0);
}

TEST_CASE("InspectorOverlay: emit_tweak_for_selection silently no-ops "
          "when selected view has no anchor",
          "[inspect][overlay][gesture]") {
    // Hand-authored views (not imported from a design) have no anchor.
    // Inspector gesture-tweak emission should silently no-op rather
    // than synthesize an anchor — those tweaks have nowhere to land
    // safely on re-import.
    View root;
    root.set_bounds({0, 0, 400, 300});
    auto child = std::make_unique<View>();
    // intentionally no set_anchor_id() call
    child->set_bounds({10, 10, 80, 40});
    auto* child_ptr = child.get();
    root.add_child(std::move(child));

    TweakStore store;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);

    MouseEvent click;
    click.position = {20, 20};
    click.is_down = true;
    overlay.handle_mouse_event(click);
    REQUIRE(overlay.selected_view() == child_ptr);
    REQUIRE(child_ptr->anchor_id().empty());

    bool ok = overlay.emit_tweak_for_selection(
        "layout.padding", choc::value::createInt32(12), "drag");
    REQUIRE_FALSE(ok);
    REQUIRE(store.count() == 0);
}

TEST_CASE("InspectorOverlay: emit_tweak_for_selection silently no-ops "
          "without a TweakStore",
          "[inspect][overlay][gesture]") {
    // Inspector can run with no TweakStore wired (e.g. in tests of just
    // the overlay, or contexts where persistence is disabled). The
    // emission path must tolerate that.
    View root;
    root.set_bounds({0, 0, 400, 300});
    auto child = std::make_unique<View>();
    child->set_anchor_id("figma:0:99");
    child->set_bounds({10, 10, 80, 40});
    auto* child_ptr = child.get();
    root.add_child(std::move(child));

    InspectorOverlay overlay(root);
    overlay.set_active(true);
    // intentionally NOT calling set_tweak_store

    MouseEvent click;
    click.position = {20, 20};
    click.is_down = true;
    overlay.handle_mouse_event(click);
    REQUIRE(overlay.selected_view() == child_ptr);

    bool ok = overlay.emit_tweak_for_selection(
        "layout.padding", choc::value::createInt32(12), "drag");
    REQUIRE_FALSE(ok);
    REQUIRE(overlay.tweak_store() == nullptr);
}

TEST_CASE("InspectorOverlay: tweak_store() round-trips set_tweak_store",
          "[inspect][overlay][gesture]") {
    View root;
    TweakStore store;
    InspectorOverlay overlay(root);
    REQUIRE(overlay.tweak_store() == nullptr);
    overlay.set_tweak_store(&store);
    REQUIRE(overlay.tweak_store() == &store);
    overlay.set_tweak_store(nullptr);
    REQUIRE(overlay.tweak_store() == nullptr);
}

// ── Phase 2.5 — tweak management panel (Photoshop-layers style) ───────────
//
// A panel listing every tweak in the attached TweakStore, with three
// per-tweak icon controls: bypass / lock / delete. Toggled with `T`.
// The panel lays out row hit-rects during paint(); tests paint first
// (RecordingCanvas, headless) to populate them, then click an icon
// rect to exercise the action.

namespace {

// Build a scene with two anchored views and seed the store with
// tweaks so the management panel has rows to lay out. Root is large
// enough (600×600) that the bottom-third tweaks section has room.
struct TweakPanelScene {
    View root;
    TweakStore store;
    InspectorOverlay overlay{root};

    TweakPanelScene() {
        root.set_bounds({0, 0, 600, 600});
        overlay.set_active(true);
        overlay.set_tweak_store(&store);
        // Two anchors; one with two property tweaks (grouping case).
        store.apply_tweak("figma:0:a", "layout.padding",
                          choc::value::createInt32(12), "drag");
        store.apply_tweak("figma:0:a", "paint.backgroundColor",
                          choc::value::createString("#abcdef"), "picker");
        store.apply_tweak("figma:0:b", "layout.gap",
                          choc::value::createInt32(4), "drag");
    }
};

}  // namespace

TEST_CASE("InspectorOverlay Phase 2.5: T toggles the tweak management panel",
          "[inspect][overlay][phase2.5]") {
    View root;
    root.set_bounds({0, 0, 600, 600});
    InspectorOverlay overlay(root);
    overlay.set_active(true);

    REQUIRE_FALSE(overlay.tweaks_panel_visible());

    KeyEvent t;
    t.key = KeyCode::t;
    t.is_down = true;
    REQUIRE(overlay.handle_key_event(t));
    REQUIRE(overlay.tweaks_panel_visible());

    REQUIRE(overlay.handle_key_event(t));
    REQUIRE_FALSE(overlay.tweaks_panel_visible());
}

TEST_CASE("InspectorOverlay Phase 2.5: panel lays out a row per tweak",
          "[inspect][overlay][phase2.5]") {
    TweakPanelScene scene;
    scene.overlay.toggle_tweaks_panel();
    REQUIRE(scene.overlay.tweaks_panel_visible());

    pulp::canvas::RecordingCanvas canvas;
    scene.overlay.paint(canvas);
    REQUIRE(canvas.command_count() > 0);

    // Three tweaks seeded → three rows laid out.
    REQUIRE(scene.overlay.tweak_row_count() == 3);

    // With the panel hidden, no rows are laid out.
    scene.overlay.toggle_tweaks_panel();
    pulp::canvas::RecordingCanvas hidden_canvas;
    scene.overlay.paint(hidden_canvas);
    REQUIRE(scene.overlay.tweak_row_count() == 0);
}

TEST_CASE("InspectorOverlay Phase 2.5: panel renders cleanly with no tweaks",
          "[inspect][overlay][phase2.5]") {
    View root;
    root.set_bounds({0, 0, 600, 600});
    TweakStore store;  // empty
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.toggle_tweaks_panel();

    pulp::canvas::RecordingCanvas canvas;
    overlay.paint(canvas);  // must not crash / must produce commands
    REQUIRE(canvas.command_count() > 0);
    REQUIRE(overlay.tweak_row_count() == 0);
}

// Helper: paint the panel, then return the center of a chosen icon
// rect for a chosen row. Walking icon centers via clicks would
// require knowing the layout; instead we sweep candidate y positions
// in the bottom-third panel region. Simpler: drive clicks by sweeping
// the known icon column. We sweep the panel and click each row icon.
namespace {

// Click every row's icon of a given kind until `predicate` flips,
// returning whether it ever flipped. The icon rects live at fixed
// offsets that we don't know exactly from the test, so we sweep the
// panel's bottom-third region row by row.
bool sweep_click_icon(InspectorOverlay& overlay, View& root,
                      int icon_index /*0=bypass,1=lock,2=delete*/,
                      const std::function<bool()>& predicate) {
    // Re-paint so tweak_rows_ is fresh, then sweep candidate points.
    pulp::canvas::RecordingCanvas canvas;
    overlay.paint(canvas);

    float root_w = root.bounds().width;
    float root_h = root.bounds().height;
    float panel_x = root_w - 300.0f;          // panel_width_ default 300
    float x = panel_x + 8.0f;
    float w = 300.0f - 16.0f;
    constexpr float kIconSize = 14.0f, kIconGap = 4.0f;
    float icons_w = 3.0f * kIconSize + 2.0f * kIconGap;
    float icons_x = x + w - icons_w;
    float icon_cx = icons_x + icon_index * (kIconSize + kIconGap)
                    + kIconSize / 2.0f;

    // Sweep y across the bottom-third panel region at 2px steps.
    for (float y = root_h * 0.55f; y < root_h - 24.0f; y += 2.0f) {
        MouseEvent click;
        click.position = {icon_cx, y};
        click.is_down = true;
        overlay.handle_mouse_event(click);
        if (predicate()) return true;
        // Re-paint between attempts: a delete mutates the row list.
        pulp::canvas::RecordingCanvas c2;
        overlay.paint(c2);
    }
    return false;
}

}  // namespace

TEST_CASE("InspectorOverlay Phase 2.5: clicking the bypass icon toggles bypass",
          "[inspect][overlay][phase2.5]") {
    TweakPanelScene scene;
    scene.overlay.toggle_tweaks_panel();

    REQUIRE_FALSE(scene.store.is_bypassed("figma:0:a", "layout.padding"));
    bool flipped = sweep_click_icon(scene.overlay, scene.root, /*bypass=*/0,
        [&] { return scene.store.is_bypassed("figma:0:a", "layout.padding") ||
                     scene.store.is_bypassed("figma:0:b", "layout.gap"); });
    REQUIRE(flipped);
}

TEST_CASE("InspectorOverlay Phase 2.5: clicking the lock icon toggles lock",
          "[inspect][overlay][phase2.5]") {
    TweakPanelScene scene;
    scene.overlay.toggle_tweaks_panel();

    REQUIRE(scene.store.locked_anchors().empty());
    bool flipped = sweep_click_icon(scene.overlay, scene.root, /*lock=*/1,
        [&] { return !scene.store.locked_anchors().empty(); });
    REQUIRE(flipped);
}

TEST_CASE("InspectorOverlay Phase 2.5: clicking the delete icon removes a tweak",
          "[inspect][overlay][phase2.5]") {
    TweakPanelScene scene;
    scene.overlay.toggle_tweaks_panel();

    REQUIRE(scene.store.count() == 3);
    bool removed = sweep_click_icon(scene.overlay, scene.root, /*delete=*/2,
        [&] { return scene.store.count() < 3; });
    REQUIRE(removed);
}

TEST_CASE("InspectorOverlay Phase 2.5: panel icon clicks are ignored when "
          "the panel is hidden",
          "[inspect][overlay][phase2.5]") {
    TweakPanelScene scene;
    // Panel stays hidden — paint to clear any stale rows.
    pulp::canvas::RecordingCanvas canvas;
    scene.overlay.paint(canvas);
    REQUIRE(scene.overlay.tweak_row_count() == 0);

    // A click anywhere in the panel column should not mutate the store.
    MouseEvent click;
    click.position = {450, 500};
    click.is_down = true;
    scene.overlay.handle_mouse_event(click);
    REQUIRE(scene.store.count() == 3);
    REQUIRE(scene.store.locked_anchors().empty());
}

// ── Phase 3b — live-editable box-model fields ─────────────────────────────
//
// Click a numeric value in the property panel → enter edit mode →
// type / arrow-nudge / Enter to commit (tweak emitted) / Esc to
// cancel. The tweak path reuses Phase 0b PR-C-1's
// emit_tweak_for_selection() so persistence (Phase 1 disk write)
// gets keyboard edits for free.
//
// Test surface uses begin_field_edit() / handle_key_event() directly
// to avoid having to know the panel's exact pixel layout (it's
// recomputed each paint pass based on root bounds + panel_width_).
// The click-into-edit path is exercised by an integration test at
// the bottom that paints first to populate editable_fields_, then
// clicks at a known panel y-offset.

namespace {

// Build a small root + child with an anchor for tweak-emission tests.
struct Phase3bScene {
    View root;
    View* child = nullptr;
    TweakStore store;
    InspectorOverlay overlay{root};

    Phase3bScene() {
        root.set_bounds({0, 0, 500, 400});
        auto c = std::make_unique<View>();
        c->set_anchor_id("figma:3b:1");
        c->set_bounds({10, 10, 80, 40});
        c->flex().padding = 8;
        c->flex().margin = 4;
        c->flex().preferred_width = 80;
        c->flex().preferred_height = 40;
        child = c.get();
        root.add_child(std::move(c));

        overlay.set_active(true);
        overlay.set_tweak_store(&store);

        // Select the child via the normal click path.
        MouseEvent click;
        click.position = {20, 20};
        click.is_down = true;
        overlay.handle_mouse_event(click);
    }
};

KeyEvent make_key(KeyCode k, bool is_down = true, uint16_t mods = 0) {
    KeyEvent e;
    e.key = k;
    e.is_down = is_down;
    e.modifiers = mods;
    return e;
}

} // namespace

TEST_CASE("InspectorOverlay Phase 3b: begin_field_edit sets editing state",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    REQUIRE(s.overlay.selected_view() == s.child);
    REQUIRE_FALSE(s.overlay.is_editing());

    bool ok = s.overlay.begin_field_edit("layout.padding", 8.0f);
    REQUIRE(ok);
    REQUIRE(s.overlay.is_editing());
    REQUIRE(s.overlay.editing_field() == "layout.padding");
    REQUIRE(s.overlay.edit_buffer() == "8");
    REQUIRE(s.overlay.edit_caret_pos() == 1);  // caret at end of "8"
}

TEST_CASE("InspectorOverlay Phase 3b: begin_field_edit refuses without selection",
          "[inspect][overlay][phase3b]") {
    View root;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    // No selection set.
    bool ok = overlay.begin_field_edit("layout.padding", 8.0f);
    REQUIRE_FALSE(ok);
    REQUIRE_FALSE(overlay.is_editing());
}

TEST_CASE("InspectorOverlay Phase 3b: typing digits extends edit buffer",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    REQUIRE(s.overlay.begin_field_edit("layout.padding", 8.0f));
    REQUIRE(s.overlay.edit_buffer() == "8");

    // Type "5" → buffer becomes "85"
    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::num5)));
    REQUIRE(s.overlay.edit_buffer() == "85");
    REQUIRE(s.overlay.edit_caret_pos() == 2);

    // Type "0" → "850"
    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::num0)));
    REQUIRE(s.overlay.edit_buffer() == "850");

    // Real-time preview: View padding should already reflect 850.
    REQUIRE(s.child->flex().padding == 850.0f);
}

TEST_CASE("InspectorOverlay Phase 3b: backspace trims buffer",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);
    s.overlay.handle_key_event(make_key(KeyCode::num5));
    REQUIRE(s.overlay.edit_buffer() == "85");

    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::backspace)));
    REQUIRE(s.overlay.edit_buffer() == "8");
    REQUIRE(s.overlay.edit_caret_pos() == 1);
}

TEST_CASE("InspectorOverlay Phase 3b: Enter commits tweak to TweakStore",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);
    // Type "5" → buffer is "85"
    s.overlay.handle_key_event(make_key(KeyCode::num5));

    REQUIRE(s.store.count() == 0);
    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::enter)));

    // Edit mode exited, tweak persisted, View reflects committed value.
    REQUIRE_FALSE(s.overlay.is_editing());
    REQUIRE(s.store.count() == 1);
    auto v = s.store.lookup("figma:3b:1", "layout.padding");
    REQUIRE(v.has_value());
    // Value stored as float32 — round-trip equality at integer values
    // is safe.
    REQUIRE(v->getFloat32() == 85.0f);
    REQUIRE(s.child->flex().padding == 85.0f);
}

TEST_CASE("InspectorOverlay Phase 3b: Esc cancels and reverts",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);
    // Bump to "85" — real-time preview mutates the View.
    s.overlay.handle_key_event(make_key(KeyCode::num5));
    REQUIRE(s.child->flex().padding == 85.0f);

    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::escape)));
    REQUIRE_FALSE(s.overlay.is_editing());
    REQUIRE(s.store.count() == 0);                 // no tweak emitted
    REQUIRE(s.child->flex().padding == 8.0f);      // reverted
}

TEST_CASE("InspectorOverlay Phase 3b: Up arrow increments by 1",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);

    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::up)));
    REQUIRE(s.overlay.edit_buffer() == "9");
    REQUIRE(s.child->flex().padding == 9.0f);

    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::up)));
    REQUIRE(s.overlay.edit_buffer() == "10");
    REQUIRE(s.child->flex().padding == 10.0f);
}

TEST_CASE("InspectorOverlay Phase 3b: Down arrow decrements by 1",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);

    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::down)));
    REQUIRE(s.overlay.edit_buffer() == "7");
    REQUIRE(s.child->flex().padding == 7.0f);
}

TEST_CASE("InspectorOverlay Phase 3b: Shift+Up nudges by 10",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);

    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::up, true, kModShift)));
    REQUIRE(s.overlay.edit_buffer() == "18");
    REQUIRE(s.child->flex().padding == 18.0f);

    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::down, true, kModShift)));
    REQUIRE(s.overlay.edit_buffer() == "8");
}

TEST_CASE("InspectorOverlay Phase 3b: Cmd+Up nudges by 100 (Figma semantics)",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);

#ifdef __APPLE__
    auto main_mod = kModCmd;
#else
    auto main_mod = kModCtrl;
#endif
    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::up, true, main_mod)));
    REQUIRE(s.overlay.edit_buffer() == "108");
    REQUIRE(s.child->flex().padding == 108.0f);
}

TEST_CASE("InspectorOverlay Phase 3b: Tab commits and moves to next field",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    // Paint once so editable_fields_ is populated with the panel's
    // tab order.
    pulp::canvas::RecordingCanvas canvas;
    s.overlay.paint(canvas);

    s.overlay.begin_field_edit("layout.padding", 8.0f);
    s.overlay.handle_key_event(make_key(KeyCode::num5));  // "85"

    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::tab)));
    // The previous field's edit was committed (tweak in store).
    REQUIRE(s.store.count() == 1);
    REQUIRE(s.store.lookup("figma:3b:1", "layout.padding").has_value());
    // We're now editing a different field (whatever follows padding
    // in the panel's draw order).
    REQUIRE(s.overlay.is_editing());
    REQUIRE(s.overlay.editing_field() != "layout.padding");
}

TEST_CASE("InspectorOverlay Phase 3b: clicking a numeric value enters edit mode",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    // Paint to populate editable_fields_ (the panel-side hit rects).
    pulp::canvas::RecordingCanvas canvas;
    s.overlay.paint(canvas);

    // The panel sits at the right edge of root (500). panel_width_
    // defaults to 300, so panel starts at x = 200. paint_props_section
    // is called with x = panel_x + 8 = 208, and editable values live
    // at x + 80 = 288 → click at x = 290 lands on the value column.
    // The Y coordinate just needs to be inside the props area; the
    // tree section is the top half (root_h * 0.5 = 200), so click at
    // y = 210 lands on the first heading. Use the first editable
    // field's reported rect to pick a guaranteed-correct y.
    //
    // Use overlay's public state via paint + then a click that lands
    // within a row. We don't have direct access to editable_fields_
    // from tests, so click at a coordinate that the first row covers
    // given the default panel layout. Tree height is root_h/2 = 200,
    // so props_y = 200; first prop row starts ~ y = 204 + heading
    // height. We pick a y of 270 which falls into the layout section
    // ("padding" row given the order width/height/padding/margin).
    //
    // To make this robust regardless of exact row arithmetic, scan a
    // band of y values clicking at the value column until edit mode
    // is entered. Bounded by the visible props area.
    bool entered = false;
    for (float y = 210.0f; y < 380.0f && !entered; y += 5.0f) {
        MouseEvent click;
        click.position = {290.0f, y};
        click.is_down = true;
        s.overlay.handle_mouse_event(click);
        entered = s.overlay.is_editing();
        if (entered) break;
        // Repaint after each scan to refresh hit rects.
        canvas.clear();
        s.overlay.paint(canvas);
    }
    REQUIRE(entered);
}

TEST_CASE("InspectorOverlay Phase 3b: editing layout.width writes to preferred_width",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    s.overlay.begin_field_edit("layout.width", s.child->flex().preferred_width);
    REQUIRE(s.overlay.edit_buffer() == "80");

    // Type "5" → buffer becomes "805"
    s.overlay.handle_key_event(make_key(KeyCode::num5));
    REQUIRE(s.child->flex().preferred_width == 805.0f);

    s.overlay.handle_key_event(make_key(KeyCode::enter));
    auto v = s.store.lookup("figma:3b:1", "layout.width");
    REQUIRE(v.has_value());
    REQUIRE(v->getFloat32() == 805.0f);
}

TEST_CASE("InspectorOverlay Phase 3b: editing style.opacity round-trips",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    s.child->set_opacity(0.5f);
    s.overlay.begin_field_edit("style.opacity", 0.5f);

    // Buffer for 0.5 should be "0.50"
    REQUIRE(s.overlay.edit_buffer() == "0.50");

    // Nudge up: 0.50 + 1 = 1.50, then clamped to 1.0 by set_opacity.
    s.overlay.handle_key_event(make_key(KeyCode::up));
    // Underlying View is clamped.
    REQUIRE(s.child->opacity() == 1.0f);
    s.overlay.handle_key_event(make_key(KeyCode::enter));

    auto v = s.store.lookup("figma:3b:1", "style.opacity");
    REQUIRE(v.has_value());
}

TEST_CASE("InspectorOverlay Phase 3b: cancel_field_edit on inactive is no-op",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    REQUIRE_FALSE(s.overlay.is_editing());
    s.overlay.cancel_field_edit();  // must not crash
    REQUIRE_FALSE(s.overlay.is_editing());
    REQUIRE(s.store.count() == 0);
}

TEST_CASE("InspectorOverlay Phase 3b: commit_field_edit with empty buffer "
          "exits edit without emitting tweak",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);
    // Backspace twice to empty the buffer ("8" → "")
    s.overlay.handle_key_event(make_key(KeyCode::backspace));
    REQUIRE(s.overlay.edit_buffer().empty());

    REQUIRE_FALSE(s.overlay.commit_field_edit());
    REQUIRE_FALSE(s.overlay.is_editing());
    REQUIRE(s.store.count() == 0);
}

TEST_CASE("InspectorOverlay Phase 3b: caret moves with Left/Right/Home/End",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    s.overlay.begin_field_edit("layout.padding", 80.0f);
    REQUIRE(s.overlay.edit_buffer() == "80");
    REQUIRE(s.overlay.edit_caret_pos() == 2);

    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::left)));
    REQUIRE(s.overlay.edit_caret_pos() == 1);

    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::home)));
    REQUIRE(s.overlay.edit_caret_pos() == 0);

    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::end_)));
    REQUIRE(s.overlay.edit_caret_pos() == 2);

    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::right)));
    REQUIRE(s.overlay.edit_caret_pos() == 2);  // already at end
}

TEST_CASE("InspectorOverlay Phase 3b: deactivating inspector cancels active edit",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);
    s.overlay.handle_key_event(make_key(KeyCode::num5));
    REQUIRE(s.child->flex().padding == 85.0f);
    REQUIRE(s.overlay.is_editing());

    s.overlay.set_active(false);
    REQUIRE_FALSE(s.overlay.is_editing());
    REQUIRE(s.child->flex().padding == 8.0f);   // reverted via cancel
    REQUIRE(s.store.count() == 0);              // no tweak emitted
}

TEST_CASE("InspectorOverlay Phase 3b: Esc while editing does NOT exit inspector",
          "[inspect][overlay][phase3b]") {
    // Spec: Esc inside an edit cancels the edit; Esc with no edit
    // exits the inspector. The two semantics share a key, the order
    // matters.
    Phase3bScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);
    REQUIRE(s.overlay.is_active());

    s.overlay.handle_key_event(make_key(KeyCode::escape));
    REQUIRE_FALSE(s.overlay.is_editing());
    REQUIRE(s.overlay.is_active());  // still active

    // Second Esc with no edit → exits the inspector.
    s.overlay.handle_key_event(make_key(KeyCode::escape));
    REQUIRE_FALSE(s.overlay.is_active());
}

TEST_CASE("InspectorOverlay Phase 3b: Tab without paint does nothing extra",
          "[inspect][overlay][phase3b]") {
    // Tab consults the editable_fields_ list populated by the last
    // paint. If we Tab without ever painting, commit happens but no
    // next-field move occurs (gracefully).
    Phase3bScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);
    s.overlay.handle_key_event(make_key(KeyCode::num5));

    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::tab)));
    // First-field commit happened.
    REQUIRE(s.store.count() == 1);
    // Without a prior paint, editable_fields_ is empty, so Tab can't
    // find a next field — we're no longer editing.
    REQUIRE_FALSE(s.overlay.is_editing());
}

// ── Phase 0b PR-C-2: property panel dot indicators ─────────────────────────
//
// When the selected view has tweaks in the TweakStore, the property
// panel renders a small dot in the gutter LEFT of the label row. The
// dot signals "this property differs from source". The exact pixel
// position isn't asserted — instead we check that paint emits MORE
// canvas commands when tweaks are present (the dot is an extra
// fill_circle per tweaked row).

TEST_CASE("InspectorOverlay: property panel paints dot when tweak exists",
          "[inspect][overlay][dot-indicator][phase0b-prc2]") {
    View root;
    root.set_bounds({0, 0, 600, 400});
    auto child = std::make_unique<View>();
    child->set_anchor_id("figma:0:42");
    child->set_bounds({10, 10, 80, 40});
    child->flex().gap = 8;  // ensure the gap row renders
    auto* child_ptr = child.get();
    root.add_child(std::move(child));

    TweakStore store;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);

    // Select the child via click so the props panel renders.
    MouseEvent click;
    click.position = {30, 30};
    click.is_down = true;
    overlay.handle_mouse_event(click);
    REQUIRE(overlay.selected_view() == child_ptr);

    // Baseline render with no tweaks.
    pulp::canvas::RecordingCanvas baseline;
    overlay.paint(baseline);
    auto baseline_count = baseline.command_count();
    REQUIRE(baseline_count > 0);

    // Add a tweak for a property that's rendered in the panel.
    store.apply_tweak("figma:0:42", "layout.gap",
                      choc::value::createInt32(16), "drag");

    pulp::canvas::RecordingCanvas with_dot;
    overlay.paint(with_dot);
    // Dot indicator = one extra fill_circle command per tweaked row.
    REQUIRE(with_dot.command_count() > baseline_count);
}

TEST_CASE("InspectorOverlay: dot indicator absent without a TweakStore",
          "[inspect][overlay][dot-indicator][phase0b-prc2]") {
    // Inspector running without a TweakStore wired (e.g. unit-test
    // contexts) must not crash and must not paint dots — they have no
    // data to drive them.
    View root;
    root.set_bounds({0, 0, 600, 400});
    auto child = std::make_unique<View>();
    child->set_anchor_id("figma:0:42");
    child->set_bounds({10, 10, 80, 40});
    auto* child_ptr = child.get();
    root.add_child(std::move(child));

    InspectorOverlay overlay(root);
    overlay.set_active(true);
    // intentionally NOT calling set_tweak_store

    MouseEvent click;
    click.position = {30, 30};
    click.is_down = true;
    overlay.handle_mouse_event(click);
    REQUIRE(overlay.selected_view() == child_ptr);

    pulp::canvas::RecordingCanvas canvas;
    overlay.paint(canvas);
    REQUIRE(canvas.command_count() > 0);
    // No crash, no dots — render completes cleanly.
}

TEST_CASE("InspectorOverlay: bypassed tweak suppresses dot",
          "[inspect][overlay][dot-indicator][phase0b-prc2]") {
    // Phase 2.5 / Codex spec: a path-scoped or whole-anchor bypass
    // marks a tweak as "inactive" but keeps it in the file. The dot
    // indicator must reflect bypass — no dot on bypassed rows.
    View root;
    root.set_bounds({0, 0, 600, 400});
    auto child = std::make_unique<View>();
    child->set_anchor_id("figma:0:42");
    child->set_bounds({10, 10, 80, 40});
    child->flex().gap = 8;
    auto* child_ptr = child.get();
    root.add_child(std::move(child));

    TweakStore store;
    store.apply_tweak("figma:0:42", "layout.gap",
                      choc::value::createInt32(16), "drag");

    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);

    MouseEvent click;
    click.position = {30, 30};
    click.is_down = true;
    overlay.handle_mouse_event(click);
    REQUIRE(overlay.selected_view() == child_ptr);

    pulp::canvas::RecordingCanvas with_dot;
    overlay.paint(with_dot);
    auto with_dot_count = with_dot.command_count();

    // Bypass the tweak — dot should disappear, command count drops.
    store.set_bypass("figma:0:42", std::vector<std::string>{"layout.gap"});

    pulp::canvas::RecordingCanvas bypassed;
    overlay.paint(bypassed);
    REQUIRE(bypassed.command_count() < with_dot_count);
}

// ── Phase 3c — color eyedropper ───────────────────────────────────────────
//
// Eyedropper mode (E key) samples a color from the rendered UI and
// applies it as a tweak to the selected view's color property. With
// no Canvas pixel-readback in headless tests, sampling falls back to
// the resolved background color of the view under the cursor; the
// tweak path reuses emit_tweak_for_selection() with source
// "inspector-eyedropper".

namespace {

// Build a root + child where the child carries an explicit background
// color (so the resolved-style fallback has something to sample) and
// an anchor (so the eyedropper tweak can land in the store).
struct Phase3cScene {
    View root;
    View* child = nullptr;
    TweakStore store;
    InspectorOverlay overlay{root};

    explicit Phase3cScene(Color child_bg = Color::rgba8(0x33, 0x66, 0xcc)) {
        root.set_bounds({0, 0, 500, 400});
        auto c = std::make_unique<View>();
        c->set_anchor_id("figma:3c:1");
        c->set_bounds({40, 40, 120, 80});
        c->set_background_color(child_bg);
        child = c.get();
        root.add_child(std::move(c));

        overlay.set_active(true);
        overlay.set_tweak_store(&store);

        // Select the child via the normal click path.
        MouseEvent click;
        click.position = {60, 60};
        click.is_down = true;
        overlay.handle_mouse_event(click);
    }
};

MouseEvent make_move(float x, float y) {
    MouseEvent e;
    e.position = {x, y};
    e.is_down = false;
    return e;
}

MouseEvent make_click(float x, float y) {
    MouseEvent e;
    e.position = {x, y};
    e.is_down = true;
    return e;
}

} // namespace

TEST_CASE("InspectorOverlay Phase 3c: eyedropper defaults OFF",
          "[inspect][overlay][phase3c]") {
    View root;
    InspectorOverlay overlay(root);
    REQUIRE_FALSE(overlay.eyedropper_active());
    REQUIRE_FALSE(overlay.eyedropper_has_sample());
    REQUIRE(overlay.eyedropper_target() == "style.background_color");

    overlay.set_eyedropper_active(true);
    REQUIRE(overlay.eyedropper_active());
    overlay.toggle_eyedropper();
    REQUIRE_FALSE(overlay.eyedropper_active());
}

TEST_CASE("InspectorOverlay Phase 3c: 'E' key toggles eyedropper only when active",
          "[inspect][overlay][phase3c]") {
    View root;
    InspectorOverlay overlay(root);

    // Inactive — E does nothing.
    REQUIRE_FALSE(overlay.handle_key_event(make_key(KeyCode::e)));
    REQUIRE_FALSE(overlay.eyedropper_active());

    overlay.set_active(true);
    // Active — E toggles.
    REQUIRE(overlay.handle_key_event(make_key(KeyCode::e)));
    REQUIRE(overlay.eyedropper_active());
    REQUIRE(overlay.handle_key_event(make_key(KeyCode::e)));
    REQUIRE_FALSE(overlay.eyedropper_active());

    // E with a modifier is ignored (reserved for chord shortcuts).
    overlay.handle_key_event(make_key(KeyCode::e, true, kModCmd));
    REQUIRE_FALSE(overlay.eyedropper_active());
}

TEST_CASE("InspectorOverlay Phase 3c: mouse-move samples color under cursor",
          "[inspect][overlay][phase3c]") {
    Phase3cScene s(Color::rgba8(0x12, 0xab, 0x5f));
    s.overlay.set_eyedropper_active(true);
    REQUIRE_FALSE(s.overlay.eyedropper_has_sample());

    // Move over the child — resolved-style fallback samples its bg.
    s.overlay.handle_mouse_event(make_move(80, 70));
    REQUIRE(s.overlay.eyedropper_has_sample());
    auto c = s.overlay.eyedropper_sample();
    REQUIRE(c.r8() == 0x12);
    REQUIRE(c.g8() == 0xab);
    REQUIRE(c.b8() == 0x5f);
}

TEST_CASE("InspectorOverlay Phase 3c: click captures color and emits tweak",
          "[inspect][overlay][phase3c]") {
    Phase3cScene s(Color::rgba8(0xff, 0x80, 0x00));
    s.overlay.set_eyedropper_active(true);

    // Hover to sample, then click to apply.
    s.overlay.handle_mouse_event(make_move(90, 80));
    bool consumed = s.overlay.handle_mouse_event(make_click(90, 80));
    REQUIRE(consumed);  // eyedropper consumes the pick click

    // A pick is a single deliberate action — mode auto-disables.
    REQUIRE_FALSE(s.overlay.eyedropper_active());

    // The tweak landed in the store under the selection's anchor.
    REQUIRE(s.store.count() == 1);
    auto v = s.store.lookup("figma:3c:1", "style.background_color");
    REQUIRE(v.has_value());
    REQUIRE(std::string(v->getString()) == "#ff8000");
}

TEST_CASE("InspectorOverlay Phase 3c: click without prior move still picks",
          "[inspect][overlay][phase3c]") {
    // Resolved-style sampling is synchronous, so a click with no
    // preceding mouse-move still captures a real color (covers
    // scripted / fast-click use).
    Phase3cScene s(Color::rgba8(0x40, 0x40, 0x40));
    s.overlay.set_eyedropper_active(true);

    bool consumed = s.overlay.handle_mouse_event(make_click(100, 90));
    REQUIRE(consumed);
    REQUIRE(s.store.count() == 1);
    auto v = s.store.lookup("figma:3c:1", "style.background_color");
    REQUIRE(v.has_value());
    REQUIRE(std::string(v->getString()) == "#404040");
}

TEST_CASE("InspectorOverlay Phase 3c: click refreshes stale hover sample",
          "[inspect][overlay][phase3c][regression]") {
    Phase3cScene s(Color::rgba8(0xff, 0x00, 0x00));

    auto second = std::make_unique<View>();
    second->set_anchor_id("figma:3c:2");
    second->set_bounds({40, 150, 120, 80});
    second->set_background_color(Color::rgba8(0x00, 0x66, 0xff));
    s.root.add_child(std::move(second));

    s.overlay.set_eyedropper_active(true);

    // Hover over the selected child first, then click elsewhere. The
    // committed tweak must use the click location, not the stale hover
    // swatch sample.
    s.overlay.handle_mouse_event(make_move(90, 80));
    auto hover_sample = s.overlay.eyedropper_sample();
    REQUIRE(hover_sample.r8() == 0xff);
    REQUIRE(hover_sample.g8() == 0x00);
    REQUIRE(hover_sample.b8() == 0x00);

    bool consumed = s.overlay.handle_mouse_event(make_click(90, 170));
    REQUIRE(consumed);

    auto v = s.store.lookup("figma:3c:1", "style.background_color");
    REQUIRE(v.has_value());
    REQUIRE(std::string(v->getString()) == "#0066ff");
}

TEST_CASE("InspectorOverlay Phase 3c: click never commits a stale sample",
          "[inspect][overlay][phase3c][regression]") {
    // Codex P1 (#2434): pressing E and clicking on empty canvas must
    // not write a stale color into the tweak store. A prior sample can
    // be left behind by an earlier hover (or by paint_eyedropper_cursor
    // sampling from the default cursor position before any mouse-move).
    // When the click lands where the resolved-style fallback finds no
    // background-colored view, the resample fails — and the pick must
    // no-op rather than commit the stale color from the old position.
    Phase3cScene s(Color::rgba8(0xff, 0x00, 0x00));
    s.overlay.set_eyedropper_active(true);

    // Seed a stale sample by hovering over the colored child.
    s.overlay.handle_mouse_event(make_move(90, 80));
    REQUIRE(s.overlay.eyedropper_has_sample());
    REQUIRE(s.overlay.eyedropper_sample().r8() == 0xff);

    // Click on empty canvas — (60, 300) is inside the root and clear of
    // the inspector panel (x >= 200), but hits no background-colored
    // view, so the resolved-style fallback returns false. The stale red
    // sample must not survive into the store.
    bool consumed = s.overlay.handle_mouse_event(make_click(60, 300));
    REQUIRE(consumed);  // the click is still consumed by the eyedropper

    // No tweak — the failed resample invalidated the sample, so
    // apply_eyedropper_pick() correctly no-ops instead of writing red.
    REQUIRE(s.store.count() == 0);
    REQUIRE_FALSE(s.overlay.eyedropper_has_sample());
}

TEST_CASE("InspectorOverlay Phase 3c: retargeting writes a different property",
          "[inspect][overlay][phase3c]") {
    Phase3cScene s(Color::rgba8(0x00, 0x99, 0xff));
    s.overlay.set_eyedropper_target("style.border_color");
    s.overlay.set_eyedropper_active(true);

    s.overlay.handle_mouse_event(make_click(80, 70));
    auto v = s.store.lookup("figma:3c:1", "style.border_color");
    REQUIRE(v.has_value());
    REQUIRE(std::string(v->getString()) == "#0099ff");

    // Empty retarget is ignored — keeps the previous path.
    s.overlay.set_eyedropper_target("");
    REQUIRE(s.overlay.eyedropper_target() == "style.border_color");
}

TEST_CASE("InspectorOverlay Phase 3c: sample_color_at hex round-trips alpha",
          "[inspect][overlay][phase3c]") {
    // A semi-transparent background encodes as 8-digit "#rrggbbaa".
    Phase3cScene s(Color::rgba8(0x20, 0x30, 0x40, 0x80));
    s.overlay.set_eyedropper_active(true);
    s.overlay.handle_mouse_event(make_click(80, 70));
    auto v = s.store.lookup("figma:3c:1", "style.background_color");
    REQUIRE(v.has_value());
    REQUIRE(std::string(v->getString()) == "#20304080");
}

TEST_CASE("InspectorOverlay Phase 3c: eyedropper yields to the panel",
          "[inspect][overlay][phase3c]") {
    // A click inside the inspector panel must NOT be eaten by the
    // eyedropper — the user still needs tree / field interaction.
    Phase3cScene s;
    s.overlay.set_eyedropper_active(true);

    // Panel occupies the right 300px of the 500px-wide root.
    MouseEvent panel_click = make_click(450, 50);
    bool consumed = s.overlay.handle_mouse_event(panel_click);
    REQUIRE(consumed);  // panel consumes its own clicks
    // No pick happened — eyedropper still armed, store untouched.
    REQUIRE(s.overlay.eyedropper_active());
    REQUIRE(s.store.count() == 0);
}

TEST_CASE("InspectorOverlay Phase 3c: pick no-ops without a selection anchor",
          "[inspect][overlay][phase3c]") {
    // Hand-authored view with no anchor — the eyedropper still
    // disables (the click was a deliberate spend) but emits nothing.
    View root;
    root.set_bounds({0, 0, 500, 400});
    auto c = std::make_unique<View>();
    c->set_bounds({40, 40, 120, 80});
    c->set_background_color(Color::rgba8(0x11, 0x22, 0x33));
    root.add_child(std::move(c));

    TweakStore store;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.handle_mouse_event(make_click(60, 60));  // select
    overlay.set_eyedropper_active(true);

    overlay.handle_mouse_event(make_click(80, 70));  // pick
    REQUIRE_FALSE(overlay.eyedropper_active());
    REQUIRE(store.count() == 0);
}

TEST_CASE("InspectorOverlay Phase 3c: deactivating inspector clears eyedropper",
          "[inspect][overlay][phase3c]") {
    Phase3cScene s;
    s.overlay.set_eyedropper_active(true);
    s.overlay.handle_mouse_event(make_move(80, 70));
    REQUIRE(s.overlay.eyedropper_has_sample());

    s.overlay.set_active(false);
    REQUIRE_FALSE(s.overlay.eyedropper_active());
    REQUIRE_FALSE(s.overlay.eyedropper_has_sample());
}

TEST_CASE("InspectorOverlay Phase 3c: paint draws swatch when armed",
          "[inspect][overlay][phase3c]") {
    // The cursor swatch must paint without crashing once a sample
    // exists; RecordingCanvas has no pixel readback so this exercises
    // the resolved-style path + the swatch chrome.
    Phase3cScene s;
    s.overlay.set_eyedropper_active(true);
    s.overlay.handle_mouse_event(make_move(80, 70));
    REQUIRE(s.overlay.eyedropper_has_sample());

    pulp::canvas::RecordingCanvas canvas;
    s.overlay.paint(canvas);  // must not crash; swatch chrome drawn
    REQUIRE(s.overlay.eyedropper_active());
}

// ── Phase 3a: drag handles ────────────────────────────────────────────────

TEST_CASE("InspectorOverlay: drag handles default OFF", "[inspect][overlay][phase3a]") {
    View root;
    InspectorOverlay overlay(root);
    REQUIRE_FALSE(overlay.dragging_enabled());
    overlay.set_dragging_enabled(true);
    REQUIRE(overlay.dragging_enabled());
    overlay.toggle_dragging();
    REQUIRE_FALSE(overlay.dragging_enabled());
}

TEST_CASE("InspectorOverlay: 'D' key toggles drag handles only when active",
          "[inspect][overlay][phase3a]") {
    View root;
    InspectorOverlay overlay(root);

    // Inactive — D does nothing.
    KeyEvent d;
    d.key = KeyCode::d;
    d.is_down = true;
    REQUIRE_FALSE(overlay.handle_key_event(d));
    REQUIRE_FALSE(overlay.dragging_enabled());

    // Active — D toggles.
    overlay.set_active(true);
    REQUIRE(overlay.handle_key_event(d));
    REQUIRE(overlay.dragging_enabled());
    REQUIRE(overlay.handle_key_event(d));
    REQUIRE_FALSE(overlay.dragging_enabled());
}

TEST_CASE("InspectorOverlay: drag from SE handle resizes view and emits tweaks",
          "[inspect][overlay][phase3a][drag]") {
    View root;
    root.set_bounds({0, 0, 600, 400});
    auto child = std::make_unique<View>();
    child->set_anchor_id("figma:0:42");
    child->set_bounds({10, 10, 80, 40});
    auto* child_ptr = child.get();
    root.add_child(std::move(child));

    TweakStore store;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_dragging_enabled(true);

    // Select via click in the canvas area.
    MouseEvent click;
    click.position = {30, 30};
    click.is_down = true;
    overlay.handle_mouse_event(click);
    REQUIRE(overlay.selected_view() == child_ptr);

    // Press on the SE handle (bottom-right corner = 90, 50).
    MouseEvent press;
    press.position = {90, 50};
    press.is_down = true;
    REQUIRE(overlay.handle_mouse_event(press));  // consumed → drag started

    // Drag +20 / +15 (move event = is_down=false).
    MouseEvent drag;
    drag.position = {110, 65};
    drag.is_down = false;
    REQUIRE(overlay.handle_mouse_event(drag));

    // View resized live (Yoga inputs + bounds both updated).
    REQUIRE(child_ptr->bounds().width == 100.0f);  // 80 + 20
    REQUIRE(child_ptr->bounds().height == 55.0f);  // 40 + 15
    REQUIRE(child_ptr->flex().preferred_width == 100.0f);
    REQUIRE(child_ptr->flex().preferred_height == 55.0f);

    // Tweaks emitted per move (apply_tweak overwrites).
    auto w = store.lookup("figma:0:42", "layout.width");
    auto h = store.lookup("figma:0:42", "layout.height");
    REQUIRE(w.has_value());
    REQUIRE(h.has_value());
    REQUIRE(w->getFloat32() == 100.0f);
    REQUIRE(h->getFloat32() == 55.0f);

    // A subsequent is_down=true ends the drag (acts as release).
    MouseEvent release;
    release.position = {200, 200};
    release.is_down = true;
    overlay.handle_mouse_event(release);
    // Drag state cleared — the press at (200,200) lands outside the
    // resized view (now 100x55 at 10,10 → corner at 110,65), so
    // selection moves to root or clears.
}

TEST_CASE("InspectorOverlay: NW handle drag resizes from top-left",
          "[inspect][overlay][phase3a][drag]") {
    View root;
    root.set_bounds({0, 0, 600, 400});
    auto child = std::make_unique<View>();
    child->set_anchor_id("a");
    child->set_bounds({50, 50, 100, 80});
    auto* child_ptr = child.get();
    root.add_child(std::move(child));

    TweakStore store;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_dragging_enabled(true);

    MouseEvent click;
    click.position = {70, 70};
    click.is_down = true;
    overlay.handle_mouse_event(click);

    // NW handle = top-left = (50, 50).
    MouseEvent press;
    press.position = {50, 50};
    press.is_down = true;
    REQUIRE(overlay.handle_mouse_event(press));

    // Drag NW by +10/+10 → shrinks the view (NW pulls top-left inward).
    MouseEvent drag;
    drag.position = {60, 60};
    drag.is_down = false;
    overlay.handle_mouse_event(drag);

    REQUIRE(child_ptr->bounds().width == 90.0f);   // 100 - 10
    REQUIRE(child_ptr->bounds().height == 70.0f);  // 80 - 10
}

TEST_CASE("InspectorOverlay: drag handle hit-test no-op without enabled mode",
          "[inspect][overlay][phase3a][drag]") {
    View root;
    root.set_bounds({0, 0, 600, 400});
    auto child = std::make_unique<View>();
    child->set_anchor_id("a");
    child->set_bounds({10, 10, 80, 40});
    auto* child_ptr = child.get();
    root.add_child(std::move(child));

    TweakStore store;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    // dragging_enabled_ NOT set — default OFF.

    MouseEvent click;
    click.position = {30, 30};
    click.is_down = true;
    overlay.handle_mouse_event(click);
    REQUIRE(overlay.selected_view() == child_ptr);

    // Press at SE corner — but drag is disabled, so this is just
    // a regular click on the canvas (not a drag start).
    MouseEvent press_corner;
    press_corner.position = {90, 50};
    press_corner.is_down = true;
    overlay.handle_mouse_event(press_corner);
    // No tweak emitted; view bounds unchanged.
    REQUIRE(store.count() == 0);
    REQUIRE(child_ptr->bounds().width == 80.0f);
}

TEST_CASE("InspectorOverlay: drag handles paint when enabled + selected",
          "[inspect][overlay][phase3a][drag]") {
    View root;
    root.set_bounds({0, 0, 600, 400});
    auto child = std::make_unique<View>();
    child->set_bounds({10, 10, 80, 40});
    root.add_child(std::move(child));

    InspectorOverlay overlay(root);
    overlay.set_active(true);

    // Select via click first.
    MouseEvent click;
    click.position = {30, 30};
    click.is_down = true;
    overlay.handle_mouse_event(click);

    // Baseline: drag disabled → no handles painted.
    pulp::canvas::RecordingCanvas baseline;
    overlay.paint(baseline);

    // Enable drag mode → 4 corner handles add canvas commands.
    overlay.set_dragging_enabled(true);
    pulp::canvas::RecordingCanvas with_handles;
    overlay.paint(with_handles);

    REQUIRE(with_handles.command_count() > baseline.command_count());
}

// ── InspectorWindow ────────────────────────────────────────────────────────

TEST_CASE("CollapsableSection toggles content from header clicks", "[inspect][window][issue-641]") {
    CollapsableSection section("Layout");
    REQUIRE(section.is_expanded());
    REQUIRE(section.content() != nullptr);
    REQUIRE(section.content()->visible());

    MouseEvent up;
    up.position = {4, 4};
    up.is_down = false;
    section.on_mouse_event(up);
    REQUIRE(section.is_expanded());

    MouseEvent body_click;
    body_click.position = {4, 30};
    body_click.is_down = true;
    section.on_mouse_event(body_click);
    REQUIRE(section.is_expanded());

    MouseEvent header_click;
    header_click.position = {4, 6};
    header_click.is_down = true;
    section.on_mouse_event(header_click);
    REQUIRE_FALSE(section.is_expanded());
    REQUIRE_FALSE(section.content()->visible());

    section.set_expanded(true);
    REQUIRE(section.is_expanded());
    REQUIRE(section.content()->visible());
}

TEST_CASE("InspectorWindow builds tabs and updates element properties", "[inspect][window][issue-641]") {
    View inspected_root;
    inspected_root.set_id("root");
    inspected_root.set_bounds({0, 0, 480, 320});

    Theme theme;
    theme.colors["accent.primary"] = Color::rgba8(10, 20, 30);
    theme.colors["bg.surface"] = Color::rgba8(40, 50, 60);
    inspected_root.set_theme(theme);

    auto knob = std::make_unique<Knob>();
    knob->set_id("gain");
    knob->set_bounds({12, 18, 48, 48});
    knob->flex().direction = FlexDirection::row;
    knob->flex().gap = 3;
    knob->flex().padding = 4;
    knob->flex().margin = 2;
    knob->flex().flex_grow = 1.0f;
    knob->flex().flex_shrink = 0.5f;
    knob->set_background_color(Color::rgba8(30, 40, 50, 200));
    knob->set_border(Color::rgba8(80, 90, 100), 2, 5);
    knob->set_opacity(0.75f);
    knob->set_value(0.625f);
    knob->set_label("Gain");
    auto* knob_ptr = knob.get();
    inspected_root.add_child(std::move(knob));

    InspectorWindow window(inspected_root);
    REQUIRE(window.child_count() == 1);

    auto* tabs = dynamic_cast<TabPanel*>(window.child_at(0));
    REQUIRE(tabs != nullptr);
    REQUIRE(tabs->tab_count() == 4);
    REQUIRE(tabs->active_tab() == 0);
    REQUIRE(tabs->child_at(0)->visible());
    REQUIRE_FALSE(tabs->child_at(1)->visible());

    auto* tree = first_view_of_type<TreeView>(*tabs->child_at(0));
    REQUIRE(tree != nullptr);
    auto* knob_node = tree->find_node_by_user_data(knob_ptr);
    REQUIRE(knob_node != nullptr);
    REQUIRE(knob_node->label.find("Knob #gain") != std::string::npos);

    bool callback_seen = false;
    window.on_view_selected = [&](View* view) {
        callback_seen = (view == knob_ptr);
    };
    window.select_view(knob_ptr);
    REQUIRE(callback_seen);
    REQUIRE(has_label_containing(window, "Type: Knob"));
    REQUIRE(has_label_containing(window, "ID: gain"));
    REQUIRE(has_label_containing(window, "Bounds: 12, 18  48 x 48"));
    REQUIRE(has_label_containing(window, "Direction: row"));
    REQUIRE(has_label_containing(window, "Gap: 3.0"));
    REQUIRE(has_label_containing(window, "Padding: 4.0"));
    REQUIRE(has_label_containing(window, "Margin: 2.0"));
    REQUIRE(has_label_containing(window, "Grow: 1.0 / Shrink: 0.5"));
    REQUIRE(has_label_containing(window, "Opacity: 0.75"));
    REQUIRE(has_label_containing(window, "Border: 2.0px"));
    REQUIRE(has_label_containing(window, "Corner radius: 5.0"));
    REQUIRE(has_label_containing(window, "Value: 0.625  Label: Gain"));

    Fader fader;
    fader.set_value(0.2f);
    fader.set_label("Mix");
    window.select_view(&fader);
    REQUIRE(has_label_containing(window, "Value: 0.200  Label: Mix"));

    Toggle toggle;
    toggle.set_on(true);
    toggle.set_label("Bypass");
    window.select_view(&toggle);
    REQUIRE(has_label_containing(window, "On: true  Label: Bypass"));

    Label label("Ready");
    window.select_view(&label);
    REQUIRE(has_label_containing(window, "Text: Ready"));

    Meter meter;
    meter.set_level(0.125f, 0.5f);
    window.select_view(&meter);
    REQUIRE(has_label_containing(window, "RMS: 0.125  Peak: 0.500"));

    window.refresh();
    REQUIRE(tree->find_node_by_user_data(knob_ptr) != nullptr);
}

TEST_CASE("InspectorWindow refreshes console performance and state tabs", "[inspect][window][issue-641]") {
    View inspected_root;
    inspected_root.set_id("root");
    inspected_root.add_child(std::make_unique<Label>("child"));

    InspectorWindow window(inspected_root);
    auto* tabs = dynamic_cast<TabPanel*>(window.child_at(0));
    REQUIRE(tabs != nullptr);

    ConsoleCapture capture;
    auto console = capture.callback();
    console("warn", "careful");
    console("debug", "trace");
    window.set_console_capture(&capture);
    tabs->set_active_tab(1);
    window.refresh();
    REQUIRE(tabs->active_tab() == 1);
    REQUIRE(tabs->child_at(1)->visible());
    REQUIRE(collect_views_of_type<ConsoleEntryView>(*tabs->child_at(1)).size() == 2);

    pulp::render::RenderPassManager rpm;
    rpm.set_budget(1.0f);
    rpm.begin_frame();
    rpm.begin_pass(pulp::render::RenderPassType::background);
    rpm.end_pass(0.75f, 3);
    rpm.begin_pass(pulp::render::RenderPassType::overlay);
    rpm.end_pass(1.75f, 7);
    rpm.end_frame();

    window.set_render_pass_manager(&rpm);
    tabs->set_active_tab(2);
    window.refresh();
    REQUIRE(has_label_containing(*tabs->child_at(2), "FPS: 400.0"));
    REQUIRE(has_label_containing(*tabs->child_at(2), "Frame time: 2.50 ms"));
    REQUIRE(has_label_containing(*tabs->child_at(2), "Budget: 1.0 ms  [OVER BUDGET]"));
    REQUIRE(has_label_containing(*tabs->child_at(2), "View count: 2"));
    REQUIRE(has_label_containing(*tabs->child_at(2), "background: 0.75 ms, 3 draws"));
    REQUIRE(has_label_containing(*tabs->child_at(2), "overlay: 1.75 ms, 7 draws"));

    InspectorWindow missing_perf_window(inspected_root);
    auto* missing_perf_tabs = dynamic_cast<TabPanel*>(missing_perf_window.child_at(0));
    REQUIRE(missing_perf_tabs != nullptr);
    missing_perf_tabs->set_active_tab(2);
    missing_perf_window.refresh();
    REQUIRE(has_label_containing(*missing_perf_tabs->child_at(2), "FPS: (no data)"));

    StateStore store;
    ParamInfo gain;
    gain.id = 7;
    gain.name = "Gain";
    gain.unit = "dB";
    gain.range = {-60.0f, 12.0f, -12.0f, 0.5f};
    gain.to_string = [](float value) {
        return std::to_string(static_cast<int>(value)) + " dB";
    };
    store.add_parameter(gain);
    store.set_value(7, -6.0f);

    StateInspector state_inspector(store);
    window.set_state_inspector(&state_inspector);
    tabs->set_active_tab(3);
    window.refresh();
    REQUIRE(has_label_containing(*tabs->child_at(3), "Gain: -6.000 (-6 dB)"));
    REQUIRE(has_label_containing(*tabs->child_at(3), "Range: [-60.00, 12.00] step=0.500  Unit: dB"));
}

// ── ConsoleCapture ──────────────────────────────────────────────────────────

#include <pulp/inspect/console_capture.hpp>

TEST_CASE("ConsoleCapture: captures log entries") {
    ConsoleCapture capture;
    auto cb = capture.callback();
    cb("log", "hello");
    cb("warn", "caution");
    cb("error", "fail");
    auto entries = capture.entries();
    REQUIRE(entries.size() == 3);
    REQUIRE(entries[0].level == "log");
    REQUIRE(entries[0].message == "hello");
    REQUIRE(entries[2].level == "error");
}

TEST_CASE("ConsoleCapture: chains previous callback") {
    std::string captured;
    auto previous = [&](std::string_view level, std::string_view msg) {
        captured = std::string(level) + ":" + std::string(msg);
    };
    ConsoleCapture capture;
    auto cb = capture.callback(previous);
    cb("log", "test");
    REQUIRE(captured == "log:test");
    REQUIRE(capture.entries().size() == 1);
}

TEST_CASE("ConsoleCapture: clear") {
    ConsoleCapture capture;
    auto cb = capture.callback();
    cb("log", "a");
    cb("log", "b");
    capture.clear();
    REQUIRE(capture.entries().empty());
}

TEST_CASE("ConsoleCapture: retains the newest ring buffer entries", "[inspect][console][issue-641]") {
    ConsoleCapture capture;
    auto cb = capture.callback();

    for (int i = 0; i < 205; ++i)
        cb("log", "entry-" + std::to_string(i));

    auto entries = capture.entries();
    REQUIRE(entries.size() == 200);
    REQUIRE(entries.front().message == "entry-5");
    REQUIRE(entries.back().message == "entry-204");

    capture.clear();
    REQUIRE(capture.entries().empty());
}

// ── AudioInspector ──────────────────────────────────────────────────────────

#include <pulp/inspect/audio_inspector.hpp>

TEST_CASE("AudioInspector: config roundtrip") {
    AudioInspector audio;
    AudioConfig cfg;
    cfg.sample_rate = 48000;
    cfg.buffer_size = 256;
    cfg.output_channels = 2;
    audio.set_config(cfg);
    auto read = audio.config();
    REQUIRE(read.sample_rate == 48000);
    REQUIRE(read.buffer_size == 256);
}

TEST_CASE("AudioInspector: MIDI logging") {
    AudioInspector audio;
    audio.log_midi(0x90, 60, 100, "Note On C4");
    audio.log_midi(0x80, 60, 0, "Note Off C4");
    auto events = audio.recent_midi();
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].status == 0x90);
    REQUIRE(events[0].description == "Note On C4");
}

TEST_CASE("AudioInspector: metering gates level snapshots") {
    AudioInspector audio;

    audio.report_levels({{0.5f, 0.25f}});
    REQUIRE(audio.latest_levels().empty());

    audio.set_metering_enabled(true);
    audio.report_levels({{0.8f, 0.4f}, {0.25f, 0.1f}});

    auto levels = audio.latest_levels();
    REQUIRE(levels.size() == 2);
    REQUIRE(levels[0].peak == 0.8f);
    REQUIRE(levels[1].rms == 0.1f);
}

// ── DomainHandler ───────────────────────────────────────────────────────────

#include <pulp/inspect/domain_handler.hpp>
#include <pulp/inspect/state_inspector.hpp>
#include <pulp/state/store.hpp>

TEST_CASE("DomainHandler: unknown domain") {
    DomainHandler handler;
    auto resp = handler.handle(make_request(1, "Bogus.method"));
    REQUIRE(resp.is_error);
}

TEST_CASE("DomainHandler: rejects malformed dispatch and missing inspect roots", "[inspect][domain][issue-641]") {
    DomainHandler handler;

    auto invalid = handler.handle(make_request(1, "DOMGetDocument"));
    REQUIRE(invalid.is_error);
    REQUIRE(invalid.params_json == "Invalid method: DOMGetDocument");

    auto unknown = handler.handle(make_request(2, "Bogus.method"));
    REQUIRE(unknown.is_error);
    REQUIRE(unknown.params_json == "Unknown domain: Bogus");

    auto dom_missing_root = handler.handle(make_request(3, methods::kDOMGetDocument));
    REQUIRE(dom_missing_root.is_error);
    REQUIRE(dom_missing_root.params_json == "No root view attached");

    auto css_missing_root = handler.handle(make_request(4, methods::kCSSGetComputedStyle, R"({"id":"root"})"));
    REQUIRE(css_missing_root.is_error);
    REQUIRE(css_missing_root.params_json == "No root view attached");
}

TEST_CASE("DomainHandler: Inspector.getInfo") {
    View root;
    DomainHandler handler;
    handler.set_root_view(&root);
    auto resp = handler.handle(make_request(1, "Inspector.getInfo"));
    REQUIRE_FALSE(resp.is_error);
    REQUIRE(resp.params_json.find("Pulp") != std::string::npos);
}

TEST_CASE("DomainHandler: DOM.getDocument") {
    View root;
    auto child = std::make_unique<View>();
    child->set_id("child1");
    root.add_child(std::move(child));
    DomainHandler handler;
    handler.set_root_view(&root);
    auto resp = handler.handle(make_request(1, "DOM.getDocument"));
    REQUIRE_FALSE(resp.is_error);
    REQUIRE(resp.params_json.find("child1") != std::string::npos);
}

TEST_CASE("DomainHandler: DOM and CSS reject malformed params", "[inspect][domain][issue-641]") {
    View root;
    root.set_id("root");

    DomainHandler handler;
    handler.set_root_view(&root);

    auto dom_bad_json = handler.handle(make_request(1, methods::kDOMGetNodeById, "{"));
    REQUIRE(dom_bad_json.is_error);
    REQUIRE(dom_bad_json.params_json == "Invalid params for DOM.getNodeById");

    auto dom_missing_id = handler.handle(make_request(2, methods::kDOMGetNodeById, R"({"nodeId":"root"})"));
    REQUIRE(dom_missing_id.is_error);
    REQUIRE(dom_missing_id.params_json == "Invalid params for DOM.getNodeById");

    auto search_bad_json = handler.handle(make_request(3, methods::kDOMSearch, "{"));
    REQUIRE(search_bad_json.is_error);
    REQUIRE(search_bad_json.params_json == "Invalid params for DOM.search");

    auto css_bad_json = handler.handle(make_request(4, methods::kCSSGetComputedStyle, "{"));
    REQUIRE(css_bad_json.is_error);
    REQUIRE(css_bad_json.params_json == "Invalid params for CSS.getComputedStyle");

    auto css_missing_id = handler.handle(make_request(5, methods::kCSSGetComputedStyle, R"({"nodeId":"root"})"));
    REQUIRE(css_missing_id.is_error);
    REQUIRE(css_missing_id.params_json == "Invalid params for CSS.getComputedStyle");
}

TEST_CASE("DomainHandler: State.getParameters") {
    StateStore store;
    store.add_parameter({0, "Volume", "dB", {-60.0f, 6.0f, -12.0f}});
    store.set_value(0, -6.0f);
    StateInspector state_inspector(store);
    DomainHandler handler;
    handler.set_state_inspector(&state_inspector);
    auto resp = handler.handle(make_request(1, "State.getParameters"));
    REQUIRE_FALSE(resp.is_error);
    REQUIRE(resp.params_json.find("Volume") != std::string::npos);
}

TEST_CASE("DomainHandler: Audio domain exposes config and MIDI log") {
    AudioInspector audio;
    AudioConfig cfg;
    cfg.sample_rate = 48000;
    cfg.buffer_size = 128;
    cfg.input_channels = 1;
    cfg.output_channels = 2;
    cfg.latency_samples = 64;
    audio.set_config(cfg);
    audio.log_midi(0x90, 60, 100, "Note On C4");

    DomainHandler handler;
    handler.set_audio_inspector(&audio);

    auto config = handler.handle(make_request(1, methods::kAudioGetConfig));
    REQUIRE_FALSE(config.is_error);
    REQUIRE(config.params_json.find("\"sample_rate\"") != std::string::npos);
    REQUIRE(config.params_json.find("48000") != std::string::npos);
    REQUIRE(config.params_json.find("\"buffer_size\"") != std::string::npos);
    REQUIRE(config.params_json.find("128") != std::string::npos);
    REQUIRE(config.params_json.find("\"latency_samples\"") != std::string::npos);
    REQUIRE(config.params_json.find("64") != std::string::npos);

    REQUIRE_FALSE(audio.metering_enabled());
    auto metering = handler.handle(make_request(2, methods::kAudioEnableMetering));
    REQUIRE_FALSE(metering.is_error);
    REQUIRE(audio.metering_enabled());
    REQUIRE(metering.params_json.find("\"metering\":true") != std::string::npos);

    auto midi = handler.handle(make_request(3, methods::kAudioGetMidiLog));
    REQUIRE_FALSE(midi.is_error);
    REQUIRE(midi.params_json.find("\"status\"") != std::string::npos);
    REQUIRE(midi.params_json.find("144") != std::string::npos);
    REQUIRE(midi.params_json.find("\"data1\"") != std::string::npos);
    REQUIRE(midi.params_json.find("60") != std::string::npos);
    REQUIRE(midi.params_json.find("\"description\"") != std::string::npos);
    REQUIRE(midi.params_json.find("Note On C4") != std::string::npos);

    auto unknown = handler.handle(make_request(4, "Audio.unknown"));
    REQUIRE(unknown.is_error);

    DomainHandler missing_audio;
    auto missing = missing_audio.handle(make_request(5, methods::kAudioGetConfig));
    REQUIRE(missing.is_error);
}

TEST_CASE("DomainHandler: dispatches inspector domain edge paths", "[inspect][domain][issue-641]") {
    View root;
    root.set_id("root");
    root.set_bounds({0, 0, 320, 200});
    Theme theme;
    theme.colors["accent.primary"] = Color::rgba8(12, 34, 56);
    root.set_theme(theme);

    auto child = std::make_unique<Label>("Child");
    child->set_id("child");
    child->set_bounds({5, 6, 70, 20});
    child->set_opacity(0.5f);
    child->set_visible(false);
    child->flex().direction = FlexDirection::row;
    child->flex().gap = 8;
    child->flex().padding = 3;
    child->flex().margin = 2;
    root.add_child(std::move(child));

    InspectorOverlay overlay(root);
    ConsoleCapture capture;
    auto console = capture.callback();
    console("info", "hello");

    StateStore store;
    ParamInfo gain;
    gain.id = 9;
    gain.name = "Gain";
    gain.unit = "dB";
    gain.range = {-60.0f, 12.0f, 0.0f, 0.5f};
    store.add_parameter(gain);
    StateInspector state(store);

    AudioInspector audio;
    AudioConfig cfg;
    cfg.sample_rate = 44100;
    cfg.buffer_size = 256;
    audio.set_config(cfg);

    pulp::render::RenderPassManager rpm;
    rpm.begin_frame();
    rpm.begin_pass(pulp::render::RenderPassType::content);
    rpm.end_pass(4.0f, 11);
    rpm.end_frame();

    DomainHandler handler;
    handler.set_root_view(&root);
    handler.set_overlay(&overlay);
    handler.set_console_capture(&capture);
    handler.set_state_inspector(&state);
    handler.set_audio_inspector(&audio);
    handler.set_render_pass_manager(&rpm);

    auto invalid = handler.handle(make_request(1, "InvalidMethod"));
    REQUIRE(invalid.is_error);
    REQUIRE(invalid.params_json.find("Invalid method") != std::string::npos);

    auto enabled = handler.handle(make_request(2, methods::kInspectorEnable));
    REQUIRE_FALSE(enabled.is_error);
    REQUIRE(overlay.is_active());

    auto disabled = handler.handle(make_request(3, methods::kInspectorDisable));
    REQUIRE_FALSE(disabled.is_error);
    REQUIRE_FALSE(overlay.is_active());

    auto unknown_inspector = handler.handle(make_request(4, "Inspector.nope"));
    REQUIRE(unknown_inspector.is_error);

    auto node = handler.handle(make_request(5, methods::kDOMGetNodeById, R"({"id":"child"})"));
    REQUIRE_FALSE(node.is_error);
    REQUIRE(node.params_json.find("\"id\"") != std::string::npos);
    REQUIRE(node.params_json.find("child") != std::string::npos);
    REQUIRE(node.params_json.find("child_count") != std::string::npos);

    auto missing_node = handler.handle(make_request(6, methods::kDOMGetNodeById, R"({"id":"missing"})"));
    REQUIRE(missing_node.is_error);

    auto bad_node_params = handler.handle(make_request(7, methods::kDOMGetNodeById, "not json"));
    REQUIRE(bad_node_params.is_error);

    auto highlight = handler.handle(make_request(8, methods::kDOMHighlightNode));
    REQUIRE_FALSE(highlight.is_error);
    auto clear = handler.handle(make_request(9, methods::kDOMClearHighlight));
    REQUIRE_FALSE(clear.is_error);

    auto search = handler.handle(make_request(10, methods::kDOMSearch, R"({"query":"child"})"));
    REQUIRE_FALSE(search.is_error);
    REQUIRE(search.params_json.find("child") != std::string::npos);

    auto bad_search = handler.handle(make_request(11, methods::kDOMSearch, "not json"));
    REQUIRE(bad_search.is_error);

    auto unknown_dom = handler.handle(make_request(12, "DOM.nope"));
    REQUIRE(unknown_dom.is_error);

    auto style = handler.handle(make_request(13, methods::kCSSGetComputedStyle, R"({"id":"child"})"));
    REQUIRE_FALSE(style.is_error);
    REQUIRE(style.params_json.find("direction") != std::string::npos);
    REQUIRE(style.params_json.find("row") != std::string::npos);
    REQUIRE(style.params_json.find("visible") != std::string::npos);
    REQUIRE(style.params_json.find("false") != std::string::npos);

    auto missing_style = handler.handle(make_request(14, methods::kCSSGetComputedStyle, R"({"id":"missing"})"));
    REQUIRE(missing_style.is_error);

    auto bad_style = handler.handle(make_request(15, methods::kCSSGetComputedStyle, "not json"));
    REQUIRE(bad_style.is_error);

    auto theme_resp = handler.handle(make_request(16, methods::kCSSGetTheme));
    REQUIRE_FALSE(theme_resp.is_error);
    REQUIRE(theme_resp.params_json.find("accent.primary") != std::string::npos);

    auto unknown_css = handler.handle(make_request(17, "CSS.nope"));
    REQUIRE(unknown_css.is_error);

    auto perf = handler.handle(make_request(18, methods::kPerfGetMetrics));
    REQUIRE_FALSE(perf.is_error);
    REQUIRE(perf.params_json.find("total_time_ms") != std::string::npos);
    REQUIRE(perf.params_json.find("draw_calls") != std::string::npos);
    REQUIRE(perf.params_json.find("11") != std::string::npos);

    auto tracking = handler.handle(make_request(19, methods::kPerfEnableTracking));
    REQUIRE_FALSE(tracking.is_error);

    auto unknown_perf = handler.handle(make_request(20, "Performance.nope"));
    REQUIRE(unknown_perf.is_error);

    auto set_param = handler.handle(make_request(21, methods::kStateSetParameter, R"({"id":9,"value":-12.5})"));
    REQUIRE_FALSE(set_param.is_error);
    REQUIRE(store.get_value(9) == -12.5f);

    auto bad_set_param = handler.handle(make_request(22, methods::kStateSetParameter, "not json"));
    REQUIRE(bad_set_param.is_error);

    auto unknown_state = handler.handle(make_request(23, "State.nope"));
    REQUIRE(unknown_state.is_error);

    auto console_entries = handler.handle(make_request(24, methods::kConsoleEnable));
    REQUIRE_FALSE(console_entries.is_error);
    REQUIRE(console_entries.params_json.find("hello") != std::string::npos);

    auto unknown_console = handler.handle(make_request(25, "Console.nope"));
    REQUIRE(unknown_console.is_error);

    auto runtime_eval = handler.handle(make_request(26, methods::kRuntimeEvaluate));
    REQUIRE(runtime_eval.is_error);

    auto hot_reload = handler.handle(make_request(27, methods::kRuntimeGetHotReloadStatus));
    REQUIRE_FALSE(hot_reload.is_error);
    REQUIRE(hot_reload.params_json.find("available") != std::string::npos);
    REQUIRE(hot_reload.params_json.find("false") != std::string::npos);

    auto unknown_runtime = handler.handle(make_request(28, "Runtime.nope"));
    REQUIRE(unknown_runtime.is_error);

    auto screenshot = handler.handle(make_request(29, methods::kCaptureScreenshot));
    REQUIRE(screenshot.is_error);
    auto screenshot_node = handler.handle(make_request(30, methods::kCaptureScreenshotNode));
    REQUIRE(screenshot_node.is_error);
    auto unknown_capture = handler.handle(make_request(31, "Capture.nope"));
    REQUIRE(unknown_capture.is_error);

    DomainHandler missing_sources;
    REQUIRE(missing_sources.handle(make_request(32, methods::kDOMGetDocument)).is_error);
    REQUIRE(missing_sources.handle(make_request(33, methods::kCSSGetTheme)).is_error);
    REQUIRE(missing_sources.handle(make_request(34, methods::kStateGetParameters)).is_error);
    REQUIRE(missing_sources.handle(make_request(35, methods::kAudioGetConfig)).is_error);

    auto no_console = missing_sources.handle(make_request(36, methods::kConsoleEnable));
    REQUIRE_FALSE(no_console.is_error);
    REQUIRE(no_console.params_json == "[]");

    auto no_perf = missing_sources.handle(make_request(37, methods::kPerfGetMetrics));
    REQUIRE_FALSE(no_perf.is_error);
    REQUIRE(no_perf.params_json.find("available") != std::string::npos);
    REQUIRE(no_perf.params_json.find("false") != std::string::npos);
}

// ─── StateInspector ListenerToken migration (Slice 3) ───────────────────────

TEST_CASE("StateInspector records parameter changes after subscribing",
          "[inspect][state][listener]") {
    StateStore store;
    ParamInfo info;
    info.id = 42;
    info.name = "Cutoff";
    info.unit = "Hz";
    info.range = {20.0f, 20000.0f, 1000.0f};
    store.add_parameter(info);

    StateInspector inspector(store);

    REQUIRE(inspector.recent_changes().empty());

    store.set_value(42, 2400.0f);
    store.set_value(42, 4800.0f);

    auto changes = inspector.recent_changes();
    REQUIRE(changes.size() == 2);
    REQUIRE(changes[0].id == 42);
    REQUIRE(changes[1].value > changes[0].value);
}

TEST_CASE("Destroying StateInspector removes its listener (no alive-guard)",
          "[inspect][state][listener]") {
    StateStore store;
    ParamInfo info;
    info.id = 1;
    info.name = "Gain";
    info.range = {0.0f, 1.0f, 0.5f};
    store.add_parameter(info);

    {
        StateInspector inspector(store);
        store.set_value(1, 0.25f);
        REQUIRE(inspector.recent_changes().size() == 1);
    } // ~StateInspector() — ListenerToken dtor unregisters

    // The store no longer has a live listener pointing at the
    // destroyed inspector. With the legacy alive-guard pattern, an
    // entry was still in the listener list (just no-op-checking
    // alive). With ListenerToken it's actually removed, so this
    // set_value is a pure atomic store + a notify() that iterates
    // an empty snapshot. No use-after-free, no leak.
    store.set_value(1, 0.75f);
    REQUIRE_THAT(store.get_value(1), Catch::Matchers::WithinAbs(0.75, 0.001));
}

// ─── Performance.setRepaintFlash (Tier A Slice 6) ───────────────────────────

#include <pulp/render/dirty_tracker.hpp>

TEST_CASE("Performance.setRepaintFlash toggles DirtyTracker::debug_overlay",
          "[inspect][perf][repaint-flash]") {
    pulp::render::DirtyTracker dirty;
    REQUIRE_FALSE(dirty.debug_overlay());

    DomainHandler handler;
    handler.set_dirty_tracker(&dirty);

    auto enable_req = make_request(1, methods::kPerfSetRepaintFlash,
                                   R"({"enabled":true})");
    auto enable_resp = handler.handle(enable_req);
    REQUIRE_FALSE(enable_resp.is_error);
    REQUIRE(dirty.debug_overlay());

    auto get_req = make_request(2, methods::kPerfGetRepaintFlash);
    auto get_resp = handler.handle(get_req);
    REQUIRE_FALSE(get_resp.is_error);
    REQUIRE(get_resp.params_json.find("\"enabled\": true")
            != std::string::npos);
    REQUIRE(get_resp.params_json.find("\"available\": true")
            != std::string::npos);

    auto disable_req = make_request(3, methods::kPerfSetRepaintFlash,
                                    R"({"enabled":false})");
    auto disable_resp = handler.handle(disable_req);
    REQUIRE_FALSE(disable_resp.is_error);
    REQUIRE_FALSE(dirty.debug_overlay());
}

TEST_CASE("Performance.setRepaintFlash without a tracker reports unavailable",
          "[inspect][perf][repaint-flash]") {
    DomainHandler handler;
    // Deliberately not calling set_dirty_tracker — the inspector
    // grew the toggle, but the host process may not have wired one
    // yet. Behavior: get reports available=false; set returns an
    // error so the UI can grey out the toggle.
    auto get_resp = handler.handle(
        make_request(1, methods::kPerfGetRepaintFlash));
    REQUIRE_FALSE(get_resp.is_error);
    REQUIRE(get_resp.params_json.find("\"available\": false")
            != std::string::npos);

    auto set_resp = handler.handle(
        make_request(2, methods::kPerfSetRepaintFlash,
                     R"({"enabled":true})"));
    REQUIRE(set_resp.is_error);
}

// ─── LiveConstant RPC (Tier A Slice 13) ─────────────────────────────────────

#include <pulp/view/live_constant_editor.hpp>

TEST_CASE("LiveConstant.list returns the registry contents",
          "[inspect][live-constant]") {
    auto& registry = pulp::view::LiveConstantRegistry::instance();

    // Seed the registry. PULP_LIVE_CONSTANT macros do this implicitly,
    // but we call register_constant directly so the test doesn't have
    // to compile in a TU that already has them.
    [[maybe_unused]] auto& cutoff =
        registry.register_constant("test_cutoff", __FILE__, __LINE__,
                                   /*default*/ 440.0f,
                                   /*min*/ 20.0f, /*max*/ 20000.0f);
    [[maybe_unused]] auto& gain =
        registry.register_constant("test_gain", __FILE__, __LINE__,
                                   0.0f, -60.0f, 12.0f);

    DomainHandler handler;
    auto resp = handler.handle(make_request(1, methods::kLiveConstList));
    REQUIRE_FALSE(resp.is_error);
    REQUIRE(resp.params_json.find("test_cutoff") != std::string::npos);
    REQUIRE(resp.params_json.find("test_gain") != std::string::npos);
    REQUIRE(resp.params_json.find("\"constants\"") != std::string::npos);
}

TEST_CASE("LiveConstant.set mutates the registry value",
          "[inspect][live-constant]") {
    auto& registry = pulp::view::LiveConstantRegistry::instance();
    [[maybe_unused]] auto& v =
        registry.register_constant("test_setter", __FILE__, __LINE__,
                                   1.0f, 0.0f, 10.0f);
    registry.reset("test_setter");

    DomainHandler handler;
    auto resp = handler.handle(make_request(
        1, methods::kLiveConstSet,
        R"({"name":"test_setter","value":4.5})"));
    REQUIRE_FALSE(resp.is_error);
    REQUIRE_THAT(registry.get("test_setter"),
                 Catch::Matchers::WithinAbs(4.5, 0.001));
}

TEST_CASE("LiveConstant.set without a name returns an error",
          "[inspect][live-constant]") {
    DomainHandler handler;
    auto resp = handler.handle(make_request(
        1, methods::kLiveConstSet, R"({"value":1.0})"));
    REQUIRE(resp.is_error);
}

TEST_CASE("LiveConstant.reset rolls a value back to its default",
          "[inspect][live-constant]") {
    auto& registry = pulp::view::LiveConstantRegistry::instance();
    [[maybe_unused]] auto& v =
        registry.register_constant("test_reset", __FILE__, __LINE__,
                                   2.0f, 0.0f, 10.0f);
    registry.set("test_reset", 7.5f);
    REQUIRE_THAT(registry.get("test_reset"),
                 Catch::Matchers::WithinAbs(7.5, 0.001));

    DomainHandler handler;
    auto resp = handler.handle(make_request(
        1, methods::kLiveConstReset, R"({"name":"test_reset"})"));
    REQUIRE_FALSE(resp.is_error);
    REQUIRE_THAT(registry.get("test_reset"),
                 Catch::Matchers::WithinAbs(2.0, 0.001));
}

// ── Phase 6.1 — per-pass GPU/render attribution viewer ──────────────────────
//
// Spec: planning/2026-05-19-inspector-phase6-gpu-perf-spike.md § Phase 6.1.
// The viewer surfaces per-pass render cost over a rolling 60-frame window
// using RenderPassManager's existing CPU-time PassStats. True GPU
// timestamps are deferred to Phase 6.5 — these tests assert only what the
// RenderPassManager can actually report today.

namespace {

using pulp::render::RenderPassManager;
using pulp::render::RenderPassType;

// Drive one synthetic render frame through the manager: begin a frame,
// emit each (type, time_ms, draw_calls) pass, end the frame.
void render_synthetic_frame(
    RenderPassManager& rpm,
    const std::vector<std::tuple<RenderPassType, float, int>>& passes) {
    rpm.begin_frame();
    for (auto& [type, ms, dc] : passes) {
        rpm.begin_pass(type);
        rpm.end_pass(ms, dc);
    }
    rpm.end_frame();
}

// Count RecordingCanvas fill_text commands whose text contains `needle`.
int count_text_containing(const pulp::canvas::RecordingCanvas& canvas,
                          std::string_view needle) {
    int n = 0;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::fill_text &&
            cmd.text.find(needle) != std::string::npos)
            ++n;
    }
    return n;
}

} // namespace

TEST_CASE("InspectorOverlay Phase 6.1: capture_pass_frame is a no-op without an RPM",
          "[inspect][overlay][phase6]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorOverlay overlay(root);

    REQUIRE_FALSE(overlay.capture_pass_frame());
    REQUIRE(overlay.pass_frames_captured() == 0);
    // Every pass entry reports zero history.
    for (const auto& a : overlay.pass_attribution())
        REQUIRE(a.samples == 0);
}

TEST_CASE("InspectorOverlay Phase 6.1: capture accumulates per-pass history",
          "[inspect][overlay][phase6]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorOverlay overlay(root);

    RenderPassManager rpm;
    overlay.set_render_pass_manager(&rpm);

    // Frame 1: background + content passes.
    render_synthetic_frame(rpm, {
        {RenderPassType::background, 1.0f, 3},
        {RenderPassType::content,    4.0f, 12},
    });
    REQUIRE(overlay.capture_pass_frame());
    REQUIRE(overlay.pass_frames_captured() == 1);

    // Frame 2: content gets heavier, an overlay pass appears.
    render_synthetic_frame(rpm, {
        {RenderPassType::background, 1.0f, 3},
        {RenderPassType::content,    8.0f, 20},
        {RenderPassType::overlay,    2.0f, 5},
    });
    REQUIRE(overlay.capture_pass_frame());
    REQUIRE(overlay.pass_frames_captured() == 2);

    auto attrib = overlay.pass_attribution();
    REQUIRE(attrib.size() == 5);  // one entry per RenderPassType.

    // background: two samples, steady at 1.0ms.
    const auto& bg = attrib[static_cast<int>(RenderPassType::background)];
    REQUIRE(bg.samples == 2);
    REQUIRE(bg.present);
    REQUIRE_THAT(bg.last_cpu_ms, Catch::Matchers::WithinAbs(1.0, 0.001));
    REQUIRE_THAT(bg.avg_cpu_ms, Catch::Matchers::WithinAbs(1.0, 0.001));
    REQUIRE(bg.last_draw_calls == 3);

    // content: avg of 4 and 8 = 6, peak 8, last 8.
    const auto& content = attrib[static_cast<int>(RenderPassType::content)];
    REQUIRE(content.samples == 2);
    REQUIRE_THAT(content.last_cpu_ms, Catch::Matchers::WithinAbs(8.0, 0.001));
    REQUIRE_THAT(content.avg_cpu_ms, Catch::Matchers::WithinAbs(6.0, 0.001));
    REQUIRE_THAT(content.peak_cpu_ms, Catch::Matchers::WithinAbs(8.0, 0.001));
    REQUIRE(content.peak_draw_calls == 20);

    // overlay: only one sample (absent from frame 1).
    const auto& overlay_pass = attrib[static_cast<int>(RenderPassType::overlay)];
    REQUIRE(overlay_pass.samples == 1);
    REQUIRE(overlay_pass.present);

    // effects + post never rendered — no history, not present.
    const auto& effects = attrib[static_cast<int>(RenderPassType::effects)];
    REQUIRE(effects.samples == 0);
    REQUIRE_FALSE(effects.present);
}

TEST_CASE("InspectorOverlay Phase 6.1: capture de-dups within a single frame",
          "[inspect][overlay][phase6]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorOverlay overlay(root);

    RenderPassManager rpm;
    overlay.set_render_pass_manager(&rpm);

    render_synthetic_frame(rpm, {{RenderPassType::content, 5.0f, 10}});

    // First capture records the frame; a second capture of the SAME
    // frame (no begin_frame between) must be a no-op so paint() running
    // multiple times per frame doesn't inflate history.
    REQUIRE(overlay.capture_pass_frame());
    REQUIRE_FALSE(overlay.capture_pass_frame());
    REQUIRE(overlay.pass_frames_captured() == 1);

    auto attrib = overlay.pass_attribution();
    REQUIRE(attrib[static_cast<int>(RenderPassType::content)].samples == 1);
}

TEST_CASE("InspectorOverlay Phase 6.1: same pass type twice in a frame sums",
          "[inspect][overlay][phase6]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorOverlay overlay(root);

    RenderPassManager rpm;
    overlay.set_render_pass_manager(&rpm);

    // Two overlay passes in one frame — the history sample is the
    // frame's TOTAL overlay cost (1.5 + 0.5 = 2.0ms, 4 + 2 = 6 draws).
    render_synthetic_frame(rpm, {
        {RenderPassType::overlay, 1.5f, 4},
        {RenderPassType::overlay, 0.5f, 2},
    });
    REQUIRE(overlay.capture_pass_frame());

    auto attrib = overlay.pass_attribution();
    const auto& ov = attrib[static_cast<int>(RenderPassType::overlay)];
    REQUIRE(ov.samples == 1);
    REQUIRE_THAT(ov.last_cpu_ms, Catch::Matchers::WithinAbs(2.0, 0.001));
    REQUIRE(ov.last_draw_calls == 6);
}

TEST_CASE("InspectorOverlay Phase 6.1: history ring caps at kPassHistoryFrames",
          "[inspect][overlay][phase6]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorOverlay overlay(root);

    RenderPassManager rpm;
    overlay.set_render_pass_manager(&rpm);

    // Push more frames than the ring holds; only the last N count
    // toward per-pass detail, but the lifetime counter keeps growing.
    const std::size_t extra = 25;
    const std::size_t total = InspectorOverlay::kPassHistoryFrames + extra;
    for (std::size_t i = 0; i < total; ++i) {
        render_synthetic_frame(rpm, {{RenderPassType::content,
                                      static_cast<float>(i % 7), 1}});
        REQUIRE(overlay.capture_pass_frame());
    }
    REQUIRE(overlay.pass_frames_captured() == total);

    auto attrib = overlay.pass_attribution();
    const auto& content = attrib[static_cast<int>(RenderPassType::content)];
    REQUIRE(content.samples == InspectorOverlay::kPassHistoryFrames);
}

TEST_CASE("InspectorOverlay Phase 6.1: budget overruns are counted",
          "[inspect][overlay][phase6]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorOverlay overlay(root);

    RenderPassManager rpm;
    rpm.set_budget(5.0f);  // tight 5ms budget.
    overlay.set_render_pass_manager(&rpm);

    // Frame 1: under budget (3ms).
    render_synthetic_frame(rpm, {{RenderPassType::content, 3.0f, 4}});
    REQUIRE(overlay.capture_pass_frame());
    REQUIRE(overlay.budget_overrun_count() == 0);

    // Frame 2: over budget (9ms).
    render_synthetic_frame(rpm, {{RenderPassType::content, 9.0f, 4}});
    REQUIRE(overlay.capture_pass_frame());
    REQUIRE(overlay.budget_overrun_count() == 1);

    // Frame 3: over budget again (12ms).
    render_synthetic_frame(rpm, {{RenderPassType::content, 12.0f, 4}});
    REQUIRE(overlay.capture_pass_frame());
    REQUIRE(overlay.budget_overrun_count() == 2);
}

TEST_CASE("InspectorOverlay Phase 6.1: P key toggles the attribution viewer",
          "[inspect][overlay][phase6]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorOverlay overlay(root);
    overlay.set_active(true);

    REQUIRE_FALSE(overlay.pass_viewer_enabled());

    KeyEvent p;
    p.key = KeyCode::p;
    p.is_down = true;
    p.modifiers = 0;
    REQUIRE(overlay.handle_key_event(p));
    REQUIRE(overlay.pass_viewer_enabled());

    REQUIRE(overlay.handle_key_event(p));
    REQUIRE_FALSE(overlay.pass_viewer_enabled());
}

TEST_CASE("InspectorOverlay Phase 6.1: P key ignored when inspector inactive",
          "[inspect][overlay][phase6]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorOverlay overlay(root);
    // Not active — the P hotkey must not fire so it can't collide with
    // a plain text field in the host UI.

    KeyEvent p;
    p.key = KeyCode::p;
    p.is_down = true;
    p.modifiers = 0;
    REQUIRE_FALSE(overlay.handle_key_event(p));
    REQUIRE_FALSE(overlay.pass_viewer_enabled());
}

TEST_CASE("InspectorOverlay Phase 6.1: viewer renders per-pass rows",
          "[inspect][overlay][phase6]") {
    View root;
    root.set_id("root");
    // Tall enough that the panel's lower section fits both pass rows
    // (heading + summary + two ~56px rows). The lower section gets
    // roughly half the window height minus the stats bar.
    root.set_bounds({0, 0, 500, 720});
    InspectorOverlay overlay(root);
    overlay.set_active(true);

    RenderPassManager rpm;
    overlay.set_render_pass_manager(&rpm);

    // Drive a few frames with two distinct pass types.
    for (int i = 0; i < 3; ++i) {
        render_synthetic_frame(rpm, {
            {RenderPassType::background, 1.0f, 2},
            {RenderPassType::content,    5.0f + static_cast<float>(i), 14},
        });
        // paint() captures the frame; toggle viewer on first.
        if (i == 0) overlay.set_pass_viewer_enabled(true);
        pulp::canvas::RecordingCanvas warmup;
        overlay.paint(warmup);
    }

    pulp::canvas::RecordingCanvas canvas;
    overlay.paint(canvas);
    REQUIRE(canvas.command_count() > 0);

    // The viewer heading and both rendered pass names must appear.
    REQUIRE(count_text_containing(canvas, "Render Passes") >= 1);
    REQUIRE(count_text_containing(canvas, "background") >= 1);
    REQUIRE(count_text_containing(canvas, "content") >= 1);
    // Honesty note about CPU vs GPU timing is surfaced.
    REQUIRE(count_text_containing(canvas, "Phase 6.5") >= 1);
    // "effects" never rendered — must NOT show a row for it.
    REQUIRE(count_text_containing(canvas, "effects") == 0);
}

TEST_CASE("InspectorOverlay Phase 6.1: viewer reports missing RPM gracefully",
          "[inspect][overlay][phase6]") {
    View root;
    root.set_bounds({0, 0, 500, 320});
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_pass_viewer_enabled(true);
    // No RenderPassManager attached.

    pulp::canvas::RecordingCanvas canvas;
    overlay.paint(canvas);
    REQUIRE(count_text_containing(canvas, "No RenderPassManager") >= 1);
}

TEST_CASE("InspectorOverlay Phase 6.1: paint() drives capture automatically",
          "[inspect][overlay][phase6]") {
    View root;
    root.set_bounds({0, 0, 500, 320});
    InspectorOverlay overlay(root);
    overlay.set_active(true);

    RenderPassManager rpm;
    overlay.set_render_pass_manager(&rpm);

    render_synthetic_frame(rpm, {{RenderPassType::content, 4.0f, 8}});

    pulp::canvas::RecordingCanvas canvas;
    overlay.paint(canvas);  // should capture the frame internally.
    REQUIRE(overlay.pass_frames_captured() == 1);

    // A second paint of the SAME frame does not re-capture.
    overlay.paint(canvas);
    REQUIRE(overlay.pass_frames_captured() == 1);

    // A new frame, then paint, captures again.
    render_synthetic_frame(rpm, {{RenderPassType::content, 6.0f, 9}});
    overlay.paint(canvas);
    REQUIRE(overlay.pass_frames_captured() == 2);
}

// ── Phase 5.1: source-jump (View provenance + overlay J hotkey) ────────────
//
// planning/2026-05-19-inspector-phase5-source-jump-spike.md § Phase 5.1.

#include <pulp/inspect/source_jump.hpp>
#include <choc/text/choc_JSON.h>

TEST_CASE("View::source_loc round-trips a file:line:col record",
          "[inspect][source-jump]") {
    View v;
    REQUIRE_FALSE(v.has_source_loc());
    REQUIRE_FALSE(v.source_loc().valid());

    v.set_source_loc({"src/Synth.jsx", 42, 7});
    REQUIRE(v.has_source_loc());
    REQUIRE(v.source_loc().valid());
    REQUIRE(v.source_loc().file == "src/Synth.jsx");
    REQUIRE(v.source_loc().line == 42);
    REQUIRE(v.source_loc().col == 7);

    v.clear_source_loc();
    REQUIRE_FALSE(v.has_source_loc());
}

TEST_CASE("ViewInspector::find_by_anchor locates a view by its anchor id",
          "[inspect][source-jump]") {
    View root;
    auto a = std::make_unique<View>();
    a->set_anchor_id("figma:a");
    auto* a_ptr = a.get();
    auto b = std::make_unique<View>();
    b->set_anchor_id("figma:b");
    auto* b_ptr = b.get();
    root.add_child(std::move(a));
    root.add_child(std::move(b));

    REQUIRE(ViewInspector::find_by_anchor(root, "figma:a") == a_ptr);
    REQUIRE(ViewInspector::find_by_anchor(root, "figma:b") == b_ptr);
    REQUIRE(ViewInspector::find_by_anchor(root, "missing") == nullptr);
    // An empty anchor never matches a (default-empty-anchor) view.
    REQUIRE(ViewInspector::find_by_anchor(root, "") == nullptr);
}

TEST_CASE("InspectorOverlay: J jumps to source for a view with provenance",
          "[inspect][overlay][source-jump]") {
    View root;
    root.set_bounds({0, 0, 500, 300});
    auto child = std::make_unique<View>();
    child->set_id("knob");
    child->set_bounds({10, 10, 80, 40});
    child->set_source_loc({"src/Panel.jsx", 24, 3});
    auto* child_ptr = child.get();
    root.add_child(std::move(child));

    InspectorOverlay overlay(root);
    overlay.set_active(true);

    // Select the child via a click.
    MouseEvent click;
    click.position = {20, 20};
    click.is_down = true;
    REQUIRE(overlay.handle_mouse_event(click));
    REQUIRE(overlay.selected_view() == child_ptr);

    // Dry-run jump — resolves the URL but never spawns the editor.
    auto result = overlay.jump_to_selection_source(/*dry_run=*/true);
    REQUIRE(result.ok);
    REQUIRE(result.url == "vscode://file/src/Panel.jsx:24");
    REQUIRE_FALSE(result.launched);

    // The J hotkey is consumed while the inspector is active.
    KeyEvent jk;
    jk.key = KeyCode::j;
    jk.modifiers = 0;
    jk.is_down = true;
    REQUIRE(overlay.handle_key_event(jk));
}

TEST_CASE("InspectorOverlay: J is a graceful no-op without a selection",
          "[inspect][overlay][source-jump]") {
    View root;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    REQUIRE(overlay.selected_view() == nullptr);

    auto result = overlay.jump_to_selection_source(/*dry_run=*/true);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.url.empty());

    // The key is still consumed (does not fall through to the view tree),
    // but no jump happens.
    KeyEvent jk;
    jk.key = KeyCode::j;
    jk.modifiers = 0;
    jk.is_down = true;
    REQUIRE(overlay.handle_key_event(jk));
}

TEST_CASE("InspectorOverlay: J is a graceful no-op for a non-imported view",
          "[inspect][overlay][source-jump]") {
    View root;
    root.set_bounds({0, 0, 500, 300});
    auto child = std::make_unique<View>();
    child->set_bounds({10, 10, 80, 40});
    // deliberately NO set_source_loc — a user-authored, non-JSX view.
    root.add_child(std::move(child));

    InspectorOverlay overlay(root);
    overlay.set_active(true);
    MouseEvent click;
    click.position = {20, 20};
    click.is_down = true;
    overlay.handle_mouse_event(click);
    REQUIRE(overlay.selected_view() != nullptr);

    auto result = overlay.jump_to_selection_source(/*dry_run=*/true);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("no source location") != std::string::npos);
}

TEST_CASE("InspectorOverlay: J does nothing when the inspector is inactive",
          "[inspect][overlay][source-jump]") {
    View root;
    InspectorOverlay overlay(root);
    // inspector NOT active
    KeyEvent jk;
    jk.key = KeyCode::j;
    jk.modifiers = 0;
    jk.is_down = true;
    REQUIRE_FALSE(overlay.handle_key_event(jk));
}

TEST_CASE("InspectorOverlay: config swap changes the source-jump template",
          "[inspect][overlay][source-jump]") {
    View root;
    root.set_bounds({0, 0, 500, 300});
    auto child = std::make_unique<View>();
    child->set_bounds({10, 10, 80, 40});
    child->set_source_loc({"a/B.tsx", 11, 2});
    root.add_child(std::move(child));

    InspectorOverlay overlay(root);
    overlay.set_active(true);
    MouseEvent click;
    click.position = {20, 20};
    click.is_down = true;
    overlay.handle_mouse_event(click);

    InspectorConfig cfg;
    cfg.editor_url_template = "zed://file/{path}:{line}:{col}";
    overlay.set_config(cfg);

    auto result = overlay.jump_to_selection_source(/*dry_run=*/true);
    REQUIRE(result.ok);
    REQUIRE(result.url == "zed://file/a/B.tsx:11:2");
}

TEST_CASE("DomainHandler propagates config to the attached overlay",
          "[inspect][source-jump][domain]") {
    View root;
    root.set_bounds({0, 0, 500, 300});
    auto child = std::make_unique<View>();
    child->set_anchor_id("anchor-1");
    child->set_bounds({10, 10, 80, 40});
    child->set_source_loc({"x/Y.jsx", 5, 0});
    root.add_child(std::move(child));

    InspectorOverlay overlay(root);
    DomainHandler handler;
    handler.set_root_view(&root);
    handler.set_overlay(&overlay);

    InspectorConfig cfg;
    cfg.editor_url_template = "cursor://file/{path}:{line}";
    handler.set_config(cfg);

    // The overlay's config now matches the handler's.
    REQUIRE(overlay.config().editor_url_template
            == "cursor://file/{path}:{line}");

    // And Inspector.jumpToSource via the anchor resolves with it.
    auto resp = handler.handle(make_request(
        1, methods::kInspectorJumpToSource,
        R"({"anchorId":"anchor-1","dryRun":true})"));
    REQUIRE_FALSE(resp.is_error);
    auto obj = choc::json::parse(resp.params_json);
    REQUIRE(obj["ok"].getBool());
    REQUIRE(std::string(obj["url"].getString())
            == "cursor://file/x/Y.jsx:5");
}

// ── Phase 2 — drift drawer ──────────────────────────────────────────────
//
// The drift drawer is a collapsible warning panel inside the inspector
// overlay that lists tweaks whose anchor_id no longer resolves to a live
// view. It is populated by refresh_drift(), which walks the live view
// tree's anchor set and diffs it against the attached TweakStore.
// Spec: planning/2026-05-18-inspector-direct-manipulation-roadmap.md
// (Phase 2).
namespace {

// True if any fill_text command's text contains `needle`.
bool canvas_has_text(const pulp::canvas::RecordingCanvas& canvas,
                     std::string_view needle) {
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::fill_text &&
            cmd.text.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("InspectorOverlay: refresh_drift finds tweaks with no live anchor",
          "[inspect][overlay][drift][phase2]") {
    View root;
    root.set_bounds({0, 0, 500, 400});

    auto child = std::make_unique<View>();
    child->set_anchor_id("anchor-still-here");
    root.add_child(std::move(child));

    TweakStore store;
    // One tweak whose anchor is live, one whose anchor is gone.
    store.apply_tweak("anchor-still-here", "layout.padding",
                      choc::value::createInt32(8));
    store.apply_tweak("anchor-removed-by-reimport", "paint.color",
                      choc::value::createString("#ff0000"));

    InspectorOverlay overlay(root);
    overlay.set_tweak_store(&store);
    overlay.refresh_drift();

    REQUIRE(overlay.drift_count() == 1);
    REQUIRE(overlay.drifted().front().anchor_id ==
            "anchor-removed-by-reimport");
    REQUIRE(overlay.drifted().front().reason ==
            TweakStore::DriftReason::anchor_not_found);
    // First drift detection auto-expands the drawer so it is not silent.
    REQUIRE(overlay.drift_drawer_open());
}

TEST_CASE("InspectorOverlay: no drift means no drawer and an empty list",
          "[inspect][overlay][drift][phase2]") {
    View root;
    root.set_bounds({0, 0, 500, 400});
    auto child = std::make_unique<View>();
    child->set_anchor_id("live-anchor");
    root.add_child(std::move(child));

    TweakStore store;
    store.apply_tweak("live-anchor", "layout.width",
                      choc::value::createInt32(120));

    InspectorOverlay overlay(root);
    overlay.set_tweak_store(&store);
    overlay.refresh_drift();

    REQUIRE(overlay.drift_count() == 0);
    REQUIRE(overlay.drifted().empty());

    // The drawer paints nothing on the happy path.
    overlay.set_active(true);
    pulp::canvas::RecordingCanvas canvas;
    overlay.paint(canvas);
    REQUIRE_FALSE(canvas_has_text(canvas, "Drift"));
}

TEST_CASE("InspectorOverlay: refresh_drift with no TweakStore is a safe no-op",
          "[inspect][overlay][drift][phase2]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorOverlay overlay(root);
    overlay.refresh_drift();  // no store wired
    REQUIRE(overlay.drift_count() == 0);
    REQUIRE(overlay.drifted().empty());
}

TEST_CASE("InspectorOverlay: drift drawer renders header and orphan rows",
          "[inspect][overlay][drift][phase2]") {
    View root;
    root.set_bounds({0, 0, 600, 500});
    auto child = std::make_unique<View>();
    child->set_anchor_id("kept");
    root.add_child(std::move(child));

    TweakStore store;
    store.apply_tweak("kept", "layout.width",
                      choc::value::createInt32(64));
    store.apply_tweak("gone-anchor", "layout.margin",
                      choc::value::createInt32(10));

    InspectorOverlay overlay(root);
    overlay.set_tweak_store(&store);
    overlay.set_active(true);

    // First paint auto-refreshes drift and auto-expands the drawer.
    pulp::canvas::RecordingCanvas canvas;
    overlay.paint(canvas);

    REQUIRE(overlay.drift_count() == 1);
    REQUIRE(overlay.drift_drawer_open());
    // Header text + the orphaned anchor + its reason tag are painted.
    REQUIRE(canvas_has_text(canvas, "Drift"));
    REQUIRE(canvas_has_text(canvas, "gone-anchor"));
    REQUIRE(canvas_has_text(canvas, "anchor-not-found"));
    REQUIRE(canvas_has_text(canvas, "layout.margin"));
}

TEST_CASE("InspectorOverlay: clicking the drift header toggles the drawer",
          "[inspect][overlay][drift][phase2]") {
    View root;
    root.set_bounds({0, 0, 600, 500});
    auto child = std::make_unique<View>();
    child->set_anchor_id("present");
    root.add_child(std::move(child));

    TweakStore store;
    store.apply_tweak("present", "layout.width",
                      choc::value::createInt32(80));
    store.apply_tweak("missing", "paint.opacity",
                      choc::value::createFloat32(0.4f));

    InspectorOverlay overlay(root);
    overlay.set_tweak_store(&store);
    overlay.set_active(true);

    pulp::canvas::RecordingCanvas canvas;
    overlay.paint(canvas);  // auto-expands; populates the header hit-rect
    REQUIRE(overlay.drift_drawer_open());

    // The drawer header sits just below the tree section (root_h * 0.5
    // = 250) on the panel side (panel_x = 600 - 300 = 300). A click in
    // that band collapses the drawer.
    MouseEvent click;
    click.position = {360, 258};
    click.is_down = true;
    REQUIRE(overlay.handle_mouse_event(click));
    REQUIRE_FALSE(overlay.drift_drawer_open());

    // Re-paint so the (now-collapsed) header hit-rect is refreshed,
    // then click again to re-expand.
    canvas.clear();
    overlay.paint(canvas);
    MouseEvent click2;
    click2.position = {360, 258};
    click2.is_down = true;
    REQUIRE(overlay.handle_mouse_event(click2));
    REQUIRE(overlay.drift_drawer_open());
}

TEST_CASE("InspectorOverlay: collapsed drawer hides rows but keeps the header",
          "[inspect][overlay][drift][phase2]") {
    View root;
    root.set_bounds({0, 0, 600, 500});

    TweakStore store;
    store.apply_tweak("orphan-a", "layout.width",
                      choc::value::createInt32(50));

    InspectorOverlay overlay(root);
    overlay.set_tweak_store(&store);
    overlay.set_active(true);
    overlay.refresh_drift();
    overlay.set_drift_drawer_open(false);

    pulp::canvas::RecordingCanvas canvas;
    overlay.paint(canvas);
    // Header still shows the drift count; the orphan row does not.
    REQUIRE(canvas_has_text(canvas, "Drift"));
    REQUIRE_FALSE(canvas_has_text(canvas, "layout.width"));
}
