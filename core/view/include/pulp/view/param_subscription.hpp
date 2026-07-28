// param_subscription.hpp - one JS subscription to a parameter's movement.
//
// Split out of widget_bridge.hpp so adding to the paramchange machinery does
// not widen that header's recompile blast radius (see the registrars.hpp
// extraction note in hotspot_size_guard.json).

#pragma once

#include <pulp/state/parameter.hpp>

#include <cstdint>
#include <string>

namespace pulp::view {

/// A live `onParamChanged` registration. The handler itself stays in JS —
/// CHOC's NativeFunction cannot carry a JSValue — so this holds only what the
/// native side needs to decide whether to dispatch, and the id that keys the
/// handler in the engine's `__callbacks__` table.
struct ParamSubscription {
    /// Handle returned to JS. Monotonic and never reused, so a stale
    /// offParamChanged() from a torn-down view cannot cancel a live
    /// subscription that happened to land in the same slot.
    /// Zero is never issued, and marks an entry tombstoned by a handler that
    /// unsubscribed mid-dispatch; those compact at the end of the pass.
    std::uint32_t id = 0;
    state::ParamID param_id = 0;  ///< resolved once at subscribe time
    std::string param_name;       ///< echoed in the payload

    /// Seeded from the store at subscribe time so subscribing is not itself
    /// reported as a change. NaN would defeat that — NaN != NaN would dispatch
    /// on the first frame.
    float last_value = 0.0f;
    float last_modulated = 0.0f;
};

} // namespace pulp::view
