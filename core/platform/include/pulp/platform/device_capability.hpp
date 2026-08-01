#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>

/// Device capability vocabulary: one tier ladder, one thermal ladder, and the
/// quota table they index.
///
/// This header is deliberately dependency-free. It includes no Pulp header, so
/// it cannot name a document, a session, a transport, or a view, and every
/// entry point is a `constexpr` free function over by-value trivially-copyable
/// inputs. A projection that cannot reach mutable state by construction needs
/// no runtime guard against mutating one.
namespace pulp::platform {

// ── Capability tier ────────────────────────────────────────────────────────

/// How much of the product a device can carry, on one ascending ladder.
///
/// There is exactly one ladder. The browser lane's Tier A/B/C and the mobile
/// lane's M-A/M-B/M-C are the same three rungs named twice, so they are the
/// same three enumerators here:
///
/// | Rung          | Browser | Mobile | Shape                                 |
/// |---------------|---------|--------|---------------------------------------|
/// | `Constrained` | Tier A  | M-A    | Review and perform; no local realtime |
/// | `Standard`    | Tier B  | M-B    | Authoring-complete                    |
/// | `Full`        | Tier C  | M-C    | Precision authoring with headroom     |
///
/// The rung is computed from observable inputs (see `DeviceCapabilityInputs`),
/// never assumed from form factor: a tablet with a keyboard and a phone-sized
/// browser tab land wherever their capabilities put them.
enum class DeviceCapabilityTier : std::uint8_t {
    Constrained = 0,
    Standard = 1,
    Full = 2,
};

inline constexpr std::size_t kDeviceCapabilityTierCount = 3;

// ── Thermal ladder ─────────────────────────────────────────────────────────

/// Live thermal pressure, ascending in heat.
///
/// Four rungs, chosen to be the coarsest ladder every reporting platform can
/// be projected onto without inventing precision it does not have. Platforms
/// with a finer ladder collapse into these; a platform with no thermal API at
/// all can only ever report `Nominal`, which is why the absence of a thermal
/// API lowers the capability tier rather than being ignored — see
/// `DeviceCapabilityInputs::thermal_reporting_available`.
enum class ThermalState : std::uint8_t {
    Nominal = 0,
    Fair = 1,
    Serious = 2,
    Critical = 3,
};

inline constexpr std::size_t kThermalStateCount = 4;

// ── Observable inputs ──────────────────────────────────────────────────────

/// Precision of the device's primary pointing device.
enum class PointerPrecision : std::uint8_t {
    /// Touch or another pointer whose contact area exceeds a small target.
    Coarse = 0,
    /// Mouse, trackpad, or stylus with sub-target precision.
    Fine = 1,
};

/// What a shell observed about the device it is running on.
///
/// Every field is something a shell can measure. Nothing here is a form
/// factor, an OS name, or a browser name: a caller that knows only "this is a
/// phone" cannot fill this in, which is the point.
struct DeviceCapabilityInputs {
    /// Memory the process may actually use, not the memory the device has
    /// installed. A browser tab's budget and an app's jetsam limit both belong
    /// here rather than the hardware figure.
    std::uint64_t usable_memory_bytes = 0;

    /// Cores available to this process for sustained work.
    std::uint32_t usable_core_count = 0;

    /// Whether the platform will report thermal pressure at all. A device that
    /// cannot say it is hot cannot be trusted with the quotas that assume it
    /// will say so.
    bool thermal_reporting_available = false;

    /// Whether a realtime render path — a dedicated audio callback fed across
    /// a shared memory region — can be constructed at all.
    ///
    /// This is the neutral form of a lane-specific fact. A browser shell folds
    /// its own isolation, shared-buffer, and worklet-module observations into
    /// this one boolean before calling; a native shell sets it directly. The
    /// per-lane spellings stay in the shell that observed them, so this header
    /// never has to learn any of them.
    bool realtime_render_available = false;

    /// Precision of the primary pointer.
    PointerPrecision pointer_precision = PointerPrecision::Coarse;

    /// Whether a physical keyboard with modifier keys is attached.
    bool precision_keyboard_available = false;

