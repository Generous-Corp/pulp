// PulpGain CLAP entry point
// Exports the clap_entry symbol that CLAP hosts scan for.
// Built from the CLAP specification: https://github.com/free-audio/clap

#include "pulp_gain.hpp"
#include "../../core/format/src/clap_adapter.hpp"
#include <pulp/format/registry.hpp>
#include <pulp/runtime/log.hpp>
#include <cstring>

// ── Plugin descriptor ──────────────────────────────────────────────────────

static const char* pulpgain_features[] = {
    CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
    CLAP_PLUGIN_FEATURE_UTILITY,
    nullptr
};

static const clap_plugin_descriptor_t pulpgain_desc = {
    .clap_version = CLAP_VERSION,
    .id = "com.pulp.gain",
    .name = "PulpGain",
    .vendor = "Pulp",
    .url = "https://github.com/danielraffel/pulp",
    .manual_url = "",
    .support_url = "",
    .version = "1.0.0",
    .description = "Simple stereo gain effect",
    .features = pulpgain_features,
};

// ── Audio ports extension ──────────────────────────────────────────────────

static uint32_t audio_ports_count(const clap_plugin_t*, bool is_input) {
    return 1; // One stereo bus for both input and output
}

static bool audio_ports_get(const clap_plugin_t*, uint32_t index, bool is_input,
                            clap_audio_port_info_t* info) {
    if (index != 0) return false;
    info->id = is_input ? 0 : 1;
    strncpy(info->name, is_input ? "Audio In" : "Audio Out", CLAP_NAME_SIZE);
    info->channel_count = 2;
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = is_input ? 1 : 0; // Paired for in-place
    return true;
}

static const clap_plugin_audio_ports_t audio_ports_ext = {
    .count = audio_ports_count,
    .get = audio_ports_get,
};

// ── State extension ────────────────────────────────────────────────────────

static bool state_save(const clap_plugin_t* plugin, const clap_ostream_t* stream) {
    auto* self = static_cast<pulp::format::clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    auto data = self->store.serialize();
    auto written = stream->write(stream, data.data(), data.size());
    return written == static_cast<int64_t>(data.size());
}

static bool state_load(const clap_plugin_t* plugin, const clap_istream_t* stream) {
    auto* self = static_cast<pulp::format::clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    std::vector<uint8_t> data;
    uint8_t buf[4096];
    while (true) {
        auto read = stream->read(stream, buf, sizeof(buf));
        if (read <= 0) break;
        data.insert(data.end(), buf, buf + read);
    }
    return self->store.deserialize(data);
}

static const clap_plugin_state_t state_ext = {
    .save = state_save,
    .load = state_load,
};

// ── Params extension ───────────────────────────────────────────────────────

static uint32_t params_count(const clap_plugin_t* plugin) {
    auto* self = static_cast<pulp::format::clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    return static_cast<uint32_t>(self->store.param_count());
}

static bool params_get_info(const clap_plugin_t* plugin, uint32_t index,
                            clap_param_info_t* info) {
    auto* self = static_cast<pulp::format::clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    auto params = self->store.all_params();
    if (index >= params.size()) return false;

    auto& p = params[index];
    memset(info, 0, sizeof(*info));
    info->id = p.id;
    strncpy(info->name, p.name.c_str(), CLAP_NAME_SIZE - 1);
    strncpy(info->module, "", CLAP_PATH_SIZE - 1);

    info->min_value = p.range.min;
    info->max_value = p.range.max;
    info->default_value = p.range.default_value;

    info->flags = CLAP_PARAM_IS_AUTOMATABLE;

    if (p.range.step >= 1.0f && (p.range.max - p.range.min) < 10.0f) {
        info->flags |= CLAP_PARAM_IS_STEPPED;
    }

    return true;
}

static bool params_get_value(const clap_plugin_t* plugin, clap_id param_id,
                             double* value) {
    auto* self = static_cast<pulp::format::clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    *value = self->store.get_value(static_cast<pulp::state::ParamID>(param_id));
    return true;
}

