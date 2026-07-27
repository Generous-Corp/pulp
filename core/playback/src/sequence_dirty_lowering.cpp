#include <pulp/playback/compile_context_registry.hpp>
#include <pulp/playback/dirty_track_resolver.hpp>

#include "compile_invalidation_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace pulp::playback {
namespace {

std::optional<std::size_t> sequence_index(std::span<const timeline::Sequence> sequences,
                                          timeline::ItemId id) {
    const auto found =
        std::lower_bound(sequences.begin(), sequences.end(), id,
                         [](const timeline::Sequence& sequence, timeline::ItemId wanted) {
                             return sequence.id() < wanted;
                         });
    if (found == sequences.end() || found->id() != id)
        return std::nullopt;
    return static_cast<std::size_t>(found - sequences.begin());
}

bool affects_compiled_content(const timeline::DirtyItem& item) {
    const auto flags = static_cast<std::uint16_t>(item.flags);
    const auto metadata_only = static_cast<std::uint16_t>(timeline::DirtyFlags::Context) |
                               static_cast<std::uint16_t>(timeline::DirtyFlags::Marker);
    return (flags & ~metadata_only) != 0;
}

bool requires_invalidation_index(const timeline::DirtyItem& item) {
    const auto flags = static_cast<std::uint16_t>(item.flags);
    const auto metadata = static_cast<std::uint16_t>(timeline::DirtyFlags::Context) |
                          static_cast<std::uint16_t>(timeline::DirtyFlags::Marker);
    return !(!item.owner_track.valid() && (flags & metadata) != 0) &&
           affects_compiled_content(item);
}

void canonicalize(DirtyTrackSet& result) {
    std::sort(result.tracks.begin(), result.tracks.end());
    result.tracks.erase(std::unique(result.tracks.begin(), result.tracks.end()),
                        result.tracks.end());
}

} // namespace

std::shared_ptr<detail::CompileInvalidationData>
detail::build_sequence_dependencies(const timeline::Project& project,
                                    timeline::ItemId root_sequence_id) {
    const auto* root = project.find_sequence(root_sequence_id);
    if (!root)
        return {};
    auto result = std::make_shared<CompileInvalidationData>();
    result->root_sequence_id = root_sequence_id;
    result->structure_token = project.sequence_compile_structure_token();
    const auto sequences = project.sequences();

    std::vector<std::vector<std::size_t>> children(sequences.size());
    std::vector<bool> reachable(sequences.size(), false);
    std::vector<std::size_t> pending;
    const auto root_index = sequence_index(sequences, root_sequence_id);
    if (!root_index)
        return {};
    for (std::size_t index = 0; index < sequences.size(); ++index) {
        for (const auto child_id : sequences[index].outgoing_sequence_refs()) {
            const auto child = sequence_index(sequences, child_id);
            if (!child)
                return {};
            children[index].push_back(*child);
        }
    }
    pending.push_back(*root_index);
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        for (const auto child : children[current]) {
            if (reachable[child])
                continue;
            reachable[child] = true;
            pending.push_back(child);
        }
    }

    std::vector<std::vector<timeline::ItemId>> root_tracks_by_sequence(sequences.size());
    for (const auto& root_track : root->tracks()) {
        for (const auto& clip : root_track.clips()) {
            const auto* reference = std::get_if<timeline::SequenceRef>(&clip.content());
            if (!reference)
                continue;
            const auto child = sequence_index(sequences, reference->sequence_id);
            if (!child)
                return {};
            root_tracks_by_sequence[*child].push_back(root_track.id());
        }
    }

    std::vector<std::size_t> indegree(sequences.size(), 0);
    for (std::size_t parent = 0; parent < sequences.size(); ++parent) {
        if (parent == *root_index || !reachable[parent])
            continue;
        for (const auto child : children[parent])
            if (reachable[child])
                ++indegree[child];
    }
    std::vector<std::size_t> ready;
    for (std::size_t index = 0; index < sequences.size(); ++index)
        if (reachable[index] && indegree[index] == 0)
            ready.push_back(index);
    while (!ready.empty()) {
        const auto current = ready.back();
        ready.pop_back();
        auto& subscribers = root_tracks_by_sequence[current];
        std::sort(subscribers.begin(), subscribers.end());
        subscribers.erase(std::unique(subscribers.begin(), subscribers.end()), subscribers.end());
        if (!subscribers.empty())
            result->dependencies.push_back(
                {.owner_sequence = sequences[current].id(), .root_tracks = subscribers});
        for (const auto child : children[current]) {
            auto& child_subscribers = root_tracks_by_sequence[child];
            child_subscribers.insert(child_subscribers.end(), subscribers.begin(),
                                     subscribers.end());
            if (--indegree[child] == 0)
                ready.push_back(child);
        }
    }
    std::sort(result->dependencies.begin(), result->dependencies.end(),
              [](const SequenceSubscribers& lhs, const SequenceSubscribers& rhs) {
                  return lhs.owner_sequence < rhs.owner_sequence;
              });
    return result;
}

