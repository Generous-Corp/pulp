#include <pulp/view/motion.hpp>

#include <pulp/view/motion_cost.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/motion_preferences.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>

#include "motion_geometry_internal.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::view::motion {


// ── Metric specs ──────────────────────────────────────────────────────

struct MetricBase {
    std::string name;
    int precision = 3;
    double epsilon = 0.0001;
    virtual ~MetricBase() = default;
    virtual std::vector<std::pair<std::string, double>> sample() const = 0;
};

struct ValueMetric : MetricBase {
    std::vector<std::pair<std::string, TraceBuilder::ValueSampler>> components;
    std::vector<std::pair<std::string, double>> sample() const override {
        std::vector<std::pair<std::string, double>> out;
        out.reserve(components.size());
        for (const auto& c : components) {
            out.emplace_back(c.first, c.second());
        }
        return out;
    }
};

struct GeometryMetric : MetricBase {
    pulp::view::View* target = nullptr;
    std::vector<GeometryProperty> props;
    GeometrySpace space = GeometrySpace::Window;
    GeometrySource source = GeometrySource::Layout;
    std::vector<std::pair<std::string, double>> sample() const override {
        std::vector<std::pair<std::string, double>> out;
        if (!target) return out;
        const Rect r = resolve_geometry(*target, space, source);
        for (auto p : props) {
            out.emplace_back(property_name(p), extract_property(r, p));
        }
        return out;
    }
};

struct ScrollGeometryMetric : MetricBase {
    pulp::view::ScrollView* target = nullptr;
    std::vector<ScrollProperty> props;
    std::vector<std::pair<std::string, double>> sample() const override {
        std::vector<std::pair<std::string, double>> out;
        if (!target) return out;
        out.reserve(props.size());
        for (auto p : props) {
            out.emplace_back(scroll_property_name(p),
                             extract_scroll_property(*target, p));
        }
        return out;
    }
};

struct TraceBuilder::Spec {
    std::string view_name;
    TraceOptions opts;
    std::vector<std::unique_ptr<MetricBase>> metrics;
    Provenance provenance;
};

// ── TraceBuilder ──────────────────────────────────────────────────────

TraceBuilder::TraceBuilder(Coordinator* coord, std::string view_name,
                           TraceOptions opts)
    : spec_(std::make_shared<Spec>()), coord_(coord) {
    spec_->view_name = std::move(view_name);
    spec_->opts = opts;
}

TraceBuilder& TraceBuilder::value(std::string name, ValueSampler sampler,
                                  int precision, double epsilon) {
    auto m = std::make_unique<ValueMetric>();
    m->name = std::move(name);
    m->precision = precision;
    m->epsilon = epsilon;
    m->components.emplace_back("value", std::move(sampler));
    spec_->metrics.push_back(std::move(m));
    return *this;
}

TraceBuilder& TraceBuilder::multi(std::string name,
                                  std::vector<Component> components,
                                  int precision, double epsilon) {
    auto m = std::make_unique<ValueMetric>();
    m->name = std::move(name);
    m->precision = precision;
    m->epsilon = epsilon;
    m->components = std::move(components);
    spec_->metrics.push_back(std::move(m));
    return *this;
}

TraceBuilder& TraceBuilder::geometry(std::string name,
                                     pulp::view::View& target,
                                     std::vector<GeometryProperty> props,
                                     GeometrySpace space,
                                     GeometrySource source,
                                     int precision, double epsilon) {
    auto m = std::make_unique<GeometryMetric>();
    m->name = std::move(name);
    m->precision = precision;
    m->epsilon = epsilon;
    m->target = &target;
    m->props = std::move(props);
    m->space = space;
    m->source = source;
    spec_->metrics.push_back(std::move(m));
    return *this;
}

TraceBuilder& TraceBuilder::scroll_geometry(std::string name,
                                            pulp::view::ScrollView& target,
                                            std::vector<ScrollProperty> props,
                                            int precision, double epsilon) {
    auto m = std::make_unique<ScrollGeometryMetric>();
    m->name = std::move(name);
    m->precision = precision;
    m->epsilon = epsilon;
    m->target = &target;
    // Empty props → preserve the documented 4-property default. (The
    // header default applies to direct C++ callers; an empty vector
    // passed explicitly — including over the JSON inspector bridge —
    // gets the same shape.)
    if (props.empty()) {
        props = {
            ScrollProperty::ContentOffsetX, ScrollProperty::ContentOffsetY,
            ScrollProperty::VisibleRectMinY, ScrollProperty::VisibleRectHeight,
        };
    }
    m->props = std::move(props);
    spec_->metrics.push_back(std::move(m));
    return *this;
}

TraceBuilder& TraceBuilder::with_provenance(Provenance p) {
    if (spec_) spec_->provenance = std::move(p);
    return *this;
}

TraceHandle TraceBuilder::attach() {
    if (!coord_) return TraceHandle();
    const int id = coord_->register_trace(spec_);
    return TraceHandle(id, coord_);
}

// ── TraceHandle ───────────────────────────────────────────────────────

TraceHandle::TraceHandle(TraceHandle&& other) noexcept
    : id_(other.id_), coord_(other.coord_) {
    other.id_ = 0;
    other.coord_ = nullptr;
}

TraceHandle& TraceHandle::operator=(TraceHandle&& other) noexcept {
    if (this != &other) {
        detach();
        id_ = other.id_;
        coord_ = other.coord_;
        other.id_ = 0;
        other.coord_ = nullptr;
    }
    return *this;
}

TraceHandle::~TraceHandle() { detach(); }

void TraceHandle::detach() {
    if (coord_) {
        coord_->detach(id_);
        coord_ = nullptr;
        id_ = 0;
    }
}

// ── format_line + sinks ───────────────────────────────────────────────

std::string format_line(const SampleEvent& e) {
    std::ostringstream ss;
    ss << "[PulpMotion][" << e.view_name << "][" << e.metric_name << "]";
    switch (e.kind) {
        case SampleEvent::Kind::TraceStarted:
            ss << " -- TraceStarted trace_id=" << e.trace_id
               << " frame=" << e.frame
               << " t=" << fmt_double(e.t_seconds, 6) << " --";
            if (e.provenance.is_set()) {
                ss << " source=" << e.provenance.source_kind;
                if (!e.provenance.source_id.empty())
                    ss << " id=" << e.provenance.source_id;
                if (!e.provenance.source_file.empty())
                    ss << " at=" << e.provenance.source_file
                       << ":" << e.provenance.source_line;
            }
            break;
        case SampleEvent::Kind::Baseline:
        case SampleEvent::Kind::Sample: {
            bool first = true;
            for (const auto& [k, v] : e.components) {
                ss << (first ? " " : " ");
                ss << k << "=" << fmt_double(v, e.precision);
                first = false;
            }
            break;
        }
        case SampleEvent::Kind::Start:
            ss << " -- Start burst=" << e.burst_id
               << " frame=" << e.frame
               << " t=" << fmt_double(e.t_seconds, 6) << " --";
            break;
        case SampleEvent::Kind::End:
            ss << " -- End burst=" << e.burst_id
               << " frame=" << e.frame
               << " t=" << fmt_double(e.t_seconds, 6) << " --";
            for (const auto& [k, v] : e.deltas) {
                ss << " " << k << "Delta=" << fmt_double(v, e.precision);
            }
            break;
        case SampleEvent::Kind::Input:
            ss << " -- Input kind=" << e.input_kind
               << " view_id=" << e.view_id
               << " frame=" << e.frame
               << " t=" << fmt_double(e.t_seconds, 6) << " --";
            for (const auto& [k, v] : e.components) {
                ss << " " << k << "=" << fmt_double(v, e.precision);
            }
            break;
    }
    return ss.str();
}

Sink make_log_sink() {
    return [](const SampleEvent& e) {
        pulp::runtime::log_debug("{}", format_line(e));
    };
}

Sink make_buffer_sink(std::vector<SampleEvent>* buffer) {
    return [buffer](const SampleEvent& e) {
        if (buffer) buffer->push_back(e);
    };
}

// ── Publish channel ──────────────────────────────────────────────────

