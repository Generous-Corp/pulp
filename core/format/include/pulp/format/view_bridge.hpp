#pragma once

#include <pulp/format/processor.hpp>
#include <pulp/view/view.hpp>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace pulp::runtime { class MessageChannel; }
namespace pulp::view {
class ScriptedUiSession;
class HostActionSurface;
class HostParamSurface;
class StateStoreHostParamSurface;
} // namespace pulp::view

namespace pulp::format {

/// Whether a DAW-hosted editor should enable scripted-UI + theme hot reload
/// (item 1.3). OFF by default — a shipping plugin must not watch and reload from
/// disk inside a host — but a developer opts in for the edit→see-it loop by
/// exporting `PULP_DEV_HOT_RELOAD=1` in the DAW's environment. (The standalone
/// app always enables it; it IS the dev tool.) Polling already runs on every
/// editor tick, so this only decides whether the watcher acts. Kept header-inline
/// so the platform view controllers (`au_view_controller_*.mm`,
/// `au_v2_cocoa_view.mm`) share one definition.
inline bool dev_editor_hot_reload_enabled() {
    const char* v = std::getenv("PULP_DEV_HOT_RELOAD");
    if (!v) return false;
    switch (v[0]) {
        case '1': case 't': case 'T': case 'y': case 'Y': return true;
        default: return false;
    }
}

/// Role of a view attached to a ViewBridge. The primary editor is
/// `Editor`; auxiliary panels (component inspector, remote preview)
/// attach as secondary views with a matching role.
enum class ViewRole {
    Editor,
    Inspector,
    Remote,
};

/// Manages editor-view lifecycle for a single `Processor` across all
/// plugin formats. A ViewBridge owns the constructed view tree, tracks
/// its attached/detached state, and dispatches lifecycle callbacks
/// (`on_view_opened`, `on_view_closed`, `on_view_resized`) to the
/// processor.
///
/// One processor can have multiple ViewBridges (multi-view):
/// each host editor window, the inspector, and any remote views each
/// own their own primary View instance. Parameter binding is shared
/// through the processor's `StateStore`, so all attached views stay in
/// sync automatically.
///
/// Construction is cheap — no view is built until `open()` is called.
/// Destruction closes the view if it is still open.
class ViewBridge {
public:
    struct Options {
        bool enable_hot_reload = false;  ///< Poll scripted UI + theme.json for changes
        ViewRole role = ViewRole::Editor;
    };

    ViewBridge(Processor& processor, state::StateStore& store);
    ViewBridge(Processor& processor, state::StateStore& store, Options options);
    ~ViewBridge();

    ViewBridge(const ViewBridge&) = delete;
    ViewBridge& operator=(const ViewBridge&) = delete;

    /// Build the view. Returns false if view construction failed; inspect
    /// `last_error()` for details. Calling `open()` on an already-open
    /// bridge is a no-op that returns true.
    ///
    /// `on_view_opened` is **not** fired here. The adapter must call
    /// `notify_attached()` once the view has been attached to its host
    /// parent window. This split avoids firing `on_view_opened` before
    /// the host attach step succeeds.
    bool open(std::string* error = nullptr);

    /// Fire `on_view_opened(view)` — called by the adapter once the
    /// view has been attached to its native parent. Idempotent: a
    /// second call after successful attach is a no-op.
    void notify_attached();

    /// Transfer ownership of the primary view to the caller while
    /// preserving the bridge's lifecycle dispatch. After `release_view()`:
    ///
    ///   - Ownership of the `view::View` passes to the caller (typically
    ///     because a container widget like `TabPanel::add_tab` needs a
    ///     `unique_ptr`).
    ///   - `view()` still returns the raw pointer so `resize()`,
    ///     `notify_attached()`, and `close()` continue to dispatch
    ///     `Processor::on_view_*` callbacks on the released view.
    ///   - The caller **must keep the view alive at least until**
    ///     `ViewBridge::close()` has been called (or the bridge
    ///     destroys), otherwise lifecycle dispatch will touch freed
    ///     memory.
    ///   - Returns `nullptr` if the bridge is not open or the view was
    ///     already released.
    std::unique_ptr<view::View> release_view();

    /// Destroy the view. Fires `on_view_closed` only if
    /// `notify_attached()` previously fired, so open/close dispatch
    /// stays balanced even when host attachment failed after `open()`.
    /// No-op if not open.
    void close();

    /// Notify the bridge that the host resized the editor. Dispatches
    /// `on_view_resized` and stores the new size.
    void resize(uint32_t width, uint32_t height);

    /// Live editor reload (live-swap 1.9). When the processor supports editor
    /// reload (`Processor::supports_editor_reload()`), the editor idle tick calls
    /// this each frame; it compares `Processor::editor_reload_generation()` to the
    /// last seen value and, on a change, rebuilds the OPEN editor in place by
    /// re-calling `create_view()` on the (now hot-swapped) processor and
    /// transplanting its content into the SAME root `View` the host references —
    /// so a DAW sees the editor update live without re-instantiating the plugin.
    /// Returns true if a rebuild happened (the caller should then relayout +
    /// repaint the host). No-op (returns false) for processors that don't support
    /// editor reload, when not open, or when the generation is unchanged.
    bool poll_editor_reload();

    /// Force a rebuild of the open primary view from `create_view()` now,
    /// transplanting the fresh content (children + background) into the stable
    /// root `View`. Returns false if not open or `create_view()` yields nothing.
    /// Normally driven by `poll_editor_reload()`; exposed for tests. NOTE: the
    /// root `View` OBJECT is preserved (the host holds it by reference); a logic
    /// whose `create_view()` returns a CUSTOM `View` subclass with root-level
    /// paint/behavior gets its children + background refreshed but not the root
    /// subclass identity — sufficient for container-root editors (the common case).
    bool rebuild_primary_view();

