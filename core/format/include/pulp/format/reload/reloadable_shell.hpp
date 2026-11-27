#pragma once

/// @file reloadable_shell.hpp
/// The hot-reloadable plugin shell for logic libraries that build their own
/// Pulp view. Everything about loading, gating, watching and swapping the DSP
/// lives in `ReloadableShellCore` (reloadable_shell_core.hpp), which has no
/// view dependency. This class adds one thing: `create_view()` forwards to the
/// active logic, so a hot reload swaps the UI as well as the DSP.
///
/// A plug-in whose editor comes from a UI package (for example the Vellum
/// scripted editor) uses `ReloadableShellCore` directly and needs no Pulp view.

#include <pulp/format/reload/reloadable_shell_core.hpp>
#include <pulp/view/view.hpp> // complete View for create_view() forwarding

#include <memory>
#include <string>
#include <utility>

namespace pulp::format::reload {

class ReloadableShell : public ReloadableShellCore {
  public:
    /// @param logic_path path to the logic shared library. Empty resolves it
    /// from the `PULP_RELOAD_LOGIC_PATH` environment variable.
    explicit ReloadableShell(std::string logic_path = {})
        : ReloadableShellCore(std::move(logic_path)) {}

    // Forward the editor to the ACTIVE logic, so a hot reload swaps the UI as
    // well as the DSP. A host that keeps the editor across reloads rebuilds it
    // when editor_reload_generation() changes (or from set_on_reloaded), or the
    // logic returns a self-contained view; see
    // ProcessorHotSwapSlot::with_active() for the lifetime note.
    std::unique_ptr<view::View> create_view() override {
        return with_active_logic([](Processor& p) { return p.create_view(); });
    }
};

} // namespace pulp::format::reload
