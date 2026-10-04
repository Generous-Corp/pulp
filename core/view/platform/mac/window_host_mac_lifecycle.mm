// window_host_mac_lifecycle.mm — process/window lifecycle seams shared by the
// standalone CPU and GPU macOS hosts.

#include "window_host_mac_lifecycle.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

namespace {

std::mutex& cocoa_dispatcher_liveness_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<std::weak_ptr<std::atomic<bool>>>& cocoa_dispatcher_liveness_tokens() {
    static std::vector<std::weak_ptr<std::atomic<bool>>> tokens;
    return tokens;
}

void mark_cocoa_dispatchers_stopping() {
    auto& mutex = cocoa_dispatcher_liveness_mutex();
    auto& tokens = cocoa_dispatcher_liveness_tokens();
    std::lock_guard lock(mutex);
    tokens.erase(std::remove_if(tokens.begin(), tokens.end(),
                                [](const auto& token) {
                                    auto alive = token.lock();
                                    if (!alive)
                                        return true;
                                    alive->store(false, std::memory_order_release);
                                    return false;
                                }),
                 tokens.end());
}

void post_cocoa_stop_event() {
    NSEvent* event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                        location:NSZeroPoint
                                   modifierFlags:0
                                       timestamp:0
                                    windowNumber:0
                                         context:nil
                                         subtype:0
                                           data1:0
                                           data2:0];
    [NSApp postEvent:event atStart:NO];
}

} // namespace

namespace pulp::view::mac_lifecycle {

void register_cocoa_dispatcher_liveness(const std::shared_ptr<std::atomic<bool>>& alive) {
    auto& mutex = cocoa_dispatcher_liveness_mutex();
    auto& tokens = cocoa_dispatcher_liveness_tokens();
    std::lock_guard lock(mutex);
    tokens.erase(std::remove_if(tokens.begin(), tokens.end(),
                                [](const auto& token) { return token.expired(); }),
                 tokens.end());
    tokens.push_back(alive);
}

void request_cocoa_app_stop() {
    mark_cocoa_dispatchers_stopping();
    dispatch_async(dispatch_get_main_queue(), ^{
      [NSApp stop:nil];
      post_cocoa_stop_event();
    });
}

void request_hidden_cocoa_window_close(NSWindow* window) {
    mark_cocoa_dispatchers_stopping();
    dispatch_async(dispatch_get_main_queue(), ^{
      if (window != nil)
          [window close];
      [NSApp stop:nil];
      post_cocoa_stop_event();
    });
}

void request_cocoa_window_close_deferred(NSWindow* window, bool initially_hidden) {
    dispatch_async(dispatch_get_main_queue(), ^{
      if (initially_hidden) {
          mark_cocoa_dispatchers_stopping();
          if (window != nil)
              [window close];
          [NSApp stop:nil];
          post_cocoa_stop_event();
          return;
      }
      if (window != nil) {
          [window performClose:nil];
      } else {
          mark_cocoa_dispatchers_stopping();
          [NSApp stop:nil];
          post_cocoa_stop_event();
      }
    });
}

pulp::events::MainThreadDispatcher::Backend
make_cocoa_main_thread_backend(std::shared_ptr<std::atomic<bool>> alive) {
    return {
        [alive](pulp::events::Task task) -> bool {
            if (!task)
                return false;
            if (!alive || !alive->load(std::memory_order_acquire))
                return false;
            auto* heap_task = new pulp::events::Task(std::move(task));
            dispatch_async(dispatch_get_main_queue(), ^{
              std::unique_ptr<pulp::events::Task> owned(heap_task);
              if (*owned)
                  (*owned)();
            });
            return true;
        },
        [alive]() -> bool {
            if (!alive || !alive->load(std::memory_order_acquire))
                return false;
            // Explicit -> bool keeps the lambda's return type identical on
            // arm64 and x86_64, where Obj-C BOOL has different signedness.
            return [NSThread isMainThread];
        },
    };
}

void pump_cocoa_main_thread_until(const std::function<bool()>& ready_to_return) {
    if (!ready_to_return)
        return;
    while (!ready_to_return()) {
        @autoreleasepool {
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                     beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
        }
    }
}

} // namespace pulp::view::mac_lifecycle

@implementation PulpWindowDelegate

- (BOOL)windowShouldClose:(NSWindow*)sender {
    if (self.onClose)
        self.onClose();
    [sender orderOut:nil];
    if (!self.isSecondaryWindow)
        pulp::view::mac_lifecycle::request_cocoa_app_stop();
    return YES;
}

- (void)windowDidResize:(NSNotification*)notification {
    if (self.onResize) {
        NSWindow* window = notification.object;
        NSSize size = window.contentView.bounds.size;
        self.onResize(static_cast<float>(size.width), static_cast<float>(size.height));
    }
}

- (NSSize)windowWillResize:(NSWindow*)sender toSize:(NSSize)frameSize {
    if (self.aspectRatio <= 0)
        return frameSize;

    NSRect frameRect = NSMakeRect(0, 0, frameSize.width, frameSize.height);
    NSRect contentRect = [sender contentRectForFrameRect:frameRect];
    CGFloat targetW = contentRect.size.width;
    CGFloat targetH = contentRect.size.height;
    if (targetW <= 0 || targetH <= 0)
        return frameSize;

    NSSize currentContent = sender.contentView.bounds.size;
    CGFloat dw = std::fabs(targetW - currentContent.width);
    CGFloat dh = std::fabs(targetH - currentContent.height);
    if (dw >= dh)
        targetH = targetW / self.aspectRatio;
    else
        targetW = targetH * self.aspectRatio;

    NSRect newContent = NSMakeRect(0, 0, targetW, targetH);
    NSRect newFrame = [sender frameRectForContentRect:newContent];
    return newFrame.size;
}

@end
