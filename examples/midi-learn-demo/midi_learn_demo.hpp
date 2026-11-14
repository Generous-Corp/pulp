#pragma once

// MidiLearnDemo: a stereo level and pan effect whose two parameters can be
// mapped to hardware controllers. It shows the whole MIDI Learn path:
//
//   - the processor owns a state::MidiLearnEngine and registers it, so a host
//     that reports live controller input (VST3 IMidiLearn) can reach it;
//   - process() makes one midi_learn_process_block() call, which routes
//     incoming MIDI to the mapped parameters and sends feedback to the
//     controller on the plug-in's MIDI output;
//   - the mappings are saved with the plug-in state;
//   - midi_learn_demo_ui.hpp builds the editor pieces: right-click menu,
//     overlay and undo.

#include <pulp/format/midi_learn_support.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/state/midi_learn_engine.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace pulp::examples {

enum MidiLearnDemoParams : state::ParamID {
    kDemoLevel = 1, // dB
    kDemoPan = 2,   // -1 (left) to 1 (right)
};

class MidiLearnDemoProcessor : public format::Processor {
  public:
    MidiLearnDemoProcessor() : learn_handle_(*this, learn_) {}

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "MidiLearnDemo",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.midi-learn-demo",
            .version = "1.0.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
            .accepts_midi = true,
            .produces_midi = true,
        };
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({
            .id = kDemoLevel,
            .name = "Level",
            .unit = "dB",
            .range = {-60.0f, 12.0f, 0.0f, 0.1f},
        });
        store.add_parameter({
            .id = kDemoPan,
            .name = "Pan",
            .unit = "",
            .range = {-1.0f, 1.0f, 0.0f, 0.01f},
        });
    }

    // descriptor() allocates, so the capability claim is resolved here and the
    // audio thread reads the stored value.
    void prepare(const format::PrepareContext&) override {
        feedback_ = format::midi_feedback_capabilities(host_format_, descriptor());
    }

    void process(audio::BufferView<float>& output, const audio::BufferView<const float>& input,
                 midi::MidiBuffer& midi_in, midi::MidiBuffer& midi_out,
                 const format::ProcessContext&) override {
        // The format is fixed by the host that loaded the plug-in; a VST3 build
        // would pass MidiLearnFormat::vst3. Passing the format an entry point
        // knows keeps the capability claim honest for that host.
        format::midi_learn_process_block(learn_, state(), midi_in, midi_out, feedback_);

        const float gain = std::pow(10.0f, state().get_value(kDemoLevel) / 20.0f);
        const float pan = state().get_value(kDemoPan);
        const float left = gain * std::min(1.0f, 1.0f - pan);
        const float right = gain * std::min(1.0f, 1.0f + pan);
        for (std::size_t ch = 0; ch < output.num_channels() && ch < input.num_channels(); ++ch) {
            const float g = ch == 0 ? left : right;
            auto in = input.channel(ch);
            auto out = output.channel(ch);
            for (std::size_t i = 0; i < output.num_samples(); ++i)
                out[i] = in[i] * g;
        }
    }

    // Mappings belong to the controller, not the sound, so they are saved once
    // with the plug-in state and survive preset changes.
    std::vector<uint8_t> serialize_plugin_state() const override {
        return learn_.save_state();
    }
    bool deserialize_plugin_state(std::span<const uint8_t> data) override {
        learn_.load_state(data, &state());
        return true;
    }

    state::MidiLearnEngine& learn() {
        return learn_;
    }

    void set_host_format(format::MidiLearnFormat f) {
        host_format_ = f;
    }

  private:
    state::MidiLearnEngine learn_;
    format::MidiLearnHandle learn_handle_; // declared after learn_
    format::MidiLearnFormat host_format_ = format::MidiLearnFormat::vst3;
    format::MidiFeedbackCapabilities feedback_{};
};

inline std::unique_ptr<format::Processor>
create_midi_learn_demo(format::MidiLearnFormat host = format::MidiLearnFormat::vst3) {
    auto processor = std::make_unique<MidiLearnDemoProcessor>();
    processor->set_host_format(host);
    return processor;
}

// One factory per entry point, so each build reports the feedback support of
// the format it actually is.
inline std::unique_ptr<format::Processor> create_midi_learn_demo_vst3() {
    return create_midi_learn_demo(format::MidiLearnFormat::vst3);
}
inline std::unique_ptr<format::Processor> create_midi_learn_demo_clap() {
    return create_midi_learn_demo(format::MidiLearnFormat::clap);
}

} // namespace pulp::examples
