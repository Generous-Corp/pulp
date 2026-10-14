// A hosted plug-in editor view accepts file and text drops.
//
// The drop delivery is an NSDraggingDestination category on the hosted NSViews,
// and an Objective-C category defines no symbol of its own. Linked from a static
// library, its object file reaches the binary only when something else in that
// object is referenced, so a category split away from the code that uses it can
// silently drop out: the view still draws and clicks, and a dragged file simply
// never arrives. This links the editor hosts from their library (no per-binary
// unity copy) and checks both halves on the CPU and GPU hosts: the view registers
// the dragged types once it joins a window, and its class carries the drop
// methods from the category.

#include <TargetConditionals.h>

#if TARGET_OS_OSX

#import <AppKit/AppKit.h>
#include <objc/runtime.h>

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/view.hpp>

#include <cstdlib>

using namespace pulp::view;

namespace {

bool class_defines(Class cls, SEL selector) {
    unsigned int count = 0;
    Method* methods = class_copyMethodList(cls, &count);
    bool found = false;
    for (unsigned int i = 0; i < count && !found; ++i)
        found = method_getName(methods[i]) == selector;
    std::free(methods);
    return found;
}

void check_hosted_view_accepts_drops(bool use_gpu) {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

    View root;
    root.set_bounds({0, 0, 200, 120});
    PluginViewHost::Options options;
    options.size = {200, 120};
    options.use_gpu = use_gpu;
    auto host = PluginViewHost::create(root, options);
    REQUIRE(host != nullptr);

    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(40, 40, 200, 120)
                                                   styleMask:NSWindowStyleMaskTitled
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    window.alphaValue = 0.0;
    window.releasedWhenClosed = NO;
    host->attach_to_parent((__bridge void*)window.contentView);
    NSView* view = (__bridge NSView*)host->native_handle();
    REQUIRE(view != nil);
    REQUIRE(view.window == window);

    NSArray<NSPasteboardType>* types = view.registeredDraggedTypes;
    CHECK([types containsObject:NSPasteboardTypeFileURL]);
    CHECK([types containsObject:NSPasteboardTypeString]);

    // Control: the class really is the hosted editor view, which defines its
    // own mouse handling; the drop methods must come from the drag category.
    REQUIRE(class_defines([view class], @selector(mouseDown:)));
    CHECK(class_defines([view class], @selector(draggingEntered:)));
    CHECK(class_defines([view class], @selector(performDragOperation:)));
    CHECK(class_defines([view class], @selector(draggingExited:)));

    host->detach();
    [window orderOut:nil];
}

} // namespace

TEST_CASE("a hosted editor view accepts file and text drops (CPU host)",
          "[view][drag-drop][macos]") {
    @autoreleasepool {
        check_hosted_view_accepts_drops(/*use_gpu=*/false);
    }
}

TEST_CASE("a hosted editor view accepts file and text drops (GPU host)",
          "[view][drag-drop][macos]") {
    @autoreleasepool {
        check_hosted_view_accepts_drops(/*use_gpu=*/true);
    }
}

#endif // TARGET_OS_OSX
