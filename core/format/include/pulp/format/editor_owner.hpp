#pragma once

namespace pulp::format {

/// Implemented by an editor view that reads its plug-in instance (Processor,
/// StateStore or other instance state) directly.
///
/// Hosts may keep an editor view after disposing the instance (Logic does) and
/// free it later. When that happens the editor's ViewBridge calls
/// on_owner_released() once on every view in the tree that implements this
/// interface, root first, on the main thread: from the editor idle pump on
/// its first tick after the instance retires, from
/// ViewBridge::notify_processor_destroyed(), or at the latest when the editor
/// closes. After the call the instance is gone; the view must not touch it
/// again and should draw a neutral state. It is never called while the
/// instance is alive. Do not add or remove views from inside it.
///
/// A view inherits it beside pulp::view::View:
///
///   class MyEditor : public view::View, public format::EditorOwnerObserver {
///       void on_owner_released() override { processor_ = nullptr; }
///   };
class EditorOwnerObserver {
  public:
    virtual ~EditorOwnerObserver() = default;
    virtual void on_owner_released() = 0;
};

} // namespace pulp::format
