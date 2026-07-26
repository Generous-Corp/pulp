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

void append_commands(const std::vector<WindowOptions::MenuCommand>& commands) {
    NSMenu* menu_bar = [NSApp mainMenu];
    if (menu_bar == nil)
        return;

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

        PulpMenuCommandTarget* target = [[[PulpMenuCommandTarget alloc] init] autorelease];
        target->action = command.action;
        NSMenuItem* item =
            [[[NSMenuItem alloc] initWithTitle:[NSString stringWithUTF8String:command.title.c_str()]
                                        action:@selector(performMenuCommand:)
                                 keyEquivalent:key_equivalent(command.key)] autorelease];
        [item setKeyEquivalentModifierMask:modifier_mask(command.modifiers)];
        [item setTarget:target];
        // representedObject retains the target for exactly as long as the item.
        [item setRepresentedObject:target];
        [menu addItem:item];
    }
}

} // namespace pulp::view::mac_menu
