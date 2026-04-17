// Non-Apple PluginViewHost fallback (#299). Apple platforms have
// native NSView/UIView-backed impls in platform/mac / platform/ios.
// On Windows/Linux/Android the host app registers a factory; without
// one, create() returns nullptr explicitly. Factory-registration API
// is compiled on every platform.

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

#if !defined(__APPLE__)

std::unique_ptr<PluginViewHost> PluginViewHost::create(View& root, Size size) {
    Options opts;
    opts.size = size;
    return create(root, opts);
}

std::unique_ptr<PluginViewHost> PluginViewHost::create(View& root,
                                                       const Options& options) {
    // #313 Codex P2: copy the factory out, release the lock, then
    // invoke. Instantiating a plugin view involves attaching to a
    // DAW's editor window — can block; definitely shouldn't hold a
    // registration mutex while it does.
    PluginViewHost::Factory local;
    {
        std::lock_guard lock(g_factory_mu);
        if (!g_factory_installed || !g_factory) return nullptr;
        local = g_factory;
    }
    return local(root, options);
}

#endif // !defined(__APPLE__)

} // namespace pulp::view
