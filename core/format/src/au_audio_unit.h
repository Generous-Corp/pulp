#pragma once

#import <AudioToolbox/AudioToolbox.h>

// Forward declaration of the Pulp AUAudioUnit subclass
// Defined in au_adapter.mm, used by au_entry.mm

@interface PulpAudioUnit : AUAudioUnit

- (NSUInteger)pulpLastParameterEventCount;
- (uint32_t)pulpLastParameterEventParamIDAtIndex:(NSUInteger)index;
- (int32_t)pulpLastParameterEventSampleOffsetAtIndex:(NSUInteger)index;
- (int32_t)pulpLastParameterEventRampDurationAtIndex:(NSUInteger)index;
- (float)pulpLastParameterEventValueAtIndex:(NSUInteger)index;

@end
