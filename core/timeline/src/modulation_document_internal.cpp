#include "modulation_document_internal.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace pulp::timeline::detail {
namespace {

template <typename T>
runtime::Result<T, ModelError> fail(ModelErrorCode code, ItemId item = {}, ItemId related = {}) {
    return runtime::Err(ModelError{code, item, related});
}

// The document identity a target references, or none when the target names
// something the owning track always has. Only a referenced identity can be
// missing from the track.
std::optional<ItemId> referenced_device_placement(const ModulationTarget& target) noexcept {
    return std::visit(
        ModulationTargetCases{
            [](const DeviceParameterTarget& device) -> std::optional<ItemId> {
                return device.device_placement_id;
            },
            [](const TrackMixerTarget&) -> std::optional<ItemId> { return std::nullopt; },
        },
        target);
}

bool valid_depth(float depth) noexcept {
    // NaN fails every comparison, so the range test also rejects it.
    return depth >= -kMaximumModulationDepth && depth <= kMaximumModulationDepth;
}

runtime::Result<ModulationTarget, ItemId> remap_target(const ModulationTarget& target,
                                                       const IdRemapTable& table) {
    using TargetResult = runtime::Result<ModulationTarget, ItemId>;
    return std::visit(
        ModulationTargetCases{
            [&](const DeviceParameterTarget& device) -> TargetResult {
                const auto mapped = table.find(device.device_placement_id);
                if (!mapped)
                    return runtime::Err(device.device_placement_id);
                return runtime::Ok(
                    ModulationTarget(DeviceParameterTarget{*mapped, device.param_id}));
            },
            [](const TrackMixerTarget& mixer) -> TargetResult {
                return runtime::Ok(ModulationTarget(mixer));
            },
        },
        target);
}

} // namespace

void append_modulation_owned_ids(std::span<const Modulator> modulators,
                                 std::span<const MacroControl> macros,
                                 std::span<const ModulationRoute> routes,
                                 std::vector<ItemId>& ids) {
    for (const auto& modulator : modulators)
        ids.push_back(modulator.id);
    for (const auto& macro : macros)
        ids.push_back(macro.id);
    for (const auto& route : routes)
        ids.push_back(route.id);
}

std::optional<ModelError> validate_attached_modulation(
    std::span<const Modulator> modulators, std::span<const MacroControl> macros,
    std::span<const ModulationRoute> routes, std::span<const DevicePlacement> device_chain,
    std::span<const ItemId> other_owned_ids) {
    std::vector<ItemId> ids(other_owned_ids.begin(), other_owned_ids.end());
    append_modulation_owned_ids(modulators, macros, routes, ids);
    for (const auto id : ids)
        if (!id.valid())
            return ModelError{ModelErrorCode::InvalidItemId, id, {}};
    std::sort(ids.begin(), ids.end());
    if (const auto duplicate = std::adjacent_find(ids.begin(), ids.end()); duplicate != ids.end())
        return ModelError{ModelErrorCode::DuplicateItemId, *duplicate, {}};

    for (const auto& modulator : modulators)
        if (!known_modulator_kind(modulator.kind))
            return ModelError{ModelErrorCode::InvalidModulator, modulator.id, {}};
    for (const auto& macro : macros)
        // NaN fails every comparison, so the range test also rejects it.
        if (!(macro.value >= 0.0f) || !(macro.value <= 1.0f))
            return ModelError{ModelErrorCode::InvalidMacroControl, macro.id, {}};

    for (const auto& route : routes) {
        if (!route.source.valid() || !valid_depth(route.depth))
            return ModelError{ModelErrorCode::InvalidModulationRoute, route.id, route.source.id};
        // A route names its source's kind as well as its identity, so a macro
        // can never quietly stand in for a modulator that shared its ID.
        const bool source_present =
            route.source.kind == ModulationSourceKind::Modulator
                ? std::any_of(modulators.begin(), modulators.end(),
                              [&](const Modulator& candidate) {
                                  return candidate.id == route.source.id;
                              })
                : std::any_of(macros.begin(), macros.end(), [&](const MacroControl& candidate) {
                      return candidate.id == route.source.id;
                  });
        if (!source_present)
            return ModelError{ModelErrorCode::MissingModulationSource, route.id, route.source.id};
        if (const auto placement_id = referenced_device_placement(route.target)) {
            const auto placement = std::find_if(
                device_chain.begin(), device_chain.end(),
                [&](const DevicePlacement& candidate) { return candidate.id == *placement_id; });
            if (placement == device_chain.end())
                return ModelError{ModelErrorCode::MissingModulationTarget, route.id,
                                  *placement_id};
        }
    }

    // Two routes may reach one parameter — their offsets sum, which is exactly
    // the difference from automation, where two lanes on one parameter is a
    // contradiction. What a document may not hold is the same source reaching
    // the same parameter twice, which is one connection stated twice with two
    // depths and no rule for which wins.
    struct RouteKey {
        ItemId source;
        // Carried rather than inferred from `source`. Identities are disjoint
        // across the three collections a few lines above, so the ID alone would
        // in fact separate a modulator from a macro today — but that makes this
        // check depend on an invariant enforced elsewhere, and it costs one
        // field not to.
        std::uint8_t source_kind = 0;
        std::uint8_t target_kind = 0;
        ItemId referenced_item;
        std::uint32_t parameter = 0;
        constexpr auto operator<=>(const RouteKey&) const = default;
    };
    std::vector<std::pair<RouteKey, ItemId>> keys;
    keys.reserve(routes.size());
    for (const auto& route : routes) {
        auto key = std::visit(
            ModulationTargetCases{
                [&](const DeviceParameterTarget& target) {
                    return RouteKey{route.source.id,
                                    static_cast<std::uint8_t>(route.source.kind), 0,
                                    target.device_placement_id, target.param_id};
                },
                [&](const TrackMixerTarget& target) {
                    return RouteKey{route.source.id,
                                    static_cast<std::uint8_t>(route.source.kind), 1, {},
                                    static_cast<std::uint32_t>(target.parameter)};
                },
            },
            route.target);
        keys.emplace_back(key, route.id);
    }
    std::sort(keys.begin(), keys.end());
    const auto duplicate_key = std::adjacent_find(
        keys.begin(), keys.end(),
        [](const auto& lhs, const auto& rhs) { return lhs.first == rhs.first; });
    if (duplicate_key != keys.end())
        return ModelError{ModelErrorCode::InvalidModulationRoute, std::next(duplicate_key)->second,
                          duplicate_key->second};
    return std::nullopt;
}

runtime::Result<ModulationRoute, ModelError>
remap_attached_modulation_route(const ModulationRoute& route, const IdRemapTable& table) {
    const auto route_id = table.find(route.id);
    if (!route_id)
        return fail<ModulationRoute>(ModelErrorCode::InvalidIdentityTransition, route.id, route.id);
    const auto source_id = table.find(route.source.id);
    if (!source_id)
        return fail<ModulationRoute>(ModelErrorCode::InvalidIdentityTransition, route.id,
                                     route.source.id);
    auto target = remap_target(route.target, table);
    if (!target)
        return fail<ModulationRoute>(ModelErrorCode::InvalidIdentityTransition, route.id,
                                     target.error());
    ModulationRoute remapped = route;
    remapped.id = *route_id;
    remapped.source.id = *source_id;
    remapped.target = std::move(target).value();
    return runtime::Ok(std::move(remapped));
}

} // namespace pulp::timeline::detail
