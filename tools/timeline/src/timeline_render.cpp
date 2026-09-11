#include <pulp/tools/timeline/agent.hpp>

#include "timeline_agent_internal.hpp"

#include <pulp/audio/audio_file.hpp>
#include <pulp/host/signal_graph_runtime.hpp>
#include <pulp/host/timeline_graph_binding.hpp>
#include <pulp/host/timeline_offline_graph.hpp>
#include <pulp/host/timeline_offline_renderer.hpp>
#include <pulp/timeline/serialize.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <vector>

namespace pulp::tools::timeline {
namespace {

// The offline renderer bounds a render by a half-open tick region. The verb's
// budget is a frame count, so it is carried to the smallest tick that covers the
// whole budget: rounding up can only ever lengthen the region, and truncating a
// bounce is the failure this must not produce.
std::optional<timebase::TickPosition> covering_tick(const timebase::CompiledTempoMap& tempo_map,
                                                    std::uint64_t frames) {
    constexpr auto ceiling = static_cast<long double>(std::numeric_limits<std::int64_t>::max());
    if (static_cast<long double>(frames) >= ceiling)
        return std::nullopt;
    const auto ticks = tempo_map.fractional_samples_to_ticks(static_cast<long double>(frames));
    if (!std::isfinite(ticks))
        return std::nullopt;
    const auto rounded = std::ceil(ticks);
    if (rounded < 0.0L || rounded >= ceiling)
        return std::nullopt;
    return timebase::TickPosition{static_cast<std::int64_t>(rounded)};
}

} // namespace

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

    constexpr std::uint32_t block_size = 512;
    host::SignalGraph graph;
    host::TimelineGraphPlaybackBinding binding(graph, compiled.value()->store);
    const auto topology = host::build_device_free_timeline_graph(graph, *program, channels);
    if (!topology)
        return detail::failure("render", "render graph topology error " +
                                             std::to_string(static_cast<unsigned>(topology.code)));

    pulp::audio::AudioFileData rendered;
    rendered.sample_rate = sample_rate;
    if (topology.routes.empty()) {
        // A program with no tracks renders silence. The offline renderer refuses
        // an empty route span as an under-specified request, so the graph stays
        // out of the way and the zero-filled buffer is written as it always was.
        try {
            rendered.channels.assign(channels,
                                     std::vector<float>(static_cast<std::size_t>(frames), 0.0f));
        } catch (const std::bad_alloc&) {
            return detail::failure("render", "could not allocate the in-memory render buffer");
        } catch (const std::length_error&) {
            return detail::failure("render", "could not allocate the in-memory render buffer");
        }
    } else {
        const auto end_tick = covering_tick(*compiled.value()->tempo_map, frames);
        if (!end_tick)
            return detail::failure("render", "sequence duration is empty or too large");

        host::TimelineOfflineRenderConfig config;
        config.sample_rate = static_cast<double>(sample_rate);
        config.block_frames = static_cast<int>(block_size);
        config.output_channels = static_cast<int>(channels);
        config.max_output_frames = detail::kMaxRenderPcmBytes / bytes_per_frame;
        // The binding's own note capacity, not the renderer's smaller default, so
        // a dense arrangement keeps the admission headroom this verb always had.
        config.maximum_note_events_per_track_per_block =
            host::TimelineGraphBindingConfig{}.maximum_note_events_per_track_per_block;

        host::TimelineOfflineRenderOptions options;
        options.start_tick = timebase::TickPosition{0};
        options.end_tick = *end_tick;
        // The verb exposes no tail, so the output ends exactly at the sequence
        // end. A ringing delay or reverb is cut there, as it has always been.
        options.tail_frames = 0;

        host::TimelineOfflineRenderResult result;
        try {
            result = host::render_timeline_offline(graph, binding, *program, topology.routes,
                                                   config, options);
        } catch (const std::bad_alloc&) {
            return detail::failure("render", "could not allocate the in-memory render buffer");
        } catch (const std::length_error&) {
            return detail::failure("render", "could not allocate the in-memory render buffer");
        }
        if (!result)
            return detail::failure("render", detail::offline_render_message(result));
        rendered = std::move(result.audio);
    }

    const auto rendered_frames =
        rendered.channels.empty() ? std::size_t{0} : rendered.channels.front().size();
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
                   std::to_string(rendered_frames) +
                   "\",\"ok\":true,\"output\":" +
                   pulp::timeline::quote_json_string(output_utf8) +
                   ",\"sample_rate\":" + std::to_string(sample_rate) + "}"};
}

} // namespace pulp::tools::timeline
