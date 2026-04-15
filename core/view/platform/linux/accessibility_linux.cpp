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

// Cross-platform entry — see accessibility_provider.hpp. Today these
// route to the existing AT-SPI stub; a future commit wires atk_bridge
// + GdkDisplay signalling per window.
void* init_accessibility(View& root, void* /*gdk_surface*/) {
    init_atspi_accessibility(root);
    return reinterpret_cast<void*>(static_cast<uintptr_t>(1));
}

void shutdown_accessibility(void* /*handle*/) {
    runtime::log_info("Linux AT-SPI: shutdown (stub)");
    // TODO: g_object_unref the AtkObject root + unregister D-Bus name.
}

void accessibility_tree_changed(void* /*handle*/) {
    // TODO: g_signal_emit_by_name(root, "children-changed::add", ...)
}

} // namespace pulp::view

#endif // __linux__
