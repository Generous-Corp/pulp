#include "timeline_daw_project.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace pulp;
using namespace pulp::examples::timeline_session;

namespace {

const timeline::Track& track(const timeline::Project& project, timeline::ItemId id) {
    const auto* sequence =
        project.find_sequence(TimelineDawProject::arrangement_sequence_id());
    REQUIRE(sequence != nullptr);
    const auto* found = sequence->find_track(id);
    REQUIRE(found != nullptr);
    return *found;
}

} // namespace

TEST_CASE("the full DAW-style project builds every phase-3 concept on one document",
          "[timeline][examples][daw-project]") {
    TimelineDawProject project;
    REQUIRE(project.build());

    // Linear arrangement and launcher live on the SAME track. The arrangement
    // clip and the slot's source clip are both present; arbitration is what
    // decides which one sounds, per track.
    const auto& hybrid = track(project.project(), TimelineDawProject::hybrid_track_id());
    REQUIRE(hybrid.clips().size() == 2);
    REQUIRE(project.track_provider(TimelineDawProject::hybrid_track_id()) ==
            playback::ProviderKind::Arrangement);
    REQUIRE(project.track_provider(TimelineDawProject::comp_track_id()) ==
            playback::ProviderKind::Launcher);
    REQUIRE(project.set_track_provider(TimelineDawProject::hybrid_track_id(),
                                       playback::ProviderKind::Launcher));
    REQUIRE(project.track_provider(TimelineDawProject::hybrid_track_id()) ==
            playback::ProviderKind::Launcher);

    // A take lane with a real two-segment comp.
    const auto& comped = track(project.project(), TimelineDawProject::comp_track_id());
    REQUIRE(comped.take_lanes().size() == 1);
    REQUIRE(comped.active_take_lane_id() == TimelineDawProject::take_lane_id());
    const auto& lane = comped.take_lanes()[0];
    REQUIRE(lane.takes().size() == 2);
    REQUIRE(lane.comp_segments().size() == 2);
    REQUIRE(lane.comp_segments()[0].take_id != lane.comp_segments()[1].take_id);

    // The reusable chorus is referenced three times and owned by none of them.
    REQUIRE(project.reference_count(TimelineDawProject::chorus_sequence_id()) == 3);
    // No copy exists until a reference is actually diverged.
    REQUIRE_FALSE(project.diverged_chorus_sequence_id().valid());
}

TEST_CASE("diverging one chorus reference copies it without disturbing the other two",
          "[timeline][examples][daw-project][nesting]") {
    TimelineDawProject project;
    REQUIRE(project.build());
    REQUIRE(project.reference_count(TimelineDawProject::chorus_sequence_id()) == 3);

    const auto before_revision = project.revision();
    REQUIRE(project.diverge_third_chorus_reference());
    REQUIRE(project.revision() > before_revision);

    // Copy-on-divergence: exactly one reference moved to the copy, and the copy
    // is a real sibling sequence in the project pool.
    const auto diverged_id = project.diverged_chorus_sequence_id();
    REQUIRE(diverged_id.valid());
    REQUIRE(diverged_id != TimelineDawProject::chorus_sequence_id());
    REQUIRE(project.reference_count(TimelineDawProject::chorus_sequence_id()) == 2);
    REQUIRE(project.reference_count(diverged_id) == 1);
    REQUIRE(project.project().find_sequence(TimelineDawProject::chorus_sequence_id()) != nullptr);
    REQUIRE(project.project().find_sequence(diverged_id) != nullptr);

    // The clone owns its own identities: no identity is shared with the source,
    // which is what makes a later edit to the copy leave the original alone.
    const auto* source = project.project().find_sequence(TimelineDawProject::chorus_sequence_id());
    const auto* clone = project.project().find_sequence(diverged_id);
    REQUIRE(source->tracks().size() == clone->tracks().size());
    REQUIRE(source->tracks()[0].id() != clone->tracks()[0].id());
}

TEST_CASE("an agent batch and autosave go through the ordinary journaled writer path",
          "[timeline][examples][daw-project][agent][autosave]") {
    TimelineDawProject project;
    REQUIRE(project.build());

    // Attaching the sink installs one initial checkpoint, and no transaction has
    // been journaled yet — so the counters start from a known, non-vacuous base.
    REQUIRE(project.autosave_stats().durable_batches == 0);
    REQUIRE(project.autosave_stats().checkpoints == 1);

    REQUIRE(project.diverge_third_chorus_reference());
    REQUIRE(project.autosave_stats().durable_batches == 1);
    REQUIRE(project.autosave_stats().last_durable_revision == project.revision());

    // An agent drives a batch edit through the same command API a human writer
    // uses — one transaction, ordinary typed commands.
    auto clip = timeline::Clip::create(timeline::ItemId{900},
                                       {8 * 4 * timebase::kTicksPerQuarter},
                                       timebase::TickDuration{4 * timebase::kTicksPerQuarter},
                                       timeline::SequenceRef{
                                           TimelineDawProject::chorus_sequence_id(), {0}});
    REQUIRE(clip);
    std::vector<timeline::Command> batch;
    batch.push_back(timeline::InsertClip{TimelineDawProject::arrangement_sequence_id(),
                                         timeline::ItemId{12}, std::move(clip).value()});
    REQUIRE(project.apply_agent_batch(std::move(batch)));

    // The agent's edit is journaled like any other, and the chorus is now
    // referenced three times again — twice originally plus the agent's.
    REQUIRE(project.autosave_stats().durable_batches == 2);
    REQUIRE(project.reference_count(TimelineDawProject::chorus_sequence_id()) == 3);

    REQUIRE(project.autosave());
    REQUIRE(project.autosave_stats().checkpoints == 2);
}

TEST_CASE("the project refuses an agent batch that would break a model invariant",
          "[timeline][examples][daw-project][agent]") {
    TimelineDawProject project;
    REQUIRE(project.build());
    const auto revision = project.revision();

    // Referencing a sequence that does not exist must be rejected, not silently
    // accepted — a batch an agent generated is validated exactly like any other.
    auto clip = timeline::Clip::create(timeline::ItemId{901},
                                       {16 * 4 * timebase::kTicksPerQuarter},
                                       timebase::TickDuration{4 * timebase::kTicksPerQuarter},
                                       timeline::SequenceRef{timeline::ItemId{9999}, {0}});
    REQUIRE(clip);
    std::vector<timeline::Command> batch;
    batch.push_back(timeline::InsertClip{TimelineDawProject::arrangement_sequence_id(),
                                         timeline::ItemId{12}, std::move(clip).value()});
    REQUIRE_FALSE(project.apply_agent_batch(std::move(batch)));
    REQUIRE(project.revision() == revision);
    REQUIRE(project.autosave_stats().durable_batches == 0);
}
