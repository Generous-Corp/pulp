#pragma once

// PulpTranspose — a MIDI effect: shifts every note by a musical interval and
// passes everything else (CCs, pitch bend, SysEx, clock) straight through.
//
// A `PluginCategory::MidiEffect` processor takes no audio and produces none;
// its whole contract is `midi_in` -> `midi_out`. On AU v2 it packages as `aumi`
// (kAudioUnitType_MIDIProcessor) and appears in a host's MIDI-FX slot.

#include <pulp/format/processor.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <memory>

namespace pulp::examples::transpose {

class TransposeProcessor : public pulp::format::Processor {
public:
    static constexpr pulp::state::ParamID kSemitones = 1;
    static constexpr pulp::state::ParamID kOctaves = 2;

    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "PulpTranspose",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.transpose",
            .version = "0.1.0",
            .category = pulp::format::PluginCategory::MidiEffect,
            // A MIDI effect declares no audio buses.
            .input_buses = {},
            .output_buses = {},
            .accepts_midi = true,
            .produces_midi = true,
        };
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({
            .id = kSemitones,
            .name = "Semitones",
            .unit = "",
            .range = {-12.0f, 12.0f, 0.0f, 1.0f},
        });
        store.add_parameter({
            .id = kOctaves,
            .name = "Octaves",
            .unit = "",
            .range = {-3.0f, 3.0f, 0.0f, 1.0f},
        });
    }

    void prepare(const pulp::format::PrepareContext&) override {}

    void process(pulp::audio::BufferView<float>&,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer& midi_in,
                 pulp::midi::MidiBuffer& midi_out,
                 const pulp::format::ProcessContext&) override {
        const int shift = static_cast<int>(state().get_value(kSemitones)) +
                          12 * static_cast<int>(state().get_value(kOctaves));

        for (const auto& event : midi_in) {
            auto out = event;
            const auto status = event.message.data()[0];
            const auto kind = static_cast<std::uint8_t>(status & 0xF0);
            // Note on / note off / poly aftertouch all carry a note number in
            // data byte 1. Everything else passes through byte-identical.
            if (kind == 0x80 || kind == 0x90 || kind == 0xA0) {
                const int shifted =
                    std::clamp(static_cast<int>(event.message.data()[1]) + shift,
                               0, 127);
                out.message = choc::midi::ShortMessage(
                    status, static_cast<std::uint8_t>(shifted),
                    event.message.data()[2]);
            }
            midi_out.add(out);
        }

        // SysEx travels in its own sidecar and is never transposed.
        for (const auto& sysex : midi_in.sysex()) {
            midi_out.add_sysex_copy(sysex.data.data(), sysex.data.size(),
                                    sysex.sample_offset, sysex.timestamp);
        }
    }
};

inline std::unique_ptr<pulp::format::Processor> create_transpose() {
    return std::make_unique<TransposeProcessor>();
}

} // namespace pulp::examples::transpose
