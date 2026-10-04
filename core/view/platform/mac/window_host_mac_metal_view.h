#pragma once

#include <TargetConditionals.h>
#if TARGET_OS_OSX && defined(__OBJC__) && defined(PULP_HAS_SKIA)

#include "window_host_mac_view.h"

#import <QuartzCore/CAMetalLayer.h>

// Private CAMetalLayer-backed view used only by MacGpuWindowHost. Its
// implementation is separate because it owns no host state or PulpView ivars.
@interface PulpMetalView : PulpView
@property (nonatomic, readonly) CAMetalLayer* metalLayer;
@property (nonatomic, copy) dispatch_block_t repaintBlock;
@property (nonatomic, copy) dispatch_block_t backingChangedBlock;
@property (nonatomic) BOOL pulpLiveResizeActive;
@property (nonatomic) NSUInteger resizeCoverGeneration;
- (void)releaseResizeCoverAfterPresent;
@end

#endif  // TARGET_OS_OSX && defined(__OBJC__) && defined(PULP_HAS_SKIA)
