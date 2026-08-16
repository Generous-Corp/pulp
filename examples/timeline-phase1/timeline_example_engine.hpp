#pragma once

#include <pulp/format/processor.hpp>
#include <pulp/host/timeline_graph_binding.hpp>
#include <pulp/playback/program_compiler.hpp>
#include <pulp/timeline/transaction.hpp>

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
    /// Sparse control-thread publication driven by an exact committed document
    /// delta. Unlike recompile(), this never sets `dirty.all`: the compiler
    /// resolves the dirty track set from `committed`, so tracks the transaction
    /// did not touch keep their existing TrackProgram by pointer rather than
    /// being rebuilt. The compile context, tempo-map owner, and decoded-audio
    /// pool are carried over from the live program so the only thing that
    /// changes is what the edit actually changed.
    bool recompile_committed(const timeline::CommitResult& committed);
    /// Generation and document revision of the live program.
    struct ProgramIdentity {
        playback::ProgramGeneration generation = 0;
        std::uint64_t document_revision = 0;
        bool valid = false;
    };
    ProgramIdentity program_identity() const noexcept;
    /// Shared owner of one track's compiled program, copied out while the
    /// store's read guard is held. Returning the owner rather than the guard
    /// lets an oracle compare pointer identity across recompiles without
    /// pinning the reader for the lifetime of the comparison.
    std::shared_ptr<const playback::TrackProgram> track_program(timeline::ItemId id) const;
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
