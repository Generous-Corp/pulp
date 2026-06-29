#pragma once

// Per-binary-unique Objective-C class names for the macOS view / window /
// accessibility layer.
//
// Objective-C class names are process-global. The same NSView / NSObject
// subclass compiled into two Pulp binaries — two plug-ins, or a plug-in plus an
// app — that load into one host makes the objc runtime emit
// "Class X is implemented in both ... One of the duplicates must be removed or
// renamed" and silently lets the first-loaded copy shadow the others. Across
// plug-in versions that shadowing is a latent source of mysterious crashes.
//
// To avoid it, every separately-loaded Pulp binary compiles its own copy of
// this layer with a unique suffix token injected as PULP_VIEW_OBJC_SUFFIX
// (PulpPluginFormats.cmake / PulpAppTargets.cmake derive it from the format /
// app target name). Because those per-binary copies are linked in as direct
// objects, they satisfy the references first and the shared pulp-view-core
// static library's fixed-name copies are never pulled into a shipped binary.
// pulp-view-core itself compiles these sources with no suffix; that fallback is
// only linked into binaries that don't supply their own (e.g. unit tests, which
// are never co-loaded), so it never collides in practice.
//
// When PULP_VIEW_OBJC_SUFFIX is undefined the class names are unchanged.

#ifdef PULP_VIEW_OBJC_SUFFIX

#define PULP_VIEW_OBJC_PASTE2(a, b) a##b
#define PULP_VIEW_OBJC_PASTE(a, b) PULP_VIEW_OBJC_PASTE2(a, b)
#define PULP_VIEW_OBJC_NAME(base) PULP_VIEW_OBJC_PASTE(base, PULP_VIEW_OBJC_SUFFIX)

// Standalone window host (window_host_mac*.mm).
#define PulpView                       PULP_VIEW_OBJC_NAME(PulpView)
#define PulpMetalView                  PULP_VIEW_OBJC_NAME(PulpMetalView)
#define PulpWindowDelegate             PULP_VIEW_OBJC_NAME(PulpWindowDelegate)
#define PulpAppTerminationHandler      PULP_VIEW_OBJC_NAME(PulpAppTerminationHandler)
// Plug-in editor host (plugin_view_host_mac.mm).
#define PulpPluginView                 PULP_VIEW_OBJC_NAME(PulpPluginView)
#define PulpGpuPluginView              PULP_VIEW_OBJC_NAME(PulpGpuPluginView)
// Accessibility elements (plugin_view_host_mac.mm, accessibility_mac.mm,
// text_accessibility_macos.mm).
#define PulpAccessibilityElement       PULP_VIEW_OBJC_NAME(PulpAccessibilityElement)
#define PulpWindowAccessibilityElement PULP_VIEW_OBJC_NAME(PulpWindowAccessibilityElement)
#define PulpTextAccessibilityElement   PULP_VIEW_OBJC_NAME(PulpTextAccessibilityElement)
// Drag-and-drop source (drag_drop_mac.mm).
#define PulpFileDragSource             PULP_VIEW_OBJC_NAME(PulpFileDragSource)

#endif  // PULP_VIEW_OBJC_SUFFIX
