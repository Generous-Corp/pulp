// iOS Metal surface — CAMetalLayer on UIView for Dawn/WebGPU rendering
// Provides the native Metal layer that Dawn uses as its swap chain target.

#if TARGET_OS_IOS

#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#include <pulp/render/gpu_surface.hpp>
#include <pulp/runtime/log.hpp>

// ── PulpMetalView: UIView backed by CAMetalLayer ────────────────────────────

@interface PulpMetalView : UIView
@end

@implementation PulpMetalView

+ (Class)layerClass {
    return [CAMetalLayer class];
}

- (CAMetalLayer *)metalLayer {
    return (CAMetalLayer *)self.layer;
}

@end

// ── IOSMetalSurface ─────────────────────────────────────────────────────────

namespace pulp::render {

/// A GPU surface backed by a CAMetalLayer on iOS.
/// This view can be embedded in the view hierarchy and provides the
/// native Metal layer handle for Dawn to create its swap chain.
class IOSMetalSurface {
public:
    IOSMetalSurface() = default;

    ~IOSMetalSurface() {
        @autoreleasepool {
            if (view_) {
                [view_ removeFromSuperview];
                view_ = nil;
            }
        }
    }

    /// Create the Metal-backed UIView and configure the layer.
    /// Returns false if Metal is not available on this device.
    bool initialize(uint32_t width, uint32_t height) {
        @autoreleasepool {
            // Check Metal availability
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            if (!device) {
                runtime::log_error("iOS Metal: no GPU device available");
                return false;
            }

            CGRect frame = CGRectMake(0, 0, width, height);
            view_ = [[PulpMetalView alloc] initWithFrame:frame];
            view_.multipleTouchEnabled = YES;

            CAMetalLayer *layer = [view_ metalLayer];
            layer.device = device;
            layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            layer.framebufferOnly = YES;
            layer.drawableSize = CGSizeMake(width, height);

            // Use display scale for retina
            CGFloat scale = UIScreen.mainScreen.scale;
            layer.contentsScale = scale;

            width_ = width;
            height_ = height;
            initialized_ = true;

            runtime::log_info("iOS Metal: surface {}x{} @ {}x scale", width, height, scale);
            return true;
        }
    }

    /// Resize the drawable surface.
    void resize(uint32_t width, uint32_t height) {
        @autoreleasepool {
            width_ = width;
            height_ = height;
            if (view_) {
                CAMetalLayer *layer = [view_ metalLayer];
                layer.drawableSize = CGSizeMake(width, height);
                view_.frame = CGRectMake(0, 0, width, height);
            }
        }
    }

    /// Get the UIView for embedding in the view hierarchy.
    void* native_view() const { return (__bridge void*)view_; }

    /// Get the CAMetalLayer for Dawn surface creation.
    void* metal_layer() const {
        return (__bridge void*)[view_ metalLayer];
    }

    bool is_initialized() const { return initialized_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    /// Apply safe area insets to the rendering viewport.
    /// Returns the usable rect after insets are applied.
    struct InsetRect { float x, y, width, height; };

    InsetRect safe_area_rect() const {
        if (!view_) return {0, 0, static_cast<float>(width_), static_cast<float>(height_)};

        @autoreleasepool {
            UIEdgeInsets insets = view_.safeAreaInsets;
            float sx = static_cast<float>(insets.left);
            float sy = static_cast<float>(insets.top);
            float sw = static_cast<float>(width_) - sx - static_cast<float>(insets.right);
            float sh = static_cast<float>(height_) - sy - static_cast<float>(insets.bottom);
            return {sx, sy, std::max(0.0f, sw), std::max(0.0f, sh)};
        }
    }

private:
    PulpMetalView* view_ = nil;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool initialized_ = false;
};

} // namespace pulp::render

#endif // TARGET_OS_IOS
