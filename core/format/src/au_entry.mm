// Audio Unit component entry point
// The processor factory must be registered via PULP_REGISTER_PLUGIN before this is called.

#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>
#include <pulp/format/registry.hpp>
#include <pulp/runtime/log.hpp>

@class PulpAudioUnit;

extern "C" void* PulpAUFactory(const AudioComponentDescription* desc) {
    if (!pulp::format::registered_factory()) {
        pulp::runtime::log_error("AU: no processor registered — use PULP_REGISTER_PLUGIN");
        return nullptr;
    }

    NSError* error = nil;
    PulpAudioUnit* unit = [[PulpAudioUnit alloc]
        initWithComponentDescription:*desc
        options:0
        error:&error];

    if (error) {
        pulp::runtime::log_error("AU: init error: {}",
            [error.localizedDescription UTF8String]);
        return nullptr;
    }

    return (__bridge_retained void*)unit;
}
