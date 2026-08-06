#pragma once

#include <pulp/timeline/item_id.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

namespace pulp::timeline {

/** @addtogroup timeline_model
 * @{
 */

/// Format-neutral document target for a placed device parameter. The placement
/// ID is a referenced Timeline identity, not a host graph node. The parameter ID
/// is the device's stable host-facing 32-bit ID, scoped by the placement; it is
/// not a registration index, graph port, or Timeline ItemId. Timeline preserves
/// it verbatim, while range and metadata validation belong to the delivery layer.
struct DeviceParameterTarget {
    ItemId device_placement_id;
    std::uint32_t param_id = 0;

    /// Returns whether the referenced device-placement identity is nonzero.
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

/// Parses the canonical persisted mixer-parameter spelling.
/// @return The corresponding parameter, or `std::nullopt` for an unknown name.
constexpr std::optional<TrackMixerParameter>
track_mixer_parameter_from_name(std::string_view name) noexcept {
    if (name == "gain")
        return TrackMixerParameter::Gain;
    if (name == "pan")
        return TrackMixerParameter::Pan;
    return std::nullopt;
}

/// Format-neutral document target for one of the owning track's own mixer
/// controls. The track identity is implicit: a target already lives on exactly
/// one track, so carrying a track ID here would let a document express a lane or
/// route whose target disagrees with its owner.
struct TrackMixerTarget {
    TrackMixerParameter parameter = TrackMixerParameter::Gain;

    /// Returns whether the parameter names a supported track mixer control.
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

/// Exhaustive set of document parameters an authored control can address.
///
/// One vocabulary serves every consumer that names a parameter — automation
/// lanes and modulation routes today — because "which parameter" is addressing
/// rather than a property of what writes there. Consumers alias this under
/// their own name and visit it through their own no-fallback overload set, so
/// adding an alternative here is a compile error at every one of them.
using ParameterTarget = std::variant<DeviceParameterTarget, TrackMixerTarget>;

/// Number of alternatives in ParameterTarget.
///
/// Guard for code that can only be correct for a known set of alternatives,
/// chiefly `std::get<T>` on a target, which under this module's
/// `-fno-exceptions` build calls `std::terminate` rather than throwing on a
/// mismatch. Anything that cannot be expressed as an exhaustive visit asserts
/// on this count, so widening the variant trips at compile time instead of
/// aborting the process at run time.
inline constexpr std::size_t kParameterTargetAlternativeCount =
    std::variant_size_v<ParameterTarget>;

/// @}

} // namespace pulp::timeline
