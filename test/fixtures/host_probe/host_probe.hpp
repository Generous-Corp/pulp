#pragma once

// A plug-in that reports what its host gave it.
//
// Built as a real VST3 and CLAP bundle so a hosting test can load it through
// PluginSlot and check, from inside a plug-in, what the host delivered: the
// transport, the process mode, the prepared sample rate and block size, the
// sidechain bus, mapped MIDI controllers, and pitch bend. Each observation is
// written to a read-only parameter, which is the channel every format already
// carries back to the host.
//
// It also acts on command: setting the Command parameter to 1 makes the
// plug-in change its own Gain parameter (a plug-in-originated edit), and 2
// makes it report a new latency through Processor::flag_latency_changed().
// It writes a constant to its aux output so a host can see that bus is live.

#include <pulp/format/processor.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>

namespace pulp::test_fixtures {

class HostProbe final : public format::Processor {
  public:
    // Host-writable.
    static constexpr state::ParamID kGain = 1;
    static constexpr state::ParamID kCommand = 2;
    // Read-only observations.
    static constexpr state::ParamID kTempo = 20;
    static constexpr state::ParamID kPlaying = 21;
    static constexpr state::ParamID kOffline = 22;
    static constexpr state::ParamID kSidechainPeak = 23;
    static constexpr state::ParamID kLastCc7 = 24;
    static constexpr state::ParamID kPitchBend = 25;
    static constexpr state::ParamID kSampleRate = 26;
    static constexpr state::ParamID kMaxBlock = 27;
    static constexpr state::ParamID kBeats = 28;
    static constexpr state::ParamID kTimeSigNumerator = 29;

    static constexpr float kEditedGainDb = -6.0f;
    static constexpr int kInitialLatency = 32;
    static constexpr int kChangedLatency = 256;
    static constexpr float kAuxLevel = 0.5f;

    format::PluginDescriptor descriptor() const override {
        format::PluginDescriptor d;
        d.name = "Pulp Host Probe";
        d.manufacturer = "Pulp";
        d.bundle_id = "dev.pulp.test.host-probe";
        d.version = "1.0.0";
        d.category = format::PluginCategory::Effect;
        d.input_buses = {{"Main In", 2, false}, {"Sidechain", 2, true, format::BusRole::Sidechain}};
        d.output_buses = {{"Main Out", 2, false}, {"Aux Out", 2, true, format::BusRole::Aux}};
        d.accepts_midi = true;
        return d;
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter(
            {.id = kGain, .name = "Gain", .unit = "dB", .range = {-60.0f, 12.0f, 0.0f}});
        store.add_parameter({.id = kCommand, .name = "Command", .range = {0.0f, 2.0f, 0.0f, 1.0f}});
        auto observed = [&](state::ParamID id, const char* name, float max) {
            state::ParamInfo info{.id = id, .name = name, .range = {0.0f, max, 0.0f}};
            info.read_only = true;
            info.automatable = false;
            store.add_parameter(info);
        };
        observed(kTempo, "Seen Tempo", 1000.0f);
        observed(kPlaying, "Seen Playing", 1.0f);
        observed(kOffline, "Seen Offline", 1.0f);
        observed(kSidechainPeak, "Seen Sidechain Peak", 1.0f);
        observed(kLastCc7, "Seen CC7", 127.0f);
        observed(kPitchBend, "Seen Pitch Bend", 16383.0f);
        observed(kSampleRate, "Seen Sample Rate", 192000.0f);
        observed(kMaxBlock, "Seen Max Block", 65536.0f);
        observed(kBeats, "Seen Beats", 100000.0f);
        observed(kTimeSigNumerator, "Seen Time Signature", 32.0f);
    }

    // Recorded here and published from process(): formats report a plug-in's
    // own parameter changes from the audio callback, not from prepare.
    void prepare(const format::PrepareContext& context) override {
        prepared_rate_ = static_cast<float>(context.sample_rate);
        prepared_block_ = static_cast<float>(context.max_buffer_size);
    }

    int latency_samples() const override {
        return latency_.load(std::memory_order_acquire);
    }

    void process(audio::BufferView<float>& output, const audio::BufferView<const float>& input,
                 midi::MidiBuffer& midi_in, midi::MidiBuffer&,
                 const format::ProcessContext& context) override {
        observe(midi_in, context);
        const float gain = std::pow(10.0f, state().get_value(kGain) / 20.0f);
        for (std::size_t c = 0; c < output.num_channels(); ++c)
            for (std::size_t i = 0; i < output.num_samples(); ++i)
                output.channel(c)[i] = c < input.num_channels() ? input.channel(c)[i] * gain : 0.0f;
    }

    // Multi-bus entry point: reads the sidechain and writes the aux output,
    // then runs the main path.
    void process(format::ProcessBuffers& audio, midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out, const format::ProcessContext& context) override {
        float peak = 0.0f;
        if (const auto* side = audio.sidechain_input()) {
            for (std::size_t c = 0; c < side->num_channels(); ++c)
                for (std::size_t i = 0; i < side->num_samples(); ++i)
                    peak = std::max(peak, std::abs(side->channel_ptr(c)[i]));
        }
        state().set_value(kSidechainPeak, std::min(peak, 1.0f));
        if (auto* aux = audio.outputs.find(format::BusRole::Aux); aux && aux->active()) {
            for (std::size_t c = 0; c < aux->buffer.num_channels(); ++c)
                std::fill_n(aux->buffer.channel_ptr(c), aux->buffer.num_samples(), kAuxLevel);
        }
        Processor::process(audio, midi_in, midi_out, context);
    }

  private:
    void observe(const midi::MidiBuffer& midi_in, const format::ProcessContext& context) {
        using F = format::TransportField;
        state().set_value(kSampleRate, prepared_rate_);
        state().set_value(kMaxBlock, prepared_block_);
        if (context.has_transport(F::Tempo))
            state().set_value(kTempo, (float)context.tempo_bpm);
        if (context.has_transport(F::Playing))
            state().set_value(kPlaying, context.is_playing ? 1.0f : 0.0f);
        if (context.has_transport(F::BeatPosition))
            state().set_value(kBeats, (float)context.position_beats);
        if (context.has_transport(F::TimeSignature))
            state().set_value(kTimeSigNumerator, (float)context.time_sig_numerator);
        state().set_value(kOffline, context.is_offline() ? 1.0f : 0.0f);

        for (const auto& event : midi_in) {
            const auto* d = event.message.data();
            if (event.message.length() < 3)
                continue;
            if ((d[0] & 0xF0) == 0xB0 && d[1] == 7)
                state().set_value(kLastCc7, (float)d[2]);
            if ((d[0] & 0xF0) == 0xE0)
                state().set_value(kPitchBend, (float)(d[1] | (d[2] << 7)));
        }

        const int command = (int)std::lround(state().get_value(kCommand));
        if (command == handled_command_)
            return;
        handled_command_ = command;
        if (command == 1) {
            state().set_value(kGain, kEditedGainDb);
        } else if (command == 2) {
            latency_.store(kChangedLatency, std::memory_order_release);
            flag_latency_changed();
        }
    }

    std::atomic<int> latency_{kInitialLatency};
    float prepared_rate_ = 0.0f;
    float prepared_block_ = 0.0f;
    int handled_command_ = 0;
};

inline std::unique_ptr<format::Processor> create_host_probe() {
    return std::make_unique<HostProbe>();
}

} // namespace pulp::test_fixtures
