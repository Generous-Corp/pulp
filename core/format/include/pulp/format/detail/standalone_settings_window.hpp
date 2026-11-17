#pragma once

// The native Audio/MIDI settings window of a standalone without a view layer
// (the contract-editor standalone).
//
// A small non-modal window over a StandaloneSettingsModel: output and input
// device, sample rate, buffer size, MIDI input and the output latency. Apply
// restarts audio through StandaloneApp::apply_config(), which also saves the
// selection for the next launch. The window asks the app for its audio and MIDI
// systems each time it refreshes, because a restart replaces them.

namespace pulp::format {
class StandaloneApp;
}

namespace pulp::format::detail {

/// True when this platform has a native settings window.
bool standalone_settings_window_supported();

/// Show the window for `app`, bringing an open one to the front. False when the
/// platform has none. Main thread.
bool open_standalone_settings_window(StandaloneApp& app);

/// Close the window if it is open. Safe to call when it is not.
void close_standalone_settings_window();

} // namespace pulp::format::detail
