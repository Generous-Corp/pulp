#include <pulp/view/motion.hpp>

#include <pulp/runtime/log.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

namespace pulp::view::motion {

// ── Geometry walker — Layout + Presentation (Phase 2) ────────────────
//
// Layout source: walks parent `bounds().{x,y}` translations and ignores
// paint-time transforms (transforms are paint-only per view.hpp).
//
// Presentation source: composes the full `paint_all()`-order affine
// chain (bounds translate + transform-origin pivot + translate / rotate
// / scale around it + the explicit 2D affine matrix) plus ScrollView's
// child-offset translate for any ancestor scroll container. The chain
// is built root-down, then the leaf's local-bounds corners are mapped
// through it and the axis-aligned bounding box is reported.

namespace {

/// Column-major 2D affine. Maps (x, y) → (a·x + c·y + tx, b·x + d·y + ty).
struct Mat2D {
    float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f, tx = 0.0f, ty = 0.0f;

    static constexpr Mat2D identity() { return {}; }

    /// Right-multiply: result = lhs * rhs. Mirrors `canvas.concat_*` ops
    /// which post-multiply onto the current matrix.
    static Mat2D multiply(const Mat2D& l, const Mat2D& r) {
        return {
            l.a * r.a + l.c * r.b,
            l.b * r.a + l.d * r.b,
            l.a * r.c + l.c * r.d,
            l.b * r.c + l.d * r.d,
            l.a * r.tx + l.c * r.ty + l.tx,
            l.b * r.tx + l.d * r.ty + l.ty,
        };
    }

    static Mat2D translation(float x, float y) {
        Mat2D m; m.tx = x; m.ty = y; return m;
    }
    static Mat2D scale_uniform(float s) {
        Mat2D m; m.a = s; m.d = s; return m;
    }
    static Mat2D rotation_rad(float r) {
        const float c = std::cos(r), s = std::sin(r);
        Mat2D m;
        m.a = c; m.b = s;
        m.c = -s; m.d = c;
        return m;
    }
    /// Raw affine in (a,b,c,d,e,f) form matching canvas::concat_transform.
    static Mat2D affine(float a, float b, float c, float d, float e, float f) {
        Mat2D m; m.a = a; m.b = b; m.c = c; m.d = d; m.tx = e; m.ty = f;
        return m;
    }

