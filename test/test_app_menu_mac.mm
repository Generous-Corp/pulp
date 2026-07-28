// Menu-bar assembly for standalone apps. The menu is built once at launch and
// is never re-read by any other code, so a mistake here is invisible until a
// human opens the menu — exactly the failure that shipped a Musical Typing
// toggle no one could find.

#import <Cocoa/Cocoa.h>

#include "../core/view/platform/mac/app_menu_mac.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using pulp::view::KeyCode;
using pulp::view::WindowOptions;
using pulp::view::mac_menu::install_application_menu;

namespace {

// install_application_menu writes through [NSApp setMainMenu:], so every test
// reads the result back off the shared application object.
NSMenu* app_menu() {
    NSMenu* bar = [NSApp mainMenu];
    if (bar == nil || [bar numberOfItems] == 0)
        return nil;
    return [[bar itemAtIndex:0] submenu];
}

NSMenu* named_menu(NSString* title) {
    NSMenu* bar = [NSApp mainMenu];
    for (NSInteger i = 0; i < [bar numberOfItems]; ++i) {
        NSMenuItem* item = [bar itemAtIndex:i];
        if ([[item title] isEqualToString:title])
            return [item submenu];
    }
    return nil;
}

std::string title_at(NSMenu* menu, NSInteger index) {
    if (menu == nil || index >= [menu numberOfItems])
        return "<out-of-range>";
    NSMenuItem* item = [menu itemAtIndex:index];
    if ([item isSeparatorItem])
        return "<separator>";
    return [[item title] UTF8String];
}

WindowOptions::MenuCommand make_command(std::string menu, std::string title,
                                        std::function<void()> action = [] {}) {
    WindowOptions::MenuCommand command;
    command.menu = std::move(menu);
    command.title = std::move(title);
    command.key = KeyCode::k;
    command.modifiers = pulp::view::kModCmd;
    command.action = std::move(action);
    return command;
}

} // namespace

TEST_CASE("application menu holds only Quit when nothing registers a command",
          "[view][menu]") {
    [NSApplication sharedApplication];
    install_application_menu({}, [] {});

    NSMenu* menu = app_menu();
    REQUIRE(menu != nil);
    // No leading separator: a lone rule above Quit reads as a missing item.
    CHECK([menu numberOfItems] == 1);
    CHECK(title_at(menu, 0) == "Quit");
}

TEST_CASE("an empty menu name places the command in the application menu",
          "[view][menu]") {
    [NSApplication sharedApplication];
    install_application_menu({make_command("", "Musical Typing Keyboard")}, [] {});

    NSMenu* menu = app_menu();
    REQUIRE(menu != nil);
    // Above Quit, separated from it — Quit must stay last, per macOS.
    REQUIRE([menu numberOfItems] == 3);
    CHECK(title_at(menu, 0) == "Musical Typing Keyboard");
    CHECK(title_at(menu, 1) == "<separator>");
    CHECK(title_at(menu, 2) == "Quit");

    NSMenuItem* item = [menu itemAtIndex:0];
    CHECK(std::string([[item keyEquivalent] UTF8String]) == "k");
    CHECK(([item keyEquivalentModifierMask] & NSEventModifierFlagCommand) != 0);
}

TEST_CASE("a named menu still becomes its own menu-bar submenu", "[view][menu]") {
    [NSApplication sharedApplication];
    install_application_menu({make_command("Window", "Inspector")}, [] {});

    NSMenu* window_menu = named_menu(@"Window");
    REQUIRE(window_menu != nil);
    CHECK([window_menu numberOfItems] == 1);
    CHECK(title_at(window_menu, 0) == "Inspector");

    // The named command must NOT also land in the app menu.
    NSMenu* menu = app_menu();
    REQUIRE(menu != nil);
    CHECK([menu numberOfItems] == 1);
    CHECK(title_at(menu, 0) == "Quit");
}

TEST_CASE("app-menu and named-menu commands coexist without stealing each other",
          "[view][menu]") {
    [NSApplication sharedApplication];
    install_application_menu(
        {
            make_command("", "Musical Typing Keyboard"),
            make_command("Window", "Inspector"),
            make_command("", "Audio Settings"),
        },
        [] {});

    NSMenu* menu = app_menu();
    REQUIRE(menu != nil);
    // Registration order is preserved, and ONE separator divides the group
    // from Quit however many commands precede it.
    REQUIRE([menu numberOfItems] == 4);
    CHECK(title_at(menu, 0) == "Musical Typing Keyboard");
    CHECK(title_at(menu, 1) == "Audio Settings");
    CHECK(title_at(menu, 2) == "<separator>");
    CHECK(title_at(menu, 3) == "Quit");

    NSMenu* window_menu = named_menu(@"Window");
    REQUIRE(window_menu != nil);
    CHECK([window_menu numberOfItems] == 1);
}

TEST_CASE("an app-menu item invokes its action", "[view][menu]") {
    [NSApplication sharedApplication];
    int fired = 0;
    install_application_menu({make_command("", "Toggle", [&fired] { ++fired; })}, [] {});

    NSMenu* menu = app_menu();
    REQUIRE(menu != nil);
    NSMenuItem* item = [menu itemAtIndex:0];
    // The target is retained via representedObject; sending the action here is
    // what AppKit does on click, and proves the target outlived installation.
    REQUIRE([item target] != nil);
    [[item target] performSelector:[item action] withObject:item];
    CHECK(fired == 1);
}

TEST_CASE("malformed commands are dropped rather than rendered blank",
          "[view][menu]") {
    [NSApplication sharedApplication];
    WindowOptions::MenuCommand no_title = make_command("", "");
    WindowOptions::MenuCommand no_action = make_command("", "Orphan");
    no_action.action = nullptr;
    install_application_menu({no_title, no_action}, [] {});

    NSMenu* menu = app_menu();
    REQUIRE(menu != nil);
    // Both rejected, so no separator either.
    CHECK([menu numberOfItems] == 1);
    CHECK(title_at(menu, 0) == "Quit");
}
