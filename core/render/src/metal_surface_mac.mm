// Per-binary-unique ObjC class names (see header).
#include "pulp_render_objc_names.h"
// macOS Metal surface — provides a CAMetalLayer-backed NSView for GPU presentation.
//
// Ownership:
//   Platform (view host) creates this surface and owns the NSView.
//   Render (GpuSurface) receives the CAMetalLayer handle for Dawn surface creation.
//   View (paint traversal) does not touch Metal objects.

#include <TargetConditionals.h>
#if TARGET_OS_OSX

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>

#include <pulp/runtime/log.hpp>

// ── PulpMetalView: NSView backed by CAMetalLayer ─────────────────────────────

@interface PulpMetalView : NSView
@property (nonatomic, readonly) CAMetalLayer* metalLayer;
@end

@implementation PulpMetalView

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.wantsLayer = YES;

        // Create and configure the Metal layer
        CAMetalLayer* layer = [CAMetalLayer layer];
        layer.device = MTLCreateSystemDefaultDevice();
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = YES;
        layer.contentsScale = self.window ? self.window.backingScaleFactor : [NSScreen mainScreen].backingScaleFactor;

        // Size the drawable to match the view
        CGSize size = [self convertSizeToBacking:frame.size];
        layer.drawableSize = size;

        self.layer = layer;
        _metalLayer = layer;
    }
    return self;
}

- (BOOL)isFlipped { return YES; }

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    // Update drawable size on resize (accounting for Retina scale)
    CGSize backing = [self convertSizeToBacking:newSize];
    self.metalLayer.drawableSize = backing;
    self.metalLayer.contentsScale = self.window ? self.window.backingScaleFactor : [NSScreen mainScreen].backingScaleFactor;
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    // Handle Retina scale changes (e.g., dragging between displays)
    CGFloat scale = self.window ? self.window.backingScaleFactor : [NSScreen mainScreen].backingScaleFactor;
    self.metalLayer.contentsScale = scale;
    CGSize backing = [self convertSizeToBacking:self.bounds.size];
    self.metalLayer.drawableSize = backing;
}

@end

// ── C++ interface for creating macOS Metal views ─────────────────────────────

namespace pulp::render::mac {

struct MacMetalSurface {
    PulpMetalView* view = nil;

    bool create(float width, float height) {
        @autoreleasepool {
            NSRect frame = NSMakeRect(0, 0, width, height);
            view = [[PulpMetalView alloc] initWithFrame:frame];
            if (!view || !view.metalLayer || !view.metalLayer.device) {
                runtime::log_error("MacMetalSurface: failed to create Metal-backed view");
                return false;
            }
            runtime::log_info("MacMetalSurface: created {}x{} Metal view (scale {:.1f})",
                static_cast<int>(width), static_cast<int>(height),
                view.metalLayer.contentsScale);
            return true;
        }
    }

    void* native_layer() const {
        return (__bridge void*)view.metalLayer;
    }

    float scale_factor() const {
        return static_cast<float>(view.metalLayer.contentsScale);
    }

    void resize(float width, float height) {
        if (!view) return;
        [view setFrameSize:NSMakeSize(width, height)];
    }

    void detach() {
        if (view) {
            [view removeFromSuperview];
            view = nil;
        }
    }
};

} // namespace pulp::render::mac

#endif // TARGET_OS_OSX
