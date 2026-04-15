#pragma once

// Cross-platform entry point for the OS-native accessibility provider.
// Workstream 04 slices 4.1 (Windows UIA) + 4.2 (Linux AT-SPI).
//
// Each platform ships its own `accessibility_<platform>.cpp` that
// defines `init_accessibility(View&, void* native_window)`. The header
// keeps the call site cross-platform so widgets and WindowHost can
// invoke a single function; the platform TU handles the OS nuance.
//
// The "provider" concept matches the Windows UIA / Linux AT-SPI
// architecture: the OS asks the plugin window for a provider object,
// which then walks the View tree and surfaces each View's access_role /
// access_label / access_value / access_bounds to the screen reader.
//
// macOS / iOS use NSAccessibility / UIAccessibility on the view side
// directly and do not route through init_accessibility().

#include <cstdint>

namespace pulp::view {

class View;

/// Initialize the OS accessibility bridge for this root view. On
/// Windows this attaches a UIA fragment-root to the HWND; on Linux it
/// registers the root AtkObject with AT-SPI. Called once per window on
/// the main thread after the View tree is built.
///
/// @param root           Root of the View hierarchy exposed to a11y.
/// @param native_window  Platform window handle. HWND on Windows;
///                       pointer to the GDK/Wayland surface on Linux.
///                       Ignored on platforms that derive it another way.
/// @return  Opaque handle for later teardown via shutdown_accessibility;
///          nullptr if the platform has no provider.
void* init_accessibility(View& root, void* native_window);

/// Release any OS-side resources acquired in init_accessibility.
void shutdown_accessibility(void* handle);

/// Request the provider to re-walk the View tree. Call after large
/// structural changes (tabs switched, panels added, etc.). Focus /
/// value updates don't need this — those fire per-property events.
void accessibility_tree_changed(void* handle);

} // namespace pulp::view
