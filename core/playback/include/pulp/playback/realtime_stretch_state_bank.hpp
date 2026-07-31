#pragma once

#include <pulp/playback/audio_renderer_limits.hpp>
#include <pulp/signal/realtime_pitch_time_processor.hpp>
#include <pulp/timeline/model.hpp>

#include <compare>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pulp::playback {

class PlaybackProgram;

struct RealtimeStretchStateSpec {
    timeline::ItemId clip_id;
    std::uint32_t channels = 0;
    signal::PitchTimeQuality quality = signal::PitchTimeQuality::low_latency;
    float max_time_ratio = 1.0f;
    constexpr auto operator<=>(const RealtimeStretchStateSpec&) const = default;
};

enum class RealtimeStretchStateBankError : std::uint8_t {
    None,
    InvalidConfiguration,
    InvalidIdentity,
    DuplicateIdentity,
    StateLimitExceeded,
    ChannelLimitExceeded,
    TimeRatioLimitExceeded,
    StateBytesExceeded,
    ProcessorPrepareRejected,
    AllocationFailed,
};

struct RealtimeStretchStateBankAdmission {
    RealtimeStretchStateBankError code = RealtimeStretchStateBankError::None;
    timeline::ItemId clip_id;
    std::uint64_t actual = 0;
    std::uint64_t limit = 0;
    std::uint64_t reserved_state_bytes = 0;
    signal::PitchTimePrepareStatus processor_status = signal::PitchTimePrepareStatus::prepared;

    constexpr explicit operator bool() const noexcept {
        return code == RealtimeStretchStateBankError::None;
    }
};

/// Pure, allocation-free admission for one immutable bank shape. The byte
/// calculation checked-sums each processor geometry's conservative retained
/// charge. The separate per-allocation ceiling remains an addressability bound.
RealtimeStretchStateBankAdmission
admit_realtime_stretch_state_bank(std::span<const RealtimeStretchStateSpec> specs,
                                  double sample_rate, std::uint32_t maximum_block_frames,
                                  const AudioRendererLimits& limits) noexcept;

/// Control-thread aggregate admission for the exact immutable publication.
/// Per-track banks partition this already-admitted set so parallel graph nodes
/// retain single-owner DSP state without multiplying the global ceilings.
RealtimeStretchStateBankAdmission admit_realtime_stretch_program(const PlaybackProgram& program,
                                                                 double sample_rate,
                                                                 std::uint32_t maximum_block_frames,
                                                                 const AudioRendererLimits& limits);

/// Control-thread-prepared bank of clip-keyed realtime stretch processors.
/// prepare() is transactional: it prepares a complete candidate and swaps it
/// into service only after every state succeeds. state_for_epoch() and reset()
/// are allocation-free and intended for the audio thread.
///
/// With exceptions enabled, allocation failures preserve the live bank and
/// return AllocationFailed. In no-exception builds the standard containers'
/// OOM behavior is process termination; prepare cannot report allocation
/// failure because the allocator provides no recoverable failure channel.
class RealtimeStretchStateBank {
  public:
    RealtimeStretchStateBank();
    ~RealtimeStretchStateBank();
    RealtimeStretchStateBank(RealtimeStretchStateBank&&) noexcept;
    RealtimeStretchStateBank& operator=(RealtimeStretchStateBank&&) noexcept;
    RealtimeStretchStateBank(const RealtimeStretchStateBank&) = delete;
    RealtimeStretchStateBank& operator=(const RealtimeStretchStateBank&) = delete;

    RealtimeStretchStateBankAdmission prepare(std::span<const RealtimeStretchStateSpec> specs,
                                              double sample_rate,
                                              std::uint32_t maximum_block_frames,
                                              const AudioRendererLimits& limits);

    signal::RealtimePitchTimeProcessor* state_for_epoch(timeline::ItemId clip_id,
                                                        std::uint64_t playback_epoch) noexcept;
    const signal::RealtimePitchTimeProcessor* find(timeline::ItemId clip_id) const noexcept;
    void reset() noexcept;

    std::size_t size() const noexcept {
        return states_.size();
    }
    std::uint64_t reserved_state_bytes() const noexcept {
        return reserved_state_bytes_;
    }

  private:
    struct State;
    std::vector<std::unique_ptr<State>> states_;
    std::uint64_t reserved_state_bytes_ = 0;
};

} // namespace pulp::playback
