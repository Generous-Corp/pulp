// Linux AT-SPI accessibility provider
// Stub implementation — provides the interface surface for future implementation.
//
// When fully implemented, this will:
// - Use AT-SPI2 (Assistive Technology Service Provider Interface) via D-Bus
// - Register each View with an AccessRole as an AT-SPI accessible object
// - Map AccessRole to ATK roles (ATK_ROLE_SLIDER, ATK_ROLE_TOGGLE_BUTTON, etc.)
// - Expose access_label as accessible name, access_value as accessible value
// - Emit state-change and value-change signals for screen readers (Orca)
//
// Dependencies: libatspi2 (AT-SPI2), libdbus-1
// Install: apt install libatspi2.0-dev on Debian/Ubuntu

#if defined(__linux__) && !defined(__ANDROID__)

#include <pulp/view/accessibility_provider.hpp>
#include <pulp/view/view.hpp>
#include <pulp/runtime/log.hpp>

// PULP_HAS_ATSPI is set by CMake when atk-bridge-2.0 is found via
// pkg-config. When unset, the init path stays a no-op so Linux distros
// without the bridge (minimal / headless CI images) still link.
#ifdef PULP_HAS_ATSPI
extern "C" {
int  atk_bridge_adaptor_init(int* argc, char*** argv);
void atk_bridge_adaptor_cleanup(void);
}
#endif

namespace pulp::view {

// Map Pulp AccessRole to ATK role constants
// These match the AtkRole enum from atk/atk.h
static int access_role_to_atk_role(View::AccessRole role) {
    switch (role) {
        case View::AccessRole::slider: return 34;  // ATK_ROLE_SLIDER
        case View::AccessRole::toggle: return 42;  // ATK_ROLE_TOGGLE_BUTTON
        case View::AccessRole::label:  return 24;  // ATK_ROLE_LABEL
        case View::AccessRole::group:  return 35;  // ATK_ROLE_PANEL (container)
        case View::AccessRole::meter:  return 33;  // ATK_ROLE_PROGRESS_BAR
        case View::AccessRole::image:  return 21;  // ATK_ROLE_IMAGE
        default: return 66; // ATK_ROLE_UNKNOWN
    }
}

// Stub: Initialize AT-SPI accessibility for a view tree
void init_atspi_accessibility(View& /*root*/) {
    runtime::log_info("Linux AT-SPI: stub initialized");
    // TODO: implement AtkObject for each accessible View
    // TODO: register with AT-SPI2 registry via D-Bus
    // TODO: emit state-change signals on focus/value changes
    // TODO: support AtkText for TextEditor widgets
    // TODO: support AtkValue for Knob/Fader/Slider widgets
}

// Cross-platform entry — see accessibility_provider.hpp.
//
// Phase 1 (this commit, issue #248): when CMake finds atk-bridge-2.0
// via pkg-config we register the bridge with D-Bus so Orca + other
// AT-SPI clients can discover the process at all. AtkObject-per-widget
// creation lands when an AtkObject subclass for View is added next.
// Without PULP_HAS_ATSPI we keep the pre-#248 no-op so headless/minimal
// Linux installations still load pulp-view.
void* init_accessibility(View& root, void* /*gdk_surface*/) {
    init_atspi_accessibility(root);
#ifdef PULP_HAS_ATSPI
    // atk_bridge_adaptor_init takes argc/argv the same way glib does
    // for main-thread bootstrap; pass nullptrs so it uses the process's
    // existing GLib main context. Safe to call twice — the bridge
    // refuses re-init with a warning rather than crashing.
    int argc = 0;
    char** argv = nullptr;
    if (atk_bridge_adaptor_init(&argc, &argv) != 0) {
        runtime::log_warn("Linux AT-SPI: atk_bridge_adaptor_init failed; "
                          "screen readers may not see this process");
    } else {
        runtime::log_info("Linux AT-SPI: bridge registered with D-Bus");
    }
    return reinterpret_cast<void*>(static_cast<uintptr_t>(1));
#else
    return reinterpret_cast<void*>(static_cast<uintptr_t>(1));
#endif
}

void shutdown_accessibility(void* /*handle*/) {
#ifdef PULP_HAS_ATSPI
    atk_bridge_adaptor_cleanup();
    runtime::log_info("Linux AT-SPI: bridge cleaned up");
#else
    runtime::log_info("Linux AT-SPI: shutdown (stub)");
#endif
}

void accessibility_tree_changed(void* /*handle*/) {
    // TODO: g_signal_emit_by_name(root, "children-changed::add", ...)
}

} // namespace pulp::view

#endif // __linux__
