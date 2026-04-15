#ifndef PULP_FORMAT_IOS_AUDIO_SESSION_H
#define PULP_FORMAT_IOS_AUDIO_SESSION_H

// C ABI bridge for iOS AVAudioSession ↔ Pulp C++ (workstream 05 slice 5.2).
//
// The Swift layer (apple/Sources/PulpSwift/PulpAudioSession.swift) observes
// AVAudioSession notifications and forwards them through this C function
// pointer table. C++ code (standalone app, plugin host) subscribes via
// pulp::format::IosAudioSessionBridge (see ios_audio_session.hpp) and
// receives typed callbacks without touching Swift or ObjC directly.
//
// Header is platform-agnostic (no iOS types) so it can be included by
// cross-platform code. The Swift side only compiles on iOS.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Event codes ────────────────────────────────────────────────────────

typedef enum PulpIosAudioEvent {
    PULP_IOS_AUDIO_EVENT_NONE                = 0,
    PULP_IOS_AUDIO_EVENT_INTERRUPTION_BEGAN  = 1,
    PULP_IOS_AUDIO_EVENT_INTERRUPTION_ENDED  = 2,
    PULP_IOS_AUDIO_EVENT_ROUTE_CHANGED       = 3,
    PULP_IOS_AUDIO_EVENT_MEDIA_SERVICES_RESET = 4,
    PULP_IOS_AUDIO_EVENT_SILENCE_SECONDARY_AUDIO_BEGAN = 5,
    PULP_IOS_AUDIO_EVENT_SILENCE_SECONDARY_AUDIO_ENDED = 6,
} PulpIosAudioEvent;

// Reasons supplied alongside ROUTE_CHANGED. Mirrors
// AVAudioSessionRouteChangeReason (UInt integer values).
typedef enum PulpIosRouteChangeReason {
    PULP_IOS_ROUTE_CHANGE_UNKNOWN                      = 0,
    PULP_IOS_ROUTE_CHANGE_NEW_DEVICE_AVAILABLE         = 1,
    PULP_IOS_ROUTE_CHANGE_OLD_DEVICE_UNAVAILABLE       = 2,
    PULP_IOS_ROUTE_CHANGE_CATEGORY_CHANGE              = 3,
    PULP_IOS_ROUTE_CHANGE_OVERRIDE                     = 4,
    PULP_IOS_ROUTE_CHANGE_WAKE_FROM_SLEEP              = 6,
    PULP_IOS_ROUTE_CHANGE_NO_SUITABLE_ROUTE            = 7,
    PULP_IOS_ROUTE_CHANGE_CONFIGURATION_CHANGE         = 8,
} PulpIosRouteChangeReason;

// Extra payload for interruption-ended events. Mirrors
// AVAudioSessionInterruptionOptions (bitfield).
typedef enum PulpIosInterruptionOption {
    PULP_IOS_INTERRUPTION_OPTION_NONE          = 0,
    PULP_IOS_INTERRUPTION_OPTION_SHOULD_RESUME = 1,
} PulpIosInterruptionOption;

// ── Event payload ──────────────────────────────────────────────────────

typedef struct PulpIosAudioSessionEvent {
    PulpIosAudioEvent event;
    int32_t reason;        // PulpIosRouteChangeReason when event == ROUTE_CHANGED
    int32_t options;       // PulpIosInterruptionOption bitfield
    double  sample_rate;   // current AVAudioSession.sharedInstance.sampleRate
    double  io_buffer_duration_seconds;
    int32_t output_channels;
    int32_t input_channels;
} PulpIosAudioSessionEvent;

// ── Callback signature ─────────────────────────────────────────────────

typedef void (*PulpIosAudioSessionCallback)(
    const PulpIosAudioSessionEvent* event, void* user_data);

// Global sink registration. Thread-safe only in the sense that Swift is
// expected to call the registered callback on the main thread — callers
// should dispatch onto their own audio thread if they need rt-safety.
// Passing NULL detaches the current sink.
void pulp_ios_audio_session_set_callback(PulpIosAudioSessionCallback cb,
                                         void* user_data);

// Swift / ObjC entry: emit a session event to the registered callback.
// Safe to call from any thread; returns immediately if no sink is set.
void pulp_ios_audio_session_emit(const PulpIosAudioSessionEvent* event);

#ifdef __cplusplus
}
#endif

#endif  // PULP_FORMAT_IOS_AUDIO_SESSION_H
