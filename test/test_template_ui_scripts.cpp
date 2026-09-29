// The `pulp create` UI scripts are the first scripted editor most projects
// start from, so they must load in the real bridge and keep their live data off
// the per-tick script path: the gain template's meter is bound natively to a
// processor value channel, and its readout has a pinned width.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/value_channel_set.hpp>
#include <pulp/view/value_source.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/widgets.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace pulp::view;
using namespace pulp::state;
using Catch::Matchers::WithinAbs;

namespace {

std::string template_script(const char* template_name) {
    const auto path = std::filesystem::path(PULP_SOURCE_DIR) / "tools" / "templates" /
                      template_name / "ui" / "main.js";
    std::ifstream in(path);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    std::string script = ss.str();
    // `pulp create` substitutes this before the script ever runs.
    const std::string placeholder = "{{PLUGIN_NAME}}";
    for (auto at = script.find(placeholder); at != std::string::npos;
         at = script.find(placeholder, at)) {
        script.replace(at, placeholder.size(), "Template");
    }
    return script;
}

void add_gain_params(StateStore& store) {
    store.add_parameter(
        {.id = 1, .name = "Gain", .unit = "dB", .range = {-60.0f, 24.0f, 0.0f, 0.1f}});
    store.add_parameter({.id = 2, .name = "Bypass", .unit = "", .range = {0.0f, 1.0f, 0.0f, 1.0f}});
}

MeterFrame stereo(float level) {
    MeterFrame frame{};
    frame.channels = 2;
    frame.rms[0] = frame.rms[1] = level;
    frame.peak[0] = frame.peak[1] = level;
    return frame;
}

} // namespace

TEST_CASE("gain template meter follows the output value channel natively",
          "[view][bridge][templates][value-channel]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_gain_params(store);

    ValueChannelSet channels;
    auto* output = channels.declare_meter("output");
    REQUIRE(output != nullptr);

    WidgetBridge bridge(engine, root, store);
    bridge.set_value_channels(&channels);
    bridge.load_script(template_script("gain"));

    auto* meter = dynamic_cast<Meter*>(bridge.widget("out-meter"));
    REQUIRE(meter != nullptr);

    // No script runs between these publishes: the binding service alone moves
    // the meter, which is what keeps per-tick data off the JS path.
    output->publish(stereo(0.25f));
    bridge.service_param_bindings();
    REQUIRE_THAT(meter->display_rms(), WithinAbs(0.25f, 1e-5f));

    output->publish(stereo(0.75f));
    bridge.service_param_bindings();
    REQUIRE_THAT(meter->display_rms(), WithinAbs(0.75f, 1e-5f));
}

TEST_CASE("gain template readout has a pinned width", "[view][bridge][templates][layout]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_gain_params(store);
    ValueChannelSet channels;
    REQUIRE(channels.declare_meter("output") != nullptr);

    WidgetBridge bridge(engine, root, store);
    bridge.set_value_channels(&channels);
    bridge.load_script(template_script("gain"));

    auto* readout = bridge.widget("readout");
    REQUIRE(readout != nullptr);
    // An intrinsic-width readout re-runs layout on every digit change.
    REQUIRE_THAT(readout->flex().preferred_width, WithinAbs(72.0f, 1e-3f));
}

TEST_CASE("design-import template placeholders load in the bridge", "[view][bridge][templates]") {
    for (const char* name : {"from-figma", "from-v0"}) {
        ScriptEngine engine;
        View root;
        root.set_bounds({0, 0, 400, 300});
        root.set_theme(Theme::dark());
        StateStore store;
        WidgetBridge bridge(engine, root, store);
        bridge.load_script(template_script(name));
        CHECK(bridge.widget("title") != nullptr);
    }
}
