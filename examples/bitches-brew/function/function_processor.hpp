#pragma once

// Function — math on an incoming control voltage.
//
// The one plug-in in the suite so far that reads its input bus. Everything else
// here generates; Function transforms. Patch a CV back into the computer (an
// ES-6, or any DC-coupled input), run it through a curve, send it out again: an
// LFO becomes an envelope shape, a slow ramp becomes a fast one at the top of its
// travel, a bipolar signal becomes a rectified unipolar one.
//
// Because it is an insert rather than a generator, its bypass means something
// different from DC's or LFO's: bypass makes it a wire, not a mute. Muting an
// insert in a CV chain would drop the voltage the *upstream* plug-in is
// generating, which is not what a user pressing Bypass on a shaping stage is
// asking for.
//
// The default settings are the identity. A freshly inserted Function passes its
// input through bit-exactly, so it can never be the thing that changed the
// signal until the user asks it to be.

#include <brew/cv.hpp>
#include <brew/function.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>

namespace pulp::examples::brew {

class FunctionProcessor : public format::Processor {
public:
    // Parameter IDs are part of the persisted state contract. Never renumber.
    enum ParamId : state::ParamID {
        kCurve = 1,
        kAmount = 2,
        kInScale = 3,
        kInOffset = 4,
        kOutOffset = 5,
        kOutputScale = 6,
        kInvert = 7,
    };

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "Function",
            .manufacturer = "Bitches Brew",
            .bundle_id = "com.bitchesbrew.function",
            .version = "0.1.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"CV In", 2}},
            .output_buses = {{"CV Out", 2}},
        };
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({.id = kCurve,
                             .name = "Curve",
                             .unit = "",
                             .range = {0.0f, static_cast<float>(kCurveCount - 1),
                                       0.0f, 1.0f}});
        // How hard the exponential and logarithmic curves bend. 1 is no bend at
        // all, which is why the range starts there rather than at 0.
        store.add_parameter({.id = kAmount,
                             .name = "Amount",
                             .unit = "",
                             .range = {1.0f, 8.0f, 2.0f, 0.01f}});
        // Bipolar: a negative input scale flips the polarity of the incoming CV,
        // which is how you turn a rising ramp into a falling one.
        store.add_parameter({.id = kInScale,
                             .name = "In Scale",
                             .unit = "",
                             .range = {-4.0f, 4.0f, 1.0f, 0.001f}});
        store.add_parameter({.id = kInOffset,
                             .name = "In Offset",
                             .unit = "",
                             .range = {-1.0f, 1.0f, 0.0f, 0.001f}});
        store.add_parameter({.id = kOutOffset,
                             .name = "Out Offset",
                             .unit = "",
                             .range = {-1.0f, 1.0f, 0.0f, 0.001f}});
        store.add_parameter({.id = kOutputScale,
                             .name = "Output Scale",
                             .unit = "",
                             .range = {0.0f, 1.0f, 1.0f, 0.001f}});
        // Rig compensation, not an artistic control: some interfaces present
        // their outputs with reversed polarity.
        store.add_parameter({.id = kInvert,
                             .name = "Invert",
                             .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f}});
    }

    void prepare(const format::PrepareContext&) override {}

    /// The size this editor actually needs. Without this override a host opens
    /// the plug-in at Processor's 400x300 default, and the layout is laid out to
    /// a geometry the editor was never checked against. The curve needs vertical room, and six knobs need two rows.
    std::pair<uint32_t, uint32_t> editor_size() const override {
        return {360, 490};
    }

    /// Defined in function_view.cpp so the audio translation units never see the
    /// view stack.
    std::unique_ptr<view::View> create_view() override;

    /// A snapshot of every knob, taken once per block rather than once per
    /// sample. The editor also calls this to draw the curve.
    [[nodiscard]] FunctionSettings settings() const noexcept {
        return {
            .in_scale = state().get_value(kInScale),
            .in_offset = state().get_value(kInOffset),
            .curve = curve_from_param(state().get_value(kCurve)),
            .amount = state().get_value(kAmount),
            .out_offset = state().get_value(kOutOffset),
            .out_scale = state().get_value(kOutputScale),
            .invert = as_toggle(state().get_value(kInvert)),
        };
    }

    /// The last sample the plug-in saw and the value it sent out, for the
    /// editor's dot on the curve. Relaxed: a reading one block stale is
    /// invisible, and synchronizing for it would not be.
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
        const std::size_t in_channels = input.num_channels();
        const std::size_t frames = output.num_samples();
        if (out_channels == 0 || frames == 0) return;

        const std::size_t shared = std::min(out_channels, in_channels);
        const FunctionSettings s = settings();

        for (std::size_t c = 0; c < shared; ++c) {
            const float* src = input.channel_ptr(c);
            float* dst = output.channel_ptr(c);
            if (src == nullptr || dst == nullptr) continue;
            // Bypass is a wire, not a mute: an insert that swallows the voltage
            // its upstream is generating has broken the patch, not bypassed a
            // stage of it.
            if (ctx.is_bypassed) {
                for (std::size_t n = 0; n < frames; ++n) dst[n] = src[n];
            } else {
                for (std::size_t n = 0; n < frames; ++n)
                    dst[n] = function_transfer(src[n], s);
            }
        }

        // An output channel with no input behind it has nothing to transform.
        // Emitting zero is the only honest answer; leaving the buffer alone would
        // hand the host whatever the previous plug-in left there.
        for (std::size_t c = shared; c < out_channels; ++c) {
            float* dst = output.channel_ptr(c);
            if (dst == nullptr) continue;
            for (std::size_t n = 0; n < frames; ++n) dst[n] = 0.0f;
        }

        const float last_in = shared > 0 && input.channel_ptr(0) != nullptr
                                  ? input.channel_ptr(0)[frames - 1]
                                  : 0.0f;
        display_in_.store(last_in, std::memory_order_relaxed);
        display_out_.store(ctx.is_bypassed ? last_in : function_transfer(last_in, s),
                           std::memory_order_relaxed);
    }

private:
    std::atomic<float> display_in_{0.0f};
    std::atomic<float> display_out_{0.0f};
};

inline std::unique_ptr<format::Processor> create_function() {
    return std::make_unique<FunctionProcessor>();
}

}  // namespace pulp::examples::brew
