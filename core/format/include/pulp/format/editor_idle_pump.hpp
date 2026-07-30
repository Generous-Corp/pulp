#pragma once

#include <pulp/format/view_bridge.hpp>
#include <pulp/view/scripted_ui.hpp>

#include <atomic>
#include <functional>

namespace pulp::format {

/// Build the per-vsync idle pump for a bridge. GPU hosts invoke it once per
/// display-link tick to drain host automation, consume restore/reload edges,
/// and poll scripted UI work while the editor is embedded. Captures the bridge
/// by pointer; the host MUST drop this callback (via `detach()` /
/// destruction) before the bridge is destroyed — every adapter resets its
/// host before `bridge.close()`.
inline std::function<void()> make_editor_idle_pump(ViewBridge& bridge) {
    auto* bridge_ptr = &bridge;
    // The display link may already have dispatched a main-queue tick when host
    // teardown begins. The liveness token makes that tick a no-op after the
    // bridge is gone; adapters must still detach their host before close.
    auto alive = bridge.alive_token();
    return [bridge_ptr, alive]() {
        if (!alive->load(std::memory_order_acquire)) return;
        if (!bridge_ptr->owner_is_alive()) return;
        bridge_ptr->pump_store_listeners();
        if (bridge_ptr->poll_state_restore()) {
            if (auto* v = bridge_ptr->view()) v->request_repaint();
        }
        if (bridge_ptr->poll_editor_reload()) {
            if (auto* v = bridge_ptr->view()) v->request_repaint();
        }
        if (auto* session = bridge_ptr->scripted_ui()) {
            session->poll();
        }
    };
}

/// Compatibility spelling retained for downstream adapter code. New code
/// should use make_editor_idle_pump(); both names have identical behavior.
inline std::function<void()> make_scripted_idle_pump(ViewBridge& bridge) {
    return make_editor_idle_pump(bridge);
}

} // namespace pulp::format
