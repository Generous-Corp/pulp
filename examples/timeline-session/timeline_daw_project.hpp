#pragma once

#include <pulp/playback/program.hpp>
#include <pulp/timeline/command.hpp>
#include <pulp/timeline/journal.hpp>
#include <pulp/timeline/model.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pulp::examples::timeline_session {

/// Counts what an autosave sink actually durably accepted. A worked example
/// should not claim "journal-backed autosave" on the strength of a sink that was
/// merely attached, so the example asserts against these counters.
struct AutosaveStats {
    std::size_t durable_batches = 0;
    std::size_t checkpoints = 0;
    timeline::DocumentRevision last_durable_revision{};
};

/// Worked example (e): a full DAW-style project. Everything at once, on one
/// document —
///
///   * a linear arrangement and a launcher on the SAME tracks, with per-track
///     arbitration choosing between them (hybrid playback, not a global mode);
///   * a reusable chorus arrangement referenced three times by nested
///     `SequenceRef` clips, one of which is diverged into its own copy;
///   * a take lane with a comp;
///   * journal-backed autosave behind a real `JournalSink`;
///   * an agent driving batch edits through the ordinary command API.
///
/// Nothing here introduces a container concept beyond the document model: every
/// step above is expressed as typed commands in one transaction stream.
class TimelineDawProject {
  public:
    TimelineDawProject();
    ~TimelineDawProject();
    TimelineDawProject(const TimelineDawProject&) = delete;
    TimelineDawProject& operator=(const TimelineDawProject&) = delete;

    /// Builds the document and attaches the autosave sink. Returns false if any
    /// stage is rejected by the model rather than papering over it.
    bool build();

    /// Clone the chorus sequence and repoint ONE of its three references at the
    /// copy. This is copy-on-divergence expressed in commands: the other two
    /// references keep sharing the original.
    bool diverge_third_chorus_reference();

    /// Apply a batch of edits the way an agent would: one transaction, ordinary
    /// commands, validated and journaled like any other writer.
    bool apply_agent_batch(std::vector<timeline::Command> commands);

    /// Ask the session to checkpoint everything durable so far.
    bool autosave();

    const timeline::Project& project() const noexcept;
    const AutosaveStats& autosave_stats() const noexcept;
    timeline::DocumentRevision revision() const noexcept;

    /// Per-track arbitration, the compiler primitive that makes a track hybrid.
    playback::ProviderKind track_provider(timeline::ItemId track_id) const noexcept;
    bool set_track_provider(timeline::ItemId track_id, playback::ProviderKind provider);

    /// How many clips in the arrangement reference `sequence_id`.
    std::size_t reference_count(timeline::ItemId sequence_id) const noexcept;

    static timeline::ItemId arrangement_sequence_id() noexcept;
    static timeline::ItemId chorus_sequence_id() noexcept;
    /// Null until `diverge_third_chorus_reference()` runs. The id is minted by
    /// the project's own allocator during the clone, so it is discovered rather
    /// than chosen.
    timeline::ItemId diverged_chorus_sequence_id() const noexcept;
    static timeline::ItemId hybrid_track_id() noexcept;
    static timeline::ItemId comp_track_id() noexcept;
    static timeline::ItemId take_lane_id() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::examples::timeline_session
