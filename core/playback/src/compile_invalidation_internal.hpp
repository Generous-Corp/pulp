#pragma once

#include <pulp/timeline/compile_context.hpp>
#include <pulp/timeline/model.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pulp::playback::detail {

struct ContextRegistryGeneration {
    std::uint64_t revision = 0;
};

struct SequenceSubscribers {
    timeline::ItemId owner_sequence;
    std::vector<timeline::ItemId> root_tracks;
};

struct OwnedContextSubscribers {
    timeline::ItemId owner_sequence;
    std::array<std::vector<timeline::ItemId>, timeline::kCompileContextKindCount> by_kind;
};

struct CompileInvalidationData {
    timeline::ItemId root_sequence_id;
    timeline::SequenceCompileStructureToken structure_token;
    std::shared_ptr<const ContextRegistryGeneration> registry_generation;
    std::uint64_t registry_revision = 0;
    std::vector<SequenceSubscribers> dependencies;
    std::array<std::vector<timeline::ItemId>, timeline::kCompileContextKindCount> context_by_kind;
    std::vector<OwnedContextSubscribers> context_by_owner;

    bool valid() const noexcept;
    bool matches(const timeline::Project& project, timeline::ItemId requested_root) const noexcept;
    std::span<const timeline::ItemId>
    root_tracks_for(timeline::ItemId owner_sequence) const noexcept;
    std::span<const timeline::ItemId> subscribers(timeline::ItemId owner_sequence,
                                                  timeline::CompileContextKind kind) const noexcept;
};

std::shared_ptr<CompileInvalidationData>
build_sequence_dependencies(const timeline::Project& project, timeline::ItemId root_sequence_id);

} // namespace pulp::playback::detail
