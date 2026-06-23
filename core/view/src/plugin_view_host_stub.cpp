// Non-Apple PluginViewHost fallback and registration path. Apple platforms have
// native NSView/UIView-backed impls in platform/mac / platform/ios. Windows and
// Linux Skia builds can auto-register built-in HWND/X11 hosts through
// register_platform_plugin_view_host(); Android, custom targets, and builds
// without a platform host require the app to register a factory. Without either
// path, create() returns nullptr explicitly. Factory-registration API is
// compiled on every platform.

#include <pulp/view/plugin_view_host.hpp>

#include <mutex>

namespace pulp::view {

namespace {
    std::mutex                g_factory_mu;
    PluginViewHost::Factory   g_factory;
    bool                      g_factory_installed = false;
}

void PluginViewHost::set_factory(Factory factory) {
    std::lock_guard lock(g_factory_mu);
    g_factory = std::move(factory);
    g_factory_installed = true;
}

void PluginViewHost::clear_factory() {
    std::lock_guard lock(g_factory_mu);
    g_factory = {};
    g_factory_installed = false;
}

bool PluginViewHost::has_factory() {
    std::lock_guard lock(g_factory_mu);
    return g_factory_installed;
}

#if !defined(PULP_HAS_PLATFORM_PLUGIN_VIEW_HOST)
// Default no-op registration. A build with a native platform host
// (plugin_view_host_win.cpp / plugin_view_host_linux.cpp) defines
// PULP_HAS_PLATFORM_PLUGIN_VIEW_HOST and supplies the real implementation, so
// this stub is compiled only when there is no platform host (Apple — which
// has built-in hosts and needs no factory — Android, or a Skia/X11-less Linux
// build). See register_platform_plugin_view_host() docs in the header.
void register_platform_plugin_view_host() {}
#endif

#if !defined(__APPLE__)

std::unique_ptr<PluginViewHost> PluginViewHost::create(View& root, Size size) {
    Options opts;
    opts.size = size;
    return create(root, opts);
}

std::unique_ptr<PluginViewHost> PluginViewHost::create(View& root,
                                                       const Options& options) {
    // Install the built-in platform factory (HWND on Windows, X11 on
    // Linux) on first use, so the embed / VST3 / CLAP adapters get a working
    // host without the caller registering one. Idempotent and a no-op if a host
    // already called set_factory() (it stays installed) or on a build with no
    // platform host. A custom factory set AFTER this call still wins.
    register_platform_plugin_view_host();

    // Copy the factory out, release the lock, then invoke. Instantiating a
    // plugin view involves attaching to a DAW's editor window — can block;
    // definitely shouldn't hold a
    // registration mutex while it does.
    PluginViewHost::Factory local;
    {
        std::lock_guard lock(g_factory_mu);
        if (!g_factory_installed || !g_factory) return nullptr;
        local = g_factory;
    }
    auto host = local(root, options);
    if (host) {
        root.set_plugin_view_host(host.get());
    }
    return host;
}

#endif // !defined(__APPLE__)

} // namespace pulp::view
