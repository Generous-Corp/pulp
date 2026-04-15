#pragma once

// C++ adapter over ios_audio_session.h (workstream 05 slice 5.2).
//
// Wraps the C callback plumbing in a std::function-based subscription so
// application code can subscribe without juggling raw function pointers
// or user_data void*s. A single subscriber is supported for now —
// matches how AVAudioSession delivers notifications (one
// NotificationCenter observer per process that cares).

#include <pulp/format/ios_audio_session.h>

#include <functional>
#include <string_view>

namespace pulp::format {

/// Typed callback — C++ side of the AVAudioSession bridge.
using IosAudioSessionListener =
    std::function<void(const PulpIosAudioSessionEvent&)>;

/// Subscribe to session events. Replaces any existing subscription.
/// Pass an empty std::function to detach.
void set_ios_audio_session_listener(IosAudioSessionListener listener);

/// Emit a session event (usually from Swift). Header-only so test harnesses
/// can exercise the subscriber path without linking the real Swift target.
/// Returns true if a subscriber was called.
bool emit_ios_audio_session_event(const PulpIosAudioSessionEvent& event);

/// Human-readable label for an event type (for logs and tests).
std::string_view to_string(PulpIosAudioEvent event);

}  // namespace pulp::format
