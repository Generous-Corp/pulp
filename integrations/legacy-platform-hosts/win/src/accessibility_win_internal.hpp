#pragma once

namespace pulp::view {

class View;

enum class UiaAccessibilityEvent {
    value_changed,
    focus_changed,
    name_changed,
};

/// Raise one UIA notification through the session that owns `handle`.
/// Provider lookup and all COM lifetime leases remain private to the main UIA
/// implementation; callers do not depend on concrete provider types.
void raise_uia_accessibility_event(void* handle, View& target,
                                   UiaAccessibilityEvent event);

} // namespace pulp::view
