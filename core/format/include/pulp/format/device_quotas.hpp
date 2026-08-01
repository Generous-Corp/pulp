#pragma once

#include <pulp/platform/device_capability.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>

/// What the product grants a device: the quota table indexed by capability tier
/// and live thermal state.
///
/// This is policy, and it lives above the engine on purpose. The tier and
/// thermal ladders it indexes are platform observations and live in
/// `pulp/platform/device_capability.hpp`, which is named in every row of the
/// engine's declared dependency floors — so nothing placed there can be
/// objected to by any floor row. A budget over voices, render-graph nodes,
/// editing surfaces, and preview fidelity is exactly the kind of thing those
/// rows exist to keep out of the modules below: `core/format` appears in no
/// floor row, so an engine module that reaches for this header is rejected.
///
/// The direction of consumption is the reason that costs nothing. Every
/// enforcement point this table names already takes its ceilings as injected
/// configuration rather than looking a table up — `playback::AudioRendererLimits`,
/// `playback::AutomationPlaybackLimits`, `timeline::SessionLimits`,
/// `graph::GraphRuntimeLimits`, `format::PrepareResourceLimits`. The consumer of
/// the table is therefore whoever *constructs* those, which is the shell, not
/// the module being bounded. No engine module needs to see this, and none
/// should.
namespace pulp::format {

/// How much work a preview render may do, ascending in cost.
enum class PreviewQuality : std::uint8_t {
    /// Previews are not rendered; consumers show authored geometry only.
    Disabled = 0,
    /// Decimated previews: coarser resolution, fewer passes.
    Reduced = 1,
    /// Full-detail previews.
    Full = 2,
};

/// Ceilings a consumer may grant at one (tier, thermal state) cell.
///
/// These are declarations, not enforcement. Nothing consumes them yet; each
/// field names a quantity an existing owner could enforce at its own admission
/// point — a voice pool for `max_voices`, a compiled render program's node
/// admission for `max_nodes`, writer registration for
/// `max_simultaneous_editors`, and a preview request for `preview_quality`.
///
/// Those four owners sit in four different modules, three of them behind floor
/// rows that cannot see each other. That is why the fields travel together as
/// one row here and are handed down individually: the row is a single policy
/// decision about one device, and its monotonicity is a property of the whole
/// row rather than of any one field.
struct DeviceQuotas {
    /// Simultaneously sounding voices across all instruments.
    std::uint32_t max_voices = 0;
    /// Nodes in the compiled render graph.
    std::uint32_t max_nodes = 0;
    /// Editing surfaces held open against one document at once.
    std::uint32_t max_simultaneous_editors = 0;
    /// Detail budget for preview rendering.
    PreviewQuality preview_quality = PreviewQuality::Disabled;

    constexpr auto operator<=>(const DeviceQuotas&) const = default;
};

namespace detail {

/// The quota table, indexed `[tier][thermal state]`.
///
/// Written out cell by cell rather than derived from a scaling formula so the
/// table is reviewable as data and so the monotonicity property is a real
/// assertion about these numbers rather than a restatement of a formula.
///
/// Two orderings hold across the table and are asserted in test: along a row,
/// no field ever rises as the device gets hotter; down a column, no field ever
/// falls as the tier rises.
inline constexpr DeviceQuotas
    kQuotaTable[platform::kDeviceCapabilityTierCount][platform::kThermalStateCount] = {
        // Constrained: Nominal, Fair, Serious, Critical
        {
            {16, 48, 1, PreviewQuality::Reduced},
            {12, 40, 1, PreviewQuality::Reduced},
            {8, 32, 1, PreviewQuality::Reduced},
            {4, 16, 1, PreviewQuality::Disabled},
        },
        // Standard: Nominal, Fair, Serious, Critical
        {
            {64, 192, 2, PreviewQuality::Full},
            {48, 160, 2, PreviewQuality::Full},
            {32, 112, 1, PreviewQuality::Reduced},
            {16, 64, 1, PreviewQuality::Disabled},
        },
        // Full: Nominal, Fair, Serious, Critical
        {
            {256, 768, 4, PreviewQuality::Full},
            {192, 640, 3, PreviewQuality::Full},
            {128, 448, 2, PreviewQuality::Reduced},
            {64, 256, 1, PreviewQuality::Disabled},
        },
};

} // namespace detail

/// Returns the quotas granted at one tier under one live thermal state.
///
/// A value cast in from outside the declared enumerators clamps to the most
/// restrictive cell rather than reading past the table: a quota lookup that
/// cannot be trusted should grant the least, not the most.
[[nodiscard]] constexpr DeviceQuotas device_quotas(platform::DeviceCapabilityTier tier,
                                                   platform::ThermalState thermal) {
    std::size_t tier_index = static_cast<std::size_t>(tier);
    if (tier_index >= platform::kDeviceCapabilityTierCount) {
        tier_index = 0;
    }
    std::size_t thermal_index = static_cast<std::size_t>(thermal);
    if (thermal_index >= platform::kThermalStateCount) {
        thermal_index = platform::kThermalStateCount - 1;
    }
    return detail::kQuotaTable[tier_index][thermal_index];
}

} // namespace pulp::format
