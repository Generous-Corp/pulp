// Non-Apple WindowHost fallback (#299). Apple platforms have the
// real NSWindow/UIWindow impls in platform/mac / platform/ios and
// supply their own WindowHost::create(). On Windows/Linux/Android
// the host app registers a factory via set_factory(); without a
// factory, create() returns nullptr explicitly so callers can
// probe has_factory() to report "platform unsupported" instead of
// silently getting a null handle.
//
// The factory-registration API is compiled on every platform.

#include <pulp/view/window_host.hpp>

#include <mutex>

namespace pulp::view {

namespace {
    std::mutex            g_factory_mu;
    WindowHost::Factory   g_factory;
    bool                  g_factory_installed = false;
}

void WindowHost::set_factory(Factory factory) {
    std::lock_guard lock(g_factory_mu);
    g_factory = std::move(factory);
    g_factory_installed = true;
}

void WindowHost::clear_factory() {
    std::lock_guard lock(g_factory_mu);
    g_factory = {};
    g_factory_installed = false;
}

bool WindowHost::has_factory() {
    std::lock_guard lock(g_factory_mu);
    return g_factory_installed;
}

#if !defined(__APPLE__)

std::unique_ptr<WindowHost> WindowHost::create(View& root,
                                                const WindowOptions& options) {
    std::lock_guard lock(g_factory_mu);
    if (!g_factory_installed || !g_factory) return nullptr;
    return g_factory(root, options);
}

#endif // !defined(__APPLE__)

} // namespace pulp::view
