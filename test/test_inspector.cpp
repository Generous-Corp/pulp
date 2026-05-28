#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/inspect/inspector_window.hpp>
#include <pulp/platform/clipboard.hpp>  // WYSIWYG T2 — paste test
#include <pulp/render/atlas_inventory.hpp>
#include <pulp/view/inspector.hpp>
#include <pulp/view/lock_to_source.hpp>  // WYSIWYG T5 — reparent → source rewrite
#include <pulp/view/widgets.hpp>
#include <pulp/view/window_host.hpp>  // P2 scale test: compute_design_viewport_transform

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
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

// WYSIWYG caret-x — a deterministic canvas that MODELS letter-spacing in its
// text metrics (the real RecordingCanvas ignores it: 7px/char flat). This lets
// a headless test prove that Label::text_edit_metrics actually pushes the
// resolved (possibly INHERITED) letter-spacing into the canvas font state, and
// that text_x_for_byte advances by it. Each glyph is `kCharW` wide and adjacent
// glyphs are separated by `letter_spacing` — exactly the model SkParagraph
// uses for letter-spacing, so caret_x_by_byte[end] == shaped width.
class SpacingCanvas : public pulp::canvas::RecordingCanvas {
public:
    static constexpr float kCharW = 10.0f;

    void set_font_full(const std::string& family, float size, int weight,
                       int slant, float letter_spacing) override {
        pulp::canvas::RecordingCanvas::set_font_full(family, size, weight,
                                                     slant, letter_spacing);
        active_letter_spacing_ = letter_spacing;
    }

    float measure_text(const std::string& text) override {
        const std::size_t n = text.size();
        if (n == 0) return 0.0f;
        return static_cast<float>(n) * kCharW
             + static_cast<float>(n - 1) * active_letter_spacing_;
    }

