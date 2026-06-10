#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor_block_adapter.hpp>
#include <pulp/midi/mpe_buffer.hpp>
#include <pulp/midi/ump_buffer.hpp>

#include <array>
#include <cstddef>
#include <stdexcept>

namespace {

struct ConstAudioView {
    explicit ConstAudioView(pulp::audio::Buffer<float>& buffer) {
        REQUIRE(buffer.num_channels() <= ptrs.size());
        for (std::size_t ch = 0; ch < buffer.num_channels(); ++ch) {
            ptrs[ch] = buffer.channel(ch).data();
        }
        view = {ptrs.data(), buffer.num_channels(), buffer.num_samples()};
    }

    std::array<const float*, 16> ptrs{};
    pulp::audio::BufferView<const float> view;
};

class RecordingProcessor : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "RecordingProcessor",
            .manufacturer = "Pulp",
            .bundle_id = "dev.pulp.recording-processor",
            .version = "1.0.0",
        };
    }

    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}

    void process(pulp::audio::BufferView<float>& audio_output,
                 const pulp::audio::BufferView<const float>& audio_input,
                 pulp::midi::MidiBuffer& midi_in,
                 pulp::midi::MidiBuffer& midi_out,
                 const pulp::format::ProcessContext& context) override {
        if (throw_on_process) throw std::runtime_error("test");
        ++calls;
        input_channels = audio_input.num_channels();
        output_channels = audio_output.num_channels();
        frame_count = audio_output.num_samples();
        sample_rate = context.sample_rate;
        num_samples = context.num_samples;
        observed_sidechain = sidechain_input();
        observed_mpe = mpe_input();
        observed_ump = ump_input();
        observed_params = param_events();
        observed_midi_in = &midi_in;
        observed_midi_out = &midi_out;
    }

    bool throw_on_process = false;
    int calls = 0;
    std::size_t input_channels = 0;
    std::size_t output_channels = 0;
    std::size_t frame_count = 0;
    double sample_rate = 0.0;
    int num_samples = 0;
    const pulp::audio::BufferView<const float>* observed_sidechain = nullptr;
    const pulp::midi::MpeBuffer* observed_mpe = nullptr;
    const pulp::midi::UmpBuffer* observed_ump = nullptr;
    const pulp::state::ParameterEventQueue* observed_params = nullptr;
    pulp::midi::MidiBuffer* observed_midi_in = nullptr;
    pulp::midi::MidiBuffer* observed_midi_out = nullptr;
};

pulp::format::ProcessBlock block_with_buses(pulp::format::BusBufferSet& buses,
                                            double sample_rate = 48000.0,
                                            std::uint32_t frames = 64) {
    pulp::format::ProcessBlock block;
    block.sample_rate = sample_rate;
    block.frame_count = frames;
    block.render_speed = 1.0;
    block.buses = &buses;
    return block;
}

} // namespace

TEST_CASE("process_processor_block invokes Processor with main buses",
          "[format][process-block][processor-adapter]") {
    pulp::audio::Buffer<float> input(2, 64);
    pulp::audio::Buffer<float> output(2, 64);
    ConstAudioView input_view(input);

    pulp::format::BusBufferSet buses;
    REQUIRE(buses.add_input("main", input_view.view));
    REQUIRE(buses.add_output("main", output.view()));

    auto block = block_with_buses(buses);
    RecordingProcessor processor;
    pulp::format::ProcessorBlockAdapterScratch scratch;

    REQUIRE(pulp::format::process_processor_block(processor, block, scratch));
    REQUIRE(processor.calls == 1);
    REQUIRE(processor.input_channels == 2);
    REQUIRE(processor.output_channels == 2);
    REQUIRE(processor.frame_count == 64);
    REQUIRE(processor.sample_rate == 48000.0);
    REQUIRE(processor.num_samples == 64);
    REQUIRE(processor.observed_params == nullptr);
    REQUIRE(processor.observed_midi_in == &scratch.fallback_midi_in);
    REQUIRE(processor.observed_midi_out == &scratch.fallback_midi_out);
}

TEST_CASE("process_processor_block allows output-only instrument blocks",
          "[format][process-block][processor-adapter]") {
    pulp::audio::Buffer<float> output(2, 32);
    pulp::format::BusBufferSet buses;
    REQUIRE(buses.add_output("main", output.view()));

    auto block = block_with_buses(buses, 44100.0, 32);
    RecordingProcessor processor;
    pulp::format::ProcessorBlockAdapterScratch scratch;

    REQUIRE(pulp::format::process_processor_block(processor, block, scratch));
    REQUIRE(processor.calls == 1);
    REQUIRE(processor.input_channels == 0);
    REQUIRE(processor.output_channels == 2);
    REQUIRE(processor.sample_rate == 44100.0);
    REQUIRE(processor.num_samples == 32);
}

