// Tests for the SDK runtime host-parameter surface:
//   - StateStoreHostParamSurface: the native backing (get/set/gesture/display).
//   - DesignFrameView integration: sync_from_host_params (host->UI snapshot),
//     route_changes_to_host_params (UI->host), set_element_param_key re-key,
//     and action routing to HostActionSurface.
//   - A framework-agnostic FakeHostParamSurface proves a view binds against the
//     interface, not the StateStore concretely (the "port once, three hosts"
//     ratchet). The fake stands in for any non-Pulp parameter store a host or
//     an existing plugin codebase brings with it: if the view only ever touches
//     HostParamSurface, that store can be swapped without touching the view.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <pulp/state/store.hpp>
#include <pulp/view/animation.hpp>
#include <pulp/view/design_frame_view.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/host_param_surface.hpp>
#include <pulp/view/plugin_view_host.hpp>

#include "harness/rt_allocation_probe.hpp"

using namespace pulp;
using pulp::view::DesignFrameElement;
using pulp::view::DesignFrameView;
using pulp::view::HostActionSurface;
using pulp::view::HostParamSurface;
using pulp::view::StateStoreHostParamSurface;

namespace {

// A minimal in-memory HostParamSurface with no StateStore behind it — proves a
// DesignFrameView binds against the abstract interface. Records writes so the
// UI->host direction can be asserted.
class FakeHostParamSurface : public HostParamSurface {
public:
    std::unordered_map<std::string, double> values;
    // Value cardinality per key. A key absent here reports 0 — the surface's
    // "continuous / no index domain" signal — which is also the base class's
    // default, so an existing test that never populates this is unaffected.
    std::unordered_map<std::string, int> steps;
    std::vector<std::string> gesture_log;   // "begin:key" / "end:key"
    int set_calls = 0;

protected:
    int do_param_step_count(std::string_view key) override {
        auto it = steps.find(std::string(key));
        return it == steps.end() ? 0 : it->second;
    }

    bool do_has_param(std::string_view key) override {
        return values.count(std::string(key)) > 0;
    }
    double do_get_param(std::string_view key) override {
        auto it = values.find(std::string(key));
        return it == values.end() ? 0.0 : it->second;
    }
    void do_set_param(std::string_view key, double v) override {
        values[std::string(key)] = v;
        ++set_calls;
    }
    void do_begin_gesture(std::string_view key) override {
        gesture_log.push_back("begin:" + std::string(key));
    }
    void do_end_gesture(std::string_view key) override {
        gesture_log.push_back("end:" + std::string(key));
    }
    std::string do_param_display_text(std::string_view key, double v) override {
        (void)v;
        return "disp:" + std::string(key);
    }
};

class RecordingActionSurface : public HostActionSurface {
public:
    std::vector<std::pair<std::string, std::string>> actions;

protected:
    bool do_send_host_action(std::string_view action, std::string_view args) override {
        actions.emplace_back(std::string(action), std::string(args));
        return true;
    }
};

// A single-knob design: a 100x100 panel with one knob at (10,10,20,20).
DesignFrameView make_single_knob(const std::string& param_key) {
    const std::string svg =
        R"(<svg width="100" height="100"><rect x="0" y="0" width="100" height="100"/></svg>)";
    DesignFrameElement knob;
    knob.kind = DesignFrameElement::Kind::knob;
    knob.x = 10; knob.y = 10; knob.w = 20; knob.h = 20;
    knob.cx = 20; knob.cy = 20;      // center — knobs hit-test by radius from cx/cy
    knob.hit_radius = 20.0f;
    knob.value = 0.0f;
    knob.param_key = param_key;
    return DesignFrameView(svg, {knob}, 0, 0, 100, 100);
}

} // namespace

TEST_CASE("StateStoreHostParamSurface round-trips through the store", "[view][host-param][issue-5230]") {
    state::StateStore store;
    state::ParamInfo gain;
    gain.id = 1;
    gain.name = "gain";
    gain.unit = "dB";
    gain.range = state::ParamRange::linear(-60.0f, 0.0f, -12.0f);
    store.add_parameter(gain);

    std::vector<state::ParamID> begun, ended;
    store.set_gesture_callbacks([&](state::ParamID id) { begun.push_back(id); },
                                [&](state::ParamID id) { ended.push_back(id); });

    StateStoreHostParamSurface surface(store);

    SECTION("membership is live and name-keyed by default") {
        REQUIRE(surface.has_param("gain"));
        REQUIRE_FALSE(surface.has_param("nonexistent"));
    }

    SECTION("get/set round-trips normalized values") {
        surface.set_param("gain", 0.75);
        REQUIRE(store.get_normalized(1) == Catch::Approx(0.75f));
        REQUIRE(surface.get_param("gain") == Catch::Approx(0.75));
    }

    SECTION("gestures forward to the store's undo grouping") {
        surface.begin_gesture("gain");
        surface.set_param("gain", 0.5);
        surface.end_gesture("gain");
        REQUIRE(begun == std::vector<state::ParamID>{1});
        REQUIRE(ended == std::vector<state::ParamID>{1});
    }

    SECTION("display text uses a numeric+unit fallback when no formatter is set") {
        // -60..0 dB linear; normalized 1.0 -> 0 dB.
        REQUIRE(surface.param_display_text("gain", 1.0) == "0 dB");
    }

    SECTION("display text uses ParamInfo::to_string when present") {
        state::ParamInfo freq;
        freq.id = 2;
        freq.name = "freq";
        freq.range = state::ParamRange::linear(20.0f, 20000.0f, 440.0f);
        freq.to_string = [](float v) { return std::to_string(static_cast<int>(v)) + " Hz custom"; };
        store.add_parameter(freq);
        StateStoreHostParamSurface s2(store);
        REQUIRE(s2.param_display_text("freq", 0.0) == "20 Hz custom");
    }

    SECTION("unknown keys are inert, never a crash") {
        REQUIRE(surface.get_param("missing") == 0.0);
        surface.set_param("missing", 0.5);            // no-op
        REQUIRE(surface.param_display_text("missing", 0.5).empty());
    }
}

TEST_CASE("param_step_count reports the parameter's own value cardinality",
          "[view][host-param][state]") {
    state::StateStore store;

    state::ParamInfo cutoff;              // continuous
    cutoff.id = 1;
    cutoff.name = "cutoff";
    cutoff.range = state::ParamRange::linear(20.0f, 20000.0f, 440.0f);
    store.add_parameter(cutoff);

    state::ParamInfo waveform;            // 6-way enum, labeled
    waveform.id = 2;
    waveform.name = "lfo_waveform";
    waveform.kind = state::ParamKind::Enum;
    waveform.range = state::ParamRange::linear(0.0f, 5.0f, 0.0f, 1.0f);
    waveform.value_labels = {"Sine", "Triangle", "Saw", "Ramp", "Square", "Random"};
    store.add_parameter(waveform);

    state::ParamInfo bypass;              // toggle: off/on
    bypass.id = 3;
    bypass.name = "bypass";
    bypass.kind = state::ParamKind::Toggle;
    bypass.range = state::ParamRange::linear(0.0f, 1.0f, 0.0f, 1.0f);
    store.add_parameter(bypass);

    state::ParamInfo quantized;           // continuous, but range.step quantizes
    quantized.id = 4;
    quantized.name = "quantized";
    quantized.range = state::ParamRange::linear(0.0f, 10.0f, 0.0f, 0.5f);
    store.add_parameter(quantized);

    state::ParamInfo octave;              // integer range, no labels
    octave.id = 5;
    octave.name = "octave";
    octave.kind = state::ParamKind::Integer;
    octave.range = state::ParamRange::linear(-2.0f, 2.0f, 0.0f, 1.0f);
    store.add_parameter(octave);

    StateStoreHostParamSurface surface(store);

    SECTION("a continuous param has no index domain") {
        REQUIRE(surface.param_step_count("cutoff") == 0);
    }

    SECTION("an unknown key reports 0, the same no-index-domain signal") {
        REQUIRE(surface.param_step_count("nonexistent") == 0);
        REQUIRE_FALSE(surface.has_param("nonexistent"));
    }

    SECTION("a discrete param reports its declared value count") {
        REQUIRE(surface.param_step_count("lfo_waveform") == 6);
        REQUIRE(surface.param_step_count("bypass") == 2);
        // Integer range with no labels: cardinality is derived from range/step.
        REQUIRE(surface.param_step_count("octave") == 5);
    }

    SECTION("range.step alone does not make a param discrete") {
        // Quantization is not semantics: the author declared Continuous, so
        // there is no index domain to divide by even though values snap.
        REQUIRE(surface.param_step_count("quantized") == 0);
    }

    SECTION("a default host surface reports 0 rather than guessing") {
        // An out-of-repo subclass that predates this accessor must degrade to
        // "continuous/unknown", never to a wrong cardinality.
        FakeHostParamSurface fake;
        fake.values["gain"] = 0.5;
        REQUIRE(fake.has_param("gain"));
        REQUIRE(fake.param_step_count("gain") == 0);
    }
}

TEST_CASE("the step count comes from the parameter, not from a view's option list",
          "[view][host-param][state]") {
    // The porting hazard this accessor exists to kill: a control that renders
    // 3 visible positions bound to a 6-value parameter. Scaling by the number
    // of things drawn silently emits wrong normalized values; the parameter is
    // the only authority on its own divisor.
    state::StateStore store;
    state::ParamInfo waveform;
    waveform.id = 1;
    waveform.name = "lfo_waveform";
    waveform.kind = state::ParamKind::Enum;
    waveform.range = state::ParamRange::linear(0.0f, 5.0f, 0.0f, 1.0f);
    waveform.value_labels = {"Sine", "Triangle", "Saw", "Ramp", "Square", "Random"};
    store.add_parameter(waveform);

    StateStoreHostParamSurface surface(store);

    const int visible_options = 3;             // what a radio happens to draw
    const int steps = surface.param_step_count("lfo_waveform");
    REQUIRE(steps == 6);                       // unchanged by the UI's opinion
    REQUIRE(steps != visible_options);

    // Selecting index 2 must scale against 6 values (2/5), never against the
    // 3 drawn options (2/2 == 1.0, which would jump the host to the last value).
    const double correct = pulp::view::param_index_to_normalized(2, steps);
    REQUIRE(correct == Catch::Approx(2.0 / 5.0));

    const double from_ui_option_count =
        pulp::view::param_index_to_normalized(2, visible_options);
    REQUIRE(from_ui_option_count == Catch::Approx(1.0));
    REQUIRE(correct != Catch::Approx(from_ui_option_count));

    // The correct divisor round-trips through the real store; the UI-derived
    // one lands on a different value entirely.
    surface.set_param("lfo_waveform", correct);
    REQUIRE(surface.param_display_text("lfo_waveform", surface.get_param("lfo_waveform")) == "2");
    surface.set_param("lfo_waveform", from_ui_option_count);
    REQUIRE(surface.param_display_text("lfo_waveform", surface.get_param("lfo_waveform")) == "5");
}

