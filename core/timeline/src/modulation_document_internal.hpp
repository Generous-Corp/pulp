#pragma once

#include <pulp/timeline/model.hpp>

#include <optional>
#include <span>
#include <vector>

namespace pulp::timeline::detail {

/// Appends every identity the three modulation collections own, in the order a
/// track enumerates them.
void append_modulation_owned_ids(std::span<const Modulator> modulators,
                                 std::span<const MacroControl> macros,
                                 std::span<const ModulationRoute> routes,
                                 std::vector<ItemId>& ids);

/// Validates one track's modulation collections against the rest of the track.
///
/// Beyond identity validity and disjointness, this enforces the two references a
/// route carries: its source must be a modulator or macro of the matching kind
/// on this same track, and a device-parameter target must name a placement in
/// this track's chain. Both are what make a removal a question with an answer
/// rather than a document left pointing at nothing.
std::optional<ModelError>
validate_attached_modulation(std::span<const Modulator> modulators,
                             std::span<const MacroControl> macros,
                             std::span<const ModulationRoute> routes,
                             std::span<const DevicePlacement> device_chain,
                             std::span<const ItemId> other_owned_ids = {});

/// Rebuilds one route against a remap table, reporting the identity that had no
/// replacement. Route, source, and any referenced placement are all rewritten;
/// a target that references nothing survives unchanged.
runtime::Result<ModulationRoute, ModelError>
remap_attached_modulation_route(const ModulationRoute& route, const IdRemapTable& table);

} // namespace pulp::timeline::detail
