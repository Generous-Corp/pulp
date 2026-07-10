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
// dithers or filters, and nothing smooths **unless the user asks** — `Smooth` is
// an explicit control, off by default, and at zero it is a wire. `test_dc.cpp`
// asserts bit-exactness with it off.
//
// It is not the trivial plug-in its name suggests. The two output knobs sum, so
// automation of the bipolar one can ride on a unipolar offset; the input bus can
// be added to or multiplied into the output; and the whole thing is designed to
// be *drawn* in a host's automation lane, which makes it an LFO with an arbitrary
// waveform.
//
// Output convention (shared across the suite): samples are normalized
// full-scale in [-1, +1]. A plug-in never knows about volts — full-scale
// voltage is a property of the interface, and differs per device and per output.
// `output_scale` and `invert` are the user's per-instance calibration; a UI may
// display volts once the user declares their interface's full-scale voltage.

#include <brew/channels.hpp>
#include <brew/cv.hpp>
#include <brew/smooth.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace pulp::examples::brew {

class DcProcessor : public format::Processor {
public:
    // Parameter IDs are part of the persisted state contract. Never renumber.
    enum ParamId : state::ParamID {
        kValue = 1,
        kOutputScale = 2,
        kInvert = 3,
        kUnipolar = 4,
        kMultiplier = 5,
        kInputAdd = 6,
        kInputMul = 7,
        kSmoothMs = 8,
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

    /// Every control DC has, with its range. Registered once per channel.
    ///
    /// A table rather than eight `add_parameter` calls, so that adding a control
    /// to the left channel cannot leave the right one behind. `param_for` derives
    /// the right channel's ID; nothing here enumerates it.
    struct ControlSpec {
        state::ParamID id;
        const char* name;
        const char* unit;
        state::ParamRange range;
    };

