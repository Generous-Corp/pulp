#include <pulp/playback/tempo_sync.hpp>

#include <cmath>
#include <limits>

namespace pulp::playback {

bool tempo_sync_block_end_host_time_micros(const TempoSyncBlockRequest& request,
                                           std::int64_t& block_end) noexcept {
    if (request.frame_count == 0 || !std::isfinite(request.sample_rate) ||
        request.sample_rate <= 0.0)
        return false;

    const auto duration_micros = static_cast<long double>(request.frame_count) * 1'000'000.0L /
                                 static_cast<long double>(request.sample_rate);
    if (!std::isfinite(duration_micros) || duration_micros < 0.0L ||
        duration_micros > static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
        return false;

    const auto rounded_duration = std::round(duration_micros);
    const auto maximum_duration_exclusive =
        -static_cast<long double>(std::numeric_limits<std::int64_t>::min());
    if (!(rounded_duration < maximum_duration_exclusive))
        return false;
    const auto duration = static_cast<std::int64_t>(rounded_duration);
    if (request.output_host_time_micros >
        std::numeric_limits<std::int64_t>::max() - duration)
        return false;
    block_end = request.output_host_time_micros + duration;
    return true;
}

bool valid_tempo_sync_request(const TempoSyncBlockRequest& request) noexcept {
    if (request.frame_count == 0 || !std::isfinite(request.sample_rate) ||
        request.sample_rate <= 0.0 || !std::isfinite(request.quantum_beats) ||
        request.quantum_beats <= 0.0)
        return false;

    std::int64_t block_end = 0;
    if (!tempo_sync_block_end_host_time_micros(request, block_end))
        return false;

    if (request.command.request_tempo &&
        (!std::isfinite(request.command.tempo_bpm) || request.command.tempo_bpm <= 0.0))
        return false;
    return !request.command.request_beat || std::isfinite(request.command.beat);
}

bool valid_tempo_sync_state(const TempoSyncBlockState& state) noexcept {
    return std::isfinite(state.tempo_bpm) && state.tempo_bpm > 0.0 &&
           std::isfinite(state.beat_start) && std::isfinite(state.beat_end) &&
           (state.is_playing ? state.beat_end > state.beat_start
                             : state.beat_end >= state.beat_start);
}

TempoSyncPlayingProjection project_tempo_sync_playing(const TempoSyncBlockRequest& request,
                                                      const TempoSyncBlockState& state,
                                                      bool previous_playing) noexcept {
    TempoSyncPlayingProjection result;
    const auto transition_time = state.is_playing_at_host_time_micros;
    if (transition_time <= request.output_host_time_micros) {
        result.boundary = TempoSyncPlayingBoundary::BeforeOrAtBlockStart;
        result.playing_for_block = state.is_playing;
        return result;
    }

    std::int64_t block_end = 0;
    if (!tempo_sync_block_end_host_time_micros(request, block_end)) {
        result.boundary = TempoSyncPlayingBoundary::AtOrAfterBlockEnd;
        result.playing_for_block = previous_playing;
        result.transition_deferred = state.is_playing != previous_playing;
        return result;
    }
    result.boundary = transition_time < block_end ? TempoSyncPlayingBoundary::InsideBlock
                                                  : TempoSyncPlayingBoundary::AtOrAfterBlockEnd;
    result.playing_for_block = previous_playing;
    result.transition_deferred = state.is_playing != previous_playing;
    return result;
}

} // namespace pulp::playback
