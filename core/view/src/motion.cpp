#include <pulp/view/motion.hpp>

#include <pulp/runtime/log.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/view.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>

namespace pulp::view::motion {

// ── Geometry walker (Layout source only in Phase 0) ──────────────────

namespace {

Rect layout_rect_in_global(const pulp::view::View& v) {
    Rect r = v.bounds();
    const pulp::view::View* p = v.parent();
    while (p) {
        const Rect pb = p->bounds();
        r.x += pb.x;
        r.y += pb.y;
        p = p->parent();
    }
    return r;
}

Rect resolve_geometry(pulp::view::View& v,
                      GeometrySpace space,
                      GeometrySource source) {
    // Phase 0: Presentation falls back to Layout. Phase 2 ships the full
    // paint_all()-order walker (transform-origin, transform_matrix,
    // scroll, clip).
    (void)source;
    switch (space) {
        case GeometrySpace::ViewLocal:
            return { 0.0f, 0.0f, v.bounds().width, v.bounds().height };
        case GeometrySpace::ViewGlobal:
        case GeometrySpace::Window:
        case GeometrySpace::Screen:
            // Phase 0: Window/Screen collapse onto ViewGlobal. Phase 2
            // adds window-origin and screen-origin offsets.
            return layout_rect_in_global(v);
    }
    return {};
}

double extract_property(const Rect& r, GeometryProperty prop) {
    switch (prop) {
        case GeometryProperty::MinX:   return r.x;
        case GeometryProperty::MinY:   return r.y;
        case GeometryProperty::MaxX:   return r.x + r.width;
        case GeometryProperty::MaxY:   return r.y + r.height;
        case GeometryProperty::MidX:   return r.x + r.width * 0.5;
        case GeometryProperty::MidY:   return r.y + r.height * 0.5;
        case GeometryProperty::Width:  return r.width;
        case GeometryProperty::Height: return r.height;
    }
    return 0.0;
}

const char* property_name(GeometryProperty prop) {
    switch (prop) {
        case GeometryProperty::MinX:   return "minX";
        case GeometryProperty::MinY:   return "minY";
        case GeometryProperty::MaxX:   return "maxX";
        case GeometryProperty::MaxY:   return "maxY";
        case GeometryProperty::MidX:   return "midX";
        case GeometryProperty::MidY:   return "midY";
        case GeometryProperty::Width:  return "width";
        case GeometryProperty::Height: return "height";
    }
    return "?";
}

std::string fmt_double(double v, int precision) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(std::max(0, precision)) << v;
    return ss.str();
}

}  // namespace

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

struct TraceBuilder::Spec {
    std::string view_name;
    TraceOptions opts;
    std::vector<std::unique_ptr<MetricBase>> metrics;
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
            ss << " -- Start frame=" << e.frame
               << " t=" << fmt_double(e.t_seconds, 6) << " --";
            break;
        case SampleEvent::Kind::End:
            ss << " -- End frame=" << e.frame
               << " t=" << fmt_double(e.t_seconds, 6) << " --";
            for (const auto& [k, v] : e.deltas) {
                ss << " " << k << "Delta=" << fmt_double(v, e.precision);
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

// ── Coordinator internals ─────────────────────────────────────────────

struct PerMetricState {
    int precision = 3;
    double epsilon = 0.0001;
    std::vector<std::pair<std::string, double>> current;
    std::vector<std::pair<std::string, double>> last_emitted;
    std::vector<std::pair<std::string, double>> motion_start;
    bool has_baseline = false;
    bool in_motion = false;
};

struct ActiveTrace {
    int id = 0;
    std::shared_ptr<TraceBuilder::Spec> spec;
    std::vector<PerMetricState> metrics;
    double accum_seconds = 0.0;
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
    state_->tracing_enabled = false;
    state_->firehose = false;
    state_->emitted_count = 0;
    state_->next_sink_id = 1;
    state_->next_trace_id = 1;
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
        if (std::fabs(a[i].second - b[i].second) > epsilon) return true;
    }
    return false;
}

void sort_components(std::vector<std::pair<std::string, double>>& v) {
    std::sort(v.begin(), v.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
}

}  // namespace

void Coordinator::on_tick(float dt) {
    // Collect events under lock, dispatch outside the lock.
    std::vector<std::pair<Sink, SampleEvent>> pending;
    {
        std::lock_guard<std::mutex> lock(state_->mtx);
        if (!state_->tracing_enabled || state_->sinks.empty()) return;

        const double t_now =
            state_->clock ? static_cast<double>(state_->clock->time()) : 0.0;
        const std::uint64_t f_now =
            state_->clock ? state_->clock->frame() : 0;

        for (auto& [trace_id, trace] : state_->traces) {
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
                    e.components = std::move(comps);
                    e.deltas = std::move(deltas);
                    for (const auto& [sid, sink] : state_->sinks) {
                        (void)sid;
                        pending.emplace_back(sink, e);
                    }
                    state_->emitted_count++;
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
                }
            }
        }
    }
    for (auto& [sink, ev] : pending) sink(ev);
}

}  // namespace pulp::view::motion
