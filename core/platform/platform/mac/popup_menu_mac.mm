// macOS menus: the synchronous PopupMenu and the asynchronous anchored menu,
// both built by build_native_menu(). A per-binary target class records the
// chosen row on the root menu, so the caller can read the selection after
// tracking ends.

#include <pulp/platform/objc_unique_class.hpp>
#include <pulp/platform/popup_menu.hpp>

#include "../../src/dialog_internal.hpp"

#import <Cocoa/Cocoa.h>
#include <objc/runtime.h>

#include <memory>

namespace pulp::platform {

namespace {

// Associated-object keys on the root NSMenu.
char kChosenKey;
char kTargetKey;

NSString* ns(const std::string& text) {
    NSString* s = [NSString stringWithUTF8String:text.c_str()];
    return s != nil ? s : @"";
}

// -pulpChoose: records the sender's tag on the root menu, which is the
// target's context (not retained: the menu retains the target).
void choose_item(id self, SEL, id sender) {
    auto* root = static_cast<NSMenu*>(mac::instance_context((__bridge void*)self));
    if (root == nil || ![sender isKindOfClass:[NSMenuItem class]])
        return;
    objc_setAssociatedObject(root, &kChosenKey, @([static_cast<NSMenuItem*>(sender) tag]),
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

void add_target_methods(void* cls, void*) {
    class_addMethod(static_cast<Class>(cls), sel_registerName("pulpChoose:"),
                    reinterpret_cast<IMP>(&choose_item), "v@:@");
}

id make_target(NSMenu* root) {
    void* cls = mac::unique_objc_class("PulpPlatformMenuTarget", (__bridge void*)[NSObject class],
                                       &add_target_methods);
    if (cls == nullptr)
        return nil;
    id target = [[static_cast<Class>(cls) alloc] init];
    mac::set_instance_context((__bridge void*)target, (__bridge void*)root);
    objc_setAssociatedObject(root, &kTargetKey, target, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [target release];
    return target;
}

void fill_menu(NSMenu* menu, const std::vector<MenuItem>& items, id target) {
    menu.autoenablesItems = NO;
    for (const auto& item : items) {
        if (item.separator) {
            [menu addItem:[NSMenuItem separatorItem]];
            continue;
        }
        NSMenuItem* row = [[NSMenuItem alloc] initWithTitle:ns(item.label)
                                                     action:nil
                                              keyEquivalent:@""];
        row.tag = item.id;
        row.enabled = item.enabled ? YES : NO;
        row.state = item.checked ? NSControlStateValueOn : NSControlStateValueOff;
        if (item.text_colour) {
            const auto& c = *item.text_colour;
            NSColor* colour = [NSColor colorWithSRGBRed:c.r green:c.g blue:c.b alpha:c.a];
            NSDictionary* attributes = @{
                NSForegroundColorAttributeName : colour,
                NSFontAttributeName : [NSFont menuFontOfSize:0],
            };
            NSAttributedString* title = [[NSAttributedString alloc] initWithString:ns(item.label)
                                                                        attributes:attributes];
            row.attributedTitle = title;
            [title release];
        }
        if (!item.submenu.empty()) {
            NSMenu* sub = [[NSMenu alloc] initWithTitle:ns(item.label)];
            fill_menu(sub, item.submenu, target);
            row.submenu = sub;
            [sub release];
        } else {
            row.target = target;
            row.action = sel_registerName("pulpChoose:");
        }
        [menu addItem:row];
        [row release];
    }
}

MenuModel model_from(const std::vector<PopupMenu::Item>& items) {
    MenuModel model;
    for (const auto& item : items) {
        if (item.is_separator) {
            model.items.push_back(MenuItem::make_separator());
            continue;
        }
        MenuItem row;
        row.id = item.id;
        row.label = item.label;
        row.enabled = item.enabled;
        row.checked = item.checked;
        model.items.push_back(std::move(row));
    }
    return model;
}

} // namespace

void* build_native_menu(const MenuModel& model, float minimum_width) {
    NSMenu* menu = [[[NSMenu alloc] initWithTitle:@""] autorelease];
    id target = make_target(menu);
    fill_menu(menu, model.items, target);
    if (minimum_width > 0)
        menu.minimumWidth = minimum_width;
    return (__bridge void*)menu;
}

std::optional<int> native_menu_chosen_id(void* menu) {
    if (menu == nullptr)
        return std::nullopt;
    id chosen = objc_getAssociatedObject((__bridge NSMenu*)menu, &kChosenKey);
    if (![chosen isKindOfClass:[NSNumber class]])
        return std::nullopt;
    return static_cast<int>([static_cast<NSNumber*>(chosen) integerValue]);
}

std::optional<int> PopupMenu::show(float x, float y) const {
    @autoreleasepool {
        auto* menu = static_cast<NSMenu*>(build_native_menu(model_from(items_), 0));
        const NSPoint location = NSMakePoint(x, [[NSScreen mainScreen] frame].size.height - y);
        [menu popUpMenuPositioningItem:nil atLocation:location inView:nil];
        return native_menu_chosen_id((__bridge void*)menu);
    }
}

std::optional<int> PopupMenu::show_at_view(void* native_view_handle) const {
    @autoreleasepool {
        NSView* view = (__bridge NSView*)native_view_handle;
        if (view == nil)
            return std::nullopt;
        auto* menu = static_cast<NSMenu*>(build_native_menu(model_from(items_), 0));
        const NSPoint point = NSMakePoint(0, view.isFlipped ? view.bounds.size.height : 0);
        [menu popUpMenuPositioningItem:nil atLocation:point inView:view];
        return native_menu_chosen_id((__bridge void*)menu);
    }
}

namespace detail {

namespace {

// Everything one asynchronous menu needs until it has finished.
struct NativeMenuOpen {
    MenuAnchor anchor;
    MenuModel model;
    MenuPlacement placement = MenuPlacement::below_anchor;
    std::function<void(std::optional<int>)> respond;
    std::unique_ptr<MainThreadTimer> opener;
    NSMenu* menu = nil; // retained while tracking
    NSView* view = nil; // retained until finished
    bool closed = false;

    ~NativeMenuOpen() {
        [menu release];
        [view release];
    }

    void finish(std::optional<int> id) {
        auto r = std::move(respond);
        respond = nullptr;
        if (r)
            r(id);
    }

    void open_now() {
        if (closed || view.window == nil) {
            finish(std::nullopt);
            return;
        }
        NSMenu* built = static_cast<NSMenu*>(build_native_menu(model, anchor.width));
        menu = [built retain];
        NSMenuItem* positioning = nil;
        if (placement == MenuPlacement::over_selected && model.selected_id)
            positioning = [menu itemWithTag:*model.selected_id];
        const MenuPoint at = menu_location_in_view(anchor, placement, view.isFlipped,
                                                   static_cast<float>(view.bounds.size.height));
        // Tracking runs a nested loop in the event-tracking mode; timers and
        // posts from MainThreadScope keep running inside it.
        [menu popUpMenuPositioningItem:positioning atLocation:NSMakePoint(at.x, at.y) inView:view];
        finish(native_menu_chosen_id((__bridge void*)menu));
        [menu release];
        menu = nil;
    }

    void close() {
        closed = true;
        if (menu != nil) {
            [menu cancelTracking]; // open_now() then finishes with no choice
            return;
        }
        if (opener)
            opener->cancel();
        finish(std::nullopt);
    }
};

} // namespace

std::function<void()> present_native_menu(MainThreadScope& scope, const MenuAnchor& anchor,
                                          MenuModel model, MenuPlacement placement,
                                          std::function<void(std::optional<int>)> respond,
                                          std::shared_ptr<void>& resources) {
    NSView* view = (__bridge NSView*)anchor.view;
    if (view == nil || view.window == nil)
        return {};
    auto open = std::make_shared<NativeMenuOpen>();
    open->anchor = anchor;
    open->model = std::move(model);
    open->placement = placement;
    open->respond = std::move(respond);
    open->view = [view retain];
    std::weak_ptr<NativeMenuOpen> weak = open;
    open->opener = std::make_unique<MainThreadTimer>(scope, [weak] {
        if (auto o = weak.lock())
            o->open_now();
    });
    // Open from a timer on a later pass, never inside the caller's handler or
    // a dispatch block.
    open->opener->fire_after(std::chrono::milliseconds(0));
    resources = open;
    return [weak] {
        if (auto o = weak.lock())
            o->close();
    };
}

} // namespace detail

} // namespace pulp::platform