TEST_CASE("param_index_to_normalized spans the full automation range",
          "[view][host-param]") {
    SECTION("index 0 and the last index reach the range ends") {
        REQUIRE(pulp::view::param_index_to_normalized(0, 6) == Catch::Approx(0.0));
        REQUIRE(pulp::view::param_index_to_normalized(5, 6) == Catch::Approx(1.0));
        REQUIRE(pulp::view::param_index_to_normalized(1, 6) == Catch::Approx(0.2));
    }

    SECTION("a toggle spans off/on") {
        REQUIRE(pulp::view::param_index_to_normalized(0, 2) == Catch::Approx(0.0));
        REQUIRE(pulp::view::param_index_to_normalized(1, 2) == Catch::Approx(1.0));
    }

    SECTION("a 0 step count never divides by zero") {
        // 0 is the continuous/unknown signal; 1 has a single value. Neither
        // carries an index domain, so both collapse to 0.0 instead of trapping.
        REQUIRE(pulp::view::param_index_to_normalized(3, 0) == Catch::Approx(0.0));
        REQUIRE(pulp::view::param_index_to_normalized(3, 1) == Catch::Approx(0.0));
        REQUIRE(pulp::view::param_normalized_to_index(0.7, 0) == 0);
        REQUIRE(pulp::view::param_normalized_to_index(0.7, 1) == 0);
    }

    SECTION("out-of-domain indices and values clamp") {
        REQUIRE(pulp::view::param_index_to_normalized(-1, 6) == Catch::Approx(0.0));
        REQUIRE(pulp::view::param_index_to_normalized(99, 6) == Catch::Approx(1.0));
        REQUIRE(pulp::view::param_normalized_to_index(-0.5, 6) == 0);
        REQUIRE(pulp::view::param_normalized_to_index(1.5, 6) == 5);
    }

    SECTION("normalized_to_index snaps to the nearest index") {
        REQUIRE(pulp::view::param_normalized_to_index(0.19, 6) == 1);
        REQUIRE(pulp::view::param_normalized_to_index(0.21, 6) == 1);
        REQUIRE(pulp::view::param_normalized_to_index(0.5, 6) == 3);  // 2.5 rounds up
    }

    SECTION("index -> normalized -> index round-trips every value") {
        for (int steps : {2, 3, 6, 17}) {
            for (int i = 0; i < steps; ++i) {
                const double n = pulp::view::param_index_to_normalized(i, steps);
                REQUIRE(pulp::view::param_normalized_to_index(n, steps) == i);
            }
        }
    }
}

TEST_CASE("a discrete param's normalized index agrees with its ParamRange",
          "[view][host-param][state]") {
    // param_index_to_normalized works off a bare step count because a host
    // surface may have no ParamRange behind it. For a StateStore-backed
    // discrete param the two must not disagree, or the embed and native lanes
    // would scale the same control differently.
    state::StateStore store;
    state::ParamInfo waveform;
    waveform.id = 1;
    waveform.name = "lfo_waveform";
    waveform.kind = state::ParamKind::Enum;
    waveform.range = state::ParamRange::linear(0.0f, 5.0f, 0.0f, 1.0f);
    waveform.value_labels = {"Sine", "Triangle", "Saw", "Ramp", "Square", "Random"};
    store.add_parameter(waveform);

    StateStoreHostParamSurface surface(store);
    const int steps = surface.param_step_count("lfo_waveform");

    REQUIRE(waveform.range.is_linear());   // the helper's stated assumption

    for (int i = 0; i < steps; ++i) {
        const double n = pulp::view::param_index_to_normalized(i, steps);
        REQUIRE(n == Catch::Approx(waveform.range.normalize(static_cast<float>(i))));

        // And it survives a real write/read through the store.
        surface.set_param("lfo_waveform", n);
        REQUIRE(pulp::view::param_normalized_to_index(surface.get_param("lfo_waveform"),
                                                      steps) == i);
    }
}

TEST_CASE("a skewed discrete range is outside the index helper's contract",
          "[view][host-param][state]") {
    // param_index_to_normalized takes a bare count because a host surface may
    // have no ParamRange behind it, so it can only assume even spacing. A
    // discrete param that declares a skew defines its own curve and the two
    // genuinely disagree. This pins that boundary as a known limit rather than
    // leaving it a silent mis-scale: authors declare discrete params linear.
    state::StateStore store;
    state::ParamInfo shaped;
    shaped.id = 1;
    shaped.name = "shaped";
    shaped.kind = state::ParamKind::Enum;
    shaped.range = state::ParamRange{0.0f, 5.0f, 0.0f, 1.0f, 0.5f, false};
    shaped.value_labels = {"a", "b", "c", "d", "e", "f"};
    store.add_parameter(shaped);

    StateStoreHostParamSurface surface(store);
    const int steps = surface.param_step_count("shaped");
    REQUIRE(steps == 6);                       // cardinality is still correct
    REQUIRE_FALSE(shaped.range.is_linear());

    // The ends still agree — a skew fixes both endpoints.
    REQUIRE(pulp::view::param_index_to_normalized(0, steps) ==
            Catch::Approx(shaped.range.normalize(0.0f)));
    REQUIRE(pulp::view::param_index_to_normalized(5, steps) ==
            Catch::Approx(shaped.range.normalize(5.0f)));

    // The interior does NOT. The range's curve is the authority here.
    REQUIRE(pulp::view::param_index_to_normalized(1, steps) != Catch::Approx(
                shaped.range.normalize(1.0f)));
}

TEST_CASE("StateStoreHostParamSurface honors a custom key resolver", "[view][host-param][issue-5230]") {
    state::StateStore store;
    state::ParamInfo p;
    p.id = 7;
    p.name = "Internal Name";
    p.range = state::ParamRange::linear(0.0f, 1.0f, 0.0f);
    store.add_parameter(p);

    // Map a design key ("cutoff") to a store param whose name differs.
    StateStoreHostParamSurface surface(store, [](std::string_view key) -> std::optional<state::ParamID> {
        if (key == "cutoff") return state::ParamID{7};
        return std::nullopt;
    });
    REQUIRE(surface.has_param("cutoff"));
    REQUIRE_FALSE(surface.has_param("Internal Name"));
    surface.set_param("cutoff", 0.3);
    REQUIRE(store.get_normalized(7) == Catch::Approx(0.3f));
}

TEST_CASE("View propagates host surfaces to children", "[view][host-param][issue-5230]") {
    auto root = std::make_unique<view::View>();
    FakeHostParamSurface params;
    RecordingActionSurface actions;

    auto child = std::make_unique<view::View>();
    view::View* child_ptr = child.get();
    root->add_child(std::move(child));

    root->set_host_params(&params);
    root->set_host_actions(&actions);
    REQUIRE(child_ptr->host_params() == &params);
    REQUIRE(child_ptr->host_actions() == &actions);

    // A child added AFTER the surfaces are set still inherits them.
    auto later = std::make_unique<view::View>();
    view::View* later_ptr = later.get();
    root->add_child(std::move(later));
    REQUIRE(later_ptr->host_params() == &params);

    // Removing a child clears its back-references.
    auto removed = root->remove_child(later_ptr);
    REQUIRE(removed->host_params() == nullptr);
}

TEST_CASE("DesignFrameView pulls values and display text from the surface at tick",
          "[view][host-param][issue-5230]") {
    DesignFrameView dfv = make_single_knob("gain");
    FakeHostParamSurface params;
    params.values["gain"] = 0.6;
    dfv.set_host_params(&params);

    // Before sync the element is at its constructed value.
    REQUIRE(dfv.element_value(0) == Catch::Approx(0.0f));
    dfv.sync_from_host_params();
    REQUIRE(dfv.element_value(0) == Catch::Approx(0.6f));

    // Null surface degrades gracefully (preview/screenshot path).
    dfv.set_host_params(nullptr);
    dfv.sync_from_host_params();  // no crash, no change
    REQUIRE(dfv.element_value(0) == Catch::Approx(0.6f));
}

TEST_CASE("DesignFrameView routes user gestures to the surface when enabled",
          "[view][host-param][issue-5230]") {
    DesignFrameView dfv = make_single_knob("gain");
    dfv.set_bounds({0, 0, 100, 100});
    FakeHostParamSurface params;
    params.values["gain"] = 0.0;

    SECTION("disabled by default: no writes to the surface") {
        dfv.set_host_params(&params);
        dfv.simulate_drag({20, 20}, {20, 5});  // drag the knob up
        REQUIRE(params.set_calls == 0);
        REQUIRE(params.gesture_log.empty());
    }

    SECTION("enabled: gesture brackets and writes reach the surface") {
        dfv.set_host_params(&params);
        dfv.route_changes_to_host_params(true);
        dfv.simulate_drag({20, 20}, {20, 5});  // drag up -> value increases
        REQUIRE(params.set_calls >= 1);
        REQUIRE(params.gesture_log.front() == "begin:gain");
        REQUIRE(params.gesture_log.back() == "end:gain");
        REQUIRE(params.values["gain"] > 0.0);
    }

    SECTION("an unbound key never writes even with routing on") {
        DesignFrameView unbound = make_single_knob("");  // no param_key
        unbound.set_bounds({0, 0, 100, 100});
        unbound.set_host_params(&params);
        unbound.route_changes_to_host_params(true);
        unbound.simulate_drag({20, 20}, {20, 5});
        REQUIRE(params.set_calls == 0);
    }
}

