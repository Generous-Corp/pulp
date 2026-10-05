#include "pulp_mac_objc_names.h"
#import <Cocoa/Cocoa.h>

#include "app_menu_mac.hpp"

#include <functional>
#include <string>
#include <utility>

@interface PulpMenuCommandTarget : NSObject {
  @public
    std::function<void()> action;
}
- (void)performMenuCommand:(id)sender;
@end

@implementation PulpMenuCommandTarget
- (void)performMenuCommand:(id)sender {
    (void)sender;
    if (action)
        action();
}
@end

namespace pulp::view::mac_menu {

namespace {

// Printable-ASCII KeyCodes only. The non-ASCII KeyCodes (escape 274, arrows
// 256..259, f1 290, …) have no single-character equivalent and would need the
// NSxxxFunctionKey constants, so a MenuCommand carrying one currently gets NO
// visible shortcut rather than a wrong one. Extend this mapping before wiring a
// menu command to a function or navigation key.
NSString* key_equivalent(KeyCode key) {
    const int value = static_cast<int>(key);
    if (value < 32 || value > 126)
        return @"";
    const unichar character = static_cast<unichar>(value);
    return [NSString stringWithCharacters:&character length:1];
}

NSEventModifierFlags modifier_mask(std::uint16_t modifiers) {
    NSEventModifierFlags flags = 0;
    if (modifiers & kModShift)
        flags |= NSEventModifierFlagShift;
    if (modifiers & kModCtrl)
        flags |= NSEventModifierFlagControl;
    if (modifiers & kModAlt)
        flags |= NSEventModifierFlagOption;
    if (modifiers & (kModCmd | kModMeta))
        flags |= NSEventModifierFlagCommand;
    return flags;
}

} // namespace

static NSString* application_name() {
    NSBundle* bundle = [NSBundle mainBundle];
    for (NSString* key in @[ @"CFBundleDisplayName", @"CFBundleName" ]) {
        id value = [bundle objectForInfoDictionaryKey:key];
        if ([value isKindOfClass:[NSString class]] && [(NSString*)value length] > 0)
            return (NSString*)value;
    }
    return [[NSProcessInfo processInfo] processName];
}

void install_application_menu(const std::vector<WindowOptions::MenuCommand>& commands,
                              std::function<void()> quit_action) {
    NSMenu* menu_bar = [[[NSMenu alloc] init] autorelease];
    NSMenuItem* app_item = [[[NSMenuItem alloc] init] autorelease];
    [menu_bar addItem:app_item];
    [NSApp setMainMenu:menu_bar];

    NSMenu* app_menu = [[[NSMenu alloc] init] autorelease];
    [app_item setSubmenu:app_menu];
    NSString* app_name = application_name();

    // Builds the item and wires its target. representedObject retains the
    // target for exactly as long as the item, so no separate ownership.
    auto add_item = [](NSMenu* menu, const WindowOptions::MenuCommand& command) {
        PulpMenuCommandTarget* target = [[[PulpMenuCommandTarget alloc] init] autorelease];
        target->action = command.action;
        NSMenuItem* item =
            [[[NSMenuItem alloc] initWithTitle:[NSString stringWithUTF8String:command.title.c_str()]
                                        action:@selector(performMenuCommand:)
                                 keyEquivalent:key_equivalent(command.key)] autorelease];
        [item setKeyEquivalentModifierMask:modifier_mask(command.modifiers)];
        [item setTarget:target];
        [item setRepresentedObject:target];
        [menu addItem:item];
    };
    // AppKit's own application actions, sent straight to NSApp.
    auto add_app_action = [app_menu](NSString* title, SEL action, NSString* key,
                                     NSEventModifierFlags modifiers) {
        NSMenuItem* item = [[[NSMenuItem alloc] initWithTitle:title action:action
                                                keyEquivalent:key] autorelease];
        [item setKeyEquivalentModifierMask:modifiers];
        [item setTarget:NSApp];
        [app_menu addItem:item];
    };
    auto is_app_command = [](const WindowOptions::MenuCommand& command,
                             WindowOptions::MenuCommand::AppMenuSection section) {
        return command.menu.empty() && !command.title.empty() && command.action &&
               command.app_menu_section == section;
    };
    using Section = WindowOptions::MenuCommand::AppMenuSection;

    // The standard macOS application menu:
    //   About <App>
    //   [after_about commands, e.g. Check for Updates…]
    //   ---
    //   [app commands, e.g. Settings…]   (and its rule, only when present)
    //   ---
    //   Services ▸
    //   ---
    //   Hide <App> / Hide Others / Show All
    //   ---
    //   Quit <App>                        (always last)
    add_app_action([@"About " stringByAppendingString:app_name],
                   @selector(orderFrontStandardAboutPanel:), @"", 0);
    for (const auto& command : commands)
        if (is_app_command(command, Section::after_about))
            add_item(app_menu, command);
    [app_menu addItem:[NSMenuItem separatorItem]];

    bool app_menu_has_commands = false;
    for (const auto& command : commands) {
        if (!is_app_command(command, Section::commands))
            continue;
        add_item(app_menu, command);
        app_menu_has_commands = true;
    }
    if (app_menu_has_commands)
        [app_menu addItem:[NSMenuItem separatorItem]];

    NSMenu* services = [[[NSMenu alloc] initWithTitle:@"Services"] autorelease];
    NSMenuItem* services_item = [[[NSMenuItem alloc] initWithTitle:@"Services"
                                                            action:nil
                                                     keyEquivalent:@""] autorelease];
    [services_item setSubmenu:services];
    [app_menu addItem:services_item];
    [NSApp setServicesMenu:services];
    [app_menu addItem:[NSMenuItem separatorItem]];

    add_app_action([@"Hide " stringByAppendingString:app_name], @selector(hide:), @"h",
                   NSEventModifierFlagCommand);
    add_app_action(@"Hide Others", @selector(hideOtherApplications:), @"h",
                   NSEventModifierFlagCommand | NSEventModifierFlagOption);
    add_app_action(@"Show All", @selector(unhideAllApplications:), @"", 0);
    [app_menu addItem:[NSMenuItem separatorItem]];

    PulpMenuCommandTarget* quit_target = [[[PulpMenuCommandTarget alloc] init] autorelease];
    quit_target->action = std::move(quit_action);
    NSMenuItem* quit_item =
        [[[NSMenuItem alloc] initWithTitle:[@"Quit " stringByAppendingString:app_name]
                                    action:@selector(performMenuCommand:)
                             keyEquivalent:@"q"] autorelease];
    [quit_item setTarget:quit_target];
    [quit_item setRepresentedObject:quit_target];
    [app_menu addItem:quit_item];

    std::vector<std::pair<std::string, NSMenu*>> menus;
    for (const auto& command : commands) {
        if (command.menu.empty() || command.title.empty() || !command.action)
            continue;

        NSMenu* menu = nil;
        for (const auto& [title, candidate] : menus) {
            if (title == command.menu) {
                menu = candidate;
                break;
            }
        }
        if (menu == nil) {
            NSString* menu_title = [NSString stringWithUTF8String:command.menu.c_str()];
            NSMenuItem* menu_item = [[[NSMenuItem alloc] initWithTitle:menu_title
                                                                action:nil
                                                         keyEquivalent:@""] autorelease];
            menu = [[[NSMenu alloc] initWithTitle:menu_title] autorelease];
            [menu_item setSubmenu:menu];
            [menu_bar addItem:menu_item];
            menus.emplace_back(command.menu, menu);
        }

        add_item(menu, command);
    }
}

} // namespace pulp::view::mac_menu
