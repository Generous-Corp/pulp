#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/format/offline_render_host.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace {

class OfflineProbeProcessor : public pulp::format::Processor {
public:
    OfflineProbeProcessor() { last = this; }

    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "OfflineProbe",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.offline-probe",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
        };
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({
            .id = 1,
            .name = "Gain",
            .unit = "x",
            .range = {0.0f, 4.0f, 1.0f, 0.01f},
        });
    }

    void prepare(const pulp::format::PrepareContext& context) override {
        ++prepare_calls;
        last_prepare_context = context;
    }

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input,
                 pulp::midi::MidiBuffer& midi_in,
                 pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext& context) override {
        const auto slot = static_cast<std::size_t>(std::min(process_calls,
            static_cast<int>(block_sizes.size() - 1)));
        block_sizes[slot] = context.num_samples;
        sample_positions[slot] = context.position_samples;
        beat_positions[slot] = context.position_beats;
        bars[slot] = context.bar;
        transport_validity[slot] = context.transport_validity;
        midi_counts[slot] = static_cast<int>(midi_in.size());
        first_midi_offsets[slot] = midi_in.empty() ? -1 : midi_in[0].sample_offset;
        last_midi_offsets[slot] = midi_in.empty() ? -1 : midi_in[midi_in.size() - 1].sample_offset;
        first_midi_notes[slot] = midi_in.empty() ? -1 : midi_in[0].note();
        second_midi_notes[slot] = midi_in.size() < 2 ? -1 : midi_in[1].note();
        last_midi_notes[slot] = midi_in.empty() ? -1 : midi_in[midi_in.size() - 1].note();

        const auto* events = param_events();
        param_counts[slot] = events ? static_cast<int>(events->size()) : -1;
        if (events && !events->empty()) {
            first_param_offsets[slot] = events->begin()->sample_offset;
            first_param_values[slot] = events->begin()->value;
        } else {
            first_param_offsets[slot] = -1;
            first_param_values[slot] = 0.0f;
        }

        const float gain = state().get_value(1);
        for (std::size_t ch = 0; ch < output.num_channels(); ++ch) {
            auto out = output.channel(ch);
            if (ch >= input.num_channels()) {
                std::fill(out.begin(), out.end(), 0.0f);
                continue;
            }
            auto in = input.channel(ch);
            for (std::size_t i = 0; i < output.num_samples(); ++i) {
                out[i] = in[i] * gain;
            }
        }

        ++process_calls;
    }

    static OfflineProbeProcessor* last;

    pulp::format::PrepareContext last_prepare_context{};
    int prepare_calls = 0;
    int process_calls = 0;
    std::array<int, 16> block_sizes{};
    std::array<std::int64_t, 16> sample_positions{};
    std::array<double, 16> beat_positions{};
    std::array<std::int64_t, 16> bars{};
    std::array<pulp::format::TransportValidity, 16> transport_validity{};
    std::array<int, 16> midi_counts{};
    std::array<int, 16> first_midi_offsets{};
    std::array<int, 16> last_midi_offsets{};
    std::array<int, 16> first_midi_notes{};
    std::array<int, 16> second_midi_notes{};
    std::array<int, 16> last_midi_notes{};
    std::array<int, 16> param_counts{};
    std::array<int, 16> first_param_offsets{};
    std::array<float, 16> first_param_values{};
};

OfflineProbeProcessor* OfflineProbeProcessor::last = nullptr;

std::unique_ptr<pulp::format::Processor> create_offline_probe() {
    return std::make_unique<OfflineProbeProcessor>();
}

std::unique_ptr<pulp::format::Processor> create_null_probe() {
    return {};
}

pulp::audio::BufferView<const float> const_view(const pulp::audio::Buffer<float>& buffer,
                                                std::span<const float* const> ptrs) {
    return {ptrs.data(), buffer.num_channels(), buffer.num_samples()};
}

} // namespace

using Catch::Matchers::WithinAbs;

