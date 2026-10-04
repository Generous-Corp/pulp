// window_host_mac_metal_view.mm — CAMetalLayer-backed NSView seam for the
// standalone GPU host. It deliberately owns no MacGpuWindowHost state.

#include "window_host_mac_metal_view.h"
#include "window_host_mac_internal.hpp"

#if TARGET_OS_OSX && defined(PULP_HAS_SKIA)

#include <Foundation/Foundation.h>

@implementation PulpMetalView

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.hostOwnsRootLayout = YES;
        self.wantsLayer = YES;

        CAMetalLayer* layer = [CAMetalLayer layer];
        layer.device = MTLCreateSystemDefaultDevice();
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = NO;
        CGFloat scale = self.window ? self.window.backingScaleFactor
                                    : [NSScreen mainScreen].backingScaleFactor;
        layer.contentsScale = scale;
        CGSize backing = NSMakeSize(frame.size.width * scale, frame.size.height * scale);
        layer.drawableSize = backing;

        layer.opaque = YES;
        layer.backgroundColor = pulp::view::mac_host::cg_host_clear_color();
        layer.contentsGravity = kCAGravityTopLeft;

        self.layer = layer;
        _metalLayer = layer;
    }
    return self;
}

- (BOOL)wantsUpdateLayer { return YES; }

- (void)updateLayer {
    // Metal frames are produced by MacGpuWindowHost::render_frame from its
    // display-link callback; AppKit must not paint over the retained layer.
}

- (void)drawRect:(NSRect)dirtyRect { (void)dirtyRect; }

- (void)setNeedsDisplay:(BOOL)needsDisplay {
    [super setNeedsDisplay:needsDisplay];
    if (needsDisplay && self.repaintBlock) self.repaintBlock();
}

- (void)setFrameSize:(NSSize)newSize {
    const NSSize oldSize = self.frame.size;
    self.resizeCoverGeneration += 1;
    const BOOL expandingLiveResize =
        (self.inLiveResize || self.pulpLiveResizeActive) &&
        (newSize.width > oldSize.width || newSize.height > oldSize.height);
    const BOOL unchangedLiveResize =
        (self.inLiveResize || self.pulpLiveResizeActive) &&
        newSize.width == oldSize.width && newSize.height == oldSize.height;
    if (expandingLiveResize) {
        self.metalLayer.contentsGravity = kCAGravityResize;
    } else if (!unchangedLiveResize) {
        self.metalLayer.contentsGravity = kCAGravityTopLeft;
    }
    [super setFrameSize:newSize];
    CGFloat scale = self.window ? self.window.backingScaleFactor
                                : [NSScreen mainScreen].backingScaleFactor;
    self.metalLayer.contentsScale = scale;
    self.metalLayer.drawableSize = CGSizeMake(newSize.width * scale,
                                              newSize.height * scale);
}

- (void)viewWillStartLiveResize {
    [super viewWillStartLiveResize];
    self.pulpLiveResizeActive = YES;
}

- (void)viewDidEndLiveResize {
    [super viewDidEndLiveResize];
    self.pulpLiveResizeActive = NO;
    [self releaseResizeCoverAfterPresent];
}

- (void)releaseResizeCoverAfterPresent {
    const NSUInteger generation = self.resizeCoverGeneration;
    PulpMetalView* view = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 64 * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{
        if (view.resizeCoverGeneration == generation)
            view.metalLayer.contentsGravity = kCAGravityTopLeft;
    });
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    CGFloat scale = self.window ? self.window.backingScaleFactor
                                : [NSScreen mainScreen].backingScaleFactor;
    self.metalLayer.contentsScale = scale;
    CGSize backing = CGSizeMake(self.bounds.size.width * scale,
                                self.bounds.size.height * scale);
    self.metalLayer.drawableSize = backing;
    if (self.backingChangedBlock) self.backingChangedBlock();
}

@end

#endif  // TARGET_OS_OSX && defined(PULP_HAS_SKIA)
