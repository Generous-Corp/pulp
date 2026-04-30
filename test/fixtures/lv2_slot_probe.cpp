#include <lv2/core/lv2.h>

#include <cstdint>

#if defined(_WIN32)
#define PULP_LV2_SLOT_PROBE_EXPORT __declspec(dllexport)
#else
#define PULP_LV2_SLOT_PROBE_EXPORT __attribute__((visibility("default")))
#endif

namespace {

constexpr const char* kDecoyUri = "http://example.com/pulp/lv2-slot-decoy";
constexpr const char* kProbeUri = "http://example.com/pulp/lv2-slot-probe";

enum PortIndex : uint32_t {
    kAudioInL = 0,
    kAudioInR = 1,
    kAudioOutL = 2,
    kAudioOutR = 3,
    kGain = 4,
};

struct ProbeState {
    uint32_t instantiate_count = 0;
    uint32_t activate_count = 0;
    uint32_t run_count = 0;
    uint32_t deactivate_count = 0;
    uint32_t cleanup_count = 0;
    uint32_t decoy_instantiate_count = 0;
    uint32_t last_sample_count = 0;
    float last_gain = 0.0f;
};

struct ProbeInstance {
    const float* in_l = nullptr;
    const float* in_r = nullptr;
    float* out_l = nullptr;
    float* out_r = nullptr;
    const float* gain = nullptr;
};

ProbeState g_state;

LV2_Handle instantiate_decoy(const LV2_Descriptor*,
                             double,
                             const char*,
                             const LV2_Feature* const*) {
    ++g_state.decoy_instantiate_count;
    return nullptr;
}

LV2_Handle instantiate_probe(const LV2_Descriptor*,
                             double,
                             const char*,
                             const LV2_Feature* const*) {
    ++g_state.instantiate_count;
    return new ProbeInstance();
}

void connect_probe_port(LV2_Handle handle, uint32_t port, void* data) {
    auto* probe = static_cast<ProbeInstance*>(handle);
    if (!probe) return;

    switch (port) {
        case kAudioInL:  probe->in_l = static_cast<const float*>(data); break;
        case kAudioInR:  probe->in_r = static_cast<const float*>(data); break;
        case kAudioOutL: probe->out_l = static_cast<float*>(data); break;
        case kAudioOutR: probe->out_r = static_cast<float*>(data); break;
        case kGain:      probe->gain = static_cast<const float*>(data); break;
        default: break;
    }
}

void activate_probe(LV2_Handle) {
    ++g_state.activate_count;
}

void run_probe(LV2_Handle handle, uint32_t sample_count) {
    auto* probe = static_cast<ProbeInstance*>(handle);
    if (!probe) return;

    ++g_state.run_count;
    g_state.last_sample_count = sample_count;

    const float gain = probe->gain ? *probe->gain : 0.0f;
    g_state.last_gain = gain;

    for (uint32_t i = 0; i < sample_count; ++i) {
        const float left = probe->in_l ? probe->in_l[i] : 0.0f;
        const float right = probe->in_r ? probe->in_r[i] : 0.0f;
        if (probe->out_l) probe->out_l[i] = left * gain;
        if (probe->out_r) probe->out_r[i] = right * gain;
    }
}

void deactivate_probe(LV2_Handle) {
    ++g_state.deactivate_count;
}

void cleanup_probe(LV2_Handle handle) {
    ++g_state.cleanup_count;
    delete static_cast<ProbeInstance*>(handle);
}

const LV2_Descriptor kDescriptors[] = {
    {kDecoyUri,
     instantiate_decoy,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr},
    {kProbeUri,
     instantiate_probe,
     connect_probe_port,
     activate_probe,
     run_probe,
     deactivate_probe,
     cleanup_probe,
     nullptr},
};

} // namespace

extern "C" PULP_LV2_SLOT_PROBE_EXPORT
const LV2_Descriptor* lv2_descriptor(uint32_t index) {
    if (index >= 2) return nullptr;
    return &kDescriptors[index];
}

extern "C" PULP_LV2_SLOT_PROBE_EXPORT
void pulp_lv2_slot_probe_reset() {
    g_state = {};
}

extern "C" PULP_LV2_SLOT_PROBE_EXPORT
uint32_t pulp_lv2_slot_probe_instantiate_count() {
    return g_state.instantiate_count;
}

extern "C" PULP_LV2_SLOT_PROBE_EXPORT
uint32_t pulp_lv2_slot_probe_decoy_instantiate_count() {
    return g_state.decoy_instantiate_count;
}

extern "C" PULP_LV2_SLOT_PROBE_EXPORT
uint32_t pulp_lv2_slot_probe_activate_count() {
    return g_state.activate_count;
}

extern "C" PULP_LV2_SLOT_PROBE_EXPORT
uint32_t pulp_lv2_slot_probe_run_count() {
    return g_state.run_count;
}

extern "C" PULP_LV2_SLOT_PROBE_EXPORT
uint32_t pulp_lv2_slot_probe_deactivate_count() {
    return g_state.deactivate_count;
}

extern "C" PULP_LV2_SLOT_PROBE_EXPORT
uint32_t pulp_lv2_slot_probe_cleanup_count() {
    return g_state.cleanup_count;
}

extern "C" PULP_LV2_SLOT_PROBE_EXPORT
uint32_t pulp_lv2_slot_probe_last_sample_count() {
    return g_state.last_sample_count;
}

extern "C" PULP_LV2_SLOT_PROBE_EXPORT
float pulp_lv2_slot_probe_last_gain() {
    return g_state.last_gain;
}
