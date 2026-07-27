#pragma once

#include <pulp/timeline/transaction.hpp>

#include <vector>

namespace pulp::playback {

struct DirtyTrackSet {
    bool all = false;
    std::vector<timeline::ItemId> tracks;
};

class CompileInvalidationIndex;

/// Canonical translation from transaction dirtiness to root-track compiler
/// dirtiness. This combines direct edits, transitive SequenceRef dependencies,
/// and compile-context subscriptions in one fail-closed path.
DirtyTrackSet resolve_dirty_tracks(const timeline::Project& project,
                                   timeline::ItemId root_sequence_id,
                                   const timeline::DirtySet& dirty,
                                   const CompileInvalidationIndex& index);

} // namespace pulp::playback
