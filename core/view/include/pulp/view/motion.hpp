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

// ── Provenance envelope (Phase 7) ────────────────────────────────────
//
// Opaque metadata describing *where* a trace came from. Carried on
// TraceStarted (trace-level) and on burst Start events (burst-level).
// Phase 7 ships the shape + plumbing; the richer adapters that fill
// it from each animation surface (Tween / CSS / AnimatorSet / JS rAF
// / design-import) land in Phase 9.

struct Provenance {
    std::string source_kind;   ///< "tween" | "css-transition" | "animator-set"
                               ///< | "rAF" | "design-import" | "user" | "publish"
    std::string source_id;     ///< Free-form id, e.g. "Card.opacity tween" or
                               ///< "figma:LevelMeter/Panel" or "import:claude:1234"
    std::string source_file;   ///< Optional, e.g. __FILE__ at attach site
    int source_line = 0;       ///< Optional, e.g. __LINE__ at attach site

    bool is_set() const noexcept {
        return !source_kind.empty() || !source_id.empty() ||
               !source_file.empty() || source_line != 0;
    }
};

// ── Sample event ─────────────────────────────────────────────────────
//
// Per-tick or per-publish event in a motion stream. Schema v2 carries
// stable identifiers (trace_id / metric_id / burst_id) so fixtures can
// be compared by identity instead of position — reordered identical
// bursts no longer false-fail an `assert_matches`.

struct SampleEvent {
    enum class Kind {
        TraceStarted,  ///< Emitted once per trace registration. Carries
                       ///< the trace's provenance envelope.
        Baseline,      ///< First sample of a metric.
        Sample,        ///< Value sample within a change burst.
        Start,         ///< Burst opened (value drifted off baseline / last
                       ///< stable).
        End,           ///< Burst closed (value stabilized). Carries deltas.
    };

    Kind kind = Kind::Baseline;
    std::string view_name;
    std::string metric_name;
    double t_seconds = 0.0;                  ///< Monotonic FrameClock::time().
    std::uint64_t frame = 0;                 ///< Monotonic FrameClock::frame().
    int precision = 3;

    // Stable identifiers (schema v2). Sampler-driven traces get a
    // positive `trace_id` from `register_trace`; publishes use 0 to
    // signal the publish channel. `metric_id` is the metric's index
    // within its trace (or 0 for publish). `burst_id` increments at
    // each Start within a given (trace_id, metric_id).
    int trace_id = 0;
    int metric_id = 0;
    int burst_id = 0;

    Provenance provenance;  ///< Populated on TraceStarted; empty otherwise.

    std::vector<std::pair<std::string, double>> components;  ///< Sorted by name.
    std::vector<std::pair<std::string, double>> deltas;      ///< End events only.
};

/// Render the canonical `[PulpMotion][view][metric] ...` line for a sample event.
std::string format_line(const SampleEvent& e);

// ── Publish channel (Phase 3) ────────────────────────────────────────
//
// `publish_value()` is the entry point for any code that wants to make
// its scalar / structured values observable without taking a
// FrameClock sampler subscription. Typical callers are animation
// primitives — a `Tween`, a CSS `TransitionSpec`, a JS rAF callback —
// that already advance once per frame; calling publish at the same
// tick site avoids the doubled-sampling cost.
//
// Routing:
//   - When `Coordinator::tracing_enabled()` is false, publish is a
//     no-op (cheap, branch-predictable; safe to leave in hot paths).
//   - When `Coordinator::firehose()` is true, the published value fans
//     out as a Sample event to every installed sink, with the same
//     epsilon / Start/End burst semantics as sampler-driven traces.
//   - When `firehose()` is false and no subscription is wired (Phase 5
//     adds the subscription filter), publish is also a no-op.
//
// Filter-scoped subscriptions land in Phase 5 alongside the
// `pulp motion trace --selector <id>` CLI surface; the publish channel
// is the underlying primitive both modes share.

struct PublishOptions {
    int precision = 3;
    double epsilon = 0.0001;
};

/// Single-component publish.
void publish_value(std::string view_name,
                   std::string metric_name,
                   double value,
                   PublishOptions opts = {});

/// Multi-component publish (e.g., a 2D point or a geometry rect).
/// Components are sorted by name before emission so log lines are stable.
void publish_components(std::string view_name,
                        std::string metric_name,
                        std::vector<std::pair<std::string, double>> components,
                        PublishOptions opts = {});

// ── Record / replay fixtures (Phase 5) ───────────────────────────────
//
// A fixture is the on-disk form of a motion stream — one JSONL event
// per line, preceded by a header line carrying the schema version. The
// format is deliberately stable so checked-in goldens survive long
// after the recording session.
//
// Layered intent:
//   - `make_fixture_sink(path)` is a Sink that writes events to disk
//     as they're emitted. Plug it into the Coordinator's sink set to
//     record everything an active trace produces.
//   - `replay_fixture(path, sink)` re-emits a fixture's events to any
//     sink without running the original UI. Lets analyzers /
//     assertions work offline on a captured run.
//   - `assert_matches(golden, captured, opts)` compares two fixtures
//     and returns a structured diff agents can act on.
//
// Fixture version 1 schema (one event per line, sorted-key JSON):
//   {"kind":"baseline|sample|start|end",
//    "view":"…","metric":"…","t":…,"frame":…,"precision":N,
//    "components":{"k":v,…}, "deltas":{"k":v,…}}
// Lines are separated by `\n`; the first line is the header:
//   {"motion_fixture_version":1}

