#include <pulp/platform/popup_menu.hpp>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

// macOS has the real impl in popup_menu_mac.mm; iOS / Windows / Linux /
// Android fall through to these stubs. iOS specifically: there is no
// native UIKit popup-menu wiring yet, and `widget_bridge.cpp`
// references `PopupMenu::show` unconditionally — without the stub, the
// AUv3 .appex link fails with "Undefined symbols for architecture
// arm64". (#316 follow-up: real iOS impl via UIMenu/UIMenuController.)
#if !(defined(__APPLE__) && TARGET_OS_OSX)

namespace pulp::platform {

std::optional<int> PopupMenu::show(float, float) const {
    return std::nullopt;
}

std::optional<int> PopupMenu::show_at_view(void*) const {
    return std::nullopt;
}

} // namespace pulp::platform

#endif
