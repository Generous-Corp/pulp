// SPDX-License-Identifier: MIT
//
// A small hand-written CLAP effect used to prove the conformance checker. Built
// once per BREAK_* definition; with none defined it is a well-behaved plug-in.
//
//   BREAK_HEAVY_MEMORY  entry init commits and touches 128 MB
//   BREAK_HEAVY_CPU     entry init burns about a billion instructions
//   BREAK_ECHO          every host parameter event is sent back as an out-event
//                       with a gesture pair, and a flush is requested
//   BREAK_ECHO_MAIN     the host parameter event is posted to the main queue and
//                       written back as an out-event only once the host main loop
//                       has turned (macOS only; elsewhere it echoes nothing)
//   BREAK_CRASH_TEARDOWN  destroy() aborts after a long run (more than 300 blocks)
//   BREAK_NULL_INPUT    process reads the first input channel without checking
//                       that the host supplied one
//   BREAK_STATE         state.save embeds a per-save counter
//   BREAK_HANG_INIT     entry init never returns
//   BREAK_HANG_PROCESS  process never returns
//   BREAK_NULL_DESCRIPTOR the factory returns no descriptor for its plug-in
//   BREAK_CONTROL_CHARS the plug-in id contains a tab and a newline

#include <clap/clap.h>

#include <unistd.h>

#if defined(BREAK_ECHO_MAIN) && defined(__APPLE__)
#include <dispatch/dispatch.h>
#endif

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

struct Plug {
    clap_plugin_t plugin;
    const clap_host_t* host;
    double value[2] = {0.5, 0.5};
    std::atomic<int> echo_pending[2] = {0, 0};
    int blocks = 0;
};

const char* const kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr};
const clap_plugin_descriptor_t kDesc = {CLAP_VERSION_INIT,
#ifdef BREAK_CONTROL_CHARS
                                        "dev.pulp.test.conformance\tfixture\nctrl",
#else
                                        "dev.pulp.test.conformance-fixture",
#endif
                                        "Conformance Fixture",
                                        "Pulp",
                                        "",
                                        "",
                                        "",
                                        "1.0.0",
                                        "",
                                        kFeatures};

uint32_t params_count(const clap_plugin_t*) {
    return 2;
}
bool params_get_info(const clap_plugin_t*, uint32_t i, clap_param_info_t* info) {
    if (i > 1)
        return false;
    std::memset(info, 0, sizeof(*info));
    info->id = 100 + i;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    std::snprintf(info->name, sizeof(info->name), i == 0 ? "Gain" : "Mix");
    info->min_value = 0.0;
    info->max_value = 1.0;
    info->default_value = 0.5;
    return true;
}
bool params_get_value(const clap_plugin_t* p, clap_id id, double* v) {
    if (id < 100 || id > 101)
        return false;
    *v = static_cast<Plug*>(p->plugin_data)->value[id - 100];
    return true;
}
bool params_value_to_text(const clap_plugin_t*, clap_id, double v, char* out, uint32_t n) {
    std::snprintf(out, n, "%.3f", v);
    return true;
}
bool params_text_to_value(const clap_plugin_t*, clap_id, const char* t, double* v) {
    *v = std::atof(t);
    return true;
}

#if defined(BREAK_ECHO_MAIN) && defined(__APPLE__)
struct EchoPost {
    Plug* plug;
    uint32_t index;
};
void echo_post_cb(void* ctx) {
    auto* e = static_cast<EchoPost*>(ctx);
    e->plug->echo_pending[e->index].store(1);
    delete e;
}
#endif

