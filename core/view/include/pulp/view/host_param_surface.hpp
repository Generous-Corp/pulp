#pragma once

#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <pulp/state/parameter.hpp>

namespace pulp::state {
class StateStore;
}

namespace pulp::view {

/// A framework-agnostic, native-view-facing runtime parameter accessor.
///
/// This is the SINGLE host surface a view binds against — defined once in the
/// SDK, so the same view can be driven by whichever parameter system its host
/// happens to own, and never knows which:
///   (a) embedded in a third-party plug-in framework — backed by that
///       framework's own parameter tree over the pulp-view-embed C ABI,
///   (b) native Pulp — backed by StateStore (see StateStoreHostParamSurface).
///
/// Unlike the bind-once path (NativeImportBindingContext / DesignFrameView's
/// element_for_param_key push), this resolves keys LIVE, so dynamic/paged
/// controls (effect racks, tabs) can rebind without a remount. Bind-once
/// remains the fast path for static top-level controls; this surface is the
/// fallthrough for keys a static registry does not carry.
///
/// Threading / call-context contract (hard rule). On macOS the embed's
/// paint/idle is already marshaled to the main
/// thread, so "which thread" is not the hazard; the hazards are re-entrancy and
/// inverting the push-at-tick architecture. Therefore:
///   - These calls are legal ONLY from tick/update, NEVER from paint(). The
///     embed facade snapshots values + display text once per tick into local
///     state; views paint from the snapshot. Calling from paint would let a
///     plugin-authored getText() override (arbitrary code, possibly holding
///     locks shared with processBlock) run mid-render-traversal.
///   - The non-virtual public methods assert !is_in_no_alloc_scope() in debug
///     builds — paint_all runs inside a ScopedNoAlloc, so a call from paint
///     trips the assert. This is a best-effort guard, not a correctness
///     guarantee; the audio thread must never touch this surface either.
///   - Host callbacks must not hold locks shared with the audio thread, and
///     display-text results should be memoized per (key, normalized) pair.
class HostParamSurface {
public:
    virtual ~HostParamSurface() = default;

    /// Live membership test — the single source of truth for "is this key
    /// bound right now". Cheap; safe to call every tick for every keyed element.
    bool has_param(std::string_view key) {
        assert_call_context("has_param");
        return do_has_param(key);
    }

    /// Current normalized [0, 1] value of @p key. Undefined (returns 0) if the
    /// key is unknown — gate with has_param() first.
    double get_param(std::string_view key) {
        assert_call_context("get_param");
        return do_get_param(key);
    }

    /// Write a normalized [0, 1] value. Bracket a continuous drag with
    /// begin_gesture / end_gesture so the host can group one undo step.
    void set_param(std::string_view key, double normalized) {
        assert_call_context("set_param");
        do_set_param(key, normalized);
    }

    void begin_gesture(std::string_view key) {
        assert_call_context("begin_gesture");
        do_begin_gesture(key);
    }

    void end_gesture(std::string_view key) {
        assert_call_context("end_gesture");
        do_end_gesture(key);
    }

    /// Host-formatted display of a normalized value, e.g. "500 ms". Backed by
    /// the host's own formatter (ParamInfo::to_string / getText / GetDisplay)
    /// with a default numeric+unit fallback when none is set. Memoize the
    /// result per (key, normalized) pair — the host formatter may be arbitrary
    /// plugin code.
    std::string param_display_text(std::string_view key, double normalized) {
        assert_call_context("param_display_text");
        return do_param_display_text(key, normalized);
    }

    /// Number of distinct values the HOST's parameter exposes for @p key — the
    /// authoritative cardinality of a discrete/choice/toggle parameter.
    ///
    /// **Returns 0 for a continuous parameter and for an unknown key.** 0 is
    /// deliberately unambiguous: it means "this parameter has no index domain",
    /// so a caller must never treat it as a denominator and must never divide
    /// by it. Use param_index_to_normalized(), which encodes that guard.
    ///
    /// This counts VALUES, not intervals — a 6-way waveform selector returns 6,
    /// a toggle returns 2. It matches ParamKind/value_labels cardinality
    /// (state::param_value_count), which is what the AU and AAX adapters
    /// advertise directly. It is NOT VST3's `stepCount` field, which counts
    /// intervals and is therefore this value minus one; the VST3 adapter
    /// applies that -1 at its own boundary. "Step count" is overloaded across
    /// plug-in APIs — this accessor is values, always.
    ///
    /// The count comes from the PARAMETER, never from a view's option list.
    /// A control that renders 3 visible positions may be bound to a 6-value
    /// parameter; the divisor is this count, not the number of things drawn.
    /// A view whose option count disagrees with this is mis-scaled — that
    /// mismatch is worth asserting on loudly rather than silently emitting
    /// wrong normalized values.
    int param_step_count(std::string_view key) {
        assert_call_context("param_step_count");
        return do_param_step_count(key);
    }

protected:
    virtual bool do_has_param(std::string_view key) = 0;
    virtual double do_get_param(std::string_view key) = 0;
    virtual void do_set_param(std::string_view key, double normalized) = 0;
    virtual void do_begin_gesture(std::string_view key) = 0;
    virtual void do_end_gesture(std::string_view key) = 0;
    virtual std::string do_param_display_text(std::string_view key, double normalized) = 0;

