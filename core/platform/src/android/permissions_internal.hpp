#pragma once

// Internal-only Android permissions surface. Shared between:
//   - permissions.cpp           (JNI callback sink, set_permission_callback)
//   - permissions_backend.cpp   (pulp::platform backend that forwards results)
//
// The Kotlin host still calls the legacy nativeOnPermissionResult JNI
// function; this header just gives the two C++ TUs one definition of the
// enum + callback signature to share.

namespace pulp::android {

enum class Permission : int {
    RecordAudio = 0,
    BluetoothMidi = 1,
    PostNotifications = 2,
};

using PermissionCallback = void(*)(Permission, bool granted);

void set_permission_callback(PermissionCallback cb);

}  // namespace pulp::android
