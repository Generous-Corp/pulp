#pragma once

/// @file motion.hpp
/// Agent-first motion observability: sample view geometry, scroll
/// state, and arbitrary scalar values at a chosen FPS, emit
/// epsilon-bounded change-only events with Start/End burst framing,
/// and route them to pluggable sinks (log lines and structured
/// events). Off by default; activate with
/// `Coordinator::set_tracing_enabled(true)`.

#include <pulp/view/geometry.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pulp::view {

class FrameClock;
class View;

namespace motion {

// ── Geometry taxonomy ────────────────────────────────────────────────

enum class GeometrySource {
    Layout,        ///< Yoga-computed bounds, ignoring paint-time transforms.
    Presentation,  ///< After paint_all() composition. Phase 0 falls back to Layout.
};

enum class GeometrySpace {
    ViewLocal,    ///< Origin at the target view's top-left, in its own frame.
    ViewGlobal,   ///< Root-view coordinate space.
    Window,       ///< Phase 0: same as ViewGlobal. Phase 2 adds window origin.
    Screen,       ///< Phase 0: same as ViewGlobal. Phase 2 adds screen offset.
};

enum class GeometryProperty {
    MinX, MinY, MaxX, MaxY,
    MidX, MidY,
    Width, Height,
};

// ── Trace options ────────────────────────────────────────────────────

struct TraceOptions {
    int fps = 15;
};

// ── Sample event ─────────────────────────────────────────────────────

struct SampleEvent {
    enum class Kind { Baseline, Sample, Start, End };

    Kind kind = Kind::Baseline;
    std::string view_name;
    std::string metric_name;
    double t_seconds = 0.0;                  ///< Monotonic FrameClock::time().
    std::uint64_t frame = 0;                 ///< Monotonic FrameClock::frame().
    int precision = 3;
    std::vector<std::pair<std::string, double>> components;  ///< Sorted by name.
    std::vector<std::pair<std::string, double>> deltas;      ///< End events only.
};

/// Render the canonical `[PulpMotion][view][metric] ...` line for a sample event.
std::string format_line(const SampleEvent& e);

// ── Sinks ────────────────────────────────────────────────────────────

using Sink = std::function<void(const SampleEvent&)>;

/// Sink that writes one log line per event via `pulp::runtime::log_debug`.
Sink make_log_sink();

/// Sink that appends events to a caller-owned buffer. For tests and
/// in-process consumers; the buffer pointer must outlive the sink.
Sink make_buffer_sink(std::vector<SampleEvent>* buffer);

// ── Forward declarations ─────────────────────────────────────────────

class Coordinator;
class TraceBuilder;

// ── Trace handle (RAII) ──────────────────────────────────────────────

class TraceHandle {
public:
    TraceHandle() noexcept = default;
    TraceHandle(TraceHandle&& other) noexcept;
    TraceHandle& operator=(TraceHandle&& other) noexcept;
    TraceHandle(const TraceHandle&) = delete;
    TraceHandle& operator=(const TraceHandle&) = delete;
    ~TraceHandle();

    /// Remove the trace from its coordinator. Idempotent.
    void detach();

    bool is_attached() const noexcept { return coord_ != nullptr; }
    int id() const noexcept { return id_; }

private:
    friend class Coordinator;
    friend class TraceBuilder;
    TraceHandle(int id, Coordinator* coord) noexcept
        : id_(id), coord_(coord) {}

    int id_ = 0;
    Coordinator* coord_ = nullptr;
};

// ── Trace builder DSL ────────────────────────────────────────────────

class TraceBuilder {
public:
    using ValueSampler = std::function<double()>;
    using Component = std::pair<std::string, ValueSampler>;

    /// Single-scalar metric. The sampler is invoked once per sampler tick.
    TraceBuilder& value(std::string name, ValueSampler sampler,
                        int precision = 3, double epsilon = 0.0001);

    /// Multi-component metric (e.g., a 2D point). Components are sampled
    /// together and emitted as one log line.
    TraceBuilder& multi(std::string name, std::vector<Component> components,
                        int precision = 3, double epsilon = 0.0001);

    /// Geometry metric over a target view. Phase 0 implements
    /// `source = Layout`; `Presentation` falls back to Layout.
    TraceBuilder& geometry(std::string name,
                           pulp::view::View& target,
                           std::vector<GeometryProperty> props
                               = {GeometryProperty::MinX, GeometryProperty::MinY,
                                  GeometryProperty::Width, GeometryProperty::Height},
                           GeometrySpace space = GeometrySpace::Window,
                           GeometrySource source = GeometrySource::Layout,
                           int precision = 2, double epsilon = 0.1);

    /// Register the trace with the coordinator and return an owning handle.
    TraceHandle attach();

    /// Internal spec (PIMPL-style). Public so Coordinator can hold a
    /// shared_ptr<Spec> in its public-API signature; clients should not
    /// construct or inspect it directly.
    struct Spec;

private:
    friend class Coordinator;
    TraceBuilder(Coordinator* coord, std::string view_name, TraceOptions opts);

    std::shared_ptr<Spec> spec_;
    Coordinator* coord_ = nullptr;
};

// ── Coordinator ──────────────────────────────────────────────────────

/// Process-wide singleton that owns one FrameClock subscription, holds
/// the active trace set, and dispatches sample events to registered
/// sinks. Off by default; call `set_tracing_enabled(true)` to start
/// emitting events.
class Coordinator {
public:
    /// Process-wide instance. Bound to a FrameClock via `bind()`.
    static Coordinator& instance();

    /// Bind the coordinator's single tick subscription to a FrameClock.
    /// Replaces any prior binding.
    void bind(pulp::view::FrameClock& clock);

    /// Drop the FrameClock subscription. No effect if not bound.
    void unbind();

    bool is_bound() const noexcept;

    /// Add a sink and return its id (for later removal).
    int add_sink(Sink sink);
    void remove_sink(int sink_id);
    void clear_sinks();

    /// Convenience: add the default `make_log_sink()` and return its id.
    int install_default_log_sink();

    /// Global tracing toggle. Off by default. When off, `on_tick` is a no-op.
    void set_tracing_enabled(bool on);
    bool tracing_enabled() const noexcept;

    /// Filter-scope firehose toggle. Reserved for Phase 3 (animation auto-trace).
    /// Phase 0 stores and exposes the flag; nothing consumes it yet.
    void set_firehose(bool on);
    bool firehose() const noexcept;

    /// Start building a trace.
    TraceBuilder trace(std::string view_name, TraceOptions opts = {});

    /// Remove a trace by id (called automatically by TraceHandle destruction).
    void detach(int trace_id);

    /// Reset all state (binding, sinks, traces, counters). For tests.
    void reset();

    std::size_t active_trace_count() const noexcept;

    /// Cumulative count of SampleEvents dispatched since `reset()`. For tests.
    std::size_t emitted_event_count() const noexcept;

private:
    Coordinator();
    ~Coordinator();
    Coordinator(const Coordinator&) = delete;
    Coordinator& operator=(const Coordinator&) = delete;

    void on_tick(float dt);
    int register_trace(std::shared_ptr<TraceBuilder::Spec> spec);
    friend class TraceBuilder;

    struct State;
    std::unique_ptr<State> state_;
};

} // namespace motion
} // namespace pulp::view