TEST_CASE("OfflineRenderHost prepares and steps deterministic blocks",
          "[format][offline-render]") {
    pulp::format::OfflineRenderHost host(create_offline_probe);
    pulp::format::OfflineRenderConfig config;
    config.sample_rate = 48000.0;
    config.max_block_frames = 32;
    config.input_channels = 2;
    config.output_channels = 2;
    config.tempo_bpm = 120.0;

    REQUIRE(host.prepare(config));
    auto* processor = OfflineProbeProcessor::last;
    REQUIRE(processor != nullptr);
    REQUIRE(processor->prepare_calls == 1);
    REQUIRE(processor->last_prepare_context.max_buffer_size == 32);
    REQUIRE(processor->last_prepare_context.input_channels == 2);
    REQUIRE(processor->last_prepare_context.output_channels == 2);

    pulp::format::OfflineRenderOptions options;
    options.frame_count = 100;
    options.block_frames = 32;
    options.start_position_samples = 256;
    options.start_position_beats = 4.0;

    auto result = host.render(options);

    REQUIRE(result.ok);
    REQUIRE(result.stats.frames_rendered == 100);
    REQUIRE(result.stats.blocks_rendered == 4);
    REQUIRE(processor->process_calls == 4);
    REQUIRE(processor->block_sizes[0] == 32);
    REQUIRE(processor->block_sizes[1] == 32);
    REQUIRE(processor->block_sizes[2] == 32);
    REQUIRE(processor->block_sizes[3] == 4);
    REQUIRE(processor->sample_positions[0] == 256);
    REQUIRE(processor->sample_positions[1] == 288);
    REQUIRE(processor->sample_positions[3] == 352);
    REQUIRE_THAT(processor->beat_positions[1],
                 WithinAbs(4.0 + (32.0 / 48000.0) * 2.0, 0.000001));
    REQUIRE(processor->transport_validity[0].has(
        pulp::format::TransportField::Playing));
    REQUIRE(processor->transport_validity[0].has(
        pulp::format::TransportField::Tempo));
    REQUIRE(processor->transport_validity[0].has(
        pulp::format::TransportField::BeatPosition));
    REQUIRE(processor->transport_validity[0].has(
        pulp::format::TransportField::SamplePosition));
    REQUIRE(processor->transport_validity[0].has(
        pulp::format::TransportField::TimeSignature));
    REQUIRE(processor->transport_validity[0].has(
        pulp::format::TransportField::Bar));
    REQUIRE(result.audio.num_channels() == 2);
    REQUIRE(result.audio.num_samples() == 100);
}

TEST_CASE("OfflineRenderHost supports zero-input instruments",
          "[format][offline-render][instrument]") {
    pulp::format::OfflineRenderHost host(create_offline_probe);
    pulp::format::OfflineRenderConfig config;
    config.max_block_frames = 16;
    config.input_channels = 0;
    config.output_channels = 2;

    REQUIRE(host.prepare(config));
    auto* processor = OfflineProbeProcessor::last;
    REQUIRE(processor != nullptr);
    REQUIRE(processor->last_prepare_context.input_channels == 0);
    REQUIRE(processor->last_prepare_context.output_channels == 2);

    pulp::format::OfflineRenderOptions options;
    options.frame_count = 32;
    options.block_frames = 16;
    auto result = host.render(options);

    REQUIRE(result.ok);
    REQUIRE(result.stats.frames_rendered == 32);
    REQUIRE(result.audio.num_channels() == 2);
    REQUIRE(result.audio.num_samples() == 32);
}

TEST_CASE("OfflineRenderHost copies input and returns rendered audio",
          "[format][offline-render]") {
    pulp::format::OfflineRenderHost host(create_offline_probe);
    pulp::format::OfflineRenderConfig config;
    config.max_block_frames = 4;
    REQUIRE(host.prepare(config));
    host.headless().state().set_value(1, 2.0f);

    pulp::audio::Buffer<float> input(2, 10);
    for (std::size_t i = 0; i < input.num_samples(); ++i) {
        input.channel(0)[i] = static_cast<float>(i + 1);
        input.channel(1)[i] = static_cast<float>(10 + i);
    }
    std::array<const float*, 2> ptrs = {input.channel(0).data(), input.channel(1).data()};

    pulp::format::OfflineRenderOptions options;
    options.frame_count = 10;
    options.block_frames = 4;
    options.input = const_view(input, ptrs);

    auto result = host.render(options);

    REQUIRE(result.ok);
    REQUIRE_FALSE(result.stats.input_truncated);
    REQUIRE_THAT(result.audio.channel(0)[0], WithinAbs(2.0, 0.000001));
    REQUIRE_THAT(result.audio.channel(0)[9], WithinAbs(20.0, 0.000001));
    REQUIRE_THAT(result.audio.channel(1)[0], WithinAbs(20.0, 0.000001));
    REQUIRE_THAT(result.audio.channel(1)[9], WithinAbs(38.0, 0.000001));
}

