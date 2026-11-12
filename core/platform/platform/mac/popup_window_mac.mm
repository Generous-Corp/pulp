// macOS popup windows: a borderless, non-activating panel that is a child of
// the editor's window, placed against an anchor in the editor view, and
// dismissed by outside clicks (consumed), Escape, focus loss or the parent
// closing.

#include <pulp/platform/objc_unique_class.hpp>
#include <pulp/platform/popup_window.hpp>

#import <Cocoa/Cocoa.h>
#include <objc/runtime.h>

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace pulp::platform {

namespace {

constexpr unsigned short kEscapeKeyCode = 53;

float primary_top() {
    NSScreen* primary = NSScreen.screens.firstObject;
    return primary != nil ? static_cast<float>(NSMaxY(primary.frame)) : 0.0f;
}

// AppKit screen rectangles are bottom-left based on the primary screen.
PopupRect to_top_left(NSRect r) {
    return {static_cast<float>(r.origin.x),
            primary_top() - static_cast<float>(r.origin.y + r.size.height),
            static_cast<float>(r.size.width), static_cast<float>(r.size.height)};
}

NSRect from_top_left(const PopupRect& r) {
    return NSMakeRect(r.x, primary_top() - (r.y + r.height), r.width, r.height);
}

BOOL panel_can_become_key(id self, SEL) {
    auto* wants_key = static_cast<bool*>(mac::instance_context((__bridge void*)self));
    return wants_key != nullptr && *wants_key ? YES : NO;
}

BOOL panel_can_become_main(id, SEL) {
    return NO;
}

void add_panel_methods(void* cls, void*) {
    Class c = static_cast<Class>(cls);
    class_addMethod(c, @selector(canBecomeKeyWindow), reinterpret_cast<IMP>(&panel_can_become_key),
                    "c@:");
    class_addMethod(c, @selector(canBecomeMainWindow),
                    reinterpret_cast<IMP>(&panel_can_become_main), "c@:");
}

} // namespace

struct PopupWindow::Impl : std::enable_shared_from_this<PopupWindow::Impl> {
    PopupWindowOptions options;
    std::function<void(PopupDismissReason)> on_dismiss;
    MainThreadScope* scope = nullptr;
    MainThreadScope::Handle handle;
    std::uint64_t teardown_id = 0;
    bool wants_key = true; // read by the panel class through its context slot

    NSPanel* panel = nil;
    NSView* content = nil;
    NSView* view = nil;
    NSWindow* parent = nil;
    id local_monitor = nil;
    id global_monitor = nil;
    std::vector<id> observers;
    bool open = false;

    ~Impl() {
        dismiss(std::nullopt);
        if (panel != nil)
            mac::set_instance_context((__bridge void*)panel, nullptr);
        [content release];
        [panel release];
        [view release];
        [parent release];
    }

    PopupRect anchor_on_screen() const {
        const PopupRect& a = options.anchor;
        const CGFloat h = view.bounds.size.height;
        const NSRect in_view =
            NSMakeRect(a.x, view.isFlipped ? a.y : h - a.y - a.height, a.width, a.height);
        const NSRect in_window = [view convertRect:in_view toView:nil];
        return to_top_left([parent convertRectToScreen:in_window]);
    }

    void place() {
        NSScreen* screen = parent.screen != nil ? parent.screen : NSScreen.mainScreen;
        const PopupRect frame = place_popup(anchor_on_screen(), options.width, options.height,
                                            to_top_left(screen.visibleFrame), options.placement);
        [panel setFrame:from_top_left(frame) display:YES];
    }

    // Close; report `reason` (posted through the scope) when there is one.
    void dismiss(std::optional<PopupDismissReason> reason) {
        if (!open)
            return;
        open = false;
        if (local_monitor != nil) {
            [NSEvent removeMonitor:local_monitor];
            [local_monitor release];
            local_monitor = nil;
        }
        if (global_monitor != nil) {
            [NSEvent removeMonitor:global_monitor];
            [global_monitor release];
            global_monitor = nil;
        }
        for (id observer : observers) {
            [[NSNotificationCenter defaultCenter] removeObserver:observer];
            [observer release];
        }
        observers.clear();
        [parent removeChildWindow:panel];
        [panel orderOut:nil];
        // A destroyed scope has already run (and dropped) its teardowns.
        if (teardown_id != 0 && handle.alive())
            scope->remove_teardown(teardown_id);
        teardown_id = 0;
        if (reason && on_dismiss) {
            auto report = std::move(on_dismiss);
            on_dismiss = nullptr;
            const PopupDismissReason why = *reason;
            handle.post([report, why] { report(why); });
        }
    }

    void observe(NSNotificationName name, id object, PopupDismissReason reason) {
        std::weak_ptr<Impl> weak = weak_from_this();
        id token = [[NSNotificationCenter defaultCenter] addObserverForName:name
                                                                     object:object
                                                                      queue:nil
                                                                 usingBlock:^(NSNotification*) {
                                                                   if (auto self = weak.lock())
                                                                       self->dismiss(reason);
                                                                 }];
        observers.push_back([token retain]);
    }

    // The popup losing key is a loss of focus only when key went somewhere
    // other than its own editor window. Activating the application can hand
    // key back to the editor window asynchronously, after the popup took it,
    // with no click; a click on the editor has already closed the popup
    // through the local monitor. So decide one pass later, once AppKit has
    // settled the key window: back on the editor means take key again, up to
    // a few times; anywhere else means focus_lost.
    int key_reclaims = 0;

    void observe_resign_key() {
        std::weak_ptr<Impl> weak = weak_from_this();
        id token = [[NSNotificationCenter defaultCenter]
            addObserverForName:NSWindowDidResignKeyNotification
                        object:panel
                         queue:nil
                    usingBlock:^(NSNotification*) {
                      if (auto self = weak.lock())
                          self->after_resign_key();
                    }];
        observers.push_back([token retain]);
    }

    void after_resign_key() {
        std::weak_ptr<Impl> weak = weak_from_this();
        const bool posted = handle.post([weak] {
            auto self = weak.lock();
            if (!self || !self->open)
                return;
            switch (popup_resign_action(self->panel.isKeyWindow == YES,
                                        self->parent.isKeyWindow == YES, self->key_reclaims)) {
            case PopupResignAction::stay_open:
                return;
            case PopupResignAction::take_key_back:
                ++self->key_reclaims;
                [self->panel makeKeyWindow];
                return;
            case PopupResignAction::close_focus_lost:
                self->dismiss(PopupDismissReason::focus_lost);
                return;
            }
        });
        if (!posted)
            dismiss(PopupDismissReason::focus_lost);
    }

    void install_dismissal() {
        std::weak_ptr<Impl> weak = weak_from_this();
        NSPanel* own = panel;
        const NSEventMask clicks =
            NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown | NSEventMaskOtherMouseDown;
        local_monitor = [[NSEvent
            addLocalMonitorForEventsMatchingMask:clicks | NSEventMaskKeyDown
                                         handler:^NSEvent*(NSEvent* event) {
                                           auto self = weak.lock();
                                           if (!self || !self->open)
                                               return event;
                                           if (event.type == NSEventTypeKeyDown) {
                                               if (event.window == own &&
                                                   event.keyCode == kEscapeKeyCode) {
                                                   self->dismiss(PopupDismissReason::escape);
                                                   return nil;
                                               }
                                               return event;
                                           }
                                           if (event.window != own) {
                                               // Consumed: the click that
                                               // closes the popup does not
                                               // reach the editor.
                                               self->dismiss(PopupDismissReason::outside_click);
                                               return nil;
                                           }
                                           return event;
                                         }] retain];
        global_monitor = [[NSEvent
            addGlobalMonitorForEventsMatchingMask:clicks
                                          handler:^(NSEvent*) {
                                            if (auto self = weak.lock())
                                                self->dismiss(PopupDismissReason::outside_click);
                                          }] retain];
        observe(NSApplicationDidResignActiveNotification, nil, PopupDismissReason::focus_lost);
        if (wants_key)
            observe_resign_key();
        observe(NSWindowWillCloseNotification, parent, PopupDismissReason::parent_closed);
    }
};

PopupWindow::PopupWindow(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

PopupWindow::~PopupWindow() = default;

std::unique_ptr<PopupWindow> PopupWindow::open(MainThreadScope& scope,
                                               const PopupWindowOptions& options,
                                               std::function<void(PopupDismissReason)> on_dismiss) {
    NSView* view = (__bridge NSView*)options.view;
    if (view == nil || view.window == nil || !scope.handle().alive())
        return nullptr;
    void* cls = mac::unique_objc_class("PulpPlatformPopupPanel", (__bridge void*)[NSPanel class],
                                       &add_panel_methods);
    if (cls == nullptr)
        return nullptr;

    auto impl = std::make_shared<Impl>();
    impl->options = options;
    impl->on_dismiss = std::move(on_dismiss);
    impl->scope = &scope;
    impl->handle = scope.handle();
    impl->wants_key = options.takes_key_focus;
    impl->view = [view retain];
    impl->parent = [view.window retain];

    NSPanel* panel = [[static_cast<Class>(cls) alloc]
        initWithContentRect:NSMakeRect(0, 0, impl->options.width, options.height)
                  styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                    backing:NSBackingStoreBuffered
                      defer:NO];
    mac::set_instance_context((__bridge void*)panel, &impl->wants_key);
    panel.releasedWhenClosed = NO;
    panel.hasShadow = YES;
    panel.opaque = NO;
    panel.backgroundColor = NSColor.clearColor;
    panel.hidesOnDeactivate = NO;
    panel.floatingPanel = YES;
    panel.becomesKeyOnlyIfNeeded = NO;
    impl->panel = panel;

    NSView* content =
        [[NSView alloc] initWithFrame:NSMakeRect(0, 0, impl->options.width, options.height)];
    content.wantsLayer = YES;
    content.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    panel.contentView = content;
    impl->content = content;

    impl->place();
    [impl->parent addChildWindow:panel ordered:NSWindowAbove];
    // At least the parent's level, and above ordinary floating panels, as a
    // system popup menu is.
    panel.level = std::max<NSWindowLevel>(impl->parent.level, NSPopUpMenuWindowLevel);
    if (options.takes_key_focus)
        [panel makeKeyAndOrderFront:nil];
    else
        [panel orderFront:nil];
    impl->open = true;
    impl->install_dismissal();

    std::weak_ptr<Impl> weak = impl;
    impl->teardown_id = scope.add_teardown([weak] {
        if (auto self = weak.lock())
            self->dismiss(std::nullopt);
    });
    return std::unique_ptr<PopupWindow>(new PopupWindow(std::move(impl)));
}

NativeViewHandle PopupWindow::content_view() const {
    return (__bridge void*)impl_->content;
}

NativeWindowHandle PopupWindow::native_window() const {
    return (__bridge void*)impl_->panel;
}

void PopupWindow::set_size(float width, float height) {
    impl_->options.width = width;
    impl_->options.height = height;
    if (impl_->open)
        impl_->place();
}

void PopupWindow::close() {
    impl_->dismiss(PopupDismissReason::client);
}

bool PopupWindow::is_open() const {
    return impl_->open;
}

PopupRect PopupWindow::screen_frame() const {
    return to_top_left(impl_->panel.frame);
}

float PopupWindow::backing_scale() const {
    return static_cast<float>(impl_->panel.backingScaleFactor);
}

} // namespace pulp::platform
