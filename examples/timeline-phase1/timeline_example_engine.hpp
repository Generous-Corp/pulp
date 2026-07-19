#pragma once

#include <pulp/format/processor.hpp>
#include <pulp/host/timeline_graph_binding.hpp>
#include <pulp/playback/program_compiler.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace pulp::examples::timeline_phase1 {

/// Shared desktop adapter used by both Phase-1 worked examples. Construction
/// and prepare are control-thread operations; process() is allocation-free
/// after prepare and drives MasterTransport -> TimelineGraphPlaybackBinding ->
/// SignalGraph for every block.
class TimelineExampleEngine {
  public:
    static constexpr audio::RtSafetyClass process_rt_safety_class =
        audio::RtSafetyClass::AudioCallbackSafeAfterPrepare;

    TimelineExampleEngine();
    ~TimelineExampleEngine();
    TimelineExampleEngine(const TimelineExampleEngine&) = delete;
    TimelineExampleEngine& operator=(const TimelineExampleEngine&) = delete;

    bool prepare(playback::ProgramCompileRequest request, double sample_rate,
                 std::uint32_t maximum_block_size, bool add_audible_synth);
    /// Control-thread publication into the existing program store. Track IDs,
    /// tempo-map identity, graph topology, and transport remain stable.
    bool recompile(playback::ProgramCompileRequest request);
    host::TimelineGraphProcessResult process(audio::BufferView<float>& output,
                                             const audio::BufferView<const float>& input) noexcept;

    playback::TransportError set_playing(bool playing) noexcept;
    playback::TransportError seek_samples(std::int64_t sample) noexcept;
    playback::TransportError set_loop_samples(bool enabled, std::int64_t start,
                                              std::int64_t end) noexcept;
    bool prepared() const noexcept;
    bool synth_has_active_notes() const noexcept;
    const playback::TransportSnapshot& last_transport() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::examples::timeline_phase1
