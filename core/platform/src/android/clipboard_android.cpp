#if defined(__ANDROID__)

#include <pulp/platform/clipboard.hpp>
#include <optional>
#include <string>
#include <vector>

// Android clipboard access requires the Kotlin layer (ClipboardManager via Context).
// These stubs allow compilation; real implementation bridges via JNI.

namespace pulp::platform {

bool Clipboard::set_text(const std::string& /*text*/) {
    // TODO: Bridge to Kotlin ClipboardManager.setPrimaryClip()
    return false;
}

bool Clipboard::has_text() {
    // TODO: Bridge to Kotlin ClipboardManager.hasPrimaryClip()
    return false;
}

std::optional<std::string> Clipboard::get_text() {
    // TODO: Bridge to Kotlin ClipboardManager.getPrimaryClip()
    return std::nullopt;
}

bool Clipboard::set_data(const std::string& /*type*/, const std::vector<uint8_t>& /*data*/) {
    return false;
}

std::optional<std::vector<uint8_t>> Clipboard::get_data(const std::string& /*type*/) {
    return std::nullopt;
}

} // namespace pulp::platform

#endif // __ANDROID__
