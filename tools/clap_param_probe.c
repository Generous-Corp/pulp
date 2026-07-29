// Ask a .clap bundle how many parameters it actually exposes.
//
// Three passes of reading the adapter ruled out three theories for the two
// failing flush tests without confirming one. The question underneath is
// simply whether any parameter exists to flush, and only the built binary can
// answer that -- maybe_synthesize_bypass is conditional on a host quirk, so
// the source does not settle it.
#include <dlfcn.h>
#include <stdio.h>
#include <clap/clap.h>

static clap_event_param_value_t g_ev;

static uint32_t probe_in_size(const clap_input_events_t* list) { (void)list; return 1; }
static const clap_event_header_t* probe_in_get(const clap_input_events_t* list, uint32_t i) {
    (void)list; (void)i; return &g_ev.header;
}
static bool probe_out_push(const clap_output_events_t* list, const clap_event_header_t* h) {
    (void)list; (void)h; return true;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: probe <binary>\n"); return 2; }
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }

    const clap_plugin_entry_t* entry =
        (const clap_plugin_entry_t*)dlsym(lib, "clap_entry");
    if (!entry) { fprintf(stderr, "no clap_entry\n"); return 1; }
    if (!entry->init(argv[1])) { fprintf(stderr, "entry init failed\n"); return 1; }

    const clap_plugin_factory_t* factory =
        (const clap_plugin_factory_t*)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    if (!factory) { fprintf(stderr, "no factory\n"); return 1; }
    printf("plugins: %u\n", factory->get_plugin_count(factory));

    const clap_plugin_descriptor_t* desc = factory->get_plugin_descriptor(factory, 0);
    printf("id: %s\n", desc ? desc->id : "(none)");

    // A minimal host. The adapter reads quirks off it, and that is exactly the
    // path that decides whether a Bypass parameter is synthesized.
    static clap_host_t host = {
        .clap_version = CLAP_VERSION_INIT, .host_data = NULL,
        .name = "probe", .vendor = "pulp", .url = "", .version = "0",
        .get_extension = NULL, .request_restart = NULL,
        .request_process = NULL, .request_callback = NULL,
    };
    const clap_plugin_t* p = factory->create_plugin(factory, &host, desc->id);
    if (!p) { fprintf(stderr, "create_plugin failed\n"); return 1; }
    if (!p->init(p)) { fprintf(stderr, "plugin init failed\n"); return 1; }

    const clap_plugin_params_t* params =
        (const clap_plugin_params_t*)p->get_extension(p, CLAP_EXT_PARAMS);
    if (!params) { printf("params extension: ABSENT\n"); return 0; }

    uint32_t n = params->count(p);
    printf("params_count: %u\n", n);
    for (uint32_t i = 0; i < n; ++i) {
        clap_param_info_t info;
        if (!params->get_info(p, i, &info)) continue;
        double v = 0;
        params->get_value(p, info.id, &v);
        printf("  [%u] id=%u \"%s\" min=%.3f max=%.3f value=%.3f flags=0x%x\n",
               i, info.id, info.name, info.min_value, info.max_value, v, info.flags);
    }

    // Reproduce the validator's param-set-events check: push one param-value
    // event through flush() and read the value back. This is the whole of the
    // failing test, isolated from everything else it does.
    if (n > 0) {
        clap_param_info_t info;
        params->get_info(p, 0, &info);
        double before = 0;
        params->get_value(p, info.id, &before);

        clap_event_param_value_t ev;
        #define ev g_ev
        ev.header.size = sizeof(ev);
        ev.header.time = 0;
        ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev.header.type = CLAP_EVENT_PARAM_VALUE;
        ev.header.flags = 0;
        ev.param_id = info.id;
        ev.cookie = NULL;
        ev.note_id = -1; ev.port_index = -1; ev.channel = -1; ev.key = -1;
        ev.value = 1.0;

        clap_input_events_t in = {
            .ctx = NULL,
            .size = probe_in_size,
            .get  = probe_in_get,
        };
        clap_output_events_t out = { .ctx = NULL, .try_push = probe_out_push };

        params->flush(p, &in, &out);

        double after = 0;
        params->get_value(p, info.id, &after);
        printf("flush: before=%.3f requested=1.000 after=%.3f -> %s\n",
               before, after, after == 1.0 ? "APPLIED" : "NOT APPLIED");
    }
    return 0;
}
