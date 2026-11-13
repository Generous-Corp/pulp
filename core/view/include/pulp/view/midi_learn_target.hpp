#pragma once

/// @file midi_learn_target.hpp
/// Connects the MIDI Learn menu and overlay (midi_learn_menu.hpp) to a
/// `state::MidiLearnEngine`.
///
/// A `MidiLearnSession` owns the UI-side bookkeeping a learn needs: the editor's
/// timer calls `tick()` once, which polls the engine, records a completed learn
/// as one undo step when an editor is supplied, and remembers which parameter
/// just bound so the overlay can confirm it. `handle_key()` cancels a listening
/// learn on Escape. `target()` returns the `MidiLearnTarget` the menu and
/// overlays read.
///
/// UI thread only. The session must outlive the target it hands out and the
/// views holding it; the engine must outlive the session.
///
/// @code
/// MidiLearnSession learn(engine, [] { return now_ms(); }, &undo_editor);
/// attach_midi_learn_menu(knob, &root, kCutoff, learn.target());
/// // editor timer:    learn.tick(); overlay.refresh();
/// // key handling:    if (learn.handle_key(event)) return true;
/// @endcode

#include <pulp/state/midi_learn_engine.hpp>
#include <pulp/state/midi_learn_undo.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/midi_learn_menu.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace pulp::view {

class MidiLearnSession {
  public:
    /// How long a freshly bound mapping reports `just_bound`.
    static constexpr std::uint64_t kBoundConfirmMs = 2000;
    /// How long a refused learn keeps reporting its reason.
    static constexpr std::uint64_t kRefusalShowMs = 6000;

    MidiLearnSession(state::MidiLearnEngine& engine, std::function<std::uint64_t()> now_ms,
                     state::MidiLearnEditor* editor = nullptr)
        : engine_(engine), now_ms_(std::move(now_ms)), editor_(editor) {}

    /// The callbacks the menu and overlays use. Captures `this`.
    MidiLearnTarget target() {
        MidiLearnTarget t;
        t.available = [] { return true; };
        t.state = [this](state::ParamID id) { return state_of(id); };
        t.begin = [this](state::ParamID id) { engine_.begin_learn(id, now()); };
        t.cancel = [this] { engine_.cancel_learn(); };
        t.set_options = [this](state::ParamID id, const MidiLearnMappingOptions& options) {
            apply_options(id, options);
        };
        t.clear = [this](state::ParamID id) {
            if (editor_)
                editor_->clear(id);
            else
                engine_.clear(id);
        };
        return t;
    }

    /// Poll the engine. Call once per UI tick. Returns true when a learn bound
    /// on this call.
    bool tick() {
        // A refused outcome does not name the parameter, so note it before the
        // poll ends the listen.
        const auto listening = engine_.learn_state();
        auto outcome = engine_.poll_learn(now());
        if (outcome && !outcome->bound && outcome->refused_reason != nullptr) {
            refused_param_ = listening.param;
            refused_reason_ = outcome->refused_reason;
            refused_at_ms_ = now();
            has_refused_ = true;
        }
        if (!outcome || !outcome->bound)
            return false;
        // A successful learn supersedes any earlier refusal still on display.
        has_refused_ = false;
        refused_reason_.clear();
        if (editor_)
            editor_->record_learn(*outcome);
        bound_param_ = outcome->mapping.id;
        bound_at_ms_ = now();
        has_bound_ = true;
        return true;
    }

    /// Whether this plug-in's format can send feedback; the menu shows the
    /// "Send feedback" row only when it can (see `format::midi_feedback_capabilities`).
    void set_feedback_supported(bool supported) {
        feedback_supported_ = supported;
    }

    /// Escape cancels a listening learn and consumes the key. Returns false for
    /// any other key, for a key-up, or when nothing is listening.
    bool handle_key(const KeyEvent& event) {
        if (!event.is_down || event.key != KeyCode::escape || !engine_.learning())
            return false;
        engine_.cancel_learn();
        return true;
    }

  private:
    // The menu's choice lists are in enum order: absolute, two's complement,
    // signed bit, offset 64; and jump, pickup, scale.
    void apply_options(state::ParamID id, const MidiLearnMappingOptions& options) {
        auto mapping = engine_.mapping_for(id);
        if (!mapping)
            return;
        state::MidiMapping next = *mapping;
        next.mode = static_cast<state::RelativeMode>(std::clamp(options.relative_mode, 0, 3));
        next.takeover = static_cast<state::TakeoverMode>(std::clamp(options.takeover, 0, 2));
        next.feedback = options.feedback;
        if (options.invert != (next.scale.min > next.scale.max))
            std::swap(next.scale.min, next.scale.max);
        if (editor_)
            editor_->set_mapping(next, "Edit MIDI mapping");
        else
            engine_.set_mapping(next);
    }

    std::uint64_t now() const {
        return now_ms_ ? now_ms_() : 0;
    }

    MidiLearnParamState state_of(state::ParamID id) const {
        MidiLearnParamState s;
        if (has_refused_ && refused_param_ == id && now() - refused_at_ms_ < kRefusalShowMs)
            s.refused_reason = refused_reason_;
        const auto ls = engine_.learn_state();
        if (ls.phase == state::MidiLearnState::Phase::listening && ls.param == id) {
            s.phase = MidiLearnParamState::Phase::listening;
            return s;
        }
        if (const auto m = engine_.mapping_for(id)) {
            s.phase = MidiLearnParamState::Phase::mapped;
            s.mapping_label = state::label(m->source);
            s.options.relative_mode = static_cast<int>(m->mode);
            s.options.takeover = static_cast<int>(m->takeover);
            s.options.invert = m->scale.min > m->scale.max;
            s.options.feedback = m->feedback;
            s.feedback_supported = feedback_supported_;
            s.just_bound =
                has_bound_ && bound_param_ == id && now() - bound_at_ms_ < kBoundConfirmMs;
        }
        return s;
    }

    state::MidiLearnEngine& engine_;
    std::function<std::uint64_t()> now_ms_;
    state::MidiLearnEditor* editor_;
    state::ParamID bound_param_ = 0;
    std::uint64_t bound_at_ms_ = 0;
    bool has_bound_ = false;
    state::ParamID refused_param_ = 0;
    std::string refused_reason_;
    std::uint64_t refused_at_ms_ = 0;
    bool has_refused_ = false;
    bool feedback_supported_ = false;
};

} // namespace pulp::view
