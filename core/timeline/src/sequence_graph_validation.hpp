#pragma once // Private timeline implementation detail.

#include <pulp/timeline/model.hpp>

#include <optional>
#include <span>

namespace pulp::timeline {

std::optional<ModelError> validate_sequence_graph(std::span<const Sequence> sequences);
std::optional<ModelError> validate_sequence_edge(std::span<const Sequence> sequences,
                                                 ItemId parent_id, ItemId child_id);

} // namespace pulp::timeline
