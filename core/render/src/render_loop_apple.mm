// iOS RenderLoop — CADisplayLink-backed vblank source.
//
// Mirrors the macOS CVDisplayLink path in render_loop.cpp but uses
// CADisplayLink, which is the iOS-native vsync callback (CVDisplayLink is
// macOS-only).
//
// The CADisplayLink is added to the main run loop in NSRunLoopCommonModes
// so it keeps ticking during scroll/track tracking. Its target fires on
// the main thread, so the frame callback runs on the main thread directly
// (no dispatch_async needed, unlike the macOS path whose callback fires on
// a CV thread). RenderLoopState is the shared dirty-flag coalescer.
//
// LIFETIME — IOSRenderLoop MUST be created, stopped, and destroyed on the
// main thread. The CADisplayLink fires its selector on [NSRunLoop
// mainRunLoop], so as long as start()/stop()/the destructor also run on
// the main thread the selector can never be mid-flight while the C++
// object is being torn down — there is no concurrency window. stop()
// clears the ObjC target's back-pointer BEFORE invalidating the link, so
// even a same-runloop re-entrant tick is a safe no-op.

#include <pulp/render/render_loop.hpp>
#include "render_loop_state.hpp"

#import <TargetConditionals.h>

#if defined(__APPLE__) && TARGET_OS_IPHONE && !defined(PULP_RENDER_LOOP_FORCE_TIMER)

#import <QuartzCore/QuartzCore.h>
#import <Foundation/Foundation.h>

namespace pulp::render {
class IOSRenderLoop;
}

// ObjC target for the CADisplayLink selector. CADisplayLink retains its
// target, so this holds only a raw back-pointer to the C++ loop; the C++
// loop clears it in stop()/dtor before the link is invalidated.
@interface PulpRenderLoopDisplayLinkTarget : NSObject {
@public
    pulp::render::IOSRenderLoop* loop;
}
- (void)tick:(CADisplayLink*)link;
@end

namespace pulp::render {

class IOSRenderLoop : public RenderLoop {
public:
    ~IOSRenderLoop() override { stop(); }

    void start(FrameCallback on_frame) override {
        if (!state_.start()) {
            return;
        }
        callback_ = std::move(on_frame);

        @autoreleasepool {
            target_ = [[PulpRenderLoopDisplayLinkTarget alloc] init];
            target_->loop = this;
            display_link_ = [CADisplayLink displayLinkWithTarget:target_
                                                        selector:@selector(tick:)];
            [display_link_ addToRunLoop:[NSRunLoop mainRunLoop]
                                forMode:NSRunLoopCommonModes];
        }
    }

    void stop() override {
        state_.stop();
        @autoreleasepool {
            // Clear the target's back-pointer FIRST so any selector that
            // somehow runs between here and invalidate observes a nil loop
            // and no-ops, then invalidate and drop the link + target.
            if (target_) {
                target_->loop = nullptr;
            }
            if (display_link_) {
                [display_link_ invalidate];
                display_link_ = nil;
            }
            target_ = nil;
        }
    }

    void request_frame() override {
        state_.request_frame();
    }

    bool is_running() const override { return state_.is_running(); }

    RenderLoopBackend backend() const override {
        return RenderLoopBackend::ca_display_link;
    }

    // Invoked from the CADisplayLink target on the main thread.
    void on_vsync() {
        if (state_.consume_frame_request() && state_.is_running()) {
            if (callback_) callback_();
        }
    }

private:
    CADisplayLink* display_link_ = nil;
    PulpRenderLoopDisplayLinkTarget* target_ = nil;
    FrameCallback callback_;
    RenderLoopState state_;
};

std::unique_ptr<RenderLoop> make_ios_render_loop() {
    return std::make_unique<IOSRenderLoop>();
}

} // namespace pulp::render

@implementation PulpRenderLoopDisplayLinkTarget
- (void)tick:(CADisplayLink*)link {
    (void)link;
    if (loop) {
        loop->on_vsync();
    }
}
@end

#endif // iOS
