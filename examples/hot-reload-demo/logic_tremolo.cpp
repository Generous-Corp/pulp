// Hot-reloadable DSP "logic" for the hot-reload demo.
//
// This is the half of the plugin you EDIT and recompile while the host keeps
// playing. The shell (the other half, a ReloadableShell built into the loaded
// VST3/CLAP) watches the compiled .dylib and hot-swaps this DSP in live.
//
// It is a tremolo: a low-frequency oscillator modulates the amplitude. Two
// automatable parameters form the STABLE contract the shell mirrors — they must
// not change across reloads (changing the parameter set needs a full reload):
//
//     id 1  "Depth"  0..1     how deep the amplitude dips
//     id 2  "Rate"   0.1..20  LFO speed in Hz
//
// Everything ELSE — the LFO SHAPE below — is fair game to edit live. Flip
// kWaveform between Sine and Square (or change kShapeName), rebuild with
//     examples/hot-reload-demo/rebuild_logic.sh
// and the tremolo morphs under your ears with no reload, no audio dropout.

#include <pulp/format/processor.hpp>
#include <pulp/format/reload/reload_abi.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

using namespace pulp;

namespace {

// ── EDIT ME LIVE ────────────────────────────────────────────────────────────
// Change this, run rebuild_logic.sh, and hear the tremolo shape morph.
enum class Wave { Sine, Square };
constexpr Wave kWaveform = Wave::Sine;
// ─────────────────────────────────────────────────────────────────────────────

class Tremolo final : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {.name = "Pulp Hot-Reload Demo", .manufacturer = "Pulp",
                .bundle_id = "com.pulp.hot-reload-demo", .version = "1.0.0",
                .category = format::PluginCategory::Effect,
                .input_buses = {{"In", 2}}, .output_buses = {{"Out", 2}}};
    }

    void define_parameters(state::StateStore& s) override {
        s.add_parameter({.id = 1, .name = "Depth", .unit = "",
                         .range = {0.0f, 1.0f, 0.5f, 0.0f}});
        s.add_parameter({.id = 2, .name = "Rate", .unit = "Hz",
                         .range = {0.1f, 20.0f, 5.0f, 0.0f}});
    }

    void prepare(const format::PrepareContext& ctx) override {
        sample_rate_ = ctx.sample_rate > 0 ? ctx.sample_rate : 48000.0;
        phase_ = 0.0;
    }

    void process(audio::BufferView<float>& out,
                 const audio::BufferView<const float>& in,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        const float depth = std::clamp(state().get_value(1), 0.0f, 1.0f);
        const float rate = std::clamp(state().get_value(2), 0.1f, 20.0f);
        const double inc = static_cast<double>(rate) / sample_rate_;
        const std::size_t ch = std::min(out.num_channels(), in.num_channels());
        const std::size_t frames = out.num_samples();

        for (std::size_t n = 0; n < frames; ++n) {
            const float lfo = shape(static_cast<float>(phase_));       // 0..1
            const float gain = 1.0f - depth * lfo;                     // dip by depth
            for (std::size_t c = 0; c < ch; ++c)
                out.channel(c)[n] = in.channel(c)[n] * gain;
            phase_ += inc;
            if (phase_ >= 1.0) phase_ -= 1.0;
        }
        for (std::size_t c = ch; c < out.num_channels(); ++c) {
            auto o = out.channel(c);
            for (std::size_t n = 0; n < frames; ++n) o[n] = 0.0f;
        }
    }

private:
    // Unipolar 0..1 LFO. The shape is what you edit live.
    static float shape(float phase01) {
        if constexpr (kWaveform == Wave::Sine)
            return 0.5f * (1.0f - std::cos(2.0f * 3.14159265358979f * phase01));
        else
            return phase01 < 0.5f ? 0.0f : 1.0f;  // square: hard chop
    }

    double sample_rate_ = 48000.0;
    double phase_ = 0.0;
};

}  // namespace

PULP_RELOAD_LOGIC(new Tremolo())
