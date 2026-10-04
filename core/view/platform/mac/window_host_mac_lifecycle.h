#pragma once

#include <TargetConditionals.h>
#if TARGET_OS_OSX && defined(__OBJC__)

#include "pulp_mac_objc_names.h"

#import <Cocoa/Cocoa.h>

#include <atomic>
#include <functional>
#include <memory>

#include <pulp/events/main_thread_dispatcher.hpp>

// Private interface shared by the CPU and GPU standalone window hosts. The
// implementation lives in window_host_mac_lifecycle.mm so the main host TU
// retains only the host-specific callbacks and PulpView state.
@interface PulpWindowDelegate : NSObject <NSWindowDelegate>
@property (nonatomic, copy) void (^onClose)(void);
@property (nonatomic, copy) void (^onResize)(float, float);
/// Real-time aspect-lock snap during user drag. A non-positive value leaves
/// AppKit's proposed frame size unchanged.
@property (nonatomic, assign) CGFloat aspectRatio;
/// Secondary windows order themselves out on close; a primary window stops the
/// application after its close callback has run.
@property (nonatomic, assign) BOOL isSecondaryWindow;
@end

namespace pulp::view::mac_lifecycle {

void register_cocoa_dispatcher_liveness(
    const std::shared_ptr<std::atomic<bool>>& alive);

void request_cocoa_app_stop();
void request_hidden_cocoa_window_close(NSWindow* window);
void request_cocoa_window_close_deferred(NSWindow* window,
                                         bool initially_hidden);

pulp::events::MainThreadDispatcher::Backend make_cocoa_main_thread_backend(
    std::shared_ptr<std::atomic<bool>> alive);

void pump_cocoa_main_thread_until(const std::function<bool()>& ready_to_return);

}  // namespace pulp::view::mac_lifecycle

#endif  // TARGET_OS_OSX && defined(__OBJC__)
