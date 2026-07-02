// Tests for the SDK runtime host-parameter surface (JUCE port accelerator P1/P2):
//   - StateStoreHostParamSurface: the native backing (get/set/gesture/display).
//   - DesignFrameView integration: sync_from_host_params (host->UI snapshot),
//     route_changes_to_host_params (UI->host), set_element_param_key re-key,
//     and action routing to HostActionSurface.
//   - A framework-agnostic FakeHostParamSurface proves a view binds against the
//     interface, not the StateStore concretely (the "port once, three hosts"
//     ratchet, exercised here with a fake stand-in for the JUCE/iPlug backings).

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <pulp/state/store.hpp>
#include <pulp/view/design_frame_view.hpp>
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

TEST_CASE("DesignFrameView tracks per-element hover (P6.2)", "[view][hover][issue-5230]") {
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

TEST_CASE("A disabled element is not hit-testable (P6.2)", "[view][hover][issue-5230]") {
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