    /// Defaults to 0 ("continuous / no index domain") so a host surface
    /// implemented outside this repo keeps compiling and degrades to the
    /// pre-existing behavior rather than reporting a wrong cardinality.
    virtual int do_param_step_count(std::string_view key) {
        (void)key;
        return 0;
    }

private:
    /// Debug-only: aborts if called from inside a no-alloc (paint) scope.
    /// Compiles to nothing under NDEBUG.
    static void assert_call_context(const char* op);
};

/// Map a discrete parameter's value index to a normalized [0, 1] value.
///
/// The denominator is `step_count - 1`, not `step_count`: with N values, index
/// 0 maps to 0.0 and index N-1 maps to 1.0, so the top index reaches the top of
/// the host's automation range. This is the same convention
/// state::ParamRange::normalize() produces for an evenly-spaced discrete range,
/// and the one every format adapter round-trips against.
///
/// Evenly-spaced is the assumption, and it is why this takes a bare count: a
/// host surface may have no ParamRange behind it at all (an embedded framework's
/// parameter tree reaches this surface over a C ABI). A discrete parameter whose
/// ParamRange declares a skew is therefore OUT of this helper's contract — the
/// range's own curve, not this line, defines its normalized values, and the two
/// disagree. Declare discrete parameters with a linear range
/// (ParamRange::is_linear()); reach for range.normalize() directly if one
/// genuinely must be shaped.
///
/// @param step_count  a HostParamSurface::param_step_count() result. 0 (the
///        continuous/unknown signal) and 1 (a single-value parameter) both
///        have no index domain and yield 0.0 — this is the guard that keeps
///        the 0 contract from becoming a division by zero at a call site.
inline double param_index_to_normalized(int index, int step_count) {
    if (step_count <= 1) return 0.0;
    const int clamped = index < 0 ? 0 : (index > step_count - 1 ? step_count - 1 : index);
    return static_cast<double>(clamped) / static_cast<double>(step_count - 1);
}

/// Inverse of param_index_to_normalized: the nearest value index for a
/// normalized [0, 1] value. Rounds to nearest so a value landing between two
/// indices snaps to the closer one. Yields 0 when @p step_count carries no
/// index domain (0 or 1).
inline int param_normalized_to_index(double normalized, int step_count) {
    if (step_count <= 1) return 0;
    const double clamped = normalized < 0.0 ? 0.0 : (normalized > 1.0 ? 1.0 : normalized);
    return static_cast<int>(std::lround(clamped * static_cast<double>(step_count - 1)));
}

/// A framework-agnostic host command channel. A view calls
/// send_host_action("insert_slot", R"({"index":2})") and the host routes it;
/// the bool is a routing/diagnostic signal (log/assert on unhandled), NEVER
/// control flow. Return payloads and async completion are deliberately deferred
/// until two real consumers need them. The action/args_json
/// vocabulary matches NativeImportHostActionDescriptor so the import lane and
/// the runtime lane stay one concept.
class HostActionSurface {
public:
    virtual ~HostActionSurface() = default;

    bool send_host_action(std::string_view action, std::string_view args_json) {
        assert_call_context("send_host_action");
        return do_send_host_action(action, args_json);
    }

protected:
    virtual bool do_send_host_action(std::string_view action, std::string_view args_json) = 0;

private:
    static void assert_call_context(const char* op);
};

/// The native (Pulp `Processor::create_view()`) backing for HostParamSurface,
/// implemented over a StateStore. Keys are resolved to ParamIDs by a caller-
/// supplied resolver; the default resolver matches ParamInfo::name == key,
/// which is the convention the greenfield param generator (readDesignParams)
/// already follows. Display text uses ParamInfo::to_string when set, otherwise
/// a numeric+unit fallback so params that don't declare a formatter still read
/// sensibly. Step count comes from the author-declared ParamKind/value_labels
/// cardinality (state::param_value_count), so a range that merely quantizes via
/// range.step stays continuous here — matching how format adapters expose it.
class StateStoreHostParamSurface : public HostParamSurface {
public:
    /// Resolve a design param_key to a registered ParamID, or nullopt if the
    /// store carries no such parameter. ParamID 0 is a valid id, so the
    /// optional (not a sentinel id) carries the "unknown" signal.
    using KeyResolver = std::function<std::optional<state::ParamID>(std::string_view key)>;

    /// @param store     backing store (must outlive this surface)
    /// @param resolver  key→ParamID map; default matches ParamInfo::name == key
    explicit StateStoreHostParamSurface(state::StateStore& store, KeyResolver resolver = {});

protected:
    bool do_has_param(std::string_view key) override;
    double do_get_param(std::string_view key) override;
    void do_set_param(std::string_view key, double normalized) override;
    void do_begin_gesture(std::string_view key) override;
    void do_end_gesture(std::string_view key) override;
    std::string do_param_display_text(std::string_view key, double normalized) override;
    int do_param_step_count(std::string_view key) override;

private:
    state::StateStore& store_;
    KeyResolver resolver_;
};

} // namespace pulp::view
