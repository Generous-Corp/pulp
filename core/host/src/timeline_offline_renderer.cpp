#include <pulp/host/timeline_offline_renderer.hpp>

#include <pulp/host/signal_graph.hpp>

#include <algorithm>
#include <limits>
#include <vector>

namespace pulp::host {
namespace {

using Code = TimelineOfflineRenderCode;

TimelineOfflineRenderResult fail(Code code) {
    TimelineOfflineRenderResult result;
    result.code = code;
    return result;
}

/// Derives a sample position from ticks, refusing a saturated answer.
///
/// CompiledTempoMap::ticks_to_samples() saturates rather than trapping when the
/// mathematical result leaves range. Saturation is the right behaviour for a
/// real-time reader that must not branch, but for a bounce it would silently
/// turn an absurd region into a plausible one, so this treats a saturated or
/// negative result as a hard rejection.
bool sample_for_tick(const timebase::CompiledTempoMap& map, timebase::TickPosition tick,
                     std::int64_t& out) {
    if (tick.value < 0)
        return false;
    const auto sample = map.ticks_to_samples(tick);
    if (sample.value < 0 || sample.value == std::numeric_limits<std::int64_t>::max())
        return false;
    out = sample.value;
    return true;
}

} // namespace

TimelineOfflineRenderResult render_timeline_offline(
    SignalGraph& graph, TimelineGraphPlaybackBinding& binding,
    const playback::PlaybackProgram& program,
    std::span<const TimelineTrackGraphRoute> routes,
    const TimelineOfflineRenderConfig& config,
    const TimelineOfflineRenderOptions& options) {

    // ---- Limits and program validity, before any allocation or graph work ----
    if (config.block_frames <= 0 || config.output_channels <= 0 ||
        config.max_output_frames == 0 || config.sample_rate <= 0.0)
        return fail(Code::InvalidLimits);
    if (program.tracks().empty() || routes.empty())
        return fail(Code::InvalidProgram);

    const auto& tempo_map = program.tempo_map();
    // The region is derived from the program's own map, so a request carrying a
    // different rate is a caller bug rather than something to reconcile.
    if (tempo_map.sample_rate().normalized() !=
        timebase::RationalRate{static_cast<std::uint64_t>(config.sample_rate), 1}.normalized())
        return fail(Code::SampleRateMismatch);

    if (options.end_tick.value <= options.start_tick.value)
        return fail(Code::InvalidRange);

    std::int64_t start_sample = 0;
    std::int64_t end_sample = 0;
    if (!sample_for_tick(tempo_map, options.start_tick, start_sample) ||
        !sample_for_tick(tempo_map, options.end_tick, end_sample))
        return fail(Code::InvalidRange);
    if (end_sample <= start_sample)
        return fail(Code::InvalidRange);

    const auto region_frames = static_cast<std::uint64_t>(end_sample - start_sample);
    const auto total_frames = region_frames + options.tail_frames;
    if (total_frames < region_frames) // overflow
        return fail(Code::InvalidRange);
    if (total_frames > config.max_output_frames)
        return fail(Code::InvalidLimits);

    // ---- Bind ----
    TimelineGraphBindingConfig binding_config;
    binding_config.audio_channels = static_cast<std::uint32_t>(config.output_channels);
    binding_config.maximum_note_events_per_track_per_block =
        config.maximum_note_events_per_track_per_block;
    binding_config.audio_limits.max_channels = static_cast<std::uint32_t>(config.output_channels);
    binding_config.audio_limits.max_block_frames = static_cast<std::uint32_t>(config.block_frames);

    if (auto admission = binding.prepare(program, routes, binding_config, config.sample_rate,
                                         config.block_frames);
        !admission) {
        auto result = fail(Code::BindingRejected);
        result.admission = admission;
        return result;
    }

    // ---- Transport ----
    playback::MasterTransport transport;
    playback::MasterTransportConfig transport_config;
    transport_config.max_buffer_size = static_cast<std::uint32_t>(config.block_frames);
    transport_config.initially_playing = true;
    if (auto error = transport.prepare(tempo_map, transport_config);
        error != playback::TransportError::None) {
        auto result = fail(Code::TransportRejected);
        result.transport_error = error;
        return result;
    }

    // Always start from tick origin. A region that begins mid-timeline is
    // reached by pre-rolling and discarding, never by seeking straight to it:
    // seeking would hand stateful nodes an empty delay line and a cold PDC
    // pipeline, and the region would differ from the same slice of a full bounce.
    if (auto error = transport.seek(timebase::TickPosition{0});
        error != playback::TransportError::None) {
        auto result = fail(Code::TransportRejected);
        result.transport_error = error;
        return result;
    }

    // ---- Buffers ----
    const auto channels = static_cast<std::size_t>(config.output_channels);
    const auto block = static_cast<std::size_t>(config.block_frames);
    std::vector<std::vector<float>> block_storage(channels, std::vector<float>(block, 0.0f));
    std::vector<float*> block_ptrs(channels);
    for (std::size_t c = 0; c < channels; ++c)
        block_ptrs[c] = block_storage[c].data();
    std::vector<const float*> input_ptrs(channels, nullptr);
    std::vector<float> silence(block, 0.0f);
    for (std::size_t c = 0; c < channels; ++c)
        input_ptrs[c] = silence.data();

    TimelineOfflineRenderResult result;
    result.audio.sample_rate = static_cast<std::uint32_t>(config.sample_rate);
    result.audio.channels.assign(channels, std::vector<float>());
    for (auto& channel : result.audio.channels)
        channel.reserve(static_cast<std::size_t>(total_frames));

    playback::TransportSnapshot snapshot;
    // MasterTransport exposes no public playing() query, so the renderer tracks
    // its own stop rather than inferring one.
    bool transport_stopped = false;
    std::int64_t rendered = 0;              // absolute frames advanced from origin
    const std::int64_t stop_at = end_sample; // transport stops exactly here

    // ---- Block loop: pre-roll, region, then the fixed pad ----
    while (rendered < stop_at || result.audio.channels[0].size() < total_frames) {
        const bool in_pad = rendered >= stop_at;
        if (in_pad && options.tail_frames == 0)
            break;

        std::uint32_t frames = static_cast<std::uint32_t>(block);
        if (!in_pad)
            frames = static_cast<std::uint32_t>(
                std::min<std::int64_t>(static_cast<std::int64_t>(block), stop_at - rendered));
        else
            frames = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                block, total_frames - result.audio.channels[0].size()));
        if (frames == 0)
            break;

