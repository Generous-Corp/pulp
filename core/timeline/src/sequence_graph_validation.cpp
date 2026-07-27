#include <pulp/timeline/model.hpp>

#include "sequence_graph_validation.hpp"

#include <algorithm>
#include <optional>
#include <vector>

namespace pulp::timeline {

namespace {

std::optional<std::size_t> sequence_index(std::span<const Sequence> sequences, ItemId id) {
    const auto found = std::lower_bound(
        sequences.begin(), sequences.end(), id,
        [](const Sequence& sequence, ItemId wanted) { return sequence.id() < wanted; });
    if (found == sequences.end() || found->id() != id)
        return std::nullopt;
    return static_cast<std::size_t>(found - sequences.begin());
}

std::optional<ModelError> find_sequence_cycle(std::span<const Sequence> sequences) {
    struct Frame {
        std::size_t sequence = 0;
        std::size_t next_reference = 0;
    };

    std::vector<std::uint8_t> state(sequences.size(), 0);
    std::vector<Frame> stack;
    stack.reserve(sequences.size());
    for (std::size_t start = 0; start < sequences.size(); ++start) {
        if (state[start] != 0)
            continue;
        state[start] = 1;
        stack.push_back({start, 0});
        while (!stack.empty()) {
            auto& frame = stack.back();
            const auto references = sequences[frame.sequence].outgoing_sequence_refs();
            if (frame.next_reference == references.size()) {
                state[frame.sequence] = 2;
                stack.pop_back();
                continue;
            }
            const auto child_id = references[frame.next_reference++];
            const auto child = *sequence_index(sequences, child_id);
            if (state[child] == 1)
                return ModelError{ModelErrorCode::SequenceReferenceCycle,
                                  sequences[frame.sequence].id(), child_id};
            if (state[child] == 0) {
                state[child] = 1;
                stack.push_back({child, 0});
            }
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<ModelError> validate_sequence_graph(std::span<const Sequence> sequences) {
    for (const auto& sequence : sequences)
        for (const auto child : sequence.outgoing_sequence_refs())
            if (!sequence_index(sequences, child))
                return ModelError{ModelErrorCode::MissingSequenceReference, sequence.id(), child};

    std::vector<std::size_t> incoming(sequences.size(), 0);
    for (const auto& sequence : sequences)
        for (const auto child : sequence.outgoing_sequence_refs())
            ++incoming[*sequence_index(sequences, child)];
    std::vector<std::size_t> pending;
    pending.reserve(sequences.size());
    for (std::size_t index = 0; index < incoming.size(); ++index)
        if (incoming[index] == 0)
            pending.push_back(index);
    std::vector<std::size_t> depth(sequences.size(), 0);
    std::size_t visited = 0;
    while (!pending.empty()) {
        const auto parent = pending.back();
        pending.pop_back();
        ++visited;
        for (const auto child_id : sequences[parent].outgoing_sequence_refs()) {
            const auto child = *sequence_index(sequences, child_id);
            depth[child] = std::max(depth[child], depth[parent] + 1);
            if (depth[child] > kMaxSequenceNestingDepth)
                return ModelError{ModelErrorCode::SequenceNestingTooDeep, sequences[parent].id(),
                                  child_id};
            if (--incoming[child] == 0)
                pending.push_back(child);
        }
    }
    if (visited == sequences.size())
        return std::nullopt;
    return find_sequence_cycle(sequences);
}

std::optional<ModelError> validate_sequence_edge(std::span<const Sequence> sequences,
                                                 ItemId parent_id, ItemId child_id) {
    const auto parent = sequence_index(sequences, parent_id);
    const auto child = sequence_index(sequences, child_id);
    if (!child)
        return ModelError{ModelErrorCode::MissingSequenceReference, parent_id, child_id};
    if (!parent)
        return ModelError{ModelErrorCode::InvalidItemId, parent_id, {}};

    std::vector<std::vector<std::size_t>> parents(sequences.size());
    for (std::size_t index = 0; index < sequences.size(); ++index)
        for (const auto referenced : sequences[index].outgoing_sequence_refs())
            if (const auto target = sequence_index(sequences, referenced))
                parents[*target].push_back(index);

    const auto longest_from =
        [&](std::size_t start, bool reverse,
            std::optional<std::size_t> forbidden) -> runtime::Result<std::size_t, ModelError> {
        std::vector<std::size_t> distance(sequences.size(), 0);
        std::vector<bool> reached(sequences.size(), false);
        std::vector<std::size_t> pending{start};
        reached[start] = true;
        std::size_t maximum = 0;
        while (!pending.empty()) {
            const auto index = pending.back();
            pending.pop_back();
            if (forbidden && index == *forbidden)
                return runtime::Err(
                    ModelError{ModelErrorCode::SequenceReferenceCycle, parent_id, child_id});
            maximum = std::max(maximum, distance[index]);
            const auto visit = [&](std::size_t next) {
                const auto candidate = distance[index] + 1;
                if (!reached[next] || distance[next] < candidate) {
                    reached[next] = true;
                    distance[next] = candidate;
                    pending.push_back(next);
                }
            };
            if (reverse) {
                for (const auto next : parents[index])
                    visit(next);
            } else {
                for (const auto referenced : sequences[index].outgoing_sequence_refs())
                    visit(*sequence_index(sequences, referenced));
            }
        }
        return runtime::Ok(maximum);
    };
    auto tail = longest_from(*child, false, parent);
    if (!tail)
        return tail.error();
    auto prefix = longest_from(*parent, true, std::nullopt);
    if (!prefix)
        return prefix.error();
    if (prefix.value() + 1 + tail.value() > kMaxSequenceNestingDepth)
        return ModelError{ModelErrorCode::SequenceNestingTooDeep, parent_id, child_id};
    return std::nullopt;
}

} // namespace pulp::timeline
