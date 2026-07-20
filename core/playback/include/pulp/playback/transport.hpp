#pragma once

#include <pulp/audio/rt_safety_contract.hpp>
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
};

using MeterSignature = timebase::MeterSignature;

struct LoopRegion {
    bool enabled = false;
    timebase::TickPosition start{};
    timebase::TickPosition end{};
    constexpr auto operator<=>(const LoopRegion&) const = default;
};

enum class TransportDiscontinuityReason : std::uint8_t {
    None,
    Seek,
    LoopWrap,
    LoopConfiguration,
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
    TransportDiscontinuityReason discontinuity_reason =
        TransportDiscontinuityReason::None;
};

struct TransportSnapshot {
    /// Non-owning identity of the exact compiled map used to resolve ranges.
    /// The map must outlive consumers of this callback snapshot.
    const timebase::CompiledTempoMap* tempo_map = nullptr;
    timebase::RationalRate sample_rate{};
    std::uint64_t block_index = 0;
    std::uint32_t frame_count = 0;
    MeterSignature meter{};
    timebase::TickPosition meter_anchor_tick{};
    timebase::BarPosition meter_anchor_bar{};
    LoopRegion loop{};
    bool is_playing = false;
    bool transport_changed = false;
    bool transport_started = false;
    bool reset_requested = false;
    bool time_sig_changed = false;
    /// Convenience mirror of ranges[0].tempo_bpm for block-level consumers.
    double tempo_bpm = 120.0;
    std::array<TransportRange, 2> ranges{};
    std::uint8_t range_count = 0;
};

/// Validates the shared structural contract consumed by every block renderer.
bool valid_transport_ranges(const TransportSnapshot& transport) noexcept;

struct MasterTransportConfig {
    std::uint32_t max_buffer_size = 0;
    MeterSignature meter{};
    LoopRegion loop{};
    timebase::TickPosition initial_position{};
    bool initially_playing = false;
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
    TransportError seek(timebase::TickPosition position) noexcept;
    TransportError set_loop(LoopRegion loop) noexcept;
    TransportError set_meter(MeterSignature meter) noexcept;

    TransportError begin_block(std::uint32_t frame_count, TransportSnapshot& snapshot) noexcept;
    void reset() noexcept;

  private:
    struct DesiredState {
        MeterSignature meter{};
        LoopRegion loop{};
        timebase::TickPosition position{};
        bool playing = false;
        std::uint64_t seek_generation = 0;
    };

    static_assert(std::is_trivially_copyable_v<DesiredState>);

    TransportError validate_loop(LoopRegion loop) const noexcept;
    void publish_desired() noexcept;

    runtime::SeqLock<DesiredState> desired_{};
    DesiredState control_state_{};
    const timebase::CompiledTempoMap* tempo_map_ = nullptr;
    timebase::TempoCursor tempo_cursor_{};
    std::uint32_t max_buffer_size_ = 0;

    timebase::SamplePosition timeline_sample_{};
    timebase::TickPosition timeline_tick_{};
    timebase::MonotonicBeat monotonic_{};
    timebase::TickPosition meter_anchor_tick_{};
    timebase::BarPosition meter_anchor_bar_{};
    MeterSignature meter_anchor_signature_{};
    std::uint64_t applied_seek_generation_ = 0;
    std::uint64_t block_index_ = 0;
    bool previous_playing_ = false;
    MeterSignature previous_meter_{};
    LoopRegion previous_loop_{};
    double previous_tempo_bpm_ = 120.0;
    bool first_block_ = true;
    TransportDiscontinuityReason pending_discontinuity_ =
        TransportDiscontinuityReason::None;
};

} // namespace pulp::playback
