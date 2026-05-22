#pragma once

// Generic CLAP entry point generator
// Plugin developers include this and call PULP_CLAP_PLUGIN() with their factory function.
// All CLAP boilerplate (factory, extensions, entry point) is generated automatically.
//
// Usage (in one .cpp file per plugin):
//   #include "my_processor.hpp"
//   #include <pulp/format/clap_entry.hpp>
//   PULP_CLAP_PLUGIN(my_namespace::create_my_processor)

#include <pulp/format/processor.hpp>
#include <pulp/format/detail/editor_environment.hpp>
#include <pulp/format/plugin_state_io.hpp>
#include <pulp/format/registry.hpp>
#include <pulp/format/clap_adapter.hpp>
#ifdef PULP_CLAP_GUI
#include <pulp/format/editor_ui.hpp>
#include <pulp/format/gpu_host_select.hpp>
#endif
#include <pulp/runtime/log.hpp>
#include <pulp/runtime/system.hpp>
#include <clap/clap.h>
#include <cstring>
#include <cstdio>

// Internal implementation — do not call directly
namespace pulp::format::clap_generic {

// These are populated at static init by PULP_CLAP_PLUGIN
inline ProcessorFactory g_factory = nullptr;
inline PluginDescriptor g_desc{};
inline clap_plugin_descriptor_t g_clap_desc{};
inline const char* g_features[4] = {};

inline void init_descriptor() {
    if (!g_factory) return;
    auto proc = g_factory();
    if (!proc) return;
    g_desc = proc->descriptor();

    // Map category to CLAP features
    switch (g_desc.category) {
        case PluginCategory::Effect:
            g_features[0] = CLAP_PLUGIN_FEATURE_AUDIO_EFFECT;
            break;
        case PluginCategory::Instrument:
            g_features[0] = CLAP_PLUGIN_FEATURE_INSTRUMENT;
            break;
        case PluginCategory::MidiEffect:
            g_features[0] = CLAP_PLUGIN_FEATURE_NOTE_EFFECT;
            break;
    }
    g_features[1] = nullptr;

    g_clap_desc = {
        .clap_version = CLAP_VERSION,
        .id = g_desc.bundle_id.c_str(),
        .name = g_desc.name.c_str(),
        .vendor = g_desc.manufacturer.c_str(),
        .url = "",
        .manual_url = "",
        .support_url = "",
        .version = g_desc.version.c_str(),
        .description = "",
        .features = g_features,
    };
}

// ── Audio ports extension (multi-bus) ──────────────────────────────────
inline uint32_t audio_ports_count(const clap_plugin_t* plugin, bool is_input) {
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    auto desc = self->processor ? self->processor->descriptor() : g_desc;
    return static_cast<uint32_t>(is_input ? desc.input_buses.size() : desc.output_buses.size());
}

inline bool audio_ports_get(const clap_plugin_t* plugin, uint32_t index, bool is_input,
                            clap_audio_port_info_t* info) {
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    auto desc = self->processor ? self->processor->descriptor() : g_desc;
    auto& buses = is_input ? desc.input_buses : desc.output_buses;

    if (index >= buses.size()) return false;
    auto& bus = buses[index];

    info->id = static_cast<clap_id>((is_input ? 0 : 100) + index);
    runtime::copy_c_string(info->name, bus.name);
    info->channel_count = bus.default_channels;
    info->flags = (index == 0) ? CLAP_AUDIO_PORT_IS_MAIN : 0;
    info->port_type = bus.default_channels == 1 ? CLAP_PORT_MONO : CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

inline const clap_plugin_audio_ports_t audio_ports_ext = {
    .count = audio_ports_count, .get = audio_ports_get,
};

// ── State extension ────────────────────────────────────────────────────
inline bool state_save(const clap_plugin_t* plugin, const clap_ostream_t* stream) {
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    if (!self || !self->processor) return false;
    const auto data = plugin_state_io::serialize(self->store, *self->processor);
    // CLAP's stream->write() is spec'd to return a short-write count on
    // success — callers MUST loop. Hosts (and clap-validator's
    // `state-reproducibility-flush` with a 23-byte write cap) exercise
    // this path. A single write() returning less than data.size() is
    // NOT an error; only negative or zero returns are.
    std::size_t written = 0;
    while (written < data.size()) {
        const auto n = stream->write(stream,
                                     data.data() + written,
                                     data.size() - written);
        if (n <= 0) return false;
        written += static_cast<std::size_t>(n);
    }
    return true;
}

inline bool state_load(const clap_plugin_t* plugin, const clap_istream_t* stream) {
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    if (!self || !self->processor) return false;
    std::vector<uint8_t> data;
    uint8_t buf[4096];
    while (true) {
        auto read = stream->read(stream, buf, sizeof(buf));
        if (read <= 0) break;
        data.insert(data.end(), buf, buf + read);
    }
    return plugin_state_io::deserialize(data, self->store, *self->processor);
}

inline const clap_plugin_state_t state_ext = { .save = state_save, .load = state_load };

// ── Params extension ───────────────────────────────────────────────────
inline uint32_t params_count(const clap_plugin_t* plugin) {
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    return static_cast<uint32_t>(self->store.param_count());
}

inline bool params_get_info(const clap_plugin_t* plugin, uint32_t index, clap_param_info_t* info) {
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    auto params = self->store.all_params();
    if (index >= params.size()) return false;
    auto& p = params[index];
    memset(info, 0, sizeof(*info));
    info->id = p.id;
    runtime::copy_c_string(info->name, p.name);
    info->min_value = p.range.min;
    info->max_value = p.range.max;
    info->default_value = p.range.default_value;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    if (p.range.step >= 1.0f && (p.range.max - p.range.min) < 10.0f)
        info->flags |= CLAP_PARAM_IS_STEPPED;
    return true;
}

inline bool params_get_value(const clap_plugin_t* plugin, clap_id param_id, double* value) {
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    *value = self->store.get_value(static_cast<state::ParamID>(param_id));
    return true;
}

inline bool params_value_to_text(const clap_plugin_t* plugin, clap_id param_id,
                                  double value, char* display, uint32_t size) {
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    auto* info = self->store.info(static_cast<state::ParamID>(param_id));
    if (!info) return false;
    if (info->to_string) {
        auto str = info->to_string(static_cast<float>(value));
        snprintf(display, size, "%s", str.c_str());
    } else if (!info->unit.empty()) {
        snprintf(display, size, "%.2f %s", value, info->unit.c_str());
    } else {
        snprintf(display, size, "%.2f", value);
    }
    return true;
}

inline bool params_text_to_value(const clap_plugin_t*, clap_id, const char* text, double* value) {
    char* end;
    *value = strtod(text, &end);
    return end != text;
}

inline void params_flush(const clap_plugin_t* plugin, const clap_input_events_t* in,
                          const clap_output_events_t*) {
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    if (!in) return;
    uint32_t count = in->size(in);
    for (uint32_t i = 0; i < count; ++i) {
        auto* hdr = in->get(in, i);
        // CLAP event-space gate: skip third-party-extension namespaces
        // so their type IDs can't alias core PARAM_VALUE. Mirrors the
        // guard in clap_adapter.cpp's process() dispatch loops.
        if (hdr->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
        // memcpy into a stack local to avoid UBSan "misaligned address"
        // when hdr isn't aligned to the struct's alignof (e.g. 8 for
        // clap_event_param_value_t's `double value`). #688.
        if (hdr->type == CLAP_EVENT_PARAM_VALUE) {
            clap_event_param_value_t ev;
            std::memcpy(&ev, hdr, sizeof(ev));
            self->store.set_value(static_cast<state::ParamID>(ev.param_id),
                                  static_cast<float>(ev.value));
        } else if (hdr->type == CLAP_EVENT_PARAM_GESTURE_BEGIN) {
            clap_event_param_gesture_t ev;
            std::memcpy(&ev, hdr, sizeof(ev));
            self->store.begin_gesture(static_cast<state::ParamID>(ev.param_id));
        } else if (hdr->type == CLAP_EVENT_PARAM_GESTURE_END) {
            clap_event_param_gesture_t ev;
            std::memcpy(&ev, hdr, sizeof(ev));
            self->store.end_gesture(static_cast<state::ParamID>(ev.param_id));
        }
    }
}

inline const clap_plugin_params_t params_ext = {
    .count = params_count, .get_info = params_get_info,
    .get_value = params_get_value, .value_to_text = params_value_to_text,
    .text_to_value = params_text_to_value, .flush = params_flush,
};

// ── Note ports extension (for instruments) ─────────────────────────────
inline uint32_t note_ports_count(const clap_plugin_t*, bool is_input) {
    if (is_input && g_desc.accepts_midi) return 1;
    if (!is_input && g_desc.produces_midi) return 1;
    return 0;
}

inline bool note_ports_get(const clap_plugin_t*, uint32_t index, bool is_input,
                            clap_note_port_info_t* info) {
    if (index != 0) return false;
    if (is_input && !g_desc.accepts_midi) return false;
    if (!is_input && !g_desc.produces_midi) return false;

    info->id = is_input ? 0 : 1;
    runtime::copy_c_string(info->name, is_input ? "Note In" : "Note Out");
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    return true;
}

inline const clap_plugin_note_ports_t note_ports_ext = {
    .count = note_ports_count, .get = note_ports_get,
};

// ── Latency extension ───────────────────────────────────────────────────
inline uint32_t latency_get(const clap_plugin_t* plugin) {
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    if (!self->processor) return 0;
    return static_cast<uint32_t>(self->processor->latency_samples());
}

inline const clap_plugin_latency_t latency_ext = { .get = latency_get };

// ── Tail extension ──────────────────────────────────────────────────────
inline uint32_t tail_get(const clap_plugin_t* plugin) {
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    if (!self->processor) return 0;
    auto tail = self->processor->descriptor().tail_samples;
    if (tail < 0) return UINT32_MAX; // infinite tail
    return static_cast<uint32_t>(tail);
}

inline const clap_plugin_tail_t tail_ext = { .get = tail_get };

// ── GUI extension (only in plugin targets that define PULP_CLAP_GUI) ──

#ifdef PULP_CLAP_GUI

inline bool gui_is_api_supported(const clap_plugin_t*, const char* api, bool is_floating) {
    if (pulp::format::detail::editor_launch_blocked_by_environment()) return false;
    if (is_floating) return false;
#ifdef __APPLE__
    return strcmp(api, CLAP_WINDOW_API_COCOA) == 0;
#elif defined(_WIN32)
    return strcmp(api, CLAP_WINDOW_API_WIN32) == 0;
#elif defined(__linux__)
    return strcmp(api, CLAP_WINDOW_API_X11) == 0;
#else
    (void)api;
    return false;
#endif
}

inline bool gui_get_preferred_api(const clap_plugin_t*, const char** api, bool* is_floating) {
    if (pulp::format::detail::editor_launch_blocked_by_environment()) return false;
    *is_floating = false;
#ifdef __APPLE__
    *api = CLAP_WINDOW_API_COCOA;
#elif defined(_WIN32)
    *api = CLAP_WINDOW_API_WIN32;
#elif defined(__linux__)
    *api = CLAP_WINDOW_API_X11;
#else
    return false;
#endif
    return true;
}

inline bool gui_create(const clap_plugin_t* plugin, const char*, bool) {
    auto* p = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    if (pulp::format::detail::editor_launch_blocked_by_environment()) {
        runtime::log_info("CLAP editor: disabled in headless/CI/test environment");
        return false;
    }
    if (!p->processor || !p->processor->has_editor()) return false;

    std::string editor_error;
    p->bridge = std::make_unique<ViewBridge>(*p->processor, p->store);
    if (!p->bridge->open(&editor_error)) {
        runtime::log_error("CLAP editor: bridge->open failed ({})", editor_error);
        p->bridge.reset();
        return false;
    }

    const auto& hints = p->bridge->size_hints();
    const auto gpu = decide_gpu_host(*p->bridge);
    view::PluginViewHost::Options opts;
    opts.size = {hints.preferred_width, hints.preferred_height};
    opts.use_gpu = gpu.use_gpu;

    p->editor_host = view::PluginViewHost::create(*p->bridge->view(), opts);
    if (p->editor_host) {
        warn_if_unexpected_cpu_fallback(gpu, p->editor_host.get());
        // Pump the scripted UI session (async results, timers, rAF) per vsync.
        p->editor_host->set_idle_callback(make_scripted_idle_pump(*p->bridge));
        runtime::log_info("CLAP editor: created ({}x{}, mode={}, gpu={})",
                          hints.preferred_width, hints.preferred_height,
                          gpu.mode, p->editor_host->is_gpu_backed());
    } else {
        runtime::log_error("CLAP editor: failed to create host");
        p->bridge->close();
        p->bridge.reset();
    }
    return p->editor_host != nullptr;
}

inline void gui_destroy(const clap_plugin_t* plugin) {
    auto* p = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    p->editor_host.reset();
    if (p->bridge) {
        p->bridge->close();
        p->bridge.reset();
    }
    p->editor_visible = false;
}

inline bool gui_set_scale(const clap_plugin_t*, double) {
    return false;  // Cocoa uses logical pixels — no explicit scaling needed
}

inline bool gui_get_size(const clap_plugin_t* plugin, uint32_t* width, uint32_t* height) {
    auto* p = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    if (!p->processor) return false;
    if (p->bridge) {
        const auto& h = p->bridge->size_hints();
        *width = h.preferred_width;
        *height = h.preferred_height;
    } else {
        auto [w, ht] = p->processor->editor_size();
        *width = w;
        *height = ht;
    }
    return true;
}

inline bool gui_can_resize(const clap_plugin_t*) { return false; }

inline bool gui_get_resize_hints(const clap_plugin_t*, clap_gui_resize_hints_t*) {
    return false;
}

inline bool gui_adjust_size(const clap_plugin_t*, uint32_t*, uint32_t*) {
    return false;
}

inline bool gui_set_size(const clap_plugin_t* plugin, uint32_t width, uint32_t height) {
    auto* p = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    if (p->bridge) p->bridge->resize(width, height);
    if (p->editor_host) {
        p->editor_host->set_size(width, height);
        return true;
    }
    return false;
}

inline bool gui_set_parent(const clap_plugin_t* plugin, const clap_window_t* window) {
    auto* p = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    if (!p->editor_host) return false;

    bool attached = false;
#ifdef __APPLE__
    if (strcmp(window->api, CLAP_WINDOW_API_COCOA) == 0) {
        p->editor_host->attach_to_parent(window->cocoa);
        attached = true;
    }
#elif defined(_WIN32)
    if (strcmp(window->api, CLAP_WINDOW_API_WIN32) == 0) {
        p->editor_host->attach_to_parent(window->win32);
        attached = true;
    }
#elif defined(__linux__)
    if (strcmp(window->api, CLAP_WINDOW_API_X11) == 0) {
        p->editor_host->attach_to_parent(reinterpret_cast<void*>(window->x11));
        attached = true;
    }
#endif
    if (attached && p->bridge) {
        p->bridge->notify_attached();
    }
    return attached;
}

inline bool gui_set_transient(const clap_plugin_t*, const clap_window_t*) {
    return false;  // No floating window support
}

inline void gui_suggest_title(const clap_plugin_t*, const char*) {
    // No-op for embedded windows
}

inline bool gui_show(const clap_plugin_t* plugin) {
    auto* p = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    if (p->editor_host) {
        p->editor_visible = true;
        p->editor_host->repaint();
        return true;
    }
    return false;
}

inline bool gui_hide(const clap_plugin_t* plugin) {
    auto* p = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    p->editor_visible = false;
    return true;
}

inline const clap_plugin_gui_t gui_ext = {
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

#endif // PULP_CLAP_GUI

// ── Extension dispatch ─────────────────────────────────────────────────
inline const void* get_extension(const clap_plugin_t*, const char* id) {
#ifdef PULP_CLAP_GUI
    if (strcmp(id, CLAP_EXT_GUI) == 0) {
        if (pulp::format::detail::editor_launch_blocked_by_environment()) return nullptr;
        return &gui_ext;
    }
#endif
    if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &audio_ports_ext;
    if (strcmp(id, CLAP_EXT_NOTE_PORTS) == 0) return &note_ports_ext;
    if (strcmp(id, CLAP_EXT_PARAMS) == 0) return &params_ext;
    if (strcmp(id, CLAP_EXT_STATE) == 0) return &state_ext;
    if (strcmp(id, CLAP_EXT_LATENCY) == 0) return &latency_ext;
    if (strcmp(id, CLAP_EXT_TAIL) == 0) return &tail_ext;
    return nullptr;
}

// ── Plugin creation ────────────────────────────────────────────────────
inline const clap_plugin_t* create_plugin(const clap_plugin_factory_t*,
                                           const clap_host_t*,
                                           const char* plugin_id) {
    if (strcmp(plugin_id, g_clap_desc.id) != 0) return nullptr;

    auto* instance = new clap_adapter::PulpClapPlugin();
    instance->factory = g_factory;
    instance->plugin = {
        .desc = &g_clap_desc,
        .plugin_data = instance,
        .init = clap_adapter::clap_init,
        .destroy = clap_adapter::clap_destroy,
        .activate = clap_adapter::clap_activate,
        .deactivate = clap_adapter::clap_deactivate,
        .start_processing = clap_adapter::clap_start_processing,
        .stop_processing = clap_adapter::clap_stop_processing,
        .reset = clap_adapter::clap_reset,
        .process = clap_adapter::clap_process,
        .get_extension = get_extension,
        .on_main_thread = clap_adapter::clap_on_main_thread,
    };
    return &instance->plugin;
}

inline uint32_t get_plugin_count(const clap_plugin_factory_t*) { return 1; }
inline const clap_plugin_descriptor_t* get_plugin_descriptor(const clap_plugin_factory_t*, uint32_t i) {
    return i == 0 ? &g_clap_desc : nullptr;
}

inline const clap_plugin_factory_t plugin_factory = {
    .get_plugin_count = get_plugin_count,
    .get_plugin_descriptor = get_plugin_descriptor,
    .create_plugin = create_plugin,
};

inline bool entry_init(const char*) { return true; }
inline void entry_deinit() {}
inline const void* entry_get_factory(const char* factory_id) {
    if (strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) == 0) return &plugin_factory;
    return nullptr;
}

} // namespace pulp::format::clap_generic

// ── Public macro ───────────────────────────────────────────────────────
// Place in ONE .cpp file per CLAP plugin.
#define PULP_CLAP_PLUGIN(factory_fn) \
    namespace { \
        struct PulpClapInit { \
            PulpClapInit() { \
                pulp::format::clap_generic::g_factory = factory_fn; \
                pulp::format::register_plugin(factory_fn); \
                pulp::format::clap_generic::init_descriptor(); \
            } \
        } _pulp_clap_init; \
    } \
    extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = { \
        .clap_version = CLAP_VERSION, \
        .init = pulp::format::clap_generic::entry_init, \
        .deinit = pulp::format::clap_generic::entry_deinit, \
        .get_factory = pulp::format::clap_generic::entry_get_factory, \
    };
