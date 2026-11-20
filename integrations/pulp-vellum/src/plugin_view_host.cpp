// Definitions of the PluginViewHost factory functions for a plug-in editor
// built on an installed Vellum SDK.
//
// Vellum's plugin_view_host.hpp declares create(), the factory registry and
// register_platform_plugin_view_host(), and leaves their definitions to the
// embedding layer: attaching an editor to a DAW's native parent is plug-in
// embedding glue, which Pulp keeps. No Vellum library defines them, so this
// target does, once per plug-in image.
//
// create() returns the registered factory's host when one is installed, and
// otherwise the platform host this target builds (macOS only). Elsewhere it
// returns nullptr, which the editor treats as "no native parent available".

#include <pulp/vellum/plugin_view_host.hpp>

#include <vellum/view/plugin_view_host.hpp>

#include <mutex>
#include <utility>

namespace vellum::view {

namespace {
std::mutex& factory_mutex() {
    static std::mutex mutex;
    return mutex;
}

PluginViewHost::Factory& factory_slot() {
    static PluginViewHost::Factory factory;
    return factory;
}
} // namespace

void PluginViewHost::set_factory(Factory factory) {
    std::lock_guard lock(factory_mutex());
    factory_slot() = std::move(factory);
}

void PluginViewHost::clear_factory() {
    std::lock_guard lock(factory_mutex());
    factory_slot() = {};
}

bool PluginViewHost::has_factory() {
    std::lock_guard lock(factory_mutex());
    return static_cast<bool>(factory_slot());
}

// The macOS host is built in; nothing needs registering. The function exists
// because the header promises it to callers that force early registration.
void register_platform_plugin_view_host() {}

std::unique_ptr<PluginViewHost> PluginViewHost::create(View& root, Size size) {
    Options options;
    options.size = size;
    return create(root, options);
}

std::unique_ptr<PluginViewHost> PluginViewHost::create(View& root, const Options& options) {
    // Copy the factory out and call it without the lock: building a host
    // attaches native views and may re-enter the registry.
    Factory factory;
    {
        std::lock_guard lock(factory_mutex());
        factory = factory_slot();
    }
    std::unique_ptr<PluginViewHost> host =
        factory ? factory(root, options)
                : ::pulp::vellum::detail::make_platform_plugin_view_host(root, options);
    if (host != nullptr)
        root.set_plugin_view_host(host.get());
    return host;
}

} // namespace vellum::view

#if !defined(PULP_VELLUM_HAS_MAC_HOST)
namespace pulp::vellum::detail {
std::unique_ptr<::vellum::view::PluginViewHost>
make_platform_plugin_view_host(::vellum::view::View&,
                               const ::vellum::view::PluginViewHost::Options&) {
    return nullptr;
}
bool platform_plugin_view_host_available() noexcept {
    return false;
}
} // namespace pulp::vellum::detail
#endif
