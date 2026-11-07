// macOS backends for alerts and open_url: an NSAlert sheet on the parent's
// window, and NSWorkspace.

#include "../../src/dialog_internal.hpp"

#import <Cocoa/Cocoa.h>

#include <memory>

namespace pulp::platform::detail {

namespace {

NSString* ns(const std::string& text) {
    NSString* s = [NSString stringWithUTF8String:text.c_str()];
    return s != nil ? s : @"";
}

// Keeps an Objective-C object alive for as long as any copy of the holder
// exists (the completion block and the closer each hold one).
struct Retained {
    id object = nil;
    explicit Retained(id o) : object([o retain]) {}
    ~Retained() {
        [object release];
    }
    Retained(const Retained&) = delete;
    Retained& operator=(const Retained&) = delete;
};

} // namespace

std::function<void()> present_native_alert(NativeViewHandle parent, const AlertRequest& request,
                                           const AlertButtonPlan& plan,
                                           std::function<void(long button_index)> respond) {
    NSView* view = (__bridge NSView*)parent;
    NSWindow* window = view != nil ? view.window : nil;
    if (window == nil || window.attachedSheet != nil || plan.titles.empty())
        return {};

    NSAlert* alert = [[NSAlert alloc] init];
    auto holder = std::make_shared<Retained>(alert);
    [alert release];
    alert.messageText = ns(request.title);
    alert.informativeText = ns(request.message);
    switch (request.style) {
    case AlertStyle::informational:
        alert.alertStyle = NSAlertStyleInformational;
        break;
    case AlertStyle::warning:
        alert.alertStyle = NSAlertStyleWarning;
        break;
    case AlertStyle::critical:
        alert.alertStyle = NSAlertStyleCritical;
        break;
    }
    for (std::size_t i = 0; i < plan.titles.size(); ++i) {
        NSButton* button = [alert addButtonWithTitle:ns(plan.titles[i])];
        if (i == plan.escape_index && i != 0)
            button.keyEquivalent = @"\033";
    }

    auto box = std::make_shared<std::function<void(long)>>(std::move(respond));
    [alert beginSheetModalForWindow:window
                  completionHandler:^(NSModalResponse code) {
                    (void)holder;
                    (*box)(static_cast<long>(code - NSAlertFirstButtonReturn));
                  }];

    const NSModalResponse escape_code =
        NSAlertFirstButtonReturn + static_cast<NSModalResponse>(plan.escape_index);
    auto window_holder = std::make_shared<Retained>(window);
    return [holder, window_holder, escape_code] {
        NSAlert* a = holder->object;
        NSWindow* w = window_holder->object;
        if (w.attachedSheet == a.window)
            [w endSheet:a.window returnCode:escape_code];
    };
}

bool native_open_url(const std::string& url) {
    @autoreleasepool {
        NSURL* target = [NSURL URLWithString:ns(url)];
        return target != nil && [[NSWorkspace sharedWorkspace] openURL:target];
    }
}

} // namespace pulp::platform::detail
