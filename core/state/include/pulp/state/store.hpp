#pragma once

#include <pulp/state/listener_token.hpp>
#include <pulp/state/parameter.hpp>
#include <array>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <memory>
#include <span>

namespace pulp::events { class EventLoop; }

namespace pulp::state {

/// Named parameter group for hierarchical UI organization.
///
/// Groups can be nested: set @c parent_id to another group's @c id.
/// A @c parent_id of 0 means the group is at the root level.
struct ParamGroup {
    int id = 0;
    std::string name;
    int parent_id = 0; ///< 0 = root-level group.
};

/// Centralized parameter storage with lock-free value access.
///
/// StateStore is the single source of truth for all plugin parameters.
/// Register parameters at initialization via add_parameter(), then
/// read/write values from any thread using the lock-free accessors.
///
/// @code
/// pulp::state::StateStore store;
/// store.add_parameter({.id = 1, .name = "Gain", .range = {-60, 12, 0}});
/// store.set_value(1, -3.0f);
/// float gain = store.get_value(1); // -3.0
/// @endcode
///
/// @note Parameter registration (add_parameter, add_group) is NOT thread-safe.
///       Call these only during plugin initialization, before processing starts.
/// @note Value access (get_value, set_value, etc.) is lock-free and safe to
///       call from the audio thread.
class StateStore {
public:
    StateStore();
    ~StateStore();

    StateStore(const StateStore&) = delete;
    StateStore& operator=(const StateStore&) = delete;

    /// Register a parameter. Call during initialization only.
    /// @param info  Immutable metadata for this parameter.
    void add_parameter(const ParamInfo& info);

    /// Register a parameter group. Call during initialization only.
    void add_group(const ParamGroup& group);

    /// Read a parameter's base value (lock-free).
    /// @note Safe to call from the audio thread.
    float get_value(ParamID id) const;

    /// Read a parameter's modulated value: base + mod_offset (lock-free).
    /// @note Safe to call from the audio thread.
    float get_modulated(ParamID id) const;

    /// Write a parameter's base value (lock-free).
    /// @note Safe to call from any thread.
    void set_value(ParamID id, float value);

    /// Real-time-safe parameter write for use from the audio thread.
    ///
    /// Performs the atomic value store (lock-free, no allocation) and
    /// fires registered @c ListenerThread::Audio listeners inline. Any
    /// @c ListenerThread::Main listeners are NOT invoked here — the
    /// change is pushed onto a single-producer/single-reader queue and
    /// drained by @c pump_listeners() on the main thread. This avoids
    /// the @c EventLoop::dispatch lambda allocation that the generic
    /// @c set_value() path performs when a Main listener exists.
    ///
    /// Use this from format-adapter audio callbacks (CLAP / VST3 / AU
    /// process) for host-driven parameter writes. UI-driven writes from
    /// the main thread should use @c set_value().
    ///
    /// @note The pending-changes queue is bounded; if @c pump_listeners()
    ///       is not called fast enough the oldest queued events are
    ///       dropped (the atomic value is still up-to-date, so the UI
    ///       picks up the latest on its next pump).
    void set_value_rt(ParamID id, float value);

    /// Real-time-safe normalized-value write. Denormalizes through the
    /// parameter's @c ParamRange and then dispatches through
    /// @c set_value_rt(). Used by the VST3 adapter where the host
    /// reports parameter changes in normalized [0, 1] form.
    void set_normalized_rt(ParamID id, float normalized);

    /// Set the absolute modulation offset for a parameter.
    void set_mod_offset(ParamID id, float offset);

    /// Add a delta to a parameter's modulation offset.
    void add_mod_offset(ParamID id, float delta);

    /// Clear all modulation offsets to zero.
    void reset_all_mod();

    /// Read a parameter's value mapped to [0, 1] (lock-free).
    float get_normalized(ParamID id) const;

    /// Write a parameter from a normalized [0, 1] value (lock-free).
    void set_normalized(ParamID id, float normalized);

    /// Get the default value for a parameter (from its ParamRange).
    float get_default(ParamID id) const;

    /// Reset a single parameter to its default value.
    void reset_to_default(ParamID id);

    /// Reset all parameters to their default values.
    void reset_all_to_defaults();

    /// Look up immutable metadata for a parameter.
    /// @return Pointer to ParamInfo, or nullptr if @p id is not registered.
    const ParamInfo* info(ParamID id) const;

    /// Block-local snapshot of @p N parameter values.
    ///
    /// Loads each parameter once and returns the values in a stack-
    /// allocated @c std::array. DSP code should call this once at the
    /// top of @c process() and read from the returned array inside
    /// the per-sample loop, instead of calling @c get_value() per
    /// sample (each per-sample atomic load is a memory fence and
    /// fans out across cores). Mirrors the "snapshot the world
    /// before you start" pattern called out in sudara "Big List of
    /// JUCE Tips" #29.
    ///
    /// @tparam N  Compile-time count of parameter IDs to read.
    /// @param ids  Parameter IDs to snapshot, in the desired output
    ///             order. Unknown IDs yield @c 0.0f at their slot.
    /// @return std::array<float, N> — same order as @p ids.
    ///
    /// @code
    /// constexpr std::array<ParamID, 2> kIds = { kGainId, kMixId };
    /// auto p = store.snapshot(kIds);
    /// for (int s = 0; s < n; ++s) {
    ///     out[s] = in[s] * p[0] + dry[s] * (1.f - p[1]);
    /// }
    /// @endcode
    template <std::size_t N>
    [[nodiscard]] std::array<float, N> snapshot(
        const std::array<ParamID, N>& ids) const noexcept;

