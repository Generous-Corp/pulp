#pragma once

#include <pulp/format/aax_model.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/state/parameter.hpp>
#include <pulp/state/store.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

// Editor-support logic for the AAX adapter: parameter-gesture routing and the
// editor sizing contract. Both are expressed without any AAX SDK type so they
// are testable on every platform, including builds with no SDK configured. The
// thin shell that talks to `AAX_CEffectGUI` lives in `aax_effect_gui.cpp`.
//
// AAX records automation from an explicit touch/release pair: a custom editor
// must tell the host "the user grabbed this control" before writing values and
// "the user let go" afterwards. Pulp's editors already emit that intent through
// `state::Binding::begin_gesture()` / `end_gesture()`, which fan out via
// `StateStore::set_gesture_callbacks`. This router is the adapter between the
// two: it resolves a `state::ParamID` to the AAX parameter-ID string the
// EffectParameters object registered, and enforces the pairing AAX requires.

namespace pulp::format::aax {

/// Resolve an AAX parameter-ID string back to the `state::ParamID` it was
/// registered under. Returns false when the ID is not one of this plugin's
/// parameters — which is the normal case for AAX's own master-bypass control,
/// registered by the adapter but absent from the Pulp store.
inline bool param_id_for_aax_id(const PluginDefinition& definition,
                                std::string_view aax_id,
                                state::ParamID& out_id) {
    for (const auto& binding : definition.parameters) {
        if (binding.aax_id == aax_id) {
            out_id = binding.id;
            return true;
        }
    }
    return false;
}

/// The editor-facing surface of the AAX data model.
///
/// AAX splits a plug-in into a host-side data model (`AAX_CEffectParameters`,
/// which owns the parameters and the GUI) and a real-time algorithm that
/// receives values as coefficient packets. The two never share a `Processor`:
/// the algorithm's lives in its private data block, so the editor needs its own
/// on the model side. The parameter manager — not either `Processor` — is the
/// value authority; the model keeps this store and the manager in sync in both
/// directions so the editor and the algorithm never drift.
///
/// The adapter's `AAX_CEffectParameters` subclass implements this, and
/// `PulpAaxEffectGUI` resolves it from `AAX_CEffectGUI::GetEffectParameters()`.
/// Declared here, free of AAX types, so the seam stays visible to code and
/// tests that never see the SDK.
class EditorHost {
public:
    virtual ~EditorHost() = default;

    /// The model-side Processor whose `create_view()` builds the editor.
    /// Null when the plugin declares no editor or the factory failed.
    virtual Processor* editor_processor() = 0;

    /// The store the editor's `Binding`s read and write. Null iff
    /// `editor_processor()` is null.
    virtual state::StateStore* editor_store() = 0;

    /// The plugin's declared `ViewSize` hints, or nullopt when it has no editor
    /// to size. Reachable before any editor exists — AAX asks for a size through
    /// `GetViewSize` independently of the window lifecycle, and the answer comes
    /// from `Processor::view_size()`, which needs no view tree.
    virtual std::optional<ViewSize> editor_view_size() = 0;

    /// The parameter/bus model this plugin registered with AAX.
    virtual const PluginDefinition& editor_definition() const = 0;

