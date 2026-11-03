#include <pulp/format/clap_contract_gui.hpp>

#include <pulp/format/clap_adapter.hpp>
#include <pulp/format/contract_editor_session.hpp>
#include <pulp/format/detail/editor_environment.hpp>
#include <pulp/runtime/log.hpp>

#include <clap/ext/state.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>

namespace pulp::format::clap_adapter {

namespace {

// Per-instance state, kept in PulpClapPlugin::contract_gui.
// Destroyed by gui destroy() or, if the host never called it, with the
// plug-in, before the processor and store (PulpClapPlugin member order).
struct ContractGui {
    const clap_host_t* host = nullptr;
    const clap_host_timer_support_t* host_timer = nullptr;
    clap_id timer_id = CLAP_INVALID_ID;
    std::unique_ptr<ContractEditorSession> session;

    ~ContractGui() {
        stop_timer();
        // Destroying an attached session invalidates its editor.
        session.reset();
    }
    void stop_timer() {
        if (timer_id != CLAP_INVALID_ID && host_timer != nullptr)
            host_timer->unregister_timer(host, timer_id);
        timer_id = CLAP_INVALID_ID;
    }
};

PulpClapPlugin* self_of(const clap_plugin_t* plugin) {
    return plugin ? static_cast<PulpClapPlugin*>(plugin->plugin_data) : nullptr;
}

ContractGui* gui_of(PulpClapPlugin& self) {
    return static_cast<ContractGui*>(self.contract_gui.get());
}

const void* host_extension(const PulpClapPlugin& self, const char* id) {
    if (self.host == nullptr || self.host->get_extension == nullptr)
        return nullptr;
    return self.host->get_extension(self.host, id);
}

bool applies(const PulpClapPlugin* self) {
    return self != nullptr && self->processor != nullptr &&
           self->processor->editor_factory() != nullptr &&
           !detail::editor_launch_blocked_by_environment();
}

const char* platform_api() {
#if defined(__APPLE__)
    return CLAP_WINDOW_API_COCOA;
#elif defined(_WIN32)
    return CLAP_WINDOW_API_WIN32;
#elif defined(__linux__)
    return CLAP_WINDOW_API_X11;
#else
    return nullptr;
#endif
}

bool is_platform_api(const char* api) {
    const char* mine = platform_api();
    return api != nullptr && mine != nullptr && std::strcmp(api, mine) == 0;
}

bool gui_is_api_supported(const clap_plugin_t* plugin, const char* api, bool is_floating) {
    return applies(self_of(plugin)) && !is_floating && is_platform_api(api);
}

bool gui_get_preferred_api(const clap_plugin_t* plugin, const char** api, bool* is_floating) {
    if (!applies(self_of(plugin)) || platform_api() == nullptr || api == nullptr ||
        is_floating == nullptr)
        return false;
    *api = platform_api();
    *is_floating = false;
    return true;
}

bool gui_create(const clap_plugin_t* plugin, const char* api, bool is_floating) {
    auto* self = self_of(plugin);
    if (!applies(self) || is_floating || !is_platform_api(api) || self->contract_gui)
        return false;

    auto gui = std::make_shared<ContractGui>();
    gui->host = self->host;
    gui->host_timer = static_cast<const clap_host_timer_support_t*>(
        host_extension(*self, CLAP_EXT_TIMER_SUPPORT));
    if (gui->host_timer != nullptr && (gui->host_timer->register_timer == nullptr ||
                                       gui->host_timer->unregister_timer == nullptr))
        gui->host_timer = nullptr;

    ContractEditorSession::HostHooks hooks;
    hooks.request_resize = [self](const EditorSize& size) {
        const auto* host_gui =
            static_cast<const clap_host_gui_t*>(host_extension(*self, CLAP_EXT_GUI));
        return host_gui != nullptr && host_gui->request_resize != nullptr &&
               host_gui->request_resize(self->host, size.width, size.height);
    };
    hooks.host_action = [self](const HostActionRequest& request) {
        if (request.action != HostAction::mark_state_dirty)
            return false;
        const auto* host_state =
            static_cast<const clap_host_state_t*>(host_extension(*self, CLAP_EXT_STATE));
        if (host_state == nullptr || host_state->mark_dirty == nullptr)
            return false;
        host_state->mark_dirty(self->host);
        return true;
    };
    const auto idle = gui->host_timer != nullptr ? ContractEditorSession::IdleSource::host_driven
                                                 : ContractEditorSession::IdleSource::self_paced;
    gui->session = ContractEditorSession::create(
        *self->processor, self->store, *self->processor->editor_factory(), std::move(hooks), idle);
    if (!gui->session)
        return false;
    self->contract_gui = std::move(gui);
    return true;
}

void gui_destroy(const clap_plugin_t* plugin) {
    auto* self = self_of(plugin);
    if (self == nullptr || !self->contract_gui)
        return;
    auto& gui = *gui_of(*self);
    gui.stop_timer();
    // An orderly close: the host's window still exists.
    if (gui.session)
        gui.session->detach();
    self->contract_gui.reset();
}

ContractEditorSession* session_of(const clap_plugin_t* plugin) {
    auto* self = self_of(plugin);
    if (self == nullptr || !self->contract_gui)
        return nullptr;
    return gui_of(*self)->session.get();
}

bool gui_set_scale(const clap_plugin_t* plugin, double scale) {
    auto* session = session_of(plugin);
    return session != nullptr && session->set_scale(scale);
}

bool gui_get_size(const clap_plugin_t* plugin, uint32_t* width, uint32_t* height) {
    auto* session = session_of(plugin);
    if (session == nullptr || width == nullptr || height == nullptr)
        return false;
    const auto size = session->size();
    *width = size.width;
    *height = size.height;
    return size.width > 0 && size.height > 0;
}

bool gui_can_resize(const clap_plugin_t* plugin) {
    auto* session = session_of(plugin);
    return session != nullptr && session->resizable();
}

bool gui_get_resize_hints(const clap_plugin_t* plugin, clap_gui_resize_hints_t* hints) {
    auto* session = session_of(plugin);
    if (session == nullptr || hints == nullptr)
        return false;
    const bool resizable = session->resizable();
    const auto h = session->size_hints();
    hints->can_resize_horizontally = resizable;
    hints->can_resize_vertically = resizable;
    hints->preserve_aspect_ratio = resizable && h.aspect_ratio > 0.0;
    hints->aspect_ratio_width = 0;
    hints->aspect_ratio_height = 0;
    if (hints->preserve_aspect_ratio) {
        // The ratio as two integers: the preferred size when it holds the
        // ratio, otherwise the ratio scaled to a thousandth.
        const double preferred = h.preferred_height > 0
                                     ? static_cast<double>(h.preferred_width) / h.preferred_height
                                     : 0.0;
        if (preferred > 0.0 && std::abs(preferred - h.aspect_ratio) < 1e-6) {
            hints->aspect_ratio_width = h.preferred_width;
            hints->aspect_ratio_height = h.preferred_height;
        } else {
            hints->aspect_ratio_width = static_cast<uint32_t>(h.aspect_ratio * 1000.0 + 0.5);
            hints->aspect_ratio_height = 1000;
        }
    }
    return true;
}

bool gui_adjust_size(const clap_plugin_t* plugin, uint32_t* width, uint32_t* height) {
    auto* session = session_of(plugin);
    if (session == nullptr || width == nullptr || height == nullptr)
        return false;
    return session->constrain(*width, *height);
}

bool gui_set_size(const clap_plugin_t* plugin, uint32_t width, uint32_t height) {
    auto* session = session_of(plugin);
    return session != nullptr && session->set_size(width, height);
}

bool gui_set_parent(const clap_plugin_t* plugin, const clap_window_t* window) {
    auto* self = self_of(plugin);
    auto* session = session_of(plugin);
    if (session == nullptr || window == nullptr || !is_platform_api(window->api))
        return false;
    NativeParent parent;
#if defined(__APPLE__)
    parent.kind = NativeParentKind::cocoa_view;
    parent.handle = window->cocoa;
#elif defined(_WIN32)
    parent.kind = NativeParentKind::win32_window;
    parent.handle = window->win32;
#elif defined(__linux__)
    parent.kind = NativeParentKind::x11_window;
    parent.window_id = static_cast<std::uint64_t>(window->x11);
#endif
    if (!session->attach(parent))
        return false;
    auto& gui = *gui_of(*self);
    if (gui.host_timer != nullptr) {
        clap_id id = CLAP_INVALID_ID;
        if (gui.host_timer->register_timer(self->host, ContractEditorSession::kIdleIntervalMs, &id))
            gui.timer_id = id;
        else
            runtime::log_error("CLAP editor: the host refused an idle timer");
    }
    return true;
}

bool gui_set_transient(const clap_plugin_t*, const clap_window_t*) {
    return false; // embedded editors only
}

void gui_suggest_title(const clap_plugin_t*, const char*) {}

bool gui_show(const clap_plugin_t* plugin) {
    auto* session = session_of(plugin);
    if (session == nullptr)
        return false;
    session->visibility_changed(true);
    return true;
}

bool gui_hide(const clap_plugin_t* plugin) {
    auto* session = session_of(plugin);
    if (session == nullptr)
        return false;
    session->visibility_changed(false);
    return true;
}

void on_timer(const clap_plugin_t* plugin, clap_id timer_id) {
    auto* self = self_of(plugin);
    if (self == nullptr || !self->contract_gui)
        return;
    auto& gui = *gui_of(*self);
    if (timer_id == gui.timer_id && gui.session)
        gui.session->tick();
}

const clap_plugin_gui_t s_contract_gui = {
    .is_api_supported = gui_is_api_supported,
    .get_preferred_api = gui_get_preferred_api,
    .create = gui_create,
    .destroy = gui_destroy,
    .set_scale = gui_set_scale,
    .get_size = gui_get_size,
    .can_resize = gui_can_resize,
    .get_resize_hints = gui_get_resize_hints,
    .adjust_size = gui_adjust_size,
    .set_size = gui_set_size,
    .set_parent = gui_set_parent,
    .set_transient = gui_set_transient,
    .suggest_title = gui_suggest_title,
    .show = gui_show,
    .hide = gui_hide,
};

const clap_plugin_timer_support_t s_contract_timer = {
    .on_timer = on_timer,
};

} // namespace

const clap_plugin_gui_t* contract_gui_extension(const clap_plugin_t* plugin) {
    return applies(self_of(plugin)) ? &s_contract_gui : nullptr;
}

const clap_plugin_timer_support_t* contract_timer_extension(const clap_plugin_t* plugin) {
    return applies(self_of(plugin)) ? &s_contract_timer : nullptr;
}

} // namespace pulp::format::clap_adapter