TEST_CASE("OfflineRenderHost pads short input with silence",
          "[format][offline-render]") {
    pulp::format::OfflineRenderHost host(create_offline_probe);
    pulp::format::OfflineRenderConfig config;
    config.max_block_frames = 4;
    config.input_channels = 2;
    config.output_channels = 2;
    REQUIRE(host.prepare(config));

    pulp::audio::Buffer<float> input(1, 5);
    for (std::size_t i = 0; i < input.num_samples(); ++i) {
        input.channel(0)[i] = static_cast<float>(i + 1);
    }
    std::array<const float*, 1> ptrs = {input.channel(0).data()};

    pulp::format::OfflineRenderOptions options;
    options.frame_count = 8;
    options.block_frames = 4;
    options.input = const_view(input, ptrs);

    auto result = host.render(options);

    REQUIRE(result.ok);
    REQUIRE(result.stats.input_truncated);
    REQUIRE_THAT(result.audio.channel(0)[4], WithinAbs(5.0, 0.000001));
    REQUIRE_THAT(result.audio.channel(0)[5], WithinAbs(0.0, 0.000001));
    REQUIRE_THAT(result.audio.channel(1)[0], WithinAbs(0.0, 0.000001));
    REQUIRE_THAT(result.audio.channel(1)[7], WithinAbs(0.0, 0.000001));
}

TEST_CASE("OfflineRenderHost slices MIDI and parameter events by absolute frame",
          "[format][offline-render][midi][params]") {
    pulp::format::OfflineRenderHost host(create_offline_probe);
    pulp::format::OfflineRenderConfig config;
    config.max_block_frames = 4;
    REQUIRE(host.prepare(config));
    auto* processor = OfflineProbeProcessor::last;
    REQUIRE(processor != nullptr);

    std::vector<pulp::format::OfflineMidiEvent> midi_events;
    const std::array<std::pair<std::uint64_t, std::uint8_t>, 6> midi_specs = {{
        {0, 60}, {3, 61}, {4, 62}, {4, 63}, {7, 64}, {8, 65},
    }};
    for (const auto [frame, note] : midi_specs) {
        auto event = pulp::midi::MidiEvent::note_on(0, note, 100);
        event.sample_offset = 99;
        midi_events.push_back({frame, event});
    }

    std::vector<pulp::format::OfflineParameterEvent> parameter_events = {
        {1, {1, 99, 0.1f, 0}},
        {5, {1, 99, 0.5f, 0}},
        {8, {1, 99, 0.8f, 0}},
    };

    pulp::format::OfflineRenderOptions options;
    options.frame_count = 9;
    options.block_frames = 4;
    options.midi_events = std::span<const pulp::format::OfflineMidiEvent>(
        midi_events.data(), midi_events.size());
    options.parameter_events = std::span<const pulp::format::OfflineParameterEvent>(
        parameter_events.data(), parameter_events.size());

    auto result = host.render(options);

    REQUIRE(result.ok);
    REQUIRE(result.stats.blocks_rendered == 3);
    REQUIRE(result.stats.midi_events_dispatched == 6);
    REQUIRE(result.stats.parameter_events_dispatched == 3);
    REQUIRE(processor->midi_counts[0] == 2);
    REQUIRE(processor->first_midi_offsets[0] == 0);
    REQUIRE(processor->last_midi_offsets[0] == 3);
    REQUIRE(processor->first_midi_notes[0] == 60);
    REQUIRE(processor->second_midi_notes[0] == 61);
    REQUIRE(processor->midi_counts[1] == 3);
    REQUIRE(processor->first_midi_offsets[1] == 0);
    REQUIRE(processor->second_midi_notes[1] == 63);
    REQUIRE(processor->last_midi_offsets[1] == 3);
    REQUIRE(processor->first_midi_notes[1] == 62);
    REQUIRE(processor->last_midi_notes[1] == 64);
    REQUIRE(processor->midi_counts[2] == 1);
    REQUIRE(processor->first_midi_offsets[2] == 0);
    REQUIRE(processor->first_midi_notes[2] == 65);
    REQUIRE(processor->param_counts[0] == 1);
    REQUIRE(processor->first_param_offsets[0] == 1);
    REQUIRE_THAT(processor->first_param_values[0], WithinAbs(0.1, 0.000001));
    REQUIRE(processor->param_counts[1] == 1);
    REQUIRE(processor->first_param_offsets[1] == 1);
    REQUIRE(processor->param_counts[2] == 1);
    REQUIRE(processor->first_param_offsets[2] == 0);
    REQUIRE(processor->param_events() == nullptr);
}

