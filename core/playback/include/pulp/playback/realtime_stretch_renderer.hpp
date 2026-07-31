#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/playback/realtime_stretch_state_bank.hpp>
#include <pulp/playback/transport.hpp>

#include <cstdint>
#include <memory>

namespace pulp::playback {

class PlaybackProgram;
class TrackProgram;
struct TrackMixerProgram;

enum class RealtimeStretchRenderCode : std::uint8_t {
    NotRequired,
    Rendered,
    GapIdentityChanged,
    StateRequired,
    StalePublication,
    UnsupportedScrubbing,
    ImpossibleRatio,
    Backpressure,
    Underflow,
};

/// One exact-publication live Stretch runtime. It owns a disjoint mutable lane
/// per track; the desktop graph may process those lanes in parallel, but its
/// one-node-per-track topology never enters one lane concurrently.
class RealtimeStretchProgramRuntime {
  public:
    RealtimeStretchProgramRuntime();
    ~RealtimeStretchProgramRuntime();
    RealtimeStretchProgramRuntime(RealtimeStretchProgramRuntime&&) noexcept;
    RealtimeStretchProgramRuntime& operator=(RealtimeStretchProgramRuntime&&) noexcept;
    RealtimeStretchProgramRuntime(const RealtimeStretchProgramRuntime&) = delete;
    RealtimeStretchProgramRuntime& operator=(const RealtimeStretchProgramRuntime&) = delete;

    RealtimeStretchStateBankAdmission prepare(const PlaybackProgram& program, double sample_rate,
                                              std::uint32_t maximum_block_frames,
                                              std::uint32_t output_channels,
                                              const AudioRendererLimits& limits);

    RealtimeStretchRenderCode preflight_track(const PlaybackProgram& program,
                                              const TrackProgram& track,
                                              const TransportSnapshot& transport,
                                              audio::BufferView<float> output) const noexcept;

    RealtimeStretchRenderCode process_track(const PlaybackProgram& program,
                                            const TrackProgram& track,
                                            const TrackMixerProgram& mixer,
                                            const TransportSnapshot& transport,
                                            audio::BufferView<float> nonstretch_output) noexcept;

    std::uint32_t latency_samples() const noexcept;
    std::uint64_t reserved_runtime_bytes() const noexcept;
    bool track_uses_realtime_stretch(timeline::ItemId track_id) const noexcept;
    void reset() noexcept;
    void force_prepare_allocation_failure_for_test() noexcept;
    void force_post_mutation_failure_for_test() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::playback
