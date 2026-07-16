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
    std::vector<std::string> gesture_log;   // "begin:key" / "end:key"
    int set_calls = 0;

protected:
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

    // And the count really IS the denominator the element normalizes against:
    // option i of 3 maps to i/2, not i/(some guess).
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
