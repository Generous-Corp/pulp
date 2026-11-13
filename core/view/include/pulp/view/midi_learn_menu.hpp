#pragma once

/// @file midi_learn_menu.hpp
/// The right-click affordance for MIDI Learn on a parameter-bound control, and
/// the overlay that shows a control is listening or mapped.
///
/// The view layer knows nothing about engines or formats. It talks to a
/// `MidiLearnTarget`: a few callbacks that report whether MIDI Learn exists in
/// this plug-in and what a parameter's mapping looks like, and that start,
/// cancel and clear a learn. `midi_learn_target()` (midi_learn_target.hpp)
/// builds one from a `state::MidiLearnEngine`; a test or another host can
/// supply its own. While `available()` is false (no engine) every function
/// here yields nothing: no menu items, no overlay.
///
/// All calls are UI-thread calls.
///
/// @code
/// auto target = view::midi_learn_target(engine, clock);
/// view::attach_midi_learn_menu(cutoff_knob, &root, kCutoff, target);
/// auto overlay = std::make_unique<view::MidiLearnOverlay>(kCutoff, target);
/// // editor timer: overlay->refresh();
/// @endcode

#include <pulp/state/parameter.hpp>
#include <pulp/view/context_menu.hpp>
#include <pulp/view/view.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pulp::view {

/// The per-mapping settings the menu can change. Values index the choice lists
/// below, which `midi_learn_menu_items()` shows in this order.
struct MidiLearnMappingOptions {
    /// 0 absolute, 1 two's complement, 2 signed bit, 3 offset 64 (relative encoders).
    int relative_mode = 0;
    /// 0 jump, 1 pickup, 2 scale.
    int takeover = 0;
    /// Flip the response: a control turned up lowers the parameter.
    bool invert = false;
    /// Send the parameter's value back to the controller.
    bool feedback = false;

    friend bool operator==(const MidiLearnMappingOptions&,
                           const MidiLearnMappingOptions&) = default;
};

/// What MIDI Learn shows for one parameter.
struct MidiLearnParamState {
    enum class Phase : std::uint8_t {
        idle,      ///< No mapping, not listening.
        listening, ///< A learn is waiting for a controller for this parameter.
        mapped,    ///< A controller is mapped to this parameter.
    };
    Phase phase = Phase::idle;
    /// The mapped controller, e.g. "CC 74 ch 1". Empty unless mapped.
    std::string mapping_label;
    /// True for a short time after a learn binds, so the overlay can confirm it.
    bool just_bound = false;
    /// The mapping's settings; meaningful only when mapped.
    MidiLearnMappingOptions options;
    /// Whether this plug-in's format can send feedback to a controller. The
    /// feedback row is shown only when it can.
    bool feedback_supported = false;
    /// Why the last learn for this parameter was refused (for example the
    /// controller is reserved by the plug-in), for a short time afterwards.
    /// Empty otherwise. The menu shows it as a disabled row.
    std::string refused_reason;
};

/// The calls the menu and overlay need. Any member may be empty; an empty
/// `available` means MIDI Learn is not present.
struct MidiLearnTarget {
    std::function<bool()> available;
    std::function<MidiLearnParamState(state::ParamID)> state;
    std::function<void(state::ParamID)> begin;
    std::function<void()> cancel;
    std::function<void(state::ParamID)> clear;
    /// Apply new settings to the mapping of a parameter. Empty hides the
    /// settings rows.
    std::function<void(state::ParamID, const MidiLearnMappingOptions&)> set_options;

    bool is_available() const {
        return available && available();
    }
};

/// Command ids of the rows `midi_learn_menu_items()` returns.
enum MidiLearnMenuCommand : int {
    kMidiLearnCommandBegin = 0x4D4C01,
    kMidiLearnCommandCancel = 0x4D4C02,
    kMidiLearnCommandClear = 0x4D4C03,
    kMidiLearnCommandInvert = 0x4D4C04,
    kMidiLearnCommandRelativeMode = 0x4D4C05,
    kMidiLearnCommandTakeover = 0x4D4C06,
    kMidiLearnCommandFeedback = 0x4D4C07,
};

/// The menu rows for `param`, in order, when it is mapped and the target can
/// change settings: toggles for "Invert" and (where supported) "Send feedback",
/// and choices for "Relative mode" and "Takeover". Each control applies its change
/// through `target.set_options` as it is changed and leaves the menu open.
///
/// The menu rows for `param`: a disabled "Cannot learn: <reason>" row while a
/// refusal is being reported, "MIDI Learn" (or "Cancel MIDI Learn" while this
/// parameter is listening), a disabled "Mapped to CC 74 ch 1" label and
/// "Clear MIDI Mapping" when it is mapped. Empty when MIDI Learn is not
/// available, so a parameter menu can append the result unconditionally.
std::vector<ContextMenu::Item> midi_learn_menu_items(const MidiLearnTarget& target,
                                                     state::ParamID param);

/// Carry out one of the rows above. Returns false for an unknown id or an
/// unavailable target.
bool run_midi_learn_menu_command(const MidiLearnTarget& target, state::ParamID param, int command);

/// Show the MIDI Learn rows alone as a context menu at a root-local point.
/// Returns the menu, or nullptr when there is nothing to show. A parameter
/// menu that builds its own rows should append `midi_learn_menu_items()`
/// instead and route the ids to `run_midi_learn_menu_command()`.
ContextMenu* show_midi_learn_menu(View* root, Point root_pos, const MidiLearnTarget& target,
                                  state::ParamID param);

/// Give `widget` a right-click handler that opens the MIDI Learn menu for
/// `param`. The widget's `on_context_menu` slot holds one handler: this
/// replaces any previous one, so a control that already has a menu should use
/// `midi_learn_menu_items()` in it instead. `root` must outlive the widget.
void attach_midi_learn_menu(View& widget, View* root, state::ParamID param, MidiLearnTarget target);

/// Drawn over a parameter control: an outline and "Listening..." chip while a
/// learn waits on it, a confirming chip with the controller's label right after
/// it binds, and a small dot while it stays mapped. Draws nothing for an idle
/// parameter or an unavailable target. Not hit-testable: it never takes a click
/// from the control beneath it. Size it to the control it decorates.
class MidiLearnOverlay : public View {
  public:
    MidiLearnOverlay(state::ParamID param, MidiLearnTarget target);

    /// Re-read the parameter's state and repaint if it changed. Cheap; call
    /// from the editor's UI timer.
    void refresh();

    const MidiLearnParamState& current() const {
        return state_;
    }

    void paint(canvas::Canvas& canvas) override;

  private:
    state::ParamID param_;
    MidiLearnTarget target_;
    MidiLearnParamState state_;
};

} // namespace pulp::view