namespace {
// A foreign-host adapter — an existing plugin codebase embedding a Pulp editor,
// or a headless QA harness — subclasses DesignFrameView to drive the editor from
// its own automation layer, calling the protected emit_* funnel so synthetic
// input travels the SAME path a pointer gesture does. emit_* is protected (not
// private) precisely so that adapter never has to patch the header to get in.
class ForeignHostDrivenFrameView : public DesignFrameView {
public:
    using DesignFrameView::DesignFrameView;
    void drive_value(int i, float v) { emit_element_changed(i, v); }
    void drive_gesture_begin(int i) { emit_gesture_begin(i); }
    void drive_gesture_end(int i) { emit_gesture_end(i); }
};

ForeignHostDrivenFrameView make_driven_single_knob(const std::string& key) {
    const std::string svg =
        R"(<svg width="100" height="100"><rect x="0" y="0" width="100" height="100"/></svg>)";
    DesignFrameElement knob;
    knob.kind = DesignFrameElement::Kind::knob;
    knob.x = 10; knob.y = 10; knob.w = 20; knob.h = 20;
    knob.cx = 20; knob.cy = 20; knob.hit_radius = 20.0f;
    knob.value = 0.0f; knob.param_key = key;
    return ForeignHostDrivenFrameView(svg, {knob}, 0, 0, 100, 100);
}
}  // namespace

TEST_CASE("a subclass drives the emit_* funnel through routing + public callbacks",
          "[view][host-param][design-import][frame]") {
    ForeignHostDrivenFrameView dfv = make_driven_single_knob("gain");
    dfv.set_bounds({0, 0, 100, 100});
    FakeHostParamSurface params;
    params.values["gain"] = 0.0;
    dfv.set_host_params(&params);
    dfv.route_changes_to_host_params(true);

    std::vector<std::pair<int, float>> cb_changes;
    std::vector<std::string> cb_gestures;
    dfv.on_element_changed = [&](int i, float v) { cb_changes.emplace_back(i, v); };
    dfv.on_gesture_begin = [&](int i) { cb_gestures.push_back("begin:" + std::to_string(i)); };
    dfv.on_gesture_end = [&](int i) { cb_gestures.push_back("end:" + std::to_string(i)); };

    // Synthetic gesture: begin -> change -> end, exactly like a pointer drag.
    dfv.drive_gesture_begin(0);
    dfv.drive_value(0, 0.75f);
    dfv.drive_gesture_end(0);

    // The public callbacks fired...
    REQUIRE(cb_changes.size() == 1);
    CHECK(cb_changes.front().first == 0);
    CHECK(cb_changes.front().second == Catch::Approx(0.75f));
    REQUIRE(cb_gestures.size() == 2);
    CHECK(cb_gestures.front() == "begin:0");
    CHECK(cb_gestures.back() == "end:0");

    // ...and the SAME funnel wrote through to the host-param surface, bracketed.
    CHECK(params.values["gain"] == Catch::Approx(0.75));
    REQUIRE(params.gesture_log.size() == 2);
    CHECK(params.gesture_log.front() == "begin:gain");
    CHECK(params.gesture_log.back() == "end:gain");
    CHECK(params.set_calls == 1);
}

TEST_CASE("emit_element_changed on an out-of-range index skips routing, still fires the callback",
          "[view][host-param][design-import][frame]") {
    ForeignHostDrivenFrameView dfv = make_driven_single_knob("gain");
    FakeHostParamSurface params;
    params.values["gain"] = 0.0;
    dfv.set_host_params(&params);
    dfv.route_changes_to_host_params(true);

    int cb = -1;
    dfv.on_element_changed = [&](int i, float) { cb = i; };
    dfv.drive_value(99, 0.5f);      // out of range
    CHECK(cb == 99);                // callback still receives the raw index
    CHECK(params.set_calls == 0);   // but no host write for an out-of-range element
}

TEST_CASE("set_element_param_key re-keys and re-binds without a remount",
          "[view][host-param][issue-5230]") {
    DesignFrameView dfv = make_single_knob("slot0.gain");
    FakeHostParamSurface params;
    params.values["slot0.gain"] = 0.2;
    params.values["slot1.gain"] = 0.9;
    dfv.set_host_params(&params);

    int rekey_calls = 0;
    std::string last_key;
    dfv.on_param_key_changed = [&](int i, const std::string& key) {
        ++rekey_calls;
        REQUIRE(i == 0);
        last_key = key;
    };

    dfv.sync_from_host_params();
    REQUIRE(dfv.element_value(0) == Catch::Approx(0.2f));

    // Page the rack: element 0 now reflects slot1's parameter.
    dfv.set_element_param_key(0, "slot1.gain");
    REQUIRE(rekey_calls == 1);
    REQUIRE(last_key == "slot1.gain");
    REQUIRE(dfv.element_param_key(0) == "slot1.gain");

    dfv.sync_from_host_params();
    REQUIRE(dfv.element_value(0) == Catch::Approx(0.9f));

    // Re-keying to the same key is a no-op (no spurious rebuild).
    dfv.set_element_param_key(0, "slot1.gain");
    REQUIRE(rekey_calls == 1);
}

TEST_CASE("DesignFrameView tracks per-element hover", "[view][hover][issue-5230]") {
    DesignFrameView dfv = make_single_knob("gain");
    dfv.set_bounds({0, 0, 100, 100});

    std::vector<std::pair<int, bool>> hover_log;
    dfv.on_element_hover = [&](int i, bool entered) { hover_log.emplace_back(i, entered); };

    REQUIRE(dfv.element_hovered() == -1);

    // Hover onto the knob (center 20,20).
    dfv.simulate_hover({20, 20});
    REQUIRE(dfv.element_hovered() == 0);
    REQUIRE(dfv.element_is_hovered(0));
    REQUIRE(hover_log == std::vector<std::pair<int, bool>>{{0, true}});

    // Hover off into empty panel space — the knob exits.
    dfv.simulate_hover({90, 90});
    REQUIRE(dfv.element_hovered() == -1);
    REQUIRE(hover_log.back() == std::pair<int, bool>{0, false});

    // A disabled element is never hovered.
    dfv.set_element_enabled(0, false);
    dfv.simulate_hover({20, 20});
    REQUIRE(dfv.element_hovered() == -1);
}

TEST_CASE("A disabled element is not hit-testable", "[view][hover][issue-5230]") {
    DesignFrameView dfv = make_single_knob("gain");
    dfv.set_bounds({0, 0, 100, 100});
    FakeHostParamSurface params;
    params.values["gain"] = 0.0;
    dfv.set_host_params(&params);
    dfv.route_changes_to_host_params(true);

    dfv.set_element_enabled(0, false);
    REQUIRE_FALSE(dfv.element_enabled(0));
    dfv.simulate_drag({20, 20}, {20, 5});  // drag over a disabled knob
    REQUIRE(params.set_calls == 0);        // no gesture reached the host

    // Re-enabling restores interaction.
    dfv.set_element_enabled(0, true);
    dfv.simulate_drag({20, 20}, {20, 5});
    REQUIRE(params.set_calls >= 1);
}

TEST_CASE("View::animate tweens via the frame clock and self-unsubscribes",
          "[view][animate][issue-5230]") {
    view::FrameClock clock;
    auto root = std::make_unique<view::View>();
    root->set_frame_clock(&clock);

    float v = -99.0f;
    bool done = false;
    int id = root->animate([&](float x) { v = x; }, 0.0f, 1.0f, 1.0f,
                           view::easing::linear, [&]() { done = true; });
    REQUIRE(id >= 0);
    REQUIRE(v == Catch::Approx(0.0f));               // start value seeded
    REQUIRE(root->active_animation_count() == 1);

    clock.tick(0.5f);
    REQUIRE(v == Catch::Approx(0.5f));               // linear midpoint
    REQUIRE_FALSE(done);

    clock.tick(0.5f);
    REQUIRE(v == Catch::Approx(1.0f));               // reached target
    REQUIRE(done);
    REQUIRE(root->active_animation_count() == 0);    // auto-unsubscribed
    REQUIRE_FALSE(clock.has_active_subscribers());
}

TEST_CASE("View::animate returns -1 with no frame clock (preview path)",
          "[view][animate][issue-5230]") {
    auto root = std::make_unique<view::View>();
    float v = 5.0f;
    REQUIRE(root->animate([&](float x) { v = x; }, 0.0f, 1.0f, 1.0f) == -1);
    REQUIRE(v == Catch::Approx(5.0f));  // untouched
}

TEST_CASE("A tagged animate cancels the prior one with the same tag",
          "[view][animate][issue-5230]") {
    view::FrameClock clock;
    auto root = std::make_unique<view::View>();
    root->set_frame_clock(&clock);
    float v = 0.0f;
    root->animate([&](float x) { v = x; }, 0.0f, 1.0f, 1.0f, {}, {}, "fade");
    root->animate([&](float x) { v = x; }, 0.0f, 10.0f, 1.0f, {}, {}, "fade");
    REQUIRE(root->active_animation_count() == 1);  // second replaced the first
    clock.tick(1.0f);
    REQUIRE(v == Catch::Approx(10.0f));            // the second animation won
}

TEST_CASE("cancel_animation stops a tween without firing on_done",
          "[view][animate][issue-5230]") {
    view::FrameClock clock;
    auto root = std::make_unique<view::View>();
    root->set_frame_clock(&clock);
    float v = 0.0f;
    bool done = false;
    int id = root->animate([&](float x) { v = x; }, 0.0f, 1.0f, 1.0f, {},
                           [&]() { done = true; });
    clock.tick(0.25f);
    REQUIRE(v == Catch::Approx(0.25f));
    root->cancel_animation(id);
    REQUIRE(root->active_animation_count() == 0);
    clock.tick(1.0f);
    REQUIRE(v == Catch::Approx(0.25f));  // frozen where cancelled
    REQUIRE_FALSE(done);
}

