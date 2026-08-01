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
    PlaybackEpochExhausted,
};

namespace detail {
/// Advances an epoch without ever wrapping to an aliased identity.
TransportError advance_playback_epoch(std::uint64_t& epoch) noexcept;
}

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
    /// Identity of the continuous playback interval containing this range.
    /// A block may contain two epochs only when its second range applies a
    /// newly latched scrub anchor. Ordinary loop wraps retain the epoch.
    std::uint64_t playback_epoch = 0;
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
    /// Epoch of ranges[0]. Later ranges carry their own epoch so a scrub-anchor
    /// discontinuity may be represented precisely when it splits a block.
    std::uint64_t playback_epoch = 0;
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

/// Where the transport is, published for a reader that is not the audio thread.
///
/// This is the counterpart to TransportSnapshot, and the difference is the
/// lifetime of what it carries. A snapshot borrows `const CompiledTempoMap*`
/// from the compiled program, which is correct for a block renderer that
/// consumes it inside the callback that produced it, and unsafe for a view that
/// keeps a copy across frames while the engine may adopt a different program
/// underneath. Every field here is a value, so a retained reading goes stale,
/// never invalid.
///
/// The vocabulary is the transport's own. Projecting a reading into the UI
/// vocabulary a timeline editor speaks is the job of whoever implements that
/// seam, because playback's dependency floor excludes the editor rung.
struct TransportPlayhead {
    /// Monotonic per publish. Two readings with the same value are the same
    /// reading, which is how a reader skips redundant work. Zero means nothing
    /// has been published yet. reset() deliberately does not restart it: a
    /// restarted counter would make a new reading indistinguishable from one a
    /// reader already acted on.
    std::uint64_t sequence = 0;
    /// Identity of the continuous playback interval this reading belongs to.
    /// A change means continuity broke, so a reader interpolating between two
    /// readings must not interpolate across one.
    std::uint64_t playback_epoch = 0;
    /// Position of the FIRST frame of the published block. That frame has not
    /// left the device yet, so it is the least-ahead-of-audible position the
    /// transport can state; publishing the block's end would put every reading
    /// one buffer into the future.
    timebase::TickPosition position{};
    LoopRegion loop{};
    double tempo_bpm = 120.0;
    /// True whenever the position advances on its own. Mirrors
    /// TransportSnapshot::is_playing, so it is also true while scrubbing and a
    /// reader asking only "does the playhead move" needs no scrub branch.
    bool is_playing = false;
    bool scrubbing = false;

    constexpr bool operator==(const TransportPlayhead&) const = default;
};

static_assert(std::is_trivially_copyable_v<TransportPlayhead>);

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
///
/// Position travels in both directions, through one SeqLock each. The control
/// thread states where playback should go; the audio thread publishes where it
/// actually is, so a view can read the playhead without touching a callback.
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
    /// output_host_time names the first sample's output-boundary time and must
    /// have been produced by the configured TempoSyncSource. Required only when
    /// a source is configured; the ordinary document-clock path is unchanged.
    TransportError begin_block(std::uint32_t frame_count, TempoSyncHostTime output_host_time,
                               TransportSnapshot& snapshot) noexcept;

    /// The latest published playhead reading, by value.
    ///
    /// Call it from any thread that is not the audio thread, as often as a
    /// frame needs it: it allocates nothing, takes no lock, and returns a
    /// coherent reading rather than one torn across a concurrent publish — at
    /// worst it retries while a publish is in flight. It returns the newest
    /// reading, never a backlog: a reader that skipped frames sees where
    /// playback is now, not where it was.
    TransportPlayhead playhead() const noexcept { return playhead_.read(); }

    void reset() noexcept;

  private:
    struct BlockProjection;
    struct RangeProjection;
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
    void begin_projected_block(const DesiredState& desired, const BlockProjection& projection,
                               timebase::TickPosition anchor_tick,
                               TransportSnapshot& snapshot) noexcept;
    void append_projected_range(const RangeProjection& projection,
                                TransportSnapshot& snapshot) noexcept;
    void finish_projected_block(const DesiredState& desired, const BlockProjection& projection,
                                TransportSnapshot& snapshot) noexcept;
    /// Stamps a reading with the next sequence number and publishes it. Every
    /// publication goes through here, so no site can skip the stamp and leave a
    /// new reading indistinguishable from the one before it.
    void publish_playhead(TransportPlayhead reading) noexcept;

    runtime::SeqLock<DesiredState> desired_{};
    runtime::SeqLock<TransportPlayhead> playhead_{};
    std::uint64_t playhead_sequence_ = 0;
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
    std::uint64_t playback_epoch_ = 0;
    bool playback_epoch_exhausted_ = false;
    std::uint64_t loop_pass_index_ = 0;
    std::uint32_t scrub_window_remaining_ = 0;
    bool previous_scrubbing_ = false;
    bool has_applied_scrub_position_ = false;
    timebase::TickPosition applied_scrub_position_{};
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
