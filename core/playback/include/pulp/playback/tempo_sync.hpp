#pragma once

#include <pulp/audio/rt_safety_contract.hpp>

#include <cstdint>

namespace pulp::playback {

/// Backend-independent command bundle applied at the first output sample of an
/// audio block. A false request flag means the corresponding value is ignored.
struct TempoSyncCommand {
    double tempo_bpm = 120.0;
    double beat = 0.0;
    bool request_tempo = false;
    bool request_beat = false;
    bool request_playing = false;
    bool playing = false;
};

/// Timing information for one audio callback. output_host_time_micros is the
/// host-clock time at which the first sample reaches the output boundary; the
/// caller is responsible for adding device/output latency.
struct TempoSyncBlockRequest {
    std::int64_t output_host_time_micros = 0;
    std::uint32_t frame_count = 0;
    double sample_rate = 0.0;
    double quantum_beats = 4.0;
    TempoSyncCommand command{};
};

/// Captured session mapping at both boundaries of one audio block. Beat values
/// are deliberately backend-neutral doubles because network tempo sources own
/// their clock mapping; MasterTransport converts them to Pulp's precise tick
/// range while retaining the fractional endpoints.
struct TempoSyncBlockState {
    double tempo_bpm = 120.0;
    double beat_start = 0.0;
    double beat_end = 0.0;
    /// Host-clock time at which is_playing becomes effective. This preserves
    /// backends whose start/stop state changes at a time other than the block
    /// boundary instead of flattening that transition into a block-wide bool.
    std::int64_t is_playing_at_host_time_micros = 0;
    bool is_playing = false;
};

enum class TempoSyncPlayingBoundary : std::uint8_t {
    BeforeOrAtBlockStart,
    InsideBlock,
    AtOrAfterBlockEnd,
};

/// Block-coherent projection of a timestamped start/stop state. Pulp's
/// TransportSnapshot has one playing state for the whole half-open block, so
/// transitions after its first sample are deferred to a later block rather
/// than exposed as a false sample-accurate transition.
struct TempoSyncPlayingProjection {
    TempoSyncPlayingBoundary boundary = TempoSyncPlayingBoundary::BeforeOrAtBlockStart;
    bool playing_for_block = false;
    bool transition_deferred = false;
};

enum class TempoSyncError : std::uint8_t {
    None,
    Disabled,
    InvalidRequest,
    InvalidState,
    BackendFailure,
};

bool valid_tempo_sync_request(const TempoSyncBlockRequest& request) noexcept;
bool valid_tempo_sync_state(const TempoSyncBlockState& state) noexcept;
bool tempo_sync_block_end_host_time_micros(const TempoSyncBlockRequest& request,
                                           std::int64_t& block_end) noexcept;
TempoSyncPlayingProjection project_tempo_sync_playing(const TempoSyncBlockRequest& request,
                                                      const TempoSyncBlockState& state,
                                                      bool previous_playing) noexcept;

/// Backend-neutral session-tempo boundary. capture_audio_block() is the sole
/// operation because enablement, peer discovery, and start/stop-sync policy are
/// backend controls rather than transport concerns. Implementations must not
/// allocate, lock, or retain the returned block state.
class TempoSyncSource {
  public:
    static constexpr audio::RtSafetyClass capture_audio_block_rt_safety_class =
        audio::RtSafetyClass::AudioCallbackSafeAfterPrepare;

    virtual ~TempoSyncSource() = default;

    virtual TempoSyncError capture_audio_block(const TempoSyncBlockRequest& request,
                                               TempoSyncBlockState& state) noexcept = 0;
};

} // namespace pulp::playback
