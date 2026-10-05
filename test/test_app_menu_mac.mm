// Menu-bar assembly for standalone apps. The menu is built once at launch and
// is never re-read by any other code, so a mistake here is invisible until a
// human opens the menu — exactly the failure that shipped a Musical Typing
// toggle no one could find.

#import <Cocoa/Cocoa.h>

#include "../core/view/platform/mac/app_menu_mac.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

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

// Every item's title in order, separators as "<separator>".
std::vector<std::string> titles(NSMenu* menu) {
    std::vector<std::string> out;
    for (NSInteger i = 0; menu != nil && i < [menu numberOfItems]; ++i)
        out.push_back(title_at(menu, i));
    return out;
}

std::string app_name() {
    NSString* name = [[NSProcessInfo processInfo] processName];
    for (NSString* key in @[ @"CFBundleDisplayName", @"CFBundleName" ]) {
        id value = [[NSBundle mainBundle] objectForInfoDictionaryKey:key];
        if ([value isKindOfClass:[NSString class]] && [(NSString*)value length] > 0) {
            name = (NSString*)value;
            break;
        }
    }
    return [name UTF8String];
}

// The fixed tail every application menu ends with.
std::vector<std::string> standard_tail() {
    return {"Services", "<separator>", "Hide " + app_name(), "Hide Others",
            "Show All", "<separator>", "Quit " + app_name()};
}

std::vector<std::string> concat(std::vector<std::string> a, const std::vector<std::string>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

WindowOptions::MenuCommand after_about(std::string title, std::function<void()> action = [] {}) {
    auto command = make_command("", std::move(title), std::move(action));
    command.key = KeyCode::unknown;
    command.modifiers = pulp::view::kModNone;
    command.app_menu_section = WindowOptions::MenuCommand::AppMenuSection::after_about;
    return command;
}

} // namespace

TEST_CASE("application menu is the standard macOS layout when nothing registers a command",
          "[view][menu]") {
    [NSApplication sharedApplication];
    install_application_menu({}, [] {});

    NSMenu* menu = app_menu();
    REQUIRE(menu != nil);
    // No app-command group, so no stray rule for it.
    CHECK(titles(menu) == concat({"About " + app_name(), "<separator>"}, standard_tail()));
    // Quit keeps Cmd-Q and is last, as macOS expects.
    NSMenuItem* quit = [menu itemAtIndex:[menu numberOfItems] - 1];
    CHECK(std::string([[quit keyEquivalent] UTF8String]) == "q");
    CHECK([NSApp servicesMenu] != nil);
}

TEST_CASE("an empty menu name places the command in the app-command group", "[view][menu]") {
    [NSApplication sharedApplication];
    install_application_menu({make_command("", "Musical Typing Keyboard")}, [] {});

    NSMenu* menu = app_menu();
    REQUIRE(menu != nil);
    CHECK(titles(menu) ==
          concat({"About " + app_name(), "<separator>", "Musical Typing Keyboard", "<separator>"},
                 standard_tail()));

    NSMenuItem* item = [menu itemAtIndex:2];
    CHECK(std::string([[item keyEquivalent] UTF8String]) == "k");
    CHECK(([item keyEquivalentModifierMask] & NSEventModifierFlagCommand) != 0);
}

TEST_CASE("Check for Updates sits directly under About, Settings below it",
          "[view][menu][updater]") {
    [NSApplication sharedApplication];
    // Registration order deliberately puts Settings… first: the section, not
    // the order, decides where the updater goes.
    install_application_menu(
        {make_command("", "Settings\xE2\x80\xA6"), after_about("Check for Updates\xE2\x80\xA6")},
        [] {});

    NSMenu* menu = app_menu();
    REQUIRE(menu != nil);
    CHECK(titles(menu) == concat({"About " + app_name(), "Check for Updates\xE2\x80\xA6",
                                  "<separator>", "Settings\xE2\x80\xA6", "<separator>"},
                                 standard_tail()));
}

TEST_CASE("without the after_about section the updater would land in the command group",
          "[view][menu][updater]") {
    // Negative control for the case above: the same title registered as an
    // ordinary app command follows Settings…, so the placement is the field's.
    [NSApplication sharedApplication];
    install_application_menu({make_command("", "Settings\xE2\x80\xA6"),
                              make_command("", "Check for Updates\xE2\x80\xA6")},
                             [] {});
    NSMenu* menu = app_menu();
    REQUIRE(menu != nil);
    CHECK(title_at(menu, 1) == "<separator>");
    CHECK(title_at(menu, 2) == "Settings\xE2\x80\xA6");
    CHECK(title_at(menu, 3) == "Check for Updates\xE2\x80\xA6");
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
    CHECK(titles(menu) == concat({"About " + app_name(), "<separator>"}, standard_tail()));
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
    // Registration order is preserved, and ONE separator closes the group
    // however many commands it holds.
    CHECK(titles(menu) == concat({"About " + app_name(), "<separator>", "Musical Typing Keyboard",
                                  "Audio Settings", "<separator>"},
                                 standard_tail()));

    NSMenu* window_menu = named_menu(@"Window");
    REQUIRE(window_menu != nil);
    CHECK([window_menu numberOfItems] == 1);
}

TEST_CASE("app-menu items invoke their actions, Quit included", "[view][menu]") {
    [NSApplication sharedApplication];
    int fired = 0;
    int updates = 0;
    int quits = 0;
    install_application_menu(
        {make_command("", "Toggle", [&fired] { ++fired; }),
         after_about("Check for Updates\xE2\x80\xA6", [&updates] { ++updates; })},
        [&quits] { ++quits; });

    NSMenu* menu = app_menu();
    REQUIRE(menu != nil);
    // Sending the action is what AppKit does on click, and proves each target
    // (retained via representedObject) outlived installation.
    auto send = [menu](NSInteger index) {
        NSMenuItem* item = [menu itemAtIndex:index];
        REQUIRE([item target] != nil);
        [[item target] performSelector:[item action] withObject:item];
    };
    REQUIRE(title_at(menu, 1) == "Check for Updates\xE2\x80\xA6");
    send(1);
    REQUIRE(title_at(menu, 3) == "Toggle");
    send(3);
    send([menu numberOfItems] - 1);
    CHECK(updates == 1);
    CHECK(fired == 1);
    CHECK(quits == 1);

    // About and Hide go to NSApp's standard actions.
    NSMenuItem* about = [menu itemAtIndex:0];
    CHECK([about target] == NSApp);
    CHECK([about action] == @selector(orderFrontStandardAboutPanel:));
}

TEST_CASE("malformed commands are dropped rather than rendered blank",
          "[view][menu]") {
    [NSApplication sharedApplication];
    WindowOptions::MenuCommand no_title = make_command("", "");
    WindowOptions::MenuCommand no_action = make_command("", "Orphan");
    no_action.action = nullptr;
    WindowOptions::MenuCommand blank_update = after_about("");
    install_application_menu({no_title, no_action, blank_update}, [] {});

    NSMenu* menu = app_menu();
    REQUIRE(menu != nil);
    // All rejected, so no app-command group and no extra separator either.
    CHECK(titles(menu) == concat({"About " + app_name(), "<separator>"}, standard_tail()));
}
