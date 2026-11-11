#pragma once

#include <string>
#include <vector>
#include <functional>
#include <optional>

#include <pulp/platform/dialog_session.hpp>
#include <pulp/platform/main_thread.hpp>
#include <pulp/platform/native_handle.hpp>

namespace pulp::platform {

// Native popup/context menu
class PopupMenu {
public:
    struct Item {
        int id = 0;
        std::string label;
        bool enabled = true;
        bool checked = false;
        bool is_separator = false;

        static Item separator() { return {0, "", true, false, true}; }
    };

    PopupMenu() = default;

    void add_item(int id, const std::string& label, bool enabled = true, bool checked = false) {
        items_.push_back({id, label, enabled, checked, false});
    }

    void add_separator() {
        items_.push_back(Item::separator());
    }

    const std::vector<Item>& items() const { return items_; }

    // Show at screen coordinates (top-left origin on the main screen) and
    // block until the menu closes. Returns the chosen item's id, or nullopt
    // when it was dismissed. Prefer show_menu_async() in editors: it does not
    // block the caller and can be anchored to a rectangle.
    std::optional<int> show(float x, float y) const;

    // As show(), below the bottom-left corner of a view (an NSView* on macOS).
    std::optional<int> show_at_view(void* native_view_handle) const;

private:
    std::vector<Item> items_;
};

// ── Asynchronous anchored menus ────────────────────────────────────────

/// Straight (not premultiplied) colour, components in [0, 1].
struct MenuColour {
    float r = 0, g = 0, b = 0, a = 1;
};

struct MenuItem {
    int id = 0;
    std::string label;
    bool enabled = true;
    bool checked = false;
    bool separator = false;
    /// Text colour for this row; unset keeps the system colour.
    std::optional<MenuColour> text_colour;
    /// Non-empty makes this row open a submenu (its own id is never chosen).
    std::vector<MenuItem> submenu;

    static MenuItem make_separator() {
        MenuItem item;
        item.separator = true;
        return item;
    }
};

struct MenuModel {
    std::vector<MenuItem> items;
    /// With MenuPlacement::over_selected, the row placed over the anchor.
    std::optional<int> selected_id;
};

/// Where a menu opens: a rectangle in the view's own coordinates, in points,
/// with the origin at the view's top-left corner.
struct MenuAnchor {
    NativeViewHandle view = nullptr;
    float x = 0, y = 0, width = 0, height = 0;
};

enum class MenuPlacement {
    /// The menu's top-left corner at the anchor's bottom-left corner.
    below_anchor,
    /// The selected row over the anchor, as a pop-up button does.
    over_selected,
};

/// A point in the view's own coordinate space (AppKit's, so y grows upward
/// unless the view is flipped).
struct MenuPoint {
    float x = 0, y = 0;
};

/// The point a menu is positioned at for `anchor` in a view of
/// `view_height` points: the anchor's bottom-left corner for below_anchor,
/// its top-left corner for over_selected.
MenuPoint menu_location_in_view(const MenuAnchor& anchor, MenuPlacement placement,
                                bool view_is_flipped, float view_height);

/// Open `model` at `anchor` and return at once. The menu opens on the next
/// run-loop pass, so the calling event handler returns first. It is at least
/// as wide as the anchor, and the click that dismisses it is consumed by the
/// menu. `done` runs once on the main thread with the chosen id, or nullopt
/// when the menu was dismissed or could not open; it never runs if `scope`
/// is destroyed first, which also closes the menu. Main thread only.
DialogSession show_menu_async(MainThreadScope& scope, const MenuAnchor& anchor, MenuModel model,
                              MenuPlacement placement,
                              std::function<void(std::optional<int>)> done);

/// Replaces the native menu (for tests and hosts that draw their own). The
/// presenter must call `respond` once with the chosen id or nullopt; `cancel`
/// closes it. An empty presenter restores the native menu.
struct MenuPresenter {
    std::function<void(const MenuAnchor& anchor, const MenuModel& model, MenuPlacement placement,
                       std::function<void(std::optional<int>)> respond)>
        present;
    std::function<void()> cancel;
};
void set_menu_presenter(MenuPresenter presenter);

#if defined(__APPLE__)
/// macOS: the NSMenu (autoreleased) that show_menu_async presents for
/// `model`, at least `minimum_width` points wide. Choosing a row records its
/// id in the menu; native_menu_chosen_id() reads it back. Exposed so the
/// structure can be checked without opening a menu.
void* build_native_menu(const MenuModel& model, float minimum_width);
/// The id recorded when a row of a build_native_menu() menu (or one of its
/// submenus) was chosen, or nullopt.
std::optional<int> native_menu_chosen_id(void* menu);
#endif

} // namespace pulp::platform
