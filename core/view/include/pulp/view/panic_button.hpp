#pragma once

/// @file panic_button.hpp
/// A stock "panic" push button: one click asks the plug-in to silence itself
/// and, where the host has MIDI output, to end the notes it sent downstream.
///
/// The button knows nothing about processors or formats. It talks to a
/// `PanicTarget`, two callbacks that raise the request and report what the
/// current host can do with it. `pulp/format/panic_target.hpp` builds one from
/// a `format::Processor`; a test or a non-plug-in app can supply its own.
///
/// When the host does not service panic requests the button hides itself (or,
/// with `set_hide_when_unavailable(false)`, stays visible but disabled), so an
/// editor can always include it and never shows a control that does nothing.
/// Availability is read at construction, before each click, and whenever the
/// editor calls `refresh()`.

#include <pulp/view/buttons.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace pulp::view {

/// What a panic request does in the current host.
enum class PanicAvailability : std::uint8_t {
    /// Nothing services the request; the button hides or disables itself.
    unavailable,
    /// The plug-in's own voices stop; no MIDI is sent downstream.
    voices_only,
    /// The plug-in's voices stop and note-offs plus channel panic messages are
    /// sent on the host's MIDI output.
    voices_and_midi,
};

/// The two calls a `PanicButton` needs. Both are invoked on the UI thread.
struct PanicTarget {
    /// Raise a panic request. `release_only` asks voices to enter their normal
    /// release (MIDI CC 123) instead of the hard stop (CC 120).
    std::function<void(bool release_only)> request;
    /// What the current host can do with a request.
    std::function<PanicAvailability()> availability;
};

class PanicButton : public TextButton {
  public:
    explicit PanicButton(PanicTarget target, std::string label = "Panic")
        : TextButton(std::move(label)), target_(std::move(target)) {
        on_click = [this] { trigger(); };
        refresh();
    }

    /// Re-read availability and update visibility, enabled state and the
    /// accessible label. Cheap; call it when the host may have changed (after
    /// the editor attaches, or from the editor's UI timer).
    PanicAvailability refresh() {
        availability_ =
            target_.availability ? target_.availability() : PanicAvailability::unavailable;
        const bool usable = availability_ != PanicAvailability::unavailable;
        set_enabled(usable);
        set_visible(usable || !hide_when_unavailable_);
        set_access_label(access_text());
        return availability_;
    }

    /// Availability as of the last `refresh()`.
    PanicAvailability availability() const { return availability_; }

    /// Raise the request now, as a click does. Returns false, and raises
    /// nothing, when the host cannot service it.
    bool trigger() {
        if (refresh() == PanicAvailability::unavailable || !target_.request)
            return false;
        target_.request(release_only_);
        ++requests_;
        return true;
    }

    /// Ask for the release-only panic (CC 123) instead of the hard stop.
    void set_release_only(bool release_only) {
        release_only_ = release_only;
        set_access_label(access_text());
    }
    bool release_only() const { return release_only_; }

    /// Hide when unavailable (the default) or stay visible but disabled.
    void set_hide_when_unavailable(bool hide) {
        hide_when_unavailable_ = hide;
        refresh();
    }
    bool hide_when_unavailable() const { return hide_when_unavailable_; }

    /// Requests this button has raised.
    std::uint64_t request_count() const { return requests_; }

  private:
    std::string access_text() const {
        std::string text = label();
        switch (availability_) {
        case PanicAvailability::unavailable:
            return text + ": unavailable in this host";
        case PanicAvailability::voices_only:
            return text + (release_only_ ? ": release all notes" : ": stop all notes");
        case PanicAvailability::voices_and_midi:
            return text + (release_only_ ? ": release all notes and send MIDI notes off"
                                         : ": stop all notes and send MIDI panic");
        }
        return text;
    }

    PanicTarget target_;
    PanicAvailability availability_ = PanicAvailability::unavailable;
    bool release_only_ = false;
    bool hide_when_unavailable_ = true;
    std::uint64_t requests_ = 0;
};

} // namespace pulp::view
