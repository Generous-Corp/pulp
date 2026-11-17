#pragma once

// The native Audio/MIDI settings window of a standalone without a view layer
// (the contract-editor standalone).
//
// A small non-modal window over a StandaloneSettingsModel: output and input
// device, sample rate, buffer size, MIDI input and the output latency. Apply
// restarts audio through StandaloneApp::apply_config(), which also saves the
// selection for the next launch. The window asks the app for its audio and MIDI
// systems each time it refreshes, because a restart replaces them.

#include <functional>

namespace pulp::format {
class StandaloneApp;
}

namespace pulp::format::detail {

/// True when this platform has a native settings window.
bool standalone_settings_window_supported();

/// Show the window for `app`, bringing an open one to the front. False when the
/// platform has none. Main thread.
bool open_standalone_settings_window(StandaloneApp& app);

/// A window that shows settings for `owner` registers how to close itself, so
/// the app can close it on stop() and destruction. One closer per owner; a
/// second registration replaces the first.
void register_standalone_settings_window_closer(const StandaloneApp* owner,
                                                std::function<void()> close);

/// Run and forget the closers registered for `owner`, and no other app's.
/// Safe to call when there are none.
void close_standalone_settings_windows(const StandaloneApp* owner);

} // namespace pulp::format::detail
