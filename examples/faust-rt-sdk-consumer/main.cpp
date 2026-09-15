#include <pulp/audio/buffer.hpp>
#include <pulp/dsl/faust_processor.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/store.hpp>

#include <cmath>
#include <cstddef>
#include <vector>

namespace {

class InstalledGainDsp final : public dsp {
  public:
    int getNumInputs() override {
        return 2;
    }
    int getNumOutputs() override {
        return 2;
    }
    int getSampleRate() override {
        return sample_rate_;
    }
    void buildUserInterface(UI*) override {}
    void init(int sample_rate) override {
        sample_rate_ = sample_rate;
    }
    void instanceInit(int sample_rate) override {
        init(sample_rate);
    }
    void instanceConstants(int sample_rate) override {
        sample_rate_ = sample_rate;
    }
    void instanceResetUserInterface() override {}
    void instanceClear() override {}
    dsp* clone() override {
        return new InstalledGainDsp(*this);
    }
    void metadata(Meta* meta) override {
        meta->declare("name", "Installed Faust RT Gain");
        meta->declare("author", "Pulp");
        meta->declare("version", "1.0.0");
    }
    void compute(int frames, FAUSTFLOAT** inputs, FAUSTFLOAT** outputs) override {
        for (int channel = 0; channel < 2; ++channel) {
            for (int frame = 0; frame < frames; ++frame) {
                outputs[channel][frame] = inputs[channel][frame] * 0.25f;
            }
        }
    }

  private:
    int sample_rate_ = 0;
};

bool render_and_check(pulp::dsl::FaustProcessor<InstalledGainDsp>& processor, int frames) {
    pulp::audio::Buffer<float> input(2, static_cast<std::size_t>(frames));
    pulp::audio::Buffer<float> output(2, static_cast<std::size_t>(frames));
    for (std::size_t channel = 0; channel < 2; ++channel) {
        for (int frame = 0; frame < frames; ++frame) {
            input.channel(channel)[static_cast<std::size_t>(frame)] =
                static_cast<float>((channel + 1) * (frame + 1)) / 2048.0f;
        }
    }

    std::vector<const float*> input_ptrs{input.channel(0).data(), input.channel(1).data()};
    pulp::audio::BufferView<const float> input_view(input_ptrs.data(), input_ptrs.size(),
                                                    static_cast<std::size_t>(frames));
    auto output_view = output.view();
    pulp::midi::MidiBuffer midi_in, midi_out;
    pulp::format::ProcessContext context;
    context.sample_rate = 48000.0;
    context.num_samples = frames;
    processor.process(output_view, input_view, midi_in, midi_out, context);

    for (std::size_t channel = 0; channel < 2; ++channel) {
        for (int frame = 0; frame < frames; ++frame) {
            const float expected = input.channel(channel)[static_cast<std::size_t>(frame)] * 0.25f;
            if (std::abs(output.channel(channel)[static_cast<std::size_t>(frame)] - expected) >
                1e-7f) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

int main() {
    pulp::dsl::FaustProcessor<InstalledGainDsp> processor;
    pulp::state::StateStore state;
    processor.set_state_store(&state);
    processor.define_parameters(state);
    processor.prepare({48000.0, 1024, 2, 2});

    // H1 proves the installed Processor directly. H4 owns the later
    // ProcessorNodeInstance + authored SignalGraph path, and Z5 must retain the
    // same numerical oracle when closing that reachability dependency.
    if (!render_and_check(processor, 37))
        return 1;
    if (!render_and_check(processor, 1024))
        return 2;
    return 0;
}