    static constexpr std::array<ControlSpec, 8> controls() {
        return {{
            // Default 0.0 is a safety property, not a style choice: a fresh
            // instance must emit no voltage until the user asks for one. A CV
            // generator that comes up at full scale points a voltage at whatever
            // module is patched to it.
            {kValue, "Value", "", {-1.0f, 1.0f, 0.0f, 0.001f}},
            // Per-jack calibration against the interface's full-scale voltage.
            // Per-channel, because an interface may differ between its outputs.
            {kOutputScale, "Output Scale", "", {0.0f, 1.0f, 1.0f, 0.001f}},
            // Some interfaces wire an output with reversed polarity. Without this
            // a CV suite is unusable on them — and it can be one output, not both.
            {kInvert, "Invert", "", {0.0f, 1.0f, 0.0f, 1.0f}},
            // A unipolar level, summed with the bipolar `Value`. Two knobs rather
            // than one because it lets an automated bipolar sweep ride on a fixed
            // unipolar offset — the shape and its resting point are separate ideas.
            {kUnipolar, "Unipolar", "", {0.0f, 1.0f, 0.0f, 0.001f}},
            // Scales the sum, and may invert it. Distinct from `Output Scale`,
            // which is unipolar rig calibration and belongs to the interface, not
            // the patch. Both exist because they answer different questions.
            {kMultiplier, "Multiplier", "", {-2.0f, 2.0f, 1.0f, 0.001f}},
            // The input bus, added to the output.
            {kInputAdd, "Input Add", "", {-1.0f, 1.0f, 0.0f, 0.001f}},
            // The input bus, multiplied into the output. At 0 the input is
            // ignored; at 1 the output is exactly the normal output multiplied by
            // the input. A ring modulator at the extremes, a VCA in between.
            {kInputMul, "Input Mul", "", {0.0f, 1.0f, 0.0f, 0.001f}},
            // Positive slews at a constant rate; negative low-passes. Milliseconds
            // for a full -1 to +1 swing. Zero is a wire — see brew/smooth.hpp for
            // why an explicit smoother does not violate the no-smoothing rule.
            {kSmoothMs, "Smooth", "ms", {-1000.0f, 1000.0f, 0.0f, 0.1f}},
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

    /// One channel's worth of settings, read once per block.
    struct ChannelSettings {
        float base = 0.0f;  ///< (unipolar + value) * multiplier
        float scale = 1.0f;
        bool invert = false;
        float in_add = 0.0f;
        float in_mul = 0.0f;
        float smooth_ms = 0.0f;
    };

    [[nodiscard]] ChannelSettings settings_for(std::size_t ch) const noexcept {
        const auto get = [&](state::ParamID id) {
            return state().get_value(static_cast<state::ParamID>(param_for(id, ch)));
        };
        return {.base = (get(kUnipolar) + get(kValue)) * get(kMultiplier),
                .scale = get(kOutputScale),
                .invert = as_toggle(get(kInvert)),
                .in_add = get(kInputAdd),
                .in_mul = get(kInputMul),
                .smooth_ms = get(kSmoothMs)};
    }

    void prepare(const format::PrepareContext& ctx) override {
        sample_rate_ = ctx.sample_rate;
        for (auto& s : smooth_) s.reset(0.0f);
    }

    /// The size this editor actually needs: two channel blocks, each a pair of
    /// knob rows, a readout and a rail. Without this override a host opens the
    /// plug-in at Processor's 400x300 default — a geometry the layout was never
    /// checked at, and one the two channels no longer fit in.
    std::pair<uint32_t, uint32_t> editor_size() const override {
        return {360, 540};
    }

    /// Defined in dc_view.cpp so the audio translation units never see the
    /// view stack.
    std::unique_ptr<view::View> create_view() override;

    /// The steady value one channel's knobs ask for, after the suite's shared
    /// output stage (clamp, scale, invert — see brew/cv.hpp). Ignores the input
    /// bus and the smoother, both of which need a signal and a clock. It is what
    /// DC emits with nothing patched in, which is how it is used.
    float current_output(std::size_t ch = 0) const noexcept {
        const ChannelSettings s = settings_for(ch);
        return resolve_output(s.base, s.scale, s.invert);
    }

    /// The last sample actually emitted on a channel, for the editor's rail.
    /// Written once per block on the audio thread with relaxed ordering: a rail
    /// one block stale is invisible, and synchronizing for it would not be.
    [[nodiscard]] float display_output(std::size_t ch = 0) const noexcept {
        return display_out_[std::min(ch, kChannelCount - 1)].load(
            std::memory_order_relaxed);
    }

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 midi::MidiBuffer&,
                 midi::MidiBuffer&,
                 const format::ProcessContext& ctx) override {
        // Bypass means stop driving the patch. Some hosts bypass by
        // short-circuiting process(); others keep calling it, and on those a
        // plug-in that ignores the flag holds its voltage at the jack after the
        // user pressed Bypass. Emitting zero is the only safe reading. Passing
        // the input bus through — an audio effect's bypass — would be worse
        // still: it would drive the modular with whatever is on the track.
        //
        // Held, not ramped. A stepped CV is what a user asks for when they
        // type a number; smoothing it would make the value the plug-in reports
        // and the value at the jack disagree for as long as the ramp lasts.
        const std::size_t channels = output.num_channels();
        const std::size_t frames = output.num_samples();
        if (channels == 0 || frames == 0) return;

        if (ctx.is_bypassed) {
            for (std::size_t c = 0; c < channels; ++c)
                if (float* dst = output.channel_ptr(c))
                    for (std::size_t n = 0; n < frames; ++n) dst[n] = 0.0f;
            // Park the smoothers at zero so releasing bypass ramps up from
            // silence rather than jumping to a value the patch last saw.
            for (auto& s : smooth_) s.reset(0.0f);
            for (auto& d : display_out_) d.store(0.0f, std::memory_order_relaxed);
            return;
        }

        const double sr = ctx.sample_rate > 0.0 ? ctx.sample_rate : sample_rate_;
        const std::size_t in_channels = input.num_channels();

        for (std::size_t c = 0; c < channels; ++c) {
            float* dst = output.channel_ptr(c);
            if (dst == nullptr) continue;
            const float* src = c < in_channels ? input.channel_ptr(c) : nullptr;
            // A host that hands us more channels than we have controls for runs
            // the last channel's settings on the extra ones. Its own smoother,
            // though: sharing one would make each channel filter the previous
            // channel's samples.
            const ChannelSettings s = settings_for(std::min(c, kChannelCount - 1));
            Smoother& sm = smooth_[std::min(c, smooth_.size() - 1)];

            for (std::size_t n = 0; n < frames; ++n) {
                const float x = src ? src[n] : 0.0f;
                // Multiply first, then add: the added signal is a signal, not
                // something for the ring modulator to chew on. At `in_mul` 0 the
                // output is untouched by the input; at 1 it is exactly the normal
                // output multiplied by the input.
                float v = s.base * (1.0f - s.in_mul + s.in_mul * x);
                v += s.in_add * x;
                dst[n] = resolve_output(sm.process(v, s.smooth_ms, sr), s.scale,
                                        s.invert);
            }
            if (c < kChannelCount)
                display_out_[c].store(dst[frames - 1], std::memory_order_relaxed);
        }
    }

private:
    // One smoother per channel. Sharing one would make each channel filter the
    // previous channel's samples.
    std::array<Smoother, 8> smooth_{};
    std::array<std::atomic<float>, kChannelCount> display_out_{};
    double sample_rate_ = 48000.0;
};

inline std::unique_ptr<format::Processor> create_dc() {
    return std::make_unique<DcProcessor>();
}

}  // namespace pulp::examples::brew