void publish_value(std::string view_name,
                   std::string metric_name,
                   double value,
                   PublishOptions opts) {
    std::vector<std::pair<std::string, double>> comps;
    comps.emplace_back("value", value);
    publish_components(std::move(view_name), std::move(metric_name),
                       std::move(comps), opts);
}

void publish_components(std::string view_name,
                        std::string metric_name,
                        std::vector<std::pair<std::string, double>> components,
                        PublishOptions opts) {
    if (components.empty()) return;
    Coordinator::instance().publish_internal(std::move(view_name),
                                             std::move(metric_name),
                                             std::move(components), opts);
}

// ── Ambient provenance ────────────────────────────────────────────────
//
// The ambient slot lives on the Coordinator's State (under its mutex)
// so concurrent publishes from any sink-fed thread see a coherent
// snapshot. Read by `publish_internal` when a caller's PublishOptions
// carries an empty provenance envelope.

void set_ambient_provenance(Provenance p) {
    Coordinator::instance().set_ambient_provenance_internal(std::move(p));
}

void clear_ambient_provenance() {
    Coordinator::instance().set_ambient_provenance_internal(Provenance{});
}

Provenance current_ambient_provenance() {
    return Coordinator::instance().current_ambient_provenance_internal();
}

// ── Coordinator internals ─────────────────────────────────────────────

struct PerMetricState {
    int precision = 3;
    double epsilon = 0.0001;
    std::vector<std::pair<std::string, double>> current;
    std::vector<std::pair<std::string, double>> last_emitted;
    std::vector<std::pair<std::string, double>> motion_start;
    bool has_baseline = false;
    bool in_motion = false;
    int next_burst_id = 1;       // Assigned at each Start.
    int current_burst_id = 0;    // 0 between bursts; set on Start.
};

struct ActiveTrace {
    int id = 0;
    std::shared_ptr<TraceBuilder::Spec> spec;
    std::vector<PerMetricState> metrics;
    double accum_seconds = 0.0;
    bool needs_trace_started = true;  // Emit TraceStarted on first tick.
};

struct PublishKey {
    std::string view_name;
    std::string metric_name;
    bool operator<(const PublishKey& o) const {
        if (view_name != o.view_name) return view_name < o.view_name;
        return metric_name < o.metric_name;
    }
};

struct PublishState {
    int precision = 3;
    double epsilon = 0.0001;
    std::vector<std::pair<std::string, double>> last_emitted;
    std::vector<std::pair<std::string, double>> motion_start;
    bool has_baseline = false;
    bool in_motion = false;
    int next_burst_id = 1;
    int current_burst_id = 0;
};

struct Coordinator::State {
    mutable std::mutex mtx;
    pulp::view::FrameClock* clock = nullptr;
    int clock_sub_id = 0;
    bool tracing_enabled = false;
    bool firehose = false;
    std::map<int, Sink> sinks;
    int next_sink_id = 1;
    std::map<int, ActiveTrace> traces;
    int next_trace_id = 1;
    std::size_t emitted_count = 0;
    // Per-(view, metric) burst state for publish_*. Independent of the
    // sampler-driven trace map above so publishes and sampled traces
    // don't interfere even on the same view/metric name.
    std::map<PublishKey, PublishState> publish_states;
    // Ambient publish provenance. Set via `set_ambient_provenance`
    // and stamped onto publishes whose
    // PublishOptions::provenance is empty. Single global slot — intended
    // for single-threaded scripted contexts.
    Provenance ambient_provenance;
};

// ── Coordinator ───────────────────────────────────────────────────────

Coordinator& Coordinator::instance() {
    static Coordinator inst;
    return inst;
}

Coordinator::Coordinator() : state_(std::make_unique<State>()) {}
Coordinator::~Coordinator() { unbind(); }

void Coordinator::bind(pulp::view::FrameClock& clock) {
    unbind();
    std::lock_guard<std::mutex> lock(state_->mtx);
    state_->clock = &clock;
    state_->clock_sub_id = clock.subscribe([this](float dt) {
        on_tick(dt);
        return true;
    });
}

void Coordinator::unbind() {
    pulp::view::FrameClock* clock = nullptr;
    int sub_id = 0;
    {
        std::lock_guard<std::mutex> lock(state_->mtx);
        clock = state_->clock;
        sub_id = state_->clock_sub_id;
        state_->clock = nullptr;
        state_->clock_sub_id = 0;
    }
    if (clock && sub_id) clock->unsubscribe(sub_id);
}

bool Coordinator::is_bound() const noexcept {
    std::lock_guard<std::mutex> lock(state_->mtx);
    return state_->clock != nullptr;
}

int Coordinator::add_sink(Sink sink) {
    std::lock_guard<std::mutex> lock(state_->mtx);
    const int id = state_->next_sink_id++;
    state_->sinks.emplace(id, std::move(sink));
    return id;
}

void Coordinator::remove_sink(int id) {
    std::lock_guard<std::mutex> lock(state_->mtx);
    state_->sinks.erase(id);
}

void Coordinator::clear_sinks() {
    std::lock_guard<std::mutex> lock(state_->mtx);
    state_->sinks.clear();
}

int Coordinator::install_default_log_sink() {
    return add_sink(make_log_sink());
}

void Coordinator::set_tracing_enabled(bool on) {
    std::lock_guard<std::mutex> lock(state_->mtx);
    state_->tracing_enabled = on;
}

bool Coordinator::tracing_enabled() const noexcept {
    std::lock_guard<std::mutex> lock(state_->mtx);
    return state_->tracing_enabled;
}

void Coordinator::set_firehose(bool on) {
    std::lock_guard<std::mutex> lock(state_->mtx);
    state_->firehose = on;
}

bool Coordinator::firehose() const noexcept {
    std::lock_guard<std::mutex> lock(state_->mtx);
    return state_->firehose;
}

TraceBuilder Coordinator::trace(std::string view_name, TraceOptions opts) {
    return TraceBuilder(this, std::move(view_name), opts);
}

int Coordinator::register_trace(std::shared_ptr<TraceBuilder::Spec> spec) {
    std::lock_guard<std::mutex> lock(state_->mtx);
    const int id = state_->next_trace_id++;
    ActiveTrace t;
    t.id = id;
    t.spec = spec;
    t.metrics.resize(spec->metrics.size());
    for (std::size_t i = 0; i < spec->metrics.size(); ++i) {
        t.metrics[i].precision = spec->metrics[i]->precision;
        t.metrics[i].epsilon = spec->metrics[i]->epsilon;
    }
    state_->traces.emplace(id, std::move(t));
    return id;
}

void Coordinator::detach(int trace_id) {
    std::lock_guard<std::mutex> lock(state_->mtx);
    state_->traces.erase(trace_id);
}

void Coordinator::reset() {
    unbind();
    std::lock_guard<std::mutex> lock(state_->mtx);
    state_->sinks.clear();
    state_->traces.clear();
    state_->publish_states.clear();
    state_->tracing_enabled = false;
    state_->firehose = false;
    state_->emitted_count = 0;
    state_->next_sink_id = 1;
    state_->next_trace_id = 1;
    state_->ambient_provenance = Provenance{};
}

void Coordinator::set_ambient_provenance_internal(Provenance p) {
    std::lock_guard<std::mutex> lock(state_->mtx);
    state_->ambient_provenance = std::move(p);
}

Provenance Coordinator::current_ambient_provenance_internal() const {
    std::lock_guard<std::mutex> lock(state_->mtx);
    return state_->ambient_provenance;
}

std::size_t Coordinator::active_trace_count() const noexcept {
    std::lock_guard<std::mutex> lock(state_->mtx);
    return state_->traces.size();
}

std::size_t Coordinator::emitted_event_count() const noexcept {
    std::lock_guard<std::mutex> lock(state_->mtx);
    return state_->emitted_count;
}

