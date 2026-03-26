// PulpBridge.cpp — C bridge implementation
// Links against pulp::state and pulp::format to expose to Swift

#include "PulpBridge.h"
#include <pulp/format/registry.hpp>
#include <pulp/state/store.hpp>
#include <cstring>
#include <cstdlib>

// The bridge operates on a global StateStore and Processor registered
// via the format registry. This is set up by the AUv3 adapter or
// standalone host before Swift code runs.

namespace {

pulp::state::StateStore* active_store() {
    // In a real plugin, the AUv3 adapter owns the store.
    // This bridge function will be wired to the active instance.
    // For now, return nullptr — the AUv3 adapter sets this up.
    static pulp::state::StateStore fallback;
    return &fallback;
}

} // namespace

extern "C" {

int pulp_param_count(void) {
    auto* store = active_store();
    return store ? static_cast<int>(store->param_count()) : 0;
}

bool pulp_param_info(int index, PulpParamInfo* out) {
    auto* store = active_store();
    if (!store || !out) return false;
    auto params = store->all_params();
    if (index < 0 || static_cast<size_t>(index) >= params.size()) return false;
    auto& p = params[static_cast<size_t>(index)];
    out->id = p.id;
    out->name = p.name.c_str();
    out->unit = p.unit.c_str();
    out->min_value = p.range.min;
    out->max_value = p.range.max;
    out->default_value = p.range.default_value;
    out->step = p.range.step;
    return true;
}

float pulp_param_get(PulpParamID id) {
    auto* store = active_store();
    return store ? store->get_value(id) : 0.0f;
}

void pulp_param_set(PulpParamID id, float value) {
    auto* store = active_store();
    if (store) store->set_value(id, value);
}

float pulp_param_get_normalized(PulpParamID id) {
    auto* store = active_store();
    return store ? store->get_normalized(id) : 0.0f;
}

void pulp_param_set_normalized(PulpParamID id, float normalized) {
    auto* store = active_store();
    if (store) store->set_normalized(id, normalized);
}

void pulp_param_begin_gesture(PulpParamID id) {
    auto* store = active_store();
    if (store) store->begin_gesture(id);
}

void pulp_param_end_gesture(PulpParamID id) {
    auto* store = active_store();
    if (store) store->end_gesture(id);
}

void pulp_param_reset(PulpParamID id) {
    auto* store = active_store();
    if (store) store->reset_to_default(id);
}

bool pulp_plugin_info(PulpPluginInfo* out) {
    auto factory = pulp::format::registered_factory();
    if (!factory || !out) return false;
    auto proc = factory();
    if (!proc) return false;
    auto desc = proc->descriptor();
    // Note: these strings point to temporary storage — caller should copy
    static std::string s_name, s_mfr, s_ver, s_bid;
    s_name = desc.name;
    s_mfr = desc.manufacturer;
    s_ver = desc.version;
    s_bid = desc.bundle_id;
    out->name = s_name.c_str();
    out->manufacturer = s_mfr.c_str();
    out->version = s_ver.c_str();
    out->bundle_id = s_bid.c_str();
    out->category = static_cast<int>(desc.category);
    out->accepts_midi = desc.accepts_midi;
    out->produces_midi = desc.produces_midi;
    return true;
}

uint8_t* pulp_state_serialize(int* out_size) {
    auto* store = active_store();
    if (!store || !out_size) return nullptr;
    auto data = store->serialize();
    if (data.empty()) { *out_size = 0; return nullptr; }
    auto* buf = static_cast<uint8_t*>(malloc(data.size()));
    memcpy(buf, data.data(), data.size());
    *out_size = static_cast<int>(data.size());
    return buf;
}

bool pulp_state_deserialize(const uint8_t* data, int size) {
    auto* store = active_store();
    if (!store || !data || size <= 0) return false;
    return store->deserialize({data, static_cast<size_t>(size)});
}

void pulp_free(void* ptr) {
    free(ptr);
}

} // extern "C"
