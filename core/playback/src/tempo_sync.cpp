#include <pulp/playback/tempo_sync.hpp>

#include <cmath>
#include <limits>

namespace pulp::playback {

bool valid_tempo_sync_request(const TempoSyncBlockRequest& request) noexcept {
    if (request.frame_count == 0 || !std::isfinite(request.sample_rate) ||
        request.sample_rate <= 0.0 || !std::isfinite(request.quantum_beats) ||
        request.quantum_beats <= 0.0)
        return false;

    const auto duration_micros =
        std::ceil(static_cast<long double>(request.frame_count) * 1'000'000.0L /
                  static_cast<long double>(request.sample_rate));
    if (!std::isfinite(duration_micros) || duration_micros < 0.0L ||
        duration_micros > static_cast<long double>(std::numeric_limits<std::int64_t>::max()) ||
        static_cast<long double>(request.output_host_time_micros) + duration_micros >
            static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
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

} // namespace pulp::playback
