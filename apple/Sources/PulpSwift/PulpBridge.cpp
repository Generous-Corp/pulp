// PulpBridge.cpp — C bridge implementation
// Links against pulp::state and pulp::format to expose to Swift

#include "PulpBridge.h"
#include <pulp/format/registry.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/motion.hpp>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

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

// ── Motion observability bridge ─────────────────────────────────────────

namespace {

/// Per-geometry-trace storage. The atomics carry the most-recently
/// published rect; the motion Coordinator's `multi(...)` samplers read
/// them on each FrameClock tick.
///
/// The samplers capture a `shared_ptr<PulpMotionGeometryState>` so they
/// keep working even after `pulp_motion_detach_trace()` removes the
/// registry entry (a tick may already be in flight). The owning
/// `TraceHandle` is intentionally NOT stored inside this struct: a
/// `~TraceHandle` reaches back into `Coordinator::detach()`, which
/// takes the coordinator's internal mutex. If we let that destructor
/// run while the coordinator is *inside* `reset()` (e.g. on
/// `Coordinator::reset()` → `traces.clear()` → spec destruction →
/// lambda destruction → shared_ptr release), we'd self-deadlock.
struct PulpMotionGeometryState {
    std::atomic<double> minx{0.0};
    std::atomic<double> miny{0.0};
    std::atomic<double> width{0.0};
    std::atomic<double> height{0.0};
};

struct PulpMotionGeometryEntry {
    std::shared_ptr<PulpMotionGeometryState> state;
    pulp::view::motion::TraceHandle handle;
    // The view_name that the SwiftUI/UIKit/AppKit probe registered
    // this trace under. Threaded through so out-of-band publish calls
    // (when `metric_name != "frame"` in pulp_motion_update_geometry)
    // can preserve the trace's view identity instead of falling back
    // to a hardcoded "swiftui" label.
    std::string view_name;
};

std::mutex& geometry_mutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<int, PulpMotionGeometryEntry>& geometry_registry() {
    static std::unordered_map<int, PulpMotionGeometryEntry> r;
    return r;
}

std::atomic<int>& next_geometry_token() {
    static std::atomic<int> n{1};
    return n;
}

std::string safe_string(const char* s) {
    return s ? std::string(s) : std::string();
}

} // namespace

extern "C" {

bool pulp_motion_tracing_enabled(void) {
    return pulp::view::motion::Coordinator::instance().tracing_enabled();
}

void pulp_motion_publish_value(const char* view,
                               const char* metric,
                               double value,
                               double epsilon,
                               int precision) {
    if (!pulp::view::motion::Coordinator::instance().tracing_enabled()) return;
    pulp::view::motion::PublishOptions opts;
    opts.epsilon = epsilon;
    opts.precision = precision;
    pulp::view::motion::publish_value(safe_string(view),
                                      safe_string(metric),
                                      value, opts);
}

void pulp_motion_publish_components(const char* view,
                                    const char* metric,
                                    const char* const* keys,
                                    const double* values,
                                    int count,
                                    double epsilon,
                                    int precision) {
    if (!pulp::view::motion::Coordinator::instance().tracing_enabled()) return;
    if (count < 0 || !keys || !values) return;
    std::vector<std::pair<std::string, double>> comps;
    comps.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        comps.emplace_back(safe_string(keys[i]), values[i]);
    }
    pulp::view::motion::PublishOptions opts;
    opts.epsilon = epsilon;
    opts.precision = precision;
    pulp::view::motion::publish_components(safe_string(view),
                                           safe_string(metric),
                                           std::move(comps), opts);
}

void pulp_motion_set_ambient_provenance(const char* kind,
                                        const char* id,
                                        const char* file,
                                        int line) {
    pulp::view::motion::Provenance p;
    p.source_kind = safe_string(kind);
    p.source_id = safe_string(id);
    p.source_file = safe_string(file);
    p.source_line = line;
    pulp::view::motion::set_ambient_provenance(std::move(p));
}

void pulp_motion_clear_ambient_provenance(void) {
    pulp::view::motion::clear_ambient_provenance();
}