namespace {

bool components_differ(
    const std::vector<std::pair<std::string, double>>& a,
    const std::vector<std::pair<std::string, double>>& b,
    double epsilon
) {
    if (a.size() != b.size()) return true;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].first != b[i].first) return true;
        const double av = a[i].second;
        const double bv = b[i].second;
        // IEEE-754 special handling: a transition into or out of a
        // non-finite value is meaningful (an animation going NaN/Inf
        // is a debug signal, not a no-op). fabs(av - bv) would yield
        // NaN and silently mask the change because NaN > epsilon is
        // false.
        const bool a_finite = std::isfinite(av);
        const bool b_finite = std::isfinite(bv);
        if (a_finite != b_finite) return true;
        if (!a_finite && !b_finite) {
            // Both non-finite: bitwise compare so NaN vs +Inf vs -Inf
            // are all distinguishable. (NaN != NaN under ==, but
            // memcmp distinguishes payloads — for our purposes any
            // NaN-to-NaN is "still NaN," not a change.)
            if (std::isnan(av) != std::isnan(bv)) return true;
            if (!std::isnan(av) && av != bv) return true;
            continue;
        }
        if (std::fabs(av - bv) > epsilon) return true;
    }
    return false;
}

void sort_components(std::vector<std::pair<std::string, double>>& v) {
    std::sort(v.begin(), v.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
}

}  // namespace

void Coordinator::on_tick(float dt) {
    // Cost attribution observes every tick (when enabled) regardless
    // of whether the event sinks are populated. note_tick_begin runs
    // first so per-tick scratch is fresh; emit_frame runs at the very
    // end so trace activity has had a chance to accumulate.
    auto& cost = CostAttributor::instance();
    const bool cost_enabled = cost.enabled();
    if (cost_enabled) {
        const double t_cost =
            state_->clock ? static_cast<double>(state_->clock->time()) : 0.0;
        const std::uint64_t f_cost =
            state_->clock ? state_->clock->frame() : 0;
        cost.note_tick_begin(f_cost, t_cost);
    }

    // Collect events under lock, dispatch outside the lock.
    std::vector<std::pair<Sink, SampleEvent>> pending;
    // Trace activity (trace_id + provenance envelope) recorded under
    // the lock; forwarded to the CostAttributor after the lock is
    // released so the attributor can sample its probe without
    // contending on the coordinator mutex.
    std::vector<std::pair<int, Provenance>> tick_activity;
    {
        std::lock_guard<std::mutex> lock(state_->mtx);
        // Cost attribution must still emit a sample even when there
        // are no active traces / sinks — render cost itself is the
        // useful signal in that case. So bail only when BOTH tracing
        // and cost attribution are off.
        const bool have_traces =
            state_->tracing_enabled && !state_->sinks.empty();
        if (!have_traces && !cost_enabled) return;

        const double t_now =
            state_->clock ? static_cast<double>(state_->clock->time()) : 0.0;
        const std::uint64_t f_now =
            state_->clock ? state_->clock->frame() : 0;

        if (!have_traces) {
            // Skip sampler-driven trace work; cost emission still
            // happens below.
            goto cost_emit;
        }

        for (auto& [trace_id, trace] : state_->traces) {
            // Emit TraceStarted once per trace on its first tick.
            if (trace.needs_trace_started) {
                SampleEvent ev;
                ev.kind = SampleEvent::Kind::TraceStarted;
                ev.view_name = trace.spec->view_name;
                ev.t_seconds = t_now;
                ev.frame = f_now;
                ev.trace_id = trace.id;
                ev.metric_id = 0;
                ev.burst_id = 0;
                ev.provenance = trace.spec->provenance;
                for (const auto& [sid, sink] : state_->sinks) {
                    (void)sid;
                    pending.emplace_back(sink, ev);
                }
                state_->emitted_count++;
                if (cost_enabled) {
                    tick_activity.emplace_back(trace.id,
                                               trace.spec->provenance);
                }
                trace.needs_trace_started = false;
            }

            const int fps = std::max(1, trace.spec->opts.fps);
            const double period = 1.0 / static_cast<double>(fps);
            trace.accum_seconds += static_cast<double>(dt);
            if (trace.accum_seconds + 1e-9 < period) continue;
            trace.accum_seconds -= period;
            // Drift cap: if dt > 2 periods we sample once then reset the
            // accumulator instead of bursting catch-up samples.
            if (trace.accum_seconds > period * 2.0) trace.accum_seconds = 0.0;

            for (std::size_t i = 0; i < trace.spec->metrics.size(); ++i) {
                auto& metric = *trace.spec->metrics[i];
                auto& mstate = trace.metrics[i];

                auto sampled = metric.sample();
                if (sampled.empty()) continue;
                sort_components(sampled);
                mstate.current = sampled;

                auto enqueue = [&](SampleEvent::Kind kind,
                                   std::vector<std::pair<std::string, double>> comps,
                                   std::vector<std::pair<std::string, double>> deltas = {}) {
                    SampleEvent e;
                    e.kind = kind;
                    e.view_name = trace.spec->view_name;
                    e.metric_name = metric.name;
                    e.t_seconds = t_now;
                    e.frame = f_now;
                    e.precision = mstate.precision;
                    e.trace_id = trace.id;
                    e.metric_id = static_cast<int>(i);
                    e.burst_id = mstate.current_burst_id;
                    e.components = std::move(comps);
                    e.deltas = std::move(deltas);
                    for (const auto& [sid, sink] : state_->sinks) {
                        (void)sid;
                        pending.emplace_back(sink, e);
                    }
                    state_->emitted_count++;
                    // Cost attribution: track every emission (including
                    // Baseline / Start / End markers) as "this trace was
                    // active this tick". TraceStarted is one-shot and
                    // emitted above; it's also legitimate cost-side
                    // activity so it's recorded with the rest below.
                    if (cost_enabled) {
                        tick_activity.emplace_back(trace.id,
                                                   trace.spec->provenance);
                    }
                };

                if (!mstate.has_baseline) {
                    enqueue(SampleEvent::Kind::Baseline, mstate.current);
                    mstate.last_emitted = mstate.current;
                    mstate.has_baseline = true;
                    continue;
                }

                const bool changed = components_differ(mstate.current,
                                                       mstate.last_emitted,
                                                       mstate.epsilon);
                if (changed) {
                    if (!mstate.in_motion) {
                        mstate.current_burst_id = mstate.next_burst_id++;
                        enqueue(SampleEvent::Kind::Start, {});
                        mstate.motion_start = mstate.last_emitted;
                        mstate.in_motion = true;
                    }
                    enqueue(SampleEvent::Kind::Sample, mstate.current);
                    mstate.last_emitted = mstate.current;
                } else if (mstate.in_motion) {
                    std::vector<std::pair<std::string, double>> deltas;
                    deltas.reserve(mstate.current.size());
                    for (const auto& [name, cur] : mstate.current) {
                        double start = 0.0;
                        for (const auto& [sn, sv] : mstate.motion_start) {
                            if (sn == name) { start = sv; break; }
                        }
                        deltas.emplace_back(name, cur - start);
                    }
                    enqueue(SampleEvent::Kind::End, {}, std::move(deltas));
                    mstate.in_motion = false;
                    mstate.motion_start.clear();
                    mstate.current_burst_id = 0;
                }
            }
        }
    cost_emit:
        (void)0;
    }
    for (auto& [sink, ev] : pending) sink(ev);

    // Forward per-tick trace activity to the CostAttributor and emit
    // a CostSample. Done outside the coordinator lock so the probe
    // (which may call into render-layer code) doesn't contend with
    // sink registration or trace attach.
    if (cost_enabled) {
        for (const auto& [tid, prov] : tick_activity) {
            cost.note_trace_activity(tid);
            cost.note_provenance(tid, prov);
        }
        cost.emit_frame();
    }
}

// ── Coordinator::publish_internal ─────────────────────────────────────
//
// Defined after on_tick so `components_differ` (anonymous namespace
// above) is in scope.

