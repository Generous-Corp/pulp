#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/inspect/inspector_window.hpp>
#include <pulp/render/atlas_inventory.hpp>
#include <pulp/view/inspector.hpp>
#include <pulp/view/widgets.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
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

struct ScopedEnv {
    explicit ScopedEnv(std::string name) : name_(std::move(name)) {
        if (const char* prev = std::getenv(name_.c_str())) {
            prev_ = std::string(prev);
            had_prev_ = true;
        }
    }

    void set(const std::string& value) {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), value.c_str());
#else
        ::setenv(name_.c_str(), value.c_str(), /*overwrite=*/1);
#endif
    }

    void unset() {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), "");
#else
        ::unsetenv(name_.c_str());
#endif
    }

    ~ScopedEnv() {
        if (had_prev_) set(prev_);
        else unset();
    }

private:
    std::string name_;
    std::string prev_;
    bool had_prev_ = false;
};

// A RecordingCanvas that also serves real pixel readback over a small
// synthetic surface — lets phase-3e tests exercise the loupe's
// readback path (which the plain RecordingCanvas never does) and
// assert the read rect that the loupe actually requested.
class ReadbackCanvas : public pulp::canvas::RecordingCanvas {
public:
    ReadbackCanvas(int w, int h) : surface_w_(w), surface_h_(h) {}

    bool read_pixels(int x, int y, int width, int height,
                     std::uint8_t* out) override {
        // Mirror Skia's SkSurface::readPixels() contract: the source
        // rect must lie fully within the surface or the read fails.
        if (!out || width <= 0 || height <= 0) return false;
        if (x < 0 || y < 0 ||
            x + width > surface_w_ || y + height > surface_h_) {
            return false;
        }
        last_read_x_ = x;
        last_read_y_ = y;
        last_read_w_ = width;
        last_read_h_ = height;
        ++read_count_;
        // Fill with a deterministic non-checkerboard color so callers
        // can tell "real readback" from the loupe's fallback render.
        for (int i = 0; i < width * height; ++i) {
            out[i * 4 + 0] = 64;
            out[i * 4 + 1] = 128;
            out[i * 4 + 2] = 192;
            out[i * 4 + 3] = 255;
        }
        return true;
    }

    int  last_read_x() const { return last_read_x_; }
    int  last_read_y() const { return last_read_y_; }
    int  last_read_w() const { return last_read_w_; }
    int  last_read_h() const { return last_read_h_; }
    int  read_count()  const { return read_count_; }

private:
    int surface_w_, surface_h_;
    int last_read_x_ = -1, last_read_y_ = -1;
    int last_read_w_ = -1, last_read_h_ = -1;
    int read_count_ = 0;
};

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

// Phase 3 — selection-mode toggle. The `M` hotkey flips between
// follows_focus (click-to-select; the default) and follows_mouse
// (selection chases the pointer). The default must be follows_focus so
// the inspector's historical behavior is unchanged until the user opts
// in.
TEST_CASE("InspectorOverlay: M hotkey toggles selection mode",
          "[inspect][overlay][phase3]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    InspectorOverlay overlay(root);

    // Default is follows_focus — historical click-to-select behavior.
    REQUIRE(overlay.selection_mode() ==
            InspectorOverlay::SelectionMode::follows_focus);

    overlay.set_active(true);

    KeyEvent m;
    m.key = KeyCode::m;
    m.is_down = true;
    m.modifiers = 0;

    // First M press → follows_mouse.
    REQUIRE(overlay.handle_key_event(m));
    REQUIRE(overlay.selection_mode() ==
            InspectorOverlay::SelectionMode::follows_mouse);

    // Second M press → back to follows_focus.
    REQUIRE(overlay.handle_key_event(m));
    REQUIRE(overlay.selection_mode() ==
            InspectorOverlay::SelectionMode::follows_focus);

    // The M hotkey only fires while the inspector is active — a press
    // with the overlay closed must not flip the mode.
    overlay.set_active(false);
    REQUIRE_FALSE(overlay.handle_key_event(m));
    REQUIRE(overlay.selection_mode() ==
            InspectorOverlay::SelectionMode::follows_focus);
}