TEST_CASE("process_processor_block requires a valid active main output",
          "[format][process-block][processor-adapter]") {
    pulp::audio::Buffer<float> input(2, 64);
    ConstAudioView input_view(input);
    pulp::format::BusBufferSet buses;
    REQUIRE(buses.add_input("main", input_view.view));

    auto block = block_with_buses(buses);
    RecordingProcessor processor;
    pulp::format::ProcessorBlockAdapterScratch scratch;

    REQUIRE_FALSE(pulp::format::process_processor_block(processor, block, scratch));
    REQUIRE(processor.calls == 0);

    block.frame_count = 0;
    REQUIRE_FALSE(pulp::format::process_processor_block(processor, block, scratch));
    REQUIRE(processor.calls == 0);
}

TEST_CASE("process_processor_block publishes event sidecars and restores previous pointers",
          "[format][process-block][processor-adapter]") {
    pulp::audio::Buffer<float> input(2, 64);
    pulp::audio::Buffer<float> output(2, 64);
    pulp::audio::Buffer<float> sidechain(1, 64);
    pulp::audio::Buffer<float> previous_sidechain(1, 64);
    ConstAudioView input_view(input);
    ConstAudioView sidechain_view(sidechain);
    ConstAudioView previous_sidechain_view(previous_sidechain);

    pulp::format::BusBufferSet buses;
    REQUIRE(buses.add_input("main", input_view.view));
    REQUIRE(buses.add_input("key", sidechain_view.view, pulp::format::BusRole::Sidechain));
    REQUIRE(buses.add_output("main", output.view()));

    pulp::midi::MidiBuffer midi_in;
    pulp::midi::MidiBuffer midi_out;
    pulp::midi::MpeBuffer mpe;
    pulp::midi::UmpBuffer ump;
    pulp::state::ParameterEventQueue params;
    REQUIRE(params.push({42, 0, 0.5f, 0}));

    pulp::format::EventBlock events;
    events.midi_in = &midi_in;
    events.midi_out = &midi_out;
    events.mpe_input = &mpe;
    events.ump_input = &ump;
    events.parameter_events = &params;

    auto block = block_with_buses(buses);
    block.events = &events;

    RecordingProcessor processor;
    pulp::state::ParameterEventQueue previous_params;
    pulp::midi::MpeBuffer previous_mpe;
    pulp::midi::UmpBuffer previous_ump;
    processor.set_sidechain(&previous_sidechain_view.view);
    processor.set_param_events(&previous_params);
    processor.set_mpe_input(&previous_mpe);
    processor.set_ump_input(&previous_ump);

    pulp::format::ProcessorBlockAdapterScratch scratch;
    REQUIRE(pulp::format::process_processor_block(processor, block, scratch));
    REQUIRE(processor.observed_sidechain != nullptr);
    REQUIRE(processor.observed_sidechain->num_channels() == 1);
    REQUIRE(processor.observed_midi_in == &midi_in);
    REQUIRE(processor.observed_midi_out == &midi_out);
    REQUIRE(processor.observed_params == &params);
    REQUIRE(processor.observed_mpe == &mpe);
    REQUIRE(processor.observed_ump == &ump);

    REQUIRE(processor.sidechain_input() == &previous_sidechain_view.view);
    REQUIRE(processor.param_events() == &previous_params);
    REQUIRE(processor.mpe_input() == &previous_mpe);
    REQUIRE(processor.ump_input() == &previous_ump);
}

TEST_CASE("process_processor_block preserves EventBlock parameter fallback semantics",
          "[format][process-block][processor-adapter]") {
    pulp::audio::Buffer<float> output(2, 64);
    pulp::format::BusBufferSet buses;
    REQUIRE(buses.add_output("main", output.view()));

    pulp::format::EventBlock events;
    auto block = block_with_buses(buses);
    block.events = &events;

    RecordingProcessor processor;
    pulp::format::ProcessorBlockAdapterScratch scratch;
    REQUIRE(pulp::format::process_processor_block(processor, block, scratch));
    REQUIRE(processor.observed_params == &scratch.empty_parameter_events);
    REQUIRE(processor.observed_params->empty());
}

TEST_CASE("process_processor_block restores sidecars when processor throws",
          "[format][process-block][processor-adapter]") {
    pulp::audio::Buffer<float> output(2, 64);
    pulp::format::BusBufferSet buses;
    REQUIRE(buses.add_output("main", output.view()));
    auto block = block_with_buses(buses);

    RecordingProcessor processor;
    processor.throw_on_process = true;
    pulp::state::ParameterEventQueue previous_params;
    processor.set_param_events(&previous_params);

    pulp::format::ProcessorBlockAdapterScratch scratch;
    REQUIRE_FALSE(pulp::format::process_processor_block(processor, block, scratch));
    REQUIRE(processor.param_events() == &previous_params);
}
