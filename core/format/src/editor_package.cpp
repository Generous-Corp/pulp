#include <pulp/format/editor_package.hpp>

#include <atomic>

namespace pulp::format {

namespace {

// Constant-initialized, so a hook installed by a static initializer in any
// other translation unit is never overwritten by this one's initialization.
std::atomic<EditorFactoryHook> g_hook{nullptr};

} // namespace

void install_plugin_editor_factory_hook(EditorFactoryHook hook) noexcept {
    g_hook.store(hook, std::memory_order_release);
}

EditorFactory* plugin_editor_factory() {
    const EditorFactoryHook hook = g_hook.load(std::memory_order_acquire);
    return hook != nullptr ? hook() : nullptr;
}

} // namespace pulp::format
