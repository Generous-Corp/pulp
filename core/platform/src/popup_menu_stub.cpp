#include <pulp/platform/popup_menu.hpp>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

// macOS has a native popup_menu_mac.mm impl. iOS has no native popover
// menu impl yet (UIMenu / UIContextMenuInteraction wiring is a follow-
// up), so the link step for any iOS bundle that pulls pulp-view-core
// fails on PopupMenu::show / show_at_view. Provide a nullopt stub for
// iOS and every other non-mac platform so the symbols exist; callers
// treat nullopt as "menu unsupported / user dismissed".
#if !defined(__APPLE__) || (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)

namespace pulp::platform {

std::optional<int> PopupMenu::show(float, float) const {
    return std::nullopt;
}

std::optional<int> PopupMenu::show_at_view(void*) const {
    return std::nullopt;
}

} // namespace pulp::platform

#endif
