#pragma once

#include <pulp/timeline/assets.hpp>
#include <pulp/timeline/item_id.hpp>

#include <bit>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace pulp::timeline {

/** @addtogroup timeline_model
 * @{
 */

/// Authored position of a device relative to the track fader.
enum class DeviceChainPosition : std::uint8_t {
    /// Runs before the authored track fader.
    PreFader,
    /// Runs after the authored track fader.
    PostFader
};

/// Static signal-domain contract of one device slot.
enum class DeviceSlotKind : std::uint8_t {
    /// Accepts events and emits events.
    EventToEvent,
    /// Accepts events and emits audio.
    EventToAudio,
    /// Accepts audio and emits audio.
    AudioToAudio
};

/// Format-neutral namespace of a durable device binding key.
enum class DeviceKind : std::uint8_t {
    /// Has no resolver binding yet.
    Unresolved,
    /// Resolves through Pulp's built-in device namespace.
    BuiltIn,
    /// Resolves through an external device namespace.
    External,
    /// Resolves through a generated-device namespace.
    Generated
};

/// Maximum UTF-8 byte length of a durable binding key.
inline constexpr std::size_t kMaximumDeviceBindingKeyBytes = 1024;

/// Authored device configuration excluding placement identity and opaque state.
struct DeviceConfiguration {
    /// Authored position relative to the track fader.
    DeviceChainPosition position = DeviceChainPosition::PreFader;
    /// Static input/output signal-domain contract.
    DeviceSlotKind slot_kind = DeviceSlotKind::AudioToAudio;
    /// Namespace used to interpret `binding_key`.
    DeviceKind device_kind = DeviceKind::Unresolved;
    /// Format-neutral resolver key; empty only for `Unresolved`.
    std::string binding_key;
    /// Whether playback bypasses this declaration.
    bool bypassed = false;
    /// Exact IEEE-754 binary32 bits for the canonical [0, 1] wet fraction.
    std::uint32_t wet_dry_bits = std::bit_cast<std::uint32_t>(1.0f);

    /// Returns whether enums, binding key, and wet/dry value are canonical.
    bool valid() const noexcept {
        switch (position) {
        case DeviceChainPosition::PreFader:
        case DeviceChainPosition::PostFader:
            break;
        default:
            return false;
        }
        switch (slot_kind) {
        case DeviceSlotKind::EventToEvent:
        case DeviceSlotKind::EventToAudio:
        case DeviceSlotKind::AudioToAudio:
            break;
        default:
            return false;
        }
        switch (device_kind) {
        case DeviceKind::Unresolved:
            if (!binding_key.empty())
                return false;
            break;
        case DeviceKind::BuiltIn:
        case DeviceKind::External:
        case DeviceKind::Generated:
            if (binding_key.empty())
                return false;
            break;
        default:
            return false;
        }
        if (binding_key.size() > kMaximumDeviceBindingKeyBytes)
            return false;
        for (const unsigned char byte : binding_key)
            if (byte < 0x20 || byte == 0x7f)
                return false;
        const auto wet_dry = std::bit_cast<float>(wet_dry_bits);
        return std::isfinite(wet_dry) && wet_dry >= 0.0f && wet_dry <= 1.0f;
    }

    auto operator<=>(const DeviceConfiguration&) const = default;
};

/// Durable declaration for one logical placement in a Track device chain.
///
/// Runtime instances, graph-node identities, plugin formats, and platform
/// metadata are deliberately absent. `binding_key` is interpreted only by a
/// higher-level resolver, and `state_ref` names content without importing a
/// package store into Timeline.
struct DevicePlacement {
    ItemId id;
    /// Authored resolver and signal-domain declaration.
    DeviceConfiguration configuration;
    /// Optional content identity of opaque device state.
    std::optional<ContentHash> state_ref;

    /// Returns whether identity, declaration, and optional state hash are valid.
    bool valid() const noexcept {
        return id.valid() && configuration.valid() &&
               (!state_ref || state_ref->valid());
    }

    auto operator<=>(const DevicePlacement&) const = default;
};

/// @}

} // namespace pulp::timeline
