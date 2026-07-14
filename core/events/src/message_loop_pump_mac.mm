#include <pulp/events/message_loop_integration.hpp>

#import <CoreFoundation/CoreFoundation.h>
#import <Foundation/Foundation.h>

#include <algorithm>

namespace pulp::events {

MainLoopPumpResult MessageLoopIntegration::pump_main_loop_for(
    std::chrono::milliseconds max_duration) {
    if (![NSThread isMainThread]) {
        return MainLoopPumpResult::WrongThread;
    }

    const auto bounded = std::max(max_duration, std::chrono::milliseconds::zero());
    const auto seconds = std::chrono::duration<double>(bounded).count();
    const auto result = CFRunLoopRunInMode(kCFRunLoopDefaultMode,
                                           seconds,
                                           /*returnAfterSourceHandled=*/true);
    switch (result) {
        case kCFRunLoopRunHandledSource:
            return MainLoopPumpResult::HandledSource;
        case kCFRunLoopRunTimedOut:
            return MainLoopPumpResult::TimedOut;
        case kCFRunLoopRunStopped:
            return MainLoopPumpResult::Stopped;
        case kCFRunLoopRunFinished:
            return MainLoopPumpResult::Finished;
        default:
            return MainLoopPumpResult::Finished;
    }
}

} // namespace pulp::events
