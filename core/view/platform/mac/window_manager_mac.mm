#include <pulp/view/window_manager.hpp>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if !TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR

#import <AppKit/AppKit.h>

namespace pulp::view {

std::vector<ScreenInfo> WindowManager::available_screens() {
    std::vector<ScreenInfo> result;
    NSArray<NSScreen*>* screens = [NSScreen screens];
    NSScreen* mainScreen = [NSScreen mainScreen];

    int idx = 0;
    for (NSScreen* screen in screens) {
        ScreenInfo info;
        info.id = idx;

        NSRect frame = [screen frame];
        info.x = static_cast<float>(frame.origin.x);
        info.y = static_cast<float>(frame.origin.y);
        info.width = static_cast<float>(frame.size.width);
        info.height = static_cast<float>(frame.size.height);
        info.scale_factor = static_cast<float>([screen backingScaleFactor]);
        info.is_primary = (screen == mainScreen);

        NSString* name = [screen localizedName];
        if (name)
            info.name = [name UTF8String];
        else
            info.name = "Screen " + std::to_string(idx);

        result.push_back(info);
        ++idx;
    }

    return result;
}

ScreenInfo WindowManager::primary_screen() {
    auto screens = available_screens();
    for (auto& s : screens) {
        if (s.is_primary) return s;
    }
    return screens.empty() ? ScreenInfo{} : screens[0];
}

} // namespace pulp::view

#endif // !TARGET_OS_IPHONE
#endif // __APPLE__