// In follows_focus mode a pointer-move must NOT change the selection —
// only an explicit click does. This is the conservative default that
// preserves the inspector's historical behavior.
TEST_CASE("InspectorOverlay: follows_focus keeps selection pinned on hover",
          "[inspect][overlay][phase3]") {
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
    REQUIRE(overlay.selection_mode() ==
            InspectorOverlay::SelectionMode::follows_focus);

    // Click to select a.
    MouseEvent click_a;
    click_a.position = {20, 20};
    click_a.is_down = true;
    REQUIRE(overlay.handle_mouse_event(click_a));
    REQUIRE(overlay.selected_view() == a_ptr);

    // Hover over b (pointer-move, no button) — hovered_ tracks b but the
    // selection stays pinned to a.
    MouseEvent hover_b;
    hover_b.position = {130, 30};
    hover_b.is_down = false;
    REQUIRE_FALSE(overlay.handle_mouse_event(hover_b));
    REQUIRE(overlay.hovered_view() == b_ptr);
    REQUIRE(overlay.selected_view() == a_ptr);  // unchanged by hover
}

// In follows_mouse mode a pointer-move DOES re-select the hovered View
// (Figma-style "select on hover"). An explicit click still works the
// same way.
TEST_CASE("InspectorOverlay: follows_mouse re-selects the hovered view",
          "[inspect][overlay][phase3]") {
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
    overlay.set_selection_mode(InspectorOverlay::SelectionMode::follows_mouse);

    // Click to select a.
    MouseEvent click_a;
    click_a.position = {20, 20};
    click_a.is_down = true;
    REQUIRE(overlay.handle_mouse_event(click_a));
    REQUIRE(overlay.selected_view() == a_ptr);

    // Hover over b (pointer-move, no button) — selection follows the
    // pointer onto b.
    MouseEvent hover_b;
    hover_b.position = {130, 30};
    hover_b.is_down = false;
    REQUIRE_FALSE(overlay.handle_mouse_event(hover_b));
    REQUIRE(overlay.hovered_view() == b_ptr);
    REQUIRE(overlay.selected_view() == b_ptr);  // selection chased pointer

    // Hover back over a — selection follows again.
    MouseEvent hover_a;
    hover_a.position = {30, 30};
    hover_a.is_down = false;
    REQUIRE_FALSE(overlay.handle_mouse_event(hover_a));
    REQUIRE(overlay.selected_view() == a_ptr);

    // Alt-hover must NOT chase the pointer — Alt-hover sibling-distance
    // mode relies on a pinned selection.
    MouseEvent alt_hover_b;
    alt_hover_b.position = {130, 30};
    alt_hover_b.modifiers = kModAlt;
    alt_hover_b.is_down = false;
    REQUIRE_FALSE(overlay.handle_mouse_event(alt_hover_b));
    REQUIRE(overlay.selected_view() == a_ptr);  // pinned despite follows_mouse
}

