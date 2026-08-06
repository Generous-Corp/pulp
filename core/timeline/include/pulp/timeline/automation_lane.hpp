#pragma once

#include <pulp/runtime/result.hpp>
#include <pulp/timeline/automation_curve.hpp>
#include <pulp/timeline/item_id.hpp>
#include <pulp/timeline/parameter_target.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <variant>

namespace pulp::timeline {

/** @addtogroup timeline_model
 * @{
 */

/// Exhaustive authored-target set. Adding a target category extends this
/// variant without changing AutomationLane's factory signature or observer API.
using AutomationTarget = ParameterTarget;

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
    kParameterTargetAlternativeCount;

/// Validation failures returned when constructing an AutomationLane.
enum class AutomationLaneErrorCode : std::uint8_t {
    InvalidLaneId,
    InvalidDevicePlacementId,
    InvalidDeviceId = InvalidDevicePlacementId,
};

/// Automation lane failure with the lane and related target identity.
struct AutomationLaneError {
    AutomationLaneErrorCode code = AutomationLaneErrorCode::InvalidLaneId;
    ItemId lane;
    ItemId related_item;
};

/// Immutable ownership of one authored automation curve and its logical target.
/// Curve values remain in the plugin's plain parameter domain. Document
/// attachment, playback compilation, metadata validation, normalization, and
/// host delivery are separate concerns and intentionally absent from this value.
/// During attachment and remapping, lane and curve-point IDs are owned
/// identities; target placement IDs are references, and parameter IDs stay verbatim.
class AutomationLane {
  public:
    /// Validates and constructs a lane owning `curve` for `target`.
    ///
    /// The lane identity and any referenced placement identity must be valid.
    static runtime::Result<AutomationLane, AutomationLaneError>
    create(ItemId id, AutomationTarget target, AutomationCurve curve);

    /// Returns the lane's stable document identity.
    ItemId id() const noexcept {
        return id_;
    }
    /// Returns the format-neutral target stored by this lane.
    const AutomationTarget& target() const noexcept {
        return target_;
    }
    /// Returns the immutable authored curve.
    const AutomationCurve& curve() const noexcept {
        return curve_;
    }

    /// Returns a lane snapshot with `replacement`, preserving identity and target.
    ///
    /// The original lane and its curve remain valid and unchanged.
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

/// @}

} // namespace pulp::timeline
