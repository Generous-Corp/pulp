#include <pulp/platform/popup_menu.hpp>

#if !defined(__APPLE__)

namespace pulp::platform {

std::optional<int> PopupMenu::show(float, float) const {
    return std::nullopt;
}

std::optional<int> PopupMenu::show_at_view(void*) const {
    return std::nullopt;
}

} // namespace pulp::platform

#endif
