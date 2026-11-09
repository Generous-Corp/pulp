#pragma once

/// @file midi_learn_undo.hpp
/// Undo/redo for MIDI mapping edits. Each edit is one `UndoManager` step: the
/// editor snapshots the table before and after, and undo/redo restore a
/// snapshot through `MidiLearnEngine::replace_all()` (mappings keep their
/// `uid`, so takeover state and later snapshots stay valid).
///
/// UI thread only. The engine must outlive the undo history that references it.

#include <algorithm>
#include <pulp/state/midi_learn_engine.hpp>
#include <pulp/state/undo_manager.hpp>
#include <string>
#include <utility>
#include <vector>

namespace pulp::state {

class MidiLearnEditor {
  public:
    MidiLearnEditor(MidiLearnEngine& engine, UndoManager& undo) : engine_(engine), undo_(undo) {}

    /// Add or replace a mapping as one undo step. False when the table is full.
    bool set_mapping(MidiMapping m, std::string name = "Edit MIDI mapping") {
        bool ok = false;
        record(std::move(name), [&] { ok = engine_.set_mapping(m); });
        return ok;
    }

    void clear(ParamID id, std::string name = "Clear MIDI mapping") {
        record(std::move(name), [&] { engine_.clear(id); });
    }

    void clear_source(const MidiSource& source, std::string name = "Clear MIDI mapping") {
        record(std::move(name), [&] { engine_.clear_source(source); });
    }

    void clear_all(std::string name = "Clear all MIDI mappings") {
        record(std::move(name), [&] { engine_.clear_all(); });
    }

    /// Load mappings (state chunk or JSON already parsed by the caller through
    /// the engine) as one undo step.
    template <typename Fn> void edit(std::string name, Fn&& fn) {
        record(std::move(name), std::forward<Fn>(fn));
    }

    /// Record a learn the engine already applied (`poll_learn()` returned
    /// `outcome`) as one undo step: undo removes the new mapping and restores
    /// whatever it replaced.
    void record_learn(const MidiLearnOutcome& outcome, std::string name = "MIDI Learn") {
        if (!outcome.bound)
            return;
        auto after = engine_.mappings();
        auto before = after;
        before.erase(
            std::remove_if(before.begin(), before.end(),
                           [&](const MidiMapping& m) { return m.uid == outcome.mapping.uid; }),
            before.end());
        before.insert(before.end(), outcome.replaced.begin(), outcome.replaced.end());
        push(std::move(name), std::move(before), std::move(after));
    }

  private:
    template <typename Fn> void record(std::string name, Fn&& fn) {
        auto before = engine_.mappings();
        fn();
        auto after = engine_.mappings();
        if (before == after)
            return;
        push(std::move(name), std::move(before), std::move(after));
    }

    void push(std::string name, std::vector<MidiMapping> before, std::vector<MidiMapping> after) {
        MidiLearnEngine* engine = &engine_;
        undo_.perform(UndoAction::create(
            std::move(name), [engine, before = std::move(before)] { engine->replace_all(before); },
            [engine, after = std::move(after)] { engine->replace_all(after); }));
    }

    MidiLearnEngine& engine_;
    UndoManager& undo_;
};

} // namespace pulp::state