TEST_CASE("OfflineRenderHost rejects invalid requests without partial render",
          "[format][offline-render]") {
    pulp::format::OfflineRenderHost host(create_offline_probe);
    pulp::format::OfflineRenderConfig invalid_config;
    invalid_config.sample_rate = 0.0;
    REQUIRE_FALSE(host.prepare(invalid_config));

    pulp::format::OfflineRenderConfig config;
    config.max_block_frames = 8;
    REQUIRE(host.prepare(config));

    pulp::format::OfflineRenderHost null_host(create_null_probe);
    REQUIRE_FALSE(null_host.prepare(config));

    pulp::format::OfflineRenderOptions bad_tempo;
    bad_tempo.frame_count = 8;
    bad_tempo.tempo_bpm = std::numeric_limits<double>::infinity();
    auto bad_tempo_result = host.render(bad_tempo);
    REQUIRE_FALSE(bad_tempo_result.ok);
    REQUIRE(bad_tempo_result.stats.blocks_rendered == 0);

    pulp::format::OfflineRenderOptions too_large_block;
    too_large_block.frame_count = 16;
    too_large_block.block_frames = 16;
    auto too_large_result = host.render(too_large_block);
    REQUIRE_FALSE(too_large_result.ok);
    REQUIRE(too_large_result.stats.blocks_rendered == 0);

    std::vector<pulp::format::OfflineMidiEvent> unsorted_midi = {
        {3, pulp::midi::MidiEvent::note_on(0, 60, 100)},
        {2, pulp::midi::MidiEvent::note_on(0, 61, 100)},
    };
    pulp::format::OfflineRenderOptions unsorted_options;
    unsorted_options.frame_count = 8;
    unsorted_options.midi_events = std::span<const pulp::format::OfflineMidiEvent>(
        unsorted_midi.data(), unsorted_midi.size());

    auto unsorted_result = host.render(unsorted_options);
    REQUIRE_FALSE(unsorted_result.ok);
    REQUIRE(unsorted_result.stats.event_order_invalid);
    REQUIRE(unsorted_result.stats.blocks_rendered == 0);

    std::vector<pulp::format::OfflineParameterEvent> too_many_parameter_events;
    too_many_parameter_events.reserve(pulp::state::ParameterEventQueue::kCapacity + 1);
    for (std::size_t i = 0; i <= pulp::state::ParameterEventQueue::kCapacity; ++i) {
        too_many_parameter_events.push_back({0, {1, 0, 0.5f, 0}});
    }
    pulp::format::OfflineRenderOptions overflow_options;
    overflow_options.frame_count = 1;
    overflow_options.parameter_events = std::span<const pulp::format::OfflineParameterEvent>(
        too_many_parameter_events.data(), too_many_parameter_events.size());

    auto overflow_result = host.render(overflow_options);
    REQUIRE_FALSE(overflow_result.ok);
    REQUIRE(overflow_result.stats.blocks_rendered == 1);
    REQUIRE(overflow_result.stats.parameter_events_dispatched ==
            pulp::state::ParameterEventQueue::kCapacity);
    REQUIRE(overflow_result.stats.parameter_events_dropped == 1);
}