    /// Release any automation gesture still open. The editor calls this as it
    /// tears down: a window closed mid-drag would otherwise leave the host's
    /// automation record open with no editor left to close it.
    virtual void release_editor_gestures() = 0;
};

/// How the editor surface reacts to the size AAX gives it.
///
/// Pro Tools owns the plugin window and pushes a size down through
/// `AAX_IViewContainer::SetViewSize`; the editor never negotiates. The plugin's
/// `ViewSize` hints decide what to do with that size, on the convention CLAP
/// and VST3 already ship (`resizable` iff both minimums are non-zero):
///
///   - fixed size (min == 0): pin the design viewport at the preferred size and
///     lock its aspect, so an off-size host pane letterbox-scales the artwork
///     instead of cropping it.
///   - resizable with an aspect ratio: pin the viewport and lock the aspect —
///     the imported-design path, where content scales but never distorts.
///   - resizable without an aspect ratio: no viewport, no lock. The root
///     reflows through Yoga at whatever size the host chose.
struct EditorSizePlan {
    uint32_t width = 0;        ///< size reported to the host from GetViewSize
    uint32_t height = 0;
    uint32_t min_width = 0;    ///< reported from GetMinimumViewSize
    uint32_t min_height = 0;
    bool resizable = false;    ///< host may drive a different size
    bool pin_viewport = false; ///< pin design viewport + lock aspect
    float aspect_ratio = 0.0f; ///< design aspect; meaningful iff pin_viewport
};

/// Derive the sizing plan from a plugin's declared `ViewSize` hints.
inline EditorSizePlan plan_editor_size(const ViewSize& hints) {
    EditorSizePlan plan;
    plan.width = hints.preferred_width;
    plan.height = hints.preferred_height;
    plan.resizable = hints.min_width > 0 && hints.min_height > 0;

    const bool free_resize = plan.resizable && hints.aspect_ratio <= 0.0;
    const bool has_design_size = hints.preferred_width > 0 && hints.preferred_height > 0;
    plan.pin_viewport = has_design_size && !free_resize;
    if (plan.pin_viewport) {
        plan.aspect_ratio = static_cast<float>(hints.preferred_width) /
                            static_cast<float>(hints.preferred_height);
    }

    // A fixed-size editor's floor is its design size: reporting a smaller one
    // would invite the host to shrink a surface that cannot reflow.
    plan.min_width = plan.resizable ? hints.min_width : plan.width;
    plan.min_height = plan.resizable ? hints.min_height : plan.height;
    return plan;
}

/// `state::ParamID` → the AAX parameter-ID string the model registered under.
///
/// The stored strings are stable for the map's lifetime, so a caller may hand
/// one straight to an `AAX_CParamID` parameter without copying.
class ParameterIdMap {
public:
    explicit ParameterIdMap(const PluginDefinition& definition) {
        ids_.reserve(definition.parameters.size());
        for (const auto& binding : definition.parameters) {
            ids_.emplace(binding.id, binding.aax_id);
        }
    }

    /// The AAX parameter-ID string for `id`, or nullptr when Pulp knows the
    /// parameter but AAX never registered it.
    const std::string* find(state::ParamID id) const {
        const auto it = ids_.find(id);
        return it == ids_.end() ? nullptr : &it->second;
    }

private:
    std::unordered_map<state::ParamID, std::string> ids_;
};

/// Writes editor-store changes back to the AAX parameter manager without the
/// two mirrors chasing each other.
///
/// The manager is the value authority; the editor's store is a mirror. Both
/// directions are live at once, so the write-back needs a rule that terminates:
///
///   - editor edit: the store changes, AAX still holds the old value, the write
///     goes through. `AAX_IParameter::SetValueWithFloat` does not store the
///     value — it asks the host for it, and the value arrives later through
///     `UpdateParameterNormalizedValue`, which is also what a fader move or a
///     control surface produces. The editor's own edits therefore come back to
///     the model on exactly the host path an external change takes.
///   - that return leg: the parameter manager is updated *before* the model
///     mirrors the value into the store, so by the time this rule runs the two
///     agree and nothing is sent. The loop closes here.
///
/// Equality closes it rather than an "I am syncing" flag because the rule holds
/// wherever and whenever the store listener lands — it is a statement about the
/// two values, not about control flow. It is also load-bearing rather than a
/// mere optimization: `StateStore::set_value()` notifies unconditionally, even
/// when the value did not change, so the echo has no fixed point of its own and
/// would ping-pong at the host's dispatch rate.
///
/// Exact float comparison is sound here because both sides derive the value
/// from the same normalized input with the same function: the adapter's taper
/// delegate maps normalized→real through `state::ParamRange::denormalize`,
/// which is precisely what `StateStore::set_normalized()` applies. The mirror
/// therefore lands bit-identical to the parameter, not merely close to it.
/// Where the store additionally constrains a value the taper does not — a
/// discrete parameter quantizing to its step — the two disagree for exactly one
/// more round trip: this writes the constrained value, AAX takes it, and the
/// next echo compares equal because the store's constraint is idempotent.
///
/// Free of AAX types: the manager is reached through two sinks, so the whole
/// contract is exercised by tests that never see the SDK.
class ParameterMirror {
public:
    /// Reads the value the AAX parameter manager currently holds. Returns false
    /// when `aax_param_id` names no registered parameter.
    using ValueReader = std::function<bool(const char* aax_param_id, float& out_value)>;

    /// Requests `value` for `aax_param_id` — `AAX_IParameter::SetValueWithFloat`.
    using ValueWriter = std::function<void(const char* aax_param_id, float value)>;

    ParameterMirror(const PluginDefinition& definition, ValueReader reader, ValueWriter writer)
        : ids_(definition)
        , reader_(std::move(reader))
        , writer_(std::move(writer))
    {}