    constexpr auto operator<=>(const DeviceCapabilityInputs&) const = default;
};

// ── Tier projection ────────────────────────────────────────────────────────

/// Memory floor for `DeviceCapabilityTier::Standard`.
inline constexpr std::uint64_t kStandardTierMemoryBytes = 3ull * 1024 * 1024 * 1024;
/// Memory floor for `DeviceCapabilityTier::Full`.
inline constexpr std::uint64_t kFullTierMemoryBytes = 8ull * 1024 * 1024 * 1024;
/// Core-count floor for `DeviceCapabilityTier::Standard`.
inline constexpr std::uint32_t kStandardTierCoreCount = 4;
/// Core-count floor for `DeviceCapabilityTier::Full`.
inline constexpr std::uint32_t kFullTierCoreCount = 8;

namespace detail {

/// Returns the lower of two tiers.
[[nodiscard]] constexpr DeviceCapabilityTier
min_tier(DeviceCapabilityTier left, DeviceCapabilityTier right) {
    return left < right ? left : right;
}

} // namespace detail

/// Projects observed capabilities onto the tier ladder.
///
/// Each axis independently states the highest rung it can support, and the
/// result is the lowest of those: capability is a weakest-link property, so a
/// device with sixteen cores and half a gigabyte is not a Full-tier device.
/// Structuring it this way keeps each axis's contribution separately
/// reviewable instead of hiding it in a cascade of conditions.
[[nodiscard]] constexpr DeviceCapabilityTier
project_device_capability_tier(DeviceCapabilityInputs inputs) {
    const DeviceCapabilityTier by_memory =
        inputs.usable_memory_bytes >= kFullTierMemoryBytes      ? DeviceCapabilityTier::Full
        : inputs.usable_memory_bytes >= kStandardTierMemoryBytes ? DeviceCapabilityTier::Standard
                                                                 : DeviceCapabilityTier::Constrained;

    const DeviceCapabilityTier by_cores =
        inputs.usable_core_count >= kFullTierCoreCount      ? DeviceCapabilityTier::Full
        : inputs.usable_core_count >= kStandardTierCoreCount ? DeviceCapabilityTier::Standard
                                                             : DeviceCapabilityTier::Constrained;

    // No realtime render path caps the device at the review-and-perform rung,
    // whatever its memory and cores: the rungs above it are defined by being
    // able to render locally.
    const DeviceCapabilityTier by_realtime = inputs.realtime_render_available
                                                 ? DeviceCapabilityTier::Full
                                                 : DeviceCapabilityTier::Constrained;

    // Full-tier quotas assume the system will report heat so the consumer can
    // step down. Without that signal the top rung is not grantable.
    const DeviceCapabilityTier by_thermal_reporting = inputs.thermal_reporting_available
                                                          ? DeviceCapabilityTier::Full
                                                          : DeviceCapabilityTier::Standard;

    // The top rung is precision authoring, which presumes a precise pointer
    // and modifier keys. Neither bars authoring outright, so each caps at
    // Standard rather than Constrained.
    const DeviceCapabilityTier by_pointer = inputs.pointer_precision == PointerPrecision::Fine
                                                ? DeviceCapabilityTier::Full
                                                : DeviceCapabilityTier::Standard;
    const DeviceCapabilityTier by_keyboard = inputs.precision_keyboard_available
                                                 ? DeviceCapabilityTier::Full
                                                 : DeviceCapabilityTier::Standard;

    DeviceCapabilityTier tier = by_memory;
    tier = detail::min_tier(tier, by_cores);
    tier = detail::min_tier(tier, by_realtime);
    tier = detail::min_tier(tier, by_thermal_reporting);
    tier = detail::min_tier(tier, by_pointer);
    tier = detail::min_tier(tier, by_keyboard);
    return tier;
}

// ── Quotas ─────────────────────────────────────────────────────────────────

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
    kQuotaTable[kDeviceCapabilityTierCount][kThermalStateCount] = {
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
[[nodiscard]] constexpr DeviceQuotas device_quotas(DeviceCapabilityTier tier,
                                                   ThermalState thermal) {
    std::size_t tier_index = static_cast<std::size_t>(tier);
    if (tier_index >= kDeviceCapabilityTierCount) {
        tier_index = 0;
    }
    std::size_t thermal_index = static_cast<std::size_t>(thermal);
    if (thermal_index >= kThermalStateCount) {
        thermal_index = kThermalStateCount - 1;
    }
    return detail::kQuotaTable[tier_index][thermal_index];
}

} // namespace pulp::platform