TEST_CASE("Destroying a view mid-animation unsubscribes safely",
          "[view][animate][issue-5230]") {
    view::FrameClock clock;
    float v = 0.0f;
    {
        auto root = std::make_unique<view::View>();
        root->set_frame_clock(&clock);
        root->animate([&](float x) { v = x; }, 0.0f, 1.0f, 1.0f);
        clock.tick(0.3f);
        REQUIRE(v == Catch::Approx(0.3f));
    }  // view destroyed mid-animation — must unsubscribe its callback
    REQUIRE_FALSE(clock.has_active_subscribers());
    clock.tick(0.5f);                    // must not fire the dangling callback
    REQUIRE(v == Catch::Approx(0.3f));   // unchanged
}

TEST_CASE("Detaching a child mid-animation then destroying it is safe (regression)",
          "[view][animate][issue-5230]") {
    // Regression for the review-found UAF: a non-root child resolves the clock
    // via its parent; once remove_child clears parent_, ~View could no longer
    // reach the clock to unsubscribe, leaving the root's clock firing on freed
    // memory. Fixed by caching the FrameClock* per animation.
    view::FrameClock clock;
    float v = 0.0f;
    auto root = std::make_unique<view::View>();
    root->set_frame_clock(&clock);
    auto child = std::make_unique<view::View>();
    view::View* cp = child.get();
    root->add_child(std::move(child));

    cp->animate([&](float x) { v = x; }, 0.0f, 1.0f, 1.0f);  // resolves clock via parent
    REQUIRE(clock.has_active_subscribers());
    clock.tick(0.3f);
    REQUIRE(v == Catch::Approx(0.3f));

    auto owned = root->remove_child(cp);  // parent_ -> null
    owned.reset();                        // destroy the detached child
    REQUIRE_FALSE(clock.has_active_subscribers());  // unsubscribed via cached clock
    clock.tick(0.5f);                     // must NOT fire on freed child
    REQUIRE(v == Catch::Approx(0.3f));    // unchanged
}

TEST_CASE("Re-keying an idle element does not emit an unbalanced end_gesture (regression)",
          "[view][host-param][issue-5230]") {
    DesignFrameView dfv = make_single_knob("slot0.gain");
    FakeHostParamSurface params;
    params.values["slot0.gain"] = 0.2;
    params.values["slot1.gain"] = 0.9;
    dfv.set_host_params(&params);
    dfv.route_changes_to_host_params(true);

    // No drag is open — re-keying must not send end_gesture for the old key.
    dfv.set_element_param_key(0, "slot1.gain");
    REQUIRE(params.gesture_log.empty());
}

TEST_CASE("DesignFrameView forwards action clicks to the host action channel",
          "[view][host-action][issue-5230]") {
    const std::string svg =
        R"(<svg width="100" height="100"><rect x="0" y="0" width="100" height="100"/></svg>)";
    DesignFrameElement btn;
    btn.kind = DesignFrameElement::Kind::action;
    btn.x = 10; btn.y = 10; btn.w = 40; btn.h = 20;
    btn.action = "load_preset";
    DesignFrameView dfv(svg, {btn}, 0, 0, 100, 100);
    dfv.set_bounds({0, 0, 100, 100});

    RecordingActionSurface actions;
    dfv.set_host_actions(&actions);

    SECTION("disabled by default") {
        dfv.simulate_click({30, 20});
        REQUIRE(actions.actions.empty());
    }
    SECTION("enabled: the action id + empty JSON payload reach the surface") {
        dfv.route_actions_to_host(true);
        dfv.simulate_click({30, 20});
        REQUIRE(actions.actions.size() == 1);
        REQUIRE(actions.actions[0].first == "load_preset");
        REQUIRE(actions.actions[0].second == "{}");
    }
}

// ── Element metadata a binder needs to map a control onto a host parameter ────
// A choice control's normalized value is selected_index / (option_count - 1). A
// binder that cannot read the option count cannot map that value onto a host
// parameter's own step count — a 3-option control reports 0, 0.5, 1.0 and a
// binder guessing "2 options" would emit 0, 1, 1. option_count is the missing
// denominator; is_discrete is the flag that says a denominator exists at all.
TEST_CASE("element_option_count exposes a choice control's denominator",
          "[view][host-param][design-import][frame]") {
    const std::string svg =
        R"(<svg width="200" height="100"><rect x="0" y="0" width="200" height="100"/></svg>)";

    DesignFrameElement knob;   // continuous
    knob.kind = DesignFrameElement::Kind::knob;
    knob.x = 0; knob.y = 0; knob.w = 20; knob.h = 20;
    knob.cx = 10; knob.cy = 10; knob.hit_radius = 10.0f;
    knob.param_key = "cutoff";

    DesignFrameElement tabs;   // 3-position choice control
    tabs.kind = DesignFrameElement::Kind::tab_group;
    tabs.x = 30; tabs.y = 0; tabs.w = 90; tabs.h = 20;
    tabs.options = {"LP", "BP", "HP"};
    tabs.param_key = "mode";

    DesignFrameElement sw;     // toggle: 2 positions, no options list
    sw.kind = DesignFrameElement::Kind::toggle;
    sw.x = 130; sw.y = 0; sw.w = 30; sw.h = 20;
    sw.param_key = "bypass";

    DesignFrameView dfv(svg, {knob, tabs, sw}, 0, 0, 200, 100);

    CHECK_FALSE(dfv.element_is_discrete(0));
    CHECK(dfv.element_option_count(0) == 0);      // continuous: no denominator

    CHECK(dfv.element_is_discrete(1));
    CHECK(dfv.element_option_count(1) == 3);      // <-- the wire

    CHECK(dfv.element_is_discrete(2));
    CHECK(dfv.element_option_count(2) == 2);      // a toggle is off/on

    // Out of range is inert.
    CHECK(dfv.element_option_count(99) == 0);
    CHECK_FALSE(dfv.element_is_discrete(-1));

    // With NO host surface installed, the count really is the denominator the
    // element normalizes against: option i of 3 maps to i/2. This is the
    // documented no-surface fallback, not the divisor in general — install a
    // surface that reports a cardinality and the parameter's count wins instead.
    dfv.set_element_value(1, 1.0f);
    CHECK(dfv.element_value(1) == Catch::Approx(1.0f));
    dfv.set_element_value(1, 0.5f);
    CHECK(dfv.element_value(1) == Catch::Approx(0.5f));   // middle of 3 == index 1
}

TEST_CASE("routing on TOP of a writing binder double-writes — the documented hazard",
          "[view][host-param][issue-5230]") {
    // This test does not assert a bug; it pins the contract that makes the
    // OFF-by-default matter.
    //
    // on_element_changed keeps firing when routing is on. For a consumer that
    // merely OBSERVES, that is free. For the existing binder path — which
    // forwards on_element_changed into the host's parameter store itself — it is
    // a double write: the surface sends the value, and then the binder sends it
    // again. The host sees a doubled automation write and a duplicated gesture
    // bracket.
    //
    // A view picks ONE path. If this test ever starts reporting a single write
    // with both wired, the funnel has grown a dedupe and the header comment
    // telling people to choose is now wrong.
    ForeignHostDrivenFrameView dfv = make_driven_single_knob("gain");
    dfv.set_bounds({0, 0, 100, 100});

    FakeHostParamSurface params;
    params.values["gain"] = 0.0;
    dfv.set_host_params(&params);
    dfv.route_changes_to_host_params(true);          // path A: the surface

    // path B: a legacy binder that ALSO writes to the same host.
    int binder_writes = 0;
    dfv.on_element_changed = [&](int, float) { ++binder_writes; };

    dfv.drive_gesture_begin(0);
    dfv.drive_value(0, 0.75f);
    dfv.drive_gesture_end(0);

    // Both paths carried the same gesture. Two writes reach the host for one
    // user movement — which is exactly why you must not wire both.
    CHECK(params.set_calls == 1);      // the surface wrote once...
    CHECK(binder_writes == 1);         // ...and the binder wrote once MORE
}

// ── Host-authoritative choice scaling, typed commits, and the bind grid ──────

namespace {

// A 3-position tab group bound to `key`, on a 200x100 panel. The motivating
// shape: a control that draws three positions for a parameter that may well
// expose more values than that.
DesignFrameView make_three_option_tabs(const std::string& key, int selected) {
    const std::string svg =
        R"(<svg width="200" height="100"><rect x="0" y="0" width="200" height="100"/></svg>)";
    DesignFrameElement tabs;
    tabs.kind = DesignFrameElement::Kind::tab_group;
    tabs.x = 10; tabs.y = 10; tabs.w = 90; tabs.h = 20;
    tabs.options = {"Sine", "Triangle", "Saw"};
    tabs.selected_index = selected;
    tabs.param_key = key;
    return DesignFrameView(svg, {tabs}, 0, 0, 200, 100);
}

} // namespace

TEST_CASE("a choice control scales against the HOST's value count, not its own options",
          "[view][host-param][design-import][frame]") {
    // The defect this fixes: a control drawn with 3 positions, bound to a 6-value
    // parameter, normalizing against the 3 things it drew. Index 2 of 3 then
    // emits 2/2 == 1.0 and slams the host to its LAST value (index 5), instead of
    // 2/5 == 0.4 (index 2). The parameter owns its range; the view does not
    // shrink it by drawing fewer positions.
    DesignFrameView dfv = make_three_option_tabs("lfo_waveform", 2);
    dfv.set_bounds({0, 0, 200, 100});

    FakeHostParamSurface params;
    params.values["lfo_waveform"] = 0.0;
    params.steps["lfo_waveform"] = 6;          // SIX values; the UI draws three
    dfv.set_host_params(&params);

    // 2/5, not 2/2. This is the whole point.
    CHECK(dfv.element_value(0) == Catch::Approx(2.0f / 5.0f));
    CHECK(dfv.element_value(0) != Catch::Approx(1.0f));

    // Every index against the host's divisor, not the option list's.
    dfv.set_element_value(0, 0.0f);
    CHECK(dfv.element_value(0) == Catch::Approx(0.0f));       // index 0 -> 0/5
    dfv.set_element_value(0, 1.0f / 5.0f);
    CHECK(dfv.element_value(0) == Catch::Approx(1.0f / 5.0f)); // index 1 -> 1/5
}

