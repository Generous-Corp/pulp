#pragma once

/// Return the backing scale for the native AAX parent (NSView* on macOS).
/// A null parent is valid because AAX may query GetViewSize before attaching
/// the view; the platform implementation uses the main display then.
namespace pulp::format::aax {
#if defined(__APPLE__)
float aax_display_scale(void* native_parent) noexcept;
#else
inline float aax_display_scale(void*) noexcept { return 1.0f; }
#endif
}
