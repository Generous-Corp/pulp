#pragma once

#include <pulp/runtime/result.hpp>
#include <pulp/timeline/automation_curve.hpp>
#include <pulp/timeline/item_id.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <variant>

namespace pulp::timeline {

/// Format-neutral document target for a placed device parameter. The placement
/// ID is a referenced Timeline identity, not a host graph node. The parameter ID
/// is the device's stable host-facing 32-bit ID, scoped by the placement; it is
/// not a registration index, graph port, or Timeline ItemId. Timeline preserves
/// it verbatim, while range and metadata validation belong to the delivery layer.
struct DeviceParameterTarget {
    ItemId device_placement_id;
    std::uint32_t param_id = 0;

    constexpr bool valid() const noexcept {
        return device_placement_id.valid();
    }

    constexpr bool operator==(const DeviceParameterTarget&) const = default;
};

/// The mixer controls a track owns directly, independent of any device it
/// hosts. They are named rather than numbered so a document never has to agree
/// with a host on which control a numeric index meant.
enum class TrackMixerParameter : std::uint8_t {
    Gain,
    Pan,
};

/// The canonical persisted spelling of a mixer parameter, shared by the encoder,
/// the decoder, and every interchange reader so no surface invents its own.
constexpr std::string_view track_mixer_parameter_name(TrackMixerParameter value) noexcept {
    switch (value) {
    case TrackMixerParameter::Gain:
        return "gain";
    case TrackMixerParameter::Pan:
        return "pan";
    }
    return "gain";
}

constexpr std::optional<TrackMixerParameter>
track_mixer_parameter_from_name(std::string_view name) noexcept {
    if (name == "gain")
        return TrackMixerParameter::Gain;
    if (name == "pan")
        return TrackMixerParameter::Pan;
    return std::nullopt;
}

/// Format-neutral document target for one of the owning track's own mixer
/// controls. The track identity is implicit: a lane already lives on exactly one
/// track, so carrying a track ID here would let a document express a lane whose
/// target disagrees with its owner.
struct TrackMixerTarget {
    TrackMixerParameter parameter = TrackMixerParameter::Gain;

    constexpr bool valid() const noexcept {
        switch (parameter) {
        case TrackMixerParameter::Gain:
        case TrackMixerParameter::Pan:
            return true;
        }
        return false;
    }

    constexpr bool operator==(const TrackMixerTarget&) const = default;
};

/// Exhaustive authored-target set. Later target categories extend this variant
/// without changing AutomationLane's factory signature or observer API.
using AutomationTarget = std::variant<DeviceParameterTarget, TrackMixerTarget>;

/// Overload set for visiting an AutomationTarget with **no generic fallback**.
///
/// Adding an alternative to AutomationTarget must not be a silent change.
/// Consumers that dispatch through a generic lambda (`[](const auto&)`, or an
/// `if constexpr` chain with no else) keep compiling and then quietly ignore the
/// new alternative, which is the worse failure: a target that exists in the
/// document but is absent from a census, an export manifest, or a delivery path
/// reads as "nothing was there" rather than as an error. Visiting through this
/// type instead makes a new alternative a compile error at every call site until
/// someone decides what it means.
///
///     std::visit(AutomationTargetCases{
///                    [&](const DeviceParameterTarget& t) { ... },
///                },
///                lane.target());
template <class... Fs> struct AutomationTargetCases : Fs... {
    using Fs::operator()...;
};
template <class... Fs> AutomationTargetCases(Fs...) -> AutomationTargetCases<Fs...>;

/// Guard for code that can only be correct for a known set of alternatives —
/// chiefly `std::get<T>` on a target, which under this module's
/// `-fno-exceptions` build calls `std::terminate` rather than throwing when the
/// alternative does not match. Anything that cannot be expressed as an
/// AutomationTargetCases visit should assert on this count so widening the
/// variant trips at compile time instead of aborting the process at run time.
inline constexpr std::size_t kAutomationTargetAlternativeCount =
    std::variant_size_v<AutomationTarget>;

enum class AutomationLaneErrorCode : std::uint8_t {
    InvalidLaneId,
    InvalidDevicePlacementId,
    InvalidDeviceId = InvalidDevicePlacementId,
};

struct AutomationLaneError {
    AutomationLaneErrorCode code = AutomationLaneErrorCode::InvalidLaneId;
    ItemId lane;
    ItemId related_item;
};

/// Immutable ownership of one authored automation curve and its logical target.
/// Curve values remain in the plugin's plain parameter domain. Document
/// attachment, playback compilation, metadata validation, normalization, and
/// host delivery are separate concerns and intentionally absent from this value.
/// When attached and remapped later, the lane and curve-point IDs are owned
/// identities; target placement IDs are references, and parameter IDs stay verbatim.
class AutomationLane {
  public:
    static runtime::Result<AutomationLane, AutomationLaneError>
    create(ItemId id, AutomationTarget target, AutomationCurve curve);

    ItemId id() const noexcept {
        return id_;
    }
    const AutomationTarget& target() const noexcept {
        return target_;
    }
    const AutomationCurve& curve() const noexcept {
        return curve_;
    }

    AutomationLane with_curve(AutomationCurve replacement) const noexcept;

  private:
    AutomationLane(ItemId id, AutomationTarget target, AutomationCurve curve) noexcept;

    ItemId id_;
    AutomationTarget target_;
    AutomationCurve curve_;
};

static_assert(std::is_nothrow_copy_constructible_v<AutomationLane>);
static_assert(std::is_nothrow_copy_assignable_v<AutomationLane>);
static_assert(std::is_nothrow_move_constructible_v<AutomationLane>);
static_assert(std::is_nothrow_move_assignable_v<AutomationLane>);

} // namespace pulp::timeline
