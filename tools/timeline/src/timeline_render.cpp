#include <pulp/tools/timeline/agent.hpp>

#include "timeline_agent_internal.hpp"

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/host/signal_graph_runtime.hpp>
#include <pulp/host/timeline_graph_binding.hpp>
#include <pulp/host/timeline_offline_graph.hpp>
#include <pulp/playback/audio_renderer.hpp>
#include <pulp/playback/transport.hpp>
#include <pulp/timeline/serialize.hpp>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace pulp::tools::timeline {

OperationResult render(const ProjectSource& project, const std::filesystem::path& output,
                       std::uint32_t sample_rate) {
    if (sample_rate == 0 || sample_rate > timebase::kMaximumCompiledSampleRate || output.empty())
        return detail::failure("render", "output and sample_rate between 1 and 768000 are required",
                               {}, 2);
    std::string output_utf8;
    try {
        output_utf8 = filesystem_path_to_utf8(output);
    } catch (...) {
        return detail::failure("render", "could not encode output path");
    }
    auto registry = pulp::timeline::make_builtin_timeline_registry();
    if (!registry)
        return detail::failure("registry", "could not construct the built-in schema registry");
    auto loaded = detail::load_project(project, registry.value());
    if (!loaded)
        return detail::failure("open", detail::persistence_message(loaded.error()),
                               loaded.error().path);
    auto compiled = detail::compile_project(loaded.value(), sample_rate);
    if (!compiled)
        return detail::failure("render", detail::compile_error_message(compiled.error()));
    const auto* sequence =
        loaded.value().value.find_sequence(loaded.value().value.root_sequence_id());
    if (!sequence)
        return detail::failure("render", "root sequence is missing");
    auto program = compiled.value()->store.read();
    if (!program)
        return detail::failure("render", "compiled program was not published");
    const auto frames =
        detail::render_frame_count(*sequence, *compiled.value()->tempo_map, *program, sample_rate);
    if (frames == 0 || frames > std::numeric_limits<std::size_t>::max())
        return detail::failure("render", "sequence duration is empty or too large");

    std::uint32_t channels = 1;
    if (const auto& assets = program->audio_assets_owner())
        for (const auto& asset : assets->assets())
            channels = std::max(channels, asset.audio->num_channels());
    const auto bytes_per_frame = static_cast<std::uint64_t>(channels) * sizeof(float);
    if (frames > detail::kMaxRenderPcmBytes / bytes_per_frame)
        return detail::failure("render", "sequence exceeds the in-memory render budget");

    pulp::audio::AudioFileData rendered;
    rendered.sample_rate = sample_rate;
    try {
        rendered.channels.reserve(channels);
        for (std::uint32_t channel = 0; channel < channels; ++channel)
            rendered.channels.emplace_back(static_cast<std::size_t>(frames));
    } catch (const std::bad_alloc&) {
        return detail::failure("render", "could not allocate the in-memory render buffer");
    } catch (const std::length_error&) {
        return detail::failure("render", "could not allocate the in-memory render buffer");
    }

    constexpr std::uint32_t block_size = 512;
    host::SignalGraph graph;
    host::TimelineGraphPlaybackBinding binding(graph, compiled.value()->store);
    const auto topology = host::build_device_free_timeline_graph(graph, *program, channels);
    if (!topology)
        return detail::failure("render", "render graph topology error " +
                                             std::to_string(static_cast<unsigned>(topology.code)));
    // A program with no tracks renders silence. Preparing an empty binding would
    // be rejected as an under-specified request, so the graph stays out of the
    // way and the zero-filled buffer is written as it always was.
    const bool routed = !topology.routes.empty();
    std::vector<float> silence;
    std::vector<const float*> silence_data;
    if (routed) {
        host::TimelineGraphBindingConfig binding_config;
        binding_config.audio_channels = channels;
        const auto admission =
            binding.prepare(*program, topology.routes, binding_config,
                            static_cast<double>(sample_rate), static_cast<int>(block_size));
        if (!admission)
            return detail::failure("render",
                                   "render graph admission error " +
                                       std::to_string(static_cast<unsigned>(admission.code)));
        try {
            silence.assign(static_cast<std::size_t>(channels) * block_size, 0.0f);
            silence_data.reserve(channels);
        } catch (const std::bad_alloc&) {
            return detail::failure("render", "could not allocate the render graph input buffer");
        }
        for (std::uint32_t channel = 0; channel < channels; ++channel)
            silence_data.push_back(silence.data() + static_cast<std::size_t>(channel) * block_size);
    }

    playback::MasterTransport transport;
    if (transport.prepare(*compiled.value()->tempo_map,
                          {.max_buffer_size = block_size, .initially_playing = true}) !=
        playback::TransportError::None)
        return detail::failure("render", "transport preparation failed");
    std::uint64_t offset = 0;
    std::vector<float*> channel_data;
    channel_data.reserve(channels);
    while (offset < frames) {
        const auto count =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(block_size, frames - offset));
        playback::TransportSnapshot snapshot;
        if (transport.begin_block(count, snapshot) != playback::TransportError::None)
            return detail::failure("render", "transport block failed");
        channel_data.clear();
        for (auto& channel : rendered.channels)
            channel_data.push_back(channel.data() + offset);
        pulp::audio::BufferView<float> block(channel_data.data(), channels, count);
        auto block_program = compiled.value()->store.read();
        if (!block_program)
            return detail::failure("render", "compiled program disappeared");
        if (routed) {
            const pulp::audio::BufferView<const float> input(silence_data.data(), channels, count);
            const auto result = binding.process(block, input, snapshot);
            if (!result)
                return detail::failure("render",
                                       "render graph process error " +
                                           std::to_string(static_cast<unsigned>(result.code)));
        }
        offset += count;
    }
    const auto written = detail::write_wav_atomic(output, rendered);
    if (written == detail::AtomicWriteOutcome::NotReplaced)
        return detail::failure("render", "could not write output WAV", output_utf8);
    if (written == detail::AtomicWriteOutcome::ReplacedButDirectorySyncFailed)
        return detail::failure(
            "render",
            "output WAV was replaced, but its parent directory could not be synchronized; "
            "durability is uncertain",
            output_utf8);
    return {0, "{\"channels\":" + std::to_string(channels) + ",\"frames\":\"" +
                   std::to_string(frames) +
                   "\",\"ok\":true,\"output\":" +
                   pulp::timeline::quote_json_string(output_utf8) +
                   ",\"sample_rate\":" + std::to_string(sample_rate) + "}"};
}

} // namespace pulp::tools::timeline
