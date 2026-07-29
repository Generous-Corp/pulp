#pragma once

#include <pulp/audio/rt_safety_contract.hpp>
#include <pulp/playback/tempo_sync.hpp>
#include <pulp/runtime/seqlock.hpp>
#include <pulp/timebase/compiled_meter_map.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>

#include <array>
#include <cstdint>
#include <type_traits>

namespace pulp::playback {

enum class TransportError {
    None,
    NotPrepared,
    InvalidMeter,
    InvalidLoop,
    LoopTooShortForMaximumBlock,
    InvalidFrameCount,
    InvalidScrubWindow,
    ScrubWindowTooShortForMaximumBlock,
    NotScrubbing,
    InvalidTempo,
    InvalidTempoSyncConfig,
    TempoSyncHostTimeRequired,
    TempoSyncUnavailable,
    InvalidTempoSyncState,
};

using MeterSignature = timebase::MeterSignature;

struct LoopRegion {
    bool enabled = false;
    timebase::TickPosition start{};
    timebase::TickPosition end{};
    constexpr auto operator<=>(const LoopRegion&) const = default;
};

struct TransportRange {
    std::uint32_t sample_offset = 0;
    std::uint32_t frame_count = 0;
    timebase::SamplePosition timeline_sample_start{};
    timebase::TickPosition timeline_tick_start{};
    timebase::TickPosition timeline_tick_end{};
    timebase::MonotonicBeat monotonic_start{};
    timebase::MonotonicBeat monotonic_end{};
    timebase::BarPosition bar_start{};
    double tempo_bpm = 120.0;
    bool tempo_changed = false;
    bool discontinuity = false;
    bool host_beat_mapping = false;
    double host_tick_start = 0.0;
    double host_tick_end = 0.0;
    bool has_precise_host_ticks = false;
    /// Zero-based loop pass within the transport's current playback epoch.
    /// Transport producers own this state so newly attached renderers and
    /// renderers that skip a callback observe the same pass.
    std::uint64_t loop_pass_index = 0;
};

struct TransportSnapshot {
    /// Non-owning identity of the exact compiled map used to resolve ranges.
    /// The map must outlive consumers of this callback snapshot.
    const timebase::CompiledTempoMap* tempo_map = nullptr;
    timebase::RationalRate sample_rate{};
    std::uint64_t block_index = 0;
    std::uint32_t frame_count = 0;
    MeterSignature meter{};
    LoopRegion loop{};
    bool is_playing = false;
    /// True while the transport is rendering scrub windows. is_playing is also
    /// true in that case: audio advances even when the musical transport is
    /// stopped, and consumers that only care whether the playhead moves need no
    /// scrub-specific branch.
    bool scrubbing = false;
    bool transport_changed = false;
    bool transport_started = false;
    bool reset_requested = false;
    bool time_sig_changed = false;
    /// Convenience mirror of ranges[0].tempo_bpm for block-level consumers.
    double tempo_bpm = 120.0;
    std::array<TransportRange, 2> ranges{};
    std::uint8_t range_count = 0;
    double host_loop_start_beats = 0.0;
    double host_loop_end_beats = 0.0;
    bool has_precise_host_loop = false;
};

/// Validates the shared structural contract consumed by every block renderer.
bool valid_transport_ranges(const TransportSnapshot& transport) noexcept;

/// Maps an output-frame boundary in a host-beat-mapped range back into the
/// program's document-sample domain. The returned value is fractional so audio
/// renderers can interpolate without discarding host-tempo scaling.
long double
host_mapped_document_sample_at_output_offset(const TransportRange& range,
                                             const timebase::CompiledTempoMap& tempo_map,
                                             std::uint32_t output_offset) noexcept;

/// Maps an exact program event tick into a half-open host-beat-mapped transport
/// range. Callers must retain the source tick rather than round-tripping an
/// integer document sample through the tempo map at range boundaries.
bool host_mapped_output_offset_for_tick(const TransportRange& range,
                                        timebase::TickPosition document_tick,
                                        std::uint32_t& output_offset) noexcept;

struct MasterTransportConfig {
    std::uint32_t max_buffer_size = 0;
    MeterSignature meter{};
    LoopRegion loop{};
    timebase::TickPosition initial_position{};
    bool initially_playing = false;
    /// Optional non-owning session-tempo source. It must outlive this transport
    /// or the next reset()/prepare(). When present, callers use the host-time
    /// begin_block overload; failure never falls back to the document clock.
    TempoSyncSource* tempo_sync_source = nullptr;
    double tempo_sync_quantum_beats = 4.0;
};

/// Master musical transport with one control-thread writer and one audio-thread
/// consumer. Control methods publish one coherent desired-state snapshot through
/// SeqLock. begin_block() is allocation-free and never locks.
class MasterTransport {
  public:
    static constexpr audio::RtSafetyClass begin_block_rt_safety_class =
        audio::RtSafetyClass::AudioCallbackSafeAfterPrepare;

    MasterTransport() = default;

