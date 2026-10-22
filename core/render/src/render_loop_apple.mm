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
// FRAME RATE — while frames are being drawn the link requests the screen's
// maximum rate (120 Hz on ProMotion; iOS otherwise holds it at 60 Hz), and it
// returns to the system default once DisplayRateBoost sees the loop go idle.
// apply_display_rate_change(), defined at the end of this file, is shared with
// the iOS window and plug-in view hosts, which own their own display links.
// An iPhone app also needs CADisableMinimumFrameDurationOnPhone in its
// Info.plist for the request to take effect; the iOS host template sets it.
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
        apply_display_rate_change((__bridge void*)display_link_, rate_.on_stop());
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
        const bool draw = state_.consume_frame_request() && state_.is_running();
        apply_display_rate_change((__bridge void*)display_link_, rate_.on_vsync(draw));
        if (draw && callback_)
            callback_();
    }

private:
    CADisplayLink* display_link_ = nil;
    PulpRenderLoopDisplayLinkTarget* target_ = nil;
    FrameCallback callback_;
    RenderLoopState state_;
    DisplayRateBoost rate_;
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

#if defined(__APPLE__) && TARGET_OS_IPHONE

#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>

namespace pulp::render {

void apply_display_rate_change(void* ca_display_link, DisplayRateBoost::Change change) {
    if (change == DisplayRateBoost::Change::none || !ca_display_link)
        return;
    if (@available(iOS 15.0, *)) {
        CADisplayLink* link = (__bridge CADisplayLink*)ca_display_link;
        CAFrameRateRange range = CAFrameRateRangeDefault;
        if (change == DisplayRateBoost::Change::raise) {
            // [UIScreen mainScreen] is deprecated in iOS 26 in favour of a
            // window's scene screen; a display link has no window, and the
            // panel cap is the same for every scene on a single-screen device.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            UIScreen* screen = [UIScreen mainScreen];
#pragma clang diagnostic pop
            const float max_fps = screen ? static_cast<float>(screen.maximumFramesPerSecond) : 0.0f;
            if (max_fps <= 60.0f)
                return;
            range = CAFrameRateRangeMake(60.0f, max_fps, max_fps);
        }
        link.preferredFrameRateRange = range;
    }
}

} // namespace pulp::render

#endif // TARGET_OS_IPHONE