TEST_CASE("with no host surface a choice control falls back to its own option count",
          "[view][host-param][design-import][frame]") {
    // The documented fallback: preview / screenshot / any path with no host to
    // ask. The control's own positions are then the only domain in existence, so
    // 3 options span 0, 0.5, 1.0. This is NOT the bug above — there is no
    // parameter here whose range could be misrepresented.
    DesignFrameView dfv = make_three_option_tabs("lfo_waveform", 2);
    dfv.set_bounds({0, 0, 200, 100});

    CHECK(dfv.element_value(0) == Catch::Approx(1.0f));        // index 2 of 3 -> 2/2
    CHECK(dfv.param_scale_mismatches().empty());               // nothing to disagree with
}

TEST_CASE("a key a LIVE host does not carry is reported, never quietly absorbed",
          "[view][host-param][design-import][frame]") {
    // The porting defect in its most likely disguise: one typo. A ported control
    // table names "lfo_wavefrom"; the host carries "lfo_waveform" with 6 values.
    //
    // A live host is NOT the hostless preview case, and folding the two together
    // is what made the reported bug survive its own fix: with a host installed,
    // an unresolved key means the element is bound to NOTHING — it never syncs,
    // its edits never route, and it scales by the 3 positions it draws. Index 2
    // then emits 2/2 == 1.0, the exact value the fix exists to prevent.
    //
    // The options remain the only domain available, so 1.0 is still what comes
    // out — the contract is that it is never SILENT.
    FakeHostParamSurface params;
    params.values["lfo_waveform"] = 0.0;   // the host's real key...
    params.steps["lfo_waveform"] = 6;
    DesignFrameView dfv = make_three_option_tabs("lfo_wavefrom", 2);  // ...and the typo
    dfv.set_bounds({0, 0, 200, 100});
    dfv.set_host_params(&params);

    CHECK(dfv.element_value(0) == Catch::Approx(1.0f));  // the guess, unavoidable...

    REQUIRE(dfv.param_scale_mismatches().size() == 1);   // ...but reported
    const auto& m = dfv.param_scale_mismatches()[0];
    CHECK(m.param_key == "lfo_wavefrom");
    CHECK(m.ui_option_count == 3);
    CHECK(m.host_step_count == 0);
    // The field that names the diagnosis. false = "this host has never heard of
    // the key" (check the spelling), which is a different repair from a surface
    // that answered 0 because it cannot answer at all.
    CHECK_FALSE(m.host_has_param);
}

TEST_CASE("an unresolved key on a live host reports distinctly from an unanswered one",
          "[view][host-param][design-import][frame]") {
    // Both report ui=3 / host=0 and both scale by the option list, so the counts
    // alone cannot tell a consumer which repair to make. host_has_param is the
    // whole difference: fix the key, or wire do_param_step_count.
    FakeHostParamSurface params;
    params.values["known_but_silent"] = 0.0;   // resolves; steps deliberately unset

    DesignFrameView answered = make_three_option_tabs("known_but_silent", 2);
    answered.set_bounds({0, 0, 200, 100});
    answered.set_host_params(&params);
    (void)answered.element_value(0);
    REQUIRE(answered.param_scale_mismatches().size() == 1);
    CHECK(answered.param_scale_mismatches()[0].host_has_param);      // key is real

    DesignFrameView unresolved = make_three_option_tabs("no_such_key", 2);
    unresolved.set_bounds({0, 0, 200, 100});
    unresolved.set_host_params(&params);
    (void)unresolved.element_value(0);
    REQUIRE(unresolved.param_scale_mismatches().size() == 1);
    CHECK_FALSE(unresolved.param_scale_mismatches()[0].host_has_param);  // key is not
}

TEST_CASE("a discrete control told '0' by its surface guesses, and says so",
          "[view][host-param][design-import][frame]") {
    // A resolved key whose surface reports 0 is NOT evidence of a continuous
    // parameter. do_param_step_count is non-pure and defaults to 0, so a surface
    // whose parameter system has not wired the accessor answers 0 for EVERY key,
    // discrete ones included. A discrete element scaling by its own option count
    // on that answer is the original bug wearing a fix's clothes — it looks
    // right and is silently wrong.
    //
    // The options remain the only domain available, so they are still used; the
    // point is that the guess is REPORTED rather than absorbed.
    DesignFrameView dfv = make_three_option_tabs("lfo_waveform", 2);
    dfv.set_bounds({0, 0, 200, 100});

    FakeHostParamSurface params;
    params.values["lfo_waveform"] = 0.0;
    // Note what is NOT set: params.steps has no entry, so the surface reports 0 —
    // exactly what a surface that never overrode do_param_step_count does.
    dfv.set_host_params(&params);

    std::vector<pulp::view::ParamScaleMismatch> seen;
    dfv.set_on_param_scale_mismatch(
        [&](const pulp::view::ParamScaleMismatch& m) { seen.push_back(m); });

    CHECK(dfv.element_value(0) == Catch::Approx(1.0f));   // guessed by the 3 drawn
    REQUIRE(seen.size() == 1);                            // ...and not silently
    CHECK(seen[0].param_key == "lfo_waveform");
    CHECK(seen[0].ui_option_count == 3);
    CHECK(seen[0].host_step_count == 0);                  // "I cannot answer"

    // A CONTINUOUS element told 0 has no index domain either way — nothing was
    // guessed, so there is nothing to report and the channel stays quiet.
    DesignFrameView knob_view = make_single_knob("gain");
    knob_view.set_bounds({0, 0, 100, 100});
    params.values["gain"] = 0.5;
    knob_view.set_host_params(&params);
    CHECK(knob_view.element_value(0) == Catch::Approx(0.0f));
    CHECK(knob_view.param_scale_mismatches().empty());
}

TEST_CASE("a UI option count that disagrees with the host's value count is reported",
          "[view][host-param][design-import][frame]") {
    DesignFrameView dfv = make_three_option_tabs("lfo_waveform", 0);
    dfv.set_bounds({0, 0, 200, 100});

    FakeHostParamSurface params;
    params.values["lfo_waveform"] = 0.0;
    params.steps["lfo_waveform"] = 6;
    dfv.set_host_params(&params);

    std::vector<pulp::view::ParamScaleMismatch> seen;
    dfv.set_on_param_scale_mismatch(
        [&](const pulp::view::ParamScaleMismatch& m) { seen.push_back(m); });

    (void)dfv.element_value(0);            // any normalize touches the resolution
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].param_key == "lfo_waveform");
    CHECK(seen[0].ui_option_count == 3);   // what the design drew
    CHECK(seen[0].host_step_count == 6);   // what the parameter really has

    // De-duplicated by key: a repaint-rate normalize does not spam the channel.
    (void)dfv.element_value(0);
    (void)dfv.element_value(0);
    CHECK(seen.size() == 1);
    CHECK(dfv.param_scale_mismatches().size() == 1);
}

TEST_CASE("a matching UI option count reports no mismatch",
          "[view][host-param][design-import][frame]") {
    // The signal must be quiet when the binding is right, or it is noise.
    DesignFrameView dfv = make_three_option_tabs("lfo_waveform", 1);
    dfv.set_bounds({0, 0, 200, 100});

    FakeHostParamSurface params;
    params.values["lfo_waveform"] = 0.0;
    params.steps["lfo_waveform"] = 3;      // three values, three drawn positions
    dfv.set_host_params(&params);

    int reports = 0;
    dfv.set_on_param_scale_mismatch([&](const pulp::view::ParamScaleMismatch&) { ++reports; });

    CHECK(dfv.element_value(0) == Catch::Approx(0.5f));   // index 1 of 3 -> 1/2
    CHECK(reports == 0);
    CHECK(dfv.param_scale_mismatches().empty());
}

TEST_CASE("a mismatch callback attached late replays what was already seen",
          "[view][host-param][design-import][frame]") {
    DesignFrameView dfv = make_three_option_tabs("lfo_waveform", 0);
    dfv.set_bounds({0, 0, 200, 100});

    FakeHostParamSurface params;
    params.values["lfo_waveform"] = 0.0;
    params.steps["lfo_waveform"] = 6;
    dfv.set_host_params(&params);

    (void)dfv.element_value(0);            // mismatch observed with no callback set
    REQUIRE(dfv.param_scale_mismatches().size() == 1);

    std::vector<pulp::view::ParamScaleMismatch> seen;
    dfv.set_on_param_scale_mismatch(
        [&](const pulp::view::ParamScaleMismatch& m) { seen.push_back(m); });
    REQUIRE(seen.size() == 1);             // replayed, not lost
    CHECK(seen[0].host_step_count == 6);
}

TEST_CASE("commit_discrete derives the divisor from the host, not from the caller",
          "[view][host-param][design-import][frame]") {
    DesignFrameView dfv = make_three_option_tabs("lfo_waveform", 0);
    dfv.set_bounds({0, 0, 200, 100});

    FakeHostParamSurface params;
    params.values["lfo_waveform"] = 0.0;
    params.steps["lfo_waveform"] = 6;
    dfv.set_host_params(&params);
    dfv.route_changes_to_host_params(true);

    dfv.commit_discrete("lfo_waveform", 2);
    CHECK(params.values["lfo_waveform"] == Catch::Approx(2.0 / 5.0));

    dfv.commit_discrete("lfo_waveform", 5);           // the top host value...
    CHECK(params.values["lfo_waveform"] == Catch::Approx(1.0));

    // ...and an index past the host's domain clamps into it rather than
    // emitting past 1.0.
    dfv.commit_discrete("lfo_waveform", 99);
    CHECK(params.values["lfo_waveform"] == Catch::Approx(1.0));
    dfv.commit_discrete("lfo_waveform", -3);
    CHECK(params.values["lfo_waveform"] == Catch::Approx(0.0));
}