    /// tempo_map must outlive this transport or the next reset()/prepare().
    TransportError prepare(const timebase::CompiledTempoMap& tempo_map,
                           const MasterTransportConfig& config) noexcept;
    TransportError set_playing(bool playing) noexcept;
    /// Publishes an explicit session-tempo request. This never edits the
    /// document TempoMap; it is sent to the configured TempoSyncSource at the
    /// next audio block.
    TransportError set_tempo_sync_tempo(double tempo_bpm) noexcept;
    TransportError seek(timebase::TickPosition position) noexcept;
    TransportError set_loop(LoopRegion loop) noexcept;
    TransportError set_meter(MeterSignature meter) noexcept;

    /// Enters scrub mode: the transport renders repeated `window_frames`-long
    /// windows that each start at the latest posted anchor, which is how a
    /// dragged playhead becomes audible. The anchor is latched, not immediate —
    /// a new position takes effect at the next window boundary, so the grain
    /// rate is the window length rather than the UI event rate. The window must
    /// be at least one maximum block so a block spans at most two windows,
    /// preserving the two-range snapshot contract.
    ///
    /// Scrubbing suspends loop wrapping. A drag is a direct statement of
    /// position, so the transport must not pull the audible window back to the
    /// loop start or make positions outside the loop unreachable; the loop is
    /// still reported in the snapshot so a UI keeps drawing it, and wrapping
    /// resumes at end_scrub().
    ///
    /// Starting a drag is itself immediate even mid-window: it abandons any
    /// window in flight so a fresh drag's window length takes effect at once.
    TransportError begin_scrub(std::uint32_t window_frames,
                               timebase::TickPosition position) noexcept;
    /// Moves the scrub anchor. Fails with NotScrubbing outside scrub mode.
    TransportError scrub_to(timebase::TickPosition position) noexcept;
    /// Leaves scrub mode and parks the playhead on the last posted anchor —
    /// where the drag was released — never mid-window.
    TransportError end_scrub() noexcept;

    TransportError begin_block(std::uint32_t frame_count, TransportSnapshot& snapshot) noexcept;
    /// output_host_time_micros names the first sample's output-boundary time in
    /// the TempoSyncSource clock domain. Required only when a source is
    /// configured; the ordinary document-clock path remains unchanged.
    TransportError begin_block(std::uint32_t frame_count, std::int64_t output_host_time_micros,
                               TransportSnapshot& snapshot) noexcept;
    void reset() noexcept;

  private:
    struct DesiredState {
        MeterSignature meter{};
        LoopRegion loop{};
        timebase::TickPosition position{};
        timebase::TickPosition scrub_position{};
        std::uint32_t scrub_window_frames = 0;
        bool playing = false;
        bool scrubbing = false;
        std::uint64_t seek_generation = 0;
        std::uint64_t scrub_generation = 0;
        std::uint64_t playing_generation = 0;
        std::uint64_t tempo_sync_tempo_generation = 0;
        double tempo_sync_tempo_bpm = 120.0;
    };

    static_assert(std::is_trivially_copyable_v<DesiredState>);

    TransportError validate_loop(LoopRegion loop) const noexcept;
    void publish_desired() noexcept;
    TransportError begin_internal_block(std::uint32_t frame_count,
                                        TransportSnapshot& snapshot) noexcept;
    TransportError begin_tempo_synced_block(std::uint32_t frame_count,
                                            std::int64_t output_host_time_micros,
                                            TransportSnapshot& snapshot) noexcept;

    runtime::SeqLock<DesiredState> desired_{};
    DesiredState control_state_{};
    const timebase::CompiledTempoMap* tempo_map_ = nullptr;
    timebase::TempoCursor tempo_cursor_{};
    TempoSyncSource* tempo_sync_source_ = nullptr;
    double tempo_sync_quantum_beats_ = 4.0;
    std::uint32_t max_buffer_size_ = 0;

    timebase::SamplePosition timeline_sample_{};
    timebase::TickPosition timeline_tick_{};
    timebase::MonotonicBeat monotonic_{};
    timebase::TickPosition meter_anchor_tick_{};
    timebase::BarPosition meter_anchor_bar_{};
    MeterSignature meter_anchor_signature_{};
    std::uint64_t applied_seek_generation_ = 0;
    std::uint64_t applied_scrub_generation_ = 0;
    std::uint64_t applied_tempo_sync_playing_generation_ = 0;
    std::uint64_t applied_tempo_sync_seek_generation_ = 0;
    std::uint64_t applied_tempo_sync_tempo_generation_ = 0;
    std::uint64_t block_index_ = 0;
    std::uint64_t loop_pass_index_ = 0;
    std::uint32_t scrub_window_remaining_ = 0;
    bool previous_scrubbing_ = false;
    bool previous_playing_ = false;
    MeterSignature previous_meter_{};
    LoopRegion previous_loop_{};
    double previous_tempo_bpm_ = 120.0;
    bool first_block_ = true;
    bool pending_discontinuity_ = false;
    bool has_expected_tempo_sync_beat_ = false;
    double expected_tempo_sync_beat_ = 0.0;
};

} // namespace pulp::playback