void Coordinator::publish_internal(std::string view_name,
                                   std::string metric_name,
                                   std::vector<std::pair<std::string, double>> components,
                                   PublishOptions opts) {
    if (components.empty()) return;
    sort_components(components);

    // Same Baseline / Start / Sample / End burst-framing semantics as
    // the sampler-driven coordinator path, just keyed by (view, metric)
    // and only running when firehose is on.
    std::vector<std::pair<Sink, SampleEvent>> pending;
    {
        std::lock_guard<std::mutex> lock(state_->mtx);
        if (!state_->tracing_enabled || state_->sinks.empty()) return;
        if (!state_->firehose) return;

        const double t_now =
            state_->clock ? static_cast<double>(state_->clock->time()) : 0.0;
        const std::uint64_t f_now =
            state_->clock ? state_->clock->frame() : 0;

        const PublishKey key{view_name, metric_name};
        auto& pstate = state_->publish_states[key];
        // Sticky: the first publish for a (view, metric) sets precision
        // and epsilon. Subsequent calls inherit those so callers can
        // omit `PublishOptions` on every tick without changing the
        // per-key threshold.
        if (!pstate.has_baseline) {
            pstate.precision = opts.precision;
            pstate.epsilon = opts.epsilon;
        }

        // Resolve effective provenance. Explicit opts.provenance wins;
        // otherwise fall back to the coordinator's ambient slot. Empty
        // stays empty for callers that do not opt into provenance.
        const Provenance effective_prov =
            opts.provenance.is_set() ? opts.provenance
                                     : state_->ambient_provenance;

        auto enqueue = [&](SampleEvent::Kind kind,
                           std::vector<std::pair<std::string, double>> comps,
                           std::vector<std::pair<std::string, double>> deltas = {}) {
            SampleEvent e;
            e.kind = kind;
            e.view_name = view_name;
            e.metric_name = metric_name;
            e.t_seconds = t_now;
            e.frame = f_now;
            e.precision = pstate.precision;
            // Publish channel uses trace_id = 0 (reserved).
            // metric_id = 0; (view_name, metric_name) already identifies
            // the metric. burst_id increments per (view, metric).
            e.trace_id = 0;
            e.metric_id = 0;
            e.burst_id = pstate.current_burst_id;
            e.provenance = effective_prov;
            e.components = std::move(comps);
            e.deltas = std::move(deltas);
            for (const auto& [sid, sink] : state_->sinks) {
                (void)sid;
                pending.emplace_back(sink, e);
            }
            state_->emitted_count++;
        };

        if (!pstate.has_baseline) {
            enqueue(SampleEvent::Kind::Baseline, components);
            pstate.last_emitted = components;
            pstate.has_baseline = true;
        } else {
            const bool changed = components_differ(components,
                                                   pstate.last_emitted,
                                                   pstate.epsilon);
            if (changed) {
                if (!pstate.in_motion) {
                    pstate.current_burst_id = pstate.next_burst_id++;
                    enqueue(SampleEvent::Kind::Start, {});
                    pstate.motion_start = pstate.last_emitted;
                    pstate.in_motion = true;
                }
                enqueue(SampleEvent::Kind::Sample, components);
                pstate.last_emitted = components;
            } else if (pstate.in_motion) {
                std::vector<std::pair<std::string, double>> deltas;
                deltas.reserve(components.size());
                for (const auto& [name, cur] : components) {
                    double start = 0.0;
                    for (const auto& [sn, sv] : pstate.motion_start) {
                        if (sn == name) { start = sv; break; }
                    }
                    deltas.emplace_back(name, cur - start);
                }
                enqueue(SampleEvent::Kind::End, {}, std::move(deltas));
                pstate.in_motion = false;
                pstate.motion_start.clear();
                pstate.current_burst_id = 0;
            }
        }
    }
    for (auto& [sink, ev] : pending) sink(ev);
}

// ── Coordinator::dispatch_input_event ────────────────────────────────

void Coordinator::dispatch_input_event(SampleEvent e) {
    std::vector<std::pair<Sink, SampleEvent>> pending;
    {
        std::lock_guard<std::mutex> lock(state_->mtx);
        if (state_->sinks.empty()) return;
        // Stamp the bound clock's monotonic time/frame so replay can
        // recover the original cadence. Input events bypass the
        // tracing_enabled gate because recording is itself a separate
        // opt-in (an `InputRecorder` was constructed); requiring both
        // would be a footgun for "record an interaction without
        // sampler-driven traces."
        if (state_->clock) {
            e.t_seconds = static_cast<double>(state_->clock->time());
            e.frame = state_->clock->frame();
        }
        for (const auto& [sid, sink] : state_->sinks) {
            (void)sid;
            pending.emplace_back(sink, e);
        }
        state_->emitted_count++;
    }
    for (auto& [sink, ev] : pending) sink(ev);
}

// ── Fixture record / replay ──────────────────────────────────────────

