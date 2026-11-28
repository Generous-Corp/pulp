#include "accessibility_win_internal.hpp"

#ifdef _WIN32

#include <pulp/view/accessibility_provider.hpp>

namespace pulp::view {

void notify_accessibility_value_changed(void* handle, View& target) {
    raise_uia_accessibility_event(
        handle, target, UiaAccessibilityEvent::value_changed);
}

void notify_accessibility_focus_changed(void* handle, View& target) {
    raise_uia_accessibility_event(
        handle, target, UiaAccessibilityEvent::focus_changed);
}

void notify_accessibility_name_changed(void* handle, View& target) {
    raise_uia_accessibility_event(
        handle, target, UiaAccessibilityEvent::name_changed);
}

} // namespace pulp::view

#endif
