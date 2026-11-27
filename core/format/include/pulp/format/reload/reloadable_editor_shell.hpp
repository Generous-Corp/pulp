#pragma once

/// @file reloadable_editor_shell.hpp
/// The hot-reloadable plug-in shell for logic libraries that bring their own
/// editor through `Processor::editor_factory()`, with no dependency on any UI
/// library: what a `pulp::vellum::ViewProcessor<>` logic library (or any
/// editor-host contract editor) reloads under, in place of `ReloadableShell`,
/// whose `create_view()` forwarding needs Pulp's view layer.
///
/// This header only declares. The shell and the reload machinery it drives
/// (ReloadableShellCore, the slot, the library loader) live in
/// src/reloadable_editor_shell.cpp, in the pulp-format-reload library.
///
/// The shell's `editor_factory()` hands each `create()` to the ACTIVE logic's
/// factory, through the `Processor` vtable both sides already share; it never
/// casts the logic to a concrete type across the library boundary. Each editor
/// it returns wraps the logic's editor and:
///
/// - holds the logic processor it was built from, so a hot swap parks that
///   processor instead of freeing it; the library image itself is retained, as
///   every reload image is;
/// - takes that hold under the slot's reader lock and builds the logic's editor
///   after releasing it, so a swap (and the audio thread) never waits for an
///   editor build;
/// - on its idle tick, when `editor_reload_generation()` has moved, detaches
///   and destroys the old logic's editor first, releases the hold, then builds
///   the new logic's editor in the same role and attaches it to the same parent
///   at the current size, so nothing calls into the retired logic after that
///   tick; after `invalidate()` it never builds again;
/// - forwards every other editor-host call to the logic's editor.
///
/// A role the active logic's factory does not offer has no editor. Editor calls
/// are on the main thread, like every editor-host call.

#include <pulp/format/processor.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace pulp::format::reload {

/// The shell's development controls, beside the `Processor` a host runs.
class ReloadableEditorShellControl {
  public:
    /// Reload the logic library now, bypassing the watcher (a dev "reload"
    /// command). Not the audio thread. False when the reload is rejected, with
    /// the reason in `error`.
    virtual bool reload_now(std::string* error = nullptr) = 0;

    /// Whether a logic processor is running.
    virtual bool has_active_dsp() const = 0;

    /// Audio blocks passed through because the slot was busy or empty.
    virtual std::uint64_t contention_blocks() const = 0;

    /// Retired logic processors an open editor still holds.
    virtual std::size_t held_retired_logic() const = 0;

    /// Free retired logic processors no editor holds. The dev watcher does this
    /// on every tick; a host or test without the watcher calls it from its
    /// control thread.
    virtual void reclaim_retired_logic() = 0;

  protected:
    ~ReloadableEditorShellControl() = default;
};

/// A shell over the logic library at `logic_path` (empty: the
/// `PULP_RELOAD_LOGIC_PATH` environment variable), as the `Processor` a format
/// adapter or host runs.
std::unique_ptr<Processor> make_reloadable_editor_shell(std::string logic_path = {});

/// The development controls of a shell `make_reloadable_editor_shell()` made,
/// or null for any other processor.
ReloadableEditorShellControl* reloadable_editor_shell_control(Processor& shell);

} // namespace pulp::format::reload
