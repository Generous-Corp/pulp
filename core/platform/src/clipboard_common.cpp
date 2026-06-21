// Clipboard bridge no-op on non-Android platforms.
//
// The Android bridge API on Clipboard::set_android_bridge /
// clear_android_bridge is public so callers don't need to guard
// behind #ifdef __ANDROID__. On non-Android builds, registering a
// bridge is a silent no-op — the real platform clipboard is the
// OS's native pasteboard and doesn't need a bridge.

#if !defined(__ANDROID__)

#include <pulp/platform/clipboard.hpp>

namespace pulp::platform {

void Clipboard::set_android_bridge(AndroidBridge /*bridge*/) {
    // Intentional no-op on non-Android builds.
}

void Clipboard::clear_android_bridge() {
    // Intentional no-op on non-Android builds.
}

} // namespace pulp::platform

#endif // !defined(__ANDROID__)
