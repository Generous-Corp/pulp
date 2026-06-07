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

// Shared provider-invocation helper. Copies the registered provider out under
// the lock (#313 Codex P2: never call it while holding the mutex), releases,
// then invokes. `*had_provider` reports whether a provider was installed so
// callers can distinguish "no provider, fall back" from "provider returned
// empty (render failed)". Used by both the no-Skia fallback render path below
// AND the built-in Skia backend (screenshot_skia.cpp), so a host that installs
// a provider still wins on every non-Apple build.
std::vector<uint8_t> invoke_screenshot_provider(View& root, uint32_t width,
                                                uint32_t height, float scale,
                                                ScreenshotBackend backend,
                                                bool* had_provider) {
    ScreenshotProvider local;
    {
        std::lock_guard lock(g_provider_mu);
        if (!g_provider_installed || !g_provider) {
            if (had_provider) *had_provider = false;
            return {};
        }
        local = g_provider;
    }
    if (had_provider) *had_provider = true;
    return local(root, width, height, scale, backend);
}

// Compile the fallback render impls only when no built-in render backend takes
// precedence:
//   - macOS:  native screenshot_mac.mm provides render_*; not compiled here.
//   - iOS:    no iOS-native render impl yet, so the provider-backed fallback
//             below is what iOS uses (compiled even when Skia is present).
//   - Linux/Windows WITH Skia: screenshot_skia.cpp provides a built-in raster
//             backend; the fallback below is NOT compiled (would collide).
//   - Linux/Windows WITHOUT Skia / Android: provider-backed fallback below.
#if (!defined(__APPLE__) && !defined(PULP_HAS_SKIA)) || (defined(__APPLE__) && TARGET_OS_IOS)

std::vector<uint8_t> render_to_png(
    View& root, uint32_t width, uint32_t height, float scale,
    ScreenshotBackend backend)
{
    bool had = false;
    return invoke_screenshot_provider(root, width, height, scale, backend, &had);
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

// Raw-RGBA render is unsupported on platforms without a native raster path.
// The registered ScreenshotProvider is PNG-only (no raw-pixel contract), so
// there is no portable producer here yet — return empty explicitly.
std::vector<uint8_t> render_to_rgba(
    View& /*root*/, uint32_t /*width*/, uint32_t /*height*/, float /*scale*/,
    uint32_t* out_width, uint32_t* out_height)
{
    if (out_width) *out_width = 0;
    if (out_height) *out_height = 0;
    return {};
}

#endif // (!__APPLE__ && !PULP_HAS_SKIA) || (__APPLE__ && TARGET_OS_IOS)

} // namespace pulp::view
