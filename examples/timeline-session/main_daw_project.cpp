#include "timeline_daw_project.hpp"

#include <cstdio>
#include <vector>

// Worked example (e): builds the full DAW-style project, diverges one of three
// chorus references, runs an agent batch, and autosaves — all headless.
int main() {
    using namespace pulp;
    using namespace pulp::examples::timeline_session;

    TimelineDawProject project;
    if (!project.build()) {
        std::fprintf(stderr, "daw project: build failed\n");
        return 1;
    }
    std::printf("chorus references: %zu\n",
                project.reference_count(TimelineDawProject::chorus_sequence_id()));

    if (!project.diverge_third_chorus_reference()) {
        std::fprintf(stderr, "daw project: divergence refused\n");
        return 1;
    }
    std::printf("after divergence: %zu shared, %zu diverged\n",
                project.reference_count(TimelineDawProject::chorus_sequence_id()),
                project.reference_count(project.diverged_chorus_sequence_id()));

    auto clip = timeline::Clip::create(timeline::ItemId{900},
                                       {8 * 4 * timebase::kTicksPerQuarter},
                                       timebase::TickDuration{4 * timebase::kTicksPerQuarter},
                                       timeline::SequenceRef{
                                           TimelineDawProject::chorus_sequence_id(), {0}});
    if (!clip) {
        std::fprintf(stderr, "daw project: agent clip invalid\n");
        return 1;
    }
    std::vector<timeline::Command> batch;
    batch.push_back(timeline::InsertClip{TimelineDawProject::arrangement_sequence_id(),
                                         timeline::ItemId{12}, std::move(clip).value()});
    if (!project.apply_agent_batch(std::move(batch))) {
        std::fprintf(stderr, "daw project: agent batch rejected\n");
        return 1;
    }
    if (!project.autosave()) {
        std::fprintf(stderr, "daw project: autosave failed\n");
        return 1;
    }
    std::printf("journaled batches: %zu, checkpoints: %zu\n",
                project.autosave_stats().durable_batches,
                project.autosave_stats().checkpoints);
    return 0;
}
