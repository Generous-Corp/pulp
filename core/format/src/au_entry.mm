// Audio Unit component entry point
// Uses AUAudioUnit.registerSubclass to register the AU class directly.

#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>
#import "au_audio_unit.h"
#include <pulp/format/registry.hpp>
#include <pulp/runtime/log.hpp>

// The factory function called by AudioComponentInstanceNew.
// For AUv3-in-v2, this should return a factory object that conforms
// to AUAudioUnitFactory. The factory's createAudioUnit: method is
// then called to produce the actual AUAudioUnit instance.

@interface PulpAUFactoryObj : NSObject <AUAudioUnitFactory>
@end

@implementation PulpAUFactoryObj

- (nullable AUAudioUnit *)createAudioUnitWithComponentDescription:(AudioComponentDescription)desc
                                                            error:(NSError * _Nullable *)error
{
    fprintf(stderr, "[pulp:AU] createAudioUnit called\n");
    return [[PulpAudioUnit alloc] initWithComponentDescription:desc
                                                       options:0
                                                         error:error];
}

@end

// Force linker to include au_register.cpp (prevents static init stripping)
extern "C" void pulp_gain_force_link();

extern "C" void* PulpAUFactory(const AudioComponentDescription* desc) {
    // Touch the force_link symbol to ensure au_register.cpp is linked
    pulp_gain_force_link();

    if (!pulp::format::registered_factory()) {
        fprintf(stderr, "[pulp:AU] ERROR: factory still null after force_link\n"); fflush(stderr);
        return nullptr;
    }

    fprintf(stderr, "[pulp:AU] factory OK, creating AUAudioUnitFactory\n"); fflush(stderr);
    return (__bridge_retained void*)[[PulpAUFactoryObj alloc] init];
}
