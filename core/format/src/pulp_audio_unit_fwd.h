// Private header: forward-declare PulpAudioUnit + Pulp accessor selectors so
// the AU v3 view controllers (`au_view_controller_mac.mm`,
// `au_view_controller_ios.mm`) can instantiate the AU and reach the shared
// Processor / StateStore the audio render block runs against.
//
// `PulpAudioUnit`'s @interface lives in `au_adapter.mm` (single TU,
// no separate .h) — we re-declare the public surface here.

#pragma once

#if defined(__APPLE__) && defined(__OBJC__)

#import <AudioToolbox/AudioToolbox.h>

namespace pulp {
namespace format {
class Processor;
}
namespace state {
class StateStore;
}
}

@interface PulpAudioUnit : AUAudioUnit
- (pulp::format::Processor *)pulpProcessor;
- (pulp::state::StateStore *)pulpStore;
@end

#endif // __APPLE__ && __OBJC__
