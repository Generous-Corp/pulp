#pragma once

// System audio volume control — get/set the system output volume.
// Platform-specific: CoreAudio on macOS, IMMDeviceEnumerator on Windows, ALSA on Linux.

#include <optional>

namespace pulp::audio {

/// Get the current system output volume (0.0 to 1.0).
/// Returns nullopt if the platform doesn't support it.
std::optional<float> get_system_volume();

/// Set the system output volume (0.0 to 1.0).
/// Returns true on success.
bool set_system_volume(float volume);

/// Get the system mute state.
std::optional<bool> is_system_muted();

/// Set the system mute state.
bool set_system_muted(bool muted);

}  // namespace pulp::audio
