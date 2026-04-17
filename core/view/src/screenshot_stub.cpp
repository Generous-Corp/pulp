// Non-native-platform screenshot fallback (#299 + #313).
//
// The provider-registration API (set/clear/has_screenshot_provider)
// is always compiled. The render_to_png / render_to_file
// implementations are compiled when no platform-native impl exists
// for this build:
//   - macOS:  screenshot_mac.mm provides native, stubs not compiled here.
//   - iOS:    no iOS-native screenshot impl yet — stub compiled (#19 P1).
//   - Other:  stub compiled.
//
// When the stub path is active, render_* delegates to the registered
// provider; without one, it returns empty / false explicitly so
// callers can distinguish "unsupported" from "render failed".
//
// Codex P2 on #313 follow-up: the provider callback is NOT invoked
// under the registration mutex. We copy the callback to a local
// under lock, release, then call. If the callback takes long
// (real screenshot capture) or re-enters set/clear/has, the mutex
// is free.

#include <pulp/view/screenshot.hpp>

#include <fstream>
#include <mutex>
#include <utility>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace pulp::view {

namespace {
    std::mutex            g_provider_mu;
    ScreenshotProvider    g_provider;
    bool                  g_provider_installed = false;
}

void set_screenshot_provider(ScreenshotProvider provider) {
    std::lock_guard lock(g_provider_mu);
    g_provider = std::move(provider);
    g_provider_installed = true;
}

void clear_screenshot_provider() {
    std::lock_guard lock(g_provider_mu);
    g_provider = {};
    g_provider_installed = false;
}

bool has_screenshot_provider() {
    std::lock_guard lock(g_provider_mu);
    return g_provider_installed;
}

// Compile render impls when no native impl takes precedence.
// macOS has native screenshot_mac.mm; iOS does NOT yet have native,
// so include the stub on iOS too.
#if !defined(__APPLE__) || TARGET_OS_IOS

std::vector<uint8_t> render_to_png(
    View& root, uint32_t width, uint32_t height, float scale,
    ScreenshotBackend backend)
{
    // #313 Codex P2: copy the provider out, release the lock, then
    // invoke. The provider can be long-running or re-enter the API.
    ScreenshotProvider local;
    {
        std::lock_guard lock(g_provider_mu);
        if (!g_provider_installed || !g_provider) return {};
        local = g_provider;
    }
    return local(root, width, height, scale, backend);
}

bool render_to_file(
    View& root, uint32_t width, uint32_t height,
    const std::string& output_path, float scale,
    ScreenshotBackend backend)
{
    auto bytes = render_to_png(root, width, height, scale, backend);
    if (bytes.empty()) return false;
    std::ofstream out(output_path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

#endif // !defined(__APPLE__) || TARGET_OS_IOS

} // namespace pulp::view