void handle_in(Plug* s, const clap_input_events_t* in, const clap_output_events_t* out) {
#if defined(BREAK_ECHO_MAIN) && defined(__APPLE__)
    for (uint32_t k = 0; k < 2; ++k) {
        if (s->echo_pending[k].exchange(0)) {
            clap_event_param_value_t e{};
            e.header = {sizeof(e), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
            e.param_id = 100 + k;
            e.note_id = e.port_index = e.channel = e.key = -1;
            e.value = s->value[k];
            out->try_push(out, &e.header);
        }
    }
#endif
    for (uint32_t i = 0; i < in->size(in); ++i) {
        const clap_event_header_t* h = in->get(in, i);
        if (h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE)
            continue;
        auto* e = reinterpret_cast<const clap_event_param_value_t*>(h);
        if (e->param_id >= 100 && e->param_id <= 101)
            s->value[e->param_id - 100] = e->value;
#if defined(BREAK_ECHO_MAIN) && defined(__APPLE__)
        if (e->param_id >= 100 && e->param_id <= 101)
            dispatch_async_f(dispatch_get_main_queue(), new EchoPost{s, e->param_id - 100},
                             echo_post_cb);
#endif
#ifdef BREAK_ECHO
        clap_event_param_gesture_t g{};
        g.header = {sizeof(g), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_GESTURE_BEGIN, 0};
        g.param_id = e->param_id;
        out->try_push(out, &g.header);
        out->try_push(out, h);
        g.header.type = CLAP_EVENT_PARAM_GESTURE_END;
        out->try_push(out, &g.header);
        if (const auto* hp = static_cast<const clap_host_params_t*>(
                s->host->get_extension(s->host, CLAP_EXT_PARAMS)))
            hp->request_flush(s->host);
#else
        (void)out;
#endif
    }
}

void params_flush(const clap_plugin_t* p, const clap_input_events_t* in,
                  const clap_output_events_t* out) {
    handle_in(static_cast<Plug*>(p->plugin_data), in, out);
}
const clap_plugin_params_t kParams = {params_count,         params_get_info,      params_get_value,
                                      params_value_to_text, params_text_to_value, params_flush};

uint32_t ports_count(const clap_plugin_t*, bool) {
    return 1;
}
bool ports_get(const clap_plugin_t*, uint32_t i, bool is_input, clap_audio_port_info_t* info) {
    if (i != 0)
        return false;
    std::memset(info, 0, sizeof(*info));
    info->id = is_input ? 0 : 1;
    std::snprintf(info->name, sizeof(info->name), is_input ? "In" : "Out");
    info->channel_count = 2;
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
const clap_plugin_audio_ports_t kPorts = {ports_count, ports_get};

#ifdef BREAK_STATE
int g_saves = 0;
#endif
bool state_save(const clap_plugin_t* p, const clap_ostream_t* s) {
    auto* self = static_cast<Plug*>(p->plugin_data);
    double buf[3] = {self->value[0], self->value[1], 0.0};
#ifdef BREAK_STATE
    buf[2] = ++g_saves;
#endif
    return s->write(s, buf, sizeof(buf)) == static_cast<int64_t>(sizeof(buf));
}
bool state_load(const clap_plugin_t* p, const clap_istream_t* s) {
    auto* self = static_cast<Plug*>(p->plugin_data);
    double buf[3] = {};
    if (s->read(s, buf, sizeof(buf)) != static_cast<int64_t>(sizeof(buf)))
        return false;
    self->value[0] = buf[0];
    self->value[1] = buf[1];
    return true;
}
const clap_plugin_state_t kState = {state_save, state_load};

bool p_init(const clap_plugin_t*) {
    return true;
}
void p_destroy(const clap_plugin_t* p) {
#ifdef BREAK_CRASH_TEARDOWN
    if (static_cast<Plug*>(p->plugin_data)->blocks > 300)
        std::abort();
#endif
    delete static_cast<Plug*>(p->plugin_data);
}
bool p_activate(const clap_plugin_t*, double, uint32_t, uint32_t) {
    return true;
}
void p_deactivate(const clap_plugin_t*) {}
bool p_start(const clap_plugin_t*) {
    return true;
}
void p_stop(const clap_plugin_t*) {}
void p_reset(const clap_plugin_t*) {}
clap_process_status p_process(const clap_plugin_t* p, const clap_process_t* pr) {
    auto* self = static_cast<Plug*>(p->plugin_data);
    ++self->blocks;
#ifdef BREAK_HANG_PROCESS
    for (;;)
        pause();
#endif
    handle_in(self, pr->in_events, pr->out_events);
#ifdef BREAK_NULL_INPUT
    volatile float probe = pr->audio_inputs[0].data32[0][0];
    (void)probe;
#endif
    const bool have_in = pr->audio_inputs_count > 0 && pr->audio_inputs &&
                         pr->audio_inputs[0].channel_count > 0 && pr->audio_inputs[0].data32;
    for (uint32_t c = 0; c < pr->audio_outputs[0].channel_count; ++c) {
        float* o = pr->audio_outputs[0].data32[c];
        const float* in = have_in && c < pr->audio_inputs[0].channel_count
                              ? pr->audio_inputs[0].data32[c]
                              : nullptr;
        for (uint32_t n = 0; n < pr->frames_count; ++n)
            o[n] = (in ? in[n] : 0.0f) * static_cast<float>(self->value[0]);
    }
    return CLAP_PROCESS_CONTINUE;
}
const void* p_ext(const clap_plugin_t*, const char* id) {
    if (!std::strcmp(id, CLAP_EXT_PARAMS))
        return &kParams;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &kPorts;
    if (!std::strcmp(id, CLAP_EXT_STATE))
        return &kState;
    return nullptr;
}
void p_main(const clap_plugin_t*) {}

uint32_t f_count(const clap_plugin_factory_t*) {
    return 1;
}
const clap_plugin_descriptor_t* f_desc(const clap_plugin_factory_t*, uint32_t i) {
#ifdef BREAK_NULL_DESCRIPTOR
    return nullptr;
#endif
    return i == 0 ? &kDesc : nullptr;
}
const clap_plugin_t* f_create(const clap_plugin_factory_t*, const clap_host_t* host,
                              const char* id) {
    if (std::strcmp(id, kDesc.id) != 0)
        return nullptr;
    auto* s = new Plug{};
    s->host = host;
    s->plugin = {&kDesc,  s,      p_init,  p_destroy, p_activate, p_deactivate,
                 p_start, p_stop, p_reset, p_process, p_ext,      p_main};
    return &s->plugin;
}
const clap_plugin_factory_t kFactory = {f_count, f_desc, f_create};

bool e_init(const char*) {
    // Plug-ins commonly log to stdout; the checker's report must survive it.
    std::printf("conformance fixture loaded\n");
#if defined(BREAK_HANG_INIT)
    for (;;)
        pause();
#endif
#if defined(BREAK_HEAVY_MEMORY)
    const size_t n = 128u << 20;
    auto* m = static_cast<volatile char*>(std::malloc(n));
    for (size_t i = 0; i < n; i += 4096)
        m[i] = 1;
#endif
#if defined(BREAK_HEAVY_CPU)
    volatile uint64_t acc = 0;
    for (uint64_t i = 0; i < 250'000'000ull; ++i)
        acc = acc + i;
#endif
    return true;
}
void e_deinit() {}
const void* e_factory(const char* id) {
    return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kFactory : nullptr;
}

} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, e_init, e_deinit,
                                                               e_factory};
