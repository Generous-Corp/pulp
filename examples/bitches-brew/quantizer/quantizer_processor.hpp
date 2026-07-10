#pragma once

// Quantizer — snap a control voltage to discrete values.
//
// An insert, like Function: it reads its input bus and shapes it. So bypass is a
// wire, not a mute — muting would drop the voltage the upstream plug-in is
// generating, which is the opposite of what a user bypassing a *shaping* stage
// asks for.
//
// Two modes. `Manual` divides the full range into N equal steps. `Scale` then
// keeps only the notes of a musical mode, which costs no calibration at all —
// once twelve lattice steps span an octave of the user's oscillator, restricting
// the lattice is arithmetic on the step index. See brew/scale.hpp.
//
// `Calibrated` is deliberately absent. It needs the interface's full-scale
// voltage, which is unmeasured, and a guessed number here is a wrong pitch on a
// patch cable.

#include <brew/channels.hpp>
#include <brew/cv.hpp>
#include <brew/scale.hpp>
#include <brew/smooth.hpp>
#include <brew/sync.hpp>
#include <brew/quantize.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <array>
#include <cmath>
#include <string>
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
        kEnable = 7,
        kMode = 8,
        kScale = 9,
        kKey = 10,
        kKeyOffset = 11,
        kSmoothMs = 12,
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

    struct ControlSpec {
        state::ParamID id;
        const char* name;
        const char* unit;
        state::ParamRange range;
    };

    /// Every control the Quantizer has. Registered once per channel: the two
    /// channels are independent and have identical controls.
    static constexpr std::array<ControlSpec, 12> controls() {
        return {{
            // Quantization on this channel. Off passes the CV through unchanged.
            {kEnable, "Enable", "", {0.0f, 1.0f, 1.0f, 1.0f}},
            // Manual divides full scale into equal steps; Scale then keeps only
            // the notes of a mode. `Calibrated` is absent — it needs the
            // interface's full-scale voltage, and guessing it is a wrong pitch.
            {kMode, "Mode", "", {0.0f, static_cast<float>(kQuantModeCount - 1), 0.0f, 1.0f}},
            // Coarse count. Twelve divisions of full scale is the obvious starting
            // point and lands on semitones only once the rail voltage is known.
            {kSteps, "Steps", "", {kMinQuantizeSteps, kMaxQuantizeSteps, 12.0f, 1.0f}},
            // Added to Steps. A fractional step count simply puts the rails
            // between lattice points, which is a legitimate thing to want.
            {kFine, "Fine", "", {-0.5f, 0.5f, 0.0f, 0.001f}},
            // Where the lattice sits within a step. At 0 a step lands on zero.
            {kOffset, "Offset", "", {0.0f, 1.0f, 0.0f, 0.001f}},
            // In Manual mode, whole steps — the only transpose that keeps the
            // output on the lattice it was just snapped to. In Scale mode, whole
            // scale degrees, so +7 in a seven-note scale is an octave.
            {kTranspose, "Transpose", "steps", {-24.0f, 24.0f, 0.0f, 1.0f}},
            {kScale, "Scale", "", {0.0f, static_cast<float>(kScaleCount - 1), 0.0f, 1.0f}},
            // Key and Key Offset are summed into the scale's root. Two controls
            // rather than one so a pattern can be automated around a fixed root.
            {kKey, "Key", "", {0.0f, 11.0f, 0.0f, 1.0f}},
            {kKeyOffset, "Key Offset", "", {-12.0f, 12.0f, 0.0f, 1.0f}},
            // As DC's. Positive slews, negative low-passes; zero is a wire, and a
            // quantizer that smooths by default would un-quantize its own output.
            {kSmoothMs, "Smooth", "ms", {-1000.0f, 1000.0f, 0.0f, 0.1f}},
            // Per-jack calibration and polarity, as everywhere else in the suite.
            {kOutputScale, "Output Scale", "", {0.0f, 1.0f, 1.0f, 0.001f}},
            {kInvert, "Invert", "", {0.0f, 1.0f, 0.0f, 1.0f}},
        }};
    }

    void define_parameters(state::StateStore& store) override {
        for (std::size_t ch = 0; ch < kChannelCount; ++ch)
            for (const auto& c : controls())
                store.add_parameter(
                    {.id = static_cast<state::ParamID>(param_for(c.id, ch)),
                     .name = std::string(c.name) + channel_suffix(ch),
                     .unit = c.unit,
                     .range = c.range});
    }

    void prepare(const format::PrepareContext& ctx) override {
        sample_rate_ = ctx.sample_rate;
        for (auto& s : smooth_) s.reset(0.0f);
    }

    /// The size this editor actually needs: two channel blocks, each a staircase
    /// and two rows of knobs. Without this override a host opens the plug-in at
    /// Processor's 400x300 default — a geometry the layout was never checked at.
    std::pair<uint32_t, uint32_t> editor_size() const override { return {360, 780}; }

    /// Defined in quantizer_view.cpp so the audio translation units never see the
    /// view stack.
    std::unique_ptr<view::View> create_view() override;

    /// A snapshot of one channel's knobs, taken once per block. The editor draws
    /// the same staircase the DSP walks, so the picture cannot drift from the
    /// signal.
    [[nodiscard]] QuantizeSettings settings(std::size_t ch = 0) const noexcept {
        const auto get = [&](state::ParamID id) {
            return state().get_value(static_cast<state::ParamID>(param_for(id, ch)));
        };
        return {
            .mode = enum_from_param<QuantMode>(get(kMode), kQuantModeCount),
            .steps = get(kSteps) + get(kFine),
            .offset = get(kOffset),
            .transpose = get(kTranspose),
            .scale = enum_from_param<Scale>(get(kScale), kScaleCount),
            // Summed, and wrapped into an octave: a root of 13 is the root of 1.
            .root = floor_mod(static_cast<int>(std::lround(get(kKey))) +
                                  static_cast<int>(std::lround(get(kKeyOffset))),
                              kSemitonesPerOctave),
            .out_scale = get(kOutputScale),
            .invert = as_toggle(get(kInvert)),
        };
    }

    /// Whether this channel quantizes at all. Off passes the CV through unchanged.
    [[nodiscard]] bool enabled(std::size_t ch) const noexcept {
        return as_toggle(
            state().get_value(static_cast<state::ParamID>(param_for(kEnable, ch))));
    }

    [[nodiscard]] float display_input(std::size_t ch = 0) const noexcept {
        return display_in_[std::min(ch, kChannelCount - 1)].load(
            std::memory_order_relaxed);
    }
    [[nodiscard]] float display_output(std::size_t ch = 0) const noexcept {
        return display_out_[std::min(ch, kChannelCount - 1)].load(
            std::memory_order_relaxed);
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
        const double sr = ctx.sample_rate > 0.0 ? ctx.sample_rate : sample_rate_;

        for (std::size_t c = 0; c < shared; ++c) {
            const float* src = input.channel_ptr(c);
            float* dst = output.channel_ptr(c);
            if (src == nullptr || dst == nullptr) continue;
            const std::size_t ch = std::min(c, kChannelCount - 1);

            // Bypass, and a channel with Enable off, are both wires. Zeroing here
            // would drop the CV the upstream plug-in is generating, which is not
            // what turning off a shaping stage means.
            if (ctx.is_bypassed || !enabled(ch)) {
                for (std::size_t n = 0; n < frames; ++n) dst[n] = src[n];
                smooth_[std::min(c, smooth_.size() - 1)].reset(src[frames - 1]);
                if (c < kChannelCount) publish(c, src[frames - 1], src[frames - 1]);
                continue;
            }

            const QuantizeSettings s = settings(ch);
            const float ms = state().get_value(
                static_cast<state::ParamID>(param_for(kSmoothMs, ch)));
            Smoother& sm = smooth_[std::min(c, smooth_.size() - 1)];
            // Smoothing runs *after* the snap, so it glides between lattice points
            // rather than quantizing a glide. At zero it is a wire, which is why
            // the staircase stays bit-exact by default.
            for (std::size_t n = 0; n < frames; ++n)
                dst[n] = sm.process(quantize_transfer(src[n], s), ms, sr);
            if (c < kChannelCount) publish(c, src[frames - 1], dst[frames - 1]);
        }

        // An output channel with no input behind it has nothing to quantize.
        for (std::size_t c = shared; c < out_channels; ++c) {
            float* dst = output.channel_ptr(c);
            if (dst == nullptr) continue;
            for (std::size_t n = 0; n < frames; ++n) dst[n] = 0.0f;
            if (c < kChannelCount) publish(c, 0.0f, 0.0f);
        }
    }

private:
    void publish(std::size_t ch, float in, float out) noexcept {
        display_in_[ch].store(in, std::memory_order_relaxed);
        display_out_[ch].store(out, std::memory_order_relaxed);
    }

    std::array<Smoother, 8> smooth_{};
    std::array<std::atomic<float>, kChannelCount> display_in_{};
    std::array<std::atomic<float>, kChannelCount> display_out_{};
    double sample_rate_ = 48000.0;
};

inline std::unique_ptr<format::Processor> create_quantizer() {
    return std::make_unique<QuantizerProcessor>();
}

}  // namespace pulp::examples::brew
