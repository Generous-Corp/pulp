// PulpMotionBridge.cpp: C bridge for the motion observability shims
// Links against pulp::view to expose motion::Coordinator to Swift.
// The parameter and state shims live in PulpBridge.cpp and need no view headers.

#include "PulpBridge.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <pulp/view/motion.hpp>
#include <string>
#include <unordered_map>
#include <vector>

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

void pulp_motion_publish_value(const char* view, const char* metric, double value, double epsilon,
                               int precision) {
    if (!pulp::view::motion::Coordinator::instance().tracing_enabled())
        return;
    pulp::view::motion::PublishOptions opts;
    opts.epsilon = epsilon;
    opts.precision = precision;
    pulp::view::motion::publish_value(safe_string(view), safe_string(metric), value, opts);
}

void pulp_motion_publish_components(const char* view, const char* metric, const char* const* keys,
                                    const double* values, int count, double epsilon,
                                    int precision) {
    if (!pulp::view::motion::Coordinator::instance().tracing_enabled())
        return;
    if (count < 0 || !keys || !values)
        return;
    std::vector<std::pair<std::string, double>> comps;
    comps.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        comps.emplace_back(safe_string(keys[i]), values[i]);
    }
    pulp::view::motion::PublishOptions opts;
    opts.epsilon = epsilon;
    opts.precision = precision;
    pulp::view::motion::publish_components(safe_string(view), safe_string(metric), std::move(comps),
                                           opts);
}

void pulp_motion_set_ambient_provenance(const char* kind, const char* id, const char* file,
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
    if (!coord.tracing_enabled())
        return 0;
    auto state = std::make_shared<PulpMotionGeometryState>();

    pulp::view::motion::TraceOptions opts;
    opts.fps = fps > 0 ? fps : 30;

    // Build a multi(...) sampler that pulls from the atomic state. We
    // capture `state` by value so the lambdas keep it alive even if the
    // C ABI map entry is removed concurrently.
    using Component = pulp::view::motion::TraceBuilder::Component;
    std::vector<Component> components;
    components.emplace_back("minX",
                            [state] { return state->minx.load(std::memory_order_relaxed); });
    components.emplace_back("minY",
                            [state] { return state->miny.load(std::memory_order_relaxed); });
    components.emplace_back("width",
                            [state] { return state->width.load(std::memory_order_relaxed); });
    components.emplace_back("height",
                            [state] { return state->height.load(std::memory_order_relaxed); });

    pulp::view::motion::Provenance prov;
    prov.source_kind = "swiftui";
    prov.source_id = safe_string(view_name);

    // Use a default metric name of "frame"; the Swift side can publish
    // additional metrics via `pulp_motion_update_geometry` calling out
    // to publish_components, but the registered trace itself owns the
    // primary "frame" sampler.
    auto handle = coord.trace(safe_string(view_name), opts)
                      .multi("frame", std::move(components), /*precision*/ 2, /*epsilon*/ 0.1)
                      .with_provenance(std::move(prov))
                      .attach();

    if (!handle.is_attached())
        return 0;

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

void pulp_motion_update_geometry(int trace_id, const char* metric_name, double minX, double minY,
                                 double width, double height) {
    std::shared_ptr<PulpMotionGeometryState> state;
    std::string trace_view_name;
    {
        std::lock_guard<std::mutex> lock(geometry_mutex());
        auto it = geometry_registry().find(trace_id);
        if (it == geometry_registry().end())
            return;
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
        if (!pulp::view::motion::Coordinator::instance().tracing_enabled())
            return;
        std::vector<std::pair<std::string, double>> comps;
        comps.reserve(4);
        comps.emplace_back("minX", minX);
        comps.emplace_back("minY", minY);
        comps.emplace_back("width", width);
        comps.emplace_back("height", height);
        pulp::view::motion::PublishOptions opts;
        opts.epsilon = 0.1;
        opts.precision = 2;
        // Use the trace's registered view_name. Hardcoding "swiftui"
        // loses the trace identity and makes it impossible to correlate
        // out-of-band metrics with their owning SwiftUI/UIKit view.
        pulp::view::motion::publish_components(trace_view_name.empty() ? std::string("swiftui")
                                                                       : trace_view_name,
                                               name, std::move(comps), opts);
    }
}

void pulp_motion_detach_trace(int trace_id) {
    PulpMotionGeometryEntry entry;
    {
        std::lock_guard<std::mutex> lock(geometry_mutex());
        auto it = geometry_registry().find(trace_id);
        if (it == geometry_registry().end())
            return;
        entry = std::move(it->second);
        geometry_registry().erase(it);
    }
    // `entry` goes out of scope here. The TraceHandle destructor calls
    // Coordinator::detach() which takes the coordinator mutex, fine,
    // because we are no longer holding the geometry mutex.
}

} // extern "C"