TEST_CASE("commit_discrete against a parameter with no index domain refuses to guess",
          "[view][host-param][design-import][frame]") {
    // A continuous parameter behind a bind-grid stand-in: no options drawn and no
    // host cardinality, so there is no divisor. Emitting anything would be a
    // guess, so the commit is refused AND reported.
    DesignFrameView dfv = make_three_option_tabs("lfo_waveform", 0);
    dfv.set_bounds({0, 0, 200, 100});

    FakeHostParamSurface params;
    params.values["cutoff"] = 0.25;
    params.steps["cutoff"] = 0;                 // continuous
    dfv.set_host_params(&params);
    dfv.route_changes_to_host_params(true);
    dfv.build_bind_grid({"cutoff"});

    std::vector<pulp::view::ParamScaleMismatch> seen;
    dfv.set_on_param_scale_mismatch(
        [&](const pulp::view::ParamScaleMismatch& m) { seen.push_back(m); });

    const int before = params.set_calls;
    dfv.commit_discrete("cutoff", 1);
    CHECK(params.set_calls == before);          // nothing guessed at the host
    CHECK(params.values["cutoff"] == Catch::Approx(0.25));
    REQUIRE(seen.size() == 1);                  // and it was not silent
    CHECK(seen[0].param_key == "cutoff");
}

TEST_CASE("commit_value clamps and brackets exactly one gesture",
          "[view][host-param][design-import][frame]") {
    DesignFrameView dfv = make_single_knob("gain");
    dfv.set_bounds({0, 0, 100, 100});

    FakeHostParamSurface params;
    params.values["gain"] = 0.0;
    dfv.set_host_params(&params);
    dfv.route_changes_to_host_params(true);

    std::vector<std::string> order;
    dfv.on_gesture_begin   = [&](int i) { CHECK(i == 0); order.push_back("begin"); };
    dfv.on_element_changed = [&](int i, float) { CHECK(i == 0); order.push_back("change"); };
    dfv.on_gesture_end     = [&](int i) { CHECK(i == 0); order.push_back("end"); };

    dfv.commit_value("gain", 0.75f);
    CHECK(params.values["gain"] == Catch::Approx(0.75));
    CHECK(dfv.element_value(0) == Catch::Approx(0.75f));
    // One edit == one undo step: begin -> change -> end, in that order, once.
    CHECK(order == std::vector<std::string>{"begin", "change", "end"});
    CHECK(params.gesture_log == std::vector<std::string>{"begin:gain", "end:gain"});

    // Out-of-range input is clamped, not emitted raw.
    dfv.commit_value("gain", 4.2f);
    CHECK(params.values["gain"] == Catch::Approx(1.0));
    dfv.commit_value("gain", -4.2f);
    CHECK(params.values["gain"] == Catch::Approx(0.0));
}

TEST_CASE("commit_bipolar round-trips -1..1 through normalized 0..1",
          "[view][host-param][design-import][frame]") {
    DesignFrameView dfv = make_single_knob("pan");
    dfv.set_bounds({0, 0, 100, 100});

    FakeHostParamSurface params;
    params.values["pan"] = 0.0;
    dfv.set_host_params(&params);
    dfv.route_changes_to_host_params(true);

    struct Case { float depth; double norm; };
    const Case cases[] = {
        {-1.0f, 0.0},    // hard left
        {-0.5f, 0.25},
        { 0.0f, 0.5},    // center — the value a bipolar control rests at
        { 0.5f, 0.75},
        { 1.0f, 1.0},    // hard right
    };
    for (const Case& c : cases) {
        dfv.commit_bipolar("pan", c.depth);
        CHECK(params.values["pan"] == Catch::Approx(c.norm));
        // ...and back: normalized * 2 - 1 recovers the depth.
        CHECK(params.values["pan"] * 2.0 - 1.0 == Catch::Approx(c.depth));
    }

    // Beyond the bipolar range is clamped, never wrapped past normalized 0..1.
    dfv.commit_bipolar("pan", 9.0f);
    CHECK(params.values["pan"] == Catch::Approx(1.0));
    dfv.commit_bipolar("pan", -9.0f);
    CHECK(params.values["pan"] == Catch::Approx(0.0));
}

TEST_CASE("committing to a key no element carries is reported, never silent",
          "[view][host-param][design-import][frame]") {
    DesignFrameView dfv = make_single_knob("gain");
    dfv.set_bounds({0, 0, 100, 100});

    FakeHostParamSurface params;
    params.values["gain"] = 0.0;
    params.values["lfo_waveform"] = 0.0;
    params.steps["lfo_waveform"] = 6;
    dfv.set_host_params(&params);
    dfv.route_changes_to_host_params(true);

    std::vector<pulp::view::ParamScaleMismatch> seen;
    dfv.set_on_param_scale_mismatch(
        [&](const pulp::view::ParamScaleMismatch& m) { seen.push_back(m); });

    int changes = 0;
    dfv.on_element_changed = [&](int, float) { ++changes; };

    // No control and no bind grid for this key: the view was never told about it.
    dfv.commit_value("lfo_waveform", 0.4f);
    CHECK(changes == 0);                       // no bogus index handed to a consumer
    CHECK(params.values["lfo_waveform"] == Catch::Approx(0.0));
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].param_key == "lfo_waveform");
    CHECK(seen[0].ui_option_count == 0);       // 0 vs N reads as "unbound key"
    CHECK(seen[0].host_step_count == 6);       // the host's real count, not a placeholder

    // Building the grid gives the key an element, and the same commit lands.
    dfv.build_bind_grid({"lfo_waveform"});
    dfv.commit_value("lfo_waveform", 0.4f);
    CHECK(params.values["lfo_waveform"] == Catch::Approx(0.4));
}

TEST_CASE("bind-grid stand-ins are invisible, off-screen, and take no hits",
          "[view][host-param][design-import][frame]") {
    DesignFrameView dfv = make_single_knob("gain");
    dfv.set_bounds({0, 0, 100, 100});

    FakeHostParamSurface params;
    params.values["gain"] = 0.0;
    for (int i = 0; i < 32; ++i) params.values["p" + std::to_string(i)] = 0.0;
    dfv.set_host_params(&params);

    std::vector<std::string> keys;
    for (int i = 0; i < 32; ++i) keys.push_back("p" + std::to_string(i));
    dfv.build_bind_grid(keys);

    REQUIRE(dfv.element_count() == 33);        // the design's knob + 32 stand-ins
    CHECK_FALSE(dfv.element_is_bind_grid_stand_in(0));   // the real knob

    for (int i = 1; i < dfv.element_count(); ++i) {
        REQUIRE(dfv.element_is_bind_grid_stand_in(i));
        CHECK_FALSE(dfv.element_enabled(i));             // not hit-testable
        const auto r = dfv.element_rect(i);
        CHECK(r.width == 0.0f);                          // no on-screen geometry
        CHECK(r.height == 0.0f);
        CHECK(r.x < -1000.0f);                           // parked far off-panel
        CHECK(r.y < -1000.0f);
    }

    // Sweep the whole panel: no point anywhere hits a stand-in. Only the design's
    // own knob (index 0) is ever hovered.
    for (float y = 0; y <= 100; y += 5) {
        for (float x = 0; x <= 100; x += 5) {
            dfv.simulate_hover({x, y});
            const int hit = dfv.element_hovered();
            REQUIRE_FALSE(dfv.element_is_bind_grid_stand_in(hit));
        }
    }

    // A stand-in still binds both directions — that is what it is for.
    params.values["p7"] = 0.6;
    dfv.sync_from_host_params();
    const int i7 = dfv.element_for_param_key("p7");
    REQUIRE(i7 >= 0);
    CHECK(dfv.element_value(i7) == Catch::Approx(0.6f));   // host -> UI

    dfv.route_changes_to_host_params(true);
    dfv.commit_value("p7", 0.2f);
    CHECK(params.values["p7"] == Catch::Approx(0.2));      // UI -> host
}

TEST_CASE("a design's own control wins over a bind-grid stand-in",
          "[view][host-param][design-import][frame]") {
    DesignFrameView dfv = make_single_knob("gain");
    dfv.set_bounds({0, 0, 100, 100});

    FakeHostParamSurface params;
    params.values["gain"] = 0.0;
    params.values["mix"] = 0.0;
    dfv.set_host_params(&params);

    // "gain" already has a real control; only "mix" needs a stand-in.
    dfv.build_bind_grid({"gain", "mix"});
    CHECK(dfv.element_count() == 2);
    CHECK(dfv.element_for_param_key("gain") == 0);          // the design's knob
    CHECK_FALSE(dfv.element_is_bind_grid_stand_in(0));
    CHECK(dfv.element_is_bind_grid_stand_in(1));            // the "mix" stand-in

    // bind_grid_keys() reports what was asked for, including the skipped key.
    CHECK(dfv.bind_grid_keys() == std::vector<std::string>{"gain", "mix"});

    // Repeated calls REPLACE the grid rather than accumulating stand-ins.
    dfv.build_bind_grid({"gain", "mix"});
    CHECK(dfv.element_count() == 2);
    dfv.build_bind_grid({});
    CHECK(dfv.element_count() == 1);                        // grid gone, knob intact
    CHECK(dfv.element_for_param_key("gain") == 0);
}

TEST_CASE("the bind grid survives a frame swap and re-fits the incoming frame",
          "[view][host-param][design-import][frame]") {
    // Frame A draws a real "mix" control; frame B does not. The same key must be
    // bound on BOTH — via the control on A, via a stand-in on B — or a swap
    // silently drops the binding.
    const std::string svg =
        R"(<svg width="100" height="100"><rect x="0" y="0" width="100" height="100"/></svg>)";
    DesignFrameElement mix;
    mix.kind = DesignFrameElement::Kind::knob;
    mix.x = 10; mix.y = 10; mix.w = 20; mix.h = 20;
    mix.cx = 20; mix.cy = 20; mix.hit_radius = 20.0f;
    mix.param_key = "mix";

    DesignFrameView dfv(svg, {mix}, 0, 0, 100, 100);   // frame A
    dfv.set_bounds({0, 0, 100, 100});

    DesignFrameElement other;                          // frame B: a DIFFERENT param
    other.kind = DesignFrameElement::Kind::knob;
    other.x = 10; other.y = 10; other.w = 20; other.h = 20;
    other.cx = 20; other.cy = 20; other.hit_radius = 20.0f;
    other.param_key = "tone";
    const int frame_b = dfv.add_frame(svg, {other}, 0, 0, 100, 100);

    FakeHostParamSurface params;
    params.values["mix"] = 0.0;
    params.values["tone"] = 0.0;
    dfv.set_host_params(&params);
    dfv.route_changes_to_host_params(true);
    dfv.build_bind_grid({"mix", "tone"});

    // Frame A: "mix" is the design's own control, "tone" is a stand-in.
    REQUIRE(dfv.element_count() == 2);
    CHECK_FALSE(dfv.element_is_bind_grid_stand_in(dfv.element_for_param_key("mix")));
    CHECK(dfv.element_is_bind_grid_stand_in(dfv.element_for_param_key("tone")));

    dfv.set_active_frame(frame_b);

    // Frame B: the roles swap, and the design's own controls were NOT eaten by
    // the grid's rebuild.
    REQUIRE(dfv.element_count() == 2);
    const int tone_i = dfv.element_for_param_key("tone");
    const int mix_i  = dfv.element_for_param_key("mix");
    REQUIRE(tone_i >= 0);
    REQUIRE(mix_i >= 0);
    CHECK_FALSE(dfv.element_is_bind_grid_stand_in(tone_i));   // now a real control
    CHECK(dfv.element_is_bind_grid_stand_in(mix_i));          // now a stand-in

    // Both keys still commit on frame B.
    dfv.commit_value("mix", 0.3f);
    dfv.commit_value("tone", 0.8f);
    CHECK(params.values["mix"] == Catch::Approx(0.3));
    CHECK(params.values["tone"] == Catch::Approx(0.8));
}


