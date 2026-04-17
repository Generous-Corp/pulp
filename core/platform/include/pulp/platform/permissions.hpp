#pragma once

// Cross-platform runtime permission queries and requests.
//
// Mobile platforms (iOS, Android) require explicit user consent for
// microphone, camera, Bluetooth, and notifications. Desktop platforms
// usually don't gate these through an app-level permission prompt, so
// their backends treat most classes as `Granted` and some as
// `Restricted` (e.g. Android's `ForegroundService`).
//
// The contract deliberately stays small:
//   query(p)    — synchronous, never blocks, never prompts.
//   request(p)  — async, always invokes the callback exactly once.
//
// Tests can redirect the backend with `PermissionsOverride` without
// touching the platform implementation.

#include <functional>

namespace pulp::platform {

enum class Permission {
    Microphone,         // iOS mic, Android RECORD_AUDIO
    Camera,             // iOS/Android camera
    BluetoothMidi,      // iOS NSBluetoothAlwaysUsageDescription, Android BLUETOOTH_*
    LocalNetwork,       // iOS NSLocalNetworkUsageDescription (RTP-MIDI, bonjour)
    Notifications,      // iOS UNUserNotificationCenter, Android POST_NOTIFICATIONS
    BackgroundAudio,    // iOS UIBackgroundModes=audio capability
    ForegroundService,  // Android FOREGROUND_SERVICE / FOREGROUND_SERVICE_MICROPHONE
};

enum class PermissionState {
    NotDetermined,  // Never asked. On platforms without a prompt, backends use Granted instead.
    Granted,        // User (or OS policy) granted it.
    Denied,         // User denied; may be re-askable depending on platform.
    Restricted,     // Policy / parental-control / unsupported-on-platform.
};

using RequestCallback = std::function<void(PermissionState)>;

/// Current state. Never blocks, never shows UI.
PermissionState query(Permission p);

/// Async request. `cb` is invoked exactly once with the resolved state.
/// On backends with no UI gate, the callback fires synchronously with
/// the same value `query()` would return.
void request(Permission p, RequestCallback cb);

/// True when a real platform backend is wired (iOS / Android). False
/// when the desktop / unsupported backend is active — useful for
/// diagnostics and feature gating.
bool has_platform_backend();

/// Test helper: scope-guarded override. While alive, `query()` and
/// `request()` return the mapped state for overridden permissions and
/// fall through to the real backend for the rest.
///
/// Not thread-safe on purpose — tests are single-threaded.
class PermissionsOverride {
public:
    PermissionsOverride();
    ~PermissionsOverride();

    PermissionsOverride(const PermissionsOverride&) = delete;
    PermissionsOverride& operator=(const PermissionsOverride&) = delete;

    /// Map this permission to `state` for the lifetime of the guard.
    void set(Permission p, PermissionState state);

    /// Remove any override for `p`. Subsequent queries fall through.
    void clear(Permission p);

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace pulp::platform
