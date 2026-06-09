#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/format/host_quirks.hpp>
#include <pulp/format/offline_render_host.hpp>
#include <pulp/format/plugin_state_io.hpp>
#include <pulp/format/process_block.hpp>
#include <pulp/format/processor_block_adapter.hpp>
#include <pulp/midi/ump_buffer.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using Catch::Matchers::WithinAbs;

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

class HostMatrixProcessor : public pulp::format::Processor {
public:
    HostMatrixProcessor() { last = this; }
    ~HostMatrixProcessor() override {
        if (last == this) {
            last = nullptr;
        }
    }

    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "HostRuntimeMatrix",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.host-runtime-matrix",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Main In", 2, false}, {"Sidechain", 1, true}},
            .output_buses = {{"Main Out", 2, false}, {"Aux Out", 2, true}},
            .accepts_midi = true,
            .produces_midi = true,
            .supports_ump = true,
            .tail_samples = tail_samples_value,
        };
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({
            .id = kGainParam,
            .name = "Gain",
            .unit = "x",
            .range = {0.0f, 4.0f, 1.0f, 0.01f},
        });
        store.add_parameter({
            .id = kBypassParam,
            .name = "Bypass",
            .unit = "",
            .range = {0.0f, 1.0f, 0.0f, 1.0f},
        });
    }

    void prepare(const pulp::format::PrepareContext& context) override {
        ++prepare_calls;
        max_buffer_size = context.max_buffer_size;
        prepared_input_channels = context.input_channels;
        prepared_output_channels = context.output_channels;
    }

    int latency_samples() const override { return latency_samples_value; }

    std::vector<std::uint8_t> serialize_plugin_state() const override {
        return {plugin_state.begin(), plugin_state.end()};
    }

    bool deserialize_plugin_state(std::span<const std::uint8_t> data) override {
        ++deserialize_calls;
        std::string payload;
        payload.reserve(data.size());
        for (const auto byte : data) {
            payload.push_back(static_cast<char>(byte));
        }
        if (!rejected_plugin_state.empty() && payload == rejected_plugin_state) {
            return false;
        }
        plugin_state = payload;
        return true;
    }

    void process(pulp::audio::BufferView<float>& audio_output,
                 const pulp::audio::BufferView<const float>& audio_input,
                 pulp::midi::MidiBuffer& midi_in,
                 pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext& context) override {
        const auto slot = static_cast<std::size_t>(std::min(process_calls,
            static_cast<int>(block_sizes.size() - 1)));
        ++process_calls;

        observed_context = context;
        observed_input_channels = audio_input.num_channels();
        observed_output_channels = audio_output.num_channels();
        observed_sidechain_channels = sidechain_input() ? sidechain_input()->num_channels() : 0;
        const auto* params = param_events();
        observed_param_events = params ? params->size() : 0;
        observed_midi_events = midi_in.size();
        observed_sysex_events = midi_in.sysex_size();
        observed_ump_events = ump_input() ? ump_input()->size() : 0;
        observed_bypass_value = state().get_value(kBypassParam);

        total_param_events += observed_param_events;
        total_midi_events += observed_midi_events;
        total_sysex_events += observed_sysex_events;
        total_ump_events += observed_ump_events;
        block_sizes[slot] = context.num_samples;
        sample_positions[slot] = context.position_samples;
        param_events_per_block[slot] = observed_param_events;
        midi_events_per_block[slot] = observed_midi_events;
        if (params && !params->empty()) {
            const auto& event = *params->begin();
            first_param_ids[slot] = event.param_id;
            first_param_offsets[slot] = event.sample_offset;
            first_param_values[slot] = event.value;
        } else {
            first_param_offsets[slot] = -1;
            first_param_values[slot] = 0.0f;
        }
        if (!midi_in.empty()) {
            first_midi_offsets[slot] = midi_in[0].sample_offset;
            first_midi_notes[slot] = midi_in[0].note();
        } else {
            first_midi_offsets[slot] = -1;
            first_midi_notes[slot] = -1;
        }

        const float gain = state().get_value(kGainParam);
        for (std::size_t ch = 0; ch < audio_output.num_channels(); ++ch) {
            auto out = audio_output.channel(ch);
            if (ch >= audio_input.num_channels()) {
                std::fill(out.begin(), out.end(), 0.0f);
                continue;
            }
            auto in = audio_input.channel(ch);
            for (std::size_t i = 0; i < audio_output.num_samples(); ++i) {
                out[i] = in[i] * gain;
            }
        }
    }

    static constexpr pulp::state::ParamID kGainParam = 1;
    static constexpr pulp::state::ParamID kBypassParam = 2;
    static HostMatrixProcessor* last;

    int latency_samples_value = 0;
    int tail_samples_value = 0;
    std::string plugin_state;
    std::string rejected_plugin_state;

    int prepare_calls = 0;
    int max_buffer_size = 0;
    int prepared_input_channels = 0;
    int prepared_output_channels = 0;
    int process_calls = 0;
    int deserialize_calls = 0;

    pulp::format::ProcessContext observed_context{};
    std::size_t observed_input_channels = 0;
    std::size_t observed_output_channels = 0;
    std::size_t observed_sidechain_channels = 0;
    std::size_t observed_param_events = 0;
    std::size_t observed_midi_events = 0;
    std::size_t observed_sysex_events = 0;
    std::size_t observed_ump_events = 0;
    float observed_bypass_value = 0.0f;

    std::size_t total_param_events = 0;
    std::size_t total_midi_events = 0;
    std::size_t total_sysex_events = 0;
    std::size_t total_ump_events = 0;
    std::array<int, 8> block_sizes{};
    std::array<std::int64_t, 8> sample_positions{};
    std::array<std::size_t, 8> param_events_per_block{};
    std::array<std::size_t, 8> midi_events_per_block{};
    std::array<pulp::state::ParamID, 8> first_param_ids{};
    std::array<int, 8> first_param_offsets{};
    std::array<float, 8> first_param_values{};
    std::array<int, 8> first_midi_offsets{};
    std::array<int, 8> first_midi_notes{};
};