static bool params_value_to_text(const clap_plugin_t* plugin, clap_id param_id,
                                 double value, char* display, uint32_t size) {
    auto* self = static_cast<pulp::format::clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    auto* info = self->store.info(static_cast<pulp::state::ParamID>(param_id));
    if (!info) return false;

    if (!info->unit.empty()) {
        snprintf(display, size, "%.2f %s", value, info->unit.c_str());
    } else {
        snprintf(display, size, "%.2f", value);
    }
    return true;
}

static bool params_text_to_value(const clap_plugin_t*, clap_id,
                                 const char* text, double* value) {
    char* end;
    *value = strtod(text, &end);
    return end != text;
}

static void params_flush(const clap_plugin_t* plugin,
                         const clap_input_events_t* in,
                         const clap_output_events_t*) {
    auto* self = static_cast<pulp::format::clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    if (!in) return;
    uint32_t count = in->size(in);
    for (uint32_t i = 0; i < count; ++i) {
        auto* hdr = in->get(in, i);
        if (hdr->type == CLAP_EVENT_PARAM_VALUE) {
            auto* ev = reinterpret_cast<const clap_event_param_value_t*>(hdr);
            self->store.set_value(
                static_cast<pulp::state::ParamID>(ev->param_id),
                static_cast<float>(ev->value));
        }
    }
}

static const clap_plugin_params_t params_ext = {
    .count = params_count,
    .get_info = params_get_info,
    .get_value = params_get_value,
    .value_to_text = params_value_to_text,
    .text_to_value = params_text_to_value,
    .flush = params_flush,
};

// ── Extension dispatch ─────────────────────────────────────────────────────

static const void* get_extension(const clap_plugin_t*, const char* id) {
    if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &audio_ports_ext;
    if (strcmp(id, CLAP_EXT_PARAMS) == 0) return &params_ext;
    if (strcmp(id, CLAP_EXT_STATE) == 0) return &state_ext;
    return nullptr;
}

// ── Plugin factory ─────────────────────────────────────────────────────────

static const clap_plugin_t* create_plugin(const clap_plugin_factory_t* factory,
                                           const clap_host_t* host,
                                           const char* plugin_id) {
    if (strcmp(plugin_id, pulpgain_desc.id) != 0) return nullptr;

    auto* instance = new pulp::format::clap_adapter::PulpClapPlugin();
    instance->factory = pulp::examples::create_pulp_gain;

    instance->plugin = {
        .desc = &pulpgain_desc,
        .plugin_data = instance,
        .init = pulp::format::clap_adapter::clap_init,
        .destroy = pulp::format::clap_adapter::clap_destroy,
        .activate = pulp::format::clap_adapter::clap_activate,
        .deactivate = pulp::format::clap_adapter::clap_deactivate,
        .start_processing = pulp::format::clap_adapter::clap_start_processing,
        .stop_processing = pulp::format::clap_adapter::clap_stop_processing,
        .reset = pulp::format::clap_adapter::clap_reset,
        .process = pulp::format::clap_adapter::clap_process,
        .get_extension = get_extension,
        .on_main_thread = pulp::format::clap_adapter::clap_on_main_thread,
    };

    return &instance->plugin;
}

static uint32_t get_plugin_count(const clap_plugin_factory_t*) { return 1; }

static const clap_plugin_descriptor_t* get_plugin_descriptor(
    const clap_plugin_factory_t*, uint32_t index) {
    return index == 0 ? &pulpgain_desc : nullptr;
}

static const clap_plugin_factory_t plugin_factory = {
    .get_plugin_count = get_plugin_count,
    .get_plugin_descriptor = get_plugin_descriptor,
    .create_plugin = create_plugin,
};

// ── CLAP entry point ───────────────────────────────────────────────────────

static bool entry_init(const char* path) {
    pulp::runtime::log_info("CLAP: entry_init path={}", path);
    return true;
}

static void entry_deinit() {}

static const void* entry_get_factory(const char* factory_id) {
    if (strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) == 0)
        return &plugin_factory;
    return nullptr;
}

// THE exported symbol that CLAP hosts look for
extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    .clap_version = CLAP_VERSION,
    .init = entry_init,
    .deinit = entry_deinit,
    .get_factory = entry_get_factory,
};