constexpr int kFixtureSchemaVersion = 2;

// (`make_fixture_sink` and `replay_fixture` are declared below
// alongside the other Sink helpers so the `using Sink = …` typedef is
// visible at their declaration site.)

/// Load a fixture file into memory. Returns events in file order.
/// Empty vector on missing / unreadable / unknown-version files.
std::vector<SampleEvent> load_fixture(const std::string& path);

/// Comparison result from `assert_matches`. Empty `differences` means
/// the captured run matched the golden within tolerances.
struct FixtureDiff {
    struct Item {
        std::string kind;      ///< "missing-event", "extra-event",
                               ///< "component-drift", "timing-drift"
        std::string view_name;
        std::string metric_name;
        std::string component_name;
        std::string detail;
        double observed = 0.0;
        double expected = 0.0;
    };
    std::vector<Item> differences;
    bool matches() const noexcept { return differences.empty(); }
};

struct FixtureMatchOptions {
    double component_epsilon = 0.05;
    double timing_epsilon_seconds = 0.05;
    bool require_same_event_count = true;
};

FixtureDiff assert_matches(const std::vector<SampleEvent>& golden,
                           const std::vector<SampleEvent>& captured,
                           FixtureMatchOptions opts = {});

// ── Assertion helpers (Phase 5) ──────────────────────────────────────
//
// Each helper operates on a flat sequence of scalars extracted from
// the captured events. Use `extract_scalar(events, view, metric, comp)`
// to pull the time-ordered series, then pass it to the assertion.
// Helpers favor explicit semantics over heuristics — `is_monotonic` is
// strictly monotonic (with epsilon), `settling_time_seconds` is "time
// from first change to final stable value", etc.

/// Flatten a fixture's events into a (time, value) series for one
/// (view, metric, component) triple. Includes Baseline + Sample only;
/// Start / End markers are skipped.
struct ScalarSample { double t = 0.0; double value = 0.0; };
std::vector<ScalarSample> extract_scalar(
    const std::vector<SampleEvent>& events,
    std::string_view view_name,
    std::string_view metric_name,
    std::string_view component_name = "value");

/// Strict monotonicity check with epsilon. Direction inferred from the
/// first non-zero change. Returns false if any subsequent change
/// reverses direction beyond epsilon.
bool is_monotonic(const std::vector<ScalarSample>& samples,
                  double epsilon = 1e-6);

/// Time from first change above epsilon to last change above epsilon.
/// 0.0 if fewer than 2 changes.
double settling_time_seconds(const std::vector<ScalarSample>& samples,
                             double epsilon = 1e-6);

/// Overshoot relative to the final value: max excursion beyond the
/// final value's direction from the first stable region. Reported as a
/// fraction of total displacement (0.0 = clean, 0.1 = 10% overshoot).
double overshoot(const std::vector<ScalarSample>& samples,
                 double epsilon = 1e-6);

/// Time from t0 to the first change above epsilon.
double start_delay_seconds(const std::vector<ScalarSample>& samples,
                           double epsilon = 1e-6);

/// Standard deviation of inter-sample intervals — proxy for frame-pacing
/// jitter when the sampler is run at a fixed FPS.
double frame_jitter_seconds(const std::vector<ScalarSample>& samples);

/// Returns the last sample's value, or NaN when empty.
double final_value(const std::vector<ScalarSample>& samples);

// ── Sinks ────────────────────────────────────────────────────────────

using Sink = std::function<void(const SampleEvent&)>;

/// Sink that writes one log line per event via `pulp::runtime::log_debug`.
Sink make_log_sink();

/// Sink that appends events to a caller-owned buffer. For tests and
/// in-process consumers; the buffer pointer must outlive the sink.
Sink make_buffer_sink(std::vector<SampleEvent>* buffer);

/// Sink that appends each `SampleEvent` as a JSONL line to `path`.
/// Opens the file (truncates) on first event. Subsequent events
/// append. Closes implicitly when the returned Sink is dropped.
/// Pair with `Coordinator::add_sink(make_fixture_sink(path))`.
Sink make_fixture_sink(std::string path);

/// Read a fixture file from disk and dispatch each event to `sink`.
/// Returns the number of events replayed, or `-1` on parse error.
int replay_fixture(const std::string& path, const Sink& sink);

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

    /// Attach a provenance envelope to the trace. Emitted once on
    /// the trace's `TraceStarted` event.
    TraceBuilder& with_provenance(Provenance p);

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

    // ── Phase 3: publish-channel internals ────────────────────────────
    /// Called by `publish_value` / `publish_components`. Public so the
    /// free functions can reach it without becoming friends; callers
    /// should prefer the `publish_*` free functions for forward
    /// compatibility.
    void publish_internal(std::string view_name,
                          std::string metric_name,
                          std::vector<std::pair<std::string, double>> components,
                          PublishOptions opts);

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