HostMatrixProcessor* HostMatrixProcessor::last = nullptr;

std::unique_ptr<pulp::format::Processor> create_host_matrix_processor() {
    return std::make_unique<HostMatrixProcessor>();
}

pulp::audio::BufferView<const float> const_view(const pulp::audio::Buffer<float>& buffer,
                                                std::span<const float* const> ptrs) {
    return {ptrs.data(), buffer.num_channels(), buffer.num_samples()};
}

} // namespace

TEST_CASE("Host runtime matrix routes ProcessBlock buses events and bypass parameter state",
          "[format][host-matrix][process-block]") {
    pulp::state::StateStore store;
    HostMatrixProcessor processor;
    processor.set_state_store(&store);
    processor.define_parameters(store);
    store.set_value(HostMatrixProcessor::kGainParam, 2.0f);
    store.set_value(HostMatrixProcessor::kBypassParam, 1.0f);

    pulp::audio::Buffer<float> input(2, 64);
    pulp::audio::Buffer<float> output(2, 64);
    pulp::audio::Buffer<float> sidechain(1, 64);
    pulp::audio::Buffer<float> aux_output(2, 64);
    for (std::size_t ch = 0; ch < aux_output.num_channels(); ++ch) {
        auto aux = aux_output.channel(ch);
        std::fill(aux.begin(), aux.end(), -123.0f);
    }
    for (std::size_t i = 0; i < input.num_samples(); ++i) {
        input.channel(0)[i] = static_cast<float>(i + 1);
        input.channel(1)[i] = static_cast<float>(100 + i);
        sidechain.channel(0)[i] = static_cast<float>(200 + i);
    }
    ConstAudioView input_view(input);
    ConstAudioView sidechain_view(sidechain);

    pulp::format::BusBufferSet buses;
    REQUIRE(buses.add_input("main", input_view.view));
    REQUIRE(buses.add_input("key", sidechain_view.view, pulp::format::BusRole::Sidechain));
    REQUIRE(buses.add_output("main", output.view()));
    REQUIRE(buses.add_output("aux", aux_output.view(), pulp::format::BusRole::Aux));
    REQUIRE(buses.size() == 4);
    REQUIRE(buses.first(pulp::format::BusDirection::Input,
                        pulp::format::BusRole::Sidechain) != nullptr);
    REQUIRE(buses.first(pulp::format::BusDirection::Output,
                        pulp::format::BusRole::Aux) != nullptr);

    pulp::state::ParameterEventQueue automation;
    REQUIRE(automation.push({HostMatrixProcessor::kGainParam, 4, 0.75f, 8}));
    automation.sort();

    pulp::midi::MidiBuffer midi_in;
    auto note = pulp::midi::MidiEvent::note_on(0, 60, 100);
    note.sample_offset = 6;
    midi_in.add(note);
    midi_in.add_sysex({0xF0, 0x7D, 0x01, 0xF7}, 12, 0.25);

    pulp::midi::UmpBuffer ump;
    ump.add(pulp::midi::UmpPacket::note_on_2(0, 0, 64, 0xFFFF), 16);
    midi_in.attach_ump(&ump);

    pulp::midi::MidiBuffer midi_out;
    pulp::format::EventBlock events;
    events.parameter_events = &automation;
    events.midi_in = &midi_in;
    events.midi_out = &midi_out;
    events.ump_input = &ump;
    events.drops.graph_events = 1;

    pulp::format::ProcessContext transport;
    transport.sample_rate = 48000.0;
    transport.num_samples = 64;
    transport.is_playing = true;
    transport.is_looping = true;
    transport.loop_start_beats = 8.0;
    transport.loop_end_beats = 12.0;
    transport.position_samples = 1024;
    transport.position_beats = 9.5;

    std::array<std::byte, 512> scratch_memory{};
    pulp::format::BlockScratch block_scratch{{scratch_memory.data(), scratch_memory.size()}};

    pulp::format::ProcessBlock block;
    block.mode = pulp::format::ProcessMode::Realtime;
    // ProcessBlock-native paths can inspect these flags directly. The legacy
    // Processor adapter does not pass them through its ABI, so this fixture
    // separately verifies the conventional bypass parameter state below.
    block.flags.bypass = true;
    block.flags.tail_drain = true;
    block.sample_rate = 48000.0;
    block.frame_count = 64;
    block.render_speed = 1.0;
    block.transport = &transport;
    block.buses = &buses;
    block.events = &events;
    block.scratch = &block_scratch;

    REQUIRE(block.validate());
    REQUIRE(block.flags.bypass);
    REQUIRE(block.flags.tail_drain);
    REQUIRE(block.events->parameter_event_count() == 1);
    REQUIRE(block.events->midi_input_event_count() == 1);
    REQUIRE(block.events->sysex_event_count() == 1);
    REQUIRE(block.events->ump_input == &ump);
    REQUIRE(block.events->drops.any());

    pulp::format::ProcessorBlockAdapterScratch adapter_scratch;
    REQUIRE(pulp::format::process_processor_block(processor, block, adapter_scratch));

    REQUIRE(processor.process_calls == 1);
    REQUIRE(processor.observed_input_channels == 2);
    REQUIRE(processor.observed_output_channels == 2);
    REQUIRE(processor.observed_sidechain_channels == 1);
    REQUIRE(processor.observed_param_events == 1);
    REQUIRE(processor.observed_midi_events == 1);
    REQUIRE(processor.observed_sysex_events == 1);
    REQUIRE(processor.observed_ump_events == 1);
    REQUIRE_THAT(processor.observed_bypass_value, WithinAbs(1.0f, 0.000001f));
    REQUIRE(processor.observed_context.is_looping);
    REQUIRE(processor.observed_context.position_samples == 1024);
    REQUIRE_THAT(output.channel(0)[0], WithinAbs(2.0f, 0.000001f));
    REQUIRE_THAT(output.channel(1)[0], WithinAbs(200.0f, 0.000001f));
    for (std::size_t ch = 0; ch < aux_output.num_channels(); ++ch) {
        for (const auto sample : aux_output.channel(ch)) {
            REQUIRE_THAT(sample, WithinAbs(-123.0f, 0.000001f));
        }
    }
}

