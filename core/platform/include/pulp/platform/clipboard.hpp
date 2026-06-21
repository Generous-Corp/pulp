#pragma once

#include <string>
#include <optional>
#include <vector>
#include <cstdint>
#include <functional>

namespace pulp::platform {

// Cross-platform clipboard access.
//
// Platform notes:
//   - macOS/iOS: real NSPasteboard / UIPasteboard integration.
//   - Windows: real OpenClipboard/SetClipboardData integration.
//   - Linux: shells out to wl-copy/wl-paste (Wayland) or xclip/xsel
//     (X11) — returns false / nullopt if none of those utilities
//     are on PATH. No in-process shadow storage, so callers can
//     reliably detect the unsupported case.
//   - Android: requires a host-registered bridge via
//     Clipboard::set_android_bridge(...). Without one, calls
//     return false / nullopt instead of fake-success. An Android
//     Pulp app installs the bridge at startup so ClipboardManager
//     calls reach a real android.content.Context.
class Clipboard {
public:
    // Text clipboard
    static bool set_text(const std::string& text);
    static std::optional<std::string> get_text();
    static bool has_text();

    // Binary data clipboard (custom pasteboard type)
    static bool set_data(const std::string& type, const std::vector<uint8_t>& data);
    static std::optional<std::vector<uint8_t>> get_data(const std::string& type);

    // Android JNI bridge. The Android app registers a bridge backed by
    // Kotlin ClipboardManager calls; without one, the Android impl fails
    // closed (returns false/nullopt) rather than fake-succeeding.
    struct AndroidBridge {
        std::function<bool(const std::string&)>       set_text;
        std::function<std::optional<std::string>()>   get_text;
        std::function<bool()>                         has_text;
    };
    static void set_android_bridge(AndroidBridge bridge);
    static void clear_android_bridge();
};

} // namespace pulp::platform