int pulp_motion_register_geometry_trace(const char* view_name, int fps) {
    auto& coord = pulp::view::motion::Coordinator::instance();
    if (!coord.tracing_enabled()) return 0;
    auto state = std::make_shared<PulpMotionGeometryState>();

    pulp::view::motion::TraceOptions opts;
    opts.fps = fps > 0 ? fps : 30;

    // Build a multi(...) sampler that pulls from the atomic state. We
    // capture `state` by value so the lambdas keep it alive even if the
    // C ABI map entry is removed concurrently.
    using Component = pulp::view::motion::TraceBuilder::Component;
    std::vector<Component> components;
    components.emplace_back("minX",   [state] { return state->minx.load(std::memory_order_relaxed); });
    components.emplace_back("minY",   [state] { return state->miny.load(std::memory_order_relaxed); });
    components.emplace_back("width",  [state] { return state->width.load(std::memory_order_relaxed); });
    components.emplace_back("height", [state] { return state->height.load(std::memory_order_relaxed); });

    pulp::view::motion::Provenance prov;
    prov.source_kind = "swiftui";
    prov.source_id   = safe_string(view_name);

    // Use a default metric name of "frame"; the Swift side can publish
    // additional metrics via `pulp_motion_update_geometry` calling out
    // to publish_components, but the registered trace itself owns the
    // primary "frame" sampler.
    auto handle = coord.trace(safe_string(view_name), opts)
        .multi("frame", std::move(components), /*precision*/ 2, /*epsilon*/ 0.1)
        .with_provenance(std::move(prov))
        .attach();

    if (!handle.is_attached()) return 0;

    const int token = next_geometry_token().fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(geometry_mutex());
        PulpMotionGeometryEntry entry;
        entry.state = state;
        entry.handle = std::move(handle);
        entry.view_name = safe_string(view_name);
        geometry_registry().emplace(token, std::move(entry));
    }
    return token;
}

void pulp_motion_update_geometry(int trace_id,
                                 const char* metric_name,
                                 double minX,
                                 double minY,
                                 double width,
                                 double height) {
    std::shared_ptr<PulpMotionGeometryState> state;
    std::string trace_view_name;
    {
        std::lock_guard<std::mutex> lock(geometry_mutex());
        auto it = geometry_registry().find(trace_id);
        if (it == geometry_registry().end()) return;
        state = it->second.state;
        trace_view_name = it->second.view_name;
    }
    state->minx.store(minX, std::memory_order_relaxed);
    state->miny.store(minY, std::memory_order_relaxed);
    state->width.store(width, std::memory_order_relaxed);
    state->height.store(height, std::memory_order_relaxed);

    // Also emit an extra `publish_components` so out-of-band metric
    // names (e.g. when a SwiftUI view declares two distinct geometry
    // metrics) ride the publish channel. The registered trace's own
    // `multi("frame", ...)` sampler still fires on the next FrameClock
    // tick, so the primary "frame" metric does not double-emit here.
    const std::string name = safe_string(metric_name);
    if (!name.empty() && name != "frame") {
        if (!pulp::view::motion::Coordinator::instance().tracing_enabled()) return;
        std::vector<std::pair<std::string, double>> comps;
        comps.reserve(4);
        comps.emplace_back("minX",   minX);
        comps.emplace_back("minY",   minY);
        comps.emplace_back("width",  width);
        comps.emplace_back("height", height);
        pulp::view::motion::PublishOptions opts;
        opts.epsilon = 0.1;
        opts.precision = 2;
        // Use the trace's registered view_name. Hardcoding "swiftui"
        // loses the trace identity and makes it impossible to correlate
        // out-of-band metrics with their owning SwiftUI/UIKit view.
        pulp::view::motion::publish_components(
            trace_view_name.empty() ? std::string("swiftui") : trace_view_name,
            name, std::move(comps), opts);
    }
}

void pulp_motion_detach_trace(int trace_id) {
    PulpMotionGeometryEntry entry;
    {
        std::lock_guard<std::mutex> lock(geometry_mutex());
        auto it = geometry_registry().find(trace_id);
        if (it == geometry_registry().end()) return;
        entry = std::move(it->second);
        geometry_registry().erase(it);
    }
    // `entry` goes out of scope here. The TraceHandle destructor calls
    // Coordinator::detach() which takes the coordinator mutex — fine,
    // because we are no longer holding the geometry mutex.
}

} // extern "C"