    /// Modulated variant of @c snapshot — returns each parameter's
    /// base + per-buffer modulation offset (see
    /// @c set_mod_offset). Use this from a synth that consumes
    /// CLAP's modulated values inside the voice loop.
    template <std::size_t N>
    [[nodiscard]] std::array<float, N> snapshot_modulated(
        const std::array<ParamID, N>& ids) const noexcept;

    /// View of all registered parameters (in registration order).
    std::span<const ParamInfo> all_params() const { return params_; }

    /// View of all registered groups (in registration order).
    std::span<const ParamGroup> all_groups() const { return groups_; }

    /// Number of registered parameters.
    std::size_t param_count() const { return params_.size(); }

    /// Signal the host that a gesture (drag, click) has begun on a parameter.
    /// Call from the UI thread before a series of set_value() calls so the
    /// host groups them into a single undo step.
    void begin_gesture(ParamID id);

    /// Signal the host that a gesture has ended.
    void end_gesture(ParamID id);

    /// Install an @c EventLoop used to marshal @c ListenerThread::Main
    /// listeners onto the main thread. If unset, Main listeners run
    /// inline on whichever thread fired @c set_value().
    ///
    /// Format adapters and standalone hosts install this during plugin
    /// initialization; tests usually leave it unset.
    void set_main_loop(pulp::events::EventLoop* loop);

    /// Subscribe to parameter-value changes.
    ///
    /// The returned @c ListenerToken owns the subscription. Destroy it
    /// (or call @c ListenerToken::reset()) to remove the listener. The
    /// token MUST outlive the listener's expected firing window — discarding
    /// it removes the subscription immediately.
    ///
    /// @param callback  Invoked for every change. Pass an empty
    ///                  @c ParamChangeCallback to register a no-op slot.
    /// @param thread    @c ListenerThread::Main (default) marshals the
    ///                  callback through the installed @c EventLoop;
    ///                  @c ListenerThread::Audio runs it inline on the
    ///                  firing thread and asserts caller RT-safety.
    [[nodiscard]]
    ListenerToken add_listener(ParamChangeCallback callback,
                               ListenerThread thread);

    /// Convenience for opting into real-time-safe inline invocation.
    /// Equivalent to @c add_listener(cb, ListenerThread::Audio).
    [[nodiscard]]
    ListenerToken add_audio_listener(ParamChangeCallback callback);

    /// Remove a listener explicitly. Equivalent to letting the token
    /// fall out of scope.
    void remove_listener(ListenerToken& token);

    /// Drain queued main-thread listener invocations posted by
    /// @c set_value_rt() / @c set_normalized_rt() from the audio thread.
    /// Call this from the main thread (e.g. the editor's per-frame
    /// timer or the host's UI tick).
    ///
    /// @return Number of changes drained from the queue.
    std::size_t pump_listeners();

    /// Legacy permanent-listener registration. Prefer the
    /// @c ListenerToken-returning overload above for new code.
    ///
    /// The callback is invoked inline on whichever thread fires
    /// @c set_value() and cannot be removed for the lifetime of the
    /// @c StateStore. Pulp's own subsystems are migrating to the token
    /// API; this overload remains for one release as a compatibility
    /// shim.
    void add_listener(ParamChangeCallback callback);

    /// Serialize all parameter values to a binary blob for preset/state save.
    ///
    /// This is the parameter-only payload. Format adapters may wrap it in a
    /// higher-level host blob alongside Processor::serialize_plugin_state().
    /// @return Byte vector suitable for storage or transmission.
    std::vector<uint8_t> serialize() const;

    /// Restore parameter values from a parameter-only binary blob.
    ///
    /// Format adapters may decode a higher-level host blob first, then pass
    /// only the embedded StateStore payload here.
    /// @return True if deserialization succeeded.
    bool deserialize(std::span<const uint8_t> data);

    /// Set the schema version embedded in serialized state.
    /// Increment this when the parameter layout changes between plugin versions.
    void set_state_version(uint32_t version) { state_version_ = version; }

    /// Get the current state schema version.
    uint32_t state_version() const { return state_version_; }

private:
    std::vector<ParamInfo> params_;
    std::vector<ParamGroup> groups_;
    std::unordered_map<ParamID, std::size_t> id_to_index_;
    std::vector<ParamValue> values_;
    std::shared_ptr<detail::ListenerRegistry> registry_;
    std::vector<ListenerToken> permanent_listener_tokens_;
    uint32_t state_version_ = 1;

    std::function<void(ParamID)> on_begin_gesture_;
    std::function<void(ParamID)> on_end_gesture_;

public:
    /// Install callbacks that forward gesture begin/end to the plugin host.
    /// Format adapters (VST3, AU, CLAP) call this during initialization so
    /// that UI-driven gestures are reported to the host for undo grouping.
    void set_gesture_callbacks(
        std::function<void(ParamID)> begin_fn,
        std::function<void(ParamID)> end_fn)
    {
        on_begin_gesture_ = std::move(begin_fn);
        on_end_gesture_ = std::move(end_fn);
    }
};

// ─── Template definitions ──────────────────────────────────────────────────

template <std::size_t N>
std::array<float, N> StateStore::snapshot(
    const std::array<ParamID, N>& ids) const noexcept
{
    std::array<float, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        out[i] = get_value(ids[i]);
    }
    return out;
}

template <std::size_t N>
std::array<float, N> StateStore::snapshot_modulated(
    const std::array<ParamID, N>& ids) const noexcept
{
    std::array<float, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        out[i] = get_modulated(ids[i]);
    }
    return out;
}

} // namespace pulp::state