namespace {

const char* sample_kind_string(SampleEvent::Kind k) {
    switch (k) {
        case SampleEvent::Kind::TraceStarted: return "trace-started";
        case SampleEvent::Kind::Baseline:     return "baseline";
        case SampleEvent::Kind::Sample:       return "sample";
        case SampleEvent::Kind::Start:        return "start";
        case SampleEvent::Kind::End:          return "end";
        case SampleEvent::Kind::Input:        return "input";
    }
    return "?";
}

bool kind_from_string(std::string_view s, SampleEvent::Kind& out) {
    if (s == "trace-started") { out = SampleEvent::Kind::TraceStarted; return true; }
    if (s == "baseline")      { out = SampleEvent::Kind::Baseline;     return true; }
    if (s == "sample")        { out = SampleEvent::Kind::Sample;       return true; }
    if (s == "start")         { out = SampleEvent::Kind::Start;        return true; }
    if (s == "end")           { out = SampleEvent::Kind::End;          return true; }
    if (s == "input")         { out = SampleEvent::Kind::Input;        return true; }
    return false;
}

/// Minimal JSON escape for the strings we control (view, metric,
/// component names). Escapes `"`, `\\`, and control characters with
/// the standard `\\uNNNN` form so the JSONL stays parseable. We don't
/// need full Unicode handling — the strings are ASCII identifiers in
/// practice.
std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char ch : s) {
        unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string format_number(double v) {
    // Non-finite values are emitted as quoted sentinels rather than
    // JSON `null` so the fixture round-trips losslessly — NaN / Inf
    // are exactly the values you most want to capture when an
    // animation misbehaves. The parser below recognizes the same
    // three sentinels and converts them back to the matching IEEE-754
    // value.
    if (std::isnan(v))               return "\"NaN\"";
    if (std::isinf(v) && v > 0)      return "\"Infinity\"";
    if (std::isinf(v) && v < 0)      return "\"-Infinity\"";
    std::ostringstream ss;
    ss << std::setprecision(15) << v;
    return ss.str();
}

std::string serialize_components(
    const std::vector<std::pair<std::string, double>>& comps
) {
    std::string out = "{";
    bool first = true;
    for (const auto& [k, v] : comps) {
        if (!first) out += ",";
        out += "\"" + json_escape(k) + "\":" + format_number(v);
        first = false;
    }
    out += "}";
    return out;
}

std::string serialize_provenance(const Provenance& p) {
    std::string out = "{";
    out += "\"source_kind\":\"" + json_escape(p.source_kind) + "\"";
    out += ",\"source_id\":\"" + json_escape(p.source_id) + "\"";
    out += ",\"source_file\":\"" + json_escape(p.source_file) + "\"";
    out += ",\"source_line\":" + std::to_string(p.source_line);
    out += "}";
    return out;
}

std::string serialize_event(const SampleEvent& e) {
    std::ostringstream ss;
    ss << "{\"kind\":\"" << sample_kind_string(e.kind) << "\""
       << ",\"view\":\"" << json_escape(e.view_name) << "\""
       << ",\"metric\":\"" << json_escape(e.metric_name) << "\""
       << ",\"t\":" << format_number(e.t_seconds)
       << ",\"frame\":" << e.frame
       << ",\"precision\":" << e.precision
       << ",\"trace_id\":" << e.trace_id
       << ",\"metric_id\":" << e.metric_id
       << ",\"burst_id\":" << e.burst_id
       << ",\"components\":" << serialize_components(e.components)
       << ",\"deltas\":" << serialize_components(e.deltas);
    if (e.provenance.is_set()) {
        ss << ",\"provenance\":" << serialize_provenance(e.provenance);
    }
    // Input fields only emit on Input events so ordinary motion lines
    // stay byte-for-byte compatible with older captures.
    if (e.kind == SampleEvent::Kind::Input) {
        ss << ",\"input_kind\":\"" << json_escape(e.input_kind) << "\""
           << ",\"view_id\":\"" << json_escape(e.view_id) << "\"";
    }
    ss << "}";
    return ss.str();
}

/// Hand-rolled JSON object parser limited to the shapes we emit.
/// Returns false on syntax or extension errors.
class FixtureLineParser {
public:
    explicit FixtureLineParser(const std::string& src) : s_(src) {}

    bool parse_event(SampleEvent& out) {
        SampleEvent e;
        if (!expect('{')) return false;
        bool first = true;
        while (true) {
            skip_ws();
            if (peek() == '}') { ++pos_; break; }
            if (!first && !expect(',')) return false;
            first = false;
            std::string key;
            if (!parse_string(key) || !expect(':')) return false;
            skip_ws();

            if (key == "kind") {
                std::string v;
                if (!parse_string(v)) return false;
                if (!kind_from_string(v, e.kind)) return false;
            } else if (key == "view") {
                if (!parse_string(e.view_name)) return false;
            } else if (key == "metric") {
                if (!parse_string(e.metric_name)) return false;
            } else if (key == "t") {
                if (!parse_number(e.t_seconds)) return false;
            } else if (key == "frame") {
                double f = 0; if (!parse_number(f)) return false;
                e.frame = static_cast<std::uint64_t>(f);
            } else if (key == "precision") {
                double p = 0; if (!parse_number(p)) return false;
                e.precision = static_cast<int>(p);
            } else if (key == "components") {
                if (!parse_components(e.components)) return false;
            } else if (key == "deltas") {
                if (!parse_components(e.deltas)) return false;
            } else if (key == "trace_id") {
                double v = 0; if (!parse_number(v)) return false;
                e.trace_id = static_cast<int>(v);
            } else if (key == "metric_id") {
                double v = 0; if (!parse_number(v)) return false;
                e.metric_id = static_cast<int>(v);
            } else if (key == "burst_id") {
                double v = 0; if (!parse_number(v)) return false;
                e.burst_id = static_cast<int>(v);
            } else if (key == "provenance") {
                if (!parse_provenance(e.provenance)) return false;
            } else if (key == "input_kind") {
                if (!parse_string(e.input_kind)) return false;
            } else if (key == "view_id") {
                if (!parse_string(e.view_id)) return false;
            } else {
                // Unknown key — skip its value tolerantly.
                if (!skip_value()) return false;
            }
        }
        out = std::move(e);
        return true;
    }

    bool parse_provenance(Provenance& out) {
        if (!expect('{')) return false;
        skip_ws();
        if (peek() == '}') { ++pos_; return true; }
        while (true) {
            std::string k;
            if (!parse_string(k) || !expect(':')) return false;
            if (k == "source_kind") {
                if (!parse_string(out.source_kind)) return false;
            } else if (k == "source_id") {
                if (!parse_string(out.source_id)) return false;
            } else if (k == "source_file") {
                if (!parse_string(out.source_file)) return false;
            } else if (k == "source_line") {
                double n = 0; if (!parse_number(n)) return false;
                out.source_line = static_cast<int>(n);
            } else {
                if (!skip_value()) return false;
            }
            skip_ws();
            if (peek() == ',') { ++pos_; continue; }
            if (peek() == '}') { ++pos_; return true; }
            return false;
        }
    }

    bool parse_header(int& version_out) {
        FixtureHeader unused;
        return parse_header_full(unused) && (version_out = unused.version, true);
    }

    /// Parses the full fixture header. Accepts any subset / order of
    /// `motion_fixture_version`, `policy`, `duration_scale`. Returns
    /// false only on malformed JSON or missing version. Unknown keys
    /// are skipped tolerantly so future additive fields don't break v2
    /// loaders.
    bool parse_header_full(FixtureHeader& out) {
        if (!expect('{')) return false;
        skip_ws();
        if (peek() == '}') { ++pos_; return false; }
        bool saw_version = false;
        while (true) {
            std::string key;
            if (!parse_string(key) || !expect(':')) return false;
            if (key == "motion_fixture_version") {
                double v = 0;
                if (!parse_number(v)) return false;
                out.version = static_cast<int>(v);
                saw_version = true;
            } else if (key == "policy") {
                if (!parse_string(out.policy)) return false;
            } else if (key == "duration_scale") {
                double v = 0;
                if (!parse_number(v)) return false;
                out.duration_scale = v;
            } else {
                if (!skip_value()) return false;
            }
            skip_ws();
            if (peek() == ',') { ++pos_; continue; }
            if (peek() == '}') { ++pos_; return saw_version; }
            return false;
        }
    }

private:
    const std::string& s_;
    std::size_t pos_ = 0;

    char peek() const { return pos_ < s_.size() ? s_[pos_] : '\0'; }
    void skip_ws() { while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_; }
    bool expect(char c) { skip_ws(); if (peek() != c) return false; ++pos_; return true; }

    bool parse_string(std::string& out) {
        skip_ws();
        if (peek() != '"') return false;
        ++pos_;
        out.clear();
        while (pos_ < s_.size() && s_[pos_] != '"') {
            char c = s_[pos_++];
            if (c == '\\' && pos_ < s_.size()) {
                char esc = s_[pos_++];
                switch (esc) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'u': {
                        if (pos_ + 4 > s_.size()) return false;
                        // We only emit ASCII (< 0x80) in `json_escape`, so any
                        // `\\uNNNN` in our own fixtures will be < 0x80.
                        unsigned int code = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = s_[pos_++];
                            code <<= 4;
                            if (h >= '0' && h <= '9') code |= (h - '0');
                            else if (h >= 'a' && h <= 'f') code |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') code |= (h - 'A' + 10);
                            else return false;
                        }
                        if (code >= 0x80) return false;
                        out += static_cast<char>(code);
                        break;
                    }
                    default: return false;
                }
            } else {
                out += c;
            }
        }
        if (peek() != '"') return false;
        ++pos_;
        return true;
    }

    bool parse_number(double& out) {
        skip_ws();
        std::size_t start = pos_;
        if (peek() == '-' || peek() == '+') ++pos_;
        bool any_digit = false;
        while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) { ++pos_; any_digit = true; }
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) { ++pos_; any_digit = true; }
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
            while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) { ++pos_; any_digit = true; }
        }
        if (!any_digit) return false;
        try {
            out = std::stod(s_.substr(start, pos_ - start));
        } catch (...) {
            return false;
        }
        return true;
    }

    /// Accepts a numeric literal OR one of the quoted sentinels
    /// `"NaN"`, `"Infinity"`, `"-Infinity"`. `format_number` writes
    /// non-finite values as those sentinels so the round-trip
    /// preserves NaN/Inf — typically the most diagnostic samples in a
    /// misbehaving animation.
    bool parse_number_or_sentinel(double& out) {
        skip_ws();
        if (peek() == '"') {
            std::string s;
            if (!parse_string(s)) return false;
            if (s == "NaN")        { out = std::numeric_limits<double>::quiet_NaN(); return true; }
            if (s == "Infinity")   { out = std::numeric_limits<double>::infinity();  return true; }
            if (s == "-Infinity")  { out = -std::numeric_limits<double>::infinity(); return true; }
            return false;
        }
        return parse_number(out);
    }

    bool parse_components(std::vector<std::pair<std::string, double>>& out) {
        out.clear();
        if (!expect('{')) return false;
        skip_ws();
        if (peek() == '}') { ++pos_; return true; }
        while (true) {
            std::string k;
            if (!parse_string(k) || !expect(':')) return false;
            double v;
            if (!parse_number_or_sentinel(v)) return false;
            out.emplace_back(std::move(k), v);
            skip_ws();
            if (peek() == ',') { ++pos_; continue; }
            if (peek() == '}') { ++pos_; return true; }
            return false;
        }
    }

    bool skip_value() {
        skip_ws();
        char c = peek();
        if (c == '"') { std::string dummy; return parse_string(dummy); }
        if (c == '{') {
            ++pos_; int depth = 1;
            while (pos_ < s_.size() && depth > 0) {
                if (s_[pos_] == '{') ++depth;
                else if (s_[pos_] == '}') --depth;
                ++pos_;
            }
            return depth == 0;
        }
        if (c == '[') {
            ++pos_; int depth = 1;
            while (pos_ < s_.size() && depth > 0) {
                if (s_[pos_] == '[') ++depth;
                else if (s_[pos_] == ']') --depth;
                ++pos_;
            }
            return depth == 0;
        }
        double n;
        return parse_number(n);
    }
};

