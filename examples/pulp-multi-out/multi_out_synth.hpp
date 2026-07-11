#pragma once

// pulp-multi-out — an 8-voice instrument that renders each voice to its OWN
// stereo output bus (1 main + 7 aux), demonstrating Pulp's multi-bus output
// path across VST3, CLAP, and AU v2 (aumu / MusicDeviceBase).
//
// The point of the example is the BUS TOPOLOGY, not the DSP: the descriptor
// declares eight stereo `output_buses`, and the processor overrides the richer
// `process(ProcessBuffers&)` surface so it can address every output bus by
// index. Incoming MIDI notes are assigned round-robin to the eight voices, and
// voice `v` always renders into output bus `v`. In a DAW this means each voice
// lands on its own mixer return — Logic/Live/Cubase/Reaper list the aux outputs
// once the AU/VST3/CLAP adapter advertises them.
//
// A processor that only wrote the main bus would leave the aux buses silent
// (the adapters pre-zero every routed output bus before process()); here we
// deliberately write all eight to prove the routing end-to-end.

#include <pulp/format/processor.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <memory>

namespace pulp::examples::multi_out {

using namespace pulp;

inline constexpr int kNumVoices = 8;

enum Params : state::ParamID {
    kMasterGainDb = 1,
};

/// One sine voice with a linear attack/release envelope. Fixed-point-free,
/// allocation-free — safe on the audio thread.
class SineVoice {
public:
    void set_sample_rate(float sr) noexcept { sample_rate_ = sr > 0 ? sr : 48000.0f; }

    void note_on(int note, int velocity) noexcept {
        note_ = note;
        gain_target_ = static_cast<float>(velocity) / 127.0f;
        active_ = true;
    }

    void note_off(int note) noexcept {
        if (active_ && note_ == note) gain_target_ = 0.0f;
    }

    bool active() const noexcept { return active_; }
    int note() const noexcept { return note_; }

    /// Render into a stereo (or mono) bus, ADDING to whatever is there.
    void render(audio::BufferView<float>& bus, std::size_t num_samples) noexcept {
        if (!active_) return;
        const float hz =
            440.0f * std::pow(2.0f, (static_cast<float>(note_) - 69.0f) / 12.0f);
        const float increment = hz / sample_rate_;
        const std::size_t nch = bus.num_channels();
        for (std::size_t i = 0; i < num_samples; ++i) {
            // Smooth the envelope toward the target (attack + release).
            gain_ += (gain_target_ - gain_) * 0.002f;
            phase_ += increment;
            if (phase_ >= 1.0f) phase_ -= 1.0f;
            const float s =
                std::sin(2.0f * 3.14159265358979f * phase_) * gain_ * 0.25f;
            for (std::size_t ch = 0; ch < nch; ++ch) bus.channel(ch)[i] += s;
        }
        // A fully released voice with a tiny residual gain frees up for reuse.
        if (gain_target_ == 0.0f && gain_ < 1e-4f) active_ = false;
    }

private:
    float sample_rate_ = 48000.0f;
    float phase_ = 0.0f;
    float gain_ = 0.0f;
    float gain_target_ = 0.0f;
    int note_ = -1;
    bool active_ = false;
};

class Processor : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        format::PluginDescriptor desc;
        desc.name = "PulpMultiOut";
        desc.manufacturer = "Pulp";
        desc.bundle_id = "com.pulp.multi-out";
        desc.version = "0.1.0";
        desc.category = format::PluginCategory::Instrument;
        desc.input_buses = {};  // instrument: no audio input
        desc.output_buses = {
            {"Voice 1 (Main)", 2}, {"Voice 2", 2}, {"Voice 3", 2},
            {"Voice 4", 2},        {"Voice 5", 2}, {"Voice 6", 2},
            {"Voice 7", 2},        {"Voice 8", 2},
        };
        desc.accepts_midi = true;
        desc.produces_midi = false;
        desc.tail_samples = -1;  // sustains until released
        return desc;
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({kMasterGainDb, "Gain", "dB", {-60, 12, 0, 0.1f}});
    }

    void prepare(const format::PrepareContext& ctx) override {
        for (auto& v : voices_)
            v.set_sample_rate(static_cast<float>(ctx.sample_rate));
    }

    // Legacy single-bus entry point — required override. Multi-bus hosts call
    // the richer process(ProcessBuffers&) below; this projection just renders
    // all voices into the main bus so the plugin still makes sound through a
    // host (or test) that only wires the main output.
    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer& midi_in, midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        output.clear();
        handle_midi(midi_in);
        const float gain = master_gain();
        const std::size_t ns = output.num_samples();
        for (auto& v : voices_) v.render(output, ns);
        apply_gain(output, gain, ns);
    }

    // Richer multi-bus surface: voice v -> output bus v. Each bus was pre-zeroed
    // by the adapter, so we ADD each voice into its own bus.
    void process(format::ProcessBuffers& audio, midi::MidiBuffer& midi_in,
                 midi::MidiBuffer&, const format::ProcessContext&) override {
        handle_midi(midi_in);
        const float gain = master_gain();
        for (std::size_t b = 0; b < audio.outputs.size(); ++b) {
            auto& bus = audio.outputs[b];
            if (!bus.active() || bus.buffer.num_channels() == 0) continue;
            const std::size_t ns = bus.buffer.num_samples();
            if (b < voices_.size()) voices_[b].render(bus.buffer, ns);
            apply_gain(bus.buffer, gain, ns);
        }
    }

    // Exposed for the headless routing test.
    std::array<SineVoice, kNumVoices>& voices() { return voices_; }

private:
    float master_gain() const {
        return std::pow(10.0f, state().get_value(kMasterGainDb) / 20.0f);
    }

    static void apply_gain(audio::BufferView<float>& bus, float gain,
                           std::size_t ns) {
        for (std::size_t ch = 0; ch < bus.num_channels(); ++ch) {
            auto span = bus.channel(ch);
            for (std::size_t i = 0; i < ns; ++i) span[i] *= gain;
        }
    }

    void handle_midi(midi::MidiBuffer& midi_in) {
        for (const auto& ev : midi_in) {
            if (ev.is_note_on() && ev.velocity() > 0) {
                voices_[next_voice_].note_on(ev.note(), ev.velocity());
                next_voice_ = (next_voice_ + 1) % kNumVoices;
            } else if (ev.is_note_off() ||
                       (ev.is_note_on() && ev.velocity() == 0)) {
                for (auto& v : voices_) v.note_off(ev.note());
            }
        }
    }

    std::array<SineVoice, kNumVoices> voices_{};
    int next_voice_ = 0;
};

inline std::unique_ptr<format::Processor> create_multi_out_synth() {
    return std::make_unique<Processor>();
}

}  // namespace pulp::examples::multi_out
