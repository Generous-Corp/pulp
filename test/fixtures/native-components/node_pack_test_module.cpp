// A minimal pulp_node_v1 node built as a loadable MODULE (.dylib/.so/.dll) for
// the node-pack loader test. Exports pulp_node_v1_entry with default visibility
// so dlsym/GetProcAddress can resolve it.

#if defined(_WIN32)
#define PULP_NODE_V1_EXPORT __declspec(dllexport)
#else
#define PULP_NODE_V1_EXPORT __attribute__((visibility("default")))
#endif

#include <pulp/native_components/pulp_node_v1.h>

#include <cstdint>
#include <cstring>

namespace {

struct GainNode {
    float level = 1.0f;
};

const char kId[] = "pulp.test.pack.gain";
const char kName[] = "Packed Gain";

pulp_node_descriptor_v1 g_desc = [] {
    pulp_node_descriptor_v1 d{};
    d.size = sizeof(d);
    d.abi_major = PULP_NODE_V1_ABI_MAJOR;
    d.stable_id = kId;
    d.stable_id_len = std::strlen(kId);
    d.display_name = kName;
    d.display_name_len = std::strlen(kName);
    d.node_version = 1;
    d.capability_flags = PULP_NODE_CAP_STATE_V1;
    d.audio_input_count = 1;
    d.audio_output_count = 1;
    return d;
}();

const pulp_node_descriptor_v1* descriptor() { return &g_desc; }
pulp_node_status_v1 create(const pulp_node_host_services_v1*,
                           pulp_node_instance_v1** out) {
    *out = reinterpret_cast<pulp_node_instance_v1*>(new GainNode());
    return PULP_NODE_OK_V1;
}
void destroy(pulp_node_instance_v1* i) { delete reinterpret_cast<GainNode*>(i); }
pulp_node_status_v1 prepare(pulp_node_instance_v1*, const pulp_node_prepare_v1*) {
    return PULP_NODE_OK_V1;
}
void reset(pulp_node_instance_v1* i) {
    reinterpret_cast<GainNode*>(i)->level = 1.0f;
}
pulp_node_status_v1 process(pulp_node_instance_v1* i,
                            const pulp_node_audio_v1* a) {
    const float lvl = reinterpret_cast<GainNode*>(i)->level;
    const uint32_t ch =
        a->input_count < a->output_count ? a->input_count : a->output_count;
    for (uint32_t c = 0; c < ch; ++c)
        for (uint32_t s = 0; s < a->frame_count; ++s)
            a->outputs[c][s] = a->inputs[c][s] * lvl;
    return PULP_NODE_OK_V1;
}
void release(pulp_node_instance_v1*) {}
pulp_node_status_v1 save_state(pulp_node_instance_v1* i,
                               const pulp_node_writer_v1* w) {
    const float lvl = reinterpret_cast<GainNode*>(i)->level;
    uint8_t b[sizeof(float)];
    std::memcpy(b, &lvl, sizeof(float));
    w->write(w->writer_context, b, sizeof(b));
    return PULP_NODE_OK_V1;
}
pulp_node_status_v1 load_state(pulp_node_instance_v1* i, const uint8_t* b,
                               size_t n) {
    if (n != sizeof(float)) return PULP_NODE_ERR_MALFORMED_STATE_V1;
    std::memcpy(&reinterpret_cast<GainNode*>(i)->level, b, sizeof(float));
    return PULP_NODE_OK_V1;
}
uint32_t report_latency(pulp_node_instance_v1*) { return 0; }

pulp_node_entry_v1 g_entry = [] {
    pulp_node_entry_v1 e{};
    e.size = sizeof(e);
    e.abi_major = PULP_NODE_V1_ABI_MAJOR;
    e.descriptor = descriptor;
    e.create = create;
    e.destroy = destroy;
    e.prepare = prepare;
    e.reset = reset;
    e.process = process;
    e.release = release;
    e.save_state = save_state;
    e.load_state = load_state;
    e.report_latency = report_latency;
    return e;
}();

}  // namespace

extern "C" PULP_NODE_V1_EXPORT const pulp_node_entry_v1* pulp_node_v1_entry(void) {
    return &g_entry;
}
