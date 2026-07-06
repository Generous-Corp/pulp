#pragma once

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
/// SDK so that a view written against it runs unchanged in three hosts:
///   (a) embedded in JUCE   — backed by APVTS over the pulp-view-embed C ABI,
///   (b) embedded in iPlug2 — backed by IParams over the same ABI,
///   (c) native Pulp        — backed by StateStore (see StateStoreHostParamSurface).
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

protected:
    virtual bool do_has_param(std::string_view key) = 0;
    virtual double do_get_param(std::string_view key) = 0;
    virtual void do_set_param(std::string_view key, double normalized) = 0;
    virtual void do_begin_gesture(std::string_view key) = 0;
    virtual void do_end_gesture(std::string_view key) = 0;
    virtual std::string do_param_display_text(std::string_view key, double normalized) = 0;

private:
    /// Debug-only: aborts if called from inside a no-alloc (paint) scope.
    /// Compiles to nothing under NDEBUG.
    static void assert_call_context(const char* op);
};

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
/// sensibly.
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

private:
    state::StateStore& store_;
    KeyResolver resolver_;
};

} // namespace pulp::view
