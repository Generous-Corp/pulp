#pragma once

#include <pulp/format/process_context.hpp>
#include <pulp/playback/transport.hpp>

#include <cstdint>

namespace pulp::sequence {

enum class HostTransportProjectionError : std::uint8_t {
    None,
    NotPrepared,
    InvalidFrameCount,
    InvalidSampleRate,
    InvalidMeter,
    InvalidLoop,
    LoopTooShortForBlock,
};

/// Re-anchors the engine transport to the host's integer sample position on
/// every callback. A loop crossing is represented by the same two-range
/// snapshot contract as MasterTransport; loops shorter than one callback fail
/// closed because they would require more than two ranges.
class HostTransportProjector {
  public:
    HostTransportProjectionError prepare(const timebase::CompiledTempoMap& tempo_map,
                                         std::uint32_t maximum_block_size) noexcept;

    HostTransportProjectionError project(const format::ProcessContext& context,
                                         playback::TransportSnapshot& snapshot) noexcept;

    void reset() noexcept;

  private:
    const timebase::CompiledTempoMap* tempo_map_ = nullptr;
    std::uint32_t maximum_block_size_ = 0;
    timebase::MonotonicBeat monotonic_{};
    timebase::SamplePosition expected_next_sample_{};
    playback::MeterSignature previous_meter_{};
    playback::LoopRegion previous_loop_{};
    std::uint64_t block_index_ = 0;
    bool has_expected_sample_ = false;
    bool first_block_ = true;
    bool previous_playing_ = false;
    bool previous_host_beat_mapping_ = false;
};

} // namespace pulp::sequence
