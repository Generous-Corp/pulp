// Android backend for pulp::platform::permissions.
//
// Bridges the cross-platform API in core/platform/include/pulp/platform/permissions.hpp
// onto the existing Kotlin permission-request flow in core/platform/src/android/permissions.cpp
// (the `pulp::android::` JNI surface).
//
// The JNI callback `Java_com_pulp_PulpActivity_nativeOnPermissionResult` still
// lives in permissions.cpp; this TU subscribes to it via `set_permission_callback`
// and fans results out to pending pulp::platform callbacks.
//
// PULP_PERMISSIONS_HAS_BACKEND is set via the build system on Android so that
// core/platform/src/permissions.cpp's desktop fallback is suppressed and this
// TU owns query/request/has_platform_backend.

#if defined(__ANDROID__)

#include "permissions_internal.hpp"
#include "../permissions_detail.hpp"
#include <pulp/platform/permissions.hpp>

#include <mutex>
#include <optional>
#include <vector>

namespace pulp::platform {

namespace {

// Map the cross-platform `Permission` enum onto the subset exposed by the
// Kotlin flow. Returns std::nullopt for classes the Android backend doesn't
// gate (e.g. LocalNetwork — Android has no runtime prompt for mDNS).
std::optional<pulp::android::Permission> to_android(Permission p) {
    using A = pulp::android::Permission;
    switch (p) {
        case Permission::Microphone:        return A::RecordAudio;
        case Permission::BluetoothMidi:     return A::BluetoothMidi;
        case Permission::Notifications:     return A::PostNotifications;
        case Permission::Camera:
        case Permission::LocalNetwork:
        case Permission::BackgroundAudio:
        case Permission::ForegroundService:
            return std::nullopt;
    }
    return std::nullopt;
}

struct Pending {
    pulp::android::Permission android_perm;
    Permission perm;
    RequestCallback cb;
};

std::mutex& pending_mutex() {
    static std::mutex m;
    return m;
}

std::vector<Pending>& pending_queue() {
    static std::vector<Pending> q;
    return q;
}

void on_permission_result(pulp::android::Permission ap, bool granted) {
    RequestCallback cb;
    {
        std::lock_guard<std::mutex> lock(pending_mutex());
        auto& q = pending_queue();
        for (auto it = q.begin(); it != q.end(); ++it) {
            if (it->android_perm == ap) {
                cb = std::move(it->cb);
                q.erase(it);
                break;
            }
        }
    }
    if (cb) cb(granted ? PermissionState::Granted : PermissionState::Denied);
}

struct BridgeInstaller {
    BridgeInstaller() {
        pulp::android::set_permission_callback(&on_permission_result);
    }
};

void ensure_bridge_installed() {
    static BridgeInstaller once;
    (void)once;
}

// Without a synchronous Android API for "has the user granted this?",
// report NotDetermined for gated classes and let request() surface the
// prompt. LocalNetwork has no runtime gate on Android.
PermissionState android_default(Permission p) {
    switch (p) {
        case Permission::Microphone:
        case Permission::Camera:
        case Permission::BluetoothMidi:
        case Permission::Notifications:
        case Permission::BackgroundAudio:
        case Permission::ForegroundService:
            return PermissionState::NotDetermined;
        case Permission::LocalNetwork:
            return PermissionState::Granted;
    }
    return PermissionState::NotDetermined;
}

}  // namespace

PermissionState query(Permission p) {
    if (auto ov = detail::override_lookup(p)) return *ov;
    return android_default(p);
}

void request(Permission p, RequestCallback cb) {
    // Overridden? Deliver synchronously — tests don't want to round-trip
    // through a JNI callback that won't fire in headless builds.
    if (auto ov = detail::override_lookup(p)) {
        if (cb) cb(*ov);
        return;
    }

    auto android_p = to_android(p);
    if (!android_p) {
        // No runtime gate for this class on Android — answer with the
        // default and fire the callback once.
        if (cb) cb(android_default(p));
        return;
    }

    ensure_bridge_installed();
    {
        std::lock_guard<std::mutex> lock(pending_mutex());
        pending_queue().push_back({*android_p, p, std::move(cb)});
    }
    // NOTE: actually surfacing the prompt UI is Kotlin's responsibility
    // (ActivityResultContracts.RequestPermission). This TU only wires the
    // native side. The Kotlin host observes pending requests and calls
    // requestPermissions(...) itself; the result comes back through the
    // existing JNI callback and is dispatched to the pending entry above.
}

bool has_platform_backend() { return true; }

}  // namespace pulp::platform

#endif  // __ANDROID__
