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

#include <brew/channels.hpp>
#include <brew/param_text.hpp>
#include <brew/cv.hpp>
#include <brew/function.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <array>
#include <memory>
#include <string>
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
        kEnable = 8,
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

    /// The five curves, by the names the editor and the host both show.
    static std::string curve_name(float v) {
        static const char* const kNames[] = {"lin", "exp", "log", "abs", "pow"};
        return text::named_at(kNames, kCurveCount, v);
    }

    /// `fmt` is how the value reads — here and in the host's parameter list. A
    /// plain function pointer, so the table stays `constexpr`.
    struct ControlSpec {
        state::ParamID id;
        const char* name;
        state::ParamRange range;
        std::string (*fmt)(float);
    };

    /// Every control Function has. Registered once per channel: the two channels
    /// are independent and have identical controls.
    static constexpr std::array<ControlSpec, 8> controls() {
        return {{
            // Processing on this channel. Off passes the signal through
            // unmodified — not to zero, which would be a mute.
            {kEnable, "Enable", {0.0f, 1.0f, 1.0f, 1.0f}, text::on_off},
            {kCurve, "Curve", {0.0f, static_cast<float>(kCurveCount - 1), 0.0f, 1.0f}, curve_name},
            // The `power` curve's exponent, and only that curve's. 1 is the
            // identity; k and 1/k bend by the same factor in opposite directions,
            // which is why one knob spans both and the range is centred on 1.
            {kAmount, "Amount", {kMinPower, kMaxPower, 1.0f, 0.001f}, text::plain},
            // Bipolar: a negative input scale flips the polarity of the incoming
            // CV, which is how you turn a rising ramp into a falling one.
            {kInScale, "In Scale", {-4.0f, 4.0f, 1.0f, 0.001f}, text::plain},
            {kInOffset, "In Offset", {-1.0f, 1.0f, 0.0f, 0.001f}, text::plain},
            {kOutOffset, "Out Offset", {-1.0f, 1.0f, 0.0f, 0.001f}, text::plain},
            {kOutputScale, "Output Scale", {0.0f, 1.0f, 1.0f, 0.001f}, text::plain},
            // Rig compensation, not an artistic control: some interfaces present
            // an output with reversed polarity. Per-channel, because it can be one
            // output and not the other.
            {kInvert, "Invert", {0.0f, 1.0f, 0.0f, 1.0f}, text::on_off},
        }};
    }

    void define_parameters(state::StateStore& store) override {
        for (std::size_t ch = 0; ch < kChannelCount; ++ch)
            for (const auto& c : controls())
                store.add_parameter(
                    {.id = static_cast<state::ParamID>(param_for(c.id, ch)),
                     .name = std::string(c.name) + channel_suffix(ch),
                     .unit = "",
                     .range = c.range,
                     .to_string = c.fmt});
    }

    void prepare(const format::PrepareContext&) override {}

    /// The size this editor actually needs: two channel blocks, each a curve and
    /// two rows of knobs. Without this override a host opens the plug-in at
    /// Processor's 400x300 default — a geometry the layout was never checked at.
    std::pair<uint32_t, uint32_t> editor_size() const override {
        return {360, 648};
    }

    /// Defined in function_view.cpp so the audio translation units never see the
    /// view stack.
    std::unique_ptr<view::View> create_view() override;

    /// A snapshot of one channel's knobs, taken once per block rather than once
    /// per sample. The editor also calls this to draw the curve.
    [[nodiscard]] FunctionSettings settings(std::size_t ch = 0) const noexcept {
        const auto get = [&](state::ParamID id) {
            return state().get_value(static_cast<state::ParamID>(param_for(id, ch)));
        };
        return {
            .in_scale = get(kInScale),
            .in_offset = get(kInOffset),
            .curve = curve_from_param(get(kCurve)),
            .amount = get(kAmount),
            .out_offset = get(kOutOffset),
            .out_scale = get(kOutputScale),
            .invert = as_toggle(get(kInvert)),
        };
    }

    /// Whether this channel processes at all. Off is a wire, not a mute.
    [[nodiscard]] bool enabled(std::size_t ch) const noexcept {
        return as_toggle(
            state().get_value(static_cast<state::ParamID>(param_for(kEnable, ch))));
    }

    /// The last sample a channel saw and the value it sent out, for the editor's
    /// dot on the curve. Relaxed: a reading one block stale is invisible, and
    /// synchronizing for it would not be.
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
        const std::size_t in_channels = input.num_channels();
        const std::size_t frames = output.num_samples();
        if (out_channels == 0 || frames == 0) return;

        const std::size_t shared = std::min(out_channels, in_channels);

        for (std::size_t c = 0; c < shared; ++c) {
            const float* src = input.channel_ptr(c);
            float* dst = output.channel_ptr(c);
            if (src == nullptr || dst == nullptr) continue;
            const std::size_t ch = std::min(c, kChannelCount - 1);
            // Bypass is a wire, not a mute: an insert that swallows the voltage
            // its upstream is generating has broken the patch, not bypassed a
            // stage of it. A channel with `Enable` off is the same thing, scoped
            // to one channel.
            if (ctx.is_bypassed || !enabled(ch)) {
                for (std::size_t n = 0; n < frames; ++n) dst[n] = src[n];
                if (c < kChannelCount) publish(c, src[frames - 1], src[frames - 1]);
                continue;
            }
            const FunctionSettings s = settings(ch);
            for (std::size_t n = 0; n < frames; ++n)
                dst[n] = function_transfer(src[n], s);
            if (c < kChannelCount)
                publish(c, src[frames - 1], dst[frames - 1]);
        }

        // An output channel with no input behind it has nothing to transform.
        // Emitting zero is the only honest answer; leaving the buffer alone would
        // hand the host whatever the previous plug-in left there.
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

    std::array<std::atomic<float>, kChannelCount> display_in_{};
    std::array<std::atomic<float>, kChannelCount> display_out_{};
};

inline std::unique_ptr<format::Processor> create_function() {
    return std::make_unique<FunctionProcessor>();
}

}  // namespace pulp::examples::brew
