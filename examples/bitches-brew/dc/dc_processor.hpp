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

#include <brew/cv.hpp>
#include <brew/smooth.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
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
        // A unipolar level, summed with the bipolar `Value`. Two knobs rather
        // than one because it lets an automated bipolar sweep ride on a fixed
        // unipolar offset — the shape and its resting point are separate ideas.
        store.add_parameter({
            .id = kUnipolar,
            .name = "Unipolar",
            .unit = "",
            .range = {0.0f, 1.0f, 0.0f, 0.001f},
        });
        // Scales the sum, and may invert it. Distinct from `Output Scale`, which
        // is unipolar rig calibration and belongs to the interface, not the
        // patch. Both exist because they are answers to different questions.
        store.add_parameter({
            .id = kMultiplier,
            .name = "Multiplier",
            .unit = "",
            .range = {-2.0f, 2.0f, 1.0f, 0.001f},
        });
        // The input bus, added to the output.
        store.add_parameter({
            .id = kInputAdd,
            .name = "Input Add",
            .unit = "",
            .range = {-1.0f, 1.0f, 0.0f, 0.001f},
        });
        // The input bus, multiplied into the output. At 0 the input is ignored;
        // at 1 the output is fully gated by it. A ring modulator at the extremes,
        // a VCA in between.
        store.add_parameter({
            .id = kInputMul,
            .name = "Input Mul",
            .unit = "",
            .range = {0.0f, 1.0f, 0.0f, 0.001f},
        });
        // Positive slews at a constant rate; negative low-passes. Milliseconds
        // for a full -1 to +1 swing. Zero is a wire — see brew/smooth.hpp for why
        // an explicit smoother does not violate the suite's no-smoothing rule.
        store.add_parameter({
            .id = kSmoothMs,
            .name = "Smooth",
            .unit = "ms",
            .range = {-1000.0f, 1000.0f, 0.0f, 0.1f},
        });
    }

    void prepare(const format::PrepareContext& ctx) override {
        sample_rate_ = ctx.sample_rate;
        for (auto& s : smooth_) s.reset(0.0f);
    }

    /// The size this editor actually needs: the rail, two rows of knobs, and a
    /// polarity switch. Without this override a host opens the plug-in at
    /// Processor's 400x300 default — a geometry the layout was never checked at.
    std::pair<uint32_t, uint32_t> editor_size() const override {
        return {360, 300};
    }

    /// Defined in dc_view.cpp so the audio translation units never see the
    /// view stack.
    std::unique_ptr<view::View> create_view() override;

    /// The steady value the knobs ask for, after the suite's shared output stage
    /// (clamp, scale, invert — see brew/cv.hpp). Ignores the input bus and the
    /// smoother, both of which need a signal and a clock. It is what DC emits
    /// with nothing patched in, which is how it is used.
    float current_output() const noexcept {
        return resolve_output(target(), state().get_value(kOutputScale),
                              as_toggle(state().get_value(kInvert)));
    }

    /// The last sample actually emitted, for the editor's rail. Written once per
    /// block on the audio thread with relaxed ordering: a rail one block stale is
    /// invisible, and synchronizing for it would not be.
    [[nodiscard]] float display_output() const noexcept {
        return display_out_.load(std::memory_order_relaxed);
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
            display_out_.store(0.0f, std::memory_order_relaxed);
            return;
        }

        const float base = target();
        const float scale = state().get_value(kOutputScale);
        const bool inv = as_toggle(state().get_value(kInvert));
        const float in_add = state().get_value(kInputAdd);
        const float in_mul = state().get_value(kInputMul);
        const float ms = state().get_value(kSmoothMs);
        const double sr = ctx.sample_rate > 0.0 ? ctx.sample_rate : sample_rate_;

        const std::size_t in_channels = input.num_channels();
        float last = 0.0f;

        for (std::size_t c = 0; c < channels; ++c) {
            float* dst = output.channel_ptr(c);
            if (dst == nullptr) continue;
            const float* src = c < in_channels ? input.channel_ptr(c) : nullptr;
            Smoother& sm = smooth_[std::min(c, smooth_.size() - 1)];

            for (std::size_t n = 0; n < frames; ++n) {
                const float x = src ? src[n] : 0.0f;
                // Multiply first, then add: the added signal is a signal, not
                // something for the ring modulator to chew on.
                float v = base * (1.0f - in_mul + in_mul * x);
                v += in_add * x;
                dst[n] = resolve_output(sm.process(v, ms, sr), scale, inv);
            }
            if (c == 0) last = dst[frames - 1];
        }
        display_out_.store(last, std::memory_order_relaxed);
    }

private:
    /// The two output knobs, summed and scaled. Not clamped here: the shared
    /// output stage clamps once, at the jack.
    [[nodiscard]] float target() const noexcept {
        return (state().get_value(kUnipolar) + state().get_value(kValue)) *
               state().get_value(kMultiplier);
    }

    // One smoother per channel. Sharing one would make each channel filter the
    // previous channel's samples.
    std::array<Smoother, 8> smooth_{};
    std::atomic<float> display_out_{0.0f};
    double sample_rate_ = 48000.0;
};

inline std::unique_ptr<format::Processor> create_dc() {
    return std::make_unique<DcProcessor>();
}

}  // namespace pulp::examples::brew