/// Shared file handle owned by the sink lambda. Lazily opens on first
/// event so empty traces don't create stale files. Header is written
/// once; subsequent events are appended.
///
/// `mtx` serializes the open / header-write / body-write sequence.
/// Coordinator::fire_sinks does not hold a lock across sink calls, and
/// the same `make_fixture_sink(path)` lambda is sometimes registered
/// on both `Coordinator::add_sink` AND `MotionScrubber::add_sink` (see
/// docstring on `make_fixture_sink` callers); two sink-fire threads
/// could otherwise interleave writes mid-line. The mutex is cheap and
/// uncontended in the single-registration case (issue #2151).
struct FixtureFileSink {
    std::string path;
    std::shared_ptr<std::ofstream> stream;
    bool header_written = false;
    std::mutex mtx;

    void ensure_open() {
        if (!stream) {
            stream = std::make_shared<std::ofstream>(path,
                std::ios::out | std::ios::trunc | std::ios::binary);
        }
    }
};

}  // namespace

Sink make_fixture_sink(std::string path) {
    auto state = std::make_shared<FixtureFileSink>();
    state->path = std::move(path);
    return [state](const SampleEvent& e) {
        std::lock_guard<std::mutex> lock(state->mtx);
        state->ensure_open();
        if (!state->stream || !state->stream->is_open()) return;
        if (!state->header_written) {
            // Snapshot the MotionPolicy + duration_scale in effect at
            // the recording's first event. Goldens recorded under
            // Reduced will not silently compare against Full captures.
            const auto policy_str = motion_policy_to_string(
                MotionPreferences::current());
            const double scale = MotionPreferences::current_duration_scale();
            *state->stream << "{\"motion_fixture_version\":"
                           << kFixtureSchemaVersion
                           << ",\"policy\":\"" << policy_str << "\""
                           << ",\"duration_scale\":" << scale
                           << "}\n";
            state->header_written = true;
        }
        *state->stream << serialize_event(e) << "\n";
        state->stream->flush();
    };
}

std::vector<SampleEvent> load_fixture(const std::string& path) {
    std::vector<SampleEvent> out;
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in.is_open()) return out;
    std::string line;
    if (!std::getline(in, line)) return out;
    FixtureLineParser header(line);
    FixtureHeader hdr;
    if (!header.parse_header_full(hdr)) return out;
    if (hdr.version != kFixtureSchemaVersion) return out;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        SampleEvent e;
        FixtureLineParser p(line);
        if (!p.parse_event(e)) return {};
        out.push_back(std::move(e));
    }
    return out;
}

FixtureHeader load_fixture_header(const std::string& path) {
    FixtureHeader hdr;
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in.is_open()) return hdr;
    std::string line;
    if (!std::getline(in, line)) return hdr;
    FixtureLineParser p(line);
    FixtureHeader parsed;
    if (!p.parse_header_full(parsed)) return hdr;
    return parsed;
}

int replay_fixture(const std::string& path, const Sink& sink) {
    auto events = load_fixture(path);
    if (events.empty()) return -1;
    for (const auto& e : events) sink(e);
    return static_cast<int>(events.size());
}

// ── assert_matches ──────────────────────────────────────────────────

namespace {

/// Burst identity used to align events across two fixtures. For events
/// without burst_id (TraceStarted, Baseline) we fall back to
/// `(view, metric, kind, trace_id, metric_id)`.
struct EventKey {
    std::string view;
    std::string metric;
    SampleEvent::Kind kind;
    int trace_id = 0;
    int metric_id = 0;
    int burst_id = 0;

    bool operator<(const EventKey& o) const {
        if (view != o.view) return view < o.view;
        if (metric != o.metric) return metric < o.metric;
        if (kind != o.kind) return kind < o.kind;
        if (trace_id != o.trace_id) return trace_id < o.trace_id;
        if (metric_id != o.metric_id) return metric_id < o.metric_id;
        return burst_id < o.burst_id;
    }
};

EventKey key_of(const SampleEvent& e) {
    return { e.view_name, e.metric_name, e.kind,
             e.trace_id, e.metric_id, e.burst_id };
}

}  // namespace

FixtureDiff assert_matches(const std::vector<SampleEvent>& golden,
                           const std::vector<SampleEvent>& captured,
                           FixtureMatchOptions opts) {
    FixtureDiff diff;

    if (opts.require_same_event_count && golden.size() != captured.size()) {
        FixtureDiff::Item item;
        item.kind = "event-count-mismatch";
        item.detail = "golden=" + std::to_string(golden.size()) +
                      " captured=" + std::to_string(captured.size());
        item.expected = static_cast<double>(golden.size());
        item.observed = static_cast<double>(captured.size());
        diff.differences.push_back(item);
    }

    // ID-keyed grouping: for each (view, metric, kind, trace_id,
    // metric_id, burst_id) group, compare in arrival order within the
    // group. Bursts that appear in different order across fixtures still
    // match if their identity matches.
    std::map<EventKey, std::vector<const SampleEvent*>> g_groups;
    std::map<EventKey, std::vector<const SampleEvent*>> c_groups;
    for (const auto& e : golden)   g_groups[key_of(e)].push_back(&e);
    for (const auto& e : captured) c_groups[key_of(e)].push_back(&e);

    const auto compare_components = [&](const SampleEvent& g,
                                        const SampleEvent& c,
                                        const auto& gc, const auto& cc,
                                        const char* tag) {
        if (gc.size() != cc.size()) {
            FixtureDiff::Item item;
            item.kind = "component-count-mismatch";
            item.view_name = g.view_name;
            item.metric_name = g.metric_name;
            item.detail = tag;
            item.observed = static_cast<double>(cc.size());
            item.expected = static_cast<double>(gc.size());
            diff.differences.push_back(item);
            return;
        }
        for (std::size_t k = 0; k < gc.size(); ++k) {
            if (gc[k].first != cc[k].first) {
                FixtureDiff::Item item;
                item.kind = "component-mismatch";
                item.view_name = g.view_name;
                item.metric_name = g.metric_name;
                item.component_name = gc[k].first;
                item.detail = std::string(tag) + ", expected name " +
                              gc[k].first + " got " + cc[k].first;
                diff.differences.push_back(item);
                continue;
            }
            if (std::fabs(gc[k].second - cc[k].second) > opts.component_epsilon) {
                FixtureDiff::Item item;
                item.kind = "component-drift";
                item.view_name = g.view_name;
                item.metric_name = g.metric_name;
                item.component_name = gc[k].first;
                item.detail = tag;
                item.observed = cc[k].second;
                item.expected = gc[k].second;
                diff.differences.push_back(item);
            }
        }
    };

    // Walk the golden groups; for each, pair up with the captured
    // group's events in arrival order.
    for (const auto& [key, g_list] : g_groups) {
        auto it = c_groups.find(key);
        if (it == c_groups.end()) {
            FixtureDiff::Item item;
            item.kind = "missing-event";
            item.view_name = key.view;
            item.metric_name = key.metric;
            item.detail = "burst_id=" + std::to_string(key.burst_id);
            diff.differences.push_back(item);
            continue;
        }
        const auto& c_list = it->second;
        const std::size_t n = std::min(g_list.size(), c_list.size());
        if (g_list.size() != c_list.size()) {
            FixtureDiff::Item item;
            item.kind = "group-count-mismatch";
            item.view_name = key.view;
            item.metric_name = key.metric;
            item.detail = "burst_id=" + std::to_string(key.burst_id);
            item.observed = static_cast<double>(c_list.size());
            item.expected = static_cast<double>(g_list.size());
            diff.differences.push_back(item);
        }
        for (std::size_t i = 0; i < n; ++i) {
            const auto& g = *g_list[i];
            const auto& c = *c_list[i];
            if (std::fabs(g.t_seconds - c.t_seconds) > opts.timing_epsilon_seconds) {
                FixtureDiff::Item item;
                item.kind = "timing-drift";
                item.view_name = g.view_name;
                item.metric_name = g.metric_name;
                item.detail = "burst_id=" + std::to_string(g.burst_id);
                item.observed = c.t_seconds;
                item.expected = g.t_seconds;
                diff.differences.push_back(item);
            }
            compare_components(g, c, g.components, c.components, "components");
            compare_components(g, c, g.deltas, c.deltas, "deltas");
        }
    }

    // Extra groups in captured that aren't in golden.
    for (const auto& [key, _] : c_groups) {
        if (g_groups.find(key) == g_groups.end()) {
            FixtureDiff::Item item;
            item.kind = "extra-event";
            item.view_name = key.view;
            item.metric_name = key.metric;
            item.detail = "burst_id=" + std::to_string(key.burst_id);
            diff.differences.push_back(item);
        }
    }

    return diff;
}

