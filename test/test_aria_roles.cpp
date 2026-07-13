// Tests for the ARIA role token -> View::AccessRole table shared by the JS
// widget bridge (core/view/src/widget_bridge/accessibility_api.cpp).
//
// Before the role vocabulary expanded, every ARIA role outside a 7-value enum
// collapsed to `group` — a `role="button"` element announced as "group", and a
// `role="combobox"` element could not be represented at all. These cases lock
// the expanded table.

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/aria_roles.hpp>

using pulp::view::View;
using pulp::view::access_role_from_aria;

TEST_CASE("ARIA interactive roles no longer collapse to group",
          "[a11y][aria]") {
    REQUIRE(access_role_from_aria("button")      == View::AccessRole::button);
    REQUIRE(access_role_from_aria("link")        == View::AccessRole::link);
    REQUIRE(access_role_from_aria("checkbox")    == View::AccessRole::checkbox);
    REQUIRE(access_role_from_aria("switch")      == View::AccessRole::toggle);
    REQUIRE(access_role_from_aria("radio")       == View::AccessRole::radio);
    REQUIRE(access_role_from_aria("textbox")     == View::AccessRole::text_field);
    REQUIRE(access_role_from_aria("searchbox")   == View::AccessRole::text_field);
    REQUIRE(access_role_from_aria("combobox")    == View::AccessRole::combo_box);
    REQUIRE(access_role_from_aria("scrollbar")   == View::AccessRole::scroll_bar);
    REQUIRE(access_role_from_aria("menuitem")    == View::AccessRole::menu_item);
    REQUIRE(access_role_from_aria("tab")         == View::AccessRole::tab);

    // Each of these is a role the old parser answered `group` for.
    for (const char* token : {"button", "link", "checkbox", "combobox",
                              "textbox", "list", "listitem", "table", "row",
                              "cell", "tab", "tablist", "menu", "menuitem",
                              "dialog", "heading", "scrollbar"}) {
        INFO(token);
        REQUIRE(access_role_from_aria(token) != View::AccessRole::group);
    }
}

TEST_CASE("ARIA structural roles map to structural AccessRoles",
          "[a11y][aria]") {
    REQUIRE(access_role_from_aria("list")       == View::AccessRole::list);
    REQUIRE(access_role_from_aria("listbox")    == View::AccessRole::list);
    REQUIRE(access_role_from_aria("listitem")   == View::AccessRole::list_item);
    REQUIRE(access_role_from_aria("option")     == View::AccessRole::list_item);
    REQUIRE(access_role_from_aria("table")      == View::AccessRole::table);
    REQUIRE(access_role_from_aria("grid")       == View::AccessRole::table);
    REQUIRE(access_role_from_aria("row")        == View::AccessRole::row);
    REQUIRE(access_role_from_aria("gridcell")   == View::AccessRole::cell);
    REQUIRE(access_role_from_aria("columnheader") == View::AccessRole::cell);
    REQUIRE(access_role_from_aria("tablist")    == View::AccessRole::tab_list);
    REQUIRE(access_role_from_aria("menu")       == View::AccessRole::menu);
    REQUIRE(access_role_from_aria("menubar")    == View::AccessRole::menu);
    REQUIRE(access_role_from_aria("dialog")     == View::AccessRole::dialog);
    REQUIRE(access_role_from_aria("alertdialog") == View::AccessRole::dialog);
    REQUIRE(access_role_from_aria("heading")    == View::AccessRole::heading);
}

TEST_CASE("progressbar and meter are distinct ARIA roles", "[a11y][aria]") {
    // The old parser folded progressbar into `meter`.
    REQUIRE(access_role_from_aria("progressbar") == View::AccessRole::progress_bar);
    REQUIRE(access_role_from_aria("meter")       == View::AccessRole::meter);
}

TEST_CASE("presentation / none / empty clear the role", "[a11y][aria]") {
    REQUIRE(access_role_from_aria("")             == View::AccessRole::none);
    REQUIRE(access_role_from_aria("none")         == View::AccessRole::none);
    REQUIRE(access_role_from_aria("presentation") == View::AccessRole::none);
}

TEST_CASE("unmodelled container roles still fall back to group",
          "[a11y][aria]") {
    // Deliberate: the author asked for exposure, and a generic container is a
    // truthful superset for roles Pulp has no widget (or platform role) for.
    REQUIRE(access_role_from_aria("region")     == View::AccessRole::group);
    REQUIRE(access_role_from_aria("navigation") == View::AccessRole::group);
    REQUIRE(access_role_from_aria("form")       == View::AccessRole::group);
    REQUIRE(access_role_from_aria("toolbar")    == View::AccessRole::group);
    REQUIRE(access_role_from_aria("blorp")      == View::AccessRole::group);
}

TEST_CASE("legacy ARIA tokens keep their historical mapping", "[a11y][aria]") {
    REQUIRE(access_role_from_aria("slider") == View::AccessRole::slider);
    REQUIRE(access_role_from_aria("img")    == View::AccessRole::image);
    REQUIRE(access_role_from_aria("image")  == View::AccessRole::image);
    REQUIRE(access_role_from_aria("text")   == View::AccessRole::label);
}