TEST_CASE("Host runtime matrix pins latency tail and state restore hooks",
          "[format][host-matrix][state][latency][tail]") {
    pulp::state::StateStore source_store;
    HostMatrixProcessor source;
    source.set_state_store(&source_store);
    source.define_parameters(source_store);
    source.latency_samples_value = 128;
    source.tail_samples_value = 4096;
    source.plugin_state = "waveform=zoomed";
    source_store.set_value(HostMatrixProcessor::kGainParam, 1.5f);
    source_store.set_value(HostMatrixProcessor::kBypassParam, 1.0f);

    REQUIRE(source.latency_samples() == 128);
    REQUIRE(source.descriptor().tail_samples == 4096);

    source.flag_latency_changed();
    source.flag_tail_changed();
    REQUIRE(source.latency_change_pending());
    REQUIRE(source.tail_change_pending());
    REQUIRE(source.consume_latency_changed_flag());
    REQUIRE(source.consume_tail_changed_flag());
    REQUIRE_FALSE(source.consume_latency_changed_flag());
    REQUIRE_FALSE(source.consume_tail_changed_flag());

    pulp::format::HostQuirks quirks;
    REQUIRE(pulp::format::reported_latency_samples(-32, quirks) == 0);
    quirks.clamp_latency_to_nonneg = false;
    REQUIRE(pulp::format::reported_latency_samples(-32, quirks) == -32);

    const auto blob = pulp::format::plugin_state_io::serialize(source_store, source);

    pulp::state::StateStore restored_store;
    HostMatrixProcessor restored;
    restored.set_state_store(&restored_store);
    restored.define_parameters(restored_store);
    restored.plugin_state = "stale";
    restored_store.set_value(HostMatrixProcessor::kGainParam, 0.25f);

    REQUIRE(pulp::format::plugin_state_io::deserialize(blob, restored_store, restored));
    REQUIRE(restored.deserialize_calls == 1);
    REQUIRE(restored.plugin_state == "waveform=zoomed");
    REQUIRE_THAT(restored_store.get_value(HostMatrixProcessor::kGainParam),
                 WithinAbs(1.5f, 0.000001f));
    REQUIRE_THAT(restored_store.get_value(HostMatrixProcessor::kBypassParam),
                 WithinAbs(1.0f, 0.000001f));
}