FixtureDiff assert_matches(const FixtureHeader& golden_header,
                           const std::vector<SampleEvent>& golden,
                           const FixtureHeader& captured_header,
                           const std::vector<SampleEvent>& captured,
                           FixtureMatchOptions opts) {
    FixtureDiff diff = assert_matches(golden, captured, opts);
    if (opts.require_matching_policy) {
        if (golden_header.policy != captured_header.policy) {
            FixtureDiff::Item item;
            item.kind = "policy-mismatch";
            item.detail = "policy: golden=" + golden_header.policy +
                          " captured=" + captured_header.policy;
            diff.differences.push_back(item);
        }
        if (std::fabs(golden_header.duration_scale -
                      captured_header.duration_scale) >
            opts.duration_scale_epsilon) {
            FixtureDiff::Item item;
            item.kind = "policy-mismatch";
            item.detail = "duration_scale mismatch";
            item.expected = golden_header.duration_scale;
            item.observed = captured_header.duration_scale;
            diff.differences.push_back(item);
        }
    }
    return diff;
}

// ── Assertion helpers ─────────────────────────────────────────────────

std::vector<ScalarSample> extract_scalar(
    const std::vector<SampleEvent>& events,
    std::string_view view_name,
    std::string_view metric_name,
    std::string_view component_name
) {
    std::vector<ScalarSample> out;
    for (const auto& e : events) {
        if (e.view_name != view_name || e.metric_name != metric_name) continue;
        // TraceStarted / Start / End are framing events with no
        // sample components — skip them and pull only Baseline +
        // Sample data.
        if (e.kind != SampleEvent::Kind::Baseline &&
            e.kind != SampleEvent::Kind::Sample) continue;
        for (const auto& [k, v] : e.components) {
            if (k == component_name) {
                out.push_back({e.t_seconds, v});
                break;
            }
        }
    }
    return out;
}

bool is_monotonic(const std::vector<ScalarSample>& samples, double epsilon) {
    int direction = 0;
    double prev = std::numeric_limits<double>::quiet_NaN();
    for (const auto& s : samples) {
        if (std::isnan(prev)) { prev = s.value; continue; }
        const double dv = s.value - prev;
        if (std::fabs(dv) <= epsilon) { prev = s.value; continue; }
        const int dir = dv > 0 ? 1 : -1;
        if (direction == 0) direction = dir;
        else if (direction != dir) return false;
        prev = s.value;
    }
    return true;
}

double settling_time_seconds(const std::vector<ScalarSample>& samples,
                             double epsilon) {
    double first_t = std::numeric_limits<double>::quiet_NaN();
    double last_t = std::numeric_limits<double>::quiet_NaN();
    double prev = std::numeric_limits<double>::quiet_NaN();
    for (const auto& s : samples) {
        if (std::isnan(prev)) { prev = s.value; continue; }
        if (std::fabs(s.value - prev) > epsilon) {
            if (std::isnan(first_t)) first_t = s.t;
            last_t = s.t;
            prev = s.value;
        }
    }
    if (std::isnan(first_t) || std::isnan(last_t)) return 0.0;
    return last_t - first_t;
}

double overshoot(const std::vector<ScalarSample>& samples, double epsilon) {
    if (samples.size() < 2) return 0.0;
    const double start = samples.front().value;
    const double end = samples.back().value;
    const double range = end - start;
    if (std::fabs(range) <= epsilon) return 0.0;
    double peak = end;
    for (const auto& s : samples) {
        if (range > 0 && s.value > peak) peak = s.value;
        if (range < 0 && s.value < peak) peak = s.value;
    }
    const double excursion = peak - end;
    return std::fabs(excursion) / std::fabs(range);
}

double start_delay_seconds(const std::vector<ScalarSample>& samples,
                           double epsilon) {
    if (samples.empty()) return 0.0;
    const double t0 = samples.front().t;
    const double v0 = samples.front().value;
    for (const auto& s : samples) {
        if (std::fabs(s.value - v0) > epsilon) return s.t - t0;
    }
    return 0.0;
}

double frame_jitter_seconds(const std::vector<ScalarSample>& samples) {
    if (samples.size() < 3) return 0.0;
    std::vector<double> deltas;
    deltas.reserve(samples.size() - 1);
    for (std::size_t i = 1; i < samples.size(); ++i) {
        deltas.push_back(samples[i].t - samples[i - 1].t);
    }
    double mean = 0.0;
    for (double d : deltas) mean += d;
    mean /= static_cast<double>(deltas.size());
    double var = 0.0;
    for (double d : deltas) var += (d - mean) * (d - mean);
    var /= static_cast<double>(deltas.size());
    return std::sqrt(var);
}

double final_value(const std::vector<ScalarSample>& samples) {
    if (samples.empty()) return std::numeric_limits<double>::quiet_NaN();
    return samples.back().value;
}

double local_step_outlier_ratio(const std::vector<ScalarSample>& samples,
                                std::size_t window_radius,
                                double epsilon) {
    // Spec: returns 0.0 when samples.size() < 2*window_radius + 1.
    // With samples.size() == 2r + 1 we have 2r steps total and a single
    // valid sample index (r); the local window has the other 2r - 1
    // steps as neighbors. Larger sample counts grow the candidate range.
    if (window_radius == 0) return 0.0;
    if (samples.size() < 2 * window_radius + 1) return 0.0;

    // Pre-compute step magnitudes between consecutive samples. Step
    // ending at sample index `i` (i >= 1) sits at `steps[i-1]`.
    const std::size_t n = samples.size();
    std::vector<double> steps;
    steps.reserve(n - 1);
    for (std::size_t i = 1; i < n; ++i) {
        steps.push_back(std::fabs(samples[i].value - samples[i - 1].value));
    }

    const std::size_t r = window_radius;
    double max_ratio = 0.0;
    // Candidate sample indices `i` in [r, n - r) — half-open per spec.
    for (std::size_t i = r; i + r < n; ++i) {
        // Window of `2r + 1` sample indices [i - r, i + r]; for each,
        // pick its incoming step (steps[k - 1]) when k >= 1. Skip the
        // candidate's own step so the median excludes it.
        std::vector<double> window;
        window.reserve(2 * r);
        for (std::size_t k = i - r; k <= i + r; ++k) {
            if (k == i) continue;          // exclude candidate
            if (k == 0) continue;          // no incoming step for the first sample
            window.push_back(steps[k - 1]);
        }
        if (window.empty()) continue;

        // Median via nth_element copy. For even counts, average the
        // middle two for the same reason `frame_jitter_seconds` uses
        // an averaging formulation — keeps the metric stable when an
        // outlier sits exactly at the median index.
        std::vector<double> tmp = window;
        const std::size_t sz = tmp.size();
        const std::size_t mid = sz / 2;
        std::nth_element(tmp.begin(), tmp.begin() + mid, tmp.end());
        double median = tmp[mid];
        if ((sz & 1u) == 0) {
            const double upper = tmp[mid];
            std::nth_element(tmp.begin(), tmp.begin() + (mid - 1), tmp.end());
            median = 0.5 * (tmp[mid - 1] + upper);
        }
        const double denom = std::max(median, epsilon);
        const double ratio = steps[i - 1] / denom;
        if (ratio > max_ratio) max_ratio = ratio;
    }
    return max_ratio;
}

