#pragma once

// The create_view() CLAP editor's per-instance state and GUI callbacks,
// defined at the end of clap_adapter.cpp under PULP_CLAP_GUI. Private and
// view-side: it names Pulp's view types, so only that code and the tests of
// the create_view() CLAP path include it. The plug-in entry reaches the
// extension through two declarations in clap_entry.hpp.

#include <pulp/format/clap_adapter.hpp>
#include <pulp/format/view_bridge.hpp>
#include <pulp/view/plugin_view_host.hpp>

#include <clap/clap.h>

#include <cstdint>
#include <memory>

namespace pulp::format::clap_adapter {

/// The create_view() editor of one plug-in instance. Members are destroyed in
/// reverse order: the GPU-surface subscription first, then the host, then the
/// bridge that owns the session the subscription writes into.
struct LegacyClapGui {
    // `bridge` owns the view tree and dispatches Processor lifecycle
    // callbacks (on_view_opened/closed/resized). editor_host is the
    // platform-native window surface that hosts bridge->view().
    std::unique_ptr<ViewBridge> bridge;
    std::unique_ptr<view::PluginViewHost> editor_host;
    // Forwards editor_host's GPU-surface transitions into bridge's scripted UI
    // session. Reset in gui_destroy() before either of them dies.
    view::PluginViewHost::GpuSurfaceSubscription gpu_surface_binding;
    // Native embedded CLAP editors use the host's timer-support extension as
    // their UI-thread frame source. The timer is registered only while the
    // editor is attached and visible; no worker or fabricated clock runs when
    // the host cannot provide this extension.
    const clap_host_timer_support_t* editor_host_timer = nullptr;
    clap_id editor_timer_id = CLAP_INVALID_ID;
    bool editor_parent_attached = false;
    bool editor_visible = false;
};

/// The instance's create_view() editor state, created on first use.
LegacyClapGui& legacy_gui(PulpClapPlugin& plugin);

} // namespace pulp::format::clap_adapter

namespace pulp::format::clap_generic {

bool gui_is_api_supported(const clap_plugin_t* plugin, const char* api, bool is_floating);
bool gui_get_preferred_api(const clap_plugin_t* plugin, const char** api, bool* is_floating);
bool gui_create(const clap_plugin_t* plugin, const char* api, bool is_floating);
void gui_destroy(const clap_plugin_t* plugin);
bool gui_set_scale(const clap_plugin_t* plugin, double scale);
bool gui_get_size(const clap_plugin_t* plugin, uint32_t* width, uint32_t* height);
bool gui_can_resize(const clap_plugin_t* plugin);
bool gui_get_resize_hints(const clap_plugin_t* plugin, clap_gui_resize_hints_t* hints);
bool gui_adjust_size(const clap_plugin_t* plugin, uint32_t* width, uint32_t* height);
bool gui_set_size(const clap_plugin_t* plugin, uint32_t width, uint32_t height);
bool gui_set_parent(const clap_plugin_t* plugin, const clap_window_t* window);
bool gui_set_transient(const clap_plugin_t* plugin, const clap_window_t* window);
void gui_suggest_title(const clap_plugin_t* plugin, const char* title);
bool gui_show(const clap_plugin_t* plugin);
bool gui_hide(const clap_plugin_t* plugin);
void native_gui_on_timer(const clap_plugin_t* plugin, clap_id timer_id);
/// Route the processor's editor-initiated resize requests to the host.
void install_editor_resize_handler(clap_adapter::PulpClapPlugin& plugin);

} // namespace pulp::format::clap_generic
