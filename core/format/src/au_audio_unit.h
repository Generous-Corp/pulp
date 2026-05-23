#pragma once

// Forward declaration of the Pulp AUAudioUnit subclass.
// Defined in `au_adapter.mm`. Used by:
//   - `au_entry.mm`                            (iOS factory entry)
//   - `au_view_controller_mac.mm` (Phase 3.5)  (macOS factory + view)
//   - `au_view_controller_ios.mm` (Phase 3.5)  (iOS factory + view)
//   - `test/test_au_plugin_state.mm`           (parameter-event tests)
//
// Phase 3.5 unified what was briefly a second `pulp_audio_unit_fwd.h`
// header into this single canonical declaration. Don't reintroduce a
// parallel forward decl — duplicate `@interface PulpAudioUnit : AUAudioUnit`
// declarations in the same TU break compilation.

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

// Raw pointer to the host-owned Processor + StateStore. Used by the
// macOS / iOS AU v3 view controllers to construct a ViewBridge against
// the same Processor that runs the audio callback (avoids the
// dual-Processor parameter-drift bug the AU v2 path used to hit).
- (pulp::format::Processor *)pulpProcessor;
- (pulp::state::StateStore *)pulpStore;

// Parameter-event introspection. Used by `test_au_plugin_state.mm` to
// assert ramp-event payload survives the AUParameterTree → ParameterEventQueue
// translation in `au_adapter.mm`.
- (NSUInteger)pulpLastParameterEventCount;
- (uint32_t)pulpLastParameterEventParamIDAtIndex:(NSUInteger)index;
- (int32_t)pulpLastParameterEventSampleOffsetAtIndex:(NSUInteger)index;
- (int32_t)pulpLastParameterEventRampDurationAtIndex:(NSUInteger)index;
- (float)pulpLastParameterEventValueAtIndex:(NSUInteger)index;

@end
