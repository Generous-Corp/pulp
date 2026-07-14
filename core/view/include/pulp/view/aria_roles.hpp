#pragma once

// ARIA role token -> View::AccessRole.
//
// The JS/html-compat layer calls setAccessibilityRole(id, role) when a script
// does `el.setAttribute('role', ...)`. This is the single parser for that
// token; it lives in a header (rather than inside
// widget_bridge/accessibility_api.cpp) so it is unit-testable without spinning
// up a QuickJS engine, and so any future non-bridge caller reuses the same
// table.
//
// Previously the bridge collapsed every role outside a 7-value enum onto
// `group` — button, link, listbox, menu, tab, dialog, navigation all became
// "group", so a screen-reader user heard "group" for a button. Now the enum
// covers the roles Pulp's widgets actually are, and only genuinely
// container-ish ARIA roles (region, navigation, form, ...) land on `group`.

#include <pulp/view/view.hpp>

#include <string_view>

namespace pulp::view {

/// Map an ARIA role token to a Pulp AccessRole.
///
/// Unknown, empty, `none` and `presentation` map to AccessRole::none, which
/// removes the view from the accessibility tree (ARIA `presentation` semantics
/// and the "clear the role" path both want that).
///
/// Roles whose ARIA meaning is a generic container (`region`, `navigation`,
/// `form`, `group`, `dialog`'s non-modal cousins, ...) map to `group`. Roles
/// Pulp has no widget for and no platform mapping for (`tree`, `grid`,
/// `toolbar`, ...) also map to `group` rather than being dropped: the author
/// said "expose this", and a generic container is a truthful superset. That is
/// a documented approximation, not silent breakage.
inline View::AccessRole access_role_from_aria(std::string_view role) {
    using R = View::AccessRole;
    if (role.empty() || role == "none" || role == "presentation") return R::none;

    if (role == "slider")                        return R::slider;
    if (role == "switch")                        return R::toggle;
    if (role == "checkbox")                      return R::checkbox;
    if (role == "radio")                         return R::radio;
    if (role == "button")                        return R::button;
    if (role == "link")                          return R::link;
    if (role == "textbox" || role == "searchbox" ||
        role == "text_field")                    return R::text_field;
    if (role == "combobox")                      return R::combo_box;
    if (role == "list" || role == "listbox")     return R::list;
    if (role == "listitem" || role == "option")  return R::list_item;
    if (role == "table" || role == "grid" ||
        role == "treegrid")                      return R::table;
    if (role == "row")                           return R::row;
    if (role == "cell" || role == "gridcell" ||
        role == "columnheader" ||
        role == "rowheader")                     return R::cell;
    if (role == "tab")                           return R::tab;
    if (role == "tablist")                       return R::tab_list;
    if (role == "menu" || role == "menubar")     return R::menu;
    if (role == "menuitem" ||
        role == "menuitemcheckbox" ||
        role == "menuitemradio")                 return R::menu_item;
    if (role == "progressbar")                   return R::progress_bar;
    if (role == "meter")                         return R::meter;
    if (role == "dialog" || role == "alertdialog") return R::dialog;
    if (role == "heading")                       return R::heading;
    if (role == "scrollbar")                     return R::scroll_bar;
    if (role == "img" || role == "image")        return R::image;
    if (role == "text" || role == "paragraph" ||
        role == "caption")                       return R::label;

    // Container-ish and not-yet-modeled roles: expose as a generic group.
    return R::group;
}

}  // namespace pulp::view