TEST_CASE("a real store scales a real view's choice control by the parameter",
          "[view][host-param][state][design-import][frame]") {
    // The PRODUCTION seam, joined end to end: a real StateStore, the real
    // StateStoreHostParamSurface that core/format/src/view_bridge.cpp constructs
    // and hands to set_host_params, and a real DesignFrameView. Every other test
    // here proves one half — a view against a hand-populated fake, or the surface
    // standalone against a store — and the halves passing does not prove the
    // seam between them carries the count.
    //
    // This is the reported bug verbatim: a 3-position control on a 6-value
    // parameter must emit idx/5, never idx/2, with no per-control override.
    state::StateStore store;
    state::ParamInfo waveform;
    waveform.id = 1;
    waveform.name = "lfo_waveform";
    waveform.kind = state::ParamKind::Enum;
    waveform.range = state::ParamRange::linear(0.0f, 5.0f, 0.0f, 1.0f);
    waveform.value_labels = {"Sine", "Triangle", "Saw", "Ramp", "Square", "Random"};
    store.add_parameter(waveform);

    // The resolver view_bridge uses: a design param_key names a registered param.
    StateStoreHostParamSurface surface(store);

    // The store's count reaches the view's divisor with nothing in between:
    // every drawn position is idx/5, never idx/2. A fresh view per index so each
    // selection is the one the design authored, not a round-trip artifact.
    const double expected[] = {0.0, 0.2, 0.4};
    for (int idx = 0; idx < 3; ++idx) {
        DesignFrameView dfv = make_three_option_tabs("lfo_waveform", idx);
        dfv.set_bounds({0, 0, 200, 100});
        dfv.set_host_params(&surface);
        CHECK(dfv.element_value(0) == Catch::Approx(expected[idx]));
    }

    // The value the option list would have produced, proven distinct: index 2 of
    // 3 drawn options is 2/2 == 1.0, which selects the parameter's SIXTH value
    // instead of its third. 0.4 != 1.0 is the entire customer report.
    DesignFrameView dfv = make_three_option_tabs("lfo_waveform", 2);
    dfv.set_bounds({0, 0, 200, 100});
    dfv.set_host_params(&surface);
    CHECK(dfv.element_value(0) != Catch::Approx(1.0f));

    // And the 3-of-6 binding is reported, since the design draws half the range.
    REQUIRE(dfv.param_scale_mismatches().size() == 1);
    const auto& m = dfv.param_scale_mismatches()[0];
    CHECK(m.param_key == "lfo_waveform");
    CHECK(m.ui_option_count == 3);
    CHECK(m.host_step_count == 6);     // straight from the store's value_labels
    CHECK(m.host_has_param);

    // ...and the value the view emits lands on the parameter the author
    // declared: index 2 of 6, not index 5. The customer's report, closed.
    dfv.route_changes_to_host_params(true);
    dfv.commit_discrete("lfo_waveform", 2);
    CHECK(surface.get_param("lfo_waveform") == Catch::Approx(0.4));
    CHECK(store.get_value(1) == Catch::Approx(2.0f));   // the THIRD label ("Saw")
}

TEST_CASE("a refused discrete commit reports the HOST's count, not the view's",
          "[view][host-param][design-import][frame]") {
    // host_step_count is documented as "values the host's parameter exposes". A
    // refused commit must honor that even when the count it refused on came from
    // somewhere else: a single-option control resolves count == 1 from its OWN
    // option list (the UI fallback), and echoing that 1 into host_step_count
    // would point the reader at the host for a number the view invented — a
    // diagnostic misstating its own provenance, on the one path a reader consults
    // BECAUSE something is already wrong.
    const std::string svg =
        R"(<svg width="200" height="100"><rect x="0" y="0" width="200" height="100"/></svg>)";
    DesignFrameElement one;
    one.kind = DesignFrameElement::Kind::tab_group;
    one.x = 10; one.y = 10; one.w = 90; one.h = 20;
    one.options = {"Only"};                 // ONE position: no index domain
    one.param_key = "mode";
    DesignFrameView dfv(svg, {one}, 0, 0, 200, 100);
    dfv.set_bounds({0, 0, 200, 100});

    FakeHostParamSurface params;
    params.values["mode"] = 0.0;
    params.steps["mode"] = 0;               // the host says "continuous"
    dfv.set_host_params(&params);
    dfv.route_changes_to_host_params(true);

    const int before = params.set_calls;
    dfv.commit_discrete("mode", 0);
    CHECK(params.set_calls == before);      // refused: no divisor anywhere

    REQUIRE(dfv.param_scale_mismatches().size() == 1);
    const auto& m = dfv.param_scale_mismatches()[0];
    CHECK(m.ui_option_count == 1);          // the view's own count, in the view's field
    CHECK(m.host_step_count == 0);          // what the HOST actually said — never the 1
    CHECK(m.host_has_param);                // the key is real; only the domain is absent
}

namespace {

// Counts repaint() calls. The plugin-editor path is the one that matters here:
// View::request_repaint calls plugin_view_host_->repaint() DIRECTLY, with none of
// the mark_dirty() coalescing the window-host path has, so every request is a
// real editor repaint.
class CountingPluginViewHost : public pulp::view::PluginViewHost {
public:
    int repaints = 0;

    void repaint() override { ++repaints; }

    // Not exercised: this fake exists to observe repaint(), not to host a view.
    pulp::view::NativeViewHandle native_handle() override { return nullptr; }
    void attach_to_parent(pulp::view::NativeViewHandle) override {}
    void detach() override {}
    void set_size(uint32_t, uint32_t) override {}
    Size get_size() const override { return {100, 100}; }
};

} // namespace

TEST_CASE("an unchanged host push does not repaint the editor",
          "[view][host-param][design-import][frame]") {
    // sync_from_host_params pushes EVERY element every tick, bind-grid stand-ins
    // included. set_element_value must therefore early-return on no change, the
    // way set_element_text already does — otherwise a steady host (the common
    // case: nothing is being automated) repaints the editor once per parameter
    // per tick while nothing on screen has moved.
    DesignFrameView dfv = make_single_knob("gain");
    dfv.set_bounds({0, 0, 100, 100});

    CountingPluginViewHost host;
    dfv.set_plugin_view_host(&host);

    FakeHostParamSurface params;
    params.values["gain"] = 0.5;
    dfv.set_host_params(&params);

    // Counts are measured as DELTAS around each action: attaching a host and
    // wiring a surface legitimately repaint on their own, and this test is about
    // what a per-tick SYNC costs, not about pinning unrelated setup.
    int before = host.repaints;
    dfv.sync_from_host_params();                       // first push: 0 -> 0.5
    CHECK(dfv.element_value(0) == Catch::Approx(0.5f));
    // Exactly ONE. A changed value is worth a repaint; it is not worth two, and
    // this path used to ask twice — once inside the switch and once past it.
    CHECK(host.repaints == before + 1);

    // Steady state: the host keeps reporting 0.5, so nothing changes and nothing
    // needs painting. Not ONE repaint across eight ticks — this is the assertion
    // that fails without the equality check in set_element_value.
    before = host.repaints;
    for (int tick = 0; tick < 8; ++tick) dfv.sync_from_host_params();
    CHECK(host.repaints == before);
    CHECK(dfv.element_value(0) == Catch::Approx(0.5f));  // and the value holds

    // A real change still lands — the early-return is an equality check, not a
    // freeze.
    before = host.repaints;
    params.values["gain"] = 0.75;
    dfv.sync_from_host_params();
    CHECK(dfv.element_value(0) == Catch::Approx(0.75f));
    CHECK(host.repaints == before + 1);
}

// ── Paint-safe host readout ──────────────────────────────────────────────────
// element_display_text() is the read half of the host readout channel: the one
// legal from paint(). These cover the contract it advertises — alloc-free reads,
// verbatim (untruncated) text, honest unbound behavior, and tick-granular
// staleness.

namespace {

// A DesignFrameView subclass that reads its bound parameter's formatted text
// from paint() — the end-to-end shape a rack/chain UI needs for a per-slot
// readout it draws itself. Holds its key as a member: element_for_param_key
// takes a const std::string&, so passing a literal would build a temporary and
// allocate right where paint must not.
class ReadoutPaintView : public DesignFrameView {
public:
    using DesignFrameView::DesignFrameView;

    std::string key = "gain";
    std::string painted;        // what paint() saw
    bool painted_bound = false;
    int paints = 0;
    std::size_t paint_allocations = 0;