bool detail::CompileInvalidationData::valid() const noexcept {
    return root_sequence_id.valid() && structure_token.valid() && registry_generation &&
           registry_generation->revision == registry_revision;
}

bool detail::CompileInvalidationData::matches(const timeline::Project& project,
                                              timeline::ItemId requested_root) const noexcept {
    return valid() && root_sequence_id == requested_root && project.find_sequence(requested_root) &&
           structure_token == project.sequence_compile_structure_token();
}

std::span<const timeline::ItemId>
detail::CompileInvalidationData::root_tracks_for(timeline::ItemId owner_sequence) const noexcept {
    const auto found =
        std::lower_bound(dependencies.begin(), dependencies.end(), owner_sequence,
                         [](const SequenceSubscribers& entry, timeline::ItemId wanted) {
                             return entry.owner_sequence < wanted;
                         });
    return found != dependencies.end() && found->owner_sequence == owner_sequence
               ? std::span<const timeline::ItemId>(found->root_tracks)
               : std::span<const timeline::ItemId>{};
}

DirtyTrackSet resolve_dirty_tracks(const timeline::Project& project,
                                   timeline::ItemId root_sequence_id,
                                   const timeline::DirtySet& dirty,
                                   const CompileInvalidationIndex& index) {
    DirtyTrackSet result;
    const auto* root = project.find_sequence(root_sequence_id);
    if (!root)
        return result;
    const bool needs_index =
        !dirty.contexts().empty() ||
        std::any_of(dirty.items().begin(), dirty.items().end(), requires_invalidation_index);
    if (!needs_index)
        return result;
    if (!index.data_ || !index.data_->matches(project, root_sequence_id)) {
        result.all = true;
        return result;
    }

    for (const auto& item : dirty.items()) {
        if (!item.owner_sequence.valid()) {
            result.all = true;
            result.tracks.clear();
            return result;
        }
        const auto flags = static_cast<std::uint16_t>(item.flags);
        const auto metadata = static_cast<std::uint16_t>(timeline::DirtyFlags::Context) |
                              static_cast<std::uint16_t>(timeline::DirtyFlags::Marker);
        if (!item.owner_track.valid() && (flags & metadata) != 0)
            continue;
        if (!affects_compiled_content(item))
            continue;
        if (item.owner_sequence == root_sequence_id) {
            if (!item.owner_track.valid()) {
                result.all = true;
                result.tracks.clear();
                return result;
            }
            result.tracks.push_back(item.owner_track);
            continue;
        }
        if (!project.find_sequence(item.owner_sequence)) {
            result.all = true;
            result.tracks.clear();
            return result;
        }
        const auto subscribers = index.data_->root_tracks_for(item.owner_sequence);
        result.tracks.insert(result.tracks.end(), subscribers.begin(), subscribers.end());
    }

    for (const auto& context : dirty.contexts()) {
        const auto subscribers = index.subscribers(context.owner_sequence, context.kind);
        for (const auto track_id : subscribers) {
            const auto* track = root->find_track(track_id);
            if (track && !track->freeze().has_value() && !track->active_take_lane_id().valid())
                result.tracks.push_back(track_id);
        }
    }
    canonicalize(result);
    return result;
}

} // namespace pulp::playback
