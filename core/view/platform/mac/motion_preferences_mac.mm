#import <AppKit/AppKit.h>
#include <pulp/view/motion_preferences.hpp>

namespace pulp::view::platform {

MotionPolicy detect_mac_motion_policy() {
    @autoreleasepool {
        // NSWorkspace exposes the reduce-motion accessibility setting via
        // `accessibilityDisplayShouldReduceMotion` (NSAccessibilityReduceMotion).
        // Returns YES when the user has asked the system to reduce motion.
        if (@available(macOS 10.12, *)) {
            NSWorkspace* ws = [NSWorkspace sharedWorkspace];
            if (ws.accessibilityDisplayShouldReduceMotion) {
                return MotionPolicy::Reduced;
            }
        }
        return MotionPolicy::Full;
    }
}

} // namespace pulp::view::platform