    void paint(canvas::Canvas& canvas) override {
        DesignFrameView::paint(canvas);
        // Measure only OUR read: the probe is scoped to the two calls under
        // test, so the base class's own painting cannot mask or inflate it.
        pulp::test::RtAllocationProbe probe;
        const int i = element_for_param_key(key);
        const std::string& text = element_display_text(i);
        const bool bound = element_has_display_text(i);
        paint_allocations = probe.allocation_count();
        // Copy out AFTER the probe's scope — the copy is the test's, not the
        // read path's.
        painted = text;
        painted_bound = bound;
        ++paints;
    }
};

// Paint a view through the real paint_all path, so the read happens inside the
// genuine ScopedNoAlloc region rather than a test-simulated one. A
// HostParamSurface call from here would trip its own call-context assert.
void paint_once(ReadoutPaintView& v) {
    canvas::RecordingCanvas canvas;   // headless: records draws, opens no window
    v.set_bounds({0, 0, 200, 200});
    v.paint_all(canvas);
}

ReadoutPaintView make_readout_view(const std::string& param_key) {
    const std::string svg =
        R"(<svg width="100" height="100"><rect x="0" y="0" width="100" height="100"/></svg>)";
    DesignFrameElement knob;
    knob.kind = DesignFrameElement::Kind::knob;
    knob.x = 10; knob.y = 10; knob.w = 20; knob.h = 20;
    knob.cx = 20; knob.cy = 20;
    knob.hit_radius = 20.0f;
    knob.param_key = param_key;
    return ReadoutPaintView(svg, {knob}, 0, 0, 100, 100);
}

} // namespace

TEST_CASE("a subclass reads a host param's formatted text from paint()",
          "[view][host-param][paint-safe]") {
    ReadoutPaintView dfv = make_readout_view("gain");
    FakeHostParamSurface params;
    params.values["gain"] = 0.6;
    dfv.set_host_params(&params);

    // The tick is what reaches the host. Paint only ever reads the cache.
    dfv.sync_from_host_params();
    paint_once(dfv);

    REQUIRE(dfv.paints == 1);
    CHECK(dfv.painted == "disp:gain");     // the fake's formatter output
    CHECK(dfv.painted_bound);
}

TEST_CASE("the paint-time display-text read allocates nothing",
          "[view][host-param][paint-safe][rt-safety]") {
    ReadoutPaintView dfv = make_readout_view("gain");
    FakeHostParamSurface params;
    // A value long enough to defeat the small-string optimization: were the read
    // path to copy or format, THIS is the case that would heap-allocate. A short
    // string could pass on SSO alone and prove nothing.
    params.values["a_parameter_key_far_longer_than_any_small_string_buffer"] = 0.25;
    dfv.key = "a_parameter_key_far_longer_than_any_small_string_buffer";
    dfv.set_element_param_key(0, dfv.key);
    dfv.set_host_params(&params);
    dfv.sync_from_host_params();
    REQUIRE(dfv.element_display_text(0).size() > 32);   // beyond any SSO buffer

    paint_once(dfv);

    REQUIRE(dfv.paints == 1);
    CHECK(dfv.paint_allocations == 0);
}

TEST_CASE("display text is cached verbatim, with no length cap or truncation",
          "[view][host-param][paint-safe]") {
    // The channel stores the host's string as-is: no fixed buffer, so no
    // truncation and no partial readout. A host formatter returning something
    // pathologically long is reported in full rather than silently cut.
    class LongTextSurface : public FakeHostParamSurface {
    protected:
        std::string do_param_display_text(std::string_view key, double v) override {
            (void)key; (void)v;
            return std::string(4096, 'x') + "!";
        }
    };
    ReadoutPaintView dfv = make_readout_view("gain");
    LongTextSurface params;
    params.values["gain"] = 0.5;
    dfv.set_host_params(&params);
    dfv.sync_from_host_params();

    REQUIRE(dfv.element_display_text(0).size() == 4097);
    CHECK(dfv.element_display_text(0).back() == '!');   // the tail survives

    // And the read of it is still alloc-free — capacity is a tick-side cost.
    paint_once(dfv);
    CHECK(dfv.paint_allocations == 0);
    CHECK(dfv.painted.size() == 4097);
}

TEST_CASE("a key with no host param reads as unbound, not as a stale or absent value",
          "[view][host-param][paint-safe]") {
    ReadoutPaintView dfv = make_readout_view("gain");
    FakeHostParamSurface params;   // deliberately empty: nothing resolves "gain"
    dfv.set_host_params(&params);
    dfv.sync_from_host_params();
    paint_once(dfv);

    CHECK(dfv.painted.empty());
    CHECK_FALSE(dfv.painted_bound);
    CHECK_FALSE(dfv.element_has_display_text(0));

    // An out-of-range index is the same answer, not a crash and not UB.
    CHECK(dfv.element_display_text(-1).empty());
    CHECK(dfv.element_display_text(99).empty());
    CHECK_FALSE(dfv.element_has_display_text(-1));
    CHECK_FALSE(dfv.element_has_display_text(99));

    // An element carrying no param_key at all is unbound too.
    dfv.set_element_param_key(0, "");
    dfv.sync_from_host_params();
    CHECK_FALSE(dfv.element_has_display_text(0));
}

TEST_CASE("a host that stops resolving a key clears the readout rather than lying",
          "[view][host-param][paint-safe]") {
    ReadoutPaintView dfv = make_readout_view("gain");
    FakeHostParamSurface params;
    params.values["gain"] = 0.6;
    dfv.set_host_params(&params);
    dfv.sync_from_host_params();
    REQUIRE(dfv.element_has_display_text(0));
    REQUIRE(dfv.element_display_text(0) == "disp:gain");

    // The host drops the parameter (a rack slot emptied, a preset swapped out).
    params.values.erase("gain");
    dfv.sync_from_host_params();

    // The last text must NOT survive — a readout for a parameter that no longer
    // exists is a stale lie.
    CHECK(dfv.element_display_text(0).empty());
    CHECK_FALSE(dfv.element_has_display_text(0));
}

TEST_CASE("bound-but-empty display text is distinguishable from unbound",
          "[view][host-param][paint-safe]") {
    // A host may legitimately format a value AS an empty string, so empty text
    // alone cannot mean "no parameter here". The bound flag carries that.
    class EmptyTextSurface : public FakeHostParamSurface {
    protected:
        std::string do_param_display_text(std::string_view, double) override { return {}; }
    };
    ReadoutPaintView dfv = make_readout_view("gain");
    EmptyTextSurface params;
    params.values["gain"] = 0.5;
    dfv.set_host_params(&params);
    dfv.sync_from_host_params();

    CHECK(dfv.element_display_text(0).empty());
    CHECK(dfv.element_has_display_text(0));   // bound — the host formats it empty
}

TEST_CASE("paint sees the last tick's text: the readout is tick-granular, never torn",
          "[view][host-param][paint-safe]") {
    ReadoutPaintView dfv = make_readout_view("gain");
    FakeHostParamSurface params;
    params.values["gain"] = 0.1;
    dfv.set_host_params(&params);

    // Before the FIRST sync there is nothing to show — not the host's current
    // value, because paint has never been allowed to ask.
    paint_once(dfv);
    CHECK(dfv.painted.empty());
    CHECK_FALSE(dfv.painted_bound);

    dfv.sync_from_host_params();
    paint_once(dfv);
    CHECK(dfv.painted == "disp:gain");

    // The host's value moves BETWEEN ticks (automation running under a frame).
    // Paint keeps reporting the last synced text: one tick, one consistent
    // snapshot. Repainting does not re-reach the host and cannot re-derive it.
    class CountingSurface : public FakeHostParamSurface {
    public:
        int text_calls = 0;
    protected:
        std::string do_param_display_text(std::string_view key, double v) override {
            ++text_calls;
            (void)v;
            return "disp:" + std::string(key);
        }
    };
    CountingSurface counting;
    counting.values["gain"] = 0.1;
    dfv.set_host_params(&counting);
    dfv.sync_from_host_params();
    const int after_tick = counting.text_calls;
    REQUIRE(after_tick == 1);

    paint_once(dfv);
    paint_once(dfv);
    paint_once(dfv);
    // Three paints, zero extra host formatter calls: the host round-trip is
    // per-tick, not per-frame.
    CHECK(counting.text_calls == after_tick);
    CHECK(dfv.painted == "disp:gain");
}

TEST_CASE("every host parameter has a readout once the bind grid is built",
          "[view][host-param][paint-safe]") {
    // The keyed readout the ask actually needs: text for a parameter the design
    // draws no control for. build_bind_grid gives that key an element, and the
    // element carries the cached text.
    ReadoutPaintView dfv = make_readout_view("gain");
    FakeHostParamSurface params;
    params.values["gain"] = 0.5;
    params.values["undrawn_mix"] = 0.25;    // no control in the design
    dfv.set_host_params(&params);

    REQUIRE(dfv.element_for_param_key("undrawn_mix") == -1);   // no element yet
    dfv.build_bind_grid({"gain", "undrawn_mix"});
    const int i = dfv.element_for_param_key("undrawn_mix");
    REQUIRE(i >= 0);
    REQUIRE(dfv.element_is_bind_grid_stand_in(i));

    dfv.sync_from_host_params();
    CHECK(dfv.element_has_display_text(i));
    CHECK(dfv.element_display_text(i) == "disp:undrawn_mix");

    // And it paints, keyed, alloc-free — the getEffectParamText shape.
    dfv.key = "undrawn_mix";
    paint_once(dfv);
    CHECK(dfv.painted == "disp:undrawn_mix");
    CHECK(dfv.paint_allocations == 0);
}

TEST_CASE("a value_label keeps its own painted readout alongside the cache",
          "[view][host-param][paint-safe]") {
    // display_text must not clobber, or be clobbered by, the author-owned `text`
    // channel: both are populated for a value_label, and set_element_text on a
    // non-label element leaves the host cache intact.
    const std::string svg = R"(<svg width="100" height="100"/>)";
    DesignFrameElement label;
    label.kind = DesignFrameElement::Kind::value_label;
    label.x = 10; label.y = 10; label.w = 40; label.h = 10;
    label.param_key = "gain";
    DesignFrameView dfv(svg, {label}, 0, 0, 100, 100);

    FakeHostParamSurface params;
    params.values["gain"] = 0.6;
    dfv.set_host_params(&params);
    dfv.sync_from_host_params();

    CHECK(dfv.element_text(0) == "disp:gain");           // the label still paints
    CHECK(dfv.element_display_text(0) == "disp:gain");   // and the cache carries it
    CHECK(dfv.element_has_display_text(0));
}
