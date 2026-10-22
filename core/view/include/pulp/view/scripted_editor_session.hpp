#pragma once

#include <string>

namespace pulp::render {
class GpuSurface;
}

namespace pulp::view {

class View;

/// The part of a scripted editor session that editor hosts drive: the frame
/// pump, the content-first open, GPU-surface handoff, and teardown.
///
/// Hosts reach a session only through these virtual calls, so a binary that
/// never constructs a ScriptedUiSession references none of the script runtime
/// and a static link drops it. Calling a non-virtual ScriptedUiSession method
/// from always-linked host code would pull the whole runtime into every
/// plug-in, including ones whose editor is AutoUi.
class ScriptedEditorSession {
public:
    virtual ~ScriptedEditorSession() = default;

    /// See ScriptedUiSession::document_load_pending().
    virtual bool document_load_pending() const noexcept = 0;
    /// See ScriptedUiSession::borrows_root().
    virtual bool borrows_root(const View& view) const noexcept = 0;
    /// See ScriptedUiSession::poll().
    virtual bool poll(std::string* error = nullptr) = 0;
    /// See ScriptedUiSession::settling().
    virtual bool settling() const noexcept = 0;
    /// See ScriptedUiSession::complete_pending_load().
    virtual bool complete_pending_load(std::string* error = nullptr) = 0;
    /// See ScriptedUiSession::release_root().
    virtual void release_root() noexcept = 0;
    /// See ScriptedUiSession::set_host_kind().
    virtual void set_host_kind(std::string kind) = 0;
    /// See ScriptedUiSession::attach_gpu_surface().
    virtual void attach_gpu_surface(render::GpuSurface* gpu_surface) = 0;

protected:
    ScriptedEditorSession() = default;
    ScriptedEditorSession(const ScriptedEditorSession&) = default;
    ScriptedEditorSession& operator=(const ScriptedEditorSession&) = default;
};

}  // namespace pulp::view
