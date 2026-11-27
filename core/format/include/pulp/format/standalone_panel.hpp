#pragma once

// A tool window the standalone app offers beside its editor, supplied by a UI
// integration: the musical typing keyboard is one. Format-core links no UI
// library, so a panel arrives through a factory the integration installs at
// static initialization, the way the control host does. The standalone
// creates every installed panel before its audio starts, adds a menu item and
// a shortcut that toggle it, hands it the main window so it can open beside
// it, drains its MIDI on the audio thread, and shuts it down once its audio
// has stopped. The panel owns its window and draws with its own library.

#include <pulp/format/standalone_config.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pulp::format {

/// What a panel factory is given.
struct StandalonePanelContext {
    /// The standalone's effective config; a factory returns null for a panel
    /// the config does not ask for.
    const StandaloneConfig& config;
    /// Queues a MIDI event for the processor through the standalone's
    /// lock-free UI MIDI path. Main thread. False when the queue is full.
    std::function<bool(const midi::MidiEvent&)> midi_sink;
    /// True while an editable text field in any of the app's windows has
    /// keyboard focus. A panel that listens for keys app-wide leaves them to
    /// the field. Main thread. Empty means the standalone cannot tell.
    std::function<bool()> text_input_focused;
};

class StandalonePanel {
  public:
    virtual ~StandalonePanel() = default;

    /// The menu item's title, which also names the window.
    virtual std::string menu_title() const = 0;
    /// The lowercase letter that, with the platform's main modifier (Cmd on
    /// macOS, Ctrl elsewhere), toggles the panel. 0 for none.
    virtual char shortcut() const = 0;

    /// Show the panel when it is hidden, hide it when it is shown. False when
    /// it cannot be shown. Main thread.
    virtual bool toggle() = 0;
    virtual bool is_visible() const = 0;

    /// The standalone's main window (an `NSWindow*` on macOS), so the panel
    /// opens beside it; null once that window is gone. Main thread.
    virtual void set_primary_window(void* native_window) = 0;

    /// Audio thread: append events the panel could not queue on the main
    /// thread (a note-off that found the queue full). Must not allocate or
    /// lock. Called from the panel's creation until the standalone destroys
    /// it, which happens after its audio has stopped.
    virtual void drain_midi_into(midi::MidiBuffer& midi, int frame_count) noexcept = 0;

    /// Close the window and release held notes. The standalone calls this
    /// once, after its audio has stopped and just before destroying the
    /// panel. Main thread.
    virtual void shutdown() noexcept = 0;
};

using StandalonePanelFactory =
    std::unique_ptr<StandalonePanel> (*)(const StandalonePanelContext& context);

namespace detail {

/// Install a panel factory for this executable. Static-init safe. Installing
/// the same factory again is a no-op that returns true; false when the
/// factory is null or the fixed table is full.
bool install_standalone_panel_factory(StandalonePanelFactory factory) noexcept;

/// One panel per installed factory that returned one, in install order.
std::vector<std::unique_ptr<StandalonePanel>>
create_standalone_panels(const StandalonePanelContext& context);

/// Test-only: forget every installed factory.
void reset_standalone_panel_factories_for_testing() noexcept;

} // namespace detail
} // namespace pulp::format
