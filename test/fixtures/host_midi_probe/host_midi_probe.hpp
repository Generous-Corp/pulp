#pragma once

// A MIDI effect that answers every note an octave up, at the same sample
// offset, and passes everything else through. Built as an AU MIDI effect
// (aumi) so the AUAudioUnit slot's MIDI output path is tested against a real
// plug-in binary: the effect probe in test/fixtures/host_probe is an aufx,
// which AU never sends MIDI.

#include <pulp/format/processor.hpp>
#include <pulp/midi/buffer.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>

namespace pulp::test_fixtures {

class HostMidiProbe final : public format::Processor {
  public:
    static constexpr state::ParamID kShift = 1;
    static constexpr int kDefaultShift = 12;

    format::PluginDescriptor descriptor() const override {
        format::PluginDescriptor d;
        d.name = "Pulp Host MIDI Probe";
        d.manufacturer = "Pulp";
        d.bundle_id = "dev.pulp.test.host-midi-probe";
        d.version = "1.0.0";
        d.category = format::PluginCategory::MidiEffect;
        d.input_buses = {};
        d.output_buses = {};
        d.accepts_midi = true;
        d.produces_midi = true;
        return d;
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter(
            {.id = kShift, .name = "Shift", .range = {-24.0f, 24.0f, (float)kDefaultShift, 1.0f}});
    }

    void prepare(const format::PrepareContext&) override {}

    void process(audio::BufferView<float>&, const audio::BufferView<const float>&,
                 midi::MidiBuffer& midi_in, midi::MidiBuffer& midi_out,
                 const format::ProcessContext&) override {
        const int shift = static_cast<int>(state().get_value(kShift));
        for (const auto& event : midi_in) {
            auto out = event;
            const auto status = event.message.data()[0];
            const auto kind = static_cast<std::uint8_t>(status & 0xF0);
            if (kind == 0x80 || kind == 0x90)
                out.message = choc::midi::ShortMessage(
                    status,
                    static_cast<std::uint8_t>(
                        std::clamp(static_cast<int>(event.message.data()[1]) + shift, 0, 127)),
                    event.message.data()[2]);
            midi_out.add(out);
        }
    }
};

inline std::unique_ptr<format::Processor> create_host_midi_probe() {
    return std::make_unique<HostMidiProbe>();
}

} // namespace pulp::test_fixtures
