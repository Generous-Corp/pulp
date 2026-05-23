// AU v3 component entry point — generic AUAudioUnitFactory hook called by
// AudioComponentInstanceNew. Returns a +1-retained NSObject conforming to
// AUAudioUnitFactory whose createAudioUnitWithComponentDescription:error:
// instantiates `PulpAudioUnit`.
//
// Plugin registration: per-plugin via `PULP_AUV3_PLUGIN(factory_fn)` from
// `<pulp/format/au_v3_entry.hpp>`. That macro calls `PULP_REGISTER_PLUGIN`
// in the AU v3 module so `registered_factory()` is non-null by the time this
// entry is invoked. No plugin-specific force-link symbol is required.

#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>
#import "au_audio_unit.h"
#include <pulp/format/registry.hpp>
#include <pulp/runtime/log.hpp>

@interface PulpAUFactoryObj : NSObject <AUAudioUnitFactory>
@end

@implementation PulpAUFactoryObj

- (nullable AUAudioUnit *)createAudioUnitWithComponentDescription:(AudioComponentDescription)desc
                                                            error:(NSError * _Nullable *)error
{
    return [[PulpAudioUnit alloc] initWithComponentDescription:desc
                                                       options:0
                                                         error:error];
}

@end

extern "C" void *PulpAUFactory(const AudioComponentDescription *desc) {
    (void)desc;
    if (!pulp::format::registered_factory()) {
        pulp::runtime::log_error("AU v3 entry: no processor factory registered — "
                                 "did the plugin link a TU with PULP_AUV3_PLUGIN()?");
        return nullptr;
    }
    return (__bridge_retained void *)[[PulpAUFactoryObj alloc] init];
}
