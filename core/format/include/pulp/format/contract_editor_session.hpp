#pragma once

// One editor opened through the editor-host contract (editor_host.hpp), held
// by a format adapter on the host's behalf.
//
// Every plug-in format turns its own editor callbacks (VST3 IPlugView, CLAP
// clap_plugin_gui, an AU Cocoa view) into the same handful of contract calls.
// This session owns what those adapters share, so each one is a thin mapping:
//
// - the editor itself, built from the processor's EditorFactory in the editor
//   role, with an EditorContext whose resize and host-action requests go to
//   hooks the adapter supplies;
// - one StateStoreParamPort over the adapter's store, so edits reach the host
//   through the adapter's existing gesture path, and closing this editor never
//   ends a gesture another editor holds;
// - attachment: `detach()` releases every gesture this session holds before
//   the editor detaches, and `invalidate()` (the adapter or its processor is
//   going away) releases them, invalidates the editor and destroys it while the
//   processor that owns its factory still exists;
// - the idle tick: while attached, Editor::idle() runs from a paced main-thread
//   chain (MainThreadDispatcher::call_async_after) unless the adapter drives
//   `tick()` itself from a host timer. A tick already queued when the session
//   detaches, invalidates or is destroyed neither ticks nor touches it;
// - size negotiation shared by every format: the current size, a host resize
//   (keeping the scale the editor reports), a new content scale, whether the
//   editor is resizable, and a proposed size adjusted by the editor and clamped
//   to its hints.
//
// Main thread only. View-free: it names no view, canvas or render type, so it
// compiles into pulp-format-core.

#include <pulp/format/editor_host.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace pulp::state {
class StateStore;
}

namespace pulp::format {

class ContractEditorSession;

/// The contract sessions one adapter instance opened that a host still holds.
///
/// A host may keep an editor (a VST3 view, an AU Cocoa view) after the plug-in
/// instance that made it is torn down, but the editor's factory belongs to the
/// processor. An adapter whose editors outlive it in that way declares one of
/// these after its processor, so its destruction (or an explicit
/// `invalidate_all()`) invalidates every registered session while the
/// processor and store still exist. Sessions unregister themselves when they
/// are destroyed or invalidated. Main thread only.
class ContractSessionRegistry {
  public:
    ContractSessionRegistry() = default;
    ~ContractSessionRegistry();
    ContractSessionRegistry(const ContractSessionRegistry&) = delete;
    ContractSessionRegistry& operator=(const ContractSessionRegistry&) = delete;

    /// Invalidate every registered session and forget them.
    void invalidate_all();
    std::size_t size() const {
        return sessions_.size();
    }

  private:
    friend class ContractEditorSession;
    std::vector<ContractEditorSession*> sessions_;
};

class ContractEditorSession {
  public:
    /// Interval of the session's own idle tick while attached (about 60 Hz).
    static constexpr int kIdleIntervalMs = 16;

    /// What the adapter does for the editor's context.
    struct HostHooks {
        /// Ask the host to resize its window to `size`. True when it accepted.
        /// Null: the host cannot resize.
        std::function<bool(const EditorSize&)> request_resize;
        /// Take a host action. Null declines every action.
        std::function<bool(const HostActionRequest&)> host_action;
    };

    /// Who calls `tick()` while attached.
    enum class IdleSource : std::uint8_t {
        self_paced,  ///< the session's own main-thread chain
        host_driven, ///< the adapter, from a host timer
    };

    /// A session over a new editor from `factory`, or null when the factory
    /// does not offer the editor role or builds no editor.
    static std::unique_ptr<ContractEditorSession> create(Processor& processor,
                                                         state::StateStore& store,
                                                         EditorFactory& factory, HostHooks hooks,
                                                         IdleSource idle = IdleSource::self_paced);

    ~ContractEditorSession();
    ContractEditorSession(const ContractEditorSession&) = delete;
    ContractEditorSession& operator=(const ContractEditorSession&) = delete;

    /// The editor, or null after `invalidate()`.
    Editor* editor() const {
        return editor_.get();
    }
    /// This session's parameter port, or null after `invalidate()`.
    StateStoreParamPort* param_port() const {
        return port_.get();
    }
    bool is_attached() const {
        return attached_;
    }

    /// Attach the editor to `parent` and start the idle tick. False when the
    /// session is already attached, the parent is empty or of no kind, or the
    /// editor refuses it.
    bool attach(const NativeParent& parent);
    /// Stop the tick, release every gesture this session holds, then detach
    /// the editor. A no-op when not attached.
    void detach();
    /// The window is gone or the processor is going away: stop the tick,
    /// release every gesture, invalidate the editor and destroy it. Every
    /// later call is a refusal.
    void invalidate();
    /// One idle tick: Editor::idle() while attached.
    void tick();

    /// Be invalidated when `registry` (its adapter instance) goes away first.
    void register_with(ContractSessionRegistry& registry);

    /// The editor's size, or its preferred size while it reports none.
    EditorSize size() const;
    /// The editor's size hints (defaults when the editor is gone).
    ViewSize size_hints() const;
    /// A host resize, keeping the scale the editor reports.
    bool set_size(std::uint32_t width, std::uint32_t height);
    /// A new content scale at the current size.
    bool set_scale(double scale);
    /// False only for an editor whose hints fix its size (equal, non-zero
    /// minimum and maximum on both axes).
    bool resizable() const;
    /// Adjust a proposed size through the editor, then clamp it to the hints.
    /// False when nothing usable results.
    bool constrain(std::uint32_t& width, std::uint32_t& height) const;

    /// Offer a key. Forward when not attached.
    KeyDisposition offer_key(const EditorKeyEvent& event);
    void focus_changed(bool focused);
    void visibility_changed(bool visible);

  private:
    friend class ContractSessionRegistry;
    class Context;
    struct IdleState;

    ContractEditorSession(Processor& processor, state::StateStore& store, HostHooks hooks,
                          IdleSource idle);
    void start_idle();
    void stop_idle();
    static void schedule_idle(std::shared_ptr<IdleState> state);

    void unregister();

    IdleSource idle_source_;
    ContractSessionRegistry* registry_ = nullptr;
    // Declared in dependency order so the editor is destroyed first, then the
    // context it was created with, then the port the context hands out.
    std::unique_ptr<StateStoreParamPort> port_;
    std::unique_ptr<Context> context_;
    std::unique_ptr<Editor> editor_;
    bool attached_ = false;
    std::shared_ptr<IdleState> idle_;
};

} // namespace pulp::format
