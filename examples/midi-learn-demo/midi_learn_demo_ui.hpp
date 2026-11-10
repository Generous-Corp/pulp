#pragma once

// The editor pieces for MidiLearnDemo: two knobs bound to the parameters, each
// with a right-click MIDI Learn menu and an overlay that shows when it is
// listening or mapped. The editor's UI timer calls tick(); Escape cancels a
// listening learn.

#include <pulp/state/midi_learn_undo.hpp>
#include <pulp/state/undo_manager.hpp>
#include <pulp/view/midi_learn_menu.hpp>
#include <pulp/view/midi_learn_target.hpp>
#include <pulp/view/parameter_binding.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

#include "midi_learn_demo.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace pulp::examples {

class MidiLearnDemoUi {
  public:
    MidiLearnDemoUi(MidiLearnDemoProcessor& processor, std::function<std::uint64_t()> now_ms)
        : editor_(processor.learn(), undo_),
          session_(processor.learn(), std::move(now_ms), &editor_) {
        root_.set_bounds({0, 0, 240, 120});
        add_knob(processor, kDemoLevel, {20, 30, 80, 80});
        add_knob(processor, kDemoPan, {140, 30, 80, 80});
    }

    view::View& root() {
        return root_;
    }
    view::Knob& level_knob() {
        return *knobs_[0];
    }
    view::Knob& pan_knob() {
        return *knobs_[1];
    }
    view::MidiLearnOverlay& level_overlay() {
        return *overlays_[0];
    }
    view::MidiLearnOverlay& pan_overlay() {
        return *overlays_[1];
    }
    state::UndoManager& undo() {
        return undo_;
    }
    view::MidiLearnSession& session() {
        return session_;
    }

    // Call from the editor's UI timer.
    void tick() {
        session_.tick();
        for (auto* o : overlays_)
            o->refresh();
    }

  private:
    void add_knob(MidiLearnDemoProcessor& processor, state::ParamID id, view::Rect bounds) {
        auto knob = std::make_unique<view::Knob>();
        knob->set_bounds(bounds);
        view::Knob* k = knob.get();
        bindings_.push_back(view::bind_parameter(*k, processor.state(), id));
        root_.add_child(std::move(knob));
        view::attach_midi_learn_menu(*k, &root_, id, session_.target());

        auto overlay = std::make_unique<view::MidiLearnOverlay>(id, session_.target());
        overlay->set_bounds(bounds);
        overlays_.push_back(overlay.get());
        root_.add_child(std::move(overlay));
        knobs_.push_back(k);
    }

    view::View root_;
    state::UndoManager undo_;
    state::MidiLearnEditor editor_;
    view::MidiLearnSession session_;
    std::vector<view::Knob*> knobs_;
    std::vector<view::MidiLearnOverlay*> overlays_;
    std::vector<view::ParameterBinding> bindings_;
};

} // namespace pulp::examples