    // text_x_for_byte uses the base default (= measure_text(prefix)), which is
    // exact for this fixed-advance model — the point of the test is the
    // letter-spacing + alignment plumbing, not kerning (that's covered by the
    // Skia canvas test).
    float active_letter_spacing() const { return active_letter_spacing_; }

private:
    float active_letter_spacing_ = 0.0f;
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

TEST_CASE("Protocol: response encoding preserves parseable result JSON",
          "[inspect][protocol][coverage]") {
    auto object_response = encode_message(make_response(7, R"({"root":{"id":"gain"},"count":2})"));
    REQUIRE(object_response.find(R"("id")") != std::string::npos);
    REQUIRE(object_response.find(R"("result")") != std::string::npos);
    REQUIRE(object_response.find(R"("gain")") != std::string::npos);
    REQUIRE(object_response.find(R"("params")") == std::string::npos);

    auto array_response = encode_message(make_response(8, R"([{"id":"gain"},{"id":"mix"}])"));
    REQUIRE(array_response.find(R"("result")") != std::string::npos);
    REQUIRE(array_response.find(R"("mix")") != std::string::npos);

    auto scalar_response = encode_message(make_response(9, R"("ready")"));
    REQUIRE(scalar_response.find(R"("result")") != std::string::npos);
    REQUIRE(scalar_response.find("ready") != std::string::npos);
}

TEST_CASE("Protocol: invalid JSON payloads encode as strings",
          "[inspect][protocol][coverage]") {
    auto response = encode_message(make_response(10, "{not-json"));
    REQUIRE(response.find(R"("result")") != std::string::npos);
    REQUIRE(response.find("{not-json") != std::string::npos);

    auto request = encode_message(make_request(11, "DOM.search", "{not-json"));
    REQUIRE(request.find(R"("id")") != std::string::npos);
    REQUIRE(request.find("DOM.search") != std::string::npos);
    REQUIRE(request.find("{not-json") != std::string::npos);

    auto event = encode_message(make_event("DOM.documentUpdated", "{not-json"));
    REQUIRE(event.find(R"("id")") == std::string::npos);
    REQUIRE(event.find("DOM.documentUpdated") != std::string::npos);
    REQUIRE(event.find("{not-json") != std::string::npos);
}

TEST_CASE("Protocol: empty response and default params omit payload keys",
          "[inspect][protocol][coverage]") {
    auto empty_response = encode_message(make_response(12, ""));
    REQUIRE(empty_response.find(R"("id")") != std::string::npos);
    REQUIRE(empty_response.find(R"("result")") == std::string::npos);

    auto empty_object_response = encode_message(make_response(13, "{}"));
    REQUIRE(empty_object_response.find(R"("result")") == std::string::npos);

    auto default_params_request = encode_message(make_request(14, "Inspector.enable", "{}"));
    REQUIRE(default_params_request.find("Inspector.enable") != std::string::npos);
    REQUIRE(default_params_request.find(R"("params")") == std::string::npos);

    auto default_params_event = encode_message(make_event("Inspector.detached", "{}"));
    REQUIRE(default_params_event.find(R"("id")") == std::string::npos);
    REQUIRE(default_params_event.find("Inspector.detached") != std::string::npos);
}

TEST_CASE("Protocol: error encoding and decoding round-trips message payloads",
          "[inspect][protocol][coverage]") {
    auto encoded = encode_message(make_error(15, "View not found: gain"));
    REQUIRE(encoded.find(R"("id")") != std::string::npos);
    REQUIRE(encoded.find(R"("error")") != std::string::npos);
    REQUIRE(encoded.find("View not found: gain") != std::string::npos);

    InspectorMessage decoded;
    REQUIRE(decode_message(encoded, decoded));
    REQUIRE(decoded.id == 15);
    REQUIRE(decoded.is_error);
    REQUIRE(decoded.params_json == "View not found: gain");

    InspectorMessage no_message;
    REQUIRE(decode_message(R"({"id":16,"error":{"code":-32000}})", no_message));
    REQUIRE(no_message.id == 16);
    REQUIRE(no_message.is_error);
}

TEST_CASE("Protocol: decode preserves params, result arrays, and notifications",
          "[inspect][protocol][coverage]") {
    InspectorMessage request;
    REQUIRE(decode_message(R"({"id":17,"method":"DOM.search","params":{"query":"Knob"}})", request));
    REQUIRE(request.id == 17);
    REQUIRE(request.method == "DOM.search");
    REQUIRE(request.params_json.find(R"("query")") != std::string::npos);
    REQUIRE(request.params_json.find(R"("Knob")") != std::string::npos);

    InspectorMessage response;
    REQUIRE(decode_message(R"({"id":18,"result":[{"id":"gain"},{"id":"mix"}]})", response));
    REQUIRE(response.id == 18);
    REQUIRE(response.params_json.find(R"("gain")") != std::string::npos);
    REQUIRE(response.params_json.find(R"("mix")") != std::string::npos);

    InspectorMessage notification;
    REQUIRE(decode_message(R"({"method":"DOM.documentUpdated","params":{"reason":"reload"}})", notification));
    REQUIRE(notification.id == 0);
    REQUIRE(notification.method == "DOM.documentUpdated");
    REQUIRE(notification.params_json.find(R"("reload")") != std::string::npos);
}

TEST_CASE("Protocol: decode rejects invalid field types without partial output",
          "[inspect][protocol][coverage]") {
    InspectorMessage msg;
    msg.id = 99;
    msg.method = "stale";
    msg.params_json = "stale";

    REQUIRE_FALSE(decode_message(R"({"id":"not-an-int","method":"DOM.search"})", msg));

    msg.id = 99;
    msg.method = "stale";
    msg.params_json = "stale";
    REQUIRE_FALSE(decode_message(R"({"id":20,"method":42})", msg));
}

// ── InspectorOverlay ────────────────────────────────────────────────────────

#include <pulp/inspect/inspector_overlay.hpp>
#include <pulp/view/text_editor.hpp>
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

TEST_CASE("InspectorOverlay: Escape deselects, never dismisses") {
    View root;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    KeyEvent ke;
    ke.key = KeyCode::escape;
    ke.is_down = true;
    // Esc with nothing selected is a NO-OP — the inspector stays active.
    // (It is dismissed only via Cmd+I / the window close button.) Previously
    // a second Esc fell through to set_active(false), which deactivated the
    // overlay so the user had to cycle Cmd+I to interact again.
    REQUIRE(overlay.handle_key_event(ke));
    REQUIRE(overlay.is_active());
    REQUIRE(overlay.handle_key_event(ke));
    REQUIRE(overlay.is_active());
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

TEST_CASE("InspectorOverlay Phase 2.5: Shift+T toggles the tweak management "
          "panel (bare T moved to the Text tool, P3)",
          "[inspect][overlay][phase2.5][phase3]") {
    View root;
    root.set_bounds({0, 0, 600, 600});
    InspectorOverlay overlay(root);
    overlay.set_active(true);

    REQUIRE_FALSE(overlay.tweaks_panel_visible());

    // P3 reconcile: bare T now selects the Text tool, NOT the tweak panel.
    KeyEvent t;
    t.key = KeyCode::t;
    t.is_down = true;
    REQUIRE(overlay.handle_key_event(t));
    REQUIRE(overlay.tool() == InspectorOverlay::Tool::text);
    REQUIRE_FALSE(overlay.tweaks_panel_visible());  // panel NOT flipped by T

    // Shift+T is the new tweak-panel toggle.
    KeyEvent shift_t;
    shift_t.key = KeyCode::t;
    shift_t.modifiers = kModShift;
    shift_t.is_down = true;
    REQUIRE(overlay.handle_key_event(shift_t));
    REQUIRE(overlay.tweaks_panel_visible());

    REQUIRE(overlay.handle_key_event(shift_t));
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

TEST_CASE("CollapsableSection collapsed default paints and toggles predictably",
          "[inspect][window][coverage][headers]") {
    CollapsableSection section("Theme Colors", false);
    section.set_bounds({0, 0, 220, 120});
    section.layout_children();

    REQUIRE_FALSE(section.is_expanded());
    REQUIRE(section.content() != nullptr);
    REQUIRE_FALSE(section.content()->visible());

    section.set_expanded(false);
    REQUIRE_FALSE(section.is_expanded());
    REQUIRE_FALSE(section.content()->visible());

    pulp::canvas::RecordingCanvas collapsed_canvas;
    section.paint(collapsed_canvas);
    REQUIRE(collapsed_canvas.command_count() > 0);

    MouseEvent header_click;
    header_click.position = {8, 8};
    header_click.is_down = true;
    section.on_mouse_event(header_click);
    REQUIRE(section.is_expanded());
    REQUIRE(section.content()->visible());

    pulp::canvas::RecordingCanvas expanded_canvas;
    section.paint(expanded_canvas);
    REQUIRE(expanded_canvas.command_count() >= collapsed_canvas.command_count());
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
    // P3 — the window now has a tool-strip header child + the tab panel.
    REQUIRE(window.child_count() == 2);

    auto* tabs = first_view_of_type<TabPanel>(window);
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

TEST_CASE("InspectorWindow default refresh and selection mirror contracts",
          "[inspect][window][coverage][headers]") {
    InspectorWindow window;
    REQUIRE(window.child_count() == 2);
    REQUIRE(window.active_tool() == 0);

    window.set_active_tool(1);
    REQUIRE(window.active_tool() == 1);
    window.set_active_tool(42);
    REQUIRE(window.active_tool() == 0);
    window.set_active_tool(-7);
    REQUIRE(window.active_tool() == 0);

    auto* tabs = first_view_of_type<TabPanel>(window);
    REQUIRE(tabs != nullptr);
    REQUIRE(tabs->tab_count() == 4);
    REQUIRE(tabs->active_tab() == 0);
    REQUIRE_NOTHROW(window.refresh());

    tabs->set_active_tab(1);
    REQUIRE_NOTHROW(window.refresh());
    REQUIRE(collect_views_of_type<ConsoleEntryView>(*tabs->child_at(1)).empty());

    tabs->set_active_tab(2);
    REQUIRE_NOTHROW(window.refresh());
    REQUIRE(has_label_containing(*tabs->child_at(2), "FPS: (no data)"));

    tabs->set_active_tab(3);
    REQUIRE_NOTHROW(window.refresh());
    REQUIRE_FALSE(has_label_containing(*tabs->child_at(3), "Range: ["));

    bool selected_callback_called = false;
    window.on_view_selected = [&](View*) { selected_callback_called = true; };
    window.reflect_selection(nullptr);
    REQUIRE_FALSE(selected_callback_called);
    window.select_view(nullptr);
    REQUIRE(selected_callback_called);
}

TEST_CASE("Inspector overlay and window public header toggles are direct contracts",
          "[inspect][overlay][window][coverage][requested]") {
    View root;
    root.set_bounds({0, 0, 320, 200});

    InspectorOverlay overlay(root);
    REQUIRE(&overlay.inspected_root() == &root);
    REQUIRE_FALSE(overlay.is_active());
    overlay.set_active(true);
    REQUIRE(overlay.is_active());
    overlay.toggle();
    REQUIRE_FALSE(overlay.is_active());

    REQUIRE(overlay.tool() == InspectorOverlay::Tool::select);
    overlay.toggle_tool();
    REQUIRE(overlay.tool() == InspectorOverlay::Tool::text);
    overlay.set_tool(InspectorOverlay::Tool::select);
    REQUIRE(overlay.tool() == InspectorOverlay::Tool::select);

    REQUIRE_FALSE(overlay.manipulate_only());
    REQUIRE_FALSE(overlay.dragging_enabled());
    overlay.set_manipulate_only(true);
    REQUIRE(overlay.manipulate_only());
    REQUIRE(overlay.dragging_enabled());

    REQUIRE_FALSE(overlay.has_reparent_source_sink());
    overlay.set_reparent_source_sink([](const InspectorOverlay::ReparentSourceEdit&) {});
    REQUIRE(overlay.has_reparent_source_sink());
    overlay.set_reparent_source_sink({});
    REQUIRE_FALSE(overlay.has_reparent_source_sink());

    InspectorWindow window(root);
    REQUIRE_FALSE(window.selection_readonly());
    window.set_selection_readonly(true);
    REQUIRE(window.selection_readonly());
    window.set_selection_readonly(false);
    REQUIRE_FALSE(window.selection_readonly());
}

TEST_CASE("InspectorWindow rebuilds tree and theme sections only from live roots",
          "[inspect][window][coverage][headers]") {
    View first_root;
    first_root.set_id("first-root");
    first_root.set_bounds({0, 0, 480, 320});

    Theme theme;
    theme.colors["accent.primary"] = Color::rgba8(10, 20, 30);
    theme.colors["meter.warn"] = Color::rgba8(200, 120, 40);
    first_root.set_theme(theme);

    auto first_label = std::make_unique<Label>("First");
    first_label->set_id("first-label");
    auto* first_label_ptr = first_label.get();
    first_root.add_child(std::move(first_label));

    InspectorWindow window(first_root);
    auto* tabs = first_view_of_type<TabPanel>(window);
    REQUIRE(tabs != nullptr);
    auto* tree = first_view_of_type<TreeView>(*tabs->child_at(0));
    REQUIRE(tree != nullptr);
    REQUIRE(tree->find_node_by_user_data(&first_root) != nullptr);
    REQUIRE(tree->find_node_by_user_data(first_label_ptr) != nullptr);
    REQUIRE(has_label_containing(window, "accent.primary"));
    REQUIRE(has_label_containing(window, "meter.warn"));

    auto sections = collect_views_of_type<CollapsableSection>(window);
    REQUIRE(sections.size() >= 5);
    REQUIRE_FALSE(sections.back()->is_expanded());
    REQUIRE(sections.back()->content() != nullptr);
    REQUIRE_FALSE(sections.back()->content()->visible());

    View second_root;
    second_root.set_id("second-root");
    second_root.set_bounds({0, 0, 360, 240});
    auto second_label = std::make_unique<Label>("Second");
    second_label->set_id("second-label");
    auto* second_label_ptr = second_label.get();
    second_root.add_child(std::move(second_label));

    window.set_inspected_root(&second_root);
    REQUIRE(tree->find_node_by_user_data(&second_root) != nullptr);
    REQUIRE(tree->find_node_by_user_data(second_label_ptr) != nullptr);
    REQUIRE(tree->find_node_by_user_data(&first_root) == nullptr);
    REQUIRE(tree->find_node_by_user_data(first_label_ptr) == nullptr);

    bool callback_seen = false;
    window.on_view_selected = [&](View* view) {
        callback_seen = (view == second_label_ptr);
    };
    window.reflect_selection(second_label_ptr);
    REQUIRE_FALSE(callback_seen);
    REQUIRE(has_label_containing(window, "ID: second-label"));

    window.select_view(second_label_ptr);
    REQUIRE(callback_seen);
}

// WYSIWYG P4 FIX 3 — a structural rebuild that drops the selected view must
// clear the stale raw `selected_view_` pointer; a subsequent refresh()
// (which would otherwise call show_properties_for(selected_view_)) must not
// deref freed memory. The live React tree can rebuild and destroy the
// previously selected view; this proves the rebuilt-tree path drops the
// dangling selection instead of dereferencing it.
TEST_CASE("InspectorWindow drops stale selection across a tree rebuild",
          "[inspect][window][wysiwyg][p4]") {
    View inspected_root;
    inspected_root.set_id("root");
    inspected_root.set_bounds({0, 0, 480, 320});

    auto knob = std::make_unique<Knob>();
    knob->set_id("gain");
    auto* knob_ptr = knob.get();
    inspected_root.add_child(std::move(knob));

    InspectorWindow window(inspected_root);
    auto* tabs = first_view_of_type<TabPanel>(window);
    REQUIRE(tabs != nullptr);
    auto* tree = first_view_of_type<TreeView>(*tabs->child_at(0));
    REQUIRE(tree != nullptr);
    REQUIRE(tree->find_node_by_user_data(knob_ptr) != nullptr);

    // Select the knob, then verify its properties are shown.
    window.select_view(knob_ptr);
    REQUIRE(has_label_containing(window, "ID: gain"));

    // Destroy the selected view (live tree rebuild — e.g. a meter rebuild).
    // Keep the removed view alive until the replacement child is allocated so
    // allocators cannot immediately recycle knob_ptr for the new child.
    auto removed = inspected_root.remove_child(knob_ptr);
    auto label = std::make_unique<Label>("fresh");
    label->set_id("fresh");
    inspected_root.add_child(std::move(label));
    removed.reset();  // free the previously selected view

    // refresh() rebuilds the tree; the saved selection (knob_ptr) is no longer
    // found, so selected_view_ must be cleared and the deref skipped. With the
    // bug this dereferenced the freed knob (UAF — caught by ASan / crash).
    REQUIRE_NOTHROW(window.refresh());

    // The stale node is gone and the new child is present.
    REQUIRE(tree->find_node_by_user_data(knob_ptr) == nullptr);

    // A further refresh with no selection is still safe (no re-deref).
    REQUIRE_NOTHROW(window.refresh());
}

// WYSIWYG P4 FIX 5 — clear_edit_history() drops every recorded undo/redo
// entry. The undo closures capture raw View* that dangle if the tree is
// rebuilt/re-imported before undo; clearing at the root-replacement seam is
// the conservative guard. This proves the clear empties both stacks.
TEST_CASE("InspectorOverlay clear_edit_history empties the undo history",
          "[inspect][overlay][wysiwyg][p4]") {
    View root;
    root.set_bounds({0, 0, 600, 400});
    auto child = std::make_unique<View>();
    child->set_anchor_id("figma:0:42");
    child->set_bounds({10, 10, 80, 40});
    child->flex().preferred_width = 80;
    child->flex().preferred_height = 40;
    auto* child_ptr = child.get();
    root.add_child(std::move(child));

    TweakStore store;
    pulp::state::EditHistory history;
    history.set_coalesce(false);
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_edit_history(&history);
    overlay.set_dragging_enabled(true);

    // No history attached is a safe no-op.
    {
        InspectorOverlay bare(root);
        REQUIRE_NOTHROW(bare.clear_edit_history());
    }

    // Record one undoable resize gesture (press SE handle, drag, release).
    MouseEvent click;
    click.position = {30, 30};
    click.is_down = true;
    overlay.handle_mouse_event(click);
    REQUIRE(overlay.selected_view() == child_ptr);

    MouseEvent press;
    press.position = {90, 50};
    press.is_down = true;
    REQUIRE(overlay.handle_mouse_event(press));
    MouseEvent drag;
    drag.position = {110, 65};
    drag.is_down = false;
    REQUIRE(overlay.handle_mouse_event(drag));
    MouseEvent release;
    release.position = {300, 300};
    release.is_down = true;
    overlay.handle_mouse_event(release);

    REQUIRE(history.undo_count() == 1);
    REQUIRE(history.can_undo());

    // Simulate a root replacement / re-import: clear the history so the
    // captured (about-to-dangle) View* closures can never be replayed.
    overlay.clear_edit_history();
    REQUIRE_FALSE(history.can_undo());
    REQUIRE_FALSE(history.can_redo());
    REQUIRE(history.undo_count() == 0);
}

// ── WYSIWYG sweep P1 — Cmd+Z UAF on live SUBTREE rebuild ─────────────────────
//
// clear_edit_history() only fires at ROOT replacement; a live React SUBTREE
// rebuild (which frees the edited/reparented view but keeps the root) does NOT
// trigger it. The pre-fix text-edit / reparent EditHistory closures captured
// raw View*, so Cmd+Z after such a rebuild dereferenced freed memory. The fix
// resolves anchored targets by stable anchor at replay time (re-finding the
// CURRENT live view, or a graceful no-op if it's gone), and for un-anchored
// targets clears the history when the tracked raw view leaves the tree.

TEST_CASE("WYSIWYG sweep P1: anchored text-edit undo resolves by anchor after "
          "a subtree rebuild",
          "[inspect][overlay][wysiwyg][undo][uaf][issue-1737]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    auto label = std::make_unique<Label>("Old");
    label->set_anchor_id("figma:label-1");
    label->set_bounds({20, 20, 120, 30});
    auto* label_ptr = label.get();
    root.add_child(std::move(label));

    TweakStore store;
    pulp::state::EditHistory history;
    history.set_coalesce(false);
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_edit_history(&history);
    overlay.set_tool(InspectorOverlay::Tool::text);

    // Edit "Old" → "New" and commit (one undoable entry).
    REQUIRE(overlay.begin_text_edit(label_ptr));
    KeyEvent bs; bs.key = KeyCode::backspace; bs.is_down = true;
    for (int i = 0; i < 3; ++i) overlay.handle_key_event(bs);
    TextInputEvent ti; ti.text = "New";
    overlay.handle_text_input(ti);
    KeyEvent enter; enter.key = KeyCode::enter; enter.is_down = true;
    overlay.handle_key_event(enter);
    REQUIRE(history.undo_count() == 1);
    REQUIRE(label_ptr->text() == "New");

    // ── Simulate a live SUBTREE rebuild: the old label is FREED and a NEW
    // view with the SAME anchor is wired in. (clear_edit_history() is NOT
    // called — this is the seam the minimal P4 guard never covered.)
    {
        auto removed = root.remove_child(label_ptr);  // frees old label here
        (void)removed;
    }
    auto rebuilt = std::make_unique<Label>("New");
    rebuilt->set_anchor_id("figma:label-1");
    rebuilt->set_bounds({20, 20, 120, 30});
    auto* rebuilt_ptr = rebuilt.get();
    root.add_child(std::move(rebuilt));

    // A paint runs rebuild_flat_tree() (the rebuild seam). The dangling-raw
    // check is a no-op here: the target was ANCHORED, so it was never tracked.
    pulp::canvas::RecordingCanvas canvas;
    REQUIRE_NOTHROW(overlay.paint(canvas));
    REQUIRE(history.can_undo());  // anchored history survives the rebuild

    // Undo must NOT deref the freed old label — it re-finds the live view by
    // anchor and reverts THAT view's text. No UAF, correct resolve.
    REQUIRE_NOTHROW(history.undo());
    REQUIRE(rebuilt_ptr->text() == "Old");

    // Redo re-applies on the live (resolved) view.
    REQUIRE_NOTHROW(history.redo());
    REQUIRE(rebuilt_ptr->text() == "New");
}

TEST_CASE("WYSIWYG sweep P1: un-anchored text-edit undo no-ops after the view "
          "is freed (history cleared on rebuild)",
          "[inspect][overlay][wysiwyg][undo][uaf][issue-1737]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    // No anchor — a --script Chainer-style label. Undo can't re-find it.
    auto label = std::make_unique<Label>("Old");
    label->set_bounds({20, 20, 120, 30});
    auto* label_ptr = label.get();
    root.add_child(std::move(label));

    TweakStore store;
    pulp::state::EditHistory history;
    history.set_coalesce(false);
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_edit_history(&history);
    overlay.set_tool(InspectorOverlay::Tool::text);

    REQUIRE(overlay.begin_text_edit(label_ptr));
    KeyEvent bs; bs.key = KeyCode::backspace; bs.is_down = true;
    for (int i = 0; i < 3; ++i) overlay.handle_key_event(bs);
    TextInputEvent ti; ti.text = "New";
    overlay.handle_text_input(ti);
    KeyEvent enter; enter.key = KeyCode::enter; enter.is_down = true;
    overlay.handle_key_event(enter);
    REQUIRE(history.undo_count() == 1);

    // Free the un-anchored label without replacing it (subtree shrank).
    {
        auto removed = root.remove_child(label_ptr);  // frees it here
        (void)removed;
    }

    // The rebuild seam (paint → rebuild_flat_tree) detects the tracked raw view
    // is gone and clears the whole history BEFORE its closures can deref it.
    pulp::canvas::RecordingCanvas canvas;
    REQUIRE_NOTHROW(overlay.paint(canvas));
    REQUIRE_FALSE(history.can_undo());

    // Any later undo is a safe no-op (nothing to deref).
    REQUIRE_NOTHROW(history.undo());
}

TEST_CASE("WYSIWYG sweep P1: anchored reparent undo no-ops after the moved view "
          "is freed by a subtree rebuild",
          "[inspect][overlay][wysiwyg][undo][uaf][reparent][issue-1737]") {
    View root;
    root.set_bounds({0, 0, 600, 300});
    root.flex().direction = FlexDirection::row;

    auto left = std::make_unique<View>();
    left->set_anchor_id("anchor-left");
    left->flex().direction = FlexDirection::column;
    left->flex().preferred_width = 300;
    left->flex().preferred_height = 300;

    auto moving = std::make_unique<View>();
    moving->set_anchor_id("anchor-moving");
    moving->flex().preferred_width = 80;
    moving->flex().preferred_height = 40;
    auto* moving_ptr = moving.get();
    left->add_child(std::move(moving));
    root.add_child(std::move(left));

    auto right = std::make_unique<View>();
    right->set_anchor_id("anchor-right");
    right->flex().direction = FlexDirection::column;
    right->flex().preferred_width = 300;
    right->flex().preferred_height = 300;
    auto* right_ptr = right.get();
    root.add_child(std::move(right));
    root.layout_children();

    TweakStore store;
    pulp::state::EditHistory history;
    history.set_coalesce(false);
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_edit_history(&history);
    overlay.set_dragging_enabled(true);
    overlay.set_selected_view(moving_ptr);

    // Drive the reflow reparent (mirrors the P2c reparent test).
    const Rect mb = moving_ptr->bounds();
    MouseEvent press; press.position = {mb.x + 10, mb.y + 10}; press.is_down = true;
    REQUIRE(overlay.handle_mouse_event(press));
    const Rect rb = right_ptr->bounds();
    MouseEvent drag;
    drag.position = {rb.x + rb.width * 0.5f, rb.y + rb.height * 0.5f};
    drag.is_down = false;
    REQUIRE(overlay.handle_mouse_event(drag));
    MouseEvent release; release.position = drag.position; release.is_down = true;
    overlay.handle_mouse_event(release);
    REQUIRE(moving_ptr->parent() == right_ptr);
    REQUIRE(history.undo_count() == 1);

    // Subtree rebuild frees the moved view (no replacement with that anchor).
    {
        auto removed = right_ptr->remove_child(moving_ptr);  // frees it here
        (void)removed;
    }

    // Paint runs the rebuild seam. The moved child WAS anchored, so the history
    // is NOT cleared (anchored captures self-heal). Undo re-finds the child by
    // anchor; it no longer resolves, so the closure is a graceful no-op — no
    // deref of the freed view.
    pulp::canvas::RecordingCanvas canvas;
    REQUIRE_NOTHROW(overlay.paint(canvas));
    REQUIRE_NOTHROW(history.undo());
    // The freed child stays gone; the live tree is coherent (right empty,
    // left still present).
    REQUIRE(right_ptr->child_count() == 0);
}

TEST_CASE("InspectorWindow refreshes console performance and state tabs", "[inspect][window][issue-641]") {
    View inspected_root;
    inspected_root.set_id("root");
    inspected_root.add_child(std::make_unique<Label>("child"));

    InspectorWindow window(inspected_root);
    auto* tabs = first_view_of_type<TabPanel>(window);
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
    auto* missing_perf_tabs = first_view_of_type<TabPanel>(missing_perf_window);
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

// ════════════════════════════════════════════════════════════════════════════
// P1 — overlay reachability / drill-down / minimal manipulate layer
// P2 — drag-to-move via absolute + layout.position/left/top tweaks
//
// planning/2026-05-21-wysiwyg-direct-manipulation-extension.md
// ════════════════════════════════════════════════════════════════════════════

// P1 acceptance: the overlay actually receives a press-on-handle → move →
// release sequence and lands a layout.width tweak. Proves the gesture
// pipeline works on the input path (the ui-preview composing hooks route
// here). Mirrors the prerequisite's stated acceptance criterion.
TEST_CASE("InspectorOverlay P1: handle drag lands a layout.width tweak "
          "(overlay receives mouse)",
          "[inspect][overlay][p1][issue-wysiwyg-p1]") {
    View root;
    root.set_bounds({0, 0, 600, 400});
    auto child = std::make_unique<View>();
    child->set_anchor_id("anchor-handle");
    child->set_bounds({10, 10, 80, 40});
    auto* child_ptr = child.get();
    root.add_child(std::move(child));

    TweakStore store;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_dragging_enabled(true);

    MouseEvent click; click.position = {30, 30}; click.is_down = true;
    overlay.handle_mouse_event(click);
    REQUIRE(overlay.selected_view() == child_ptr);

    MouseEvent press; press.position = {90, 50}; press.is_down = true;  // SE handle
    REQUIRE(overlay.handle_mouse_event(press));
    MouseEvent drag; drag.position = {110, 65}; drag.is_down = false;
    REQUIRE(overlay.handle_mouse_event(drag));

    auto w = store.lookup("anchor-handle", "layout.width");
    REQUIRE(w.has_value());
    REQUIRE(w->getFloat32() == 100.0f);
}

// P1 drill-down: a click resolves to the DEEPEST hittable element, not the
// container. Esc then DESELECTS (one press, stays active) — the maintainer's
// requested behavior so hover + click keep working without a Cmd+I cycle.
TEST_CASE("InspectorOverlay P1: click selects deepest nested element, "
          "Esc deselects and stays active",
          "[inspect][overlay][p1][drill-down][issue-wysiwyg-p1]") {
    View root;
    root.set_bounds({0, 0, 400, 300});

    auto container = std::make_unique<View>();
    container->set_id("container");
    container->set_anchor_id("anchor-container");
    container->set_bounds({20, 20, 200, 120});
    auto* container_ptr = container.get();

    auto nested = std::make_unique<View>();
    nested->set_id("nested-label");
    nested->set_anchor_id("anchor-nested");
    // bounds are in the PARENT's coord space.
    nested->set_bounds({10, 10, 80, 24});
    auto* nested_ptr = nested.get();
    container->add_child(std::move(nested));
    root.add_child(std::move(container));

    InspectorOverlay overlay(root);
    overlay.set_active(true);

    // Click over the nested label (root coords: container 20,20 + nested
    // 10,10 = 30,30 .. 110,54). Pick a point inside the nested element.
    MouseEvent click; click.position = {40, 35}; click.is_down = true;
    REQUIRE(overlay.handle_mouse_event(click));
    // DEEPEST element selected, NOT the container.
    REQUIRE(overlay.selected_view() == nested_ptr);

    // Esc DESELECTS in one press, and stays active so hover + click keep
    // working without a Cmd+I cycle (maintainer's requested behavior — was
    // ascend-to-parent, which needed multiple presses then deactivated).
    KeyEvent esc; esc.key = KeyCode::escape; esc.is_down = true;
    REQUIRE(overlay.handle_key_event(esc));
    REQUIRE(overlay.selected_view() == nullptr);
    REQUIRE(overlay.is_active());  // still active — deselect, don't exit

    // A second click re-selects the deepest element (no Cmd+I cycle needed).
    REQUIRE(overlay.handle_mouse_event(click));
    REQUIRE(overlay.selected_view() == nested_ptr);
    REQUIRE(overlay.selected_view()->parent() == container_ptr);

    // Esc deselects again; a further Esc with nothing selected is a NO-OP —
    // the inspector stays active (Esc never dismisses; Cmd+I / close does).
    REQUIRE(overlay.handle_key_event(esc));
    REQUIRE(overlay.selected_view() == nullptr);
    REQUIRE(overlay.is_active());
    REQUIRE(overlay.handle_key_event(esc));
    REQUIRE(overlay.is_active());
}

// P1 minimal manipulate layer: in manipulate-only mode point_in_panel is
// false everywhere (whole canvas is live) and paint draws only the
// selection box + handles (no dev side-panel rows).
TEST_CASE("InspectorOverlay P1: manipulate-only mode suppresses the dev panel",
          "[inspect][overlay][p1][manipulate][issue-wysiwyg-p1]") {
    View root;
    root.set_bounds({0, 0, 600, 400});
    auto child = std::make_unique<View>();
    child->set_anchor_id("anchor-x");
    child->set_bounds({10, 10, 80, 40});
    auto* child_ptr = child.get();
    root.add_child(std::move(child));

    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_manipulate_only(true);
    REQUIRE(overlay.manipulate_only());
    REQUIRE(overlay.dragging_enabled());  // implicitly enabled

    // The far-right region (default dev panel x-range) is NOT a panel now —
    // a click there selects the canvas, not a tree row. Select via the
    // child first, then assert a click that would have hit the panel still
    // routes to canvas selection logic (returns consumed, not a tree pick).
    MouseEvent click; click.position = {30, 30}; click.is_down = true;
    overlay.handle_mouse_event(click);
    REQUIRE(overlay.selected_view() == child_ptr);

    // The tweak-management panel rows are only populated by paint_panel,
    // which manipulate-only mode skips entirely. Paint into a recording
    // canvas and confirm no tree/props rows were laid out.
    overlay.set_tweaks_panel_visible(true);
    pulp::canvas::RecordingCanvas canvas;
    overlay.paint(canvas);
    REQUIRE(overlay.tweak_row_count() == 0);   // panel never laid out
}

// ── P2: drag-to-move ────────────────────────────────────────────────────────

// Core acceptance: body-drag converts to absolute and writes
// position+left+top atomically (one batch).
TEST_CASE("InspectorOverlay P2: body-drag moves via absolute + 3 atomic tweaks",
          "[inspect][overlay][p2][move][issue-wysiwyg-p2]") {
    View root;
    root.set_bounds({0, 0, 600, 400});
    root.flex().direction = FlexDirection::row;

    auto child = std::make_unique<View>();
    child->set_anchor_id("anchor-move");
    child->flex().preferred_width = 80;
    child->flex().preferred_height = 40;
    auto* child_ptr = child.get();
    root.add_child(std::move(child));
    root.layout_children();  // resolve initial flex position

    // Snapshot the resolved bounds before the move (it's a flex child).
    const Rect before = child_ptr->bounds();

    TweakStore store;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_dragging_enabled(true);

    // Select the child (first is_down — no move yet because selected_ was
    // null when the press arrived).
    MouseEvent click;
    click.position = {before.x + 10, before.y + 10};
    click.is_down = true;
    overlay.handle_mouse_event(click);
    REQUIRE(overlay.selected_view() == child_ptr);

    // Press on the BODY (not a handle) to start the move. P2c: the DEFAULT
    // body-drag is now reflow-aware; ⌘-drag is the ABSOLUTE FLOAT escape
    // hatch this test exercises, so hold Cmd on the press + drag.
    MouseEvent press;
    press.position = {before.x + 20, before.y + 20};
    press.is_down = true;
    press.modifiers = kModCmd;
    REQUIRE(overlay.handle_mouse_event(press));

    // Drag +50 / +30.
    MouseEvent drag;
    drag.position = {press.position.x + 50, press.position.y + 30};
    drag.is_down = false;
    drag.modifiers = kModCmd;
    REQUIRE(overlay.handle_mouse_event(drag));

    // Converted to absolute.
    REQUIRE(child_ptr->position() == View::Position::absolute);

    // Three tweaks present, in one batch.
    REQUIRE(store.count() == 3);
    auto pos = store.lookup("anchor-move", "layout.position");
    auto left = store.lookup("anchor-move", "layout.left");
    auto top = store.lookup("anchor-move", "layout.top");
    REQUIRE(pos.has_value());
    REQUIRE(left.has_value());
    REQUIRE(top.has_value());
    REQUIRE(pos->getString() == "absolute");

    // left/top ≈ seeded origin + delta. Seed for a child of a row flex
    // root with no border/margin is its resolved (x,y); delta is +50/+30.
    REQUIRE(left->getFloat32() == Catch::Approx(before.x + 50.0f).margin(0.5));
    REQUIRE(top->getFloat32() == Catch::Approx(before.y + 30.0f).margin(0.5));

    // Re-layout: the moved element relocates (resolved bounds change).
    root.layout_children();
    REQUIRE(child_ptr->bounds().x == Catch::Approx(before.x + 50.0f).margin(0.5));
    REQUIRE(child_ptr->bounds().y == Catch::Approx(before.y + 30.0f).margin(0.5));
}

// No-visual-jump on conversion: the seeded left/top reproduce the pre-move
// resolved position within ~1px when the drag delta is zero.
TEST_CASE("InspectorOverlay P2: conversion seeds origin so there is no jump",
          "[inspect][overlay][p2][move][issue-wysiwyg-p2]") {
    View root;
    root.set_bounds({0, 0, 600, 400});
    root.flex().direction = FlexDirection::column;
    root.flex().padding = 0;

    auto spacer = std::make_unique<View>();
    spacer->flex().preferred_height = 25;
    root.add_child(std::move(spacer));

    auto child = std::make_unique<View>();
    child->set_anchor_id("anchor-seed");
    child->flex().preferred_width = 60;
    child->flex().preferred_height = 30;
    auto* child_ptr = child.get();
    root.add_child(std::move(child));
    root.layout_children();

    const Rect before = child_ptr->bounds();  // y should be ~25 (after spacer)

    TweakStore store;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_dragging_enabled(true);

    MouseEvent click;
    click.position = {before.x + 5, before.y + 5};
    click.is_down = true;
    overlay.handle_mouse_event(click);
    REQUIRE(overlay.selected_view() == child_ptr);

    // Start a ⌘-move (absolute float) and immediately "drag" by zero (move
    // event at the press position) — this exercises the seed without any
    // delta. P2c: absolute float is the ⌘-drag path.
    MouseEvent press;
    press.position = {before.x + 5, before.y + 5};
    press.is_down = true;
    press.modifiers = kModCmd;
    REQUIRE(overlay.handle_mouse_event(press));
    MouseEvent zero_drag = press;
    zero_drag.is_down = false;
    zero_drag.modifiers = kModCmd;
    REQUIRE(overlay.handle_mouse_event(zero_drag));

    root.layout_children();
    // Within 1px of the pre-move resolved position — no jump.
    REQUIRE(child_ptr->bounds().x == Catch::Approx(before.x).margin(1.0));
    REQUIRE(child_ptr->bounds().y == Catch::Approx(before.y).margin(1.0));
}

// Sibling reflow: after the moved child leaves flow (absolute), an in-flow
// sibling shifts to fill the freed space.
TEST_CASE("InspectorOverlay P2: moving a child reflows its in-flow sibling",
          "[inspect][overlay][p2][move][issue-wysiwyg-p2]") {
    View root;
    root.set_bounds({0, 0, 600, 200});
    root.flex().direction = FlexDirection::row;

    auto a = std::make_unique<View>();
    a->set_anchor_id("anchor-a");
    a->flex().preferred_width = 100;
    a->flex().preferred_height = 50;
    auto* a_ptr = a.get();
    root.add_child(std::move(a));

    auto b = std::make_unique<View>();
    b->flex().preferred_width = 100;
    b->flex().preferred_height = 50;
    auto* b_ptr = b.get();
    root.add_child(std::move(b));
    root.layout_children();

    const float b_x_before = b_ptr->bounds().x;  // ~100 (after a)

    TweakStore store;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_dragging_enabled(true);
    overlay.set_selected_view(a_ptr);

    // P2c: absolute float (the path that pulls a OUT of flow) is the
    // ⌘-drag escape hatch — hold Cmd so the sibling reflows.
    MouseEvent press;
    press.position = {a_ptr->bounds().x + 10, a_ptr->bounds().y + 10};
    press.is_down = true;
    press.modifiers = kModCmd;
    REQUIRE(overlay.handle_mouse_event(press));
    MouseEvent drag;
    drag.position = {press.position.x + 200, press.position.y + 60};
    drag.is_down = false;
    drag.modifiers = kModCmd;
    REQUIRE(overlay.handle_mouse_event(drag));

    root.layout_children();
    // a is now absolute (out of flow); b slides to the row's start.
    REQUIRE(a_ptr->position() == View::Position::absolute);
    REQUIRE(b_ptr->bounds().x < b_x_before);
    REQUIRE(b_ptr->bounds().x == Catch::Approx(0.0).margin(0.5));
}

// Grid guard: a move on a grid child is refused (no tweaks, no conversion).
TEST_CASE("InspectorOverlay P2: move is refused for a grid child",
          "[inspect][overlay][p2][move][grid-guard][issue-wysiwyg-p2]") {
    View root;
    root.set_bounds({0, 0, 600, 400});

    auto grid = std::make_unique<View>();
    grid->set_layout_mode(LayoutMode::grid);
    grid->set_bounds({0, 0, 600, 400});

    auto cell = std::make_unique<View>();
    cell->set_anchor_id("anchor-grid-child");
    cell->set_bounds({20, 20, 80, 40});
    auto* cell_ptr = cell.get();
    grid->add_child(std::move(cell));
    root.add_child(std::move(grid));

    TweakStore store;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_dragging_enabled(true);
    overlay.set_selected_view(cell_ptr);

    // ⌘-body-press on the grid child: absolute-float refused (grid children
    // ignore position/top/left). P2c: only the float path is grid-guarded;
    // plain reflow drag-out is allowed, so the refusal test holds Cmd.
    MouseEvent press;
    press.position = {40, 40};
    press.is_down = true;
    press.modifiers = kModCmd;
    REQUIRE(overlay.handle_mouse_event(press));  // consumed (so not a select)

    // A subsequent move event must NOT reposition or emit tweaks.
    MouseEvent drag;
    drag.position = {200, 200};
    drag.is_down = false;
    drag.modifiers = kModCmd;
    overlay.handle_mouse_event(drag);

    REQUIRE(cell_ptr->position() != View::Position::absolute);
    REQUIRE(store.count() == 0);
}

// Resize still works alongside move (no regression): a handle press starts
// a resize, not a move, and emits width/height tweaks.
TEST_CASE("InspectorOverlay P2: resize handle still wins over body-move "
          "(no regression)",
          "[inspect][overlay][p2][move][regression][issue-wysiwyg-p2]") {
    View root;
    root.set_bounds({0, 0, 600, 400});
    auto child = std::make_unique<View>();
    child->set_anchor_id("anchor-resize");
    child->set_bounds({10, 10, 80, 40});
    auto* child_ptr = child.get();
    root.add_child(std::move(child));

    TweakStore store;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_dragging_enabled(true);
    overlay.set_selected_view(child_ptr);

    // Press exactly on the SE handle (90,50).
    MouseEvent press; press.position = {90, 50}; press.is_down = true;
    REQUIRE(overlay.handle_mouse_event(press));
    MouseEvent drag; drag.position = {110, 65}; drag.is_down = false;
    REQUIRE(overlay.handle_mouse_event(drag));

    // Resized, NOT moved.
    REQUIRE(child_ptr->position() != View::Position::absolute);
    REQUIRE(child_ptr->bounds().width == 100.0f);
    REQUIRE(store.lookup("anchor-resize", "layout.width").has_value());
    REQUIRE_FALSE(store.lookup("anchor-resize", "layout.left").has_value());
}

// Nested element is movable (not just containers).
TEST_CASE("InspectorOverlay P2: a nested element can be moved",
          "[inspect][overlay][p2][move][issue-wysiwyg-p2]") {
    View root;
    root.set_bounds({0, 0, 400, 300});

    auto container = std::make_unique<View>();
    container->set_bounds({20, 20, 200, 120});
    auto nested = std::make_unique<View>();
    nested->set_anchor_id("anchor-nested-move");
    nested->set_bounds({10, 10, 80, 24});
    auto* nested_ptr = nested.get();
    container->add_child(std::move(nested));
    root.add_child(std::move(container));

    TweakStore store;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_dragging_enabled(true);

    // Click resolves to the nested element.
    MouseEvent click; click.position = {40, 35}; click.is_down = true;
    overlay.handle_mouse_event(click);
    REQUIRE(overlay.selected_view() == nested_ptr);

    // P2c: absolute float via ⌘-drag.
    MouseEvent press; press.position = {45, 38}; press.is_down = true;
    press.modifiers = kModCmd;
    REQUIRE(overlay.handle_mouse_event(press));
    MouseEvent drag; drag.position = {65, 58}; drag.is_down = false;
    drag.modifiers = kModCmd;
    REQUIRE(overlay.handle_mouse_event(drag));

    REQUIRE(nested_ptr->position() == View::Position::absolute);
    REQUIRE(store.lookup("anchor-nested-move", "layout.left").has_value());
    REQUIRE(store.lookup("anchor-nested-move", "layout.top").has_value());
}

// Hit-test under scale: a drag of N SCREEN px at non-1.0 design-viewport
// scale produces ~N/scale LOGICAL px of movement. The overlay paints + does
// gesture math in logical (post-inverse) space, so the host inverse-maps
// before dispatch. This test reproduces that wiring with
// compute_design_viewport_transform and asserts the logical delta.
TEST_CASE("InspectorOverlay P2: drag delta is in logical space under viewport scale",
          "[inspect][overlay][p2][move][scale][issue-wysiwyg-p2]") {
    // Design surface 300×200 shown in a 600×400 window → scale 2.0.
    const float design_w = 300, design_h = 200;
    const float window_w = 600, window_h = 400;
    float sx, sy, tx, ty;
    REQUIRE(pulp::view::WindowHost::compute_design_viewport_transform(
        window_w, window_h, design_w, design_h, sx, sy, tx, ty));
    REQUIRE(sx == Catch::Approx(2.0));

    auto to_logical = [&](float px, float py) {
        return Point{(px - tx) / sx, (py - ty) / sy};
    };

    View root;
    root.set_bounds({0, 0, design_w, design_h});  // root thinks it's design-size
    root.flex().direction = FlexDirection::row;
    auto child = std::make_unique<View>();
    child->set_anchor_id("anchor-scale");
    child->flex().preferred_width = 60;
    child->flex().preferred_height = 30;
    auto* child_ptr = child.get();
    root.add_child(std::move(child));
    root.layout_children();
    const Rect before = child_ptr->bounds();

    TweakStore store;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_dragging_enabled(true);
    overlay.set_selected_view(child_ptr);

    // Press at a screen point inside the child (inverse-mapped to logical).
    Point press_screen{(before.x + 10) * sx + tx, (before.y + 10) * sy + ty};
    MouseEvent press;
    press.position = to_logical(press_screen.x, press_screen.y);
    press.is_down = true;
    press.modifiers = kModCmd;  // P2c: absolute float via ⌘-drag
    REQUIRE(overlay.handle_mouse_event(press));

    // Drag +100 SCREEN px in x, +60 SCREEN px in y → at scale 2.0 that is
    // +50 / +30 LOGICAL px.
    Point drag_screen{press_screen.x + 100, press_screen.y + 60};
    MouseEvent drag;
    drag.position = to_logical(drag_screen.x, drag_screen.y);
    drag.is_down = false;
    drag.modifiers = kModCmd;
    REQUIRE(overlay.handle_mouse_event(drag));

    auto left = store.lookup("anchor-scale", "layout.left");
    auto top = store.lookup("anchor-scale", "layout.top");
    REQUIRE(left.has_value());
    REQUIRE(top.has_value());
    // Logical movement = screen / scale = 100/2 and 60/2.
    REQUIRE(left->getFloat32() == Catch::Approx(before.x + 50.0f).margin(0.5));
    REQUIRE(top->getFloat32() == Catch::Approx(before.y + 30.0f).margin(0.5));
}

// Two-IR-worlds shim: a move tweak (layout.* namespace) is consumable by the
// C++/native apply path via apply_move_tweak_to_view.
TEST_CASE("InspectorOverlay P2: move tweak round-trips on the C++ apply path",
          "[inspect][overlay][p2][move][round-trip][issue-wysiwyg-p2]") {
    View v;
    // Apply the three move-tweak property paths the gesture emits.
    REQUIRE(apply_move_tweak_to_view(v, "layout.position",
                                     choc::value::createString("absolute")));
    REQUIRE(apply_move_tweak_to_view(v, "layout.left",
                                     choc::value::createFloat32(42.0f)));
    REQUIRE(apply_move_tweak_to_view(v, "layout.top",
                                     choc::value::createFloat32(99.0f)));

    REQUIRE(v.position() == View::Position::absolute);
    REQUIRE(v.has_left());
    REQUIRE(v.left() == 42.0f);
    REQUIRE(v.has_top());
    REQUIRE(v.top() == 99.0f);

    // Bare-leaf paths and the style.* namespace also resolve.
    View v2;
    REQUIRE(apply_move_tweak_to_view(v2, "left",
                                     choc::value::createFloat32(7.0f)));
    REQUIRE(v2.left() == 7.0f);
    REQUIRE(apply_move_tweak_to_view(v2, "style.top",
                                     choc::value::createFloat32(8.0f)));
    REQUIRE(v2.top() == 8.0f);

    // Unknown paths are rejected without mutating the view.
    REQUIRE_FALSE(apply_move_tweak_to_view(v2, "paint.color",
                                           choc::value::createString("#fff")));
}

// WYSIWYG P6 FIX 2 — the selection badge (type W×H tooltip and the grid-
// refuse "can't move" badge) flips BELOW the selection when there isn't
// room above (so it doesn't clip under the window title bar), and clamps
// its x to the window edges. compute_badge_placement is the pure helper
// both badges share.
TEST_CASE("compute_badge_placement flips below near the window top",
          "[inspect][overlay][badge][issue-wysiwyg-p6]") {
    constexpr float kBadgeW = 80.0f;
    constexpr float kBadgeH = 16.0f;
    constexpr float kRootW  = 400.0f;
    constexpr float kGap    = 2.0f;
    constexpr float kTopMargin = kBadgeH;

    SECTION("plenty of room above → badge sits above") {
        auto bp = compute_badge_placement(/*sel_x=*/100, /*sel_y=*/200,
                                          /*sel_h=*/50, kBadgeW, kBadgeH,
                                          kRootW, kGap, kTopMargin);
        REQUIRE_FALSE(bp.below);
        REQUIRE(bp.y == Catch::Approx(200.0f - kGap - kBadgeH));
        REQUIRE(bp.x == Catch::Approx(100.0f));
    }

    SECTION("selection at the very top → badge flips below") {
        auto bp = compute_badge_placement(/*sel_x=*/100, /*sel_y=*/2,
                                          /*sel_h=*/40, kBadgeW, kBadgeH,
                                          kRootW, kGap, kTopMargin);
        REQUIRE(bp.below);
        REQUIRE(bp.y == Catch::Approx(2.0f + 40.0f + kGap));
    }

    SECTION("x clamps off the left edge") {
        auto bp = compute_badge_placement(/*sel_x=*/-20, /*sel_y=*/200,
                                          /*sel_h=*/50, kBadgeW, kBadgeH,
                                          kRootW, kGap, kTopMargin);
        REQUIRE(bp.x == Catch::Approx(0.0f));
    }

    SECTION("x clamps off the right edge") {
        auto bp = compute_badge_placement(/*sel_x=*/380, /*sel_y=*/200,
                                          /*sel_h=*/50, kBadgeW, kBadgeH,
                                          kRootW, kGap, kTopMargin);
        REQUIRE(bp.x == Catch::Approx(kRootW - kBadgeW));
    }

    SECTION("badge wider than root pins to left") {
        auto bp = compute_badge_placement(/*sel_x=*/10, /*sel_y=*/200,
                                          /*sel_h=*/50, /*badge_w=*/500,
                                          kBadgeH, kRootW, kGap, kTopMargin);
        REQUIRE(bp.x == Catch::Approx(0.0f));
    }
}

// Re-import round-trip: a moved element's tweaks survive an apply pass — the
// TweakStore values reconstruct an absolute view at the moved left/top.
TEST_CASE("InspectorOverlay P2: move tweaks reconstruct absolute view on re-apply",
          "[inspect][overlay][p2][move][round-trip][issue-wysiwyg-p2]") {
    // Simulate: gesture wrote tweaks; a fresh import re-creates the view and
    // re-applies the stored tweaks by anchor.
    TweakStore store;
    std::vector<TweakStore::BatchEntry> batch;
    batch.push_back({"layout.position", choc::value::createString("absolute")});
    batch.push_back({"layout.left", choc::value::createFloat32(120.0f)});
    batch.push_back({"layout.top", choc::value::createFloat32(75.0f)});
    store.apply_tweaks_batch("anchor-reimport", std::move(batch),
                             "inspector-drag-move");

    // Fresh view (as if re-lowered from a re-import) with the same anchor.
    View fresh;
    fresh.set_anchor_id("anchor-reimport");
    REQUIRE(fresh.position() != View::Position::absolute);  // pre-apply

    // Apply each stored tweak through the C++ apply path.
    for (const auto& rec : store.list_tweaks()) {
        if (rec.anchor_id != "anchor-reimport") continue;
        REQUIRE(apply_move_tweak_to_view(fresh, rec.property_path, rec.value));
    }

    REQUIRE(fresh.position() == View::Position::absolute);
    REQUIRE(fresh.left() == 120.0f);
    REQUIRE(fresh.top() == 75.0f);
}

// ── P2a: undo safety net ────────────────────────────────────────────────────
//
// planning/2026-05-21-wysiwyg-direct-manipulation-extension.md § R2.2.
// Every committed manipulation gesture becomes ONE undoable EditHistory
// entry whose undo restores BOTH the live View layout inputs AND the
// TweakStore. These drive the real handle_mouse_event() gesture path
// (press → move → release) with an EditHistory attached, then assert
// undo/redo behavior.

TEST_CASE("InspectorOverlay P2a: resize gesture is one undoable unit",
          "[inspect][overlay][p2a][undo][issue-wysiwyg-p2]") {
    View root;
    root.set_bounds({0, 0, 600, 400});
    auto child = std::make_unique<View>();
    child->set_anchor_id("figma:0:42");
    child->set_bounds({10, 10, 80, 40});
    child->flex().preferred_width = 80;
    child->flex().preferred_height = 40;
    auto* child_ptr = child.get();
    root.add_child(std::move(child));

    TweakStore store;
    pulp::state::EditHistory history;
    history.set_coalesce(false);
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_edit_history(&history);
    overlay.set_dragging_enabled(true);

    // Select.
    MouseEvent click;
    click.position = {30, 30};
    click.is_down = true;
    overlay.handle_mouse_event(click);
    REQUIRE(overlay.selected_view() == child_ptr);

    // Pre-gesture: no width/height tweaks, EditHistory empty.
    REQUIRE_FALSE(store.lookup("figma:0:42", "layout.width").has_value());
    REQUIRE_FALSE(history.can_undo());

    // Press SE handle (corner at 90,50), drag +20/+15, release.
    MouseEvent press;
    press.position = {90, 50};
    press.is_down = true;
    REQUIRE(overlay.handle_mouse_event(press));

    MouseEvent drag;
    drag.position = {110, 65};
    drag.is_down = false;
    REQUIRE(overlay.handle_mouse_event(drag));

    MouseEvent release;  // is_down=true ends the gesture (acts as release)
    release.position = {300, 300};
    release.is_down = true;
    overlay.handle_mouse_event(release);

    // After commit: the resize landed live + as tweaks, and exactly ONE
    // undo entry exists.
    REQUIRE(child_ptr->flex().preferred_width == 100.0f);
    REQUIRE(child_ptr->flex().preferred_height == 55.0f);
    REQUIRE(store.lookup("figma:0:42", "layout.width")->getFloat32() == 100.0f);
    REQUIRE(store.lookup("figma:0:42", "layout.height")->getFloat32() == 55.0f);
    REQUIRE(history.undo_count() == 1);
    REQUIRE(history.undo_description() == "resize");

    // Undo: View inputs restored AND the tweaks reverted (removed, since
    // none existed before the gesture).
    REQUIRE(history.undo());
    REQUIRE(child_ptr->flex().preferred_width == 80.0f);
    REQUIRE(child_ptr->flex().preferred_height == 40.0f);
    REQUIRE_FALSE(store.lookup("figma:0:42", "layout.width").has_value());
    REQUIRE_FALSE(store.lookup("figma:0:42", "layout.height").has_value());
    REQUIRE(child_ptr->bounds().width == 80.0f);
    REQUIRE(child_ptr->bounds().height == 40.0f);

    // Redo: re-applies the resize (View inputs + tweaks back).
    REQUIRE(history.redo());
    REQUIRE(child_ptr->flex().preferred_width == 100.0f);
    REQUIRE(child_ptr->flex().preferred_height == 55.0f);
    REQUIRE(store.lookup("figma:0:42", "layout.width")->getFloat32() == 100.0f);
    REQUIRE(store.lookup("figma:0:42", "layout.height")->getFloat32() == 55.0f);
}

TEST_CASE("InspectorOverlay P2a: resize undo restores a PRIOR tweak value",
          "[inspect][overlay][p2a][undo][issue-wysiwyg-p2]") {
    // When a width tweak already exists, undo must RESTORE it (not remove
    // it) — proving the prior-value capture path, not just the remove path.
    View root;
    root.set_bounds({0, 0, 600, 400});
    auto child = std::make_unique<View>();
    child->set_anchor_id("a");
    child->set_bounds({10, 10, 80, 40});
    child->flex().preferred_width = 80;
    child->flex().preferred_height = 40;
    root.add_child(std::move(child));

    TweakStore store;
    store.apply_tweak("a", "layout.width", choc::value::createFloat32(80.0f),
                      "seed");
    store.apply_tweak("a", "layout.height", choc::value::createFloat32(40.0f),
                      "seed");

    pulp::state::EditHistory history;
    history.set_coalesce(false);
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_edit_history(&history);
    overlay.set_dragging_enabled(true);

    MouseEvent click;
    click.position = {30, 30};
    click.is_down = true;
    overlay.handle_mouse_event(click);

    MouseEvent press;
    press.position = {90, 50};
    press.is_down = true;
    overlay.handle_mouse_event(press);
    MouseEvent drag;
    drag.position = {110, 65};
    drag.is_down = false;
    overlay.handle_mouse_event(drag);
    MouseEvent release;
    release.position = {300, 300};
    release.is_down = true;
    overlay.handle_mouse_event(release);

    REQUIRE(store.lookup("a", "layout.width")->getFloat32() == 100.0f);

    // Undo: the prior tweak value (80) is restored, NOT removed.
    REQUIRE(history.undo());
    auto w = store.lookup("a", "layout.width");
    auto h = store.lookup("a", "layout.height");
    REQUIRE(w.has_value());
    REQUIRE(h.has_value());
    REQUIRE(w->getFloat32() == 80.0f);
    REQUIRE(h->getFloat32() == 40.0f);
}

TEST_CASE("InspectorOverlay P2a: move gesture undo reverts all 3 tweaks atomically",
          "[inspect][overlay][p2a][undo][move][issue-wysiwyg-p2]") {
    View root;
    root.set_bounds({0, 0, 600, 400});
    root.flex().direction = FlexDirection::row;

    auto child = std::make_unique<View>();
    child->set_anchor_id("anchor-move");
    child->flex().preferred_width = 80;
    child->flex().preferred_height = 40;
    auto* child_ptr = child.get();
    root.add_child(std::move(child));
    root.layout_children();

    const Rect before = child_ptr->bounds();
    const auto before_pos = child_ptr->position();

    TweakStore store;
    pulp::state::EditHistory history;
    history.set_coalesce(false);
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_edit_history(&history);
    overlay.set_dragging_enabled(true);

    MouseEvent click;
    click.position = {before.x + 10, before.y + 10};
    click.is_down = true;
    overlay.handle_mouse_event(click);
    REQUIRE(overlay.selected_view() == child_ptr);

    // P2c: absolute float (3 tweaks) is the ⌘-drag path.
    MouseEvent press;
    press.position = {before.x + 20, before.y + 20};
    press.is_down = true;
    press.modifiers = kModCmd;
    overlay.handle_mouse_event(press);

    MouseEvent drag;
    drag.position = {press.position.x + 50, press.position.y + 30};
    drag.is_down = false;
    drag.modifiers = kModCmd;
    overlay.handle_mouse_event(drag);

    MouseEvent release;
    release.position = {500, 350};
    release.is_down = true;
    release.modifiers = kModCmd;
    overlay.handle_mouse_event(release);

    // After commit: converted to absolute, 3 tweaks, one undo entry.
    REQUIRE(child_ptr->position() == View::Position::absolute);
    REQUIRE(store.count() == 3);
    REQUIRE(history.undo_count() == 1);
    REQUIRE(history.undo_description() == "move-float");

    // ONE undo reverts ALL THREE move tweaks atomically AND restores the
    // pre-move View position.
    REQUIRE(history.undo());
    REQUIRE(child_ptr->position() == before_pos);
    REQUIRE_FALSE(store.lookup("anchor-move", "layout.position").has_value());
    REQUIRE_FALSE(store.lookup("anchor-move", "layout.left").has_value());
    REQUIRE_FALSE(store.lookup("anchor-move", "layout.top").has_value());
    REQUIRE(store.count() == 0);

    // Re-layout: the element returns to its in-flow position.
    root.layout_children();
    REQUIRE(child_ptr->bounds().x == Catch::Approx(before.x).margin(0.5));
    REQUIRE(child_ptr->bounds().y == Catch::Approx(before.y).margin(0.5));

    // Redo: the move comes back atomically.
    REQUIRE(history.redo());
    REQUIRE(child_ptr->position() == View::Position::absolute);
    REQUIRE(store.count() == 3);
    REQUIRE(store.lookup("anchor-move", "layout.left").has_value());
}

TEST_CASE("InspectorOverlay P2a: gestures behave normally when no EditHistory",
          "[inspect][overlay][p2a][undo][issue-wysiwyg-p2]") {
    // Guard: without an EditHistory wired, the resize gesture still applies
    // live + emits tweaks exactly as before (the safety net is additive).
    View root;
    root.set_bounds({0, 0, 600, 400});
    auto child = std::make_unique<View>();
    child->set_anchor_id("a");
    child->set_bounds({10, 10, 80, 40});
    child->flex().preferred_width = 80;
    child->flex().preferred_height = 40;
    auto* child_ptr = child.get();
    root.add_child(std::move(child));

    TweakStore store;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_dragging_enabled(true);
    // No set_edit_history() — edit_history_ stays null.

    MouseEvent click;
    click.position = {30, 30};
    click.is_down = true;
    overlay.handle_mouse_event(click);
    MouseEvent press;
    press.position = {90, 50};
    press.is_down = true;
    overlay.handle_mouse_event(press);
    MouseEvent drag;
    drag.position = {110, 65};
    drag.is_down = false;
    overlay.handle_mouse_event(drag);
    MouseEvent release;
    release.position = {300, 300};
    release.is_down = true;
    overlay.handle_mouse_event(release);

    REQUIRE(child_ptr->flex().preferred_width == 100.0f);
    REQUIRE(store.lookup("a", "layout.width")->getFloat32() == 100.0f);
}

TEST_CASE("InspectorOverlay P2a: Cmd+Z / Cmd+Shift+Z drive undo and redo",
          "[inspect][overlay][p2a][undo][issue-wysiwyg-p2]") {
    View root;
    root.set_bounds({0, 0, 600, 400});
    auto child = std::make_unique<View>();
    child->set_anchor_id("a");
    child->set_bounds({10, 10, 80, 40});
    child->flex().preferred_width = 80;
    child->flex().preferred_height = 40;
    auto* child_ptr = child.get();
    root.add_child(std::move(child));

    TweakStore store;
    pulp::state::EditHistory history;
    history.set_coalesce(false);
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_edit_history(&history);
    overlay.set_dragging_enabled(true);

    // Drive a resize gesture so there is something to undo.
    MouseEvent click;
    click.position = {30, 30};
    click.is_down = true;
    overlay.handle_mouse_event(click);
    MouseEvent press;
    press.position = {90, 50};
    press.is_down = true;
    overlay.handle_mouse_event(press);
    MouseEvent drag;
    drag.position = {110, 65};
    drag.is_down = false;
    overlay.handle_mouse_event(drag);
    MouseEvent release;
    release.position = {300, 300};
    release.is_down = true;
    overlay.handle_mouse_event(release);
    REQUIRE(child_ptr->flex().preferred_width == 100.0f);

    // Cmd+Z (primary modifier) undoes.
    KeyEvent undo_key;
    undo_key.key = KeyCode::z;
    undo_key.is_down = true;
    undo_key.modifiers = pulp::view::is_main_modifier(kModCmd)
                             ? kModCmd
                             : kModCtrl;
    REQUIRE(overlay.handle_key_event(undo_key));
    REQUIRE(child_ptr->flex().preferred_width == 80.0f);

    // Cmd+Shift+Z redoes.
    KeyEvent redo_key;
    redo_key.key = KeyCode::z;
    redo_key.is_down = true;
    redo_key.modifiers = (pulp::view::is_main_modifier(kModCmd)
                              ? kModCmd
                              : kModCtrl) |
                         kModShift;
    REQUIRE(overlay.handle_key_event(redo_key));
    REQUIRE(child_ptr->flex().preferred_width == 100.0f);
}

TEST_CASE("InspectorOverlay P2a: tweak-panel delete is undoable",
          "[inspect][overlay][p2a][undo][delete][issue-wysiwyg-p2]") {
    // Drive the REAL panel delete-icon click path through
    // handle_mouse_event() (the TweakAction::remove branch that builds the
    // EditHistory entry), then undo to restore the deleted tweak. The icon
    // hit-rects are private, so we sweep the known delete-icon column the
    // same way the Phase 2.5 panel tests do.
    View root;
    root.set_bounds({0, 0, 600, 600});
    TweakStore store;
    store.apply_tweak("figma:0:a", "layout.padding",
                      choc::value::createInt32(12), "seed");

    pulp::state::EditHistory history;
    history.set_coalesce(false);
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_edit_history(&history);
    overlay.toggle_tweaks_panel();
    REQUIRE(overlay.tweaks_panel_visible());
    REQUIRE(store.count() == 1);

    // Sweep the delete-icon (index 2) column to land the real click.
    constexpr float kIconSize = 14.0f, kIconGap = 4.0f;
    const float x = (600.0f - 300.0f) + 8.0f;
    const float w = 300.0f - 16.0f;
    const float icons_w = 3.0f * kIconSize + 2.0f * kIconGap;
    const float icons_x = x + w - icons_w;
    const float icon_cx = icons_x + 2 * (kIconSize + kIconGap) + kIconSize / 2.0f;

    bool deleted = false;
    for (float y = 600.0f * 0.55f; y < 600.0f - 24.0f && !deleted; y += 2.0f) {
        pulp::canvas::RecordingCanvas c;
        overlay.paint(c);  // refresh row hit-rects
        MouseEvent click;
        click.position = {icon_cx, y};
        click.is_down = true;
        overlay.handle_mouse_event(click);
        deleted = (store.count() == 0);
    }
    REQUIRE(deleted);

    // The committed delete pushed ONE undoable entry.
    REQUIRE(history.undo_count() == 1);
    REQUIRE(history.undo_description() == "delete-tweak");

    // Undo: the deleted tweak is restored with its original value.
    REQUIRE(history.undo());
    auto restored = store.lookup("figma:0:a", "layout.padding");
    REQUIRE(restored.has_value());
    REQUIRE(restored->getInt32() == 12);
}

// ════════════════════════════════════════════════════════════════════════════
// P2c — WYSIWYG "Figma feel" pivot
//
// planning/2026-05-21-wysiwyg-direct-manipulation-extension.md § Refinement 2.
//   - Reflow-aware MOVE is the DEFAULT body-drag (reorder among siblings /
//     reparent into a container); ⌘-drag is the absolute-float escape hatch
//     (covered by the P2 tests above, now Cmd-gated).
//   - Proportional RESIZE via Shift + handle-drag (scales content).
//   - Selection comes ONLY from the canvas; the floating window reflects.
// ════════════════════════════════════════════════════════════════════════════

// Reflow reorder: a plain (no-modifier) body-drag of a flex child past a
// sibling's midpoint rewrites flex().order so the dragged child re-sequences,
// WITHOUT converting to absolute. Asserts the resolved child order changed and
// the gesture is undoable.
TEST_CASE("InspectorOverlay P2c: reflow drag reorders flex siblings via order",
          "[inspect][overlay][p2c][reflow][move][issue-wysiwyg-p2c]") {
    View root;
    root.set_bounds({0, 0, 600, 200});
    root.flex().direction = FlexDirection::row;

    auto a = std::make_unique<View>();
    a->set_anchor_id("anchor-a");
    a->flex().preferred_width = 100;
    a->flex().preferred_height = 50;
    auto* a_ptr = a.get();
    root.add_child(std::move(a));

    auto b = std::make_unique<View>();
    b->set_anchor_id("anchor-b");
    b->flex().preferred_width = 100;
    b->flex().preferred_height = 50;
    auto* b_ptr = b.get();
    root.add_child(std::move(b));

    auto c = std::make_unique<View>();
    c->set_anchor_id("anchor-c");
    c->flex().preferred_width = 100;
    c->flex().preferred_height = 50;
    auto* c_ptr = c.get();
    root.add_child(std::move(c));
    root.layout_children();

    // Initial visual x-order: a < b < c.
    REQUIRE(a_ptr->bounds().x < b_ptr->bounds().x);
    REQUIRE(b_ptr->bounds().x < c_ptr->bounds().x);

    TweakStore store;
    pulp::state::EditHistory history;
    history.set_coalesce(false);
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_edit_history(&history);
    overlay.set_dragging_enabled(true);
    overlay.set_selected_view(a_ptr);

    const int a_order_before = a_ptr->flex().order;

    // PLAIN body-press on a (no Cmd) → reflow-aware move.
    MouseEvent press;
    press.position = {a_ptr->bounds().x + 10, a_ptr->bounds().y + 10};
    press.is_down = true;
    REQUIRE(overlay.handle_mouse_event(press));

    // Drag the cursor past c's midpoint (far right) so a should drop last.
    MouseEvent drag;
    drag.position = {c_ptr->bounds().x + c_ptr->bounds().width - 5,
                     c_ptr->bounds().y + 10};
    drag.is_down = false;
    REQUIRE(overlay.handle_mouse_event(drag));

    // a stays in flow (NOT converted to absolute) — reflow, not float.
    REQUIRE(a_ptr->position() != View::Position::absolute);

    // Release commits the reorder.
    MouseEvent release;
    release.position = drag.position;
    release.is_down = true;
    overlay.handle_mouse_event(release);

    root.layout_children();
    // a's order changed AND it now sorts to the end (largest order).
    REQUIRE(a_ptr->flex().order != a_order_before);
    REQUIRE(a_ptr->flex().order > b_ptr->flex().order);
    REQUIRE(a_ptr->flex().order > c_ptr->flex().order);
    // Visually a is now last in the row.
    REQUIRE(a_ptr->bounds().x > b_ptr->bounds().x);
    REQUIRE(a_ptr->bounds().x > c_ptr->bounds().x);

    // One undoable unit; undo restores the original order.
    REQUIRE(history.undo_count() == 1);
    REQUIRE(history.undo_description() == "move-reflow");
    REQUIRE(history.undo());
    REQUIRE(a_ptr->flex().order == a_order_before);
    root.layout_children();
    REQUIRE(a_ptr->bounds().x < b_ptr->bounds().x);
}

// WYSIWYG sweep P1 — a same-parent reflow REORDER must PERSIST the new order as
// a layout.order tweak (it was previously live-only; the "persisted elsewhere"
// comment was wrong). The moved child AND any normalized sibling whose order
// changed get a tweak keyed by their OWN anchor, and the value round-trips
// through lock_tweak_into_source as `el.style.order`.
TEST_CASE("InspectorOverlay sweep P1: same-parent reorder emits a layout.order "
          "tweak that round-trips",
          "[inspect][overlay][reflow][reorder][issue-wysiwyg-reflow-slot]") {
    View root;
    root.set_bounds({0, 0, 600, 200});
    root.flex().direction = FlexDirection::row;

    auto a = std::make_unique<View>();
    a->set_anchor_id("anchor-a");
    a->flex().preferred_width = 100; a->flex().preferred_height = 50;
    auto* a_ptr = a.get();
    root.add_child(std::move(a));
    auto b = std::make_unique<View>();
    b->set_anchor_id("anchor-b");
    b->flex().preferred_width = 100; b->flex().preferred_height = 50;
    auto* b_ptr = b.get();
    root.add_child(std::move(b));
    auto c = std::make_unique<View>();
    c->set_anchor_id("anchor-c");
    c->flex().preferred_width = 100; c->flex().preferred_height = 50;
    auto* c_ptr = c.get();
    root.add_child(std::move(c));
    root.layout_children();

    TweakStore store;
    pulp::state::EditHistory history;
    history.set_coalesce(false);
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_edit_history(&history);
    overlay.set_dragging_enabled(true);
    overlay.set_selected_view(a_ptr);

    // Drag a past c → a re-sequences to last (same parent).
    MouseEvent press;
    press.position = {a_ptr->bounds().x + 10, a_ptr->bounds().y + 10};
    press.is_down = true;
    REQUIRE(overlay.handle_mouse_event(press));
    MouseEvent drag;
    drag.position = {c_ptr->bounds().x + c_ptr->bounds().width - 5,
                     c_ptr->bounds().y + 10};
    drag.is_down = false;
    REQUIRE(overlay.handle_mouse_event(drag));
    MouseEvent release;
    release.position = drag.position;
    release.is_down = true;
    overlay.handle_mouse_event(release);
    root.layout_children();

    // a moved to the end → its order is now the largest. A layout.order tweak
    // was persisted for a (keyed by a's own anchor).
    auto a_tw = store.lookup("anchor-a", "layout.order");
    REQUIRE(a_tw.has_value());
    const int a_order = static_cast<int>(a_tw->getInt64());
    REQUIRE(a_order == a_ptr->flex().order);
    REQUIRE(a_order > b_ptr->flex().order);
    REQUIRE(a_order > c_ptr->flex().order);

    // The persisted order round-trips through the lock engine as el.style.order.
    // Build a generated source whose anchor block matches a, then lock.
    const std::string gen =
        "// Generated by Pulp import-design from v0\n"
        "// @pulp-anchor anchor-a\n"
        "const a0 = document.createElement('div');\n"
        "a0.style.width = '100px';\n"
        "root.appendChild(a0);\n"
        "setAnchor(a0._id, 'anchor-a');\n";
    pulp::view::LockToSourceTweak lt{"anchor-a", "layout.order",
                                     std::to_string(a_order)};
    auto lr = pulp::view::lock_tweak_into_source(gen, lt);
    REQUIRE(lr.ok());
    REQUIRE(lr.mutated());
    REQUIRE(lr.style_property == "order");
    REQUIRE(lr.source.find(".style.order = '" + std::to_string(a_order) + "'")
            != std::string::npos);
}

// WYSIWYG sweep P1 — a cross-parent reflow drop must carry the insertion SLOT
// (preceding-sibling anchor) through the ReparentSourceEdit so the source
// rewrite lands the moved block at the dragged position, not always first-child.
TEST_CASE("InspectorOverlay sweep P1: cross-parent reflow drop carries the "
          "insertion slot to the source sink",
          "[inspect][overlay][reflow][reparent][issue-wysiwyg-reflow-slot]") {
    View root;
    root.set_bounds({0, 0, 600, 300});
    root.flex().direction = FlexDirection::row;

    // left{ moving } and right{ rA } — drop moving into right AFTER rA.
    auto left = std::make_unique<View>();
    left->set_anchor_id("a-left");
    left->flex().direction = FlexDirection::column;
    left->flex().preferred_width = 300; left->flex().preferred_height = 300;

    auto moving = std::make_unique<View>();
    moving->set_anchor_id("a-moving");
    moving->flex().preferred_width = 80; moving->flex().preferred_height = 40;
    auto* moving_ptr = moving.get();
    left->add_child(std::move(moving));
    root.add_child(std::move(left));

    auto right = std::make_unique<View>();
    right->set_anchor_id("a-right");
    right->flex().direction = FlexDirection::column;
    right->flex().preferred_width = 300; right->flex().preferred_height = 300;
    auto* right_ptr = right.get();
    auto rA = std::make_unique<View>();
    rA->set_anchor_id("a-rA");
    rA->flex().preferred_width = 80; rA->flex().preferred_height = 40;
    rA->flex().order = 0;
    right->add_child(std::move(rA));
    root.add_child(std::move(right));
    root.layout_children();

    TweakStore store;
    pulp::state::EditHistory history;
    history.set_coalesce(false);
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_edit_history(&history);
    overlay.set_dragging_enabled(true);
    overlay.set_selected_view(moving_ptr);

    // Capture the edit the sink receives.
    InspectorOverlay::ReparentSourceEdit captured;
    int sink_calls = 0;
    overlay.set_reparent_source_sink(
        [&](const InspectorOverlay::ReparentSourceEdit& e) {
            captured = e;
            ++sink_calls;
        });

    // Drop moving into the right container near its BOTTOM so it lands AFTER rA.
    const Rect mb = moving_ptr->bounds();
    MouseEvent press; press.position = {mb.x + 10, mb.y + 10}; press.is_down = true;
    REQUIRE(overlay.handle_mouse_event(press));
    const Rect rb = right_ptr->bounds();
    MouseEvent drag;
    drag.position = {rb.x + rb.width * 0.5f, rb.y + rb.height - 5};  // bottom slot
    drag.is_down = false;
    REQUIRE(overlay.handle_mouse_event(drag));
    MouseEvent release; release.position = drag.position; release.is_down = true;
    overlay.handle_mouse_event(release);

    REQUIRE(moving_ptr->parent() == right_ptr);
    REQUIRE(sink_calls >= 1);
    REQUIRE(captured.child_anchor == "a-moving");
    REQUIRE(captured.new_parent_anchor == "a-right");
    // The insertion slot points at rA — moving landed AFTER it, not first-child.
    REQUIRE(captured.insert_after_anchor == "a-rA");
}

// Reflow reparent: a plain body-drag of a node whose cursor ends INSIDE a
// different container reparents the node into that container, and undo
// restores the original parent.
TEST_CASE("InspectorOverlay P2c: reflow drag reparents a node into another "
          "container, undo restores parent",
          "[inspect][overlay][p2c][reflow][reparent][move][issue-wysiwyg-p2c]") {
    View root;
    root.set_bounds({0, 0, 600, 300});
    root.flex().direction = FlexDirection::row;

    // Two side-by-side flex containers.
    auto left = std::make_unique<View>();
    left->set_anchor_id("anchor-left");
    left->flex().direction = FlexDirection::column;
    left->flex().preferred_width = 300;
    left->flex().preferred_height = 300;
    auto* left_ptr = left.get();

    auto moving = std::make_unique<View>();
    moving->set_anchor_id("anchor-moving");
    moving->flex().preferred_width = 80;
    moving->flex().preferred_height = 40;
    auto* moving_ptr = moving.get();
    left->add_child(std::move(moving));
    root.add_child(std::move(left));

    auto right = std::make_unique<View>();
    right->set_anchor_id("anchor-right");
    right->flex().direction = FlexDirection::column;
    right->flex().preferred_width = 300;
    right->flex().preferred_height = 300;
    auto* right_ptr = right.get();
    root.add_child(std::move(right));
    root.layout_children();

    REQUIRE(moving_ptr->parent() == left_ptr);

    TweakStore store;
    pulp::state::EditHistory history;
    history.set_coalesce(false);
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_edit_history(&history);
    overlay.set_dragging_enabled(true);
    overlay.set_selected_view(moving_ptr);

    // Plain body-press on the moving node.
    const Rect mb = moving_ptr->bounds();  // parent-space, but left is at x=0
    MouseEvent press;
    press.position = {mb.x + 10, mb.y + 10};
    press.is_down = true;
    REQUIRE(overlay.handle_mouse_event(press));

    // Drag the cursor into the RIGHT container's interior (it's empty, so the
    // drop resolves as drop-inside the right container).
    const Rect rb = right_ptr->bounds();
    MouseEvent drag;
    drag.position = {rb.x + rb.width * 0.5f, rb.y + rb.height * 0.5f};
    drag.is_down = false;
    REQUIRE(overlay.handle_mouse_event(drag));

    MouseEvent release;
    release.position = drag.position;
    release.is_down = true;
    overlay.handle_mouse_event(release);

    // Reparented into the right container.
    REQUIRE(moving_ptr->parent() == right_ptr);
    REQUIRE(history.undo_count() == 1);
    REQUIRE(history.undo_description() == "move-reflow");

    // Undo restores the original parent.
    REQUIRE(history.undo());
    REQUIRE(moving_ptr->parent() == left_ptr);

    // Redo reparents again.
    REQUIRE(history.redo());
    REQUIRE(moving_ptr->parent() == right_ptr);
}

// WYSIWYG T5 (gap #1) — a live reflow reparent gesture must ALSO lock the
// structural change into the generated source. The overlay emits the edit
// through a host-installed sink (it owns the source text); here the test sink
// runs the real pulp::view::reparent_in_source() engine against a held source
// buffer that mirrors the live tree's anchors. We assert the full round-trip:
//   (a) the live tree reparented,
//   (b) the source's child block moved under the new parent,
//   (c) undo reverts BOTH the live tree and the source,
//   (d) a redo re-applies, and re-running the engine is idempotent.
TEST_CASE("InspectorOverlay T5: reflow reparent locks the structural edit to "
          "source and undo reverts both",
          "[inspect][overlay][wysiwyg][t5][reparent][reflow]") {
    // Live tree: root(row) → left(col){moving} + right(col, empty).
    // Anchors match the generated source below.
    View root;
    root.set_bounds({0, 0, 600, 300});
    root.flex().direction = FlexDirection::row;

    auto left = std::make_unique<View>();
    left->set_anchor_id("a-left");
    left->flex().direction = FlexDirection::column;
    left->flex().preferred_width = 300;
    left->flex().preferred_height = 300;
    auto* left_ptr = left.get();

    auto moving = std::make_unique<View>();
    moving->set_anchor_id("a-moving");
    moving->flex().preferred_width = 80;
    moving->flex().preferred_height = 40;
    auto* moving_ptr = moving.get();
    left->add_child(std::move(moving));
    root.add_child(std::move(left));

    auto right = std::make_unique<View>();
    right->set_anchor_id("a-right");
    right->flex().direction = FlexDirection::column;
    right->flex().preferred_width = 300;
    right->flex().preferred_height = 300;
    auto* right_ptr = right.get();
    root.add_child(std::move(right));
    root.layout_children();

    REQUIRE(moving_ptr->parent() == left_ptr);

    // A generated web-compat source whose anchor blocks mirror the live tree.
    // `moving` is wired under `left`; `right` is an empty sibling container.
    const std::string initial_source =
        "// Generated by Pulp import-design from v0\n"
        "// @pulp-anchor a-root\n"
        "const root = document.createElement('div');\n"
        "root.style.display = 'flex';\n"
        "setAnchor(root._id, 'a-root');\n"
        "\n"
        "  // @pulp-anchor a-left\n"
        "  const left0 = document.createElement('div');\n"
        "  left0.style.display = 'flex';\n"
        "  root.appendChild(left0);\n"
        "  setAnchor(left0._id, 'a-left');\n"
        "\n"
        "    // @pulp-anchor a-moving\n"
        "    const moving1 = document.createElement('div');\n"
        "    moving1.style.width = '80px';\n"
        "    left0.appendChild(moving1);\n"
        "    setAnchor(moving1._id, 'a-moving');\n"
        "\n"
        "  // @pulp-anchor a-right\n"
        "  const right2 = document.createElement('div');\n"
        "  right2.style.display = 'flex';\n"
        "  root.appendChild(right2);\n"
        "  setAnchor(right2._id, 'a-right');\n"
        "\n"
        "document.body.appendChild(root);\n";

    // The host owns the source text; the sink rewrites it via the real engine.
    std::string source = initial_source;
    int sink_calls = 0;
    InspectorOverlay overlay(root);
    overlay.set_reparent_source_sink(
        [&](const InspectorOverlay::ReparentSourceEdit& e) {
            ++sink_calls;
            pulp::view::ReparentToSourceEdit engine_edit{e.child_anchor,
                                                         e.new_parent_anchor,
                                                         e.insert_after_anchor};
            auto r = pulp::view::reparent_in_source(source, engine_edit);
            source = r.source;  // mirror the host's read/confirm/write loop
        });
    REQUIRE(overlay.has_reparent_source_sink());

    TweakStore store;
    pulp::state::EditHistory history;
    history.set_coalesce(false);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_edit_history(&history);
    overlay.set_dragging_enabled(true);
    overlay.set_selected_view(moving_ptr);

    // Helper: where does an anchor block sit in `source`?
    auto pos = [&](const std::string& id) {
        return source.find("// @pulp-anchor " + id);
    };

    // Before: moving's block sits under left (precedes right's block).
    REQUIRE(pos("a-moving") < pos("a-right"));

    // ── Drive the reflow reparent gesture (mirror the P2c reparent test). ──
    const Rect mb = moving_ptr->bounds();
    MouseEvent press;
    press.position = {mb.x + 10, mb.y + 10};
    press.is_down = true;
    REQUIRE(overlay.handle_mouse_event(press));

    const Rect rb = right_ptr->bounds();
    MouseEvent drag;
    drag.position = {rb.x + rb.width * 0.5f, rb.y + rb.height * 0.5f};
    drag.is_down = false;
    REQUIRE(overlay.handle_mouse_event(drag));

    MouseEvent release;
    release.position = drag.position;
    release.is_down = true;
    overlay.handle_mouse_event(release);

    // (a) Live tree reparented.
    REQUIRE(moving_ptr->parent() == right_ptr);
    REQUIRE(history.undo_count() == 1);
    REQUIRE(history.undo_description() == "move-reflow");

    // (b) Source rewrite locked the structural change: moving's block now sits
    // physically AFTER the right (new parent) block, and its appendChild
    // receiver targets right2. The sink fired once on the initial commit.
    REQUIRE(sink_calls == 1);
    REQUIRE(pos("a-right") < pos("a-moving"));
    REQUIRE(source.find("right2.appendChild(moving1)") != std::string::npos);
    REQUIRE(source.find("left0.appendChild(moving1)") == std::string::npos);

    // (d) Idempotent re-apply of the engine against the now-rewritten source is
    // a no-op (the receiver already targets the new parent).
    {
        auto again = pulp::view::reparent_in_source(
            source, {"a-moving", "a-right"});
        REQUIRE(again.status == pulp::view::LockStatus::already_current);
        REQUIRE(again.source == source);
    }

    // (c) Undo reverts BOTH the live tree and the source. The undo sink re-emits
    // the edit with the ORIGINAL parent (a-left), so the engine moves the block
    // back under left.
    REQUIRE(history.undo());
    REQUIRE(moving_ptr->parent() == left_ptr);
    REQUIRE(sink_calls == 2);
    REQUIRE(pos("a-moving") < pos("a-right"));  // back above right's block
    REQUIRE(source.find("left0.appendChild(moving1)") != std::string::npos);
    REQUIRE(source.find("right2.appendChild(moving1)") == std::string::npos);

    // Redo re-applies to both surfaces.
    REQUIRE(history.redo());
    REQUIRE(moving_ptr->parent() == right_ptr);
    REQUIRE(sink_calls == 3);
    REQUIRE(pos("a-right") < pos("a-moving"));
    REQUIRE(source.find("right2.appendChild(moving1)") != std::string::npos);
}

// WYSIWYG T5 (gap #1) — when NO source sink is wired, a reflow reparent affects
// only the live tree + EditHistory (the structural edit is not locked to
// source). This is the default host posture for a non-design-import session.
TEST_CASE("InspectorOverlay T5: reflow reparent without a source sink is "
          "live-only",
          "[inspect][overlay][wysiwyg][t5][reparent][reflow]") {
    View root;
    root.set_bounds({0, 0, 600, 300});
    root.flex().direction = FlexDirection::row;

    auto left = std::make_unique<View>();
    left->set_anchor_id("a-left");
    left->flex().direction = FlexDirection::column;
    left->flex().preferred_width = 300;
    left->flex().preferred_height = 300;

    auto moving = std::make_unique<View>();
    moving->set_anchor_id("a-moving");
    moving->flex().preferred_width = 80;
    moving->flex().preferred_height = 40;
    auto* moving_ptr = moving.get();
    left->add_child(std::move(moving));
    root.add_child(std::move(left));

    auto right = std::make_unique<View>();
    right->set_anchor_id("a-right");
    right->flex().direction = FlexDirection::column;
    right->flex().preferred_width = 300;
    right->flex().preferred_height = 300;
    auto* right_ptr = right.get();
    root.add_child(std::move(right));
    root.layout_children();

    TweakStore store;
    pulp::state::EditHistory history;
    history.set_coalesce(false);
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_edit_history(&history);
    overlay.set_dragging_enabled(true);
    overlay.set_selected_view(moving_ptr);
    REQUIRE_FALSE(overlay.has_reparent_source_sink());

    const Rect mb = moving_ptr->bounds();
    MouseEvent press;
    press.position = {mb.x + 10, mb.y + 10};
    press.is_down = true;
    overlay.handle_mouse_event(press);
    const Rect rb = right_ptr->bounds();
    MouseEvent drag;
    drag.position = {rb.x + rb.width * 0.5f, rb.y + rb.height * 0.5f};
    drag.is_down = false;
    overlay.handle_mouse_event(drag);
    MouseEvent release;
    release.position = drag.position;
    release.is_down = true;
    overlay.handle_mouse_event(release);

    // Live reparent + undo still work with no sink wired.
    REQUIRE(moving_ptr->parent() == right_ptr);
    REQUIRE(history.undo());
    // (parent restored — same behavior as the pre-T5 landed gesture)
}

// Proportional resize: Shift + corner-handle drag SCALES the container's
// content (View::set_scale) rather than just stretching the box, and the
// scale is undoable.
TEST_CASE("InspectorOverlay P2c: Shift + handle drag scales content proportionally",
          "[inspect][overlay][p2c][resize][proportional][issue-wysiwyg-p2c]") {
    View root;
    root.set_bounds({0, 0, 600, 400});
    auto child = std::make_unique<View>();
    child->set_anchor_id("anchor-scale");
    child->set_bounds({10, 10, 80, 40});
    child->flex().preferred_width = 80;
    child->flex().preferred_height = 40;
    auto* child_ptr = child.get();
    root.add_child(std::move(child));

    REQUIRE(child_ptr->scale() == 1.0f);

    TweakStore store;
    pulp::state::EditHistory history;
    history.set_coalesce(false);
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_edit_history(&history);
    overlay.set_dragging_enabled(true);
    overlay.set_selected_view(child_ptr);

    // SHIFT + press on the SE handle (corner at 90,50).
    MouseEvent press;
    press.position = {90, 50};
    press.is_down = true;
    press.modifiers = kModShift;
    REQUIRE(overlay.handle_mouse_event(press));

    // Drag the SE corner out by +80/+40 (doubles the box) with Shift held.
    MouseEvent drag;
    drag.position = {170, 90};
    drag.is_down = false;
    drag.modifiers = kModShift;
    REQUIRE(overlay.handle_mouse_event(drag));

    // The content scale grew (proportional), not just the box.
    REQUIRE(child_ptr->scale() > 1.0f);
    // A transform.scale tweak landed.
    REQUIRE(store.lookup("anchor-scale", "transform.scale").has_value());

    MouseEvent release;
    release.position = {300, 300};
    release.is_down = true;
    overlay.handle_mouse_event(release);

    const float scaled = child_ptr->scale();
    REQUIRE(scaled > 1.0f);
    REQUIRE(history.undo_count() == 1);

    // Undo restores the original scale (1.0).
    REQUIRE(history.undo());
    REQUIRE(child_ptr->scale() == Catch::Approx(1.0f));

    // Redo re-applies the proportional scale.
    REQUIRE(history.redo());
    REQUIRE(child_ptr->scale() == Catch::Approx(scaled));
}

// Read-only mode is the supported OPT-OUT (the default is two-way, P2e). In
// read-only mode a tree-row "click" (firing the tree's on_select) shows the
// node's properties but does NOT change the shared selection / fire
// on_view_selected. reflect_selection() never fires the callback either (it is
// the no-feedback canvas → window mirror).
TEST_CASE("InspectorWindow P2e: read-only mode opt-out suppresses the select "
          "callback",
          "[inspect][window][p2e][readonly][issue-wysiwyg-p2e]") {
    View inspected_root;
    inspected_root.set_bounds({0, 0, 400, 300});
    auto child = std::make_unique<View>();
    child->set_id("decouple-child");
    auto* child_ptr = child.get();
    inspected_root.add_child(std::move(child));

    InspectorWindow window(inspected_root);
    window.set_selection_readonly(true);
    REQUIRE(window.selection_readonly());

    bool callback_fired = false;
    window.on_view_selected = [&](View*) { callback_fired = true; };

    // Find the tree and the child's node.
    auto* tabs = first_view_of_type<TabPanel>(window);
    REQUIRE(tabs != nullptr);
    auto* tree = first_view_of_type<TreeView>(*tabs->child_at(0));
    REQUIRE(tree != nullptr);
    auto* node = tree->find_node_by_user_data(child_ptr);
    REQUIRE(node != nullptr);

    // Simulate a tree-row click by firing on_select (the path the TreeView
    // invokes on a real click). In read-only mode this must NOT fire
    // on_view_selected.
    REQUIRE(tree->on_select);
    tree->on_select(*node);
    REQUIRE_FALSE(callback_fired);

    // reflect_selection (the one-way canvas → window mirror) also never fires
    // the callback.
    window.reflect_selection(child_ptr);
    REQUIRE_FALSE(callback_fired);
}

// ── WYSIWYG P2d polish ──────────────────────────────────────────────────────
//
// planning/2026-05-21-wysiwyg-direct-manipulation-extension.md (P2d):
//   B. cursor affordances over the selected element (move vs resize vs none)
//   D. drop-indicator clarity — never shown at rest, only during an active drag
//   A. reflect-state-only — a canvas reflection must NOT highlight a tree row

// P2d (D): a selected-but-idle element shows no drop indicator. The blue
// insertion line / container highlight is reserved for an ACTIVE reflow drag.
TEST_CASE("InspectorOverlay P2d: drop indicator is NOT shown when idle",
          "[inspect][overlay][p2d][drop-indicator][issue-wysiwyg-p2d]") {
    View root;
    root.set_bounds({0, 0, 600, 200});
    root.flex().direction = FlexDirection::row;

    auto a = std::make_unique<View>();
    a->set_anchor_id("anchor-a");
    a->flex().preferred_width = 100;
    a->flex().preferred_height = 50;
    auto* a_ptr = a.get();
    root.add_child(std::move(a));

    auto b = std::make_unique<View>();
    b->set_anchor_id("anchor-b");
    b->flex().preferred_width = 100;
    b->flex().preferred_height = 50;
    auto* b_ptr = b.get();
    root.add_child(std::move(b));

    auto c = std::make_unique<View>();
    c->set_anchor_id("anchor-c");
    c->flex().preferred_width = 100;
    c->flex().preferred_height = 50;
    root.add_child(std::move(c));
    root.layout_children();

    TweakStore store;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_dragging_enabled(true);
    overlay.set_selected_view(a_ptr);

    // At rest: a is selected, dragging mode on, but no drag in progress.
    // There must be NO drop indicator.
    REQUIRE_FALSE(overlay.drop_indicator_active());

    // Hovering the selected element (still no press) must not raise it either.
    MouseEvent hover;
    hover.position = {a_ptr->bounds().x + 10, a_ptr->bounds().y + 10};
    hover.is_down = false;
    overlay.handle_mouse_event(hover);
    REQUIRE_FALSE(overlay.drop_indicator_active());

    // Begin a plain (reflow) body-drag and move past b's midpoint — NOW the
    // drop indicator appears (we're actively dragging with a resolved target).
    MouseEvent press;
    press.position = {a_ptr->bounds().x + 10, a_ptr->bounds().y + 10};
    press.is_down = true;
    REQUIRE(overlay.handle_mouse_event(press));

    MouseEvent drag;
    drag.position = {b_ptr->bounds().x + b_ptr->bounds().width - 5,
                     b_ptr->bounds().y + 10};
    drag.is_down = false;
    REQUIRE(overlay.handle_mouse_event(drag));
    REQUIRE(overlay.drop_indicator_active());  // shown DURING the drag

    // Release commits and clears the drop indicator — back to a single
    // selection affordance, no drop indicator.
    MouseEvent release;
    release.position = drag.position;
    release.is_down = true;
    overlay.handle_mouse_event(release);
    REQUIRE_FALSE(overlay.drop_indicator_active());
}

// P2d (D) regression: an ABSOLUTE-float (⌘-drag) move must also never raise
// the reflow drop indicator (the float path moves the live element directly
// and uses a ghost, not the insertion line).
TEST_CASE("InspectorOverlay P2d: float (Cmd) move shows no reflow drop indicator",
          "[inspect][overlay][p2d][drop-indicator][issue-wysiwyg-p2d]") {
    View root;
    root.set_bounds({0, 0, 600, 200});

    auto a = std::make_unique<View>();
    a->set_anchor_id("anchor-a");
    a->flex().preferred_width = 100;
    a->flex().preferred_height = 50;
    a->set_bounds({20, 20, 100, 50});
    auto* a_ptr = a.get();
    root.add_child(std::move(a));
    root.layout_children();

    TweakStore store;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_dragging_enabled(true);
    overlay.set_selected_view(a_ptr);

    // ⌘-drag = absolute float.
    MouseEvent press;
    press.position = {a_ptr->bounds().x + 10, a_ptr->bounds().y + 10};
    press.is_down = true;
    press.modifiers = kModCmd;
    REQUIRE(overlay.handle_mouse_event(press));

    MouseEvent drag;
    drag.position = {200, 120};
    drag.is_down = false;
    drag.modifiers = kModCmd;
    REQUIRE(overlay.handle_mouse_event(drag));
    // Float move: live element repositioned, but the reflow drop indicator
    // (insertion line) is NOT used.
    REQUIRE_FALSE(overlay.drop_indicator_active());
}

// P2d (B): the cursor-affordance hit-test returns resize over a corner
// handle, move over the body, and none outside / when dragging mode is off.
TEST_CASE("InspectorOverlay P2d: cursor affordance is move on body, resize on "
          "corner, none outside",
          "[inspect][overlay][p2d][cursor][issue-wysiwyg-p2d]") {
    using CA = InspectorOverlay::CursorAffordance;

    View root;
    root.set_bounds({0, 0, 400, 300});

    auto box = std::make_unique<View>();
    box->set_anchor_id("anchor-box");
    box->set_bounds({100, 80, 120, 60});  // root coords: x 100..220, y 80..140
    auto* box_ptr = box.get();
    root.add_child(std::move(box));
    // NOTE: no layout_children() — the cursor hit-test reads bounds() directly
    // via view_bounds_in_root(); we want the explicit box bounds preserved.

    InspectorOverlay overlay(root);
    overlay.set_active(true);

    // Dragging mode OFF → no affordance even over the body.
    overlay.set_selected_view(box_ptr);
    REQUIRE(overlay.cursor_affordance_at({160, 110}) == CA::none);
    REQUIRE(overlay.cursor_style_for({160, 110}) == -1);

    overlay.set_dragging_enabled(true);

    // Over the body interior → MOVE.
    REQUIRE(overlay.cursor_affordance_at({160, 110}) == CA::move);
    REQUIRE(overlay.cursor_style_for({160, 110}) ==
            static_cast<int>(View::CursorStyle::multi_directional_resize));

    // Over the NW corner handle (top-left = 100,80) → diagonal NW-SE resize.
    REQUIRE(overlay.cursor_affordance_at({100, 80}) == CA::resize_nw_se);
    REQUIRE(overlay.cursor_style_for({100, 80}) ==
            static_cast<int>(View::CursorStyle::top_left_resize));
    // SE corner (220,140) is the same NW-SE diagonal.
    REQUIRE(overlay.cursor_affordance_at({220, 140}) == CA::resize_nw_se);

    // NE corner (220,80) and SW corner (100,140) → the other diagonal.
    REQUIRE(overlay.cursor_affordance_at({220, 80}) == CA::resize_ne_sw);
    REQUIRE(overlay.cursor_affordance_at({100, 140}) == CA::resize_ne_sw);
    REQUIRE(overlay.cursor_style_for({220, 80}) ==
            static_cast<int>(View::CursorStyle::top_right_resize));

    // Well outside the selection → no override.
    REQUIRE(overlay.cursor_affordance_at({350, 280}) == CA::none);
    REQUIRE(overlay.cursor_style_for({350, 280}) == -1);

    // No selection → no affordance.
    overlay.set_selected_view(nullptr);
    REQUIRE(overlay.cursor_affordance_at({160, 110}) == CA::none);
}

// P2e (CORRECTS P2d): reflect_selection (a canvas-driven mirror) DOES
// highlight the matching tree row AND shows the node's properties — two-way
// selection means a canvas pick highlights the corresponding row in the
// inspector tree. (Maintainer correction: "we DO want to be able to tap and
// select an item in the inspector as it works today." The only forbidden
// thing is a stray selection BOX inside the inspector window — that leak is
// fixed in the paint hook, not by stripping the row highlight.)
TEST_CASE("InspectorWindow P2e: reflect_selection highlights the matching tree row",
          "[inspect][window][p2e][reflect][issue-wysiwyg-p2e]") {
    View inspected_root;
    inspected_root.set_id("root");

    auto child = std::make_unique<View>();
    child->set_id("child");
    auto* child_ptr = child.get();
    inspected_root.add_child(std::move(child));

    InspectorWindow window(inspected_root);

    auto* tabs = first_view_of_type<TabPanel>(window);
    REQUIRE(tabs != nullptr);
    auto* tree = first_view_of_type<TreeView>(*tabs->child_at(0));
    REQUIRE(tree != nullptr);

    // Build the tree once.
    window.refresh();
    auto* node = tree->find_node_by_user_data(child_ptr);
    REQUIRE(node != nullptr);

    // A canvas reflection: shows properties AND highlights the matching row.
    window.reflect_selection(child_ptr);
    REQUIRE(tree->selected_node() == node);

    // A subsequent refresh (idle tick) keeps the row highlighted — the
    // reflection's highlight survives the next 30 Hz refresh.
    window.refresh();
    REQUIRE(tree->selected_node() == node);

    // A DELIBERATE tree click also highlights its own row (unchanged).
    REQUIRE(tree->on_select);
    tree->set_selected_node(node);   // TreeView selects on click before on_select
    tree->on_select(*node);
    REQUIRE(tree->selected_node() == node);
    window.refresh();
    REQUIRE(tree->selected_node() == node);
}

// P2e Fix 1 — paint-leak gate. The installed inspector paint hook must NOT
// paint the in-canvas overlay when the painting root is NOT the inspected
// root (e.g. the floating InspectorWindow painting its own root). This is the
// root cause of the "stray box at a random coordinate inside the inspector
// window": the global paint hook fired for every root that painted, so the
// overlay's selection box leaked into the inspector window's surface.
TEST_CASE("InspectorOverlay P2e: paint hook gates on the painting root",
          "[inspect][overlay][p2e][paint-leak][issue-wysiwyg-p2e]") {
    View inspected_root;
    inspected_root.set_bounds({0, 0, 400, 300});
    auto child = std::make_unique<View>();
    child->set_bounds({40, 40, 100, 60});
    auto* child_ptr = child.get();
    inspected_root.add_child(std::move(child));

    // A SEPARATE root standing in for the floating InspectorWindow's own root.
    View other_root;
    other_root.set_bounds({0, 0, 340, 300});

    InspectorOverlay overlay(inspected_root);
    overlay.set_active(true);
    overlay.set_selected_view(child_ptr);  // something to draw a box around

    install_inspector_hooks(overlay);

    // Painting the INSPECTED root: the overlay paints (selection box etc.).
    pulp::canvas::RecordingCanvas inspected_canvas;
    View::paint_overlays(inspected_canvas, &inspected_root);
    REQUIRE(inspected_canvas.command_count() > 0);

    // Painting a DIFFERENT root (the inspector window's own root): the overlay
    // must NOT paint — no stray box leaks into that surface.
    pulp::canvas::RecordingCanvas other_canvas;
    View::paint_overlays(other_canvas, &other_root);
    REQUIRE(other_canvas.command_count() == 0);

    // nullptr (legacy/headless caller, root unknown) still paints.
    pulp::canvas::RecordingCanvas null_canvas;
    View::paint_overlays(null_canvas, nullptr);
    REQUIRE(null_canvas.command_count() > 0);

    // Clean up the global hooks so later tests aren't affected.
    g_active_inspector = nullptr;
    View::set_inspector_paint_hook({});
    View::set_inspector_key_hook({});
    View::set_inspector_mouse_hook({});
}

// P2e Fix 2 — two-way selection. A tree-row click (default, non-read-only)
// fires on_view_selected so the host can drive the SHARED canvas selection,
// and a canvas-driven reflection highlights the matching tree row. This
// asserts the round-trip both directions without recursing.
TEST_CASE("InspectorWindow P2e: two-way selection (canvas <-> tree row)",
          "[inspect][window][p2e][two-way][issue-wysiwyg-p2e]") {
    View inspected_root;
    inspected_root.set_bounds({0, 0, 400, 300});
    auto child = std::make_unique<View>();
    child->set_id("child");
    auto* child_ptr = child.get();
    inspected_root.add_child(std::move(child));

    InspectorWindow window(inspected_root);
    // Default (NOT read-only) — two-way selection is the P2e default.
    REQUIRE_FALSE(window.selection_readonly());

    auto* tabs = first_view_of_type<TabPanel>(window);
    REQUIRE(tabs != nullptr);
    auto* tree = first_view_of_type<TreeView>(*tabs->child_at(0));
    REQUIRE(tree != nullptr);
    window.refresh();
    auto* node = tree->find_node_by_user_data(child_ptr);
    REQUIRE(node != nullptr);

    // Direction 1: a tree-row click drives canvas selection via the callback.
    View* canvas_selected = nullptr;
    int callback_count = 0;
    window.on_view_selected = [&](View* v) {
        ++callback_count;
        canvas_selected = v;
    };
    REQUIRE(tree->on_select);
    tree->set_selected_node(node);   // TreeView highlights the row on click
    tree->on_select(*node);
    REQUIRE(callback_count == 1);
    REQUIRE(canvas_selected == child_ptr);   // drove the shared canvas selection
    REQUIRE(tree->selected_node() == node);  // and its own row highlights

    // Direction 2: a canvas reflection highlights the matching row WITHOUT
    // firing on_view_selected (no feedback loop / recursion).
    window.reflect_selection(child_ptr);
    REQUIRE(callback_count == 1);            // reflection did NOT re-fire
    REQUIRE(tree->selected_node() == node);  // row highlighted by the reflection
}

// ── WYSIWYG P2h — interactive manipulation regressions ──────────────────────
//
// These exercise the gesture state machine with the EXPLICIT MousePhase the
// mac host now stamps (press / drag / release / hover), which is the OPPOSITE
// is_down convention from the legacy headless tests above. The pre-P2h
// machine inferred drag-vs-release from is_down alone, so a live mac drag
// ended the gesture on the first drag tick and fell through to re-selection.

namespace {
// Build a two-child root used by the P2h move/resize cases. `a` is the
// element under manipulation; `b` is a second, non-overlapping element the
// drag must NOT accidentally re-select.
struct TwoChild {
    View root;
    View* a = nullptr;
    View* b = nullptr;
};
std::unique_ptr<TwoChild> make_two_child() {
    auto tc = std::make_unique<TwoChild>();
    tc->root.set_bounds({0, 0, 600, 400});
    auto a = std::make_unique<View>();
    a->set_anchor_id("a");
    a->set_bounds({40, 40, 120, 80});      // body spans 40..160 / 40..120
    a->set_position(View::Position::absolute);
    a->set_left(40); a->set_top(40);
    tc->a = a.get();
    tc->root.add_child(std::move(a));
    auto b = std::make_unique<View>();
    b->set_anchor_id("b");
    b->set_bounds({300, 200, 120, 80});    // far away, no overlap with a
    tc->b = b.get();
    tc->root.add_child(std::move(b));
    return tc;
}
}  // namespace

TEST_CASE("InspectorOverlay P2h: body-press of selected element begins a MOVE, "
          "drag moves THAT element and never re-selects",
          "[inspect][overlay][p2h][move][regression1]") {
    auto tc = make_two_child();
    TweakStore store;
    InspectorOverlay overlay(tc->root);
    overlay.set_manipulate_only(true);   // dragging implicitly enabled
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_selected_view(tc->a);    // a is already selected (orange)

    const float left0 = tc->a->left();
    const float top0 = tc->a->top();

    // PRESS on a's BODY (centre) with explicit press phase — must begin a
    // move of a, NOT re-run selection hit-testing.
    MouseEvent press;
    press.position = {100, 80};          // inside a (40..160 / 40..120)
    press.is_down = true;
    press.phase = MousePhase::press;
    REQUIRE(overlay.handle_mouse_event(press));   // consumed → move started
    REQUIRE(overlay.selected_view() == tc->a);    // still a, not b

    // DRAG ticks (explicit drag phase, is_down still true on a real mac).
    // ⌘ is NOT held here → reflow move; assert it does not re-select b even
    // though the cursor passes over b's region.
    MouseEvent drag1;
    drag1.position = {320, 210};         // now over b's body
    drag1.is_down = true;
    drag1.phase = MousePhase::drag;
    REQUIRE(overlay.handle_mouse_event(drag1));
    REQUIRE(overlay.selected_view() == tc->a);    // selection NOT hijacked by b

    MouseEvent drag2;
    drag2.position = {340, 230};
    drag2.is_down = true;
    drag2.phase = MousePhase::drag;
    REQUIRE(overlay.handle_mouse_event(drag2));
    REQUIRE(overlay.selected_view() == tc->a);

    // RELEASE — gesture commits; selection is still a.
    MouseEvent release;
    release.position = {340, 230};
    release.is_down = false;
    release.phase = MousePhase::release;
    overlay.handle_mouse_event(release);
    REQUIRE(overlay.selected_view() == tc->a);
    (void)left0; (void)top0;
}

TEST_CASE("InspectorOverlay P2h: ⌘-drag float move repositions the selected "
          "element (and only it)",
          "[inspect][overlay][p2h][move][regression1]") {
    auto tc = make_two_child();
    TweakStore store;
    InspectorOverlay overlay(tc->root);
    overlay.set_manipulate_only(true);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_selected_view(tc->a);

    const float left0 = tc->a->left();
    const float top0 = tc->a->top();

    // ⌘ held → absolute-float move path mutates left/top live.
    MouseEvent press;
    press.position = {100, 80};
    press.is_down = true;
    press.modifiers = kModCmd;
    press.phase = MousePhase::press;
    REQUIRE(overlay.handle_mouse_event(press));

    MouseEvent drag;
    drag.position = {130, 110};          // +30 / +30
    drag.is_down = true;
    drag.modifiers = kModCmd;
    drag.phase = MousePhase::drag;
    REQUIRE(overlay.handle_mouse_event(drag));

    // a moved by the drag delta; b untouched; selection unchanged.
    REQUIRE(tc->a->left() == Catch::Approx(left0 + 30.0f));
    REQUIRE(tc->a->top() == Catch::Approx(top0 + 30.0f));
    REQUIRE(overlay.selected_view() == tc->a);

    MouseEvent release;
    release.position = {130, 110};
    release.is_down = false;
    release.phase = MousePhase::release;
    overlay.handle_mouse_event(release);
    REQUIRE(overlay.selected_view() == tc->a);
}

TEST_CASE("InspectorOverlay P2h: corner-handle press begins a RESIZE and the "
          "drag changes size (explicit phase)",
          "[inspect][overlay][p2h][resize][regression2]") {
    View root;
    root.set_bounds({0, 0, 600, 400});
    auto child = std::make_unique<View>();
    child->set_anchor_id("a");
    child->set_bounds({10, 10, 80, 40});     // SE corner at (90, 50)
    auto* child_ptr = child.get();
    root.add_child(std::move(child));

    TweakStore store;
    InspectorOverlay overlay(root);
    overlay.set_manipulate_only(true);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_selected_view(child_ptr);

    // PRESS on the SE handle (90, 50) — explicit press phase.
    MouseEvent press;
    press.position = {90, 50};
    press.is_down = true;
    press.phase = MousePhase::press;
    REQUIRE(overlay.handle_mouse_event(press));     // resize started

    // DRAG +20 / +15 (explicit drag phase, is_down still true).
    MouseEvent drag;
    drag.position = {110, 65};
    drag.is_down = true;
    drag.phase = MousePhase::drag;
    REQUIRE(overlay.handle_mouse_event(drag));

    REQUIRE(child_ptr->bounds().width == 100.0f);   // 80 + 20
    REQUIRE(child_ptr->bounds().height == 55.0f);    // 40 + 15
    REQUIRE(store.lookup("a", "layout.width")->getFloat32() == 100.0f);
    REQUIRE(store.lookup("a", "layout.height")->getFloat32() == 55.0f);

    MouseEvent release;
    release.position = {110, 65};
    release.is_down = false;
    release.phase = MousePhase::release;
    overlay.handle_mouse_event(release);
}

TEST_CASE("InspectorOverlay P2h: edge-handle press resizes a single axis",
          "[inspect][overlay][p2h][resize][regression2][regression5]") {
    View root;
    root.set_bounds({0, 0, 600, 400});
    auto child = std::make_unique<View>();
    child->set_anchor_id("a");
    child->set_bounds({100, 100, 80, 40});   // E edge mid at (180, 120)
    auto* child_ptr = child.get();
    root.add_child(std::move(child));

    TweakStore store;
    InspectorOverlay overlay(root);
    overlay.set_manipulate_only(true);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_selected_view(child_ptr);

    // Press on the EAST edge midpoint (right edge, vertical centre).
    MouseEvent press;
    press.position = {180, 120};
    press.is_down = true;
    press.phase = MousePhase::press;
    REQUIRE(overlay.handle_mouse_event(press));

    MouseEvent drag;
    drag.position = {200, 130};              // +20 x, +10 y
    drag.is_down = true;
    drag.phase = MousePhase::drag;
    REQUIRE(overlay.handle_mouse_event(drag));

    // East edge resizes WIDTH only — height is unchanged.
    REQUIRE(child_ptr->bounds().width == 100.0f);  // 80 + 20
    REQUIRE(child_ptr->bounds().height == 40.0f);  // unchanged
}

TEST_CASE("InspectorOverlay P2h: Esc deselect leaves hover + click-to-select "
          "working with no Cmd+I cycle",
          "[inspect][overlay][p2h][regression3]") {
    auto tc = make_two_child();
    InspectorOverlay overlay(tc->root);
    overlay.set_manipulate_only(true);
    overlay.set_active(true);
    overlay.set_selected_view(tc->a);

    // Esc → deselect, stay active.
    KeyEvent esc;
    esc.key = KeyCode::escape;
    esc.is_down = true;
    REQUIRE(overlay.handle_key_event(esc));
    REQUIRE(overlay.selected_view() == nullptr);
    REQUIRE(overlay.is_active());              // still in inspect mode

    // Hover over b (explicit hover phase) — highlight predicate is true:
    // hovered_ tracks b again, NOT pinned dead.
    MouseEvent hover;
    hover.position = {340, 230};               // inside b
    hover.is_down = false;
    hover.phase = MousePhase::hover;
    REQUIRE_FALSE(overlay.handle_mouse_event(hover));   // hover not consumed
    REQUIRE(overlay.hovered_view() == tc->b);

    // Click selects again — no Cmd+I cycle needed.
    MouseEvent click;
    click.position = {340, 230};
    click.is_down = true;
    click.phase = MousePhase::press;
    REQUIRE(overlay.handle_mouse_event(click));
    REQUIRE(overlay.selected_view() == tc->b);
}

TEST_CASE("InspectorOverlay P2h: a drag tick on empty canvas never mutates "
          "selection (no rubber-band re-select)",
          "[inspect][overlay][p2h][regression1]") {
    auto tc = make_two_child();
    InspectorOverlay overlay(tc->root);
    overlay.set_manipulate_only(true);
    overlay.set_active(true);
    overlay.set_selected_view(tc->a);

    // A bare DRAG tick (explicit phase) that did not begin a gesture — e.g.
    // a drag started over empty space. It is consumed but must NOT re-run
    // selection hit-testing, so the selection stays a.
    MouseEvent drag;
    drag.position = {340, 230};                // over b
    drag.is_down = true;
    drag.phase = MousePhase::drag;
    REQUIRE(overlay.handle_mouse_event(drag));  // consumed
    REQUIRE(overlay.selected_view() == tc->a);  // selection NOT hijacked
}

TEST_CASE("InspectorOverlay P2h: context-aware resize/move cursor per zone",
          "[inspect][overlay][p2h][regression5][cursor]") {
    View root;
    root.set_bounds({0, 0, 600, 400});
    auto child = std::make_unique<View>();
    child->set_anchor_id("a");
    child->set_bounds({100, 100, 80, 60});    // 100..180 / 100..160
    auto* child_ptr = child.get();
    root.add_child(std::move(child));

    InspectorOverlay overlay(root);
    overlay.set_manipulate_only(true);
    overlay.set_active(true);
    overlay.set_selected_view(child_ptr);

    using CA = InspectorOverlay::CursorAffordance;
    const float l = 100, t = 100, r = 180, b = 160;
    const float mx = 140, my = 130;

    // Corners → diagonals.
    REQUIRE(overlay.cursor_affordance_at({l, t}) == CA::resize_nw_se);  // NW
    REQUIRE(overlay.cursor_affordance_at({r, b}) == CA::resize_nw_se);  // SE
    REQUIRE(overlay.cursor_affordance_at({r, t}) == CA::resize_ne_sw);  // NE
    REQUIRE(overlay.cursor_affordance_at({l, b}) == CA::resize_ne_sw);  // SW
    // Edges → single-axis.
    REQUIRE(overlay.cursor_affordance_at({mx, t}) == CA::resize_ns);    // N
    REQUIRE(overlay.cursor_affordance_at({mx, b}) == CA::resize_ns);    // S
    REQUIRE(overlay.cursor_affordance_at({r, my}) == CA::resize_ew);    // E
    REQUIRE(overlay.cursor_affordance_at({l, my}) == CA::resize_ew);    // W
    // Body → move.
    REQUIRE(overlay.cursor_affordance_at({mx, my}) == CA::move);
    // Outside → none.
    REQUIRE(overlay.cursor_affordance_at({500, 300}) == CA::none);

    // cursor_style_for maps each to the right View::CursorStyle int.
    REQUIRE(overlay.cursor_style_for({l, t}) ==
            static_cast<int>(View::CursorStyle::top_left_resize));
    REQUIRE(overlay.cursor_style_for({r, t}) ==
            static_cast<int>(View::CursorStyle::top_right_resize));
    REQUIRE(overlay.cursor_style_for({mx, t}) ==
            static_cast<int>(View::CursorStyle::vertical_resize));
    REQUIRE(overlay.cursor_style_for({r, my}) ==
            static_cast<int>(View::CursorStyle::horizontal_resize));
    REQUIRE(overlay.cursor_style_for({mx, my}) ==
            static_cast<int>(View::CursorStyle::multi_directional_resize));
    REQUIRE(overlay.cursor_style_for({500, 300}) == -1);  // default
}

TEST_CASE("InspectorOverlay P2h: legacy is_down gesture convention still works "
          "(no explicit phase)",
          "[inspect][overlay][p2h][regression2]") {
    // Guards the headless JUCE-style convention path: press=is_down,
    // drag=!is_down, release=is_down, phase left automatic. This is the
    // convention the pre-P2h tests use, and it must remain intact.
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
    overlay.set_dragging_enabled(true);
    overlay.set_selected_view(child_ptr);

    MouseEvent press;          // press (is_down, automatic phase)
    press.position = {90, 50};
    press.is_down = true;
    REQUIRE(overlay.handle_mouse_event(press));

    MouseEvent drag;           // legacy drag tick (is_down=false)
    drag.position = {110, 65};
    drag.is_down = false;
    REQUIRE(overlay.handle_mouse_event(drag));
    REQUIRE(child_ptr->bounds().width == 100.0f);
    REQUIRE(child_ptr->bounds().height == 55.0f);
}

// ── WYSIWYG P2i (Refinement A) ────────────────────────────────────────────
// During a reflow body-drag over a flex container, resolve_drop_target() must
// resolve a valid insertion slot for EVERY cursor position across the child
// span (before-first, between each pair, after-last) and surface the
// Figma-style blue insertion LINE — not a vague container highlight — at the
// resolved boundary. The committed drop must land at the indicated slot.
TEST_CASE("InspectorOverlay P2i: reflow drag shows an insertion LINE at every "
          "slot across a row container's children",
          "[inspect][overlay][p2i][reflow][drop-indicator][issue-wysiwyg-p2i]") {
    View root;
    root.set_bounds({0, 0, 600, 200});
    root.flex().direction = FlexDirection::row;

    // Four equal siblings so there are 5 boundaries: before a, a|b, b|c, c|d,
    // after d. We drag `a` and aim the cursor at each boundary in turn.
    auto make = [&](const char* anchor) -> View* {
        auto v = std::make_unique<View>();
        v->set_anchor_id(anchor);
        v->flex().preferred_width = 100;
        v->flex().preferred_height = 50;
        View* p = v.get();
        root.add_child(std::move(v));
        return p;
    };
    View* a = make("anchor-a");
    View* b = make("anchor-b");
    View* c = make("anchor-c");
    View* d = make("anchor-d");
    root.layout_children();

    TweakStore store;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_dragging_enabled(true);
    overlay.set_selected_view(a);

    // Begin a plain (reflow) body-drag on `a`.
    MouseEvent press;
    press.position = {a->bounds().x + 10, a->bounds().y + 10};
    press.is_down = true;
    REQUIRE(overlay.handle_mouse_event(press));

    const float cy = b->bounds().y + b->bounds().height * 0.5f;

    // Helper: drag to an x position, then assert the indicator is a LINE
    // (not a highlight) and report the resolved slot index.
    auto drag_to_x = [&](float x) -> int {
        MouseEvent drag;
        drag.position = {x, cy};
        drag.is_down = false;
        REQUIRE(overlay.handle_mouse_event(drag));
        REQUIRE(overlay.drop_indicator_active());
        // A populated row container always resolves to the between-sibling
        // insertion LINE, never the empty-container highlight.
        REQUIRE(overlay.drop_indicator_is_line());
        // The line is a thin vertical bar spanning the container cross-axis.
        const Rect ind = overlay.drop_indicator_rect();
        REQUIRE(ind.width <= 3.0f);
        REQUIRE(ind.height > 0.0f);
        return overlay.drop_index();
    };

    // The drop slots exclude the dragged node `a`, so the visible siblings in
    // order are {b, c, d} (3 slots → indices 0..3). Sweep across the span and
    // assert the index increases monotonically as the cursor moves right.
    // Far left (before b): slot 0.
    int s_before = drag_to_x(b->bounds().x - 5);
    REQUIRE(s_before == 0);

    // Between b and c.
    int s_bc = drag_to_x((b->bounds().x + b->bounds().width + c->bounds().x) * 0.5f);
    REQUIRE(s_bc == 1);

    // Between c and d.
    int s_cd = drag_to_x((c->bounds().x + c->bounds().width + d->bounds().x) * 0.5f);
    REQUIRE(s_cd == 2);

    // After d (far right): last slot (3).
    int s_after = drag_to_x(d->bounds().x + d->bounds().width + 5);
    REQUIRE(s_after == 3);

    // Monotonic across the whole span.
    REQUIRE(s_before < s_bc);
    REQUIRE(s_bc < s_cd);
    REQUIRE(s_cd < s_after);

    // The committed drop must match the LAST indicated slot (after d): `a`
    // ends up last in the row.
    const int indicated = overlay.drop_index();
    REQUIRE(indicated == 3);
    MouseEvent release;
    release.position = {d->bounds().x + d->bounds().width + 5, cy};
    release.is_down = true;
    overlay.handle_mouse_event(release);
    root.layout_children();
    // `a` now sorts after b, c, d — i.e. it landed at the indicated slot.
    REQUIRE(a->bounds().x > b->bounds().x);
    REQUIRE(a->bounds().x > c->bounds().x);
    REQUIRE(a->bounds().x > d->bounds().x);
}

// Column container variant — the insertion LINE is horizontal and the slot
// index tracks the cursor's Y across the child span.
TEST_CASE("InspectorOverlay P2i: reflow drag shows a horizontal insertion LINE "
          "across a column container's children",
          "[inspect][overlay][p2i][reflow][drop-indicator][issue-wysiwyg-p2i]") {
    View root;
    root.set_bounds({0, 0, 200, 600});
    root.flex().direction = FlexDirection::column;

    auto make = [&](const char* anchor) -> View* {
        auto v = std::make_unique<View>();
        v->set_anchor_id(anchor);
        v->flex().preferred_width = 100;
        v->flex().preferred_height = 100;
        View* p = v.get();
        root.add_child(std::move(v));
        return p;
    };
    View* a = make("col-a");
    View* b = make("col-b");
    View* c = make("col-c");
    root.layout_children();

    TweakStore store;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_dragging_enabled(true);
    overlay.set_selected_view(a);

    MouseEvent press;
    press.position = {a->bounds().x + 10, a->bounds().y + 10};
    press.is_down = true;
    REQUIRE(overlay.handle_mouse_event(press));

    const float cx = b->bounds().x + b->bounds().width * 0.5f;
    auto drag_to_y = [&](float y) -> int {
        MouseEvent drag;
        drag.position = {cx, y};
        drag.is_down = false;
        REQUIRE(overlay.handle_mouse_event(drag));
        REQUIRE(overlay.drop_indicator_active());
        REQUIRE(overlay.drop_indicator_is_line());
        const Rect ind = overlay.drop_indicator_rect();
        // Horizontal line: thin in height, spans the cross-axis width.
        REQUIRE(ind.height <= 3.0f);
        REQUIRE(ind.width > 0.0f);
        return overlay.drop_index();
    };

    // Visible slots {b, c} (dragged `a` excluded): indices 0..2.
    int s_before = drag_to_y(b->bounds().y - 5);
    int s_bc = drag_to_y((b->bounds().y + b->bounds().height + c->bounds().y) * 0.5f);
    int s_after = drag_to_y(c->bounds().y + c->bounds().height + 5);
    REQUIRE(s_before == 0);
    REQUIRE(s_bc == 1);
    REQUIRE(s_after == 2);
}

// ── WYSIWYG P2i (Refinement B) ────────────────────────────────────────────
// After a Shift (proportional) resize, the scaled content must stay WITHIN
// the resize box: every direct child's scaled rect (origin-(0,0) anchored,
// scaled by the view's scale()) must lie inside the container's box bounds —
// no spill past any edge. Also asserts the box is clipped (overflow:hidden)
// and the scale anchor is top-left.
TEST_CASE("InspectorOverlay P2i: Shift-resize keeps scaled content inside the "
          "box (no spill past the selection rectangle)",
          "[inspect][overlay][p2i][resize][proportional][issue-wysiwyg-p2i]") {
    View root;
    root.set_bounds({0, 0, 800, 600});

    // A container whose child fills it edge-to-edge at scale 1 (a knob that
    // already touches the box corners — the worst case for spill). Bounds are
    // set manually and we do NOT call layout_children() before the gesture so
    // the box stays put (Yoga would otherwise reflow a flex child to the
    // content origin and move the resize handles). This mirrors the existing
    // P2c proportional-resize test's setup.
    auto box = std::make_unique<View>();
    box->set_anchor_id("anchor-box");
    box->set_bounds({40, 40, 120, 80});
    box->flex().preferred_width = 120;
    box->flex().preferred_height = 80;
    View* box_ptr = box.get();

    auto knob = std::make_unique<View>();
    knob->set_anchor_id("anchor-knob");
    // Child fills the box exactly at scale 1 (local coords, origin at box TL).
    knob->set_bounds({0, 0, 120, 80});
    knob->flex().preferred_width = 120;
    knob->flex().preferred_height = 80;
    View* knob_ptr = knob.get();
    box->add_child(std::move(knob));
    root.add_child(std::move(box));

    TweakStore store;
    pulp::state::EditHistory history;
    history.set_coalesce(false);
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_edit_history(&history);
    overlay.set_dragging_enabled(true);
    overlay.set_selected_view(box_ptr);

    // Shift + SE-corner press (corner at box bottom-right: 40+120, 40+80).
    MouseEvent press;
    press.position = {160, 120};
    press.is_down = true;
    press.modifiers = kModShift;
    REQUIRE(overlay.handle_mouse_event(press));

    // NON-UNIFORM corner drag: grow width much more than height. With the old
    // average-ratio + center-origin scheme the content would overflow the
    // top/left of the box. Drag +200 in x, +20 in y.
    MouseEvent drag;
    drag.position = {360, 140};
    drag.is_down = false;
    drag.modifiers = kModShift;
    REQUIRE(overlay.handle_mouse_event(drag));

    // The content scaled up.
    REQUIRE(box_ptr->scale() > 1.0f);

    // Box grew.
    const Rect bb = box_ptr->bounds();
    REQUIRE(bb.width > 120.0f);

    // Scale is anchored at the box's top-left, and the box clips its content.
    REQUIRE(box_ptr->transform_origin_x() == Catch::Approx(0.0f));
    REQUIRE(box_ptr->transform_origin_y() == Catch::Approx(0.0f));
    REQUIRE(box_ptr->overflow() == View::Overflow::hidden);

    // The child's SCALED extent (origin-(0,0) anchored) must fit inside the
    // box on every edge. Local child rect scaled by box scale:
    const float s = box_ptr->scale();
    const Rect kb = knob_ptr->bounds();  // local coords within box
    const float scaled_left   = kb.x * s;
    const float scaled_top    = kb.y * s;
    const float scaled_right  = (kb.x + kb.width) * s;
    const float scaled_bottom = (kb.y + kb.height) * s;
    // Top-left anchor → no negative spill.
    REQUIRE(scaled_left >= -0.01f);
    REQUIRE(scaled_top >= -0.01f);
    // Right/bottom within the box (small epsilon for float).
    REQUIRE(scaled_right <= bb.width + 0.01f);
    REQUIRE(scaled_bottom <= bb.height + 0.01f);

    // Release commits one undoable unit.
    MouseEvent release;
    release.position = {360, 140};
    release.is_down = true;
    overlay.handle_mouse_event(release);
    REQUIRE(history.undo_count() == 1);

    // Undo restores the original scale, transform-origin, and overflow.
    REQUIRE(history.undo());
    REQUIRE(box_ptr->scale() == Catch::Approx(1.0f));
    REQUIRE(box_ptr->transform_origin_x() == Catch::Approx(0.5f));
    REQUIRE(box_ptr->transform_origin_y() == Catch::Approx(0.5f));
    REQUIRE(box_ptr->overflow() == View::Overflow::visible);
}

// ── P3: Figma-style tool palette + inline text editing ──────────────────────
//
// planning/2026-05-21-wysiwyg-direct-manipulation-extension.md
// § "Future idea — Figma-style tool palette + inline text editing".
// V = Select tool (default), T = Text tool. The Text tool clicks a
// text-bearing element to edit its copy in place: Enter commits (live
// View text + a `text` content tweak, ONE undoable EditHistory unit),
// Esc cancels. The bare T tweak-panel toggle moved to Shift+T.

TEST_CASE("InspectorOverlay P3: set_tool / V / T switch tools",
          "[inspect][overlay][phase3][tools]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorOverlay overlay(root);
    overlay.set_active(true);

    // Default is Select.
    REQUIRE(overlay.tool() == InspectorOverlay::Tool::select);

    // T → Text tool.
    KeyEvent t;
    t.key = KeyCode::t;
    t.is_down = true;
    REQUIRE(overlay.handle_key_event(t));
    REQUIRE(overlay.tool() == InspectorOverlay::Tool::text);

    // V → back to Select tool.
    KeyEvent v;
    v.key = KeyCode::v;
    v.is_down = true;
    REQUIRE(overlay.handle_key_event(v));
    REQUIRE(overlay.tool() == InspectorOverlay::Tool::select);

    // set_tool API mirrors the keyboard.
    overlay.set_tool(InspectorOverlay::Tool::text);
    REQUIRE(overlay.tool() == InspectorOverlay::Tool::text);
    overlay.toggle_tool();
    REQUIRE(overlay.tool() == InspectorOverlay::Tool::select);
}

TEST_CASE("InspectorOverlay P3: Text tool click begins inline edit, not "
          "select-for-drag",
          "[inspect][overlay][phase3][text-tool]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    auto label = std::make_unique<Label>("Hello");
    label->set_anchor_id("figma:label-1");
    label->set_bounds({20, 20, 120, 30});
    auto* label_ptr = label.get();
    root.add_child(std::move(label));

    TweakStore store;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    // Drag handles ON to prove the Text tool click does NOT start a
    // resize/move even when those gestures are available.
    overlay.set_dragging_enabled(true);
    overlay.set_tool(InspectorOverlay::Tool::text);

    REQUIRE(InspectorOverlay::view_has_editable_text(label_ptr));

    // Click the label in Text-tool mode → begins editing, no move active.
    MouseEvent click;
    click.position = {40, 35};
    click.is_down = true;
    REQUIRE(overlay.handle_mouse_event(click));
    REQUIRE(overlay.text_editing());
    REQUIRE(overlay.text_edit_target() == label_ptr);
    REQUIRE(overlay.text_edit_buffer() == "Hello");
    // Selection points at the edit target (so props panel tracks it),
    // and the click did NOT start a move (no tweak emitted yet).
    REQUIRE(overlay.selected_view() == label_ptr);
    REQUIRE(store.count() == 0);
}

TEST_CASE("InspectorOverlay P3: typing + Enter commits text, emits `text` "
          "tweak, undoable",
          "[inspect][overlay][phase3][text-tool][undo]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    auto label = std::make_unique<Label>("Old");
    label->set_anchor_id("figma:label-1");
    label->set_bounds({20, 20, 120, 30});
    auto* label_ptr = label.get();
    root.add_child(std::move(label));

    TweakStore store;
    pulp::state::EditHistory history;
    history.set_coalesce(false);
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_edit_history(&history);
    overlay.set_tool(InspectorOverlay::Tool::text);

    // Begin an inline edit, clear the buffer, type a new copy.
    REQUIRE(overlay.begin_text_edit(label_ptr));

    // Backspace the whole "Old" buffer (3 chars), then type "New".
    KeyEvent bs;
    bs.key = KeyCode::backspace;
    bs.is_down = true;
    for (int i = 0; i < 3; ++i) REQUIRE(overlay.handle_key_event(bs));
    REQUIRE(overlay.text_edit_buffer().empty());
    REQUIRE(label_ptr->text().empty());  // live preview tracks the buffer

    TextInputEvent ti;
    ti.text = "New";
    REQUIRE(overlay.handle_text_input(ti));
    REQUIRE(overlay.text_edit_buffer() == "New");
    REQUIRE(label_ptr->text() == "New");  // live preview

    // Enter commits.
    KeyEvent enter;
    enter.key = KeyCode::enter;
    enter.is_down = true;
    REQUIRE(overlay.handle_key_event(enter));
    REQUIRE_FALSE(overlay.text_editing());

    // Live View keeps the new text; a `text` tweak was emitted; one undo.
    REQUIRE(label_ptr->text() == "New");
    auto tw = store.lookup("figma:label-1", "text");
    REQUIRE(tw.has_value());
    REQUIRE(tw->getString() == "New");
    REQUIRE(history.undo_count() == 1);
    REQUIRE(history.undo_description() == "edit-text");

    // Undo restores the original copy on BOTH the View and the store.
    REQUIRE(history.undo());
    REQUIRE(label_ptr->text() == "Old");
    REQUIRE_FALSE(store.lookup("figma:label-1", "text").has_value());

    // Redo re-applies.
    REQUIRE(history.redo());
    REQUIRE(label_ptr->text() == "New");
    REQUIRE(store.lookup("figma:label-1", "text")->getString() == "New");
}

TEST_CASE("InspectorOverlay P3: Esc cancels inline text edit, reverts copy",
          "[inspect][overlay][phase3][text-tool]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    auto label = std::make_unique<Label>("Keep");
    label->set_anchor_id("figma:label-1");
    label->set_bounds({20, 20, 120, 30});
    auto* label_ptr = label.get();
    root.add_child(std::move(label));

    TweakStore store;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_tool(InspectorOverlay::Tool::text);

    REQUIRE(overlay.begin_text_edit(label_ptr));
    TextInputEvent ti;
    ti.text = "X";
    overlay.handle_text_input(ti);
    REQUIRE(label_ptr->text() == "KeepX");  // live preview mutated it

    KeyEvent esc;
    esc.key = KeyCode::escape;
    esc.is_down = true;
    REQUIRE(overlay.handle_key_event(esc));
    REQUIRE_FALSE(overlay.text_editing());

    // Original copy restored, no tweak emitted.
    REQUIRE(label_ptr->text() == "Keep");
    REQUIRE(store.count() == 0);
}

// ── WYSIWYG T2 — in-place text-edit caret + selection ───────────────────────
//
// The inline edit keeps the LIVE View text but layers TextEditor-style caret /
// selection / clipboard LOGIC on top. These tests cover the spec's required
// surface: caret movement, select-all, and paste, plus shift-select + insert-
// at-caret and the caret/selection paint overlay.

namespace {
// Build a Label-in-root + an active Text-tool overlay editing it. Returns the
// label pointer via out-param; the overlay is constructed by the caller.
std::unique_ptr<View> t2_make_label(const std::string& text, Label** out) {
    auto label = std::make_unique<Label>(text);
    label->set_anchor_id("figma:t2");
    label->set_bounds({20, 20, 200, 30});
    *out = label.get();
    return label;
}
}  // namespace

TEST_CASE("InspectorOverlay T2: caret moves with arrows and inserts mid-text",
          "[inspect][overlay][wysiwyg][t2]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    Label* label = nullptr;
    root.add_child(t2_make_label("abc", &label));

    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tool(InspectorOverlay::Tool::text);
    REQUIRE(overlay.begin_text_edit(label));

    // begin_text_edit seeds the caret at the end (after "abc").
    REQUIRE(overlay.text_caret() == 3);
    REQUIRE_FALSE(overlay.text_has_selection());

    // Two LEFT arrows → caret between 'a' and 'b' (index 1).
    KeyEvent left; left.key = KeyCode::left; left.is_down = true;
    REQUIRE(overlay.handle_key_event(left));
    REQUIRE(overlay.handle_key_event(left));
    REQUIRE(overlay.text_caret() == 1);

    // Type "X" → inserted at the caret, not appended.
    TextInputEvent ti; ti.text = "X";
    REQUIRE(overlay.handle_text_input(ti));
    REQUIRE(overlay.text_edit_buffer() == "aXbc");
    REQUIRE(label->text() == "aXbc");        // live preview
    REQUIRE(overlay.text_caret() == 2);      // caret advanced past the insert

    // RIGHT arrow to the end, type "!" → appended.
    KeyEvent right; right.key = KeyCode::right; right.is_down = true;
    REQUIRE(overlay.handle_key_event(right));
    REQUIRE(overlay.handle_key_event(right));
    ti.text = "!";
    REQUIRE(overlay.handle_text_input(ti));
    REQUIRE(overlay.text_edit_buffer() == "aXbc!");
}

TEST_CASE("InspectorOverlay T2: primary-select selects all, then paste replaces it",
          "[inspect][overlay][wysiwyg][t2]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    Label* label = nullptr;
    root.add_child(t2_make_label("hello", &label));

    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tool(InspectorOverlay::Tool::text);
    REQUIRE(overlay.begin_text_edit(label));

    // The platform-primary modifier selects the whole buffer.
#if defined(__APPLE__)
    const auto primary_modifier = pulp::view::kModCmd;
#else
    const auto primary_modifier = pulp::view::kModCtrl;
#endif
    KeyEvent sel_all;
    sel_all.key = KeyCode::a;
    sel_all.is_down = true;
    sel_all.modifiers = primary_modifier;
    REQUIRE(overlay.handle_key_event(sel_all));
    REQUIRE(overlay.text_has_selection());
    auto [lo, hi] = overlay.text_selection();
    REQUIRE(lo == 0);
    REQUIRE(hi == 5);

    // Seed the clipboard, then paste — replaces the full selection.
    if (!pulp::platform::Clipboard::set_text("WORLD")) {
        SUCCEED("native clipboard unavailable on this platform");
        return;
    }
    KeyEvent paste;
    paste.key = KeyCode::v;
    paste.is_down = true;
    paste.modifiers = primary_modifier;
    REQUIRE(overlay.handle_key_event(paste));
    REQUIRE(overlay.text_edit_buffer() == "WORLD");
    REQUIRE(label->text() == "WORLD");
    REQUIRE_FALSE(overlay.text_has_selection());  // selection collapsed
    REQUIRE(overlay.text_caret() == 5);
}

TEST_CASE("InspectorOverlay T2: shift-arrow extends selection; typing replaces",
          "[inspect][overlay][wysiwyg][t2]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    Label* label = nullptr;
    root.add_child(t2_make_label("abcd", &label));

    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tool(InspectorOverlay::Tool::text);
    REQUIRE(overlay.begin_text_edit(label));  // caret at 4 (end)

    // Shift+Left twice selects the last two chars ("cd").
    KeyEvent shleft;
    shleft.key = KeyCode::left;
    shleft.is_down = true;
    shleft.modifiers = pulp::view::kModShift;
    REQUIRE(overlay.handle_key_event(shleft));
    REQUIRE(overlay.handle_key_event(shleft));
    REQUIRE(overlay.text_has_selection());
    {
        auto [lo, hi] = overlay.text_selection();
        REQUIRE(lo == 2);
        REQUIRE(hi == 4);
    }

    // Typing over the selection replaces it.
    TextInputEvent ti; ti.text = "Z";
    REQUIRE(overlay.handle_text_input(ti));
    REQUIRE(overlay.text_edit_buffer() == "abZ");
    REQUIRE_FALSE(overlay.text_has_selection());
}

TEST_CASE("InspectorOverlay T2: caret + selection paint as a light overlay",
          "[inspect][overlay][wysiwyg][t2]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    Label* label = nullptr;
    root.add_child(t2_make_label("abcd", &label));
    root.layout_children();

    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_manipulate_only(true);  // P1 manipulate layer (ui-preview path)
    overlay.set_tool(InspectorOverlay::Tool::text);
    REQUIRE(overlay.begin_text_edit(label));
    overlay.text_select_all();

    // Painting must not crash and must emit fill_rect commands for the caret /
    // selection band overlay (the light overlay, not an input box).
    pulp::canvas::RecordingCanvas canvas;
    overlay.paint(canvas);
    std::size_t rects = 0;
    for (const auto& c : canvas.commands())
        if (c.type == pulp::canvas::DrawCommand::Type::fill_rect) ++rects;
    REQUIRE(rects >= 1);  // selection band and/or caret line drawn
}

// ── WYSIWYG QA follow-ups — maintainer-reported live bugs ───────────────────
//
// These pin the exact scenarios the maintainer reported from the running app
// (the headless suite passed but the live behavior was wrong / chrome was
// wrong). BUGs 1–3 lock the in-place text-edit semantics at the precise
// boundary the QA report named; BUG 4 locks the select-chrome predicate.

TEST_CASE("InspectorOverlay QA BUG1: typed char lands AT the caret, not the end",
          "[inspect][overlay][wysiwyg][qa][bug1]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    Label* label = nullptr;
    root.add_child(t2_make_label("polywave", &label));

    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tool(InspectorOverlay::Tool::text);
    REQUIRE(overlay.begin_text_edit(label));
    REQUIRE(overlay.text_caret() == 8);  // seeded at end of "polywave"

    // Put the caret between 'y' and 'w' (after "poly", index 4) the way the
    // maintainer did: four LEFT arrows from the end.
    KeyEvent left; left.key = KeyCode::left; left.is_down = true;
    for (int i = 0; i < 4; ++i) REQUIRE(overlay.handle_key_event(left));
    REQUIRE(overlay.text_caret() == 4);

    // Type 't' — it must land AT the caret ("polytwave"), NOT appended
    // ("polywavet" was the bug), and the caret must advance past the insert.
    TextInputEvent ti; ti.text = "t";
    REQUIRE(overlay.handle_text_input(ti));
    REQUIRE(overlay.text_edit_buffer() == "polytwave");
    REQUIRE(label->text() == "polytwave");   // live View text updated too
    REQUIRE(overlay.text_caret() == 5);      // caret advanced past 't'
    REQUIRE_FALSE(overlay.text_edit_buffer() == "polywavet");  // not appended
}

TEST_CASE("InspectorOverlay QA BUG2: Delete removes only the SELECTION, not all",
          "[inspect][overlay][wysiwyg][qa][bug2]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    Label* label = nullptr;
    root.add_child(t2_make_label("polywave", &label));

    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tool(InspectorOverlay::Tool::text);
    REQUIRE(overlay.begin_text_edit(label));

    // Caret at end (8). Home, then Shift+Right x4 selects "poly" [0,4).
    overlay.text_move_home(/*extend=*/false);
    KeyEvent shright; shright.key = KeyCode::right; shright.is_down = true;
    shright.modifiers = pulp::view::kModShift;
    for (int i = 0; i < 4; ++i) REQUIRE(overlay.handle_key_event(shright));
    REQUIRE(overlay.text_has_selection());
    {
        auto [lo, hi] = overlay.text_selection();
        REQUIRE(lo == 0);
        REQUIRE(hi == 4);
    }

    // Backspace removes EXACTLY "poly" — leaving "wave", not wiping the whole
    // string (the reported bug erased everything).
    KeyEvent bs; bs.key = KeyCode::backspace; bs.is_down = true;
    REQUIRE(overlay.handle_key_event(bs));
    REQUIRE(overlay.text_edit_buffer() == "wave");
    REQUIRE(label->text() == "wave");
    REQUIRE_FALSE(overlay.text_has_selection());  // collapsed to the cut point
    REQUIRE(overlay.text_caret() == 0);

    // Forward Delete with NO selection removes one char at the caret ('w').
    KeyEvent del; del.key = KeyCode::delete_; del.is_down = true;
    REQUIRE(overlay.handle_key_event(del));
    REQUIRE(overlay.text_edit_buffer() == "ave");
}

TEST_CASE("InspectorOverlay QA BUG3: committed text edit is one undoable unit",
          "[inspect][overlay][wysiwyg][qa][bug3][undo]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    Label* label = nullptr;
    root.add_child(t2_make_label("polywave", &label));

    TweakStore store;
    pulp::state::EditHistory history;
    history.set_coalesce(false);
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_edit_history(&history);
    overlay.set_tool(InspectorOverlay::Tool::text);
    REQUIRE(overlay.begin_text_edit(label));

    // Edit: caret at 4, insert 't' -> "polytwave".
    overlay.text_move_home(false);
    overlay.text_move_caret(4, /*extend=*/false);
    TextInputEvent ti; ti.text = "t";
    REQUIRE(overlay.handle_text_input(ti));
    REQUIRE(label->text() == "polytwave");

    // Commit (Enter) — exactly ONE EditHistory entry restores the prior text.
    KeyEvent enter; enter.key = KeyCode::enter; enter.is_down = true;
    REQUIRE(overlay.handle_key_event(enter));
    REQUIRE_FALSE(overlay.text_editing());
    REQUIRE(history.undo_count() == 1);
    REQUIRE(history.undo_description() == "edit-text");
    REQUIRE(store.lookup("figma:t2", "text")->getString() == "polytwave");

    // Undo restores the ORIGINAL text on both the View and the store (the
    // reported bug: Cmd+Z undid moves but not text edits).
    REQUIRE(history.undo());
    REQUIRE(label->text() == "polywave");
    REQUIRE_FALSE(store.lookup("figma:t2", "text").has_value());

    // Redo re-applies the edit.
    REQUIRE(history.redo());
    REQUIRE(label->text() == "polytwave");
    REQUIRE(store.lookup("figma:t2", "text")->getString() == "polytwave");
}

TEST_CASE("InspectorOverlay QA BUG4: text-edit shows subtle outline, no handles",
          "[inspect][overlay][wysiwyg][qa][bug4]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    Label* label = nullptr;
    root.add_child(t2_make_label("polywave", &label));
    root.layout_children();

    InspectorOverlay overlay(root);
    overlay.set_active(true);

    // ── Select tool: selecting an element shows the orange box + handles ──
    overlay.set_tool(InspectorOverlay::Tool::select);
    overlay.set_dragging_enabled(true);          // handles opt-in (D)
    overlay.set_selected_view(label);
    REQUIRE_FALSE(overlay.text_editing());
    REQUIRE(overlay.selection_shows_resize_handles());          // BUG 4 predicate
    REQUIRE_FALSE(overlay.selection_uses_subtle_edit_outline());
    {
        pulp::canvas::RecordingCanvas canvas;
        overlay.paint(canvas);
        std::size_t handle_rects = 0;
        bool saw_orange_stroke = false;
        for (const auto& c : canvas.commands()) {
            // Handles are 8×8 filled squares (half-side 4 -> w==h==8).
            if (c.type == pulp::canvas::DrawCommand::Type::fill_rect &&
                std::abs(c.f[2] - 8.0f) < 0.01f &&
                std::abs(c.f[3] - 8.0f) < 0.01f)
                ++handle_rects;
            if (c.type == pulp::canvas::DrawCommand::Type::set_stroke_color &&
                c.color.r > 0.9f && c.color.g > 0.4f && c.color.g < 0.6f &&
                c.color.b < 0.1f)
                saw_orange_stroke = true;  // kSelectedStroke (orange)
        }
        REQUIRE(handle_rects == 8);     // 4 corners + 4 edge midpoints
        REQUIRE(saw_orange_stroke);
    }

    // ── Text tool / active edit: subtle thin-blue outline, NO handles ──
    overlay.set_tool(InspectorOverlay::Tool::text);
    REQUIRE(overlay.begin_text_edit(label));
    REQUIRE(overlay.text_editing());
    REQUIRE(overlay.selection_uses_subtle_edit_outline());      // BUG 4 predicate
    REQUIRE_FALSE(overlay.selection_shows_resize_handles());    // handles suppressed
    {
        pulp::canvas::RecordingCanvas canvas;
        overlay.paint(canvas);
        std::size_t handle_rects = 0;
        bool saw_blue_stroke = false;
        for (const auto& c : canvas.commands()) {
            if (c.type == pulp::canvas::DrawCommand::Type::fill_rect &&
                std::abs(c.f[2] - 8.0f) < 0.01f &&
                std::abs(c.f[3] - 8.0f) < 0.01f)
                ++handle_rects;
            if (c.type == pulp::canvas::DrawCommand::Type::set_stroke_color &&
                c.color.b > 0.9f && c.color.r < 0.4f)
                saw_blue_stroke = true;  // kHighlightStroke (blue) outline
        }
        REQUIRE(handle_rects == 0);     // no resize handles mid-edit
        REQUIRE(saw_blue_stroke);       // thin blue outline drawn
    }
}

TEST_CASE("InspectorOverlay P3: Select tool clicks still select (unchanged)",
          "[inspect][overlay][phase3][text-tool][regression]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    auto label = std::make_unique<Label>("Click me");
    label->set_anchor_id("figma:label-1");
    label->set_bounds({20, 20, 120, 30});
    auto* label_ptr = label.get();
    root.add_child(std::move(label));

    InspectorOverlay overlay(root);
    overlay.set_active(true);
    // Default tool is Select.

    MouseEvent click;
    click.position = {40, 35};
    click.is_down = true;
    REQUIRE(overlay.handle_mouse_event(click));
    // Select tool selects (does NOT begin a text edit).
    REQUIRE(overlay.selected_view() == label_ptr);
    REQUIRE_FALSE(overlay.text_editing());
}

TEST_CASE("InspectorOverlay P3: Text tool only edits text-bearing views",
          "[inspect][overlay][phase3][text-tool]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    // A plain (non-text) View.
    auto box = std::make_unique<View>();
    box->set_anchor_id("figma:box-1");
    box->set_bounds({20, 20, 120, 60});
    auto* box_ptr = box.get();
    root.add_child(std::move(box));

    REQUIRE_FALSE(InspectorOverlay::view_has_editable_text(box_ptr));

    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tool(InspectorOverlay::Tool::text);

    // begin_text_edit refuses a non-text view.
    REQUIRE_FALSE(overlay.begin_text_edit(box_ptr));
    REQUIRE_FALSE(overlay.text_editing());

    // A Text-tool click on a non-text element is a consumed no-op (no edit).
    MouseEvent click;
    click.position = {40, 40};
    click.is_down = true;
    REQUIRE(overlay.handle_mouse_event(click));
    REQUIRE_FALSE(overlay.text_editing());
}

TEST_CASE("InspectorOverlay P3: TextEditor is also editable via the Text tool",
          "[inspect][overlay][phase3][text-tool]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    auto editor = std::make_unique<pulp::view::TextEditor>();
    editor->set_text("seed");
    editor->set_anchor_id("figma:editor-1");
    editor->set_bounds({20, 20, 120, 30});
    auto* editor_ptr = editor.get();
    root.add_child(std::move(editor));

    TweakStore store;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_tool(InspectorOverlay::Tool::text);

    REQUIRE(overlay.begin_text_edit(editor_ptr));
    REQUIRE(overlay.text_edit_buffer() == "seed");

    TextInputEvent ti;
    ti.text = "!";
    overlay.handle_text_input(ti);
    REQUIRE(editor_ptr->text() == "seed!");

    REQUIRE(overlay.commit_text_edit());
    REQUIRE(store.lookup("figma:editor-1", "text")->getString() == "seed!");
}

// ── WYSIWYG P5 FIX 1 — text_edit_target_ raw-pointer UAF guard ───────────────
//
// text_edit_target_ is a raw View*. If the edited Label/TextEditor is destroyed
// mid-edit (e.g. a live React tree rebuild), the rebuild-validation must clear
// the WHOLE text-edit state without touching the freed view, and every text op
// (handle_text_input / Backspace / commit / cancel) must no-op when the target
// is no longer reachable from the root — never deref freed memory.
TEST_CASE("InspectorOverlay P5: text edit survives target destroyed mid-edit",
          "[inspect][overlay][p5][text-tool][uaf][issue-wysiwyg-p5]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    auto label = std::make_unique<Label>("Editing");
    label->set_anchor_id("figma:label-1");
    label->set_bounds({20, 20, 120, 30});
    auto* label_ptr = label.get();
    root.add_child(std::move(label));

    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tool(InspectorOverlay::Tool::text);

    REQUIRE(overlay.begin_text_edit(label_ptr));
    REQUIRE(overlay.text_editing());
    REQUIRE(overlay.text_edit_target() == label_ptr);

    // Simulate a live React tree rebuild that destroys the edited Label:
    // remove + free it, then run the inspector's rebuild-validation. The
    // rebuild must drop the dangling text_edit_target_ without dereferencing
    // the freed view.
    auto removed = root.remove_child(label_ptr);  // owns + frees on scope exit
    removed.reset();                               // free now
    // paint() runs rebuild_flat_tree(), which contains the P5 FIX 1 guard.
    pulp::canvas::RecordingCanvas rebuild_canvas;
    REQUIRE_NOTHROW(overlay.paint(rebuild_canvas));

    // Edit state fully cleared — no deref of the freed Label.
    REQUIRE_FALSE(overlay.text_editing());
    REQUIRE(overlay.text_edit_target() == nullptr);
    REQUIRE(overlay.text_edit_buffer().empty());

    // Subsequent text ops are safe no-ops (would have deref'd the freed view).
    TextInputEvent ti;
    ti.text = "x";
    REQUIRE_FALSE(overlay.handle_text_input(ti));

    KeyEvent bs;
    bs.key = KeyCode::backspace;
    bs.is_down = true;
    // Backspace is consumed (text tool owns it) but must not deref.
    // (Not editing now, so the text-tool branch is skipped — just assert no crash.)
    (void)overlay.handle_key_event(bs);

    REQUIRE_FALSE(overlay.commit_text_edit());
    overlay.cancel_text_edit();  // no-op, no deref
    REQUIRE_FALSE(overlay.text_editing());
}

// Same UAF guard, exercised on the commit/cancel paths directly: if the target
// is removed but the rebuild-validation has NOT yet run (e.g. a click-away
// commit races a tree mutation), commit/cancel must detect the unreachable
// target and clear state instead of writing to freed memory.
TEST_CASE("InspectorOverlay P5: commit/cancel no-op when target left the tree",
          "[inspect][overlay][p5][text-tool][uaf][issue-wysiwyg-p5]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    auto label = std::make_unique<Label>("Bye");
    label->set_anchor_id("figma:label-1");
    label->set_bounds({20, 20, 120, 30});
    auto* label_ptr = label.get();
    root.add_child(std::move(label));

    TweakStore store;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.set_tool(InspectorOverlay::Tool::text);

    REQUIRE(overlay.begin_text_edit(label_ptr));
    auto removed = root.remove_child(label_ptr);
    removed.reset();  // free; overlay.text_edit_target_ now dangles

    // commit must NOT deref the freed target; it clears state + emits nothing.
    REQUIRE_FALSE(overlay.commit_text_edit());
    REQUIRE_FALSE(overlay.text_editing());
    REQUIRE(store.count() == 0);
}

// ── WYSIWYG P5 FIX 2 — install_inspector_hooks installs the text hook ────────
//
// install_inspector_hooks() must install the inline-text-edit hook (and the
// cursor hook) so the STANDALONE host can deliver typed characters into a
// Text-tool edit. The hook is root-gated like the mouse/cursor hooks: text
// from a secondary window's root is rejected; nullptr (legacy/headless) runs.
TEST_CASE("InspectorOverlay P5: install_inspector_hooks installs a root-gated "
          "text hook",
          "[inspect][overlay][p5][text-tool][hooks][issue-wysiwyg-p5]") {
    View inspected_root;
    inspected_root.set_bounds({0, 0, 400, 300});
    auto label = std::make_unique<Label>("Type here");
    label->set_anchor_id("figma:label-1");
    label->set_bounds({20, 20, 120, 30});
    auto* label_ptr = label.get();
    inspected_root.add_child(std::move(label));

    // A separate root standing in for the floating InspectorWindow.
    View other_root;
    other_root.set_bounds({0, 0, 340, 300});

    InspectorOverlay overlay(inspected_root);
    overlay.set_active(true);
    overlay.set_tool(InspectorOverlay::Tool::text);
    REQUIRE(overlay.begin_text_edit(label_ptr));

    install_inspector_hooks(overlay);

    // Text routed for the INSPECTED root is consumed + appended to the buffer.
    TextInputEvent ti;
    ti.text = "A";
    REQUIRE(View::call_inspector_text_hook(ti, &inspected_root));
    REQUIRE(overlay.text_edit_buffer() == "Type hereA");

    // Text routed for a DIFFERENT window's root is rejected (not consumed),
    // and does NOT mutate the canvas overlay's edit buffer.
    TextInputEvent ti2;
    ti2.text = "Z";
    REQUIRE_FALSE(View::call_inspector_text_hook(ti2, &other_root));
    REQUIRE(overlay.text_edit_buffer() == "Type hereA");

    // nullptr root (legacy/headless caller) runs unconditionally.
    TextInputEvent ti3;
    ti3.text = "B";
    REQUIRE(View::call_inspector_text_hook(ti3, nullptr));
    REQUIRE(overlay.text_edit_buffer() == "Type hereAB");

    // The cursor hook is installed too (parity): returns -1 (defer) off the
    // selection rather than the no-hook sentinel; here it simply must not crash
    // and respects the root gate.
    MouseEvent mv;
    mv.position = {200, 200};
    (void)View::call_inspector_cursor_hook(mv, &inspected_root);
    REQUIRE(View::call_inspector_cursor_hook(mv, &other_root) == -1);

    // Clean up the global hooks so later tests aren't affected.
    g_active_inspector = nullptr;
    View::set_inspector_paint_hook({});
    View::set_inspector_key_hook({});
    View::set_inspector_mouse_hook({});
    View::set_inspector_text_hook({});
    View::set_inspector_cursor_hook({});
}

// ── WYSIWYG P5 regression — typing V/T mid-edit must NOT flip the tool ────────
//
// While an inline text edit is in progress, the V/T tool shortcuts are gated
// off (the Text tool owns the keyboard). The JUST-fixed key-hook also returns
// false for non-control keys so insertText delivers the character — assert both:
// the tool stays Text, and Esc cancels the edit and re-enables the V/T flip.
TEST_CASE("InspectorOverlay P5: V/T do not flip tool mid-edit; Esc re-enables",
          "[inspect][overlay][p5][text-tool][regression][issue-wysiwyg-p5]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    auto label = std::make_unique<Label>("Hi");
    label->set_anchor_id("figma:label-1");
    label->set_bounds({20, 20, 120, 30});
    auto* label_ptr = label.get();
    root.add_child(std::move(label));

    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tool(InspectorOverlay::Tool::text);
    REQUIRE(overlay.begin_text_edit(label_ptr));
    REQUIRE(overlay.tool() == InspectorOverlay::Tool::text);

    // A 'v' key-down mid-edit must NOT flip to Select — it is character input,
    // so the key-hook returns false (lets insertText through) and the tool
    // stays Text. (The control keys Enter/Esc/Backspace are handled above the
    // tool shortcuts; everything else falls through as character input.)
    KeyEvent v;
    v.key = KeyCode::v;
    v.is_down = true;
    REQUIRE_FALSE(overlay.handle_key_event(v));
    REQUIRE(overlay.tool() == InspectorOverlay::Tool::text);
    REQUIRE(overlay.text_editing());

    // A 't' key-down mid-edit likewise does not toggle the tweak panel / tool.
    KeyEvent t;
    t.key = KeyCode::t;
    t.is_down = true;
    REQUIRE_FALSE(overlay.handle_key_event(t));
    REQUIRE(overlay.tool() == InspectorOverlay::Tool::text);
    REQUIRE(overlay.text_editing());

    // Esc cancels the edit; afterwards the bare V/T shortcuts flip the tool.
    KeyEvent esc;
    esc.key = KeyCode::escape;
    esc.is_down = true;
    REQUIRE(overlay.handle_key_event(esc));
    REQUIRE_FALSE(overlay.text_editing());

    REQUIRE(overlay.handle_key_event(v));  // now consumed → Select
    REQUIRE(overlay.tool() == InspectorOverlay::Tool::select);
    REQUIRE(overlay.handle_key_event(t));  // now consumed → Text
    REQUIRE(overlay.tool() == InspectorOverlay::Tool::text);
}

// ── WYSIWYG P5 FIX 4 — committed text re-shapes (laid-out width updates) ──────
//
// Editing a Label's text must re-run the PreText-style TextShaper measurement
// so the laid-out width tracks the new copy (not stale at the old advance).
// Label::set_text marks layout dirty; the Yoga measure callback re-runs
// TextShaper::prepare keyed on the current text, so a relayout produces a
// different width for a clearly-different-length string.
TEST_CASE("InspectorOverlay P5: editing text re-shapes — laid-out width updates",
          "[inspect][overlay][p5][text-tool][reshape][issue-wysiwyg-p5]") {
    View root;
    root.set_bounds({0, 0, 600, 200});
    root.flex().direction = FlexDirection::row;

    // No fixed width on the Label so its laid-out width is the shaped intrinsic
    // width (intrinsic_width() → TextShaper::prepare(text_, ...)).
    auto label = std::make_unique<Label>("Hi");
    label->set_anchor_id("figma:label-1");
    auto* label_ptr = label.get();
    root.add_child(std::move(label));

    root.layout_children();
    const float narrow_w = label_ptr->bounds().width;
    REQUIRE(narrow_w > 0.0f);

    // Edit the copy to a much longer string via the inspector's commit path.
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tool(InspectorOverlay::Tool::text);
    REQUIRE(overlay.begin_text_edit(label_ptr));
    overlay.set_editable_text_of(
        label_ptr, "This is a much longer label string that must re-shape");

    // set_text marked layout dirty; re-run layout. The new shaped width must be
    // strictly wider — proving the shaper re-measured the new copy, not a stale
    // cached width for "Hi".
    root.layout_children();
    const float wide_w = label_ptr->bounds().width;
    REQUIRE(wide_w > narrow_w);

    // And shrinking back re-shapes narrower again (cache is per-text, not
    // monotonic).
    overlay.set_editable_text_of(label_ptr, "Hi");
    root.layout_children();
    REQUIRE(label_ptr->bounds().width == Catch::Approx(narrow_w).margin(0.5f));

    overlay.cancel_text_edit();
}

// ── P3: InspectorWindow tool strip ──────────────────────────────────────────
//
// The window header carries a Figma-style tool strip wired to the overlay
// tool BOTH ways via the host: a strip-button click fires on_tool_picked
// (host → overlay.set_tool); a keyboard V/T flip is mirrored back by the
// host calling set_active_tool so the strip highlights the active tool.

TEST_CASE("InspectorWindow P3: tool strip is present and active-tool "
          "round-trips",
          "[inspect][window][phase3][tools]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorWindow window(root);

    // Strip header + tab panel.
    REQUIRE(window.child_count() == 2);
    REQUIRE(window.active_tool() == 0);  // Select default

    // set_active_tool mirrors the overlay tool (host → strip).
    window.set_active_tool(1);
    REQUIRE(window.active_tool() == 1);
    window.set_active_tool(0);
    REQUIRE(window.active_tool() == 0);

    // on_tool_picked fires the host callback (strip click → overlay).
    int picked = -1;
    window.on_tool_picked = [&](int idx) { picked = idx; };
    // Drive a click into the strip's first child (the ToolStrip header).
    // The strip is child 0; clicking the second button (Text) should fire
    // on_tool_picked(1). Lay out first so the strip + button rects are valid.
    window.set_bounds({0, 0, 400, 600});
    window.layout_children();

    View* strip = window.child_at(0);
    REQUIRE(strip != nullptr);
    const float strip_mid_y = strip->bounds().height * 0.5f;
    MouseEvent click_text;
    // Second button center: strip x=6 + 1*(40+4) + 40/2 = 70, y mid (local).
    click_text.position = {70, strip_mid_y};
    click_text.is_down = true;
    strip->on_mouse_event(click_text);
    REQUIRE(picked == 1);

    MouseEvent click_select;
    click_select.position = {26, strip_mid_y};  // first button center
    click_select.is_down = true;
    strip->on_mouse_event(click_select);
    REQUIRE(picked == 0);

    // The strip paints without crashing for both active states.
    window.set_active_tool(1);
    pulp::canvas::RecordingCanvas canvas;
    window.paint_all(canvas);
    REQUIRE(canvas.command_count() > 0);
}

// WYSIWYG T1 — the V/T tooltips. The strip paints an inline "Select (V)" /
// "Text (T)" tooltip keyed on the hovered button. The bug was that a
// mouse-MOVE (hover, no button down) never reached the strip's hit-tracking:
// simulate_hover() set hovered_ via on_mouse_enter (no position), so
// hovered_button_ stayed -1 and the tooltip never showed. T1 adds a positioned
// on_hover_move() hook that simulate_hover() now delivers to the hit target.
TEST_CASE("InspectorWindow T1: hover over a tool button shows its tooltip",
          "[inspect][window][wysiwyg][t1]") {
    auto helper = [](int button_index, const char* expect_tip,
                     const char* not_tip) {
        View root;
        root.set_bounds({0, 0, 400, 300});
        InspectorWindow window(root);
        window.set_bounds({0, 0, 400, 600});
        window.layout_children();

        View* strip = window.child_at(0);
        REQUIRE(strip != nullptr);

        // Button centers in ROOT space mirror the click test above:
        //   button 0 (Select) ≈ x=26, button 1 (Text) ≈ x=70.
        const float strip_y = strip->bounds().y + strip->bounds().height * 0.5f;
        const float bx = button_index == 0 ? 26.0f : 70.0f;

        // Before any hover the tooltip is absent.
        {
            pulp::canvas::RecordingCanvas canvas;
            window.paint_all(canvas);
            bool tip_drawn = false;
            for (const auto& c : canvas.commands())
                if (c.type == pulp::canvas::DrawCommand::Type::fill_text &&
                    c.text == expect_tip)
                    tip_drawn = true;
            REQUIRE_FALSE(tip_drawn);
        }

        // Drive a hover over the button via the window root — exactly the path
        // the platform host's mouseMoved: handler uses. T1 makes this deliver
        // on_hover_move() to the strip so hovered_button_ is set.
        window.simulate_hover({bx, strip_y});

        pulp::canvas::RecordingCanvas canvas;
        window.paint_all(canvas);
        bool expect_drawn = false;
        bool other_drawn = false;
        for (const auto& c : canvas.commands()) {
            if (c.type != pulp::canvas::DrawCommand::Type::fill_text) continue;
            if (c.text == expect_tip) expect_drawn = true;
            if (c.text == not_tip) other_drawn = true;
        }
        REQUIRE(expect_drawn);        // the hovered button's tooltip shows
        REQUIRE_FALSE(other_drawn);   // and only that one
    };

    SECTION("Select button → 'Select (V)'") {
        helper(0, "Select (V)", "Text (T)");
    }
    SECTION("Text button → 'Text (T)'") {
        helper(1, "Text (T)", "Select (V)");
    }
}

// ── WYSIWYG caret-x — T2 regression ─────────────────────────────────────────
//
// The inspector text-edit overlay must draw the caret + selection band on the
// EXACT glyphs the Label paints. The painter resolves INHERITED letter-spacing
// (a parent's value when the Label sets none) and shifts the draw origin for
// center / right alignment. A pre-fix overlay re-measured with the Label's OWN
// fields (spacing 0) and anchored at the box's left edge, so a letter-spaced,
// center/right-aligned label drew the caret a glyph early and the band offset
// from the text. Label::text_edit_metrics now factors the same resolver, so:
//
//   - inherited letter-spacing widens caret_x_by_byte (end ≈ shaped width),
//   - caret_x_by_byte is monotonic non-decreasing,
//   - center / right alignment shifts local_text_left so the band tracks the
//     glyphs, and
//   - the overlay paints the selection band at the shifted origin.
TEST_CASE("WYSIWYG caret: inherited letter-spacing + alignment shift the band",
          "[inspect][overlay][wysiwyg][caret][issue-wysiwyg-caret-x]") {
    constexpr float kCharW = SpacingCanvas::kCharW;
    const std::string kText = "ENVELOPE";  // 8 ASCII glyphs
    const float kSpacing = 4.0f;

    // root → parent (sets INHERITABLE letter-spacing) → label (own spacing 0).
    auto make_tree = [&](LabelAlign align, Label*& out_label) {
        auto root = std::make_unique<View>();
        root->set_bounds({0, 0, 600, 200});
        auto parent = std::make_unique<View>();
        parent->set_bounds({0, 0, 600, 200});
        parent->set_inheritable_letter_spacing(kSpacing);
        auto label = std::make_unique<Label>(kText);
        label->set_bounds({0, 0, 400, 40});   // box wider than the shaped text
        label->set_text_align(align);
        out_label = label.get();
        parent->add_child(std::move(label));
        root->add_child(std::move(parent));
        return root;
    };

    // Expected shaped width under the inherited 4px tracking:
    //   8 glyphs × 10px + 7 gaps × 4px = 80 + 28 = 108.
    const float expected_w =
        static_cast<float>(kText.size()) * kCharW
        + static_cast<float>(kText.size() - 1) * kSpacing;

    SECTION("metrics: inherited spacing + monotonic + left origin") {
        Label* label = nullptr;
        auto root = make_tree(LabelAlign::left, label);
        SpacingCanvas canvas;
        const auto m = label->text_edit_metrics(canvas, kText);

        // The canvas saw the INHERITED spacing (not the Label's own 0).
        REQUIRE(canvas.active_letter_spacing() == Catch::Approx(kSpacing));
        REQUIRE(m.caret_x_by_byte.size() == kText.size() + 1);
        // Monotonic non-decreasing.
        for (std::size_t i = 1; i < m.caret_x_by_byte.size(); ++i)
            REQUIRE(m.caret_x_by_byte[i] >= m.caret_x_by_byte[i - 1]);
        // End caret == shaped width (would be 8×7=56 if spacing were dropped,
        // or 80 if only glyph advances counted — both wrong).
        REQUIRE(m.caret_x_by_byte.back() == Catch::Approx(expected_w));
        REQUIRE(m.caret_x_by_byte.front() == Catch::Approx(0.0f));
        // Left-aligned → text starts at the box's left edge.
        REQUIRE(m.local_text_left == Catch::Approx(0.0f));
    }

    SECTION("metrics: center alignment shifts the origin") {
        Label* label = nullptr;
        auto root = make_tree(LabelAlign::center, label);
        SpacingCanvas canvas;
        const auto m = label->text_edit_metrics(canvas, kText);
        // (box_w - shaped_w) / 2 = (400 - 108) / 2 = 146.
        REQUIRE(m.local_text_left == Catch::Approx((400.0f - expected_w) * 0.5f));
        REQUIRE(m.caret_x_by_byte.back() == Catch::Approx(expected_w));
    }

    SECTION("metrics: right alignment shifts the origin") {
        Label* label = nullptr;
        auto root = make_tree(LabelAlign::right, label);
        SpacingCanvas canvas;
        const auto m = label->text_edit_metrics(canvas, kText);
        // box_w - shaped_w = 400 - 108 = 292.
        REQUIRE(m.local_text_left == Catch::Approx(400.0f - expected_w));
    }

    SECTION("overlay: paints the selection band at the shifted (center) origin") {
        Label* label = nullptr;
        auto root = make_tree(LabelAlign::center, label);

        InspectorOverlay overlay(*root);
        overlay.set_active(true);
        REQUIRE(overlay.begin_text_edit(label));
        REQUIRE(overlay.text_editing());
        overlay.text_select_all();        // select the whole buffer
        REQUIRE(overlay.text_has_selection());

        SpacingCanvas canvas;
        overlay.paint(canvas);

        // The selection band is the translucent-accent fill_rect. Its x must be
        // the label's root x + the center-shifted text origin (146 here, since
        // the label sits at root x=0). A left-anchored (pre-fix) overlay would
        // draw it at x≈0.
        const float expected_left = (400.0f - expected_w) * 0.5f;
        bool found_band = false;
        for (const auto& c : canvas.commands()) {
            if (c.type != pulp::canvas::DrawCommand::Type::fill_rect) continue;
            // The selection band spans the full text (≈ shaped width wide).
            if (c.f[2] >= expected_w - 1.0f && c.f[2] <= expected_w + 2.0f) {
                REQUIRE(c.f[0] == Catch::Approx(expected_left).margin(1.0f));
                found_band = true;
            }
        }
        REQUIRE(found_band);
    }
}

// ── WYSIWYG caret-x — font-variant feature regression (sweep P2) ─────────────
//
// Label::paint() applies the CSS font-variant CSV as SkShaper OpenType feature
// tags (tabular-nums → tnum, small-caps → smcp, …) so HarfBuzz uses the
// alternate glyph advances. A pre-fix text_edit_metrics() set the font + letter-
// spacing but NOT the features, so for a font-variant label the caret/selection
// x was shaped from the DEFAULT advances and drifted from the rendered glyphs.
// The fix factors the feature setup into a shared apply_font_features() that
// BOTH paint() and text_edit_metrics() call.
//
// We prove it with a feature-aware spy canvas: when font features are active the
// per-glyph advance widens (mirroring how tabular-nums forces a wider, uniform
// advance). The metrics MUST measure with features applied, so caret_x_by_byte
// reflects the WIDER, feature-applied width — not the default-advance width.
namespace {
class FeatureWidthCanvas : public pulp::canvas::RecordingCanvas {
public:
    static constexpr float kBaseAdvance = 10.0f;
    static constexpr float kFeatureAdvance = 14.0f;  // wider when features active

    void set_font_features(std::vector<pulp::canvas::Canvas::FontFeature> f) override {
        (void)f;
        features_active_ = true;
        set_feature_calls_++;
    }
    void clear_font_features() override {
        features_active_ = false;
    }
    float measure_text(const std::string& text) override {
        const float adv = features_active_ ? kFeatureAdvance : kBaseAdvance;
        return static_cast<float>(text.size()) * adv;
    }
    bool features_active() const { return features_active_; }
    int set_feature_calls() const { return set_feature_calls_; }

private:
    bool features_active_ = false;
    int set_feature_calls_ = 0;
};
} // namespace

TEST_CASE("WYSIWYG caret: font-variant features apply to text_edit_metrics",
          "[inspect][overlay][wysiwyg][caret][issue-1737]") {
    const std::string kText = "12345";  // 5 ASCII glyphs

    Label label(kText);
    label.set_bounds({0, 0, 400, 40});
    label.set_text_align(LabelAlign::left);
    label.set_font_variant("tabular-nums");  // → tnum feature

    FeatureWidthCanvas canvas;
    const auto m = label.text_edit_metrics(canvas, kText);

    // The metrics resolver pushed the font-variant feature into the canvas.
    REQUIRE(canvas.set_feature_calls() >= 1);

    // caret_x_by_byte was shaped with features ACTIVE, so the end caret is the
    // WIDER feature-applied width (5 × 14 = 70), not the default (5 × 10 = 50).
    const float feature_w =
        static_cast<float>(kText.size()) * FeatureWidthCanvas::kFeatureAdvance;
    const float default_w =
        static_cast<float>(kText.size()) * FeatureWidthCanvas::kBaseAdvance;
    REQUIRE(m.caret_x_by_byte.size() == kText.size() + 1);
    REQUIRE(m.caret_x_by_byte.back() == Catch::Approx(feature_w));
    REQUIRE(m.caret_x_by_byte.back() != Catch::Approx(default_w));
    // Per-byte boundaries land on the feature-applied advance grid.
    for (std::size_t i = 0; i <= kText.size(); ++i)
        REQUIRE(m.caret_x_by_byte[i] ==
                Catch::Approx(static_cast<float>(i) *
                              FeatureWidthCanvas::kFeatureAdvance));
}

// A label with NO font-variant must NOT activate features — caret shapes with
// the default advances (the cross-check that the fix didn't force features on).
TEST_CASE("WYSIWYG caret: empty font-variant leaves features clear",
          "[inspect][overlay][wysiwyg][caret][issue-1737]") {
    const std::string kText = "12345";
    Label label(kText);
    label.set_bounds({0, 0, 400, 40});
    label.set_text_align(LabelAlign::left);
    // No set_font_variant — empty CSV.

    FeatureWidthCanvas canvas;
    const auto m = label.text_edit_metrics(canvas, kText);

    REQUIRE_FALSE(canvas.features_active());
    const float default_w =
        static_cast<float>(kText.size()) * FeatureWidthCanvas::kBaseAdvance;
    REQUIRE(m.caret_x_by_byte.back() == Catch::Approx(default_w));
}

// WYSIWYG caret RESIZE/SCALE — once a text field has been RESIZED or SCALED
// via the Select-tool corner drag, re-entering text edit must still land the
// caret/selection on the RENDERED glyphs. There are two resize modes:
//
//   (a) CONTAINER resize — the box's bounds grow (preferred_/dim_ + set_bounds),
//       scale stays 1.0. A left-aligned label still renders its text at the
//       box's left edge, so the caret origin is unchanged; the band height/y
//       must track the box top. This section pins that growing the box does
//       NOT drift the caret.
//
//   (b) PROPORTIONAL resize (Shift drag) — View::set_scale(s) +
//       transform-origin (0,0). View::paint_all renders the subtree under
//       `scale(s,s)` about the top-left origin, so a glyph at element-local x
//       renders on screen at s*x (origin 0,0 → screen_local = s*local). The
//       overlay computes caret_x_by_byte / local_text_left at the UNSCALED
//       font and view_bounds_in_root returns UNSCALED bounds, so a pre-fix
//       overlay drew the caret short by the scale factor — the maintainer's
//       "caret lands a glyph or two short after enlarging the field" bug.
//
// The overlay must apply the target's effective scale (and transform-origin)
// to the caret x, selection band x/width, and band y/height so both modes
// track the rendered text.
TEST_CASE("WYSIWYG caret: tracks rendered text after resize and scale",
          "[inspect][overlay][wysiwyg][caret][resize][issue-wysiwyg-caret-resize]") {
    constexpr float kCharW = SpacingCanvas::kCharW;
    // Chainer-like multi-token label; left-aligned (the default the running
    // design uses for these field labels).
    const std::string kText = "MULTIBAAND / M-S";  // 16 ASCII glyphs

    // root → label (left-aligned, no inherited spacing). Box starts wider than
    // the shaped text so the caret end is comfortably inside the original box.
    auto make_tree = [&](Label*& out_label) {
        auto root = std::make_unique<View>();
        root->set_bounds({0, 0, 600, 400});
        auto label = std::make_unique<Label>(kText);
        label->set_bounds({0, 0, 200, 40});
        label->set_text_align(LabelAlign::left);
        out_label = label.get();
        root->add_child(std::move(label));
        return root;
    };

    // No letter-spacing here → shaped width is purely glyph advances.
    const float shaped_w = static_cast<float>(kText.size()) * kCharW;

    // Helper: scan the recorded fill_rects for the selection band (the full-
    // width translucent fill) and the caret line (1.5px wide). Returns the
    // band's left x, the band's full width, and the caret line's left x.
    struct Bands { float band_x; float band_w; float caret_x; bool found_band; bool found_caret; };
    auto scan = [&](const SpacingCanvas& canvas, float want_w) -> Bands {
        Bands b{0, 0, 0, false, false};
        for (const auto& c : canvas.commands()) {
            if (c.type != pulp::canvas::DrawCommand::Type::fill_rect) continue;
            const float w = c.f[2];
            if (w >= want_w - 1.0f && w <= want_w + 2.0f) {
                b.band_x = c.f[0]; b.band_w = w; b.found_band = true;
            } else if (w >= 1.0f && w <= 2.0f) {  // 1.5px caret line
                b.caret_x = c.f[0]; b.found_caret = true;
            }
        }
        return b;
    };

    SECTION("container resize: box grows, left-aligned caret stays at text end") {
        Label* label = nullptr;
        auto root = make_tree(label);

        // Grow the box well past the text (the maintainer "resize much larger").
        auto b = label->bounds();
        b.width = 500.0f;
        b.height = 120.0f;
        label->set_bounds(b);

        InspectorOverlay overlay(*root);
        overlay.set_active(true);
        REQUIRE(overlay.begin_text_edit(label));
        overlay.text_select_all();
        REQUIRE(overlay.text_has_selection());

        SpacingCanvas canvas;
        overlay.paint(canvas);

        // Left-aligned: text renders at the box's left edge regardless of how
        // wide the box grew, so the band spans [0, shaped_w] and the caret
        // (whole buffer selected → caret at end) sits at shaped_w.
        auto bands = scan(canvas, shaped_w);
        REQUIRE(bands.found_band);
        REQUIRE(bands.band_x == Catch::Approx(0.0f).margin(1.0f));
        REQUIRE(bands.found_caret);
        REQUIRE(bands.caret_x == Catch::Approx(shaped_w).margin(1.5f));
    }

    SECTION("proportional scale: caret + band track scaled glyphs") {
        Label* label = nullptr;
        auto root = make_tree(label);

        // Proportional resize applies set_scale + top-left transform-origin
        // (see InspectorOverlay handle-drag: transform-origin (0,0)). Mirror
        // exactly what the gesture does so the test exercises the real shape.
        constexpr float kScale = 2.0f;
        label->set_transform_origin(0.0f, 0.0f);
        label->set_scale(kScale);

        InspectorOverlay overlay(*root);
        overlay.set_active(true);
        REQUIRE(overlay.begin_text_edit(label));
        overlay.text_select_all();
        REQUIRE(overlay.text_has_selection());

        SpacingCanvas canvas;
        overlay.paint(canvas);

        // With origin (0,0), View::paint_all renders glyph-local x at kScale*x.
        // So the full-string selection band must span [0, kScale*shaped_w] and
        // the end caret must sit at kScale*shaped_w — NOT the unscaled
        // shaped_w (the pre-fix short-caret bug).
        const float scaled_w = kScale * shaped_w;
        auto bands = scan(canvas, scaled_w);
        REQUIRE(bands.found_band);
        REQUIRE(bands.band_x == Catch::Approx(0.0f).margin(1.0f));
        REQUIRE(bands.band_w == Catch::Approx(scaled_w).margin(2.0f));
        REQUIRE(bands.found_caret);
        REQUIRE(bands.caret_x == Catch::Approx(scaled_w).margin(2.0f));
    }

    SECTION("scaled band height/y also scale") {
        Label* label = nullptr;
        auto root = make_tree(label);
        constexpr float kScale = 2.0f;
        label->set_transform_origin(0.0f, 0.0f);
        label->set_scale(kScale);

        // Unscaled band metrics for reference.
        SpacingCanvas mcanvas;
        const auto m = label->text_edit_metrics(mcanvas, kText);

        InspectorOverlay overlay(*root);
        overlay.set_active(true);
        REQUIRE(overlay.begin_text_edit(label));
        overlay.text_select_all();

        SpacingCanvas canvas;
        overlay.paint(canvas);

        // Find the selection band and assert its height == kScale * band_height
        // and its y == root_y + kScale * local_band_y (origin 0,0).
        const float scaled_w = kScale * shaped_w;
        bool found = false;
        for (const auto& c : canvas.commands()) {
            if (c.type != pulp::canvas::DrawCommand::Type::fill_rect) continue;
            if (c.f[2] >= scaled_w - 1.0f && c.f[2] <= scaled_w + 2.0f) {
                REQUIRE(c.f[1] == Catch::Approx(kScale * m.local_band_y).margin(1.5f));
                REQUIRE(c.f[3] == Catch::Approx(kScale * m.band_height).margin(1.5f));
                found = true;
            }
        }
        REQUIRE(found);
    }
}