    /// Apply the matrix to a 2D point.
    void apply(float x, float y, float& out_x, float& out_y) const {
        out_x = a * x + c * y + tx;
        out_y = b * x + d * y + ty;
    }
};

/// Compose this view's own paint-time transforms (translate + rotate +
/// scale around transform-origin, then the full 2D matrix around the
/// same origin when set explicitly). Excludes the leading
/// `translate(bounds.x, bounds.y)` — callers compose that separately.
Mat2D self_transform(const pulp::view::View& v) {
    Mat2D m = Mat2D::identity();
    const float w = v.bounds().width;
    const float h = v.bounds().height;
    const float ox = w * v.transform_origin_x();
    const float oy = h * v.transform_origin_y();

    const bool has_basic =
        v.scale() != 1.0f || v.rotation() != 0.0f ||
        v.translate_x() != 0.0f || v.translate_y() != 0.0f;
    if (has_basic) {
        m = Mat2D::multiply(m, Mat2D::translation(ox, oy));
        if (v.translate_x() != 0.0f || v.translate_y() != 0.0f) {
            m = Mat2D::multiply(m, Mat2D::translation(v.translate_x(),
                                                      v.translate_y()));
        }
        if (v.rotation() != 0.0f) {
            constexpr float kPi = 3.14159265358979323846f;
            m = Mat2D::multiply(m, Mat2D::rotation_rad(v.rotation() * kPi / 180.0f));
        }
        if (v.scale() != 1.0f) {
            m = Mat2D::multiply(m, Mat2D::scale_uniform(v.scale()));
        }
        m = Mat2D::multiply(m, Mat2D::translation(-ox, -oy));
    }

    if (v.has_transform_matrix()) {
        float a, b, c, d, e, f;
        v.get_transform_matrix(a, b, c, d, e, f);
        const bool apply_origin = v.transform_origin_explicit();
        if (apply_origin) m = Mat2D::multiply(m, Mat2D::translation(ox, oy));
        m = Mat2D::multiply(m, Mat2D::affine(a, b, c, d, e, f));
        if (apply_origin) m = Mat2D::multiply(m, Mat2D::translation(-ox, -oy));
    }

    return m;
}

/// Returns the parent-level paint contribution that the child sees on
/// the canvas matrix when `parent` calls `paint_all` and then paints a
/// child via `child->paint_all(canvas)`. For plain `View`, this is
/// `translate(bounds.x, bounds.y) * self_transform`. For `ScrollView`,
/// the override skips parent transforms and substitutes a scroll
/// translate, so the contribution is
/// `translate(bounds.x - scroll_x, bounds.y - scroll_y)`.
Mat2D parent_to_child_origin(const pulp::view::View& parent) {
    const auto& pb = parent.bounds();
    if (auto* sv = dynamic_cast<const pulp::view::ScrollView*>(&parent)) {
        return Mat2D::translation(pb.x - sv->scroll_x(), pb.y - sv->scroll_y());
    }
    Mat2D base = Mat2D::translation(pb.x, pb.y);
    return Mat2D::multiply(base, self_transform(parent));
}

/// Build the matrix that maps `v`'s local coords into the global frame
/// (root-relative). Walks root-down so multiplication order matches the
/// canvas: M = T(root.bounds) * X(root) * T(c1.bounds) * X(c1) * …
Mat2D local_to_global_matrix(const pulp::view::View& v,
                             bool include_self_transform) {
    std::vector<const pulp::view::View*> chain;
    for (const pulp::view::View* p = &v; p; p = p->parent()) {
        chain.push_back(p);
    }
    std::reverse(chain.begin(), chain.end());

    Mat2D m = Mat2D::identity();
    for (std::size_t i = 0; i < chain.size(); ++i) {
        const pulp::view::View* node = chain[i];
        const bool is_leaf = (i + 1 == chain.size());
        if (!is_leaf) {
            m = Mat2D::multiply(m, parent_to_child_origin(*node));
        } else {
            // Leaf: always apply its own bounds translate so we land
            // on its top-left in parent's frame, then optionally its
            // self transform.
            m = Mat2D::multiply(m, Mat2D::translation(node->bounds().x,
                                                       node->bounds().y));
            if (include_self_transform) {
                m = Mat2D::multiply(m, self_transform(*node));
            }
        }
    }
    return m;
}

/// Axis-aligned bounding box of the leaf's local rect `(0,0,w,h)` after
/// being mapped through `m`.
Rect aabb_local_through(const Mat2D& m, float w, float h) {
    float xs[4]; float ys[4];
    m.apply(0.f, 0.f, xs[0], ys[0]);
    m.apply(w,   0.f, xs[1], ys[1]);
    m.apply(0.f, h,   xs[2], ys[2]);
    m.apply(w,   h,   xs[3], ys[3]);
    float min_x = xs[0], max_x = xs[0], min_y = ys[0], max_y = ys[0];
    for (int i = 1; i < 4; ++i) {
        min_x = std::min(min_x, xs[i]);
        max_x = std::max(max_x, xs[i]);
        min_y = std::min(min_y, ys[i]);
        max_y = std::max(max_y, ys[i]);
    }
    return { min_x, min_y, max_x - min_x, max_y - min_y };
}

Rect layout_rect_in_global(const pulp::view::View& v) {
    const Mat2D m = local_to_global_matrix(v, /*include_self_transform=*/false);
    return aabb_local_through(m, v.bounds().width, v.bounds().height);
}

Rect presentation_rect_in_global(const pulp::view::View& v) {
    const Mat2D m = local_to_global_matrix(v, /*include_self_transform=*/true);
    return aabb_local_through(m, v.bounds().width, v.bounds().height);
}

Rect resolve_geometry(pulp::view::View& v,
                      GeometrySpace space,
                      GeometrySource source) {
    if (space == GeometrySpace::ViewLocal) {
        return { 0.0f, 0.0f, v.bounds().width, v.bounds().height };
    }
    // Window and Screen collapse onto ViewGlobal in Phase 2.
    // Phase 6 adds window-origin / screen-origin offsets when the host
    // exposes them.
    return (source == GeometrySource::Presentation)
               ? presentation_rect_in_global(v)
               : layout_rect_in_global(v);
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
