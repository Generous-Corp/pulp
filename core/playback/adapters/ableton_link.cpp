#include <pulp/playback/ableton_link.hpp>

#include <ableton/Link.hpp>

#include <chrono>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace pulp::playback {

struct AbletonLinkTempoSync::Impl {
    explicit Impl(double initial_tempo_bpm) : link(initial_tempo_bpm) {}

    ableton::Link link;
};

AbletonLinkTempoSync::AbletonLinkTempoSync(double initial_tempo_bpm) {
    if (!std::isfinite(initial_tempo_bpm) || initial_tempo_bpm <= 0.0)
        throw std::invalid_argument("Ableton Link initial tempo must be finite and positive");
    impl_ = std::make_unique<Impl>(initial_tempo_bpm);
}

AbletonLinkTempoSync::~AbletonLinkTempoSync() = default;

void AbletonLinkTempoSync::set_enabled(bool enabled) {
    impl_->link.enable(enabled);
}

bool AbletonLinkTempoSync::enabled() const {
    return impl_->link.isEnabled();
}

void AbletonLinkTempoSync::set_start_stop_sync_enabled(bool enabled) {
    impl_->link.enableStartStopSync(enabled);
}

bool AbletonLinkTempoSync::start_stop_sync_enabled() const {
    return impl_->link.isStartStopSyncEnabled();
}

std::size_t AbletonLinkTempoSync::peer_count() const {
    return impl_->link.numPeers();
}

TempoSyncError AbletonLinkTempoSync::capture_audio_block(const TempoSyncBlockRequest& request,
                                                         TempoSyncBlockState& state) noexcept {
    state = {};
    if (!valid_tempo_sync_request(request))
        return TempoSyncError::InvalidRequest;

    try {
        if (!enabled())
            return TempoSyncError::Disabled;

        using Micros = std::chrono::microseconds;
        const Micros start{request.output_host_time_micros};
        const auto duration = static_cast<std::int64_t>(
            std::llround(static_cast<long double>(request.frame_count) * 1'000'000.0L /
                         static_cast<long double>(request.sample_rate)));
        const Micros end{request.output_host_time_micros + duration};

        auto session = impl_->link.captureAudioSessionState();
        const auto& command = request.command;
        if (command.request_tempo)
            session.setTempo(command.tempo_bpm, start);

        if (command.request_playing && command.playing && command.request_beat) {
            session.setIsPlayingAndRequestBeatAtTime(command.playing, start, command.beat,
                                                     request.quantum_beats);
        } else {
            if (command.request_playing)
                session.setIsPlaying(command.playing, start);
            if (command.request_beat)
                session.requestBeatAtTime(command.beat, start, request.quantum_beats);
        }

        if (command.request_tempo || command.request_playing || command.request_beat)
            impl_->link.commitAudioSessionState(session);

        state.tempo_bpm = session.tempo();
        state.beat_start = session.beatAtTime(start, request.quantum_beats);
        state.beat_end = session.beatAtTime(end, request.quantum_beats);
        state.is_playing = session.isPlaying();
        return valid_tempo_sync_state(state) ? TempoSyncError::None : TempoSyncError::InvalidState;
    } catch (...) {
        state = {};
        return TempoSyncError::BackendFailure;
    }
}

} // namespace pulp::playback
