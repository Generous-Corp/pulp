#include <catch2/catch_test_macros.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/view.hpp>

#import <AppKit/AppKit.h>

#include <cstdlib>
#include <cmath>
#include <memory>
#include <optional>
#include <string>

namespace {
class EditorEnvironment {
 public:
    EditorEnvironment() {
        for (auto& entry : entries_) {
            if (const char* value = std::getenv(entry.name)) entry.value = value;
            unsetenv(entry.name);
        }
    }
    ~EditorEnvironment() {
        for (const auto& entry : entries_) {
            if (entry.value) setenv(entry.name, entry.value->c_str(), 1);
            else unsetenv(entry.name);
        }
    }
 private:
    struct Entry { const char* name; std::optional<std::string> value; };
    Entry entries_[4] = {{"CI", {}}, {"PULP_TEST_MODE", {}}, {"PULP_HEADLESS", {}},
                       {"PULP_DISABLE_PLUGIN_EDITOR", {}}};
};

class OrientationView final : public pulp::view::View {
 public:
    void paint(pulp::canvas::Canvas& canvas) override {
        canvas.set_fill_color(pulp::canvas::Color::rgba8(0, 0, 0));
        canvas.fill_rect(0, 0, 180, 140);
        canvas.set_fill_color(pulp::canvas::Color::rgba8(255, 0, 0));
        canvas.fill_rect(10, 10, 30, 20);
        canvas.set_fill_color(pulp::canvas::Color::rgba8(0, 0, 255));
        canvas.fill_rect(10, 110, 30, 20);
        // Imported editors replace their local transform after capturing the
        // host baseline. That operation must retain the same top-left origin.
        canvas.capture_paint_baseline_transform();
        canvas.save();
        canvas.set_transform(0.5f, 0, 0, 0.5f, 50, 20);
        canvas.set_fill_color(pulp::canvas::Color::rgba8(0, 255, 0));
        canvas.fill_rect(80, 40, 40, 20);
        canvas.restore();
        ++paints;
    }
    int paints = 0;
};

void require_rgb(NSBitmapImageRep* bitmap, int x, int y, int r, int g, int b) {
    NSColor* color = [[bitmap colorAtX:x y:y] colorUsingColorSpace:NSColorSpace.deviceRGBColorSpace];
    REQUIRE(color != nil);
    INFO("pixel " << x << ',' << y << " RGB=" << color.redComponent << ','
                  << color.greenComponent << ',' << color.blueComponent);
    // AppKit color-manages the capture for the display profile. Assert the
    // dominant primary, preserving orientation proof across sRGB/P3 displays.
    REQUIRE((r ? color.redComponent > 0.8 : color.redComponent < 0.3));
    REQUIRE((g ? color.greenComponent > 0.8 : color.greenComponent < 0.3));
    REQUIRE((b ? color.blueComponent > 0.8 : color.blueComponent < 0.3));
}
}  // namespace

TEST_CASE("mac CPU plugin host preserves top-left coordinates through native drawing",
          "[plugin-host][mac][orientation]") {
    REQUIRE([NSThread isMainThread]);
    EditorEnvironment environment;
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSWindow* window = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(0, 0, 180, 140)
                      styleMask:NSWindowStyleMaskBorderless
                        backing:NSBackingStoreBuffered defer:NO];
        REQUIRE(window != nil);
        [window setReleasedWhenClosed:NO];
        OrientationView root;
        pulp::view::PluginViewHost::Options options;
        options.size = {180, 140};
        options.use_gpu = false;
        auto host = pulp::view::PluginViewHost::create(root, options);
        REQUIRE(host != nullptr);
        REQUIRE_FALSE(host->is_gpu_backed());
        REQUIRE(host->try_attach_to_parent((__bridge void*)window.contentView));
        NSView* native = (__bridge NSView*)host->native_handle();
        REQUIRE(native != nil);
        REQUIRE_FALSE(native.isFlipped);
        NSBitmapImageRep* bitmap = [native bitmapImageRepForCachingDisplayInRect:native.bounds];
        REQUIRE(bitmap != nil);
        [native cacheDisplayInRect:native.bounds toBitmapImageRep:bitmap];
        REQUIRE(root.paints > 0);
        const float sx = float(bitmap.pixelsWide) / 180;
        const float sy = float(bitmap.pixelsHigh) / 140;
        require_rgb(bitmap, int(20 * sx), int(20 * sy), 255, 0, 0);
        require_rgb(bitmap, int(20 * sx), int(120 * sy), 0, 0, 255);
        require_rgb(bitmap, int(95 * sx), int(45 * sy), 0, 255, 0);
        host->detach();
        host.reset();
        [window close];
#if !__has_feature(objc_arc)
        [window release];
#endif
    }
}
