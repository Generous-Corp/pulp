#pragma once

// Combined-bundle demo — two distinct plugins packaged into ONE binary per
// format via pulp_add_plugin_bundle(). Proves the deliverable a single-plugin
// binary cannot: a CLAP/VST3 module that exposes N addressable plugins. Both
// are trivial stereo effects so the demo stays about packaging, not DSP.

#include <pulp/format/processor.hpp>

namespace pulp::examples::bundle {

// ── Plugin 1: a stereo gain trim ───────────────────────────────────────────
class GainProcessor : public format::Processor {
public:
    enum Params : state::ParamID { kGain = 1 };

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "Bundle Gain",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.bundle-demo.gain",
            .version = "1.0.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
        };
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({
            .id = kGain,
            .name = "Gain",
            .unit = "x",
            .range = {0.0f, 2.0f, 1.0f, 0.01f},
        });
    }

    void prepare(const format::PrepareContext&) override {}

    void process(audio::BufferView<float>& out,
                 const audio::BufferView<const float>& in,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        const float g = state().get_value(kGain);
        for (std::size_t ch = 0; ch < out.num_channels() && ch < in.num_channels(); ++ch) {
            auto i = in.channel(ch);
            auto o = out.channel(ch);
            for (std::size_t n = 0; n < out.num_samples(); ++n) o[n] = i[n] * g;
        }
    }
};

// ── Plugin 2: a mono-sum / stereo-width trim ────────────────────────────────
class WidthProcessor : public format::Processor {
public:
    enum Params : state::ParamID { kWidth = 1 };

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "Bundle Width",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.bundle-demo.width",
            .version = "1.0.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
        };
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({
            .id = kWidth,
            .name = "Width",
            .unit = "",
            .range = {0.0f, 1.0f, 1.0f, 0.01f},
        });
    }

    void prepare(const format::PrepareContext&) override {}

    void process(audio::BufferView<float>& out,
                 const audio::BufferView<const float>& in,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        const float w = state().get_value(kWidth);
        if (out.num_channels() < 2 || in.num_channels() < 2) {
            // Mono (or degenerate): pass through.
            for (std::size_t ch = 0; ch < out.num_channels() && ch < in.num_channels(); ++ch) {
                auto i = in.channel(ch);
                auto o = out.channel(ch);
                for (std::size_t n = 0; n < out.num_samples(); ++n) o[n] = i[n];
            }
            return;
        }
        auto il = in.channel(0);
        auto ir = in.channel(1);
        auto ol = out.channel(0);
        auto orr = out.channel(1);
        for (std::size_t n = 0; n < out.num_samples(); ++n) {
            const float mid = 0.5f * (il[n] + ir[n]);
            const float side = 0.5f * (il[n] - ir[n]) * w;
            ol[n] = mid + side;
            orr[n] = mid - side;
        }
    }
};

inline std::unique_ptr<format::Processor> create_gain() {
    return std::make_unique<GainProcessor>();
}
inline std::unique_ptr<format::Processor> create_width() {
    return std::make_unique<WidthProcessor>();
}

} // namespace pulp::examples::bundle
