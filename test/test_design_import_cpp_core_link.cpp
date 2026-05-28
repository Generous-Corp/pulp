#include "chainer-phase-f-hybrid.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/design_import.hpp>

#include <string>
#include <string_view>
#include <unordered_set>

namespace {

class CountingBindingContext final : public pulp::view::NativeImportBindingContext {
public:
    void bind_knob(pulp::view::Knob&, const pulp::view::NativeImportBindingDescriptor& descriptor) override {
        add_param(descriptor.param_key);
        ++knobs;
    }

    void bind_fader(pulp::view::Fader&, const pulp::view::NativeImportBindingDescriptor& descriptor) override {
        add_param(descriptor.param_key);
        ++faders;
    }

    void bind_xy_pad(pulp::view::XYPad&, const pulp::view::NativeImportXYPadBindingDescriptor& descriptor) override {
        add_param(descriptor.x_param_key);
        add_param(descriptor.y_param_key);
        ++xy_pads;
    }

    void bind_toggle_button(pulp::view::ToggleButton&,
                            const pulp::view::NativeImportBindingDescriptor& descriptor) override {
        add_param(descriptor.param_key);
        ++toggles;
    }

    void bind_choice_button(pulp::view::ToggleButton&,
                            const pulp::view::NativeImportChoiceBindingDescriptor& descriptor) override {
        add_param(descriptor.param_key);
        ++choices;
    }

    void bind_meter(pulp::view::Meter&, const pulp::view::NativeImportMeterBindingDescriptor& descriptor) override {
        if (!descriptor.channel.empty())
            meter_channels.insert(std::string(descriptor.channel));
        ++meters;
    }

    void bind_waveform_display(pulp::view::WaveformView&,
                               const pulp::view::NativeImportWaveformBindingDescriptor& descriptor) override {
        add_param(descriptor.param_key);
        ++waveform_displays;
    }

    int knobs = 0;
    int faders = 0;
    int xy_pads = 0;
    int toggles = 0;
    int choices = 0;
    int meters = 0;
    int waveform_displays = 0;
    std::unordered_set<std::string> param_keys;
    std::unordered_set<std::string> meter_channels;

private:
    void add_param(std::string_view key) {
        if (!key.empty())
            param_keys.insert(std::string(key));
    }
};

}  // namespace

TEST_CASE("Phase F generated Chainer C++ links through view-core only") {
    auto root = pulp::test::phase_f_chainer_hybrid::build_chainer_phase_f_hybrid_ui();
    REQUIRE(root != nullptr);

    CountingBindingContext bindings;
    pulp::test::phase_f_chainer_hybrid::bind_chainer_phase_f_hybrid_ui(*root, bindings);

    REQUIRE(bindings.knobs == 8);
    REQUIRE(bindings.faders == 6);
    REQUIRE(bindings.xy_pads == 1);
    REQUIRE(bindings.toggles == 2);
    REQUIRE(bindings.choices == 21);
    REQUIRE(bindings.meters == 2);
    REQUIRE(bindings.waveform_displays == 1);
    REQUIRE(bindings.param_keys.size() == 20);
    for (const auto* key : {
             "osc_freq",
             "osc_detune",
             "osc_shape",
             "osc_waveform",
             "env_a",
             "env_d",
             "env_s",
             "env_r",
             "xover_lo",
             "xover_hi",
             "ms_mid_width",
             "ms_side_width",
             "filt_x",
             "filt_y",
             "send_level",
             "return_level",
             "master_out",
             "mid_bypass",
             "side_bypass",
         }) {
        REQUIRE(bindings.param_keys.count(key) == 1);
    }
    REQUIRE(bindings.param_keys.count("selected_mod") == 1);
    REQUIRE(bindings.meter_channels.size() == 2);
    REQUIRE(bindings.meter_channels.count("L") == 1);
    REQUIRE(bindings.meter_channels.count("R") == 1);
}
