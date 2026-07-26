#include <pulp/playback/program_compiler.hpp>

#include <algorithm>
#include <optional>
#include <variant>
#include <vector>

namespace pulp::playback {

DirtyTrackSet lower_dirty_set(const timeline::Project& project, timeline::ItemId root_sequence_id,
                              const timeline::DirtySet& dirty) {
    DirtyTrackSet result;
    if (!project.find_sequence(root_sequence_id)) {
        result.all = true;
        return result;
    }
    struct ParentSite {
        timeline::ItemId parent;
        timeline::ItemId track;
    };
    const auto sequences = project.sequences();
    const auto sequence_index = [&](timeline::ItemId id) -> std::optional<std::size_t> {
        const auto found =
            std::lower_bound(sequences.begin(), sequences.end(), id,
                             [](const timeline::Sequence& sequence, timeline::ItemId wanted) {
                                 return sequence.id() < wanted;
                             });
        if (found == sequences.end() || found->id() != id)
            return std::nullopt;
        return static_cast<std::size_t>(found - sequences.begin());
    };
    std::vector<std::vector<ParentSite>> parents(sequences.size());
    for (const auto& sequence : sequences)
        for (const auto& track : sequence.tracks())
            for (const auto& clip : track.clips())
                if (const auto* reference = std::get_if<timeline::SequenceRef>(&clip.content());
                    reference) {
                    const auto child = sequence_index(reference->sequence_id);
                    if (!child) {
                        result.all = true;
                        return result;
                    }
                    parents[*child].push_back({sequence.id(), track.id()});
                }

    std::vector<std::size_t> pending;
    for (const auto& item : dirty.items()) {
        if (!item.owner_sequence.valid()) {
            result.all = true;
            result.tracks.clear();
            return result;
        }
        if (item.owner_sequence == root_sequence_id) {
            if (!item.owner_track.valid()) {
                result.all = true;
                result.tracks.clear();
                return result;
            }
            result.tracks.push_back(item.owner_track);
            continue;
        }
        const auto owner = sequence_index(item.owner_sequence);
        if (!owner) {
            result.all = true;
            result.tracks.clear();
            return result;
        }
        pending.push_back(*owner);
    }
    std::vector<bool> visited(sequences.size(), false);
    while (!pending.empty()) {
        const auto child = pending.back();
        pending.pop_back();
        if (visited[child])
            continue;
        visited[child] = true;
        for (const auto& site : parents[child]) {
            if (site.parent == root_sequence_id) {
                result.tracks.push_back(site.track);
                continue;
            }
            const auto parent = sequence_index(site.parent);
            if (!parent) {
                result.all = true;
                result.tracks.clear();
                return result;
            }
            pending.push_back(*parent);
        }
    }
    std::sort(result.tracks.begin(), result.tracks.end());
    result.tracks.erase(std::unique(result.tracks.begin(), result.tracks.end()),
                        result.tracks.end());
    return result;
}

} // namespace pulp::playback