    bool is_open() const { return view_raw_ != nullptr; }
    ViewRole role() const { return options_.role; }
    view::View* view() { return view_raw_; }
    const view::View* view() const { return view_raw_; }

    /// The processor's parameter store, used (e.g.) by the editor idle pump
    /// to drain queued host-automation changes to Main-thread listeners so
    /// parameter-bound widgets follow automation playback.
    state::StateStore& store() { return store_; }
    bool uses_script_ui() const { return uses_script_ui_; }

    /// The runtime host-parameter surface installed on the open view tree.
    ///
    /// A view built by `create_view()` — in particular an imported
    /// `DesignFrameView` with `route_changes_to_host_params(true)` — resolves
    /// its `param_key`s against `View::host_params()`. `open()` installs this
    /// StateStore-backed surface on the tree, so those views drive the
    /// processor's parameters with no per-plugin wiring: keys resolve to the
    /// store parameter whose `ParamInfo::name` matches (the convention the
    /// design-param generator already emits).
    ///
    /// This is the ONLY writer the routed path uses. A consumer that ALSO
    /// forwards `DesignFrameView::on_element_changed` into the same store would
    /// deliver each gesture twice; wire one or the other, never both (see
    /// `routes_changes_to_host_params()`).
    ///
    /// Null before `open()` / after `close()`.
    view::HostParamSurface* host_params();

    /// Install the command channel a routed view's `Kind::action` clicks reach
    /// (`route_actions_to_host(true)` → `View::host_actions()`). Pulp has no
    /// native action backing — the destination is host-defined — so this stays
    /// caller-supplied and is null by default. The surface must outlive the
    /// open view. Applied immediately when the bridge is already open.
    void set_host_actions(view::HostActionSurface* actions);
    view::HostActionSurface* host_actions() const { return host_actions_; }

    /// Cross-thread liveness token. Set true at construction, false in the
    /// destructor. A callback that outlives or races this bridge — the GPU
    /// display-link scripted-idle pump is dispatched to the main queue and can
    /// run after the bridge is replaced (host view reload) or freed (teardown
    /// ordering / stop-doesn't-join races) — copies this shared token and
    /// no-ops when it reads false, instead of dereferencing a freed bridge.
    const std::shared_ptr<std::atomic<bool>>& alive_token() const { return alive_; }

    /// Access the scripted UI session, if the primary view was built from
    /// a JS script. This includes the framework-owned `PULP_UI_SCRIPT_PATH`
    /// path and custom processors that expose a session through
    /// `Processor::active_scripted_ui()`.
    view::ScriptedUiSession* scripted_ui();
    const view::ScriptedUiSession* scripted_ui() const;

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    const ViewSize& size_hints() const { return size_hints_; }

    const std::string& last_error() const { return last_error_; }

    /// Attach a secondary view (e.g. inspector, remote) to this bridge.
    /// The bridge takes ownership. Returns a non-owning pointer the caller
    /// can use to reference the attached view. Multiple secondary views
    /// may share the same role.
    view::View* attach_secondary_view(std::unique_ptr<view::View> view, ViewRole role);

    /// Detach and destroy a previously-attached secondary view. Returns
    /// true if the view was found and removed.
    bool detach_secondary_view(view::View* view);

    /// Attach a remote view session driving a `MessageChannel` (usually
    /// a `WebSocketChannel`). The session speaks the Remote View
    /// Protocol — see `docs/reference/remote-view-protocol.md`. The
    /// bridge takes ownership of the session and its channel; callers
    /// use the returned non-owning pointer to drive the protocol.
    /// Returns nullptr if the handshake fails; the bridge's
    /// `last_error()` records the reason.
    class RemoteViewSession* attach_remote_channel(
        std::unique_ptr<runtime::MessageChannel> channel,
        std::string label = {});

    /// Detach and destroy a remote view session. Idempotent.
    bool detach_remote(class RemoteViewSession* session);

    /// Total number of attached views (primary + secondary). Zero when
    /// not open and no secondaries are attached.
    size_t view_count() const;

    /// Access an attached view by index. Index 0 is the primary view
    /// (if open); indices >=1 are secondary views in attach order.
    /// Returns nullptr if index is out of range.
    view::View* view_at(size_t index);

    /// Role of the view at `index`. Returns `Editor` when out of range.
    ViewRole role_at(size_t index) const;

private:
    Processor& processor_;
    state::StateStore& store_;
    Options options_;

    std::unique_ptr<view::View> view_;
    view::View* view_raw_ = nullptr;  ///< valid even after release_view()
    /// Production backing for View::host_params() — built in open(), detached
    /// from the tree in close() BEFORE the view dies. See host_params().
    std::unique_ptr<view::StateStoreHostParamSurface> host_param_surface_;
    view::HostActionSurface* host_actions_ = nullptr;  ///< caller-owned; may be null
    std::unique_ptr<view::ScriptedUiSession> scripted_ui_;
    bool uses_script_ui_ = false;
    bool attached_ = false;  ///< true between notify_attached() and close()
    bool released_ = false;  ///< true after release_view() transfers ownership
    uint64_t last_reload_generation_ = 0;  ///< editor-reload generation last applied (1.9)
    /// Cross-thread liveness (see alive_token()). Flipped false in ~ViewBridge.
    std::shared_ptr<std::atomic<bool>> alive_ =
        std::make_shared<std::atomic<bool>>(true);

    struct Secondary {
        std::unique_ptr<view::View> view;
        ViewRole role;
    };
    std::vector<Secondary> secondaries_;

    std::vector<std::unique_ptr<class RemoteViewSession>> remotes_;

    ViewSize size_hints_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    std::string last_error_;
};

} // namespace pulp::format
