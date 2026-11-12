// Popup windows: placement arithmetic and names. The window itself lives in
// platform/mac/popup_window_mac.mm (a stub elsewhere).

#include <pulp/platform/popup_window.hpp>

#include <algorithm>

namespace pulp::platform {

const char* to_string(PopupDismissReason reason) noexcept {
    switch (reason) {
    case PopupDismissReason::client:
        return "client";
    case PopupDismissReason::outside_click:
        return "outside_click";
    case PopupDismissReason::escape:
        return "escape";
    case PopupDismissReason::focus_lost:
        return "focus_lost";
    case PopupDismissReason::parent_closed:
        return "parent_closed";
    }
    return "unknown";
}

PopupResignAction popup_resign_action(bool popup_is_key, bool parent_is_key, int reclaims) {
    // A bound, so two windows that keep handing key to each other cannot keep
    // the popup alive forever.
    constexpr int kMaxReclaims = 3;
    if (popup_is_key)
        return PopupResignAction::stay_open;
    if (parent_is_key && reclaims < kMaxReclaims)
        return PopupResignAction::take_key_back;
    return PopupResignAction::close_focus_lost;
}

PopupRect place_popup(const PopupRect& anchor, float width, float height, const PopupRect& visible,
                      PopupPlacement placement) {
    PopupRect frame;
    frame.width = std::max(width, anchor.width);
    frame.height = height;

    // Horizontal: start at the anchor, then keep it on the screen.
    frame.x = anchor.x;
    const float right = visible.x + visible.width;
    if (frame.x + frame.width > right)
        frame.x = right - frame.width;
    if (frame.x < visible.x)
        frame.x = visible.x;

    const float below_y = anchor.y + anchor.height;
    const float above_y = anchor.y - height;
    const float bottom = visible.y + visible.height;
    switch (placement) {
    case PopupPlacement::below:
        frame.y = below_y;
        break;
    case PopupPlacement::above:
        frame.y = above_y;
        break;
    case PopupPlacement::auto_flip: {
        const float room_below = bottom - below_y;
        const float room_above = anchor.y - visible.y;
        if (room_below >= height)
            frame.y = below_y;
        else if (room_above >= height)
            frame.y = above_y;
        else
            frame.y = room_below >= room_above ? below_y : above_y;
        break;
    }
    }
    // A popup taller than its side of the screen stays on the screen.
    if (frame.y + frame.height > bottom)
        frame.y = bottom - frame.height;
    if (frame.y < visible.y)
        frame.y = visible.y;
    return frame;
}

} // namespace pulp::platform
