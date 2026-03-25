// Audio Unit v2 component entry point
// Provides the AudioComponentFactoryFunction that the AU host calls

#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>

// Forward declaration of our AUAudioUnit subclass
@class PulpAudioUnit;

// The factory function — called by the AU host to create instances
extern "C" void* PulpAUFactory(const AudioComponentDescription* desc) {
    // AUv3-in-v2 wrapper: create the AUAudioUnit subclass
    // The host provides the component description, we return an AUAudioUnit
    return (__bridge_retained void*)[[PulpAudioUnit alloc]
        initWithComponentDescription:*desc
        options:0
        error:nil];
}