        // Stop the transport exactly at the region end, before the first pad
        // block. A pre-end note may ring out through the pad; a note authored
        // after the end must never enter it.
        if (in_pad && !transport_stopped) {
            if (auto error = transport.set_playing(false);
                error != playback::TransportError::None) {
                auto failure = fail(Code::TransportRejected);
                failure.transport_error = error;
                return failure;
            }
            transport_stopped = true;
        }

        if (auto error = transport.begin_block(frames, snapshot);
            error != playback::TransportError::None) {
            auto failure = fail(Code::TransportRejected);
            failure.transport_error = error;
            return failure;
        }

        for (std::size_t c = 0; c < channels; ++c)
            std::fill_n(block_storage[c].data(), block, 0.0f);

        audio::BufferView<float> output(block_ptrs.data(), channels, frames);
        audio::BufferView<const float> input(input_ptrs.data(), channels, frames);
        auto processed = binding.process(output, input, snapshot);
        if (!processed) {
            auto failure = fail(Code::ProcessFailed);
            failure.process = processed;
            return failure;
        }

        const bool keep = in_pad || rendered + static_cast<std::int64_t>(frames) > start_sample;
        if (keep) {
            // Discard the part of this block that precedes the region start, so
            // a non-block-aligned start is exact rather than rounded outward.
            const std::size_t offset =
                in_pad ? 0
                       : static_cast<std::size_t>(std::max<std::int64_t>(0, start_sample - rendered));
            for (std::size_t c = 0; c < channels; ++c) {
                const float* src = block_storage[c].data() + offset;
                const std::size_t count = frames - offset;
                auto& dst = result.audio.channels[c];
                const std::size_t room =
                    static_cast<std::size_t>(total_frames) - dst.size();
                dst.insert(dst.end(), src, src + std::min(count, room));
            }
        }

        rendered += static_cast<std::int64_t>(frames);
        if (result.audio.channels[0].size() >= total_frames)
            break;
    }

    result.code = Code::Ok;
    result.region_frames = region_frames;
    result.tail_frames = options.tail_frames;
    return result;
}

} // namespace pulp::host
