#pragma once

#include <pulp/format/processor.hpp>
#include <pulp/signal/adsr.hpp>
#include <pulp/signal/osc/va.hpp>
#include <pulp/state/store.hpp>

#include <cstddef>

namespace g11 {

// Stable product parameter identity; control code addresses it by its catalog name.
inline constexpr pulp::state::ParamID kFrequency = 1;
inline constexpr const char* kFrequencyPath = "voice.frequency";

class Instrument final : public pulp::format::Processor {
  public:
    Instrument(double initial_frequency, double initial_phase)
        : initial_frequency_(initial_frequency), initial_phase_(initial_phase) {}

    pulp::format::PluginDescriptor descriptor() const override {
        return {.name = "G11 live retuning instrument",
                .manufacturer = "Pulp",
                .bundle_id = "com.pulp.g11-live-retune",
                .version = "1.0.0",
                .category = pulp::format::PluginCategory::Instrument,
                .input_buses = {},
                .output_buses = {{"Voice", 1}},
                .accepts_midi = true,
                .produces_midi = false};
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({.id = kFrequency,
                             .name = kFrequencyPath,
                             .unit = "Hz",
                             .range = {.min = 20.0f, .max = 2000.0f,
                                       .default_value = static_cast<float>(initial_frequency_)}});
    }

    void prepare(const pulp::format::PrepareContext& context) override {
        sample_rate_ = context.sample_rate;
        frequency_ = state().param_reader(kFrequency);
        oscillator_.set_shape(pulp::signal::osc::VaShape::sine);
        oscillator_.reset(initial_phase_);
        envelope_.set_sample_rate(sample_rate_);
        envelope_.set_params({.attack = 0.01, .decay = 0.1, .sustain = 0.7, .release = 0.3});
        envelope_.reset();
        frames_ = 0;
        note_ons_ = 0;
        ++prepares_;
    }

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>&, pulp::midi::MidiBuffer& midi,
                 pulp::midi::MidiBuffer&, const pulp::format::ProcessContext&) override {
        const double increment = static_cast<double>(frequency_.get()) / sample_rate_;
        auto event = midi.begin();
        for (std::size_t frame = 0; frame < output.num_samples(); ++frame) {
            while (event != midi.end() && event->sample_offset <= static_cast<int>(frame)) {
                if (event->is_note_on()) {
                    envelope_.note_on();
                    ++note_ons_;
                } else if (event->is_note_off()) {
                    envelope_.note_off();
                }
                ++event;
            }
            last_envelope_ = envelope_.next();
            output.channel(0)[frame] =
                static_cast<float>(0.5 * last_envelope_ * oscillator_.next(increment));
            ++frames_;
        }
    }

    // Read only between callbacks in the offline harness, never concurrently.
    double phase() const { return oscillator_.phase(); }
    double envelope() const { return last_envelope_; }
    std::size_t frames() const { return frames_; }
    int note_ons() const { return note_ons_; }
    int prepares() const { return prepares_; }

  private:
    double initial_frequency_;
    double initial_phase_;
    double sample_rate_ = 0.0;
    pulp::state::ParamReader frequency_;
    pulp::signal::osc::VaOscillator oscillator_;
    pulp::signal::Adsr64 envelope_;
    double last_envelope_ = 0.0;
    std::size_t frames_ = 0;
    int note_ons_ = 0;
    int prepares_ = 0;
};

} // namespace g11
