#pragma once

#include <pulp/format/processor.hpp>
#include <pulp/format/view_bridge.hpp>

#include <cstdint>
#include <functional>
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
/// The caller must remove this owner-scoped handler before `bridge`,
/// `native_resize`, or `commit_viewport` captures are destroyed.
inline void install_editor_resize_handler(
    Processor& processor, const void* editor_owner, ViewBridge& bridge,
    std::function<bool(uint32_t, uint32_t)> native_resize,
    std::function<void(uint32_t, uint32_t)> commit_viewport) {
    processor.set_editor_resize_handler(
        editor_owner,
        [&bridge, native_resize = std::move(native_resize),
         commit_viewport = std::move(commit_viewport)](
            uint32_t width, uint32_t height) {
            if (!native_resize) return false;
            const bool accepted = pulp::format::detail::negotiate_preferred_size(
                bridge, width, height, native_resize);
            if (!accepted) return false;
            if (commit_viewport) commit_viewport(width, height);
            return true;
        });
}

}  // namespace pulp::format::au::editor_resize_detail
