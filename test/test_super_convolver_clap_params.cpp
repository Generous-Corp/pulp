// CLAP parameter-display coverage for the SuperConvolver example.
//
// CLAP deliberately puts no `unit` field on clap_param_info: a host renders a
// parameter by calling clap_plugin_params.value_to_text(), which is where a
// Pulp plugin's ParamInfo::unit ("%", "s", "dB") surfaces. Anything that reads
// only the info struct — a DAW, a web host, a generated widget grid — shows a
// bare number unless it goes through this call. These tests pin the exact
// strings SuperConvolver's parameters render, because they are also the golden
// strings the WAM / WebCLAP browser demos assert against
// (examples/web-demos/wasm-build/superconvolver_runner.mjs): the two web ABIs
// must display identical parameter text, and this is the C++ end of that
// contract.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/clap_adapter.hpp>
#include <pulp/format/clap_entry.hpp>

#include "super_convolver.hpp"

#include <cstring>
#include <string>

namespace {

using pulp::format::clap_adapter::PulpClapPlugin;

// Bring up the CLAP adapter around SuperConvolver exactly as the generated
// entry point does (create_plugin → clap_init): the factory builds the
// processor and define_parameters() populates the store the params extension
// answers from. Only the parameter surface is exercised; the plugin is never
// activated, so no audio/GPU resources are touched.
struct ClapFixture {
    PulpClapPlugin* self = new PulpClapPlugin();

    ClapFixture() {
        self->factory = pulp::examples::create_super_convolver;
        self->plugin.plugin_data = self;
        REQUIRE(pulp::format::clap_adapter::clap_init(&self->plugin));
    }
    ~ClapFixture() { pulp::format::clap_adapter::clap_destroy(&self->plugin); }

    std::string text(pulp::state::ParamID id, double value) const {
        char buf[64];
        std::memset(buf, 0, sizeof(buf));
        REQUIRE(pulp::format::clap_generic::params_value_to_text(
            &self->plugin, static_cast<clap_id>(id), value, buf, sizeof(buf)));
        return std::string(buf);
    }
};

} // namespace

TEST_CASE("SuperConvolver's CLAP value_to_text renders each parameter's unit",
          "[superconvolver][clap][params]") {
    ClapFixture fx;

    // The three knobs the web demos expose, at their default values — the exact
    // readouts the demo pages show on first paint.
    CHECK(fx.text(pulp::examples::kMix, 35.0) == "35.00 %");
    CHECK(fx.text(pulp::examples::kSize, 1.5) == "1.50 s");
    CHECK(fx.text(pulp::examples::kGain, 0.0) == "0.00 dB");

    // Away from the defaults, including a negative value (Gain is -24..+24 dB).
    CHECK(fx.text(pulp::examples::kMix, 100.0) == "100.00 %");
    CHECK(fx.text(pulp::examples::kSize, 0.05) == "0.05 s");
    CHECK(fx.text(pulp::examples::kGain, -6.5) == "-6.50 dB");

    // Bypass is a SWITCH, and it says so: ParamKind::Toggle plus value_labels.
    // A host therefore renders it "Off"/"On" and never the bare "0.00" a
    // continuous parameter would produce — a number is not a state, and a DAW
    // showing "0.00" for bypass tells the user nothing.
    CHECK(fx.text(pulp::examples::kBypass, 0.0) == "Off");
    CHECK(fx.text(pulp::examples::kBypass, 1.0) == "On");
}

TEST_CASE("SuperConvolver's CLAP params round-trip display text back to a value",
          "[superconvolver][clap][params]") {
    ClapFixture fx;

    // A host hands the unit-suffixed string it displayed straight back on a
    // typed-in edit; text_to_value must accept it (the suffix is tolerated).
    double value = 0.0;
    REQUIRE(pulp::format::clap_generic::params_text_to_value(
        &fx.self->plugin, static_cast<clap_id>(pulp::examples::kMix), "35.00 %", &value));
    CHECK(value == 35.0);

    REQUIRE(pulp::format::clap_generic::params_text_to_value(
        &fx.self->plugin, static_cast<clap_id>(pulp::examples::kGain), "-6.50 dB", &value));
    CHECK(value == -6.5);

    // The switch round-trips through its LABEL, not through a number: the host
    // displayed "On", so "On" is what it hands back on a typed edit.
    REQUIRE(pulp::format::clap_generic::params_text_to_value(
        &fx.self->plugin, static_cast<clap_id>(pulp::examples::kBypass), "On", &value));
    CHECK(value == 1.0);
    REQUIRE(pulp::format::clap_generic::params_text_to_value(
        &fx.self->plugin, static_cast<clap_id>(pulp::examples::kBypass), "Off", &value));
    CHECK(value == 0.0);
}
