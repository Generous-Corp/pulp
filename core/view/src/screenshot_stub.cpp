// Non-Apple screenshot fallback (#299). No built-in backend in
// core/view — hosts install a provider via set_screenshot_provider()
// for real captures. Without a provider, render_to_png returns an
// empty buffer and render_to_file returns false explicitly — callers
// probe has_screenshot_provider() to tell "unsupported" apart from
// "render failed".
//
// The provider-registration API is always compiled; the render_*
// forwards live only on non-Apple (Apple platforms have a native
// impl in screenshot_mac.mm / screenshot_ios.mm).

#include <pulp/view/screenshot.hpp>

#include <fstream>
#include <mutex>

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

#if !defined(__APPLE__)

std::vector<uint8_t> render_to_png(
    View& root, uint32_t width, uint32_t height, float scale,
    ScreenshotBackend backend)
{
    std::lock_guard lock(g_provider_mu);
    if (!g_provider_installed || !g_provider) return {};
    return g_provider(root, width, height, scale, backend);
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

#endif // !defined(__APPLE__)

} // namespace pulp::view