// Codex P1 follow-up on #2556: in follows_mouse mode a hover must NOT
// chase the pointer while a numeric field edit is in progress.
// begin_field_edit() snapshots the edit target, but write_field_value()
// / commit_field_edit() operate on the *current* selected_. If a
// mid-edit hover were allowed to move selected_, the edit would commit
// to the wrong (or no-longer-valid) node. follows_focus mode is already
// safe because it never chases the pointer; this guards follows_mouse.
TEST_CASE("InspectorOverlay: follows_mouse hover does not move selection "
          "during a field edit (codex P1 #2556 regression)",
          "[inspect][overlay][phase3][regression]") {
    View root;
    root.set_id("root");
    root.set_bounds({0, 0, 500, 300});

    auto a = std::make_unique<View>();
    a->set_id("a");
    a->set_anchor_id("figma:edit-target");
    a->set_bounds({10, 10, 60, 60});
    a->flex().padding = 8;
    auto* a_ptr = a.get();
    root.add_child(std::move(a));

    auto b = std::make_unique<View>();
    b->set_id("b");
    b->set_bounds({100, 10, 60, 60});
    b->flex().padding = 99;
    auto* b_ptr = b.get();
    root.add_child(std::move(b));

    TweakStore store;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_selection_mode(InspectorOverlay::SelectionMode::follows_mouse);

    // Click to select a, then start a numeric field edit on it.
    MouseEvent click_a;
    click_a.position = {20, 20};
    click_a.is_down = true;
    REQUIRE(overlay.handle_mouse_event(click_a));
    REQUIRE(overlay.selected_view() == a_ptr);

    REQUIRE(overlay.begin_field_edit("layout.padding", 8.0f));
    REQUIRE(overlay.is_editing());

    // Move the mouse over b mid-edit. In follows_mouse mode this would
    // normally re-select b — but a field edit is in progress, so the
    // selection must stay pinned to the edit target a.
    MouseEvent hover_b;
    hover_b.position = {130, 30};
    hover_b.is_down = false;
    REQUIRE_FALSE(overlay.handle_mouse_event(hover_b));
    REQUIRE(overlay.hovered_view() == b_ptr);    // hover tracking still works
    REQUIRE(overlay.selected_view() == a_ptr);   // selection NOT chased

    // Type a new value and commit — the edit must land on a, not b.
    KeyEvent num5;
    num5.key = KeyCode::num5;
    num5.is_down = true;
    REQUIRE(overlay.handle_key_event(num5));  // "8" → "85"
    REQUIRE(overlay.commit_field_edit());
    REQUIRE_FALSE(overlay.is_editing());

    REQUIRE(a_ptr->flex().padding == 85.0f);  // edit committed to a
    REQUIRE(b_ptr->flex().padding == 99.0f);  // b untouched
    REQUIRE(store.count() == 1);              // exactly one tweak emitted

    // After the edit ends, follows_mouse resumes chasing the pointer.
    MouseEvent hover_b2;
    hover_b2.position = {130, 30};
    hover_b2.is_down = false;
    REQUIRE_FALSE(overlay.handle_mouse_event(hover_b2));
    REQUIRE(overlay.selected_view() == b_ptr);  // chasing resumes post-edit
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

// ── Phase 3e — 20× zoom loupe ─────────────────────────────────────────────
//
// The loupe is a magnified-pixel preview panel toggled with the Z key.
// It samples the region under the cursor (via Canvas::read_pixels() when
// available, else a graceful resolved-color fallback) and renders a
// magnified grid + center crosshair + coordinate / hex readout. These
// tests exercise it through the headless RecordingCanvas — which does
// NOT implement read_pixels(), so they all run the fallback path.

TEST_CASE("InspectorOverlay: Z toggles the zoom loupe on and off",
          "[inspect][overlay][phase3e]") {
    View root;
    root.set_bounds({0, 0, 400, 300});

    InspectorOverlay overlay(root);
    overlay.set_active(true);
    REQUIRE_FALSE(overlay.zoom_active());

    KeyEvent z;
    z.key = KeyCode::z;
    z.is_down = true;
    z.modifiers = 0;
    REQUIRE(overlay.handle_key_event(z));
    REQUIRE(overlay.zoom_active());

    // Toggle back off.
    REQUIRE(overlay.handle_key_event(z));
    REQUIRE_FALSE(overlay.zoom_active());

    // Z does nothing when the inspector itself is inactive.
    overlay.set_active(false);
    REQUIRE_FALSE(overlay.handle_key_event(z));
    REQUIRE_FALSE(overlay.zoom_active());
}

TEST_CASE("InspectorOverlay: dismissing the inspector closes the loupe",
          "[inspect][overlay][phase3e]") {
    View root;
    root.set_bounds({0, 0, 400, 300});

    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_zoom_active(true);
    REQUIRE(overlay.zoom_active());

    // set_active(false) should reset the transient loupe state.
    overlay.set_active(false);
    REQUIRE_FALSE(overlay.zoom_active());
}

TEST_CASE("InspectorOverlay: zoom panel renders extra commands when active",
          "[inspect][overlay][phase3e]") {
    View root;
    root.set_bounds({0, 0, 400, 300});

    InspectorOverlay overlay(root);
    overlay.set_active(true);

    // Baseline: loupe OFF.
    pulp::canvas::RecordingCanvas baseline;
    overlay.paint(baseline);
    auto baseline_count = baseline.command_count();
    REQUIRE(baseline_count > 0);

    // Loupe ON → paint_zoom_panel() draws the panel, the magnified
    // grid, grid lines, the crosshair, and the readout — strictly more
    // commands than the baseline.
    overlay.set_zoom_active(true);
    pulp::canvas::RecordingCanvas with_loupe;
    overlay.paint(with_loupe);
    REQUIRE(with_loupe.command_count() > baseline_count);

    // The grid is an N×N block of filled cells, so the loupe alone
    // contributes well over a hundred fill_rect commands.
    auto fills = with_loupe.count(
        pulp::canvas::DrawCommand::Type::fill_rect);
    REQUIRE(fills > 100);
}

TEST_CASE("InspectorOverlay: loupe readout reflects the hovered position",
          "[inspect][overlay][phase3e]") {
    // Wide root so the test cursor positions land in the canvas area
    // (left of the 300px-wide props panel that hugs the right edge).
    View root;
    root.set_bounds({0, 0, 900, 400});

    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_zoom_active(true);

    // Move the cursor over the canvas — the loupe records the sample
    // center on every mouse event while active, without consuming a
    // plain hover.
    MouseEvent move;
    move.position = {137, 92};
    move.is_down = false;
    REQUIRE_FALSE(overlay.handle_mouse_event(move));

    auto center = overlay.zoom_sample_center();
    REQUIRE(center.x == Catch::Approx(137.0f));
    REQUIRE(center.y == Catch::Approx(92.0f));

    // A second move re-centers the loupe.
    MouseEvent move2;
    move2.position = {210, 40};
    move2.is_down = false;
    overlay.handle_mouse_event(move2);
    REQUIRE(overlay.zoom_sample_center().x == Catch::Approx(210.0f));
    REQUIRE(overlay.zoom_sample_center().y == Catch::Approx(40.0f));

    // The loupe also tracks the cursor when it's over the props panel
    // (panel hover events ARE consumed, but the loupe still records).
    MouseEvent panel_move;
    panel_move.position = {700, 50};  // x >= 900-300 → inside panel
    panel_move.is_down = false;
    overlay.handle_mouse_event(panel_move);
    REQUIRE(overlay.zoom_sample_center().x == Catch::Approx(700.0f));
}

TEST_CASE("InspectorOverlay: loupe falls back to resolved view color",
          "[inspect][overlay][phase3e]") {
    // RecordingCanvas has no read_pixels() — exercises the graceful
    // no-readback path. With a background-colored view under the
    // cursor, the center-pixel readout should resolve to that color.
    View root;
    root.set_bounds({0, 0, 400, 300});

    auto panel = std::make_unique<View>();
    panel->set_bounds({50, 50, 200, 150});
    const Color kPanelColor = Color::rgba(0.2f, 0.6f, 0.9f, 1.0f);
    panel->set_background_color(kPanelColor);
    root.add_child(std::move(panel));

    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_zoom_active(true);

    // Cursor inside the colored panel.
    MouseEvent inside;
    inside.position = {120, 110};
    inside.is_down = false;
    overlay.handle_mouse_event(inside);

    pulp::canvas::RecordingCanvas canvas;
    overlay.paint(canvas);  // update_zoom_sample() runs inside paint()

    auto c = overlay.zoom_center_color();
    REQUIRE(c.r == Catch::Approx(kPanelColor.r));
    REQUIRE(c.g == Catch::Approx(kPanelColor.g));
    REQUIRE(c.b == Catch::Approx(kPanelColor.b));

    // Cursor outside any background-bearing view → fully transparent.
    MouseEvent outside;
    outside.position = {10, 10};
    outside.is_down = false;
    overlay.handle_mouse_event(outside);
    overlay.paint(canvas);
    REQUIRE(overlay.zoom_center_color().a == Catch::Approx(0.0f));
}

TEST_CASE("InspectorOverlay: zoom factor is clamped to a sane range",
          "[inspect][overlay][phase3e]") {
    View root;
    root.set_bounds({0, 0, 400, 300});

    InspectorOverlay overlay(root);
    REQUIRE(overlay.zoom_factor() == 20);  // roadmap default

    overlay.set_zoom_factor(8);
    REQUIRE(overlay.zoom_factor() == 8);

    // Out-of-range requests clamp rather than degenerate the grid.
    overlay.set_zoom_factor(1);
    REQUIRE(overlay.zoom_factor() >= 4);

    overlay.set_zoom_factor(1000);
    REQUIRE(overlay.zoom_factor() <= 40);
}

TEST_CASE("InspectorOverlay: loupe clamps the sample window at canvas edges",
          "[inspect][overlay][phase3e]") {
    // codex P2 #2464 — a loupe centered within kZoomGridCells/2 pixels
    // of a canvas edge used to push the cells×cells read rect
    // out of bounds, so read_pixels() rejected the WHOLE block and the
    // grid fell back to checkerboard exactly where edge inspection
    // matters most. With clamping the read rect stays in-bounds and the
    // grid keeps showing real device pixels.
    const int kW = 400, kH = 300;
    View root;
    root.set_bounds({0, 0, static_cast<float>(kW),
                     static_cast<float>(kH)});

    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_zoom_active(true);

    // Park the loupe on the exact top-left corner — the worst case:
    // an unclamped window would read from (-5, -5).
    MouseEvent corner;
    corner.position = {0, 0};
    corner.is_down = false;
    overlay.handle_mouse_event(corner);

    ReadbackCanvas canvas(kW, kH);
    overlay.paint(canvas);

    // The loupe must have issued a block read AND it must have
    // succeeded — i.e. the requested rect was clamped fully in-bounds.
    REQUIRE(canvas.read_count() >= 1);
    REQUIRE(canvas.last_read_x() >= 0);
    REQUIRE(canvas.last_read_y() >= 0);
    REQUIRE(canvas.last_read_x() + canvas.last_read_w() <= kW);
    REQUIRE(canvas.last_read_y() + canvas.last_read_h() <= kH);

    // The center-pixel readout must agree with the block: a real
    // readback succeeded, so the readout reports the synthetic
    // readback color (64,128,192) — not the degraded fallback path.
    auto c = overlay.zoom_center_color();
    REQUIRE(c.r == Catch::Approx(64.0f / 255.0f));
    REQUIRE(c.g == Catch::Approx(128.0f / 255.0f));
    REQUIRE(c.b == Catch::Approx(192.0f / 255.0f));

    // The magnified grid is painted with the synthetic readback color
    // (it is NOT a checkerboard). The loupe draws kZoomGridCells² grid
    // cells; all of them should carry the real readback color, proving
    // the whole block — corner included — used real pixels.
    int readback_cells = 0;
    const Color kReadback =
        Color::rgba(64.0f / 255.0f, 128.0f / 255.0f, 192.0f / 255.0f, 1.0f);
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type != pulp::canvas::DrawCommand::Type::set_fill_color)
            continue;
        if (cmd.color.r == Catch::Approx(kReadback.r) &&
            cmd.color.g == Catch::Approx(kReadback.g) &&
            cmd.color.b == Catch::Approx(kReadback.b)) {
            ++readback_cells;
        }
    }
    // 11×11 = 121 cells, all real-readback colored. Allow a generous
    // floor — the point is "many real cells", not "exactly zero
    // checkerboard" (panel chrome uses other colors).
    REQUIRE(readback_cells >= 100);

    // Bottom-right corner is the symmetric worst case: an unclamped
    // window would read past the surface.
    MouseEvent br;
    br.position = {static_cast<float>(kW), static_cast<float>(kH)};
    br.is_down = false;
    overlay.handle_mouse_event(br);

    ReadbackCanvas canvas2(kW, kH);
    overlay.paint(canvas2);
    REQUIRE(canvas2.read_count() >= 1);
    REQUIRE(canvas2.last_read_x() >= 0);
    REQUIRE(canvas2.last_read_y() >= 0);
    REQUIRE(canvas2.last_read_x() + canvas2.last_read_w() <= kW);
    REQUIRE(canvas2.last_read_y() + canvas2.last_read_h() <= kH);

    auto c2 = overlay.zoom_center_color();
    REQUIRE(c2.r == Catch::Approx(64.0f / 255.0f));
    REQUIRE(c2.g == Catch::Approx(128.0f / 255.0f));
    REQUIRE(c2.b == Catch::Approx(192.0f / 255.0f));
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

