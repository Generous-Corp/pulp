#pragma once

// pulp-mpe-synth — MPE-aware sine synth demonstrating Pulp's MPE support.
//
// Opts in via PluginDescriptor::supports_mpe = true. The CLAP adapter
// builds an MpeBuffer from inbound MIDI each block and hands it to the
// processor via Processor::mpe_input(). An MpeVoiceAllocator with
// MpeSynthVoice subclasses handles per-note pitch bend (up to 48
// semitones), pressure (maps to amplitude), and CC 74 timbre (maps to
// brightness via a one-pole lowpass cutoff).

#include <pulp/format/processor.hpp>
#include <pulp/midi/mpe_buffer.hpp>
#include <pulp/midi/mpe_synth_voice.hpp>
#include <algorithm>
#include <cmath>
#include <memory>

namespace pulp::examples::mpe_synth {

enum Params : state::ParamID {
    kMasterGainDb = 1,
    kGlideMs      = 2,
};

class Voice : public midi::MpeSynthVoice {
public:
    void set_sample_rate(float sr) { sample_rate_ = sr > 0 ? sr : 48000.0f; }

    void on_note_on(const midi::MpeNoteState& n) override {
        midi::MpeSynthVoice::on_note_on(n);
        // Start from silence; amp envelope follows pressure.
        amp_ = 0.0f;
        phase_ = 0.0f;
        lp_ = 0.0f;
    }

    void on_note_off() override { midi::MpeSynthVoice::on_note_off(); }

    void render(float* out, int num_samples) override {
        if (!active()) return;
        for (int i = 0; i < num_samples; ++i) {
            advance_smoothers();

            const float base_hz = 440.0f * std::pow(2.0f,
                (static_cast<float>(note_number()) - 69.0f + pitch_bend()) / 12.0f);
            const float increment = base_hz / sample_rate_;
            phase_ += increment;
            if (phase_ >= 1.0f) phase_ -= 1.0f;

            const float osc = std::sin(2.0f * 3.14159265358979f * phase_);
            // Amplitude follows pressure with a gentle attack when > 0.
            const float amp_target = releasing() ? 0.0f : pressure();
            amp_ += (amp_target - amp_) * 0.005f;

            // Timbre → brightness via simple one-pole lowpass.
            // alpha = 1 - timbre; higher timbre → brighter (more signal).
            const float alpha = std::clamp(1.0f - timbre(), 0.02f, 0.98f);
            lp_ = alpha * lp_ + (1.0f - alpha) * osc;
            const float signal = osc - lp_ * (1.0f - timbre());

            out[i] += signal * amp_ * 0.25f;

            if (releasing() && amp_ < 1e-4f) {
                finish_release();
                break;
            }
        }
    }

private:
    float sample_rate_ = 48000.0f;
    float phase_ = 0.0f;
    float amp_ = 0.0f;
    float lp_ = 0.0f;
};

class Processor : public format::Processor {
public:
    Processor() : allocator_(8) {}

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "PulpMpeSynth",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.mpe-synth",
            .version = "0.1.0",
            .category = format::PluginCategory::Instrument,
            .input_buses = {},
            .output_buses = {{"Audio Out", 2}},
            .accepts_midi = true,
            .produces_midi = false,
            .supports_mpe = true,  // ← opt-in
            .tail_samples = -1,
        };
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({kMasterGainDb, "Gain", "dB", {-60, 12, -6, 0.1f}});
        store.add_parameter({kGlideMs, "Glide", "ms", {0, 500, 0, 1}});
    }

    void prepare(const format::PrepareContext& ctx) override {
        for (std::size_t i = 0; i < allocator_.polyphony(); ++i) {
            allocator_.voice(i).set_sample_rate(static_cast<float>(ctx.sample_rate));
            allocator_.voice(i).set_smoothing(0.995f);
        }
    }

    void process(
        audio::BufferView<float>& output,
        const audio::BufferView<const float>&,
        midi::MidiBuffer&, midi::MidiBuffer&,
        const format::ProcessContext& ctx) override {

        const std::size_t nch = output.num_channels();
        const std::size_t ns = output.num_samples();
        for (std::size_t ch = 0; ch < nch; ++ch) {
            auto span = output.channel(ch);
            for (std::size_t i = 0; i < ns; ++i) span[i] = 0.0f;
        }

        if (const auto* mpe = mpe_input()) {
            allocator_.dispatch_all(*mpe);
        }

        const float gain_db = state().get_value(kMasterGainDb);
        const float gain = std::pow(10.0f, gain_db / 20.0f);

        if (nch == 0 || ns == 0) return;

        auto left = output.channel(0);
        for (std::size_t i = 0; i < allocator_.polyphony(); ++i) {
            allocator_.voice(i).render(left.data(), static_cast<int>(ns));
        }
        for (std::size_t i = 0; i < ns; ++i) left[i] *= gain;
        for (std::size_t ch = 1; ch < nch; ++ch) {
            auto other = output.channel(ch);
            for (std::size_t i = 0; i < ns; ++i) other[i] = left[i];
        }
        (void)ctx;
    }

    /// Exposed for tests.
    midi::MpeVoiceAllocator<Voice>& allocator() { return allocator_; }

private:
    midi::MpeVoiceAllocator<Voice> allocator_;
};

inline std::unique_ptr<format::Processor> create_pulp_mpe_synth() {
    return std::make_unique<Processor>();
}

} // namespace pulp::examples::mpe_synth
