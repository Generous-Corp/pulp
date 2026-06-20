#if defined(__ANDROID__)

#include <pulp/platform/clipboard.hpp>

#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Android clipboard access requires the Kotlin layer
// (android.content.ClipboardManager via Context). The C++ side can't
// instantiate a Context on its own, so the Android app installs a
// host-provided bridge at startup. When no bridge is installed,
// every call fails closed (returns false / nullopt). This is the
// intentional behavior so Android callers never mistake a missing
// Kotlin bridge for successful clipboard access.

namespace pulp::platform {

namespace {
    std::mutex               g_bridge_mu;
    Clipboard::AndroidBridge g_bridge;
    bool                     g_bridge_installed = false;
}

void Clipboard::set_android_bridge(AndroidBridge bridge) {
    std::lock_guard lock(g_bridge_mu);
    g_bridge = std::move(bridge);
    g_bridge_installed = true;
}

void Clipboard::clear_android_bridge() {
    std::lock_guard lock(g_bridge_mu);
    g_bridge = {};
    g_bridge_installed = false;
}

bool Clipboard::set_text(const std::string& text) {
    std::lock_guard lock(g_bridge_mu);
    if (!g_bridge_installed || !g_bridge.set_text) return false;
    return g_bridge.set_text(text);
}

bool Clipboard::has_text() {
    std::lock_guard lock(g_bridge_mu);
    if (!g_bridge_installed || !g_bridge.has_text) return false;
    return g_bridge.has_text();
}

std::optional<std::string> Clipboard::get_text() {
    std::lock_guard lock(g_bridge_mu);
    if (!g_bridge_installed || !g_bridge.get_text) return std::nullopt;
    return g_bridge.get_text();
}

bool Clipboard::set_data(const std::string& /*type*/,
                         const std::vector<uint8_t>& /*data*/) {
    // Custom pasteboard types aren't exposed through the minimal
    // ClipboardManager bridge. Explicit false so callers know the
    // binary-clipboard path is unsupported on Android today.
    return false;
}

std::optional<std::vector<uint8_t>> Clipboard::get_data(
    const std::string& /*type*/) {
    return std::nullopt;
}

} // namespace pulp::platform

#endif // __ANDROID__
