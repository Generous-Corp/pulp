#pragma once

// Quantizer — snap a control voltage to discrete values.
//
// An insert, like Function: it reads its input bus and shapes it. So bypass is a
// wire, not a mute — muting would drop the voltage the upstream plug-in is
// generating, which is the opposite of what a user bypassing a *shaping* stage
// asks for.
//
// Manual mode only. The full range is divided into N equal steps. Musical scale
// quantization needs the interface's full-scale voltage, which is unmeasured, and
// this plug-in will not guess it. See brew/quantize.hpp.

#include <brew/cv.hpp>
#include <brew/quantize.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>

namespace pulp::examples::brew {

class QuantizerProcessor : public format::Processor {
public:
    // Parameter IDs are part of the persisted state contract. Never renumber.
    enum ParamId : state::ParamID {
        kSteps = 1,
        kFine = 2,
        kOffset = 3,
        kTranspose = 4,
        kOutputScale = 5,
        kInvert = 6,
    };

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "Quantizer",
            .manufacturer = "Bitches Brew",
            .bundle_id = "com.bitchesbrew.quantizer",
            .version = "0.1.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"CV In", 2}},
            .output_buses = {{"CV Out", 2}},
        };
    }

    void define_parameters(state::StateStore& store) override {
        // Coarse count. Twelve divisions of full scale is the obvious starting
        // point and lands on semitones only once the rail voltage is known.
        store.add_parameter({.id = kSteps,
                             .name = "Steps",
                             .unit = "",
                             .range = {kMinQuantizeSteps, kMaxQuantizeSteps, 12.0f, 1.0f}});
        // Added to Steps. A fractional step count simply puts the rails between
        // lattice points, which is a legitimate thing to want.
        store.add_parameter({.id = kFine,
                             .name = "Fine",
                             .unit = "",
                             .range = {-0.5f, 0.5f, 0.0f, 0.001f}});
        // Where the lattice sits within a step. At 0 a step lands exactly on zero.
        store.add_parameter({.id = kOffset,
                             .name = "Offset",
                             .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 0.001f}});
        // Whole steps, not volts: the only transpose that keeps the output on the
        // lattice it was just snapped to.
        store.add_parameter({.id = kTranspose,
                             .name = "Transpose",
                             .unit = "steps",
                             .range = {-24.0f, 24.0f, 0.0f, 1.0f}});
        store.add_parameter({.id = kOutputScale,
                             .name = "Output Scale",
                             .unit = "",
                             .range = {0.0f, 1.0f, 1.0f, 0.001f}});
        store.add_parameter({.id = kInvert,
                             .name = "Invert",
                             .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f}});
    }

    void prepare(const format::PrepareContext&) override {}

    /// The size this editor actually needs. Without this override a host opens
    /// the plug-in at Processor's 400x300 default, and the layout is laid out to
    /// a geometry the editor was never checked against. The staircase needs
    /// vertical room; four knobs fit one row.
    std::pair<uint32_t, uint32_t> editor_size() const override { return {360, 440}; }

    /// Defined in quantizer_view.cpp so the audio translation units never see the
    /// view stack.
    std::unique_ptr<view::View> create_view() override;

    /// A snapshot of every knob, taken once per block. The editor draws the same
    /// staircase the DSP walks, so the picture cannot drift from the signal.
    [[nodiscard]] QuantizeSettings settings() const noexcept {
        return {
            .steps = state().get_value(kSteps) + state().get_value(kFine),
            .offset = state().get_value(kOffset),
            .transpose = state().get_value(kTranspose),
            .out_scale = state().get_value(kOutputScale),
            .invert = as_toggle(state().get_value(kInvert)),
        };
    }

    [[nodiscard]] float display_input() const noexcept {
        return display_in_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] float display_output() const noexcept {
        return display_out_.load(std::memory_order_relaxed);
    }

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 midi::MidiBuffer&,
                 midi::MidiBuffer&,
                 const format::ProcessContext& ctx) override {
        const std::size_t out_channels = output.num_channels();
        const std::size_t frames = output.num_samples();
        if (out_channels == 0 || frames == 0) return;

        const std::size_t shared = std::min(out_channels, input.num_channels());
        const QuantizeSettings s = settings();

        for (std::size_t c = 0; c < shared; ++c) {
            const float* src = input.channel_ptr(c);
            float* dst = output.channel_ptr(c);
            if (src == nullptr || dst == nullptr) continue;
            if (ctx.is_bypassed) {
                for (std::size_t n = 0; n < frames; ++n) dst[n] = src[n];
            } else {
                for (std::size_t n = 0; n < frames; ++n)
                    dst[n] = quantize_transfer(src[n], s);
            }
        }

        // An output channel with no input behind it has nothing to quantize.
        for (std::size_t c = shared; c < out_channels; ++c) {
            float* dst = output.channel_ptr(c);
            if (dst == nullptr) continue;
            for (std::size_t n = 0; n < frames; ++n) dst[n] = 0.0f;
        }

        const float last_in = shared > 0 && input.channel_ptr(0) != nullptr
                                  ? input.channel_ptr(0)[frames - 1]
                                  : 0.0f;
        display_in_.store(last_in, std::memory_order_relaxed);
        display_out_.store(ctx.is_bypassed ? last_in : quantize_transfer(last_in, s),
                           std::memory_order_relaxed);
    }

private:
    std::atomic<float> display_in_{0.0f};
    std::atomic<float> display_out_{0.0f};
};

inline std::unique_ptr<format::Processor> create_quantizer() {
    return std::make_unique<QuantizerProcessor>();
}

}  // namespace pulp::examples::brew
