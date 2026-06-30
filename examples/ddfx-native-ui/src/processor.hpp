#pragma once
// Knob Playground — minimal native Pulp effect: a single "Macro" param (0..1)
// driving output gain, with a native GPU UI that draws Dream Date FX's macro
// knob (cream ring + purple triangle). Goal: prove render + drag + param
// binding end-to-end in the cleanest possible native-Pulp environment before
// porting the pattern back into a full editor.
//
// Mirrors examples/bendr/src/reference_processor.hpp in shape.

#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>
#include <algorithm>
#include <memory>

namespace pulp::view { class View; }

namespace knobpg {

enum Params : pulp::state::ParamID {
    kMacro = 1,
};

class KnobProcessor : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "KnobPlayground",
            .manufacturer = "Dream Date Designs",
            .bundle_id = "local.knobpg.knob",
            .version = "0.1.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
            .accepts_midi = false,
        };
    }

    void define_parameters(pulp::state::StateStore& store) override {
        // {min, max, default, step} — 0 step = continuous.
        store.add_parameter({.id = kMacro, .name = "Macro", .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 0.0f}});
    }

    void prepare(const pulp::format::PrepareContext&) override {}
    void release() override {}

    // Native GPU UI (out-of-line in view.cpp).
    std::unique_ptr<pulp::view::View> create_view() override;

    pulp::format::ViewSize view_size() const override {
        // DDFX editor frame is 1300x697 (aspect 1.865). Window opens proportional.
        pulp::format::ViewSize s;
        s.preferred_width = 1000;
        s.preferred_height = 536;
        s.min_width = 650;
        s.min_height = 348;
        s.max_width = 1560;
        s.max_height = 836;
        s.aspect_ratio = 1300.0 / 697.0;
        return s;
    }

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        const float g = state().get_value(kMacro);   // macro = output gain
        const int n = static_cast<int>(output.num_samples());
        const int oc = static_cast<int>(output.num_channels());
        const int ic = static_cast<int>(input.num_channels());
        for (int ch = 0; ch < oc; ++ch) {
            auto out = output.channel(static_cast<size_t>(ch));
            if (ch < ic) {
                auto in = input.channel(static_cast<size_t>(ch));
                for (int i = 0; i < n; ++i) out[i] = in[i] * g;
            } else {
                std::fill(out.begin(), out.end(), 0.0f);
            }
        }
    }
};

inline std::unique_ptr<pulp::format::Processor> create_knob_processor() {
    return std::make_unique<KnobProcessor>();
}

} // namespace knobpg