// ── Input recording / replay ─────────────────────────────────────────
//
// Recording: View::simulate_* calls `record_simulated_input` which —
// when at least one InputRecorder is alive — builds an `Input`
// SampleEvent and dispatches it to every installed sink. The
// FrameClock-relative timestamp comes from the Coordinator's bound
// clock so a replay can recover the original cadence.
//
// Replay: `replay_inputs` walks the fixture, resolves each Input's
// `view_id` against `root_view` via DFS, advances `frame_clock` to
// match the recorded `t_seconds`, and calls the matching
// `View::simulate_*`. Sinks installed on the Coordinator (typically the
// same `make_fixture_sink` paired with the recorder) re-capture the
// motion stream the replayed inputs produce.

namespace {

/// Process-wide recording-active counter. Bumped by `make_input_recorder`,
/// dropped by `InputRecorder::stop()` / destructor. `View::simulate_*`
/// gates on this so the non-recording cost is a single relaxed load.
std::atomic<int> g_recorder_count{0};

}  // namespace

bool input_recording_enabled() noexcept {
    return g_recorder_count.load(std::memory_order_relaxed) > 0;
}

void record_simulated_input(const std::string& input_kind,
                            const std::string& view_id,
                            std::vector<std::pair<std::string, double>> coords) {
    // Snapshot a SampleEvent::Kind::Input under the recorder mutex
    // and let Coordinator's sinks pick it up. The Coordinator itself
    // already serializes sink iteration internally on its own mutex
    // via the publish path; we bypass it here because Input events
    // don't participate in the Baseline/Start/Sample/End burst state
    // machine — they're a flat event stream tagged into the same
    // fixture for chronological replay.
    auto& coord = Coordinator::instance();
    if (!input_recording_enabled()) return;

    SampleEvent e;
    e.kind = SampleEvent::Kind::Input;
    e.view_name = "input";   // sentinel so format_line groups under [input]
    e.metric_name = input_kind;
    e.input_kind = input_kind;
    e.view_id = view_id;
    e.components = std::move(coords);
    // Coordinator owns the FrameClock binding; reach in via its
    // public sink set and let the dispatch loop stamp `t`/`frame`
    // ourselves. We keep the lookup simple — if the coordinator
    // isn't bound, the event still serializes with t=0 / frame=0
    // and replay falls back to elapsed-clock advance.
    e.t_seconds = 0.0;
    e.frame = 0;
    // The dispatcher below mirrors Coordinator's pending-then-fire
    // pattern so a sink that calls back into motion (e.g. write-then-
    // log) doesn't deadlock.
    coord.dispatch_input_event(e);
}

// ── InputRecorder ────────────────────────────────────────────────────

InputRecorder::InputRecorder(InputRecorder&& o) noexcept : sink_id_(o.sink_id_) {
    o.sink_id_ = 0;
}

InputRecorder& InputRecorder::operator=(InputRecorder&& o) noexcept {
    if (this != &o) {
        stop();
        sink_id_ = o.sink_id_;
        o.sink_id_ = 0;
    }
    return *this;
}

InputRecorder::~InputRecorder() { stop(); }

void InputRecorder::stop() {
    if (sink_id_ == 0) return;
    Coordinator::instance().remove_sink(sink_id_);
    sink_id_ = 0;
    // Drop the recorder count last so any in-flight record_simulated_input
    // either fully publishes (if it loaded the count before our store) or
    // becomes a clean no-op (if it loaded after). Either is correct — the
    // sink was already removed.
    g_recorder_count.fetch_sub(1, std::memory_order_relaxed);
}

InputRecorder make_input_recorder(std::string path) {
    // Bump the recorder-count first so any simulate_* dispatch racing
    // against our sink install becomes a fully published event rather
    // than a silently-dropped one. The sink id is the recorder's
    // ownership token; we hand it back wrapped in the RAII handle.
    g_recorder_count.fetch_add(1, std::memory_order_relaxed);
    const int sink_id =
        Coordinator::instance().add_sink(make_fixture_sink(std::move(path)));
    return InputRecorder(sink_id);
}

// ── replay_inputs ────────────────────────────────────────────────────
//
// Walks a fixture, locates each Input's target by `view_id` (DFS from
// `root_view`), advances `frame_clock` to the recorded timestamp, and
// re-invokes the matching `View::simulate_*`. Sinks installed on the
// Coordinator (typically the same `make_fixture_sink` paired with the
// recorder) re-capture the motion stream the replayed inputs produce,
// so a captured fixture replayed against a fresh view tree yields a
// byte-equivalent motion fixture.

namespace {

const View* find_by_id(const View& root, const std::string& id) {
    if (root.id() == id) return &root;
    for (std::size_t i = 0; i < root.child_count(); ++i) {
        if (const View* found = find_by_id(*root.child_at(i), id)) {
            return found;
        }
    }
    return nullptr;
}

double component_value(const std::vector<std::pair<std::string, double>>& comps,
                       const std::string& name) {
    for (const auto& [k, v] : comps) {
        if (k == name) return v;
    }
    return 0.0;
}

}  // namespace

int replay_inputs(const std::string& path, View& root_view, FrameClock& clock) {
    auto events = load_fixture(path);
    if (events.empty()) return -1;

    int replayed = 0;
    double last_t = 0.0;
    bool have_last_t = false;
    for (const auto& e : events) {
        if (e.kind != SampleEvent::Kind::Input) continue;

        // Advance the clock to the recorded timestamp. The first input
        // anchors `last_t` so a delayed first interaction is preserved.
        if (have_last_t) {
            const double dt = e.t_seconds - last_t;
            if (dt > 0.0) clock.tick(static_cast<float>(dt));
        } else {
            // Anchor on the recorded `t` so geometry traces that
            // sample on the same FrameClock observe a consistent
            // start offset.
            if (e.t_seconds > 0.0) clock.tick(static_cast<float>(e.t_seconds));
            have_last_t = true;
        }
        last_t = e.t_seconds;

        // Coordinates in the fixture are root-space (matching the
        // original `View::simulate_*` contract), so dispatch through
        // `root_view`: its `hit_test` walks the tree and lands on the
        // same descendant the recorder captured. `view_id` is a
        // recorded provenance signal — useful for diffing across
        // replay runs — not the actual dispatch receiver, because
        // calling `simulate_click` on a leaf view with root-space
        // coords would always miss its local hit_test.
        //
        // We still resolve `view_id` so the lookup itself is
        // exercised, and (when the id has moved or been renamed) the
        // diagnostic surface here can be extended without changing
        // the public contract.
        if (!e.view_id.empty()) {
            (void)find_by_id(root_view, e.view_id);
        }
        View* target = &root_view;

        if (e.input_kind == "click") {
            const Point p{ static_cast<float>(component_value(e.components, "x")),
                           static_cast<float>(component_value(e.components, "y")) };
            target->simulate_click(p);
            ++replayed;
        } else if (e.input_kind == "hover") {
            const Point p{ static_cast<float>(component_value(e.components, "x")),
                           static_cast<float>(component_value(e.components, "y")) };
            target->simulate_hover(p);
            ++replayed;
        } else if (e.input_kind == "drag") {
            const Point s{ static_cast<float>(component_value(e.components, "start_x")),
                           static_cast<float>(component_value(e.components, "start_y")) };
            const Point en{ static_cast<float>(component_value(e.components, "end_x")),
                            static_cast<float>(component_value(e.components, "end_y")) };
            const int steps = std::max(
                1, static_cast<int>(component_value(e.components, "steps")));
            target->simulate_drag(s, en, steps);
            ++replayed;
        }
    }
    return replayed;
}

}  // namespace pulp::view::motion