TEST_CASE("Host runtime matrix covers offline bounce transport automation and MIDI",
          "[format][host-matrix][offline-render]") {
    HostMatrixProcessor::last = nullptr;
    pulp::format::OfflineRenderHost host(create_host_matrix_processor);
    pulp::format::OfflineRenderConfig config;
    config.sample_rate = 48000.0;
    config.max_block_frames = 8;
    config.input_channels = 2;
    config.output_channels = 2;
    config.tempo_bpm = 120.0;
    REQUIRE(host.prepare(config));

    auto* processor = HostMatrixProcessor::last;
    REQUIRE(processor != nullptr);
    REQUIRE(processor->prepare_calls == 1);
    REQUIRE(processor->max_buffer_size == 8);
    REQUIRE(processor->prepared_input_channels == 2);
    REQUIRE(processor->prepared_output_channels == 2);
    host.headless().state().set_value(HostMatrixProcessor::kGainParam, 0.5f);

    pulp::audio::Buffer<float> input(2, 12);
    for (std::size_t i = 0; i < input.num_samples(); ++i) {
        input.channel(0)[i] = static_cast<float>(i + 1);
        input.channel(1)[i] = static_cast<float>(20 + i);
    }
    std::array<const float*, 2> input_ptrs = {input.channel(0).data(), input.channel(1).data()};

    std::array<pulp::format::OfflineMidiEvent, 2> midi_events = {{
        {0, pulp::midi::MidiEvent::note_on(0, 60, 100)},
        {9, pulp::midi::MidiEvent::note_on(0, 64, 100)},
    }};
    std::array<pulp::format::OfflineParameterEvent, 2> automation = {{
        {0, {HostMatrixProcessor::kGainParam, 0, 0.5f, 0}},
        {9, {HostMatrixProcessor::kGainParam, 0, 0.75f, 0}},
    }};

    pulp::format::OfflineRenderOptions options;
    options.frame_count = 12;
    options.block_frames = 8;
    options.input = const_view(input, input_ptrs);
    options.midi_events = midi_events;
    options.parameter_events = automation;
    options.start_position_samples = 256;
    options.start_position_beats = 4.0;
    options.is_looping = true;
    options.loop_start_beats = 4.0;
    options.loop_end_beats = 8.0;

    const auto result = host.render(options);

    REQUIRE(result.ok);
    REQUIRE(result.stats.frames_rendered == 12);
    REQUIRE(result.stats.blocks_rendered == 2);
    REQUIRE(result.stats.midi_events_dispatched == 2);
    REQUIRE(result.stats.parameter_events_dispatched == 2);
    REQUIRE(processor->process_calls == 2);
    REQUIRE(processor->total_midi_events == 2);
    REQUIRE(processor->total_param_events == 2);
    REQUIRE(processor->block_sizes[0] == 8);
    REQUIRE(processor->block_sizes[1] == 4);
    REQUIRE(processor->sample_positions[0] == 256);
    REQUIRE(processor->sample_positions[1] == 264);
    REQUIRE(processor->param_events_per_block[0] == 1);
    REQUIRE(processor->param_events_per_block[1] == 1);
    REQUIRE(processor->first_param_ids[0] == HostMatrixProcessor::kGainParam);
    REQUIRE(processor->first_param_ids[1] == HostMatrixProcessor::kGainParam);
    REQUIRE(processor->first_param_offsets[0] == 0);
    REQUIRE(processor->first_param_offsets[1] == 1);
    REQUIRE_THAT(processor->first_param_values[0], WithinAbs(0.5f, 0.000001f));
    REQUIRE_THAT(processor->first_param_values[1], WithinAbs(0.75f, 0.000001f));
    REQUIRE(processor->midi_events_per_block[0] == 1);
    REQUIRE(processor->midi_events_per_block[1] == 1);
    REQUIRE(processor->first_midi_offsets[0] == 0);
    REQUIRE(processor->first_midi_offsets[1] == 1);
    REQUIRE(processor->first_midi_notes[0] == 60);
    REQUIRE(processor->first_midi_notes[1] == 64);
    REQUIRE(processor->observed_context.is_looping);
    // OfflineRenderHost slices automation sidecars; applying those events to
    // StateStore remains adapter/processor policy, so audio stays at gain 0.5.
    REQUIRE_THAT(result.audio.channel(0)[0], WithinAbs(0.5f, 0.000001f));
    REQUIRE_THAT(result.audio.channel(0)[11], WithinAbs(6.0f, 0.000001f));
    REQUIRE_THAT(result.audio.channel(1)[0], WithinAbs(10.0f, 0.000001f));
}
