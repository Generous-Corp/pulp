#pragma once

#include <pulp/format/processor.hpp>
#include <pulp/format/view_bridge.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

namespace pulp::format::au::editor_resize_detail {

/// Install the AU v2 Cocoa editor's plugin-initiated resize transaction.
///
/// AU v2 has no `request_resize` host callback. Its Cocoa contract gives the
/// plugin an NSView whose frame is the editor's natural size, so the adapter
/// updates that returned view through `native_resize`. The bridge publishes the
/// new preferred size before the native call because Logic may synchronously
/// query the Audio Unit while responding to the frame change. A rejected or
/// clamped native resize restores the prior preferred size; `commit_viewport`
/// runs only after the returned view accepted the exact dimensions.
///
/// `acquire_lifetime` runs before any captured bridge/host access. It must
/// reject a stale, off-thread, or nested request and return a lease that keeps
/// the editor ownership alive through both native and bridge commits. The
/// caller must still remove this owner-scoped handler during editor teardown.
inline void install_editor_resize_handler(
    Processor& processor, const void* editor_owner, ViewBridge& bridge,
    std::function<std::shared_ptr<void>()> acquire_lifetime,
    std::function<bool(uint32_t, uint32_t)> native_resize,
    std::function<void(uint32_t, uint32_t)> commit_viewport,
    std::function<bool()> is_active = {}) {
    processor.set_editor_resize_handler(
        editor_owner,
        [&bridge, acquire_lifetime = std::move(acquire_lifetime),
         native_resize = std::move(native_resize),
         commit_viewport = std::move(commit_viewport)](
            uint32_t width, uint32_t height) {
            // Processor copies handlers out of its side-table lock before
            // invoking them. An owner can therefore clear the registered
            // handler while an already-copied callback is still pending. The
            // AU adapter's lease must reject stale/off-thread/reentrant calls
            // and keep the editor ownership alive for this entire transaction
            // before this callback dereferences the captured bridge.
            if (!acquire_lifetime) return false;
            auto lifetime = acquire_lifetime();
            if (!lifetime) return false;
            if (!native_resize) return false;
            const bool accepted = pulp::format::detail::negotiate_preferred_size(
                bridge, width, height, native_resize);
            if (!accepted) return false;
            if (commit_viewport) commit_viewport(width, height);
            return true;
        },
        std::move(is_active));
}

}  // namespace pulp::format::au::editor_resize_detail
