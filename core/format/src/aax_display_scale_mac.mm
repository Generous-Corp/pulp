#include <pulp/format/aax_display_scale.hpp>

#import <AppKit/AppKit.h>

namespace pulp::format::aax {

float aax_display_scale(void* native_parent) noexcept {
    @autoreleasepool {
        NSView* view = (__bridge NSView*)native_parent;
        NSWindow* window = view.window;
        if (window) return static_cast<float>(window.backingScaleFactor);
        NSScreen* screen = NSScreen.mainScreen;
        return screen ? static_cast<float>(screen.backingScaleFactor) : 1.0f;
    }
}

} // namespace pulp::format::aax