    /// Editor store → AAX parameter manager. Wire to a `StateStore` value
    /// listener. Returns true when a write was issued.
    bool on_store_value_changed(state::ParamID id, float store_value) {
        const auto* aax_id = ids_.find(id);
        if (!aax_id) {
            return false;  // not an AAX-registered parameter
        }
        float aax_value = 0.0f;
        if (!reader_ || !reader_(aax_id->c_str(), aax_value)) {
            return false;
        }
        if (aax_value == store_value) {
            ++suppressed_count_;
            return false;  // AAX already holds this value — this is an echo
        }
        if (writer_) writer_(aax_id->c_str(), store_value);
        return true;
    }

    /// Echoes absorbed. Non-zero is the healthy case — it counts the write-backs
    /// the equality rule swallowed rather than bouncing at the host.
    std::uint64_t suppressed_count() const { return suppressed_count_; }

private:
    ParameterIdMap ids_;
    ValueReader reader_;
    ValueWriter writer_;
    std::uint64_t suppressed_count_ = 0;
};

/// Maps `StateStore` gesture callbacks onto AAX automation touch/release.
///
/// The sinks receive the AAX parameter-ID string owned by this router; it stays
/// valid for the router's lifetime, so a sink may pass it straight to an
/// `AAX_CParamID` parameter without copying.
///
/// **Main-thread only**, matching `StateStore::begin_gesture` /
/// `end_gesture`. An editor that edits parameters from a worker marshals the
/// whole gesture with `StateStore::run_gesture_on_main`.
///
/// The router is the single place that guarantees AAX's balance invariant:
///   - a second `begin` for an already-touched parameter emits nothing, so a
///     widget that re-enters a drag cannot open two automation records;
///   - an `end` without a matching `begin` emits nothing, so a stray release
///     cannot close a record the host never opened;
///   - a parameter Pulp knows but AAX never registered is ignored rather than
///     touched under a bogus ID.
class GestureRouter {
public:
    /// Invoked with the AAX parameter-ID string for the gesture's parameter.
    using Sink = std::function<void(const char* aax_param_id)>;

    GestureRouter(const PluginDefinition& definition, Sink touch, Sink release)
        : ids_(definition)
        , touch_(std::move(touch))
        , release_(std::move(release))
    {}

    /// Route `Binding::begin_gesture()` to `AAX_IEffectParameters::TouchParameter`.
    /// Returns true when a touch was emitted.
    bool begin(state::ParamID id) {
        const auto* aax_id = lookup(id);
        if (!aax_id) {
            ++unknown_count_;
            return false;
        }
        if (!touched_.insert(id).second) {
            return false;  // already touched — keep the record single
        }
        if (touch_) touch_(aax_id->c_str());
        return true;
    }

    /// Route `Binding::end_gesture()` to `AAX_IEffectParameters::ReleaseParameter`.
    /// Returns true when a release was emitted.
    bool end(state::ParamID id) {
        const auto* aax_id = lookup(id);
        if (!aax_id) {
            ++unknown_count_;
            return false;
        }
        if (touched_.erase(id) == 0) {
            return false;  // never touched — nothing to release
        }
        if (release_) release_(aax_id->c_str());
        return true;
    }

    /// Release every parameter still held. An editor torn down mid-drag (the
    /// user closes the plugin window without lifting the mouse) would otherwise
    /// leave the host's automation record open forever.
    void release_all() {
        // Copy: end() mutates touched_.
        const std::unordered_set<state::ParamID> held = touched_;
        for (const auto id : held) {
            end(id);
        }
    }

    /// Whether a gesture is currently open on `id`.
    bool is_touched(state::ParamID id) const { return touched_.count(id) != 0; }

    /// Number of parameters with an open gesture.
    std::size_t touched_count() const { return touched_.size(); }

    /// Gestures dropped because the parameter has no AAX registration. Non-zero
    /// means the editor and the parameter definition disagree — a wiring bug.
    std::uint64_t unknown_count() const { return unknown_count_; }

    /// The AAX parameter-ID string for `id`, or nullptr when unregistered.
    const std::string* aax_id_for(state::ParamID id) const { return lookup(id); }

private:
    const std::string* lookup(state::ParamID id) const { return ids_.find(id); }

    ParameterIdMap ids_;
    Sink touch_;
    Sink release_;
    std::unordered_set<state::ParamID> touched_;
    std::uint64_t unknown_count_ = 0;
};

} // namespace pulp::format::aax
