#pragma once

// DC — a constant control-voltage source.
//
// The simplest plug-in in the Bitches Brew suite, and the one you reach for
// first: it writes one steady sample value to every output channel. Patched
// through a DC-coupled audio interface, a steady sample value *is* a steady
// voltage, so DC doubles as the suite's connection tester — set a value, watch
// it appear at the jack.
//
// It is also the guard for a property the whole suite depends on: a held value
// must survive the trip from process() to the host bus unaltered. Nothing here
// smooths, dithers, ramps, or filters. `test_dc.cpp` asserts bit-exactness.
//
// Output convention (shared across the suite): samples are normalized
// full-scale in [-1, +1]. A plug-in never knows about volts — full-scale
// voltage is a property of the interface, and differs per device and per output.
// `output_scale` and `invert` are the user's per-instance calibration; a UI may
// display volts once the user declares their interface's full-scale voltage.

#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>

namespace pulp::examples::brew {

class DcProcessor : public format::Processor {
public:
    // Parameter IDs are part of the persisted state contract. Never renumber.
    enum ParamId : state::ParamID {
        kValue = 1,
        kOutputScale = 2,
        kInvert = 3,
    };

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "DC",
            .manufacturer = "Bitches Brew",
            .bundle_id = "com.bitchesbrew.dc",
            .version = "0.1.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"Main In", 2}},
            .output_buses = {{"CV Out", 2}},
        };
    }

    void define_parameters(state::StateStore& store) override {
        // Default 0.0 is a safety property, not a style choice: a fresh
        // instance must emit no voltage until the user asks for one. A CV
        // generator that comes up at full scale points a voltage at whatever
        // module is patched to it.
        store.add_parameter({
            .id = kValue,
            .name = "Value",
            .unit = "",
            .range = {-1.0f, 1.0f, 0.0f, 0.001f},
        });
        // Per-instance calibration against the interface's full-scale voltage.
        store.add_parameter({
            .id = kOutputScale,
            .name = "Output Scale",
            .unit = "",
            .range = {0.0f, 1.0f, 1.0f, 0.001f},
        });
        // Some interfaces wire their outputs with reversed polarity. Without
        // this a CV suite is unusable on them.
        store.add_parameter({
            .id = kInvert,
            .name = "Invert",
            .unit = "",
            .range = {0.0f, 1.0f, 0.0f, 1.0f},
        });
    }

    void prepare(const format::PrepareContext&) override {}

    /// The value actually written to every output sample this block, after
    /// scale and polarity. Pure; shared by process() and the tests so a test
    /// cannot silently diverge from the DSP.
    static float resolve_output(float value, float scale, bool invert) noexcept {
        const float scaled = std::clamp(value, -1.0f, 1.0f) *
                             std::clamp(scale, 0.0f, 1.0f);
        return invert ? -scaled : scaled;
    }

    float current_output() const noexcept {
        return resolve_output(state().get_value(kValue),
                              state().get_value(kOutputScale),
                              state().get_value(kInvert) >= 0.5f);
    }

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer&,
                 midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        // Held, not ramped. A stepped CV is what a user asks for when they
        // type a number; smoothing it would make the value the plug-in reports
        // and the value at the jack disagree for as long as the ramp lasts.
        const float out = current_output();
        for (std::size_t c = 0; c < output.num_channels(); ++c) {
            float* dst = output.channel_ptr(c);
            if (dst == nullptr) continue;
            for (std::size_t n = 0; n < output.num_samples(); ++n) dst[n] = out;
        }
    }

    void process(format::ProcessBuffers& audio,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out,
                 const format::ProcessContext& context) override {
        if (auto* out = audio.main_output()) {
            audio::BufferView<const float> unused_input;
            process(*out, unused_input, midi_in, midi_out, context);
        }
    }
};

inline std::unique_ptr<format::Processor> create_dc() {
    return std::make_unique<DcProcessor>();
}

}  // namespace pulp::examples::brew
